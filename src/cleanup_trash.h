#ifndef BYTESIEVE_CLEANUP_TRASH_H
#define BYTESIEVE_CLEANUP_TRASH_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "cleanup_rules.h"
#include "cleanup_scan.h"

/*
 * 정리 후보를 Windows 휴지통으로 옮긴다. 영구 삭제는 하지 않는다.
 * 휴지통을 쓸 수 없으면 같은 드라이브의 격리 폴더(quarantine.h)로 옮긴다:
 *   - 이동식 드라이브(USB 등, 휴지통 없음)는 처음부터 격리 폴더로
 *   - 고정 디스크에서 셸이 영구 삭제하려 하면(휴지통 용량 초과·휴지통 없는 볼륨) 거부한 뒤,
 *     또는 셸이 아무것도 건드리지 못하고 실패하면 항목을 다시 확인하고 격리 폴더로
 *   - 휴지통이 일부를 옮긴 뒤 실패했으면 둘로 나뉘지 않게 격리하지 않고 실패로 남긴다
 *
 * 안전 장치(앞에서부터 차례로 확인하며 하나라도 걸리면 그 항목은 건드리지 않는다):
 *   1. 경로 판정(cleanup_guard_check, 파일 시스템 접근 없음)
 *      - 로컬 드라이브의 절대 경로("X:\...")만, MAX_PATH 미만만(휴지통이 긴 경로를 지원하지 않음)
 *      - 탐지 경로 안쪽(탐지 경로 자체는 아님)
 *      - 시스템 보호 경로(Windows·Program Files·ProgramData·볼륨 루트 시스템 폴더)와 같거나 그 아래가 아님
 *      - 중요 경로(볼륨 루트, 사용자 프로필, 바탕 화면·문서 등 알려진 폴더, Users 폴더,
 *        실행 중인 프로그램 폴더, 보호 경로)와 같거나 그 상위가 아님
 *      - 하위를 다 읽지 못한 항목(CLEANUP_ITEM_INCOMPLETE)·보호 경로 안 탐지 결과가 아님
 *   2. 옮기기 직전 확인(cleanup_trash_one)
 *      - 고정 디스크(휴지통) 또는 이동식 드라이브(격리 폴더)일 것
 *      - 아직 있고, 탐지 때와 종류(폴더/파일)가 같고, 링크(재분석 지점)가 아닐 것
 *      - 실제 위치(GetFinalPathNameByHandleW)가 표시 경로와 같을 것(중간 폴더가 정션으로
 *        바뀌어 다른 곳을 가리키면 거부) — 실제 위치도 1 의 판정을 다시 통과해야 한다
 *   3. 옮기는 중(IFileOperation, FOFX_RECYCLEONDELETE)
 *      - 휴지통 용량 초과 등으로 셸이 영구 삭제하려 하면 진행 싱크에서 거부한다
 *      - 휴지통에 들어간 위치를 받지 못하거나 원래 경로가 남아 있으면 실패로 본다
 *   4. 격리 폴더로 옮길 때(quarantine_move)
 *      - 같은 볼륨 안의 이름 바꾸기만 하고(복사·덮어쓰기 없음) 기록을 먼저 남긴다
 *      - 격리 폴더 안의 항목은 1 에서 보호 경로로 거부한다
 */

typedef enum {
    CLEANUP_TRASH_PENDING = 0,  /* 아직 처리하지 않음 */
    CLEANUP_TRASH_MOVED,        /* 휴지통으로 옮김 */
    CLEANUP_TRASH_QUARANTINED,  /* 휴지통 대신 같은 드라이브의 격리 폴더로 옮김 */
    CLEANUP_TRASH_DENY_PATH,    /* 휴지통이 다룰 수 없는 경로(상대·네트워크·너무 김) */
    CLEANUP_TRASH_DENY_OUTSIDE, /* 탐지 경로 밖이거나 탐지 경로 자체 */
    CLEANUP_TRASH_DENY_PROTECTED, /* 시스템 보호 경로이거나 그 안(격리 폴더 포함) */
    CLEANUP_TRASH_DENY_CRITICAL,  /* 중요 경로이거나 그 상위 */
    CLEANUP_TRASH_DENY_INCOMPLETE, /* 하위를 다 읽지 못한 항목 */
    CLEANUP_TRASH_DENY_DRIVE,   /* 고정·이동식 드라이브가 아님(네트워크·CD 등) */
    CLEANUP_TRASH_MISSING,      /* 이미 없음 */
    CLEANUP_TRASH_CHANGED,      /* 종류가 바뀌었거나 링크이거나 실제 위치가 다름 */
    CLEANUP_TRASH_NO_RECYCLE,   /* 휴지통에 넣을 수 없어 거부(영구 삭제 방지) */
    CLEANUP_TRASH_FAILED,       /* 옮기지 못함(사용 중·접근 거부 등, code 참고) */
    CLEANUP_TRASH_QUARANTINE_FAILED, /* 격리 폴더로도 옮기지 못함(code 참고, 항목은 그대로) */
    CLEANUP_TRASH_CANCELLED,    /* 취소로 처리하지 않음 */
    CLEANUP_TRASH_STATUS_COUNT
} CleanupTrashStatus;

/* 화면용 짧은 설명("휴지통으로 이동됨", "시스템 보호 경로" 등). */
const wchar_t *cleanup_trash_status_text(CleanupTrashStatus status);

/* 식별자 이름("moved", "deny_protected" 등). */
const wchar_t *cleanup_trash_status_name(CleanupTrashStatus status);

