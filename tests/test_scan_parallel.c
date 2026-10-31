#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path.h"
#include "scanner.h"

/*
 * 병렬 미리 열거의 메모리 상한·분배·정확도·취소 검사.
 *   - 넓은 폴더(하위 폴더 WIDE 개)를 스캔해도 동시에 살아 있는 폴더 작업 수가 상한 안에 머무는지
 *     (예전 방식은 하위 폴더마다 작업을 미리 만들어 WIDE 개를 넘었다)
 *   - 작업 스레드 1개와 8개의 훅 호출이 같은지, 집계가 정확한지
 *   - 미리 열거·직접 열거·버린 작업 수가 처리한 폴더 수와 맞는지(빠뜨리거나 두 번 열거하지 않음)
 *   - 제외 훅·취소가 미리 열거한 작업을 정리하는지
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

/* 넓은 폴더의 하위 폴더 수. 미리 열거 상한(1024)보다 충분히 커야 예전 방식과 구분된다. */
#define WIDE 2500
/* 깊은 폴더 사슬의 깊이. */
#define DEEP 12
/*
 * 동시에 살아 있는 작업 수 상한: 미리 열거 상한 + 열거 중인 작업(스레드 수) + 스택 칸(깊이) + 여유.
 * 예전 방식이면 WIDE 개를 넘는다.
 */
#define TASK_BOUND_POOLED (1024 + SCAN_MAX_THREADS + DEEP + 16)
#define TASK_BOUND_SINGLE (DEEP + 8)

static wchar_t g_root[MAX_PATH];

/*
 * root\
 *   wide\d0000 .. d2499\f.bin(10)   하위 폴더 2500개
 *   deep\l1\l2\...\l12\f.bin(10)    깊이 12 사슬
 *   files\a.bin(100), b.bin(200)
 */
#define PATH_FILES (WIDE + 1 + 2)
#define PATH_BYTES ((unsigned long long)WIDE * 10 + 10 + 300)
#define PATH_DIRS (3 + WIDE + DEEP)

