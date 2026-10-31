#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "scanner.h"

void scan_report_init(ScanReport *report)
{
    memset(report, 0, sizeof(*report));
}

void scan_report_free(ScanReport *report)
{
    size_t i;

    if (report == NULL) {
        return;
    }
    for (i = 0; i < report->errors_len; i++) {
        free(report->errors[i].path);
    }
    free(report->errors);
    scan_report_init(report);
}

uint64_t scan_combine_size(uint32_t high, uint32_t low)
{
    return ((uint64_t)high << 32) | (uint64_t)low;
}

int scan_should_descend(uint32_t attributes)
{
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

ScanErrorKind scan_classify_error(uint32_t code)
{
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD:
        return SCAN_ERR_ACCESS_DENIED;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_NOT_READY:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return SCAN_ERR_NOT_FOUND;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY:
    case ERROR_INVALID_PARAMETER:
        return SCAN_ERR_INVALID_PATH;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_BUFFER_OVERFLOW:
        return SCAN_ERR_PATH_TOO_LONG;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return SCAN_ERR_NO_MEMORY;
    default:
        return SCAN_ERR_OTHER;
    }
}

const wchar_t *scan_status_name(ScanStatus status)
{
    switch (status) {
    case SCAN_COMPLETE:
        return L"complete";
    case SCAN_PARTIAL:
        return L"partial";
    default:
        return L"fatal";
    }
}

const wchar_t *scan_error_kind_name(ScanErrorKind kind)
{
    switch (kind) {
    case SCAN_ERR_ACCESS_DENIED:
        return L"access_denied";
    case SCAN_ERR_NOT_FOUND:
        return L"not_found";
    case SCAN_ERR_INVALID_PATH:
        return L"invalid_path";
    case SCAN_ERR_PATH_TOO_LONG:
        return L"path_too_long";
    case SCAN_ERR_NO_MEMORY:
        return L"no_memory";
    default:
        return L"other";
    }
}

/* 오류를 기록한다. 메모리가 부족하면 개수만 센다. */
static void record_error(ScanReport *report, const wchar_t *path, DWORD code)
{
    report->error_count++;

    if (report->errors_len == report->errors_cap) {
        size_t cap = report->errors_cap ? report->errors_cap * 2 : 8;
        ScanError *grown = realloc(report->errors, cap * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        report->errors = grown;
        report->errors_cap = cap;
    }
    report->errors[report->errors_len].path = path_strip_extended(path);
    report->errors[report->errors_len].code = (uint32_t)code;
    report->errors[report->errors_len].kind = scan_classify_error((uint32_t)code);
    report->errors_len++;
}

void scan_report_account_alloc(ScanReport *report, uint64_t size, uint64_t alloc,
                               int alloc_known, uint32_t attributes)
{
    uint64_t saved;

    if (!alloc_known) {
        report->alloc_unknown_files++;
        report->alloc_bytes += size;
        return;
    }
    report->alloc_bytes += alloc;
    if (alloc > size) {
        report->slack_bytes += alloc - size;
        return;
    }
    if (alloc == size) {
        return;
    }
    saved = size - alloc;
    if (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                      FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
        report->cloud_files++;
        report->cloud_saved_bytes += saved;
    } else if (attributes & FILE_ATTRIBUTE_COMPRESSED) {
        report->compressed_files++;
        report->compressed_saved_bytes += saved;
    } else if (attributes & FILE_ATTRIBUTE_SPARSE_FILE) {
        report->sparse_files++;
        report->sparse_saved_bytes += saved;
    } else {
        report->other_saved_files++;
        report->other_saved_bytes += saved;
    }
}

/* ---- 작업 스레드 수 ---- */

static volatile LONG g_threads = 0;

void scan_set_threads(unsigned threads)
{
    InterlockedExchange(&g_threads, (LONG)(threads > SCAN_MAX_THREADS ? SCAN_MAX_THREADS : threads));
}

unsigned scan_effective_threads(void)
{
    LONG n = InterlockedCompareExchange(&g_threads, 0, 0);
    DWORD cpus;

    if (n > 0) {
        return (unsigned)n;
    }
    cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cpus == 0) {
        return 1;
    }
    return cpus > SCAN_MAX_THREADS ? SCAN_MAX_THREADS : (unsigned)cpus;
}

/* ---- 폴더 하나의 열거 결과 ---- */

/*
 * 열거된 항목 하나. 8바이트 단위로 이어 붙이며 이름(NUL 포함)이 바로 뒤따른다.
 * REC_HAS_ID 이면 이름 앞에 파일 ID(RecId, 16바이트)가 끼어 있다(정밀 스캔에서만).
 * "." 와 ".." 는 담지 않는다.
 */
typedef struct {
    uint64_t size;
    uint64_t alloc;
    uint32_t attributes;
    uint32_t name_len; /* 문자 수(NUL 제외) */
    uint32_t flags;    /* REC_* */
    uint32_t reserved;
} EntryRec;

/* 파일 ID(FILE_ID_128). NTFS 의 64비트 ID 는 hi 가 0 이다. */
typedef struct {
    uint64_t lo;
    uint64_t hi;
} RecId;

#define REC_ALLOC_KNOWN 1u
#define REC_HAS_ID 2u

/* 작업 상태. 큐에는 작업이 아니라 칸(KidSlot)이 들어가므로, 막 만든 작업은 TASK_NEW 다. */
typedef enum { TASK_NEW = 0, TASK_RUNNING, TASK_DONE } TaskState;

/*
 * 한 번의 탐색에서 살아 있는 폴더 작업의 수와 메모리(구조·경로·키·열거 결과·자식 목록) 집계.
 * 작업 스레드와 호출 스레드가 함께 고치므로 원자적으로 더한다. ScanReport 의 진단 값이 된다.
 */
typedef struct {
    volatile LONG64 tasks;
    volatile LONG64 bytes;
    volatile LONG64 peak_tasks;
    volatile LONG64 peak_bytes;
    volatile LONG64 discarded; /* 처리하지 않고 해제한 작업 */
} MemTally;

static void tally_max(volatile LONG64 *peak, LONG64 v)
{
    LONG64 cur = *peak;

    while (v > cur) {
        LONG64 prev = InterlockedCompareExchange64(peak, v, cur);
        if (prev == cur) {
            break;
        }
        cur = prev;
    }
}

static void tally_add(MemTally *m, LONG64 tasks, LONG64 bytes)
{
    LONG64 t, b;

    if (m == NULL) {
        return;
    }
    t = InterlockedAdd64(&m->tasks, tasks);
    b = InterlockedAdd64(&m->bytes, bytes);
    if (tasks > 0) {
        tally_max(&m->peak_tasks, t);
    }
    if (bytes > 0) {
        tally_max(&m->peak_bytes, b);
    }
}

struct DirTask;

/*
 * 아직 내려가지 않은 하위 폴더 하나(부모 작업의 slots 에 열거 순서로 둔다).
 * 폴더 작업(경로 사본 등)은 열거하기 직전에야 만든다. 그 전에는 부모의 열거 기록을 가리키기만 한다.
 *   task == NULL, heap_index == HEAP_NONE : 아직 아무도 손대지 않음(호출 스레드가 차례가 되면 만든다)
 *   task == NULL, heap_index != HEAP_NONE : 미리 열거할 차례를 큐에서 기다림
 *   task != NULL                          : 작업 스레드가 열거 중(RUNNING)이거나 마침(DONE)
 * 작업 스레드가 있으면 Pool.lock 으로 보호한다.
 */
typedef struct {
    struct DirTask *task;
    size_t heap_index;  /* 큐(힙) 안의 위치, 없으면 HEAP_NONE */
    uintptr_t id;       /* 호출 스레드가 on_entry 결과로 채운다 */
    uint32_t rec_off;   /* 부모 buf 안의 이 폴더 기록 위치 */
    uint32_t flags;     /* SLOT_* */
} KidSlot;

#define SLOT_EXCLUDED 1u /* should_skip_dir 훅이 뺐다 */
#define SLOT_TOO_LONG 2u /* 경로가 너무 길어 내려갈 수 없다(부모를 처리할 때 오류로 기록) */

/*
 * 열거할 폴더 하나와 그 결과.
 * 폴더를 열거한 쪽(작업 스레드 또는 호출 스레드)이 하위 탐색 대상 폴더(재분석 지점이 아니고 깊이
 * 제한 안인 폴더)마다 slots 에 칸을 만든다. 작업 스레드가 있으면 그 칸들을 큐에 넣어, 작업 스레드가
 * 가장 먼저 필요한 것부터 꺼내 작업을 만들고 미리 열거한다.
 * 호출 스레드가 처리한 작업은 남은 하위 폴더가 있는 동안 깊이 우선 스택의 한 칸이 되어, slots 를
 * 마지막 칸부터 차례로 넘겨받는다(next_kid). 그래서 기다리는 형제 폴더마다 작업을 만들어 두지 않는다.
 */
