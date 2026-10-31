#ifndef BYTESIEVE_SCAN_MODEL_H
#define BYTESIEVE_SCAN_MODEL_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "scanner.h"

/*
 * GUI 연동용 스캔 데이터 모델. 출력(CLI·JSON Lines)과 무관하게
 *   - 진행률 스냅샷(ScanProgress, 콜백)
 *   - 폴더 트리(ScanNode, 인덱스로 참조)
 *   - 폴더별 용량과 용량 순위
 * 를 제공한다. 파일은 노드로 만들지 않고 소속 폴더의 own_* 에만 집계한다.
 *
 * 노드는 인덱스(size_t)로 참조하며 0 이 루트다. 부모는 항상 자식보다 인덱스가 작다.
 * scan_model_build 가 끝난 뒤의 모델은 읽기 전용으로 써도 된다.
 */

#define SCAN_NODE_NONE ((size_t)-1)

/* ScanNode.flags */
enum {
    SCAN_NODE_REPARSE = 1u << 0,     /* 재분석 지점이라 하위로 들어가지 않음 */
    SCAN_NODE_DEPTH_LIMIT = 1u << 1, /* max_depth 때문에 하위로 들어가지 않음 */
    SCAN_NODE_FAILED = 1u << 2,      /* 열지 못함(하위 내용 모름) */
    SCAN_NODE_INCOMPLETE = 1u << 3,  /* 열거 도중 오류·취소(일부 항목 누락 가능) */
    SCAN_NODE_CANCELLED = 1u << 4    /* 취소로 열거하지 않음(하위 내용 모름) */
};

typedef struct {
    wchar_t *name;   /* 루트는 표시용 전체 경로, 나머지는 폴더 이름(모델이 소유, 읽기 전용) */
    size_t parent;   /* 루트는 SCAN_NODE_NONE */
    size_t first_child;
    size_t last_child;
    size_t next_sibling;
    size_t child_count;
    uint32_t depth;  /* 루트 0 */
    uint32_t flags;  /* SCAN_NODE_* */

    uint64_t own_files; /* 이 폴더 바로 아래 파일 */
    uint64_t own_bytes;
    uint64_t total_files; /* 하위 트리 전체(자신 포함) */
    uint64_t total_dirs;  /* 하위 폴더 수(자신 제외) */
    uint64_t total_bytes;
    uint64_t own_alloc;   /* 바로 아래 파일의 디스크 할당 크기(ScanEntry.alloc_size 합) */
    uint64_t total_alloc; /* 하위 트리 전체의 디스크 할당 크기 */

    /*
     * 정밀 스캔(SCAN_OPT_HARDLINKS)에서 위 값 중 하드 링크로 이미 다른 경로에서 센 몫.
     * 기본 스캔이면 모두 0 이다. 같은 실제 파일은 처음 만난 폴더에만 넣으므로(du 와 같은 방식)
     *   중복 제외 용량 = total_bytes - total_dup_bytes,  실제 디스크 사용 = total_alloc - total_dup_alloc
     * 이고 자식들의 중복 제외 용량 합이 부모와 맞는다. 다른 폴더와 실제 파일을 나눠 쓰는 폴더는
     * 그 폴더만 따로 지울 때 확보되는 용량보다 작게 보일 수 있다.
     */
    uint64_t own_dup_files;
    uint64_t own_dup_bytes;
    uint64_t own_dup_alloc;
    uint64_t total_dup_files;
    uint64_t total_dup_bytes;
    uint64_t total_dup_alloc;
} ScanNode;

/* 진행률 스냅샷(콜백 호출 동안만 유효). 전체 양은 미리 알 수 없으므로 비율은 추정치다. */
typedef struct {
    uint64_t files;
    uint64_t dirs;
    uint64_t bytes;
    uint64_t errors;
    uint64_t dirs_scanned;   /* 열거를 마친 폴더 */
    size_t dirs_pending;     /* 발견했지만 아직 열거하지 않은 폴더 */
    uint32_t depth;          /* current_dir 의 깊이 */
    const wchar_t *current_dir; /* 지금 열거 중인 폴더(표시용 경로), 완료 시 NULL */
    int finished;            /* 마지막 호출이면 1 */
    ScanStatus status;       /* finished 일 때만 의미 있음 */
    int precise;             /* 정밀 스캔(하드 링크 확인) 중이면 1 */
    uint64_t dup_files;      /* 정밀 스캔: 지금까지 찾은 하드 링크 중복 경로 */
    uint64_t dup_bytes;      /* 그 논리 크기 합 */
} ScanProgress;

typedef void (*scan_progress_fn)(const ScanProgress *progress, void *ctx);

/* 0 이 아니면 스캔을 멈춘다. 스캔 스레드에서 호출되므로 다른 스레드가 세운 플래그를 읽으면 된다. */
typedef int (*scan_cancel_fn)(void *ctx);

