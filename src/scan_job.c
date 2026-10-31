#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "scan_job.h"

struct ScanJob {
    HANDLE thread;
    wchar_t *root;
    uint32_t max_depth;
    unsigned options; /* SCAN_OPT_* */
    uint32_t interval_ms;
    scan_job_notify_fn notify;
    void *ctx;

    volatile LONG cancel; /* 0 이 아니면 취소 요청 */
    ULONGLONG started;
    ULONGLONG last_notify;

    CRITICAL_SECTION lock; /* snapshot 보호 */
    ScanJobSnapshot snapshot;

    ScanModel model; /* 스레드가 끝난 뒤에만 읽는다 */
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
    if (len < SCAN_JOB_DIR_LEN) {
        memcpy(dst, src, (len + 1) * sizeof(*src));
        return;
    }
    keep = SCAN_JOB_DIR_LEN - 4;
    memcpy(dst, L"...", 3 * sizeof(*dst));
    memcpy(dst + 3, src + len - keep, (keep + 1) * sizeof(*src));
}

static void on_progress(const ScanProgress *p, void *ctx)
{
    ScanJob *job = ctx;
    ULONGLONG now = GetTickCount64();
    int due;

    EnterCriticalSection(&job->lock);
    job->snapshot.progress = *p;
    job->snapshot.progress.current_dir = NULL;
    copy_dir(job->snapshot.current_dir, p->current_dir);
    job->snapshot.elapsed_ms = now - job->started;
    LeaveCriticalSection(&job->lock);

    /* 마지막 알림은 스레드 함수가 모델을 마무리한 뒤 보낸다. */
    due = !p->finished && now - job->last_notify >= job->interval_ms;
    if (due && job->notify != NULL) {
        job->last_notify = now;
        job->notify(0, job->ctx);
    }
}

static int on_cancel(void *ctx)
{
    ScanJob *job = ctx;

    return InterlockedCompareExchange(&job->cancel, 0, 0) != 0;
}

static unsigned __stdcall job_thread(void *arg)
{
    ScanJob *job = arg;

    scan_model_build_opts(&job->model, job->root, job->max_depth, job->options, on_progress,
                          on_cancel, job);
    if (job->notify != NULL) {
        job->notify(1, job->ctx);
    }
    return 0;
}

static void job_free(ScanJob *job)
{
    DeleteCriticalSection(&job->lock);
    scan_model_free(&job->model);
    free(job->root);
    free(job);
}

ScanJob *scan_job_start(const wchar_t *root, uint32_t max_depth, uint32_t interval_ms,
                        scan_job_notify_fn notify, void *ctx)
{
    return scan_job_start_ex(root, max_depth, 0, interval_ms, notify, ctx);
}

ScanJob *scan_job_start_ex(const wchar_t *root, uint32_t max_depth, unsigned options,
                           uint32_t interval_ms, scan_job_notify_fn notify, void *ctx)
{
    ScanJob *job = calloc(1, sizeof(*job));

    if (job == NULL) {
        return NULL;
    }
    InitializeCriticalSection(&job->lock);
    scan_model_init(&job->model);
    job->root = path_dup(root != NULL ? root : L"");
    job->max_depth = max_depth;
    job->options = options;
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

void scan_job_cancel(ScanJob *job)
{
    if (job != NULL) {
        InterlockedExchange(&job->cancel, 1);
    }
}

void scan_job_progress(ScanJob *job, ScanJobSnapshot *out)
{
    EnterCriticalSection(&job->lock);
    *out = job->snapshot;
    LeaveCriticalSection(&job->lock);
    out->cancel_requested = on_cancel(job);
}

int scan_job_wait(ScanJob *job, uint32_t timeout_ms)
{
    DWORD wait = timeout_ms == UINT32_MAX ? INFINITE : (DWORD)timeout_ms;

    return WaitForSingleObject(job->thread, wait) == WAIT_OBJECT_0;
}

ScanStatus scan_job_finish(ScanJob *job, ScanModel *out)
{
    ScanStatus status;

    if (job == NULL) {
        return SCAN_FATAL;
    }
    WaitForSingleObject(job->thread, INFINITE);
    CloseHandle(job->thread);

    status = job->model.status;
    if (out != NULL) {
        scan_model_free(out);
        *out = job->model;
        scan_model_init(&job->model);
    }
    job_free(job);
    return status;
}