typedef struct DirTask {
    wchar_t *path;      /* 확장 경로(구조체와 같은 할당) */
    uint32_t *key;      /* 처리 순서 키(구조체와 같은 할당, 아래 설명) */
    unsigned char *buf; /* EntryRec 이어 붙임 */
    KidSlot *slots;     /* 하위 탐색 대상 폴더마다 하나(열거 순서, 만들지 못했으면 NULL) */
    uintptr_t id;       /* 호출 스레드가 on_entry 결과로 채운다 */
    size_t path_len;
    size_t len;
    size_t cap;
    size_t kid_count;   /* 하위 탐색 대상 폴더 수(slots 가 NULL 이어도 센다) */
    size_t next_kid;    /* 스택 칸일 때 아직 넘기지 않은 칸 수(slots[next_kid - 1] 이 다음 차례) */
    DWORD open_error;   /* 0 이 아니면 폴더를 열지 못함 */
    DWORD enum_error;   /* 0 이 아니면 열거 도중 오류(그때까지의 항목은 유효) */
    uint32_t depth;
    uint64_t volume;    /* 정밀 스캔: 이 폴더가 있는 볼륨의 일련번호(모르면 0) */
    MemTally *tally;    /* 탐색 하나의 메모리 집계(자식 작업이 물려받는다) */
    size_t self_bytes;  /* 구조·키·경로를 담은 할당 크기 */
    unsigned char processed; /* 호출 스레드가 처리했으면 1(아니면 해제할 때 버린 작업으로 센다) */
    /*
     * 깊이 우선 처리 순서 키의 길이. 키는 루트에서 이 폴더까지 각 단계의 (형제 수 - 1 - 열거 순번)
     * 이다. 호출 스레드는 마지막 자식부터 내려가므로 키를 사전순으로 비교하면 처리 순서와 같다.
     * 작업 스레드는 키가 가장 작은(가장 먼저 필요한) 작업부터 열거한다.
     */
    uint32_t key_len;
    unsigned char state;     /* TaskState. 작업 스레드가 있으면 Pool.lock 으로 보호 */
    unsigned char by_worker; /* 작업 스레드가 열거했으면 1 */
    unsigned char discard;   /* 작업 스레드가 열거하는 도중 버려졌으면 1(그 스레드가 해제) */
    unsigned char awaited;   /* 호출 스레드가 끝나기를 기다리면 1(끝낸 쪽이 깨운다) */
    /* 작업 스레드가 만든 작업: 부모의 어느 칸인지(내쫓을 때 칸을 되돌린다), 준비 힙 위치 */
    struct DirTask *parent;
    size_t slot_index;
    size_t ready_index;      /* 준비 힙(Pool.ready_heap) 안의 위치, 없으면 HEAP_NONE */
} DirTask;

#define HEAP_NONE ((size_t)-1)

/* GetFileInformationByHandleEx 버퍼(FIND_FIRST_EX_LARGE_FETCH 와 비슷한 크기). */
#define FETCH_BUFFER_BYTES (64 * 1024)

static size_t rec_bytes(size_t name_len, int has_id)
{
    return (sizeof(EntryRec) + (has_id ? sizeof(RecId) : 0) + (name_len + 1) * sizeof(wchar_t) +
            7) &
           ~(size_t)7;
}

/* 기록 하나의 전체 크기(다음 기록까지의 거리). */
static size_t rec_size(const EntryRec *rec)
{
    return rec_bytes(rec->name_len, (rec->flags & REC_HAS_ID) != 0);
}

static const RecId *rec_id(const EntryRec *rec)
{
    return (rec->flags & REC_HAS_ID) ? (const RecId *)(rec + 1) : NULL;
}

static const wchar_t *rec_name(const EntryRec *rec)
{
    return (const wchar_t *)((const unsigned char *)(rec + 1) +
                             ((rec->flags & REC_HAS_ID) ? sizeof(RecId) : 0));
}

/*
 * 작업을 만든다. 구조체 뒤에 키와 경로(dir + 구분자 + name)를 이어 담아 한 번만 할당한다.
 * name 이 NULL 이면 dir 가 곧 경로다. key 는 부모 키(key_len 개) 뒤에 last 를 붙인 것이다
 * (has_last 가 0 이면 붙이지 않는다). 경로가 PATH_MAX_WIDE 를 넘거나 메모리가 없으면 NULL.
 * 만든 작업은 tally 에 센다(NULL 허용).
 */
static DirTask *task_new(const wchar_t *dir, size_t dir_len, const wchar_t *name, size_t name_len,
                         const uint32_t *key, uint32_t key_len, int has_last, uint32_t last,
                         uint32_t depth, MemTally *tally)
{
    DirTask *t;
    size_t sep, path_len, bytes;
    uint32_t klen = key_len + (has_last ? 1u : 0u);
    wchar_t *path;

    sep = name != NULL && dir_len > 0 && dir[dir_len - 1] != L'\\' && dir[dir_len - 1] != L'/' &&
          dir[dir_len - 1] != L':';
    path_len = dir_len + sep + name_len;
    if (path_len > PATH_MAX_WIDE) {
        return NULL;
    }
    bytes = sizeof(*t) + klen * sizeof(uint32_t) + (path_len + 1) * sizeof(wchar_t);
    t = malloc(bytes);
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->tally = tally;
    t->self_bytes = bytes;
    tally_add(tally, 1, (LONG64)bytes);
    t->ready_index = HEAP_NONE;
    t->depth = depth;
    t->key = (uint32_t *)(t + 1);
    t->key_len = klen;
    if (key_len > 0) {
        memcpy(t->key, key, key_len * sizeof(uint32_t));
    }
    if (has_last) {
        t->key[key_len] = last;
    }
    path = (wchar_t *)(t->key + klen);
    memcpy(path, dir, dir_len * sizeof(wchar_t));
    if (sep) {
        path[dir_len] = L'\\';
    }
    if (name_len > 0) {
        memcpy(path + dir_len + sep, name, name_len * sizeof(wchar_t));
    }
    path[path_len] = L'\0';
    t->path = path;
    t->path_len = path_len;
    return t;
}

static size_t slots_bytes(const DirTask *t)
{
    return t->slots != NULL ? t->kid_count * sizeof(*t->slots) : 0;
}

/* 작업 하나만 해제한다(slots 가 가리키는 작업은 건드리지 않는다). */
static void task_free_one(DirTask *t)
{
    if (t->tally != NULL) {
        tally_add(t->tally, -1, -(LONG64)(t->self_bytes + t->cap + slots_bytes(t)));
        if (!t->processed) {
            InterlockedIncrement64(&t->tally->discarded);
        }
    }
    free(t->buf);
    free(t->slots);
    free(t);
}

/* 작업과 그 slots 가 가리키는 작업 전체를 해제한다. 작업 스레드가 없을 때만 부른다. */
static void task_free_tree(DirTask *t)
{
    DirTask **work = NULL;
    size_t len = 0, cap = 0;

    while (t != NULL) {
        size_t i;
        for (i = 0; t->slots != NULL && i < t->kid_count; i++) {
            if (t->slots[i].task == NULL) {
                continue;
            }
            if (len == cap) {
                size_t ncap = cap ? cap * 2 : 64;
                DirTask **grown = realloc(work, ncap * sizeof(*grown));
                if (grown == NULL) {
                    /* 메모리가 없으면 재귀로라도 해제한다(깊이는 폴더 깊이로 제한). */
                    task_free_tree(t->slots[i].task);
                    continue;
                }
                work = grown;
                cap = ncap;
            }
            work[len++] = t->slots[i].task;
        }
        task_free_one(t);
        t = len > 0 ? work[--len] : NULL;
    }
    free(work);
}

static int is_dot_name(const wchar_t *name, size_t len)
{
    return name[0] == L'.' && (len == 1 || (len == 2 && name[1] == L'.'));
}

/*
 * extra 바이트를 더 담을 수 있게 늘린다. 첫 묶음은 정확히 맞춰 작은 폴더의 낭비를 없애고,
 * 그다음부터는 1.5배씩 늘려 큰 폴더에서 재할당 복사가 반복되지 않게 한다. 메모리가 부족하면 0.
 */
static int task_reserve(DirTask *t, size_t extra)
{
    unsigned char *grown;
    size_t cap;

    if (t->len + extra <= t->cap) {
        return 1;
    }
    cap = t->len + extra;
    if (t->cap > 0 && cap < t->cap + t->cap / 2) {
        cap = t->cap + t->cap / 2;
    }
    grown = realloc(t->buf, cap);
    if (grown == NULL) {
        return 0;
    }
    tally_add(t->tally, 0, (LONG64)(cap - t->cap));
    t->buf = grown;
    t->cap = cap;
    return 1;
}

/* 항목을 하나 붙인다. id 가 NULL 이 아니면 파일 ID도 담는다. 메모리가 부족하면 0. */
static int task_append(DirTask *t, const wchar_t *name, size_t name_len, uint64_t size,
                       uint64_t alloc, uint32_t attributes, uint32_t flags, const RecId *id)
{
    size_t need = rec_bytes(name_len, id != NULL);
    EntryRec *rec;
    wchar_t *dst;

    if (t->len + need > t->cap) {
        size_t cap = t->cap ? t->cap : 2048;
        unsigned char *grown;
        while (cap < t->len + need) {
            cap *= 2;
        }
        grown = realloc(t->buf, cap);
        if (grown == NULL) {
            return 0;
        }
        tally_add(t->tally, 0, (LONG64)(cap - t->cap));
        t->buf = grown;
        t->cap = cap;
    }
    rec = (EntryRec *)(t->buf + t->len);
    rec->size = size;
    rec->alloc = alloc;
    rec->attributes = attributes;
    rec->name_len = (uint32_t)name_len;
    rec->flags = flags | (id != NULL ? REC_HAS_ID : 0u);
    rec->reserved = 0;
    if (id != NULL) {
        memcpy(rec + 1, id, sizeof(*id));
    }
    dst = (wchar_t *)rec_name(rec);
    memcpy(dst, name, name_len * sizeof(wchar_t));
    dst[name_len] = L'\0';
    t->len += need;
    return 1;
}

