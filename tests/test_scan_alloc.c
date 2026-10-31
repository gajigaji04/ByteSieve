#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path.h"
#include "scanner.h"

/*
 * 디스크 사용량 분석(논리 크기 대 할당 크기)과 병렬 미리 열거 테스트.
 *   - scan_report_account_alloc 의 원인 분류(순수 함수)
 *   - 실제 NTFS 파일: 클러스터 반올림 낭비, 압축, 스파스
 *   - 작업 스레드 수와 상관없이 훅 호출 순서·집계가 같은지(제외·취소 포함)
 */

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

#define CHECK_U64(actual, expected)                                     \
    do {                                                                \
        unsigned long long a_ = (actual), e_ = (expected);              \
        if (a_ != e_) {                                                 \
            fprintf(stderr, "%s:%d: %s: expected %llu, got %llu\n",     \
                    __FILE__, __LINE__, #actual, e_, a_);               \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static wchar_t g_root[MAX_PATH];

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

static int make_dir(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    BOOL ok = p != NULL && CreateDirectoryW(p, NULL);

    free(p);
    return ok;
}

static HANDLE create_file(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    HANDLE h = p != NULL ? CreateFileW(p, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, NULL)
                         : INVALID_HANDLE_VALUE;

    free(p);
    return h;
}

/* len 바이트의 0 을 쓴다. */
static int write_zeros(HANDLE h, DWORD len)
{
    static char zeros[64 * 1024];
    DWORD done = 0;

    while (done < len) {
        DWORD chunk = len - done < sizeof(zeros) ? len - done : (DWORD)sizeof(zeros), n = 0;
        if (!WriteFile(h, zeros, chunk, &n, NULL) || n != chunk) {
            return 0;
        }
        done += n;
    }
    return 1;
}

static int write_file(const wchar_t *name, DWORD len)
{
    HANDLE h = create_file(name);
    int ok;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = write_zeros(h, len);
    CloseHandle(h);
    return ok;
}

static void remove_tree(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = path_join(dir, L"*");
    HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;

    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            child = path_join(dir, fd.cFileName);
            if (child == NULL) {
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static int setup_root(const wchar_t *tag)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_%ls_%lu", tmp, tag,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    return CreateDirectoryW(g_root, NULL) != 0;
}

/* ---- 원인 분류(순수 함수) ---- */

static void test_account_alloc(void)
{
    ScanReport r;

    scan_report_init(&r);
    /* 클러스터 반올림: 할당 > 논리 */
    scan_report_account_alloc(&r, 10000, 12288, 1, FILE_ATTRIBUTE_ARCHIVE);
    CHECK_U64(r.alloc_bytes, 12288);
    CHECK_U64(r.slack_bytes, 2288);
    /* 같으면 아무 분류에도 넣지 않는다. */
    scan_report_account_alloc(&r, 4096, 4096, 1, 0);
    CHECK_U64(r.alloc_bytes, 16384);
    CHECK_U64(r.slack_bytes, 2288);
    /* 압축 */
    scan_report_account_alloc(&r, 100000, 8192, 1, FILE_ATTRIBUTE_COMPRESSED);
    CHECK_U64(r.compressed_files, 1);
    CHECK_U64(r.compressed_saved_bytes, 91808);
    /* 스파스 */
    scan_report_account_alloc(&r, 1 << 20, 0, 1, FILE_ATTRIBUTE_SPARSE_FILE);
    CHECK_U64(r.sparse_files, 1);
    CHECK_U64(r.sparse_saved_bytes, 1 << 20);
    /* 클라우드 전용이 압축·스파스보다 우선한다. */
    scan_report_account_alloc(&r, 5000, 0, 1,
                              FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_SPARSE_FILE);
    scan_report_account_alloc(&r, 7000, 0, 1, FILE_ATTRIBUTE_OFFLINE);
    CHECK_U64(r.cloud_files, 2);
    CHECK_U64(r.cloud_saved_bytes, 12000);
    CHECK_U64(r.sparse_files, 1);
    /* 그 밖(MFT 상주 작은 파일 등) */
    scan_report_account_alloc(&r, 300, 0, 1, FILE_ATTRIBUTE_ARCHIVE);
    CHECK_U64(r.other_saved_files, 1);
    CHECK_U64(r.other_saved_bytes, 300);
    /* 할당 크기를 모르면 논리 크기로 센다. */
    scan_report_account_alloc(&r, 999, 0, 0, 0);
    CHECK_U64(r.alloc_unknown_files, 1);
    CHECK_U64(r.alloc_bytes, 16384 + 8192 + 0 + 0 + 0 + 0 + 999);
    scan_report_free(&r);
}

/* ---- 실제 파일 ---- */

static uint64_t round_up(uint64_t v, uint64_t unit)
{
    return unit == 0 ? v : (v + unit - 1) / unit * unit;
}

static int set_compressed(const wchar_t *name, DWORD len)
{
    HANDLE h = create_file(name);
    USHORT format = COMPRESSION_FORMAT_DEFAULT;
    DWORD ret;
    int ok;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    /* 압축은 캐시를 디스크에 쓸 때 일어나므로 닫기 전에 비운다. */
    ok = DeviceIoControl(h, FSCTL_SET_COMPRESSION, &format, sizeof(format), NULL, 0, &ret,
                         NULL) &&
         write_zeros(h, len) && FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

static int set_sparse(const wchar_t *name, LONGLONG size)
{
    HANDLE h = create_file(name);
    LARGE_INTEGER pos;
    DWORD ret;
    int ok;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    pos.QuadPart = size;
    ok = DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL) &&
         SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

static void test_real_files(void)
{
    ScanReport r;
    ScanStatus st;
    int compressed, sparse;
    uint64_t cluster;

    if (!setup_root(L"alloc")) {
        fprintf(stderr, "skip: cannot create fixture\n");
        return;
    }
    /* 한 파일에 하나씩 원인을 확인하도록 폴더를 나눈다. */
    CHECK(make_dir(L"slack") && write_file(L"slack\\f.bin", 10000));
    CHECK(make_dir(L"exact") && write_file(L"exact\\f.bin", 1 << 20));
    compressed = make_dir(L"comp") && set_compressed(L"comp\\zeros.bin", 1 << 20);
    sparse = make_dir(L"sparse") && set_sparse(L"sparse\\hole.bin", 64LL << 20);

    scan_report_init(&r);
    {
        wchar_t *p = fixture(L"slack");
        st = scan_tree(p, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL);
        free(p);
    }
    CHECK(st == SCAN_COMPLETE);
    CHECK(r.cluster_bytes >= 512);
    cluster = r.cluster_bytes;
    /* 10000 바이트는 클러스터 단위로 반올림되어 할당된다. */
    CHECK_U64(r.alloc_bytes, round_up(10000, cluster));
    CHECK_U64(r.slack_bytes, round_up(10000, cluster) - 10000);
    CHECK_U64(r.alloc_unknown_files, 0);
    scan_report_free(&r);

    scan_report_init(&r);
    {
        wchar_t *p = fixture(L"exact");
        st = scan_tree(p, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL);
        free(p);
    }
    CHECK(st == SCAN_COMPLETE);
    CHECK_U64(r.alloc_bytes, 1 << 20);
    CHECK_U64(r.slack_bytes, 0);
    scan_report_free(&r);

    if (compressed) {
        scan_report_init(&r);
        {
            wchar_t *p = fixture(L"comp");
            st = scan_tree(p, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL);
            free(p);
        }
        CHECK(st == SCAN_COMPLETE);
        CHECK_U64(r.total_bytes, 1 << 20);
        /* 0 으로 채운 1 MiB 는 압축되어 훨씬 적게 차지한다. */
        CHECK_U64(r.compressed_files, 1);
        CHECK(r.alloc_bytes < (1 << 20) / 2);
        CHECK_U64(r.compressed_saved_bytes, (1 << 20) - r.alloc_bytes);
        scan_report_free(&r);
    } else {
        fprintf(stderr, "skip: NTFS compression not available\n");
    }

    if (sparse) {
        scan_report_init(&r);
        {
            wchar_t *p = fixture(L"sparse");
            st = scan_tree(p, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL);
            free(p);
        }
        CHECK(st == SCAN_COMPLETE);
        CHECK_U64(r.total_bytes, 64ULL << 20);
        CHECK_U64(r.sparse_files, 1);
        CHECK(r.alloc_bytes < (1 << 20));
        CHECK_U64(r.sparse_saved_bytes, (64ULL << 20) - r.alloc_bytes);
        scan_report_free(&r);
    } else {
        fprintf(stderr, "skip: sparse files not available\n");
    }

    /* 전체: 각 원인의 합이 맞아야 한다. */
    scan_report_init(&r);
    st = scan_tree(g_root, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL);
    CHECK(st == SCAN_COMPLETE);
    CHECK_U64(r.alloc_bytes + r.compressed_saved_bytes + r.sparse_saved_bytes +
                  r.cloud_saved_bytes + r.other_saved_bytes,
              r.total_bytes + r.slack_bytes);
    scan_report_free(&r);

    remove_tree(g_root);
}

/* ---- 병렬 미리 열거: 훅 호출 순서가 같아야 한다 ---- */

typedef struct {
    wchar_t *log;
    size_t len;
    size_t cap;
    uintptr_t next_id;
    const wchar_t *skip_name;  /* 이 이름의 폴더는 들어가지 않는다 */
    unsigned cancel_after;     /* 0 이 아니면 이만큼 폴더를 연 뒤 취소 */
    unsigned dirs_begun;
} Trace;

static void trace_add(Trace *t, const wchar_t *fmt, ...)
{
    wchar_t line[1024];
    size_t n;
    va_list ap;

    va_start(ap, fmt);
    vswprintf(line, sizeof(line) / sizeof(line[0]), fmt, ap);
    va_end(ap);
    n = wcslen(line);
    if (t->len + n + 2 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 4096;
        wchar_t *grown;
        while (cap < t->len + n + 2) {
            cap *= 2;
        }
        grown = realloc(t->log, cap * sizeof(wchar_t));
        if (grown == NULL) {
            return;
        }
        t->log = grown;
        t->cap = cap;
    }
    memcpy(t->log + t->len, line, n * sizeof(wchar_t));
    t->len += n;
    t->log[t->len++] = L'\n';
    t->log[t->len] = L'\0';
}

static uintptr_t tr_entry(const ScanEntry *e, uintptr_t parent, int will_descend, void *ctx)
{
    Trace *t = ctx;

    trace_add(t, L"E %llu %ls %llu %d", (unsigned long long)parent, e->name,
              (unsigned long long)e->size, will_descend);
    return will_descend ? ++t->next_id : 0;
}

static void tr_begin(const ScanDirInfo *d, void *ctx)
{
    Trace *t = ctx;

    t->dirs_begun++;
    trace_add(t, L"B %llu %u %ls", (unsigned long long)d->id, d->depth, d->path);
}

static void tr_end(const ScanDirInfo *d, ScanStatus result, void *ctx)
{
    trace_add(ctx, L"D %llu %d", (unsigned long long)d->id, (int)result);
}

static int tr_cancel(void *ctx)
{
    Trace *t = ctx;

    return t->cancel_after != 0 && t->dirs_begun >= t->cancel_after;
}

static int tr_skip(const ScanEntry *e, const ScanDirInfo *parent, void *ctx)
{
    Trace *t = ctx;

    (void)parent;
    return t->skip_name != NULL && _wcsicmp(e->name, t->skip_name) == 0;
}

/* 폭과 깊이가 있는 트리: d0..d5 \ s0..s5 \ t0..t3, 폴더마다 파일 두 개. */
static int build_wide_tree(void)
{
    wchar_t name[MAX_PATH];
    int a, b, c, ok = 1;

    for (a = 0; a < 6 && ok; a++) {
        swprintf(name, MAX_PATH, L"d%d", a);
        ok = make_dir(name);
        swprintf(name, MAX_PATH, L"d%d\\f.txt", a);
        ok = ok && write_file(name, (DWORD)(a + 1));
        for (b = 0; b < 6 && ok; b++) {
            swprintf(name, MAX_PATH, L"d%d\\s%d", a, b);
            ok = make_dir(name);
            for (c = 0; c < 4 && ok; c++) {
                swprintf(name, MAX_PATH, L"d%d\\s%d\\t%d", a, b, c);
                ok = make_dir(name);
                swprintf(name, MAX_PATH, L"d%d\\s%d\\t%d\\x.bin", a, b, c);
                ok = ok && write_file(name, (DWORD)(a * 100 + b * 10 + c));
                swprintf(name, MAX_PATH, L"d%d\\s%d\\t%d\\y.bin", a, b, c);
                ok = ok && write_file(name, 3);
            }
        }
    }
    return ok;
}

static void run_trace(unsigned threads, uint32_t max_depth, const wchar_t *skip, unsigned cancel,
                      Trace *t, ScanReport *r, ScanStatus *st)
{
    ScanHooks hooks = {tr_entry, tr_begin, tr_end, tr_cancel, tr_skip};

    memset(t, 0, sizeof(*t));
    t->skip_name = skip;
    t->cancel_after = cancel;
    scan_report_init(r);
    scan_set_threads(threads);
    *st = scan_tree_ex(g_root, max_depth, 0, r, &hooks, t);
    scan_set_threads(0);
}

static void compare_modes(uint32_t max_depth, const wchar_t *skip, unsigned cancel)
{
    Trace a, b;
    ScanReport ra, rb;
    ScanStatus sa, sb;
    int round;

    run_trace(1, max_depth, skip, cancel, &a, &ra, &sa);
    /* 스레드 경합은 매번 달라지므로 여러 번 돌려 본다. */
    for (round = 0; round < 5; round++) {
        run_trace(SCAN_MAX_THREADS, max_depth, skip, cancel, &b, &rb, &sb);
        CHECK(sa == sb);
        CHECK(a.log != NULL && b.log != NULL && wcscmp(a.log, b.log) == 0);
        CHECK_U64(rb.file_count, ra.file_count);
        CHECK_U64(rb.dir_count, ra.dir_count);
        CHECK_U64(rb.total_bytes, ra.total_bytes);
        CHECK_U64(rb.alloc_bytes, ra.alloc_bytes);
        CHECK_U64(rb.excluded_dir_count, ra.excluded_dir_count);
        CHECK_U64(rb.skipped_dir_count, ra.skipped_dir_count);
        CHECK(rb.cancelled == ra.cancelled);
        free(b.log);
        scan_report_free(&rb);
    }
    free(a.log);
    scan_report_free(&ra);
}

static void test_parallel_same_order(void)
{
    Trace t;
    ScanReport r;
    ScanStatus st;

    if (!setup_root(L"par") || !build_wide_tree()) {
        fprintf(stderr, "skip: cannot create fixture\n");
        CHECK(0);
        return;
    }
    CHECK(scan_effective_threads() >= 1 && scan_effective_threads() <= SCAN_MAX_THREADS);
    scan_set_threads(1);
    CHECK(scan_effective_threads() == 1);
    scan_set_threads(3);
    CHECK(scan_effective_threads() == 3);
    scan_set_threads(100);
    CHECK(scan_effective_threads() == SCAN_MAX_THREADS);
    scan_set_threads(0);

    /* 6 + 36 + 144 폴더, 6 + 288 파일 */
    run_trace(SCAN_MAX_THREADS, SCAN_DEPTH_UNLIMITED, NULL, 0, &t, &r, &st);
    CHECK(st == SCAN_COMPLETE);
    CHECK_U64(r.dir_count, 6 + 36 + 144);
    CHECK_U64(r.file_count, 6 + 288);
    free(t.log);
    scan_report_free(&r);

    compare_modes(SCAN_DEPTH_UNLIMITED, NULL, 0);
    /* 깊이 제한: 작업 스레드도 제한 밖 폴더는 만들지 않는다. */
    compare_modes(1, NULL, 0);
    /* 제외한 폴더(s3)를 미리 열거했더라도 결과에 섞이지 않는다. */
    compare_modes(SCAN_DEPTH_UNLIMITED, L"s3", 0);
    compare_modes(SCAN_DEPTH_UNLIMITED, L"d0", 0);
    /* 취소: 같은 지점에서 멈추고 남은 폴더를 같게 센다. */
    compare_modes(SCAN_DEPTH_UNLIMITED, NULL, 20);
    compare_modes(SCAN_DEPTH_UNLIMITED, L"s1", 7);

    remove_tree(g_root);
}

int main(void)
{
    test_account_alloc();
    test_real_files();
    test_parallel_same_order();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all scan_alloc tests passed\n");
    return 0;
}
