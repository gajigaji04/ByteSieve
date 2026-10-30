#ifndef BYTESIEVE_CLEANUP_SCAN_H
#define BYTESIEVE_CLEANUP_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "cleanup_rules.h"
#include "scanner.h"

/*
 * 정리 후보 탐지. scan_tree_ex 로 root 아래를 탐색하며 규칙(cleanup_rules)에 맞는
 * 폴더·파일을 모은다. 파일 내용은 읽지 않고 열거로 얻은 이름·크기만 쓴다.
 * 아무것도 삭제·수정하지 않는다.
 *
 * 정책:
 *   - 루트에서 가장 먼저 맞은 폴더가 그 아래 전체를 차지한다. 그 안의 다른 후보
 *     (node_modules\x\dist, build\a.log 등)는 따로 만들지 않고 용량에 포함한다.
 *     따라서 항목 용량의 합이 곧 예상 확보 용량이며 중복이 없다.
 *   - 재분석 지점(정션·심볼릭 링크)은 폴더든 파일이든 따라가지도, 항목으로 만들지도,
 *     용량에 더하지도 않는다.
 *   - 보호 경로(CleanupProtect)와 같은 폴더는 들어가지 않는다(excluded_dir_count).
 *     단, root 자체가 보호 경로 안이면 제외하지 않고 root_protected 를 1 로 둔다.
 *   - root 자체는 탐색 범위일 뿐 항목이 되지 않는다.
 *   - 오류 처리는 scanner 와 같다(접근 거부 등은 기록하고 계속, root 를 못 열면 SCAN_FATAL).
 */

/* CleanupItem.flags */
enum {
    CLEANUP_ITEM_DIR = 1u << 0,        /* 폴더 항목(아니면 파일) */
    CLEANUP_ITEM_CONFIRMED = 1u << 1,  /* 같은 폴더에 프로젝트 표시 파일이 있음 */
    CLEANUP_ITEM_INCOMPLETE = 1u << 2, /* 하위를 다 읽지 못함(오류·취소) — 용량이 더 클 수 있음 */
    CLEANUP_ITEM_PROTECTED = 1u << 3   /* 시스템 보호 경로 안(root_protected 일 때 모든 항목) */
};

typedef struct {
    wchar_t *path; /* 표시용 전체 경로(확장 접두사 없음) */
    CleanupCategory category;
    uint32_t flags;
    uint64_t bytes; /* 폴더는 하위 전체 파일의 논리 크기 합 */
    uint64_t files; /* 폴더는 하위 파일 수, 파일은 1 */
    uint64_t dirs;  /* 폴더의 하위 폴더 수 */
    size_t pending; /* 내부용: 아직 열거를 마치지 않은 하위 폴더 수 */
} CleanupItem;

typedef struct {
    CleanupItem *items; /* 발견 순서 */
    size_t len;
    size_t cap;
    uint64_t total_bytes; /* 예상 확보 용량(모든 항목 용량 합) */
    uint64_t category_bytes[CLEANUP_CATEGORY_COUNT];
    size_t category_count[CLEANUP_CATEGORY_COUNT];
    ScanReport report; /* 전체 탐색 집계·오류(scanner 와 같은 의미) */
    ScanStatus status;
    wchar_t *root;     /* 표시용 전체 경로 */
    int root_protected;
    int out_of_memory; /* 항목을 만들지 못해 일부 빠졌으면 1 */
} CleanupResult;

/* 진행률(콜백 호출 동안만 유효). */
typedef struct {
    uint64_t files;
    uint64_t dirs;
    uint64_t errors;
    uint64_t dirs_scanned;
    size_t dirs_pending;
    size_t items;
    uint64_t item_bytes;        /* 지금까지 찾은 후보 용량 */
    const wchar_t *current_dir; /* 표시용 경로, 완료 시 NULL */
    int finished;
} CleanupProgress;

typedef void (*cleanup_progress_fn)(const CleanupProgress *progress, void *ctx);
typedef int (*cleanup_cancel_fn)(void *ctx);

void cleanup_result_init(CleanupResult *r);
void cleanup_result_free(CleanupResult *r);

/*
 * root 아래 정리 후보를 찾아 out 을 채운다(이전 내용은 지운다).
 * protect 가 NULL 이면 볼륨 루트의 시스템 폴더 이름만 제외한다.
 * progress 는 폴더 열거를 시작할 때마다, 끝에 finished=1 로 한 번 더 호출한다.
 * cancel 이 0 이 아니면 그때까지 모은 결과로 마치고 SCAN_PARTIAL 을 반환한다
 * (report.cancelled = 1, 다 읽지 못한 항목은 CLEANUP_ITEM_INCOMPLETE).
 * progress·cancel 은 NULL 일 수 있고 같은 ctx 를 받는다.
 */
ScanStatus cleanup_scan(CleanupResult *out, const wchar_t *root, const CleanupProtect *protect,
                        cleanup_progress_fn progress, cleanup_cancel_fn cancel, void *ctx);

/*
 * mask 에 든 유형의 항목 인덱스를 용량 내림차순(같으면 경로순)으로 out 에 최대 cap 개 쓰고
 * 조건에 맞는 전체 개수를 반환한다. out 이 NULL 이면 개수만 센다. bytes 가 NULL 이 아니면
 * 조건에 맞는 항목의 용량 합을 넣는다. 정렬용 메모리가 부족하면 발견 순서로 쓴다.
 */
size_t cleanup_select(const CleanupResult *r, unsigned mask, size_t *out, size_t cap,
                      uint64_t *bytes);

#endif /* BYTESIEVE_CLEANUP_SCAN_H */