/*
 * 압축·스파스 파일은 폴더 열거의 AllocationSize 가 실제 점유량과 다를 수 있어 파일 이름으로
 * 다시 구한다(파일을 열지 않음). 클라우드 자리표시자는 내려받기를 일으키지 않도록 건드리지 않는다.
 */
static uint64_t refine_alloc(const DirTask *t, const wchar_t *name, size_t name_len,
                             uint32_t attributes, uint64_t alloc)
{
    wchar_t local[MAX_PATH];
    wchar_t *full;
    size_t dir_len;
    DWORD high = 0, low;

    if (!(attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) ||
        (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                       FILE_ATTRIBUTE_RECALL_ON_OPEN))) {
        return alloc;
    }
    dir_len = t->path_len;
    if (dir_len + name_len + 2 > PATH_MAX_WIDE) {
        return alloc;
    }
    full = dir_len + name_len + 2 <= MAX_PATH ? local
                                               : malloc((dir_len + name_len + 2) * sizeof(wchar_t));
    if (full == NULL) {
        return alloc;
    }
    memcpy(full, t->path, dir_len * sizeof(wchar_t));
    if (dir_len > 0 && full[dir_len - 1] != L'\\') {
        full[dir_len++] = L'\\';
    }
    memcpy(full + dir_len, name, name_len * sizeof(wchar_t));
    full[dir_len + name_len] = L'\0';
    low = GetCompressedFileSizeW(full, &high);
    if (full != local) {
        free(full);
    }
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        return alloc;
    }
    return ((uint64_t)high << 32) | low;
}

/* 할당 크기를 주지 않는 파일 시스템용 대체 열거. */
static void fetch_records_find(DirTask *t)
{
    WIN32_FIND_DATAW fd;
    HANDLE find;
    wchar_t *pattern = path_join(t->path, L"*");
    DWORD err;

    if (pattern == NULL) {
        t->open_error = ERROR_FILENAME_EXCED_RANGE;
        return;
    }
    find = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                            FIND_FIRST_EX_LARGE_FETCH);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        /* 드라이브 루트처럼 "." 조차 없는 빈 디렉터리. */
        if (err != ERROR_FILE_NOT_FOUND) {
            t->open_error = err != 0 ? err : ERROR_GEN_FAILURE;
        }
        return;
    }
    do {
        size_t len = wcslen(fd.cFileName);
        uint64_t size;
        if (is_dot_name(fd.cFileName, len)) {
            continue;
        }
        size = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                   ? 0
                   : scan_combine_size((uint32_t)fd.nFileSizeHigh, (uint32_t)fd.nFileSizeLow);
        if (!task_append(t, fd.cFileName, len, size, size, fd.dwFileAttributes, 0, NULL)) {
            t->enum_error = ERROR_NOT_ENOUGH_MEMORY;
            FindClose(find);
            return;
        }
    } while (FindNextFileW(find, &fd));
    err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) {
        t->enum_error = err;
    }
    FindClose(find);
}

static int fetch_unsupported(DWORD err)
{
    return err == ERROR_INVALID_PARAMETER || err == ERROR_INVALID_FUNCTION ||
           err == ERROR_NOT_SUPPORTED || err == ERROR_INVALID_LEVEL ||
           err == ERROR_CALL_NOT_IMPLEMENTED;
}

/* 열거 정보 종류 하나에서 꺼낸 항목(정보 종류마다 구조가 달라 한곳에서 읽는다). */
typedef struct {
    DWORD next;          /* 다음 항목까지의 거리(0 이면 마지막) */
    const wchar_t *name;
    size_t name_len;
    uint32_t attributes;
    uint64_t size;
    uint64_t alloc;
    RecId id;
    int has_id;
} DirView;

static void view_entry(FILE_INFO_BY_HANDLE_CLASS cls, const void *p, DirView *v)
{
    memset(v, 0, sizeof(*v));
    if (cls == FileIdExtdDirectoryInfo) {
        const FILE_ID_EXTD_DIR_INFO *d = p;
        v->next = d->NextEntryOffset;
        v->name = d->FileName;
        v->name_len = d->FileNameLength / sizeof(wchar_t);
        v->attributes = (uint32_t)d->FileAttributes;
        v->size = (uint64_t)d->EndOfFile.QuadPart;
        v->alloc = (uint64_t)d->AllocationSize.QuadPart;
        memcpy(&v->id.lo, d->FileId.Identifier, 8);
        memcpy(&v->id.hi, d->FileId.Identifier + 8, 8);
        v->has_id = 1;
    } else if (cls == FileIdBothDirectoryInfo) {
        const FILE_ID_BOTH_DIR_INFO *d = p;
        v->next = d->NextEntryOffset;
        v->name = d->FileName;
        v->name_len = d->FileNameLength / sizeof(wchar_t);
        v->attributes = (uint32_t)d->FileAttributes;
        v->size = (uint64_t)d->EndOfFile.QuadPart;
        v->alloc = (uint64_t)d->AllocationSize.QuadPart;
        v->id.lo = (uint64_t)d->FileId.QuadPart;
        v->has_id = 1;
    } else {
        const FILE_FULL_DIR_INFO *d = p;
        v->next = d->NextEntryOffset;
        v->name = d->FileName;
        v->name_len = d->FileNameLength / sizeof(wchar_t);
        v->attributes = (uint32_t)d->FileAttributes;
        v->size = (uint64_t)d->EndOfFile.QuadPart;
        v->alloc = (uint64_t)d->AllocationSize.QuadPart;
    }
}

/* 폴더 핸들로 볼륨 일련번호를 구한다(모르면 0). */
static uint64_t handle_volume(HANDLE h)
{
    FILE_ID_INFO fi;
    BY_HANDLE_FILE_INFORMATION bh;

    if (GetFileInformationByHandleEx(h, FileIdInfo, &fi, sizeof(fi))) {
        return fi.VolumeSerialNumber;
    }
    if (GetFileInformationByHandle(h, &bh)) {
        return bh.dwVolumeSerialNumber;
    }
    return 0;
}

/*
 * 폴더 하나를 열거해 t 에 담는다. 정밀 스캔(SCAN_OPT_HARDLINKS)이면 파일 ID를 함께 받는
 * FileIdExtdDirectoryInfo(128비트 ID), 안 되면 FileIdBothDirectoryInfo(64비트 ID)를 쓰고
 * 폴더 핸들로 볼륨 일련번호를 구한다. 둘 다 안 되면 ID 없이 열거한다.
 */