static int make_dir(const wchar_t *path)
{
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int write_file(const wchar_t *path, DWORD len)
{
    static char data[512];
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = WriteFile(h, data, len, &written, NULL);
    CloseHandle(h);
    return ok && written == len;
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
                DeleteFileW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static int setup(void)
{
    wchar_t tmp[MAX_PATH], p[MAX_PATH];
    int i;

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_parallel_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    if (!make_dir(g_root)) {
        return 0;
    }
    swprintf(p, MAX_PATH, L"%ls\\wide", g_root);
    if (!make_dir(p)) {
        return 0;
    }
    for (i = 0; i < WIDE; i++) {
        swprintf(p, MAX_PATH, L"%ls\\wide\\d%04d", g_root, i);
        if (!make_dir(p)) {
            return 0;
        }
        swprintf(p, MAX_PATH, L"%ls\\wide\\d%04d\\f.bin", g_root, i);
        if (!write_file(p, 10)) {
            return 0;
        }
    }
    swprintf(p, MAX_PATH, L"%ls\\deep", g_root);
    if (!make_dir(p)) {
        return 0;
    }
    for (i = 1; i <= DEEP; i++) {
        size_t len = wcslen(p);
        swprintf(p + len, MAX_PATH - len, L"\\l%d", i);
        if (!make_dir(p)) {
            return 0;
        }
    }
    wcscat_s(p, MAX_PATH, L"\\f.bin");
    if (!write_file(p, 10)) {
        return 0;
    }
    swprintf(p, MAX_PATH, L"%ls\\files", g_root);
    if (!make_dir(p)) {
        return 0;
    }
    swprintf(p, MAX_PATH, L"%ls\\files\\a.bin", g_root);
    if (!write_file(p, 100)) {
        return 0;
    }
    swprintf(p, MAX_PATH, L"%ls\\files\\b.bin", g_root);
    return write_file(p, 200);
}

/* 훅 호출 기록: 순서까지 비교하도록 이름을 이어 해시한다. */
typedef struct {
    unsigned long long hash;
    unsigned long long entries;
    unsigned long long dirs_begun;
    unsigned long long dirs_ended;
    unsigned long long cancel_after; /* 0 이 아니면 이만큼 폴더를 시작한 뒤 취소 */
    int exclude_wide;
    int begun_after_cancel;
    int cancelled;
} Trace;

static void mix(Trace *t, const wchar_t *s)
{
    for (; *s != L'\0'; s++) {
        t->hash = (t->hash ^ (unsigned)*s) * 1099511628211ULL;
    }
    t->hash = (t->hash ^ 0xFFu) * 1099511628211ULL;
}

static uintptr_t on_entry(const ScanEntry *e, uintptr_t parent, int will_descend, void *ctx)
{
    Trace *t = ctx;

    (void)parent;
    t->entries++;
    mix(t, e->name);
    return will_descend ? 1 : 0;
}

static void on_begin(const ScanDirInfo *d, void *ctx)
{
    Trace *t = ctx;

    t->dirs_begun++;
    if (t->cancelled) {
        t->begun_after_cancel = 1;
    }
    mix(t, d->path);
}

static void on_end(const ScanDirInfo *d, ScanStatus st, void *ctx)
{
    Trace *t = ctx;

    (void)d;
    (void)st;
    t->dirs_ended++;
}

static int should_cancel(void *ctx)
{
    Trace *t = ctx;

    if (t->cancel_after != 0 && t->dirs_begun >= t->cancel_after) {
        t->cancelled = 1;
    }
    return t->cancelled;
}

static int skip_dir(const ScanEntry *e, const ScanDirInfo *parent, void *ctx)
{
    Trace *t = ctx;

    (void)parent;
    return t->exclude_wide && wcscmp(e->name, L"wide") == 0;
}

static ScanStatus run(unsigned threads, Trace *trace, ScanReport *r)
{
    ScanHooks hooks;
    ScanStatus st;

    memset(&hooks, 0, sizeof(hooks));
    hooks.on_entry = on_entry;
    hooks.on_dir_begin = on_begin;
    hooks.on_dir_end = on_end;
    hooks.should_cancel = should_cancel;
    hooks.should_skip_dir = skip_dir;
    scan_report_init(r);
    scan_set_threads(threads);
    st = scan_tree_ex(g_root, SCAN_DEPTH_UNLIMITED, 1, r, &hooks, trace);
    scan_set_threads(0);
    return st;
}

/* 열거한 폴더(직접 + 미리 - 버림)가 처리한 폴더 수와 같다: 빠뜨리거나 두 번 쓴 열거가 없다. */
static void check_accounting(const ScanReport *r, const Trace *t)
{
    CHECK_U64(r->self_fetched_dirs + r->prefetched_dirs - r->discarded_dirs, t->dirs_begun);
    CHECK(t->dirs_begun == t->dirs_ended);
    CHECK(r->peak_task_bytes > 0);
}

static void test_bounded_and_same(void)
{
    Trace t1, t8;
    ScanReport r1, r8;

    memset(&t1, 0, sizeof(t1));
    memset(&t8, 0, sizeof(t8));
    CHECK(run(1, &t1, &r1) == SCAN_COMPLETE);
    CHECK(run(8, &t8, &r8) == SCAN_COMPLETE);

    /* 정확도: 집계와 훅 호출(순서 포함)이 같다 */
    CHECK_U64(r1.file_count, PATH_FILES);
    CHECK_U64(r1.total_bytes, PATH_BYTES);
    CHECK_U64(r1.dir_count, PATH_DIRS);
    CHECK_U64(r8.file_count, r1.file_count);
    CHECK_U64(r8.total_bytes, r1.total_bytes);
    CHECK_U64(r8.dir_count, r1.dir_count);
    CHECK(t1.hash == t8.hash && t1.entries == t8.entries && t1.dirs_begun == t8.dirs_begun);
    CHECK_U64(t1.dirs_begun, PATH_DIRS + 1);

    /* 메모리: 하위 폴더 2500개를 기다리는 동안에도 작업을 미리 만들지 않는다 */
    CHECK(r1.peak_tasks <= TASK_BOUND_SINGLE);
    CHECK(r8.peak_tasks <= TASK_BOUND_POOLED);
    CHECK(r1.peak_tasks < WIDE && r8.peak_tasks < WIDE);
    printf("peak tasks: 1 thread %llu (%llu bytes), %u threads %llu (%llu bytes)\n",
           (unsigned long long)r1.peak_tasks, (unsigned long long)r1.peak_task_bytes,
           scan_effective_threads(), (unsigned long long)r8.peak_tasks,
           (unsigned long long)r8.peak_task_bytes);

    /* 분배: 1개면 모두 직접, 여러 개면 미리 열거가 일을 나눠 맡는다 */
    CHECK_U64(r1.prefetched_dirs, 0);
    CHECK_U64(r1.discarded_dirs, 0);
    check_accounting(&r1, &t1);
    check_accounting(&r8, &t8);
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        if (si.dwNumberOfProcessors > 1) {
            CHECK(r8.prefetched_dirs > 0);
            CHECK(r8.self_fetched_dirs < r8.prefetched_dirs);
        }
    }
    scan_report_free(&r1);
    scan_report_free(&r8);
}

static void test_exclude(void)
{
    Trace t;
    ScanReport r;

    memset(&t, 0, sizeof(t));
    t.exclude_wide = 1;
    CHECK(run(8, &t, &r) == SCAN_COMPLETE);
    /* wide 아래는 세지 않고, 미리 열거했던 것은 버린 것으로 센다 */
    CHECK_U64(r.excluded_dir_count, 1);
    CHECK_U64(r.file_count, PATH_FILES - WIDE);
    CHECK_U64(t.dirs_begun, 1 + 2 + DEEP);
    check_accounting(&r, &t);
    scan_report_free(&r);
}

static void test_cancel(void)
{
    unsigned threads[2] = {1, 8};
    int k;

    for (k = 0; k < 2; k++) {
        Trace t;
        ScanReport r;
        memset(&t, 0, sizeof(t));
        t.cancel_after = 300;
        CHECK(run(threads[k], &t, &r) == SCAN_PARTIAL);
        CHECK(r.cancelled);
        /* 취소 뒤에는 폴더를 새로 시작하지 않는다 */
        CHECK(!t.begun_after_cancel);
        CHECK(t.dirs_begun >= 300 && t.dirs_begun < PATH_DIRS);
        CHECK(r.skipped_dir_count > 0);
        CHECK(r.file_count < PATH_FILES);
        /* 미리 열거했지만 처리하지 않은 것은 버린 것으로 센다 */
        check_accounting(&r, &t);
        scan_report_free(&r);
    }
}

int main(void)
{
    if (!setup()) {
        fprintf(stderr, "fixture setup failed\n");
        remove_tree(g_root);
        return 1;
    }
    test_bounded_and_same();
    test_exclude();
    test_cancel();
    remove_tree(g_root);

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all scan_parallel tests passed\n");
    return 0;
}
