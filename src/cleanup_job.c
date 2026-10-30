#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <stdlib.h>
#include <string.h>

#include "cleanup_job.h"
#include "path.h"

struct CleanupJob {
    HANDLE thread;
    wchar_t *root;
    uint32_t interval_ms;
    cleanup_job_notify_fn notify;
    void *ctx;
    CleanupProtect protect;

    volatile LONG cancel; /* 0 이 아니면 취소 요청 */
    ULONGLONG started;
    ULONGLONG last_notify;

    CRITICAL_SECTION lock; /* snapshot 보호 */
    CleanupJobSnapshot snapshot;

    CleanupResult result; /* 스레드가 끝난 뒤에만 읽는다 */
};

/* 긴 경로는 끝부분(현재 폴더 이름 쪽)을 남기고 앞을 "..." 으로 줄인다. */
static void copy_dir(wchar_t *dst, const wchar_t *src)
{
    size_t len, keep;

    if (src == NULL) {
        dst[0] = L'\0';
        return;
    }
    len = wcslen(src);
    if (len < CLEANUP_JOB_DIR_LEN) {
        memcpy(dst, src, (len + 1) * sizeof(*src));
        return;
    }
    keep = CLEANUP_JOB_DIR_LEN - 4;
    memcpy(dst, L"...", 3 * sizeof(*dst));
    memcpy(dst + 3, src + len - keep, (keep + 1) * sizeof(*src));
}

static void on_progress(const CleanupProgress *p, void *ctx)
{
    CleanupJob *job = ctx;
    ULONGLONG now = GetTickCount64();
    int due;

    EnterCriticalSection(&job->lock);
    job->snapshot.progress = *p;
    job->snapshot.progress.current_dir = NULL;
    copy_dir(job->snapshot.current_dir, p->current_dir);
    job->snapshot.elapsed_ms = now - job->started;
    LeaveCriticalSection(&job->lock);

    /* 마지막 알림은 스레드 함수가 결과를 마무리한 뒤 보낸다. */
    due = !p->finished && now - job->last_notify >= job->interval_ms;
    if (due && job->notify != NULL) {
        job->last_notify = now;
        job->notify(0, job->ctx);
    }
}

static int on_cancel(void *ctx)
{
    CleanupJob *job = ctx;

    return InterlockedCompareExchange(&job->cancel, 0, 0) != 0;
}

static unsigned __stdcall job_thread(void *arg)
{
    CleanupJob *job = arg;

    cleanup_scan(&job->result, job->root, &job->protect, on_progress, on_cancel, job);
    if (job->notify != NULL) {
        job->notify(1, job->ctx);
    }
    return 0;
}

static void job_free(CleanupJob *job)
{
    DeleteCriticalSection(&job->lock);
    cleanup_result_free(&job->result);
    cleanup_protect_free(&job->protect);
    free(job->root);
    free(job);
}

CleanupJob *cleanup_job_start(const wchar_t *root, uint32_t interval_ms,
                              cleanup_job_notify_fn notify, void *ctx)
{
    CleanupJob *job = calloc(1, sizeof(*job));

    if (job == NULL) {
        return NULL;
    }
    InitializeCriticalSection(&job->lock);
    cleanup_result_init(&job->result);
    cleanup_protect_init(&job->protect);
    cleanup_protect_add_system(&job->protect);
    job->root = path_dup(root != NULL ? root : L"");
    job->interval_ms = interval_ms;
    job->notify = notify;
    job->ctx = ctx;
    job->started = GetTickCount64();
    job->last_notify = job->started;
    if (job->root == NULL) {
        job_free(job);
        return NULL;
    }

    job->thread = (HANDLE)_beginthreadex(NULL, 0, job_thread, job, 0, NULL);
    if (job->thread == NULL) {
        job_free(job);
        return NULL;
    }
    return job;
}

void cleanup_job_cancel(CleanupJob *job)
{
    if (job != NULL) {
        InterlockedExchange(&job->cancel, 1);
    }
}

void cleanup_job_progress(CleanupJob *job, CleanupJobSnapshot *out)
{
    EnterCriticalSection(&job->lock);
    *out = job->snapshot;
    LeaveCriticalSection(&job->lock);
    out->cancel_requested = on_cancel(job);
}

int cleanup_job_wait(CleanupJob *job, uint32_t timeout_ms)
{
    DWORD wait = timeout_ms == UINT32_MAX ? INFINITE : (DWORD)timeout_ms;

    return WaitForSingleObject(job->thread, wait) == WAIT_OBJECT_0;
}

ScanStatus cleanup_job_finish(CleanupJob *job, CleanupResult *out)
{
    ScanStatus status;

    if (job == NULL) {
        return SCAN_FATAL;
    }
    WaitForSingleObject(job->thread, INFINITE);
    CloseHandle(job->thread);

    status = job->result.status;
    if (out != NULL) {
        cleanup_result_free(out);
        *out = job->result;
        cleanup_result_init(&job->result);
    }
    job_free(job);
    return status;
}