static void fetch_records(DirTask *t, unsigned char *scratch, unsigned options)
{
    FILE_INFO_BY_HANDLE_CLASS cls = FileFullDirectoryInfo;
    HANDLE h;
    int first = 1;

    h = CreateFileW(t->path, FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        t->open_error = err != 0 ? err : ERROR_GEN_FAILURE;
        return;
    }
    if (options & SCAN_OPT_HARDLINKS) {
        cls = FileIdExtdDirectoryInfo;
        t->volume = handle_volume(h);
    }
    for (;;) {
        const unsigned char *p;
        DirView v;
        if (!GetFileInformationByHandleEx(h, cls, scratch, FETCH_BUFFER_BYTES)) {
            DWORD err = GetLastError();
            /* 빈 볼륨 루트는 "." 도 없어 첫 호출이 FILE_NOT_FOUND 다. */
            if (err == ERROR_NO_MORE_FILES || (first && err == ERROR_FILE_NOT_FOUND)) {
                break;
            }
            if (first && fetch_unsupported(err)) {
                /* ID 를 주는 정보 종류를 차례로 낮춘다. */
                if (cls == FileIdExtdDirectoryInfo) {
                    cls = FileIdBothDirectoryInfo;
                    continue;
                }
                if (cls == FileIdBothDirectoryInfo) {
                    cls = FileFullDirectoryInfo;
                    continue;
                }
                CloseHandle(h);
                fetch_records_find(t);
                return;
            }
            t->enum_error = err;
            break;
        }
        first = 0;
        /* 이 묶음에 필요한 만큼만 미리 늘린다(대부분의 폴더는 묶음 하나로 끝난다). */
        {
            size_t need = 0;
            p = scratch;
            for (;;) {
                view_entry(cls, p, &v);
                need += rec_bytes(v.name_len, v.has_id);
                if (v.next == 0) {
                    break;
                }
                p += v.next;
            }
            if (!task_reserve(t, need)) {
                t->enum_error = ERROR_NOT_ENOUGH_MEMORY;
                break;
            }
        }
        p = scratch;
        for (;;) {
            view_entry(cls, p, &v);
            if (v.name_len > 0 && !is_dot_name(v.name, v.name_len)) {
                uint64_t size = 0, alloc = 0;
                if (!(v.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    size = v.size;
                    alloc = refine_alloc(t, v.name, v.name_len, v.attributes, v.alloc);
                }
                if (!task_append(t, v.name, v.name_len, size, alloc, v.attributes,
                                 REC_ALLOC_KNOWN, v.has_id ? &v.id : NULL)) {
                    t->enum_error = ERROR_NOT_ENOUGH_MEMORY;
                    CloseHandle(h);
                    return;
                }
            }
            if (v.next == 0) {
                break;
            }
            p += v.next;
        }
    }
    CloseHandle(h);
}

/* 하위 탐색 대상(재분석 지점이 아닌 폴더이고 깊이 제한 안)인지. 호출 스레드와 같은 판단이다. */
static int rec_wants_descend(const DirTask *t, const EntryRec *rec, uint32_t max_depth)
{
    return t->depth < max_depth && scan_should_descend(rec->attributes);
}

/*
 * 폴더 하나를 열거해 t 에 담고 하위 탐색 대상 폴더마다 빈 칸(slots)을 만든다. 자식 작업은 아직
 * 만들지 않는다. 어느 스레드에서나 부를 수 있다(t 외에는 건드리지 않음).
 */
static void fetch_dir(DirTask *t, unsigned char *scratch, uint32_t max_depth, unsigned options)
{
    size_t off, n = 0;

    fetch_records(t, scratch, options);
    /* 큰 폴더는 1.5배씩 늘린 여유가 남으므로 처리를 기다리는 동안 붙잡지 않게 줄인다. */
    if (t->buf != NULL && t->len > 0 && t->cap - t->len > t->len / 8) {
        unsigned char *fit = realloc(t->buf, t->len);
        if (fit != NULL) {
            tally_add(t->tally, 0, -(LONG64)(t->cap - t->len));
            t->buf = fit;
            t->cap = t->len;
        }
    }
    for (off = 0; off < t->len; off += rec_size((const EntryRec *)(t->buf + off))) {
        n += rec_wants_descend(t, (const EntryRec *)(t->buf + off), max_depth);
    }
    t->kid_count = n;
    if (n == 0) {
        return;
    }
    /* 칸을 만들지 못하면 slots 없이 두어 호출 스레드가 메모리 부족으로 기록한다. */
    t->slots = t->len <= UINT32_MAX ? calloc(n, sizeof(*t->slots)) : NULL;
    if (t->slots == NULL) {
        return;
    }
    tally_add(t->tally, 0, (LONG64)(n * sizeof(*t->slots)));
    n = 0;
    for (off = 0; off < t->len; off += rec_size((const EntryRec *)(t->buf + off))) {
        if (rec_wants_descend(t, (const EntryRec *)(t->buf + off), max_depth)) {
            t->slots[n].heap_index = HEAP_NONE;
            t->slots[n].rec_off = (uint32_t)off;
            n++;
        }
    }
}

/* 칸 i 의 폴더 기록. */
static const EntryRec *slot_rec(const DirTask *parent, size_t i)
{
    return (const EntryRec *)(parent->buf + parent->slots[i].rec_off);
}

/*
 * 칸 i 의 자식 작업을 만든다(경로 = 부모 경로 + 이름). 처리 순서 키는 부모 키 뒤에
 * (형제 수 - 1 - i)를 붙인 것이다. 경로가 너무 길거나 메모리가 없으면 NULL.
 */
static DirTask *slot_task_new(const DirTask *parent, size_t i)
{
    const EntryRec *rec = slot_rec(parent, i);

    return task_new(parent->path, parent->path_len, rec_name(rec), rec->name_len, parent->key,
                    parent->key_len, 1, (uint32_t)(parent->kid_count - 1 - i), parent->depth + 1,
                    parent->tally);
}

/* ---- 미리 열거하는 작업 스레드 ---- */

/*
 * 미리 열거해 두고 아직 처리하지 않은 폴더의 상한. 상한에 닿으면 더 급한 칸이 있을 때만 가장 나중에
 * 필요한 것을 내쫓고 열거하며, 아니면 작업 스레드가 쉬다가 호출 스레드가 따라잡으면 깨어난다
 * (결과는 같다). C:\Windows 측정에서 1024 개·2 MiB 아래로는 메모리가 거의 줄지 않고(큰 폴더 하나의
 * 목록이 바닥) 256 개부터 호출 스레드가 기다려 느려졌다(performance.md 참고).
 */
#define POOL_READY_LIMIT 1024
#define POOL_READY_BYTES ((size_t)2 * 1024 * 1024)

/*
 * 큐 항목: 미리 열거할 하위 폴더 하나(부모 작업과 칸 번호, 16바이트). 폴더 작업(경로 사본 등)은
 * 작업 스레드가 꺼낼 때 만들므로, 큐가 길어져도 메모리를 거의 쓰지 않는다.
 */
typedef struct {
    DirTask *parent;
    size_t index;
} QueueItem;

typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE work_cv; /* 할 일이 생김·여유가 생김·중지(작업 스레드가 기다림) */
    CONDITION_VARIABLE done_cv; /* 호출 스레드가 기다리는 작업이 끝남 */
    QueueItem *queue;           /* 최소 힙(item_before 순서) */
    size_t qlen;
    size_t qcap;
    /*
     * 미리 열거해 둔 작업 중 가장 나중에 필요한 것이 맨 위인 힙. 상한이 찼는데 큐 맨 앞이 그보다 먼저
     * 필요하면 그 작업을 내쫓고(칸은 큐로 되돌림) 큐 맨 앞을 열거한다. 그래서 상한 안에는 언제나
     * 가장 먼저 필요한 것들이 남고, 깊은 곳에서 새로 나타난 급한 폴더가 막히지 않는다.
     */
    DirTask **ready_heap;
    size_t rlen;
    size_t rcap;
    uint64_t evicted; /* 내쫓은 작업 수(진단, 열거를 한 번 버림) */
    size_t ready; /* 작업 스레드가 끝냈고 아직 넘겨받거나 버리지 않은 작업 */
    size_t ready_bytes; /* 그 작업들이 붙잡은 메모리(ready_cost 합) */
    int idle;           /* work_cv 에서 쉬는 작업 스레드 수 */
    int throttled;      /* 상한 때문에 쉬는 작업 스레드가 있으면 1 */
    int stop;
    uint32_t max_depth;
    unsigned options;    /* SCAN_OPT_* */
    uint64_t prefetched; /* 작업 스레드가 열거한 폴더(진단) */
    HANDLE threads[SCAN_MAX_THREADS];
    int nthreads;
} Pool;

/* 미리 열거한 작업 하나가 붙잡고 있는 메모리. 넣을 때와 뺄 때 같은 값이 나와야 한다. */
static size_t ready_cost(const DirTask *t)
{
    return t->self_bytes + t->cap + slots_bytes(t);
}

static int pool_full_locked(const Pool *p)
{
    return p->ready >= POOL_READY_LIMIT || p->ready_bytes >= POOL_READY_BYTES;
}

/* 쉬고 있는 작업 스레드를 n 개까지 깨운다(lock 보유). 모두 깨워 다시 잠드는 낭비를 줄인다. */
static void wake_workers_locked(Pool *p, size_t n)
{
    if (p->idle <= 0 || n == 0) {
        return;
    }
    if (n >= (size_t)p->idle) {
        WakeAllConditionVariable(&p->work_cv);
        return;
    }
    while (n-- > 0) {
        WakeConditionVariable(&p->work_cv);
    }
}

static void ready_remove_locked(Pool *p, DirTask *t);

/* 작업 스레드가 끝낸 작업을 넘겨받거나 버릴 때 상한 계산에서 뺀다(lock 보유). */
static void ready_release_locked(Pool *p, DirTask *t)
{
    if (!t->by_worker) {
        return;
    }
    ready_remove_locked(p, t);
    p->ready--;
    p->ready_bytes -= ready_cost(t);
    if (p->throttled && !pool_full_locked(p)) {
        p->throttled = 0;
        wake_workers_locked(p, p->qlen);
    }
}

/* 처리 순서 키 보기: key[0..len) 뒤에 has_last 이면 last 를 붙인 것. */
typedef struct {
    const uint32_t *key;
    uint32_t len;
    uint32_t last;
    int has_last;
} KeyView;

static KeyView item_key(const QueueItem *q)
{
    KeyView k;

    k.key = q->parent->key;
    k.len = q->parent->key_len;
    k.last = (uint32_t)(q->parent->kid_count - 1 - q->index); /* 형제 수 - 1 - 칸 번호 */
    k.has_last = 1;
    return k;
}

static KeyView task_key(const DirTask *t)
{
    KeyView k;

    k.key = t->key;
    k.len = t->key_len;
    k.last = 0;
    k.has_last = 0;
    return k;
}

/* a 가 b 보다 먼저 처리되는지(키 사전순, 접두사가 같으면 짧은 쪽이 먼저). */
static int key_before(const KeyView *a, const KeyView *b)
{
    uint32_t la = a->len + (a->has_last ? 1u : 0u), lb = b->len + (b->has_last ? 1u : 0u);
    uint32_t n = la < lb ? la : lb, i;

    for (i = 0; i < n; i++) {
        uint32_t ka = i < a->len ? a->key[i] : a->last;
        uint32_t kb = i < b->len ? b->key[i] : b->last;
        if (ka != kb) {
            return ka < kb;
        }
    }
    return la < lb;
}

static int item_before(const QueueItem *a, const QueueItem *b)
{
    KeyView ka, kb;

    if (a->parent == b->parent) {
        return a->index > b->index; /* 같은 부모: 마지막 칸부터 */
    }
    ka = item_key(a);
    kb = item_key(b);
    return key_before(&ka, &kb);
}

/* ---- 준비 힙(가장 나중에 필요한 작업이 맨 위) ---- */

static int ready_later(const DirTask *a, const DirTask *b)
{
    KeyView ka = task_key(a), kb = task_key(b);

    return key_before(&kb, &ka);
}

static void ready_set(Pool *p, size_t i, DirTask *t)
{
    p->ready_heap[i] = t;
    t->ready_index = i;
}

static void ready_up(Pool *p, size_t i)
{
    DirTask *t = p->ready_heap[i];

    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!ready_later(t, p->ready_heap[parent])) {
            break;
        }
        ready_set(p, i, p->ready_heap[parent]);
        i = parent;
    }
    ready_set(p, i, t);
}

