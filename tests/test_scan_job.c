#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path.h"
#include "scan_job.h"

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

static int make_dir(const wchar_t *name)
{
    wchar_t *p = path_join(g_root, name);
    BOOL ok = p != NULL && CreateDirectoryW(p, NULL);
    free(p);
    return ok;
}

static int write_file(const wchar_t *name, DWORD len)
{
    wchar_t *p = path_join(g_root, name);
    HANDLE h = p ? CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    char buf[256];
    DWORD written = 0;
    BOOL ok;

    free(p);
    if (h == INVALID_HANDLE_VALUE || len > sizeof(buf)) {
        return 0;
    }
    memset(buf, 'x', len);
    ok = len == 0 || WriteFile(h, buf, len, &written, NULL);
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

/*
 * root\
 *   r.bin        7 B
 *   d0..d4\x.bin 10 B 씩
 */
static int setup_fixture(void)
{
    wchar_t tmp[MAX_PATH];
    int ok;
    unsigned i;

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_job_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    ok = CreateDirectoryW(g_root, NULL) && write_file(L"r.bin", 7);
    for (i = 0; ok && i < 5; i++) {
        wchar_t dir[16], file[32];
        swprintf(dir, 16, L"d%u", i);
        swprintf(file, 32, L"d%u\\x.bin", i);
        ok = make_dir(dir) && write_file(file, 10);
    }
    return ok;
}

typedef struct {
    volatile LONG progress_calls;
    volatile LONG finished_calls;
    HANDLE first_progress; /* 첫 진행률 알림에서 신호 */
    HANDLE resume;         /* NULL 이 아니면 첫 알림에서 이 신호를 기다린다 */
} NotifyCtx;

static void on_notify(int finished, void *ctx)
{
    NotifyCtx *n = ctx;

    if (finished) {
        InterlockedIncrement(&n->finished_calls);
        return;
    }
    if (InterlockedIncrement(&n->progress_calls) == 1) {
        SetEvent(n->first_progress);
        if (n->resume != NULL) {
            WaitForSingleObject(n->resume, INFINITE);
        }
    }
}

static void test_complete(void)
{
    NotifyCtx n = {0, 0, CreateEventW(NULL, TRUE, FALSE, NULL), NULL};
    ScanJobSnapshot snap;
    ScanModel m;
    ScanJob *job;

    scan_model_init(&m);
    job = scan_job_start(g_root, SCAN_DEPTH_UNLIMITED, 0, on_notify, &n);
    CHECK(job != NULL);
    if (job == NULL) {
        return;
    }
    CHECK(scan_job_wait(job, 10000));
    scan_job_progress(job, &snap);
    CHECK(snap.progress.finished == 1);
    CHECK(snap.progress.status == SCAN_COMPLETE);
    CHECK_U64(snap.progress.files, 6);
    CHECK_U64(snap.progress.bytes, 57);
    CHECK(snap.cancel_requested == 0);

    CHECK(scan_job_finish(job, &m) == SCAN_COMPLETE);
    CHECK_U64(n.finished_calls, 1);
    CHECK(n.progress_calls >= 1); /* interval 0: 폴더마다 */
    CHECK_U64(m.len, 6);
    CHECK_U64(m.nodes[0].total_bytes, 57);
    CHECK(m.report.cancelled == 0);
    scan_model_free(&m);
    CloseHandle(n.first_progress);
}

/* 첫 진행률 알림(루트 열거 직전)에서 스캔 스레드를 붙잡아 두고 취소한다. */
static void test_cancel(void)
{
    NotifyCtx n = {0, 0, CreateEventW(NULL, TRUE, FALSE, NULL),
                   CreateEventW(NULL, TRUE, FALSE, NULL)};
    ScanJobSnapshot snap;
    ScanModel m;
    ScanJob *job;
    size_t i;

    scan_model_init(&m);
    job = scan_job_start(g_root, SCAN_DEPTH_UNLIMITED, 0, on_notify, &n);
    CHECK(job != NULL);
    if (job == NULL) {
        return;
    }
    CHECK(WaitForSingleObject(n.first_progress, 10000) == WAIT_OBJECT_0);
    scan_job_progress(job, &snap);
    CHECK(snap.progress.finished == 0);
    CHECK(wcscmp(snap.current_dir, g_root) == 0);
    CHECK(!scan_job_wait(job, 0));

    scan_job_cancel(job);
    scan_job_progress(job, &snap);
    CHECK(snap.cancel_requested == 1);
    SetEvent(n.resume);

    CHECK(scan_job_finish(job, &m) == SCAN_PARTIAL);
    CHECK_U64(n.finished_calls, 1);
    CHECK(m.report.cancelled == 1);
    CHECK_U64(m.report.skipped_dir_count, 5);
    CHECK_U64(m.nodes[0].total_bytes, 7);
    CHECK_U64(m.nodes[0].child_count, 5);
    for (i = 1; i < m.len; i++) {
        CHECK(m.nodes[i].flags == SCAN_NODE_CANCELLED);
    }
    scan_model_free(&m);
    CloseHandle(n.first_progress);
    CloseHandle(n.resume);
}

/* 없는 경로도 스레드에서 SCAN_FATAL 로 끝나고 알림은 한 번 온다. */
static void test_missing(void)
{
    NotifyCtx n = {0, 0, CreateEventW(NULL, TRUE, FALSE, NULL), NULL};
    wchar_t *missing = path_join(g_root, L"no-such-dir");
    ScanModel m;
    ScanJob *job;

    scan_model_init(&m);
    job = scan_job_start(missing, SCAN_DEPTH_UNLIMITED, 100, on_notify, &n);
    CHECK(job != NULL);
    if (job != NULL) {
        CHECK(scan_job_finish(job, &m) == SCAN_FATAL);
        CHECK(m.report.aborted == 1);
        CHECK_U64(n.finished_calls, 1);
    }
    scan_model_free(&m);
    free(missing);
    CloseHandle(n.first_progress);
}

/* 결과를 버리는 finish(out = NULL)와 이미 끝난 작업의 cancel 은 안전해야 한다. */
static void test_discard(void)
{
    ScanJob *job = scan_job_start(g_root, SCAN_DEPTH_UNLIMITED, 100, NULL, NULL);

    CHECK(job != NULL);
    if (job != NULL) {
        CHECK(scan_job_wait(job, 10000));
        scan_job_cancel(job);
        CHECK(scan_job_finish(job, NULL) == SCAN_COMPLETE);
    }
}

int main(void)
{
    if (!setup_fixture()) {
        fprintf(stderr, "fixture setup failed (error %lu)\n", GetLastError());
        remove_tree(g_root);
        return 1;
    }
    test_complete();
    test_cancel();
    test_missing();
    test_discard();
    remove_tree(g_root);

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all scan job tests passed\n");
    return 0;
}
