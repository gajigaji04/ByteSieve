#ifndef BYTESIEVE_SCAN_JOB_H
#define BYTESIEVE_SCAN_JOB_H

#include <stdint.h>
#include <wchar.h>

#include "scan_model.h"

/*
 * scan_model_build_ex 를 별도 스레드에서 돌리는 작업. UI 스레드는
 *   - scan_job_progress 로 최신 진행률 스냅샷을 복사해 가고
 *   - scan_job_cancel 로 멈추게 하고
 *   - scan_job_finish 로 스레드를 기다린 뒤 완성된 모델을 넘겨받는다.
 * notify 는 스캔 스레드에서 호출되므로 막히지 않아야 한다(예: PostMessage).
 */

typedef struct ScanJob ScanJob;

/* finished 가 1 이면 마지막 호출이며 이후 scan_job_finish 가 바로 반환된다. */
typedef void (*scan_job_notify_fn)(int finished, void *ctx);

/* 진행률 스냅샷에 담는 현재 폴더 경로 길이(넘으면 앞부분을 "..." 으로 줄인다). */
#define SCAN_JOB_DIR_LEN 512

typedef struct {
    ScanProgress progress; /* current_dir 는 NULL, 대신 아래 버퍼를 쓴다 */
    wchar_t current_dir[SCAN_JOB_DIR_LEN];
    uint64_t elapsed_ms;
    int cancel_requested;
} ScanJobSnapshot;

/*
 * root 스캔을 새 스레드에서 시작한다. root 는 복사한다. notify 는 NULL 일 수 있으며,
 * 진행 중에는 최대 interval_ms 마다 한 번(0 이면 폴더마다), 끝나면 반드시 한 번 호출된다.
 * 실패(메모리 부족·스레드 생성 실패) 시 NULL.
 */
ScanJob *scan_job_start(const wchar_t *root, uint32_t max_depth, uint32_t interval_ms,
                        scan_job_notify_fn notify, void *ctx);

/* 멈춰 달라고 요청한다(기다리지 않음). 어느 스레드에서 불러도 된다. */
void scan_job_cancel(ScanJob *job);

/* 최신 진행률을 out 에 복사한다. */
void scan_job_progress(ScanJob *job, ScanJobSnapshot *out);

/* 스레드가 끝났으면 1. timeout_ms 만큼 기다린다(UINT32_MAX 는 무한). */
int scan_job_wait(ScanJob *job, uint32_t timeout_ms);

/*
 * 스레드가 끝날 때까지 기다린 뒤 결과 모델을 out 으로 옮기고(out 의 이전 내용은 해제)
 * job 을 해제한다. out 이 NULL 이면 결과를 버린다. 반환값은 스캔 상태.
 */
ScanStatus scan_job_finish(ScanJob *job, ScanModel *out);

#endif /* BYTESIEVE_SCAN_JOB_H */