static void ready_down(Pool *p, size_t i)
{
    DirTask *t = p->ready_heap[i];

    for (;;) {
        size_t l = 2 * i + 1, best = i;
        DirTask *best_t = t;
        if (l < p->rlen && ready_later(p->ready_heap[l], best_t)) {
            best = l;
            best_t = p->ready_heap[l];
        }
        if (l + 1 < p->rlen && ready_later(p->ready_heap[l + 1], best_t)) {
            best = l + 1;
        }
        if (best == i) {
            break;
        }
        ready_set(p, i, p->ready_heap[best]);
        i = best;
    }
    ready_set(p, i, t);
}

/* 준비 힙에 넣는다(lock 보유). 늘리지 못하면 넣지 않는다(그 작업은 내쫓기지 않을 뿐). */
static void ready_push_locked(Pool *p, DirTask *t)
{
    if (p->rlen == p->rcap) {
        size_t cap = p->rcap ? p->rcap * 2 : 256;
        DirTask **grown = realloc(p->ready_heap, cap * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        p->ready_heap = grown;
        p->rcap = cap;
    }
    ready_set(p, p->rlen++, t);
    ready_up(p, p->rlen - 1);
}

static void ready_remove_locked(Pool *p, DirTask *t)
{
    size_t i = t->ready_index;

    if (i == HEAP_NONE) {
        return;
    }
    t->ready_index = HEAP_NONE;
    if (i == --p->rlen) {
        return;
    }
    {
        DirTask *last = p->ready_heap[p->rlen];
        ready_set(p, i, last);
        ready_up(p, i);
        ready_down(p, last->ready_index);
    }
}

static void heap_set(Pool *p, size_t i, QueueItem item)
{
    p->queue[i] = item;
    item.parent->slots[item.index].heap_index = i;
}

static void heap_up(Pool *p, size_t i)
{
    QueueItem item = p->queue[i];

    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!item_before(&item, &p->queue[parent])) {
            break;
        }
        heap_set(p, i, p->queue[parent]);
        i = parent;
    }
    heap_set(p, i, item);
}

static void heap_down(Pool *p, size_t i)
{
    QueueItem item = p->queue[i];

    for (;;) {
        size_t l = 2 * i + 1, best = i;
        const QueueItem *best_item = &item;
        if (l < p->qlen && item_before(&p->queue[l], best_item)) {
            best = l;
            best_item = &p->queue[l];
        }
        if (l + 1 < p->qlen && item_before(&p->queue[l + 1], best_item)) {
            best = l + 1;
        }
        if (best == i) {
            break;
        }
        heap_set(p, i, p->queue[best]);
        i = best;
    }
    heap_set(p, i, item);
}

/* 칸을 큐에 넣는다(lock 보유). 큐를 늘리지 못하면 넣지 않는다(차례가 되면 호출 스레드가 열거). */
static int queue_push_locked(Pool *p, DirTask *parent, size_t index)
{
    QueueItem item;

    if (p->qlen == p->qcap) {
        size_t cap = p->qcap ? p->qcap * 2 : 256;
        QueueItem *grown = realloc(p->queue, cap * sizeof(*grown));
        if (grown == NULL) {
            return 0;
        }
        p->queue = grown;
        p->qcap = cap;
    }
    item.parent = parent;
    item.index = index;
    heap_set(p, p->qlen++, item);
    heap_up(p, p->qlen - 1);
    return 1;
}

/* 가장 먼저 필요한 칸을 꺼낸다(lock 보유, 큐가 비어 있지 않아야 함). */
static QueueItem queue_pop_locked(Pool *p)
{
    QueueItem item = p->queue[0];

    item.parent->slots[item.index].heap_index = HEAP_NONE;
    if (--p->qlen > 0) {
        heap_set(p, 0, p->queue[p->qlen]);
        heap_down(p, 0);
    }
    return item;
}

static void queue_remove_locked(Pool *p, KidSlot *slot)
{
    size_t i = slot->heap_index;

    if (i == HEAP_NONE) {
        return;
    }
    slot->heap_index = HEAP_NONE;
    if (i == --p->qlen) {
        return;
    }
    {
        QueueItem last = p->queue[p->qlen];
        heap_set(p, i, last);
        heap_up(p, i);
        heap_down(p, last.parent->slots[last.index].heap_index);
    }
}

/* 아직 아무도 손대지 않은 칸을 모두 큐에 넣고 그만큼 쉬는 작업 스레드를 깨운다(lock 보유). */
static void submit_kids_locked(Pool *p, DirTask *t)
{
    size_t i, pushed = 0;

    for (i = 0; t->slots != NULL && i < t->kid_count; i++) {
        const KidSlot *s = &t->slots[i];
        if (s->task == NULL && s->heap_index == HEAP_NONE &&
            !(s->flags & (SLOT_EXCLUDED | SLOT_TOO_LONG))) {
            pushed += queue_push_locked(p, t, i);
        }
    }
    /* 상한이 찼어도 하나는 깨워, 새 칸이 더 급하면 나중 것을 내쫓고 열거하게 한다. */
    wake_workers_locked(p, pool_full_locked(p) && pushed > 0 ? 1 : pushed);
}

/*
 * 필요 없어진 작업과 그 아래를 버린다(lock 보유). 열거 중이면 그 작업 스레드가 끝낸 뒤 해제하게
 * 표시하고, 끝났으면 칸들을 따라가 큐에서 빼고 해제한다.
 */
static void discard_task_locked(Pool *p, DirTask *t)
{
    DirTask **work = NULL;
    size_t len = 0, cap = 0;

    while (t != NULL) {
        if (t->state == TASK_RUNNING) {
            t->discard = 1;
        } else {
            size_t i;
            for (i = 0; t->slots != NULL && i < t->kid_count; i++) {
                KidSlot *s = &t->slots[i];
                queue_remove_locked(p, s);
                if (s->task == NULL) {
                    continue;
                }
                if (len == cap) {
                    size_t ncap = cap ? cap * 2 : 64;
                    DirTask **grown = realloc(work, ncap * sizeof(*grown));
                    if (grown == NULL) {
                        discard_task_locked(p, s->task);
                        s->task = NULL;
                        continue;
                    }
                    work = grown;
                    cap = ncap;
                }
                work[len++] = s->task;
                s->task = NULL;
            }
            ready_release_locked(p, t);
            task_free_one(t);
        }
        t = len > 0 ? work[--len] : NULL;
    }
    free(work);
}

/* 칸을 버린다(lock 보유, 작업 스레드가 없으면 NULL): 큐에서 빼고 만든 작업이 있으면 버린다. */
static void discard_slot(Pool *p, KidSlot *s, uint32_t flag)
{
    s->flags |= flag;
    if (p == NULL) {
        return;
    }
    queue_remove_locked(p, s);
    if (s->task != NULL) {
        discard_task_locked(p, s->task);
        s->task = NULL;
    }
}

/*
 * 상한이 찼을 때, 큐 맨 앞이 미리 열거해 둔 것 중 가장 나중에 필요한 작업보다 먼저 필요하면 그 작업을
 * 내쫓는다(lock 보유): 칸을 큐로 되돌리고 작업(과 그 아래)을 버린다. 내쫓았으면 1.
 */
static int evict_later_locked(Pool *p)
{
    DirTask *victim, *parent;
    KeyView kq, kv;
    size_t index;

    if (p->rlen == 0 || p->qlen == 0) {
        return 0;
    }
    victim = p->ready_heap[0];
    kq = item_key(&p->queue[0]);
    kv = task_key(victim);
    if (victim->awaited || victim->parent == NULL || !key_before(&kq, &kv)) {
        return 0;
    }
    parent = victim->parent;
    index = victim->slot_index;
    parent->slots[index].task = NULL;
    discard_task_locked(p, victim);
    queue_push_locked(p, parent, index);
    p->evicted++;
    return 1;
}

static unsigned __stdcall pool_worker(void *arg)
{
    Pool *p = arg;
    unsigned char *scratch = malloc(FETCH_BUFFER_BYTES);

    if (scratch == NULL) {
        /* 이 스레드 몫은 호출 스레드가 직접 열거한다. */
        return 0;
    }
    AcquireSRWLockExclusive(&p->lock);
    for (;;) {
        QueueItem item;
        DirTask *t;
        while (!p->stop) {
            if (p->qlen != 0) {
                if (!pool_full_locked(p) || evict_later_locked(p)) {
                    break;
                }
                p->throttled = 1;
            }
            p->idle++;
            SleepConditionVariableSRW(&p->work_cv, &p->lock, INFINITE, 0);
            p->idle--;
        }
        if (p->stop) {
            break;
        }
        /* 작업은 이제야 만든다. 부모의 열거 기록은 부모가 해제되기 전까지 바뀌지 않는다. */
        item = queue_pop_locked(p);
        t = slot_task_new(item.parent, item.index);
        if (t == NULL) {
            /* 경로가 너무 길거나 메모리가 없다. 차례가 되면 호출 스레드가 다시 해 보고 기록한다. */
            continue;
        }
        t->state = TASK_RUNNING;
        t->by_worker = 1;
        t->parent = item.parent;
        t->slot_index = item.index;
        item.parent->slots[item.index].task = t;
        ReleaseSRWLockExclusive(&p->lock);

        fetch_dir(t, scratch, p->max_depth, p->options);

        AcquireSRWLockExclusive(&p->lock);
        p->prefetched++;
        if (t->discard) {
            /* 열거하는 동안 필요 없어졌다. 칸은 아직 큐에 넣지 않았다. */
            task_free_one(t);
            continue;
        }
        t->state = TASK_DONE;
        p->ready++;
        p->ready_bytes += ready_cost(t);
        ready_push_locked(p, t);
        if (t->awaited) {
            WakeConditionVariable(&p->done_cv);
        }
        submit_kids_locked(p, t);
    }
    ReleaseSRWLockExclusive(&p->lock);
    free(scratch);
    return 0;
}