/* 옮기기 전에 거부된 상태인지(DENY_*, MISSING, CHANGED). */
int cleanup_trash_is_denied(CleanupTrashStatus status);

/* 삭제 보호 목록. protect 는 같거나 그 아래, critical 은 같거나 그 상위를 거부한다. */
typedef struct {
    CleanupProtect protect;
    CleanupProtect critical;
} CleanupGuard;

void cleanup_guard_init(CleanupGuard *g);
void cleanup_guard_free(CleanupGuard *g);

/*
 * 현재 시스템 기준으로 채운다: protect 에 cleanup_protect_add_system 의 경로,
 * critical 에 사용자 프로필·그 부모(Users)·공용 프로필, 알려진 폴더(바탕 화면, 문서,
 * 다운로드, 사진, 음악, 동영상, AppData Roaming/Local/LocalLow), 실행 파일 폴더.
 * 추가한 개수를 반환한다.
 */
size_t cleanup_guard_add_system(CleanupGuard *g);

/*
 * root(탐지 경로) 아래 항목을 휴지통으로 옮겨도 되는지 경로만 보고 판정한다.
 * 허용이면 CLEANUP_TRASH_PENDING, 아니면 DENY_* 를 반환한다.
 */
CleanupTrashStatus cleanup_guard_check(const CleanupGuard *g, const wchar_t *root,
                                       const CleanupItem *item);

/* 항목 하나의 처리 결과. */
typedef struct {
    CleanupTrashStatus status;
    uint32_t code;          /* FAILED·NO_RECYCLE·QUARANTINE_FAILED 일 때 HRESULT 또는 Windows 오류 코드 */
    wchar_t *recycled_path; /* MOVED·QUARANTINED 일 때 휴지통·격리 폴더 안의 실제 위치(모르면 NULL) */
} CleanupTrashOutcome;

void cleanup_trash_outcome_free(CleanupTrashOutcome *o);

/*
 * 항목 하나를 확인한 뒤 휴지통으로 옮긴다. 호출 스레드는 STA 로 COM 을 초기화해야 한다
 * (cleanup_trash_run·작업 스레드는 스스로 한다). out 의 이전 내용은 해제한다.
 * 반환값은 out->status.
 */
CleanupTrashStatus cleanup_trash_one(const CleanupGuard *g, const wchar_t *root,
                                     const CleanupItem *item, CleanupTrashOutcome *out);

/* ---- 작업 스레드 ---- */

typedef struct CleanupTrashJob CleanupTrashJob;

/* finished 가 1 이면 마지막 호출이며 이후 cleanup_trash_job_finish 가 바로 반환된다. */
typedef void (*cleanup_trash_notify_fn)(int finished, void *ctx);

typedef struct {
    size_t total;
    size_t done;   /* 처리를 마친 항목 수(성공·실패·거부 모두) */
    size_t moved;       /* 휴지통 또는 격리 폴더로 옮긴 항목 수 */
    size_t quarantined; /* moved 중 격리 폴더로 옮긴 항목 수 */
    size_t failed;      /* 옮기지 못한 항목 수(거부 포함) */
    uint64_t moved_bytes;
    int cancel_requested;
} CleanupTrashSnapshot;

/*
 * items[0..count) 를 순서대로 휴지통으로 옮기는 스레드를 시작한다. root 와 항목은 복사하므로
 * 호출 뒤 바로 해제해도 된다. 보호 목록은 cleanup_guard_add_system 으로 만든다.
 * notify 는 항목 하나를 마칠 때마다(최대 interval_ms 마다 한 번), 끝나면 반드시 한 번 호출된다.
 * 실패(메모리 부족·스레드 생성 실패) 시 NULL.
 */
CleanupTrashJob *cleanup_trash_job_start(const wchar_t *root, const CleanupItem *const *items,
                                         size_t count, uint32_t interval_ms,
                                         cleanup_trash_notify_fn notify, void *ctx);

/*
 * cleanup_trash_job_start 와 같지만 항목마다 탐지 경로를 따로 받는다(roots[i] 가 items[i] 의
 * 탐지 경로). 공용 개발 캐시처럼 서로 다른 폴더·드라이브에 있는 항목을 한 번에 옮길 때 쓴다.
 * 안전 장치는 항목마다 자기 탐지 경로로 똑같이 거친다(탐지 경로 안쪽이어야 함 등).
 */
CleanupTrashJob *cleanup_trash_job_start_each(const wchar_t *const *roots,
                                              const CleanupItem *const *items, size_t count,
                                              uint32_t interval_ms,
                                              cleanup_trash_notify_fn notify, void *ctx);

/* 남은 항목을 처리하지 않게 한다(진행 중인 항목은 끝까지 옮긴다). 어느 스레드에서든 된다. */
void cleanup_trash_job_cancel(CleanupTrashJob *job);

void cleanup_trash_job_progress(CleanupTrashJob *job, CleanupTrashSnapshot *out);

/*
 * 스레드가 끝날 때까지 기다린 뒤 항목별 결과(입력 순서, count 개)를 *outcomes 로 넘기고
 * job 을 해제한다. outcomes 가 NULL 이면 결과를 버린다. 넘겨받은 배열은
 * cleanup_trash_outcomes_free 로 해제한다. 반환값은 항목 수.
 */
size_t cleanup_trash_job_finish(CleanupTrashJob *job, CleanupTrashOutcome **outcomes);

void cleanup_trash_outcomes_free(CleanupTrashOutcome *outcomes, size_t count);

#endif /* BYTESIEVE_CLEANUP_TRASH_H */
