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

typedef enum { TASK_QUEUED = 0, TASK_RUNNING, TASK_DONE } TaskState;

/*
 * 열거할 폴더 하나와 그 결과.
 * 폴더를 열거한 쪽(작업 스레드 또는 호출 스레드)이 하위 탐색 대상 폴더마다 자식 작업을 미리
 * 만들어 kids 에 둔다(재분석 지점이 아니고 깊이 제한 안인 폴더, 열거 순서). 작업 스레드가 있으면
 * 자식도 바로 큐에 넣어 미리 열거하므로, 호출 스레드가 깊이 우선으로 내려갈 때 기다리지 않는다.
 * 호출 스레드는 항목을 처리하며 kids 를 차례로 넘겨받고, 훅이 제외한 폴더의 작업은 버린다.
 */
typedef struct DirTask {
    wchar_t *path;      /* 확장 경로(구조체와 같은 할당) */
    uint32_t *key;      /* 처리 순서 키(구조체와 같은 할당, 아래 설명) */
    unsigned char *buf; /* EntryRec 이어 붙임 */
    struct DirTask **kids; /* 하위 탐색 대상 폴더마다 하나(만들지 못했으면 NULL) */
    uintptr_t id;       /* 호출 스레드가 on_entry 결과로 채운다 */
    size_t path_len;
    size_t len;
    size_t cap;
    size_t kid_count;
    size_t heap_index;  /* 큐(힙) 안의 위치, 없으면 HEAP_NONE */
    DWORD open_error;   /* 0 이 아니면 폴더를 열지 못함 */
    DWORD enum_error;   /* 0 이 아니면 열거 도중 오류(그때까지의 항목은 유효) */
    uint32_t depth;
    uint64_t volume;    /* 정밀 스캔: 이 폴더가 있는 볼륨의 일련번호(모르면 0) */
    /*
     * 깊이 우선 처리 순서 키의 길이. 키는 루트에서 이 폴더까지 각 단계의 (형제 수 - 1 - 열거 순번)
     * 이다. 호출 스레드는 마지막 자식부터 내려가므로 키를 사전순으로 비교하면 처리 순서와 같다.
     * 작업 스레드는 키가 가장 작은(가장 먼저 필요한) 작업부터 열거한다.
     */
    uint32_t key_len;
    unsigned char state;     /* TaskState. 작업 스레드가 있으면 Pool.lock 으로 보호 */
    unsigned char by_worker; /* 작업 스레드가 열거했으면 1 */
    unsigned char discard;   /* 작업 스레드가 열거하는 도중 버려졌으면 1(그 스레드가 해제) */
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
 */
static DirTask *task_new(const wchar_t *dir, size_t dir_len, const wchar_t *name, size_t name_len,
                         const uint32_t *key, uint32_t key_len, int has_last, uint32_t last,
                         uint32_t depth)
{
    DirTask *t;
    size_t sep, path_len;
    uint32_t klen = key_len + (has_last ? 1u : 0u);
    wchar_t *path;

    sep = name != NULL && dir_len > 0 && dir[dir_len - 1] != L'\\' && dir[dir_len - 1] != L'/' &&
          dir[dir_len - 1] != L':';
    path_len = dir_len + sep + name_len;
    if (path_len > PATH_MAX_WIDE) {
        return NULL;
    }
    t = malloc(sizeof(*t) + klen * sizeof(uint32_t) + (path_len + 1) * sizeof(wchar_t));
    if (t == NULL) {
        return NULL;
    }
    memset(t, 0, sizeof(*t));
    t->depth = depth;
    t->heap_index = HEAP_NONE;
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

/* 작업 하나만 해제한다(kids 가 가리키는 작업은 건드리지 않는다). */
static void task_free_one(DirTask *t)
{
    free(t->buf);
    free(t->kids);
    free(t);
}

/* 작업과 아직 넘겨받지 않은 자식 작업 전체를 해제한다. 작업 스레드가 없을 때만 부른다. */
static void task_free_tree(DirTask *t)
{
    DirTask **work = NULL;
    size_t len = 0, cap = 0;

    while (t != NULL) {
        size_t i;
        for (i = 0; i < t->kid_count; i++) {
            if (t->kids[i] == NULL) {
                continue;
            }
            if (len == cap) {
                size_t ncap = cap ? cap * 2 : 64;
                DirTask **grown = realloc(work, ncap * sizeof(*grown));
                if (grown == NULL) {
                    /* 메모리가 없으면 재귀로라도 해제한다(깊이는 폴더 깊이로 제한). */
                    task_free_tree(t->kids[i]);
                    continue;
                }
                work = grown;
                cap = ncap;
            }
            work[len++] = t->kids[i];
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
 * 폴더 하나를 열거해 t 에 담고 자식 작업을 만든다. 어느 스레드에서나 부를 수 있다
 * (t 와 새로 만든 자식 외에는 건드리지 않음).
 */
static void fetch_dir(DirTask *t, unsigned char *scratch, uint32_t max_depth, unsigned options)
{
    size_t off, n = 0;

    fetch_records(t, scratch, options);
    for (off = 0; off < t->len; off += rec_size((const EntryRec *)(t->buf + off))) {
        n += rec_wants_descend(t, (const EntryRec *)(t->buf + off), max_depth);
    }
    if (n == 0) {
        return;
    }
    t->kids = calloc(n, sizeof(*t->kids));
    if (t->kids == NULL) {
        /* 자식 작업 없이 두면 호출 스레드가 메모리 부족으로 기록한다. */
        return;
    }
    t->kid_count = n;
    n = 0;
    for (off = 0; off < t->len;) {
        const EntryRec *rec = (const EntryRec *)(t->buf + off);
        off += rec_size(rec);
        if (rec_wants_descend(t, rec, max_depth)) {
            t->kids[n] = task_new(t->path, t->path_len, rec_name(rec), rec->name_len, t->key,
                                  t->key_len, 1, (uint32_t)(t->kid_count - 1 - n), t->depth + 1);
            n++;
        }
    }
}

/* ---- 미리 열거하는 작업 스레드 ---- */

/*
 * 미리 열거해 두고 아직 처리하지 않은 폴더의 상한(메모리 보호). 보통은 호출 스레드가 바로 뒤따라와
 * 닿지 않지만, 큰 폴더 하나를 열거하는 동안 다른 작업 스레드가 뒤쪽 하위 트리를 앞서 읽어 둘 수
 * 있어 넉넉히 잡는다. 상한에 닿으면 작업 스레드가 쉬고 호출 스레드가 직접 열거한다(결과는 같다).
 */
#define POOL_READY_LIMIT 65536
#define POOL_READY_BYTES ((size_t)32 * 1024 * 1024)

typedef struct {
    SRWLOCK lock;
    CONDITION_VARIABLE work_cv; /* 할 일이 생김·여유가 생김·중지 */
    CONDITION_VARIABLE done_cv; /* 작업 하나가 끝남 */
    DirTask **queue;            /* TASK_QUEUED 인 작업의 최소 힙(task_before 순서) */
    size_t qlen;
    size_t qcap;
    size_t ready; /* 작업 스레드가 끝냈고 아직 넘겨받거나 버리지 않은 작업 */
    size_t ready_bytes; /* 그 작업들이 붙잡은 메모리 추정치(ready_cost 합) */
    int throttled;      /* 상한 때문에 쉬는 작업 스레드가 있으면 1 */
    int stop;
    uint32_t max_depth;
    unsigned options; /* SCAN_OPT_* */
    HANDLE threads[SCAN_MAX_THREADS];
    int nthreads;
} Pool;

/*
 * 미리 열거한 작업 하나가 붙잡고 있는 메모리 추정치: 열거 결과와, 그 결과로 만든 자식 작업
 * (작업 구조·경로·키, 할당 머리 포함). 넣을 때와 뺄 때 같은 값이 나와야 한다.
 */
static size_t ready_cost(const DirTask *t)
{
    size_t per_kid = sizeof(DirTask) + sizeof(*t->kids) + (t->path_len + 32) * sizeof(wchar_t) +
                     (t->key_len + 1) * sizeof(uint32_t) + 16;

    return sizeof(*t) + t->cap + t->kid_count * per_kid;
}

static int pool_full_locked(const Pool *p)
{
    return p->ready >= POOL_READY_LIMIT || p->ready_bytes >= POOL_READY_BYTES;
}

/* 작업 스레드가 끝낸 작업을 넘겨받거나 버릴 때 상한 계산에서 뺀다(lock 보유). */
static void ready_release_locked(Pool *p, const DirTask *t)
{
    if (!t->by_worker) {
        return;
    }
    p->ready--;
    p->ready_bytes -= ready_cost(t);
    if (p->throttled && !pool_full_locked(p)) {
        p->throttled = 0;
        WakeAllConditionVariable(&p->work_cv);
    }
}

/* a 가 b 보다 먼저 처리되는지(키 사전순, 접두사가 같으면 짧은 쪽). */
static int task_before(const DirTask *a, const DirTask *b)
{
    uint32_t i, n = a->key_len < b->key_len ? a->key_len : b->key_len;

    for (i = 0; i < n; i++) {
        if (a->key[i] != b->key[i]) {
            return a->key[i] < b->key[i];
        }
    }
    return a->key_len < b->key_len;
}

static void heap_set(Pool *p, size_t i, DirTask *t)
{
    p->queue[i] = t;
    t->heap_index = i;
}

static void heap_up(Pool *p, size_t i)
{
    DirTask *t = p->queue[i];

    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!task_before(t, p->queue[parent])) {
            break;
        }
        heap_set(p, i, p->queue[parent]);
        i = parent;
    }
    heap_set(p, i, t);
}

static void heap_down(Pool *p, size_t i)
{
    DirTask *t = p->queue[i];

    for (;;) {
        size_t l = 2 * i + 1, best = i;
        const DirTask *best_t = t;
        if (l < p->qlen && task_before(p->queue[l], best_t)) {
            best = l;
            best_t = p->queue[l];
        }
        if (l + 1 < p->qlen && task_before(p->queue[l + 1], best_t)) {
            best = l + 1;
        }
        if (best == i) {
            break;
        }
        heap_set(p, i, p->queue[best]);
        i = best;
    }
    heap_set(p, i, t);
}

/* 큐에 넣는다(lock 보유). 큐를 늘리지 못하면 넣지 않는다(나중에 호출 스레드가 열거). */
static void queue_push_locked(Pool *p, DirTask *t)
{
    if (p->qlen == p->qcap) {
        size_t cap = p->qcap ? p->qcap * 2 : 256;
        DirTask **grown = realloc(p->queue, cap * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        p->queue = grown;
        p->qcap = cap;
    }
    heap_set(p, p->qlen++, t);
    heap_up(p, p->qlen - 1);
}

/* 가장 먼저 필요한 작업을 꺼낸다(lock 보유, 큐가 비어 있지 않아야 함). */
static DirTask *queue_pop_locked(Pool *p)
{
    DirTask *t = p->queue[0];

    t->heap_index = HEAP_NONE;
    if (--p->qlen > 0) {
        heap_set(p, 0, p->queue[p->qlen]);
        heap_down(p, 0);
    }
    return t;
}

static void queue_remove_locked(Pool *p, DirTask *t)
{
    size_t i = t->heap_index;

    if (i == HEAP_NONE) {
        return;
    }
    t->heap_index = HEAP_NONE;
    if (i == --p->qlen) {
        return;
    }
    {
        DirTask *last = p->queue[p->qlen];
        heap_set(p, i, last);
        heap_up(p, i);
        heap_down(p, last->heap_index);
    }
}

/* 자식 작업을 큐에 넣는다. */
static void submit_kids_locked(Pool *p, DirTask *t)
{
    size_t i;

    for (i = 0; i < t->kid_count; i++) {
        if (t->kids[i] != NULL) {
            queue_push_locked(p, t->kids[i]);
        }
    }
}

/*
 * 필요 없어진 작업과 그 자식들을 버린다(lock 보유). 큐에 있으면 빼서 해제하고, 열거 중이면
 * 그 작업 스레드가 끝낸 뒤 해제하게 표시하며, 끝났으면 자식까지 따라가 해제한다.
 */
static void discard_locked(Pool *p, DirTask *t)
{
    DirTask **work = NULL;
    size_t len = 0, cap = 0;

    while (t != NULL) {
        if (t->state == TASK_QUEUED) {
            queue_remove_locked(p, t);
            task_free_one(t);
        } else if (t->state == TASK_RUNNING) {
            t->discard = 1;
        } else {
            size_t i;
            for (i = 0; i < t->kid_count; i++) {
                if (t->kids[i] == NULL) {
                    continue;
                }
                if (len == cap) {
                    size_t ncap = cap ? cap * 2 : 64;
                    DirTask **grown = realloc(work, ncap * sizeof(*grown));
                    if (grown == NULL) {
                        discard_locked(p, t->kids[i]);
                        continue;
                    }
                    work = grown;
                    cap = ncap;
                }
                work[len++] = t->kids[i];
            }
            ready_release_locked(p, t);
            task_free_one(t);
        }
        t = len > 0 ? work[--len] : NULL;
    }
    free(work);
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
        DirTask *t;
        while (!p->stop && (p->qlen == 0 || pool_full_locked(p))) {
            if (p->qlen != 0) {
                p->throttled = 1;
            }
            SleepConditionVariableSRW(&p->work_cv, &p->lock, INFINITE, 0);
        }
        if (p->stop) {
            break;
        }
        t = queue_pop_locked(p);
        t->state = TASK_RUNNING;
        t->by_worker = 1;
        ReleaseSRWLockExclusive(&p->lock);

        fetch_dir(t, scratch, p->max_depth, p->options);

        AcquireSRWLockExclusive(&p->lock);
        if (t->discard) {
            /* 열거하는 동안 필요 없어졌다. 자식은 아직 아무에게도 넘기지 않았다. */
            task_free_tree(t);
            continue;
        }
        submit_kids_locked(p, t);
        t->state = TASK_DONE;
        p->ready++;
        p->ready_bytes += ready_cost(t);
        WakeAllConditionVariable(&p->done_cv);
        if (t->kid_count > 0) {
            WakeAllConditionVariable(&p->work_cv);
        }
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

/* 필요 없어진 자식 작업을 버린다. */
static void walker_discard(Walker *w, DirTask *t)
{
    if (t == NULL) {
        return;
    }
    if (w->pooled) {
        AcquireSRWLockExclusive(&w->pool.lock);
        discard_locked(&w->pool, t);
        ReleaseSRWLockExclusive(&w->pool.lock);
    } else {
        task_free_tree(t);
    }
}

/*
 * 처리할 차례가 된 작업을 넘겨받아 열거 결과를 준비한다. 아무도 시작하지 않았으면 큐에서 빼고
 * 직접 열거하고, 작업 스레드가 열거 중이면 끝날 때까지 기다린다.
 */
static void walker_obtain(Walker *w, DirTask *t)
{
    int self = 1;

    if (w->pooled) {
        AcquireSRWLockExclusive(&w->pool.lock);
        if (t->state == TASK_QUEUED) {
            queue_remove_locked(&w->pool, t);
            t->state = TASK_RUNNING;
        } else {
            while (t->state != TASK_DONE) {
                SleepConditionVariableSRW(&w->pool.done_cv, &w->pool.lock, INFINITE, 0);
            }
            ready_release_locked(&w->pool, t);
            self = 0;
        }
        ReleaseSRWLockExclusive(&w->pool.lock);
    }
    if (!self) {
        return;
    }
    fetch_dir(t, w->scratch, w->max_depth, w->options);
    if (w->pooled && t->kid_count > 0) {
        AcquireSRWLockExclusive(&w->pool.lock);
        submit_kids_locked(&w->pool, t);
        ReleaseSRWLockExclusive(&w->pool.lock);
        WakeAllConditionVariable(&w->pool.work_cv);
    }
    t->state = TASK_DONE;
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
 * 열거해 둔 폴더 t 의 항목을 집계하고 훅을 부르며, 하위 탐색할 폴더의 작업을 스택에 쌓는다.
 * 폴더를 열지 못했으면 SCAN_FATAL, 도중 오류·취소는 SCAN_PARTIAL.
 * 열지 못한 폴더와 스택에 넣지 못한 하위 폴더는 skipped_dir_count 로 센다.
 * 넘겨받지 않은 t 의 자식 작업은 모두 버리거나 스택으로 옮긴다(t->kids 는 비운다).
 */
static ScanStatus process_dir(Walker *w, DirTask *t)
{
    const ScanHooks *hooks = w->hooks;
    ScanReport *report = w->report;
    ScanStatus status = SCAN_COMPLETE;
    ScanDirInfo info;
    unsigned seen = 0;
    size_t off, kid = 0;

    info.id = t->id;
    info.path = t->path;
    info.depth = t->depth;
    info.pending = w->stack.len;
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
            DirTask *ct = NULL;

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
            will_descend = rec_wants_descend(t, rec, w->max_depth);
            if (will_descend) {
                /* fetch_dir 가 같은 판단으로 만든 자식 작업(kids 를 못 만들었으면 없음). */
                if (kid < t->kid_count) {
                    ct = t->kids[kid];
                    t->kids[kid] = NULL;
                }
                kid++;
            }
            if (will_descend && hooks->should_skip_dir != NULL) {
                ScanDirInfo parent = info;
                parent.pending = w->stack.len;
                if (hooks->should_skip_dir(&entry, &parent, w->ctx)) {
                    report->excluded_dir_count++;
                    will_descend = 0;
                    walker_discard(w, ct);
                    ct = NULL;
                }
            }
            if (hooks->on_entry != NULL) {
                child_id = hooks->on_entry(&entry, t->id, will_descend, w->ctx);
            }

            if (will_descend) {
                if (ct == NULL) {
                    /* 경로가 너무 길거나 작업을 만들지 못했다. */
                    int too_long = t->path_len + 1 + rec->name_len > PATH_MAX_WIDE;
                    record_error(report, t->path,
                                 too_long ? ERROR_FILENAME_EXCED_RANGE : ERROR_NOT_ENOUGH_MEMORY);
                    report->skipped_dir_count++;
                    status = SCAN_PARTIAL;
                } else {
                    ct->id = child_id;
                    if (!stack_push(&w->stack, ct)) {
                        walker_discard(w, ct);
                        record_error(report, t->path, ERROR_NOT_ENOUGH_MEMORY);
                        report->skipped_dir_count++;
                        status = SCAN_PARTIAL;
                    }
                }
            }
        }
        if (!report->cancelled && t->enum_error != 0) {
            record_error(report, t->path, t->enum_error);
            status = SCAN_PARTIAL;
        }
    }
    /* 취소로 멈췄으면 남은 자식 작업을 버린다. */
    for (; kid < t->kid_count; kid++) {
        walker_discard(w, t->kids[kid]);
        t->kids[kid] = NULL;
    }

    if (hooks->on_dir_end != NULL) {
        info.pending = w->stack.len;
        hooks->on_dir_end(&info, status, w->ctx);
    }
    return status;
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
        pool_stop(&w->pool);
        w->pooled = 0;
    }
    for (i = 0; i < w->stack.len; i++) {
        task_free_tree(w->stack.items[i]);
    }
    free(w->stack.items);
    free(w->scratch);
    free(w->ids.slots);
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
    t = task_new(ext, wcslen(ext), NULL, 0, NULL, 0, 0, 0, 0);
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
    task_free_one(t);
    if (result == SCAN_FATAL) {
        /* 루트를 열지 못하면 하위 트리 전체를 알 수 없으므로 중단한다. */
        report->aborted = 1;
        walker_finish(&w);
        return SCAN_FATAL;
    }
    if (result != SCAN_COMPLETE) {
        status = SCAN_PARTIAL;
    }

    /*
     * 마지막에 쌓인 디렉터리부터 꺼내므로 깊이 우선으로 진행된다.
     * 하위 디렉터리 실패(SCAN_FATAL 포함)는 그 디렉터리만 건너뛰고 계속한다.
     */
    while (w.stack.len > 0) {
        if (report->cancelled ||
            (hooks->should_cancel != NULL && hooks->should_cancel(ctx))) {
            /* 남은 디렉터리는 열거하지 못한 것으로 센다. */
            report->cancelled = 1;
            report->skipped_dir_count += w.stack.len;
            status = SCAN_PARTIAL;
            break;
        }
        t = w.stack.items[--w.stack.len];
        walker_obtain(&w, t);
        result = process_dir(&w, t);
        if (result != SCAN_COMPLETE) {
            status = SCAN_PARTIAL;
        }
        task_free_one(t);
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