/* 작업 스레드를 띄운다. 하나도 못 띄우면 0(호출 스레드가 모두 열거한다). */
static int pool_start(Pool *p, unsigned threads, uint32_t max_depth, unsigned options)
{
    unsigned i;

    memset(p, 0, sizeof(*p));
    InitializeSRWLock(&p->lock);
    InitializeConditionVariable(&p->work_cv);
    InitializeConditionVariable(&p->done_cv);
    p->max_depth = max_depth;
    p->options = options;
    for (i = 0; i < threads && i < SCAN_MAX_THREADS; i++) {
        HANDLE h = (HANDLE)_beginthreadex(NULL, 0, pool_worker, p, 0, NULL);
        if (h == NULL) {
            break;
        }
        p->threads[p->nthreads++] = h;
    }
    return p->nthreads > 0;
}

/* 작업 스레드를 멈추고 끝날 때까지 기다린다. 남은 작업은 호출자가 해제한다. */
static void pool_stop(Pool *p)
{
    int i;

    AcquireSRWLockExclusive(&p->lock);
    p->stop = 1;
    ReleaseSRWLockExclusive(&p->lock);
    WakeAllConditionVariable(&p->work_cv);
    for (i = 0; i < p->nthreads; i++) {
        WaitForSingleObject(p->threads[i], INFINITE);
        CloseHandle(p->threads[i]);
    }
    free(p->queue);
    p->queue = NULL;
    p->qlen = 0;
    free(p->ready_heap);
    p->ready_heap = NULL;
    p->rlen = 0;
    p->nthreads = 0;
}

/* ---- 정밀 스캔: 본 파일 ID 집합 ---- */

/* (볼륨, 파일 ID) 하나. lo 와 hi 가 모두 0 이면 빈 칸(0 인 ID 는 넣지 않는다). */
typedef struct {
    uint64_t volume;
    uint64_t lo;
    uint64_t hi;
} IdKey;

typedef struct {
    IdKey *slots; /* 개방 주소법, 크기는 2의 거듭제곱 */
    size_t cap;
    size_t len;
    int failed;   /* 늘리지 못해 더 넣을 수 없음 */
} IdSet;

#define IDSET_MIN_CAP 4096

static uint64_t idset_hash(const IdKey *k)
{
    uint64_t h = k->lo ^ (k->hi * 0x9E3779B97F4A7C15ULL) ^ (k->volume * 0xC2B2AE3D27D4EB4FULL);

    /* splitmix64 마무리 */
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    return h ^ (h >> 31);
}

/* 빈 칸 또는 같은 키가 있는 칸. */
static IdKey *idset_slot(IdKey *slots, size_t cap, const IdKey *k)
{
    size_t i = (size_t)idset_hash(k) & (cap - 1);

    for (;;) {
        IdKey *s = &slots[i];
        if ((s->lo == 0 && s->hi == 0) ||
            (s->lo == k->lo && s->hi == k->hi && s->volume == k->volume)) {
            return s;
        }
        i = (i + 1) & (cap - 1);
    }
}

static int idset_grow(IdSet *set)
{
    size_t cap = set->cap ? set->cap * 2 : IDSET_MIN_CAP, i;
    IdKey *slots;

    if (cap > ((size_t)-1) / sizeof(IdKey) || (slots = calloc(cap, sizeof(IdKey))) == NULL) {
        set->failed = 1;
        return 0;
    }
    for (i = 0; i < set->cap; i++) {
        const IdKey *k = &set->slots[i];
        if (k->lo != 0 || k->hi != 0) {
            *idset_slot(slots, cap, k) = *k;
        }
    }
    free(set->slots);
    set->slots = slots;
    set->cap = cap;
    return 1;
}

/* 넣는다. 이미 있었으면 1, 새로 넣었으면 0, 메모리가 부족해 넣지 못했으면 -1. */
static int idset_add(IdSet *set, uint64_t volume, const RecId *id)
{
    IdKey k, *s;

    k.volume = volume;
    k.lo = id->lo;
    k.hi = id->hi;
    /* 70% 를 넘기 전에 늘린다. */
    if ((set->len + 1) * 10 > set->cap * 7 && (set->failed || !idset_grow(set))) {
        if (set->cap == 0) {
            return -1;
        }
        /* 늘리지 못했어도 이미 있는지는 알 수 있다. 꽉 차기 전까지만 넣는다. */
        s = idset_slot(set->slots, set->cap, &k);
        if (s->lo != 0 || s->hi != 0) {
            return 1;
        }
        if (set->len + 1 >= set->cap) {
            return -1;
        }
        *s = k;
        set->len++;
        return 0;
    }
    s = idset_slot(set->slots, set->cap, &k);
    if (s->lo != 0 || s->hi != 0) {
        return 1;
    }
    *s = k;
    set->len++;
    return 0;
}

/* ---- 깊이 우선 탐색 ---- */

/* 깊이 우선 스택. 칸 하나는 처리를 마쳤고 아직 내려가지 않은 하위 폴더가 남은 폴더 작업이다. */
typedef struct {
    DirTask **items;
    size_t len;
    size_t cap;
} DirStack;

typedef struct {
    uint32_t max_depth;
    unsigned options; /* SCAN_OPT_*(하드 링크를 지원하지 않는 볼륨이면 HARDLINKS 를 뺀다) */
    ScanReport *report;
    const ScanHooks *hooks;
    void *ctx;
    DirStack stack;
    Pool pool;
    int pooled; /* 작업 스레드가 동작 중이면 1 */
    unsigned char *scratch; /* 호출 스레드의 열거 버퍼 */
    IdSet ids;  /* 정밀 스캔: 지금까지 센 파일 */
    MemTally tally;        /* 폴더 작업 메모리 집계(진단) */
    uint64_t self_fetched; /* 호출 스레드가 직접 열거한 폴더(진단) */
    uint64_t waits;        /* 작업 스레드를 기다린 횟수(진단) */
    uint64_t prefetched;   /* 작업 스레드가 열거한 폴더(풀을 멈출 때 옮겨 둔다) */
    size_t pending;        /* 스택 칸들에 남은 하위 폴더 수(ScanDirInfo.pending, 취소 시 건너뛴 수) */
} Walker;

static int stack_push(DirStack *stack, DirTask *t)
{
    if (stack->len == stack->cap) {
        size_t cap = stack->cap ? stack->cap * 2 : 16;
        DirTask **grown = realloc(stack->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return 0;
        }
        stack->items = grown;
        stack->cap = cap;
    }
    stack->items[stack->len++] = t;
    return 1;
}

/* 칸을 버린다(should_skip_dir 로 뺐거나 경로가 너무 길거나 취소). */
static void walker_drop_slot(Walker *w, DirTask *t, size_t i, uint32_t flag)
{
    if (t->slots == NULL) {
        return;
    }
    if (w->pooled) {
        AcquireSRWLockExclusive(&w->pool.lock);
        discard_slot(&w->pool, &t->slots[i], flag);
        ReleaseSRWLockExclusive(&w->pool.lock);
    } else {
        discard_slot(NULL, &t->slots[i], flag);
    }
}

/* 스택에 올리지 못한 작업 t 와 그 칸들(큐에 있거나 미리 열거한 작업)을 모두 버린다. */
static void walker_drop_all(Walker *w, DirTask *t)
{
    size_t i;

    if (w->pooled) {
        AcquireSRWLockExclusive(&w->pool.lock);
        for (i = 0; t->slots != NULL && i < t->kid_count; i++) {
            discard_slot(&w->pool, &t->slots[i], SLOT_EXCLUDED);
        }
        ReleaseSRWLockExclusive(&w->pool.lock);
    }
    task_free_tree(t);
}

/*
 * 스택 칸 f 의 칸 i 차례가 된 하위 폴더 작업을 넘겨받아 열거 결과를 준비한다. 작업 스레드가 열거했으면
 * 그 결과를 받고(열거 중이면 끝날 때까지 기다림), 아직이면 큐에서 빼고 직접 만들어 열거한다.
 * 작업을 만들지 못하면(메모리 부족) NULL.
 */
static DirTask *walker_take(Walker *w, DirTask *f, size_t i)
{
    KidSlot *s = &f->slots[i];
    DirTask *t = NULL;

    if (w->pooled) {
        AcquireSRWLockExclusive(&w->pool.lock);
        queue_remove_locked(&w->pool, s);
        t = s->task;
        s->task = NULL;
        if (t != NULL) {
            if (t->state != TASK_DONE) {
                w->waits++;
                t->awaited = 1;
            }
            while (t->state != TASK_DONE) {
                SleepConditionVariableSRW(&w->pool.done_cv, &w->pool.lock, INFINITE, 0);
            }
            ready_release_locked(&w->pool, t);
        }
        ReleaseSRWLockExclusive(&w->pool.lock);
        if (t != NULL) {
            return t;
        }
    }
    t = slot_task_new(f, i);
    if (t == NULL) {
        return NULL;
    }
    w->self_fetched++;
    fetch_dir(t, w->scratch, w->max_depth, w->options);
    t->state = TASK_DONE;
    if (w->pooled && t->kid_count > 0) {
        AcquireSRWLockExclusive(&w->pool.lock);
        submit_kids_locked(&w->pool, t);
        ReleaseSRWLockExclusive(&w->pool.lock);
    }
    return t;
}

