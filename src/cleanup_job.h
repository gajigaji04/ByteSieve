#ifndef BYTESIEVE_CLEANUP_JOB_H
#define BYTESIEVE_CLEANUP_JOB_H

#include <stdint.h>
#include <wchar.h>

#include "cleanup_scan.h"

/*
 * cleanup_scan 을 별도 스레드에서 돌리는 작업(scan_job 과 같은 방식). UI 스레드는
 *   - cleanup_job_progress 로 최신 진행률 스냅샷을 복사해 가고
 *   - cleanup_job_cancel 로 멈추게 하고
 *   - cleanup_job_finish 로 스레드를 기다린 뒤 결과를 넘겨받는다.
 * 보호 경로는 시작할 때 현재 시스템 기준(cleanup_protect_add_system)으로 만든다.
 * notify 는 작업 스레드에서 호출되므로 막히지 않아야 한다(예: PostMessage).
 */

typedef struct CleanupJob CleanupJob;

/* finished 가 1 이면 마지막 호출이며 이후 cleanup_job_finish 가 바로 반환된다. */
typedef void (*cleanup_job_notify_fn)(int finished, void *ctx);

/* 스냅샷에 담는 현재 폴더 경로 길이(넘으면 앞부분을 "..." 으로 줄인다). */
#define CLEANUP_JOB_DIR_LEN 512

typedef struct {
    CleanupProgress progress; /* current_dir 는 NULL, 대신 아래 버퍼를 쓴다 */
    wchar_t current_dir[CLEANUP_JOB_DIR_LEN];
    uint64_t elapsed_ms;
    int cancel_requested;
} CleanupJobSnapshot;

/*
 * root 탐지를 새 스레드에서 시작한다. root 는 복사한다. notify 는 NULL 일 수 있으며,
 * 진행 중에는 최대 interval_ms 마다 한 번, 끝나면 반드시 한 번 호출된다.
 * 실패(메모리 부족·스레드 생성 실패) 시 NULL.
 */
CleanupJob *cleanup_job_start(const wchar_t *root, uint32_t interval_ms,
                              cleanup_job_notify_fn notify, void *ctx);

/* 멈춰 달라고 요청한다(기다리지 않음). 어느 스레드에서 불러도 된다. */
void cleanup_job_cancel(CleanupJob *job);

/* 최신 진행률을 out 에 복사한다. */
void cleanup_job_progress(CleanupJob *job, CleanupJobSnapshot *out);

/* 스레드가 끝났으면 1. timeout_ms 만큼 기다린다(UINT32_MAX 는 무한). */
int cleanup_job_wait(CleanupJob *job, uint32_t timeout_ms);

/*
 * 스레드가 끝날 때까지 기다린 뒤 결과를 out 으로 옮기고(out 의 이전 내용은 해제)
 * job 을 해제한다. out 이 NULL 이면 결과를 버린다. 반환값은 탐색 상태.
 */
ScanStatus cleanup_job_finish(CleanupJob *job, CleanupResult *out);

#endif /* BYTESIEVE_CLEANUP_JOB_H */