/* 폴더 이름 저장소(내부용). 이름을 노드마다 따로 할당하지 않고 큰 덩어리에 이어 담는다. */
typedef struct ScanNameChunk ScanNameChunk;

typedef struct {
    ScanNode *nodes;
    size_t len;
    size_t cap;
    ScanNameChunk *names; /* 내부용 */
    ScanReport report; /* 전체 집계·오류(scanner 와 같은 의미) */
    ScanStatus status;
    uint32_t max_depth;
    unsigned options;  /* 스캔에 쓴 SCAN_OPT_*(정밀 스캔이면 SCAN_OPT_HARDLINKS) */
    int out_of_memory; /* 노드를 만들지 못해 트리가 일부 빠졌으면 1 (report 집계는 정확) */
} ScanModel;

void scan_model_init(ScanModel *model);
void scan_model_free(ScanModel *model);

/*
 * root 를 max_depth 까지 스캔해 model 을 채운다(이전 내용은 지운다).
 * progress 가 NULL 이 아니면 폴더 열거를 시작할 때마다, 그리고 끝에 finished=1 로
 * 한 번 더 호출한다. SCAN_FATAL 이어도 루트 노드는 SCAN_NODE_FAILED 로 남는다
 * (루트 경로 자체를 표현할 수 없으면 노드 없음).
 */
ScanStatus scan_model_build(ScanModel *model, const wchar_t *root, uint32_t max_depth,
                            scan_progress_fn progress, void *ctx);

/*
 * scan_model_build 에 취소를 더한 판. cancel 이 0 이 아닌 값을 돌려주면 그때까지 모은
 * 트리로 마무리하고 SCAN_PARTIAL 을 반환한다(model->report.cancelled = 1).
 * 열거하지 못한 폴더는 SCAN_NODE_CANCELLED, 열거 도중 멈춘 폴더는 SCAN_NODE_INCOMPLETE.
 * progress 와 cancel 은 같은 ctx 를 받는다. cancel 이 NULL 이면 scan_model_build 와 같다.
 */
ScanStatus scan_model_build_ex(ScanModel *model, const wchar_t *root, uint32_t max_depth,
                               scan_progress_fn progress, scan_cancel_fn cancel,
                               void *ctx);

/*
 * scan_model_build_ex 에 스캔 옵션(SCAN_OPT_*)을 더한 판. SCAN_OPT_HARDLINKS 면 정밀 스캔으로
 * 노드의 *_dup_* 와 report 의 hardlink_* 를 채운다(경로마다 센 기본 값은 그대로). options 0 은
 * scan_model_build_ex 와 같다.
 */
ScanStatus scan_model_build_opts(ScanModel *model, const wchar_t *root, uint32_t max_depth,
                                 unsigned options, scan_progress_fn progress,
                                 scan_cancel_fn cancel, void *ctx);

/* 진행률 추정(0.0~1.0): 열거를 마친 폴더 / (마친 폴더 + 남은 폴더). */
double scan_progress_fraction(const ScanProgress *progress);

/* 범위 밖이면 NULL. */
const ScanNode *scan_model_node(const ScanModel *model, size_t index);

typedef enum {
    SCAN_ORDER_SCAN = 0,     /* 열거 순서 */
    SCAN_ORDER_SIZE_DESC,    /* total_bytes 내림차순(같으면 이름순) */
    SCAN_ORDER_NAME          /* 이름 오름차순(대소문자 무시) */
} ScanChildOrder;

/*
 * index 의 자식 인덱스를 order 순서로 out 에 최대 cap 개 쓰고 전체 자식 수를 반환한다.
 * out 이 NULL 이면 개수만 반환한다.
 */
size_t scan_model_children(const ScanModel *model, size_t index, ScanChildOrder order,
                           size_t *out, size_t cap);

/* 노드의 표시용 전체 경로(malloc, 호출자가 free). 실패 시 NULL. */
wchar_t *scan_model_path(const ScanModel *model, size_t index);

typedef enum {
    SCAN_RANK_TOTAL_BYTES = 0, /* 하위 트리 포함 용량 */
    SCAN_RANK_OWN_BYTES,       /* 폴더 바로 아래 파일 용량 */
    SCAN_RANK_TOTAL_ALLOC,     /* 하위 트리 포함 디스크 할당 크기 */
    SCAN_RANK_TOTAL_UNIQUE,    /* 정밀 스캔: 하드 링크 중복 제외 용량(total_bytes - total_dup_bytes) */
    SCAN_RANK_TOTAL_DUP        /* 정밀 스캔: 하드 링크로 중복 집계된 용량(total_dup_bytes) */
} ScanRankKey;

/*
 * 루트를 제외한 폴더를 key 기준 내림차순으로 최대 n 개 out 에 쓰고 쓴 개수를 반환한다.
 * 값이 같으면 인덱스(열거 순서)가 작은 쪽이 앞선다.
 */
size_t scan_model_rank(const ScanModel *model, ScanRankKey key, size_t *out, size_t n);

#endif /* BYTESIEVE_SCAN_MODEL_H */