/*
 * 정밀 스캔: 파일이 이미 다른 경로로 센 실제 파일인지 확인한다(호출 스레드에서 훅 순서대로 부르므로
 * 작업 스레드 수와 상관없이 같은 경로가 "처음"이 된다).
 */
static void account_hardlink(Walker *w, const DirTask *t, const EntryRec *rec, ScanEntry *entry)
{
    ScanReport *report = w->report;
    const RecId *id = rec_id(rec);
    int seen;

    if (id == NULL || (id->lo == 0 && id->hi == 0)) {
        report->hardlink_unknown_files++;
        return;
    }
    seen = idset_add(&w->ids, t->volume, id);
    if (seen < 0) {
        report->hardlink_unknown_files++;
    } else if (seen > 0) {
        entry->hardlink_dup = 1;
        report->hardlink_dup_files++;
        report->hardlink_dup_bytes += entry->size;
        report->hardlink_dup_alloc += entry->alloc_size;
    }
}

static void account_entry(const EntryRec *rec, ScanReport *report, ScanEntry *entry)
{
    entry->hardlink_dup = 0;
    entry->name = rec_name(rec);
    entry->attributes = rec->attributes;
    entry->is_dir = (rec->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry->is_reparse = (rec->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry->descend = scan_should_descend(entry->attributes);

    if (entry->is_dir) {
        entry->size = 0;
        entry->alloc_size = 0;
        entry->alloc_known = 1;
        report->dir_count++;
        if (entry->is_reparse) {
            report->reparse_count++;
        }
    } else {
        entry->size = rec->size;
        entry->alloc_known = (rec->flags & REC_ALLOC_KNOWN) != 0;
        entry->alloc_size = entry->alloc_known ? rec->alloc : rec->size;
        report->file_count++;
        report->total_bytes += entry->size;
        scan_report_account_alloc(report, entry->size, rec->alloc, entry->alloc_known,
                                  entry->attributes);
    }
}

/*
 * 열거해 둔 폴더 t 의 항목을 집계하고 훅을 부르며, 하위 탐색할 폴더의 칸에 on_entry 결과를 적는다.
 * 폴더를 열지 못했으면 SCAN_FATAL, 도중 오류·취소는 SCAN_PARTIAL.
 * 열지 못한 폴더와 내려갈 수 없는 하위 폴더(경로가 너무 김·메모리 부족)는 skipped_dir_count 로 센다.
 * 끝나면 t->next_kid 가 내려갈 칸 수가 되고(0 이 아니면 호출자가 스택에 올린다), 그 수만큼
 * w->pending 이 는다. 제외·취소로 빠진 칸은 버린다.
 */
static ScanStatus process_dir(Walker *w, DirTask *t)
{
    const ScanHooks *hooks = w->hooks;
    ScanReport *report = w->report;
    ScanStatus status = SCAN_COMPLETE;
    ScanDirInfo info;
    unsigned seen = 0;
    size_t off, kid = 0, live = 0;

    t->processed = 1;
    info.id = t->id;
    info.path = t->path;
    info.depth = t->depth;
    info.pending = w->pending;
    if (hooks->on_dir_begin != NULL) {
        hooks->on_dir_begin(&info, w->ctx);
    }

    if (t->open_error != 0) {
        record_error(report, t->path, t->open_error);
        report->skipped_dir_count++;
        status = SCAN_FATAL;
    } else {
        for (off = 0; off < t->len;) {
            const EntryRec *rec = (const EntryRec *)(t->buf + off);
            ScanEntry entry;
            uintptr_t child_id = 0;
            int will_descend;
            size_t slot = kid;

            off += rec_size(rec);
            /* 큰 디렉터리에서도 취소가 늦지 않도록 열거 도중에도 확인한다. */
            if (hooks->should_cancel != NULL && ++seen % SCAN_CANCEL_CHECK_INTERVAL == 0 &&
                hooks->should_cancel(w->ctx)) {
                report->cancelled = 1;
                status = SCAN_PARTIAL;
                break;
            }
            account_entry(rec, report, &entry);
            if ((w->options & SCAN_OPT_HARDLINKS) && !entry.is_dir) {
                account_hardlink(w, t, rec, &entry);
            }
            /* fetch_dir 가 같은 판단으로 칸을 만들었다(slots 를 못 만들었으면 없음). */
            will_descend = rec_wants_descend(t, rec, w->max_depth);
            if (will_descend) {
                kid++;
            }
            if (will_descend && hooks->should_skip_dir != NULL) {
                ScanDirInfo parent = info;
                parent.pending = w->pending + live;
                if (hooks->should_skip_dir(&entry, &parent, w->ctx)) {
                    report->excluded_dir_count++;
                    will_descend = 0;
                    walker_drop_slot(w, t, slot, SLOT_EXCLUDED);
                }
            }
            if (hooks->on_entry != NULL) {
                child_id = hooks->on_entry(&entry, t->id, will_descend, w->ctx);
            }

            if (will_descend) {
                int too_long = t->path_len + 1 + rec->name_len > PATH_MAX_WIDE;
                if (t->slots == NULL || too_long) {
                    /* 경로가 너무 길거나 칸을 만들지 못했다. */
                    record_error(report, t->path,
                                 too_long ? ERROR_FILENAME_EXCED_RANGE : ERROR_NOT_ENOUGH_MEMORY);
                    report->skipped_dir_count++;
                    status = SCAN_PARTIAL;
                    if (too_long) {
                        walker_drop_slot(w, t, slot, SLOT_TOO_LONG);
                    }
                } else {
                    t->slots[slot].id = child_id;
                    live++;
                }
            }
        }
        if (!report->cancelled && t->enum_error != 0) {
            record_error(report, t->path, t->enum_error);
            status = SCAN_PARTIAL;
        }
    }
    /* 취소로 멈췄으면 남은 칸을 버린다. */
    for (; t->slots != NULL && kid < t->kid_count; kid++) {
        walker_drop_slot(w, t, kid, SLOT_EXCLUDED);
    }
    t->next_kid = t->slots != NULL ? t->kid_count : 0;
    w->pending += live;

    if (hooks->on_dir_end != NULL) {
        info.pending = w->pending;
        hooks->on_dir_end(&info, status, w->ctx);
    }
    return status;
}

/* 스택 칸 f 의 다음 차례 칸 번호. 남은 칸이 없으면 0 을 돌려준다(있으면 번호 + 1). */
static size_t next_live_slot(DirTask *f)
{
    while (f->next_kid > 0 &&
           (f->slots[f->next_kid - 1].flags & (SLOT_EXCLUDED | SLOT_TOO_LONG))) {
        f->next_kid--;
    }
    if (f->next_kid == 0) {
        return 0;
    }
    return f->next_kid--;
}

/* 남은 칸 수(제외·너무 긴 경로 제외). */
static size_t live_slots(const DirTask *f)
{
    size_t i, n = 0;

    for (i = 0; i < f->next_kid; i++) {
        n += !(f->slots[i].flags & (SLOT_EXCLUDED | SLOT_TOO_LONG));
    }
    return n;
}

/*
 * 스택에 올릴 작업의 열거 결과에서 남은 하위 폴더 기록만 남기고(파일·제외한 폴더 기록을 버림)
 * 메모리를 줄인다. 작업 스레드는 칸의 이름을 lock 안에서만 읽으므로 lock 을 쥔 채 부른다.
 */
static void frame_compact(DirTask *t)
{
    size_t i, out = 0;
    unsigned char *fit;

    for (i = 0; i < t->kid_count; i++) {
        KidSlot *s = &t->slots[i];
        const EntryRec *rec;
        size_t n;
        if (s->flags & (SLOT_EXCLUDED | SLOT_TOO_LONG)) {
            continue; /* 차례가 와도 건너뛰므로 기록이 필요 없다 */
        }
        rec = (const EntryRec *)(t->buf + s->rec_off);
        n = rec_size(rec);
        /* 칸은 열거 순서라 기록 위치가 커지기만 하므로 앞으로 당겨도 겹치지 않는다. */
        memmove(t->buf + out, rec, n);
        s->rec_off = (uint32_t)out;
        out += n;
    }
    t->len = out;
    if (out > 0 && out < t->cap && (fit = realloc(t->buf, out)) != NULL) {
        tally_add(t->tally, 0, -(LONG64)(t->cap - out));
        t->buf = fit;
        t->cap = out;
    }
}

/*
 * 처리를 마친 작업 t 를 스택에 올린다(내려갈 칸이 없으면 바로 해제). 올리지 못하면 남은 하위 폴더를
 * 건너뛴 것으로 기록하고 SCAN_PARTIAL.
 */
static ScanStatus walker_push(Walker *w, DirTask *t)
{
    size_t left;

    if (t->next_kid == 0) {
        task_free_one(t);
        return SCAN_COMPLETE;
    }
    if (w->pooled) {
        AcquireSRWLockExclusive(&w->pool.lock);
        frame_compact(t);
        ReleaseSRWLockExclusive(&w->pool.lock);
    } else {
        frame_compact(t);
    }
    if (stack_push(&w->stack, t)) {
        return SCAN_COMPLETE;
    }
    left = live_slots(t);
    record_error(w->report, t->path, ERROR_NOT_ENOUGH_MEMORY);
    w->report->skipped_dir_count += left;
    w->pending -= left;
    walker_drop_all(w, t);
    return SCAN_PARTIAL;
}

/* 볼륨의 클러스터 크기(모르면 0). */
static uint32_t volume_cluster_bytes(const wchar_t *ext)
{
    wchar_t volume[MAX_PATH];
    DWORD spc, bps, free_clusters, total_clusters;

    if (!GetVolumePathNameW(ext, volume, MAX_PATH) ||
        !GetDiskFreeSpaceW(volume, &spc, &bps, &free_clusters, &total_clusters)) {
        return 0;
    }
    return spc * bps;
}

/* 루트 경로를 검증해 확장 경로로 돌려준다. 실패하면 오류를 기록하고 NULL. */
static wchar_t *open_root(const wchar_t *dir, ScanReport *report)
{
    wchar_t *ext;
    DWORD attrs;

    if (dir == NULL || dir[0] == L'\0') {
        record_error(report, dir, ERROR_INVALID_PARAMETER);
        return NULL;
    }

    ext = path_to_extended(dir);
    if (ext == NULL) {
        record_error(report, dir, ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }

    /* 빈 디렉터리와 존재하지 않는 경로를 구분하기 위해 먼저 확인한다. */
    attrs = GetFileAttributesW(ext);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        record_error(report, ext, GetLastError());
        free(ext);
        return NULL;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        record_error(report, ext, ERROR_DIRECTORY);
        free(ext);
        return NULL;
    }
    if (report->cluster_bytes == 0) {
        report->cluster_bytes = volume_cluster_bytes(ext);
    }
    return ext;
}

/* 작업 스레드를 멈추고 남은 작업을 모두 해제한다. */
static void walker_finish(Walker *w)
{
    size_t i;

    if (w->pooled) {
        /* 멈추는 동안 열거를 마친 작업까지 세도록 스레드가 끝난 뒤 읽는다. */
        pool_stop(&w->pool);
        w->prefetched = w->pool.prefetched;
        w->pooled = 0;
    }
    for (i = 0; i < w->stack.len; i++) {
        task_free_tree(w->stack.items[i]);
    }
    free(w->stack.items);
    free(w->scratch);
    free(w->ids.slots);

    /* 진단 값: 최대값은 더 큰 쪽, 횟수는 더한다(scan_tree_ex 를 여러 번 부르는 경우). */
    if ((uint64_t)w->tally.peak_tasks > w->report->peak_tasks) {
        w->report->peak_tasks = (uint64_t)w->tally.peak_tasks;
    }
    if ((uint64_t)w->tally.peak_bytes > w->report->peak_task_bytes) {
        w->report->peak_task_bytes = (uint64_t)w->tally.peak_bytes;
    }
    w->report->prefetched_dirs += w->prefetched;
    w->report->self_fetched_dirs += w->self_fetched;
    w->report->prefetch_waits += w->waits;
    w->report->discarded_dirs += (uint64_t)w->tally.discarded;
}

/* 폴더가 있는 볼륨이 하드 링크를 지원하는지(모르면 지원한다고 보고 ID 로 확인한다). */
static int volume_supports_hardlinks(const wchar_t *ext)
{
    HANDLE h = CreateFileW(ext, FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    DWORD flags = 0;
    BOOL ok;

    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    ok = GetVolumeInformationByHandleW(h, NULL, 0, NULL, NULL, &flags, NULL, 0);
    CloseHandle(h);
    return !ok || (flags & FILE_SUPPORTS_HARD_LINKS) != 0;
}

ScanStatus scan_tree_ex(const wchar_t *dir, uint32_t max_depth, uintptr_t root_id,
                        ScanReport *report, const ScanHooks *hooks, void *ctx)
{
    return scan_tree_opts(dir, max_depth, root_id, report, hooks, ctx, 0);
}

ScanStatus scan_tree_opts(const wchar_t *dir, uint32_t max_depth, uintptr_t root_id,
                          ScanReport *report, const ScanHooks *hooks, void *ctx,
                          unsigned options)
{
    static const ScanHooks no_hooks = {NULL, NULL, NULL, NULL, NULL};
    ScanStatus status = SCAN_COMPLETE;
    ScanStatus result;
    unsigned threads;
    DirTask *t;
    Walker w;
    wchar_t *ext;

    if (report == NULL) {
        return SCAN_FATAL;
    }
    if (hooks == NULL) {
        hooks = &no_hooks;
    }

    ext = open_root(dir, report);
    if (ext == NULL) {
        report->aborted = 1;
        return SCAN_FATAL;
    }

    memset(&w, 0, sizeof(w));
    w.max_depth = max_depth;
    w.options = options;
    if (options & SCAN_OPT_HARDLINKS) {
        report->hardlinks_checked = 1;
        /* FAT·exFAT 처럼 하드 링크가 없으면 ID 를 모을 필요가 없다(FAT 의 ID 는 믿을 수도 없다). */
        if (!volume_supports_hardlinks(ext)) {
            report->hardlinks_unsupported = 1;
            w.options &= ~(unsigned)SCAN_OPT_HARDLINKS;
        }
    }
    w.report = report;
    w.hooks = hooks;
    w.ctx = ctx;
    w.scratch = malloc(FETCH_BUFFER_BYTES);
    t = task_new(ext, wcslen(ext), NULL, 0, NULL, 0, 0, 0, 0, &w.tally);
    free(ext);
    if (w.scratch == NULL || t == NULL) {
        record_error(report, dir, ERROR_NOT_ENOUGH_MEMORY);
        report->aborted = 1;
        free(w.scratch);
        if (t != NULL) {
            task_free_one(t);
        }
        return SCAN_FATAL;
    }
    t->id = root_id;

    /* 루트는 직접 열거하고, 하위 폴더가 있을 때만 작업 스레드를 띄운다. */
    w.self_fetched++;
    fetch_dir(t, w.scratch, max_depth, w.options);
    threads = scan_effective_threads();
    if (t->kid_count > 0 && threads > 1 && pool_start(&w.pool, threads, max_depth, w.options)) {
        w.pooled = 1;
        AcquireSRWLockExclusive(&w.pool.lock);
        submit_kids_locked(&w.pool, t);
        ReleaseSRWLockExclusive(&w.pool.lock);
        WakeAllConditionVariable(&w.pool.work_cv);
    }
    t->state = TASK_DONE;
    result = process_dir(&w, t);
    if (result == SCAN_FATAL) {
        /* 루트를 열지 못하면 하위 트리 전체를 알 수 없으므로 중단한다. */
        task_free_one(t);
        report->aborted = 1;
        walker_finish(&w);
        return SCAN_FATAL;
    }
    if (result != SCAN_COMPLETE || walker_push(&w, t) != SCAN_COMPLETE) {
        status = SCAN_PARTIAL;
    }

    /*
     * 스택 맨 위 폴더의 남은 하위 폴더를 마지막 것부터 하나씩 내려가므로 깊이 우선으로 진행된다.
     * 하위 디렉터리 실패(SCAN_FATAL 포함)는 그 디렉터리만 건너뛰고 계속한다.
     */
    while (w.stack.len > 0) {
        DirTask *f = w.stack.items[w.stack.len - 1];
        size_t i;

        if (report->cancelled ||
            (hooks->should_cancel != NULL && hooks->should_cancel(ctx))) {
            /* 남은 디렉터리는 열거하지 못한 것으로 센다. */
            report->cancelled = 1;
            report->skipped_dir_count += w.pending;
            status = SCAN_PARTIAL;
            break;
        }
        i = next_live_slot(f);
        if (i == 0) {
            /* 이 폴더의 하위 폴더를 다 돌았다. */
            w.stack.len--;
            task_free_one(f);
            continue;
        }
        i--;
        w.pending--;
        t = walker_take(&w, f, i);
        if (t == NULL) {
            record_error(report, f->path, ERROR_NOT_ENOUGH_MEMORY);
            report->skipped_dir_count++;
            status = SCAN_PARTIAL;
            continue;
        }
        t->id = f->slots[i].id;
        result = process_dir(&w, t);
        if (result != SCAN_COMPLETE) {
            status = SCAN_PARTIAL;
        }
        if (walker_push(&w, t) != SCAN_COMPLETE) {
            status = SCAN_PARTIAL;
        }
    }

    walker_finish(&w);
    return status;
}

/* scan_tree 의 visit 콜백을 on_entry 훅으로 옮긴다. */
typedef struct {
    scan_visit_fn visit;
    void *ctx;
} VisitAdapter;

static uintptr_t adapt_visit(const ScanEntry *entry, uintptr_t parent,
                             int will_descend, void *ctx)
{
    VisitAdapter *a = ctx;

    (void)parent;
    (void)will_descend;
    a->visit(entry, a->ctx);
    return 0;
}

ScanStatus scan_tree(const wchar_t *dir, uint32_t max_depth, ScanReport *report,
                     scan_visit_fn visit, void *ctx)
{
    ScanHooks hooks = {NULL, NULL, NULL, NULL};
    VisitAdapter adapter;

    adapter.visit = visit;
    adapter.ctx = ctx;
    if (visit != NULL) {
        hooks.on_entry = adapt_visit;
    }
    return scan_tree_ex(dir, max_depth, 0, report, &hooks, &adapter);
}

ScanStatus scan_directory(const wchar_t *dir, ScanReport *report,
                          scan_visit_fn visit, void *ctx)
{
    return scan_tree(dir, 0, report, visit, ctx);
}
