#ifndef BYTESIEVE_SCAN_HISTORY_H
#define BYTESIEVE_SCAN_HISTORY_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "scan_model.h"

/*
 * 폴더별 용량 변화 추적. 스캔이 끝나면 폴더 트리를 깊이 HISTORY_DEPTH 까지 스냅숏으로 저장해
 * 두고, 같은 대상 폴더를 다시 스캔하면 직전 스냅숏과 비교해 늘어난·줄어든·새로 생긴·사라진
 * 폴더를 구한다. 깊이 HISTORY_DEPTH 의 폴더 용량에는 그 아래 전체가 포함되므로 더 깊은 곳의
 * 변화도 그 조상에서 보인다.
 *
 * 저장 위치: 환경 변수 BYTESIEVE_HISTORY_DIR, 없으면 %LOCALAPPDATA%\ByteSieve\history.
 * 대상 폴더마다 파일 하나(<경로 해시>.snap, UTF-8 텍스트)이며 직전 스냅숏만 남긴다.
 */

/* 스냅숏에 남기는 폴더 깊이(루트 0). */
#define HISTORY_DEPTH 4

/* HistoryEntry.flags: SCAN_NODE_* 에 더해 */
enum {
    /* 이 폴더나 그 아래 어딘가를 다 읽지 못했다(용량이 실제보다 작을 수 있음). */
    HISTORY_UNCERTAIN = 1u << 16
};

typedef struct {
    wchar_t *rel;   /* 대상 폴더 기준 상대 경로. 루트는 "" */
    uint32_t depth; /* 루트 0 */
    uint32_t flags; /* SCAN_NODE_* | HISTORY_UNCERTAIN */
    uint64_t bytes; /* 하위 포함 논리 크기 */
    uint64_t alloc; /* 하위 포함 디스크 할당 크기 */
    uint64_t files; /* 하위 포함 파일 수 */
    size_t node;    /* 모델에서 만든 스냅숏이면 노드 인덱스, 불러온 스냅숏이면 SCAN_NODE_NONE */
} HistoryEntry;

typedef struct {
    wchar_t *root;    /* 대상 폴더(표시용 전체 경로) */
    uint64_t time;    /* 스캔 시각(FILETIME, UTC) */
    uint32_t depth;   /* 기록한 최대 깊이 */
    ScanStatus status;
    HistoryEntry *entries; /* 부모가 자식보다 앞에 온다 */
    size_t len;
    size_t cap;
} HistorySnapshot;

void history_snapshot_init(HistorySnapshot *s);
void history_snapshot_free(HistorySnapshot *s);

/*
 * 스캔 모델에서 스냅숏을 만든다(깊이는 HISTORY_DEPTH 와 모델의 max_depth 중 작은 쪽).
 * 시각은 지금이다. 모델이 비었거나 SCAN_FATAL 이거나 메모리가 부족하면 -1.
 * 이름에 제어 문자가 든 폴더(저장할 수 없음)와 그 아래는 뺀다.
 */
int history_snapshot_from_model(HistorySnapshot *out, const ScanModel *model);

/* 파일에 쓴다(임시 파일에 쓴 뒤 바꿔치기). 성공 0, 실패 -1(*error 에 Windows 오류, NULL 허용). */
int history_save(const HistorySnapshot *s, const wchar_t *file, unsigned long *error);

/* 파일에서 읽는다. 파일이 없거나 형식이 맞지 않으면 -1. */
int history_load(HistorySnapshot *out, const wchar_t *file);

/*
 * 기록 폴더(끝 구분자 없음)를 buf 에 쓴다. BYTESIEVE_HISTORY_DIR 이 있으면 그것,
 * 없으면 %LOCALAPPDATA%\ByteSieve\history. 정할 수 없으면 -1.
 */
int history_default_dir(wchar_t *buf, size_t len);

/* 폴더를 (상위까지) 만든다. 이미 있으면 성공. 성공 0. */
int history_ensure_dir(const wchar_t *dir);

/* dir 안에서 대상 폴더 root 의 스냅숏 파일 경로(malloc). 대소문자·끝 구분자는 무시한다. */
wchar_t *history_file_for(const wchar_t *dir, const wchar_t *root);

/*
 * 자동 저장 설정. 기록 폴더 안의 settings.txt(UTF-8 텍스트)에 둔다. 기록을 삭제해도 남는다.
 * GUI 가 스캔이 끝날 때 스냅숏을 저장할지 정하는 데 쓴다(CLI --track 은 명시적 요청이라 보지 않는다).
 */
#define HISTORY_SETTINGS_FILE L"settings.txt"

/* 자동 저장이 켜져 있으면 1, 꺼져 있으면 0. 설정 파일이 없거나 읽을 수 없거나 손상됐으면 1(기본값). */
int history_autosave_enabled(const wchar_t *dir);

/*
 * 자동 저장 설정을 쓴다(폴더가 없으면 만들고, 임시 파일에 쓴 뒤 바꿔치기).
 * 성공 0, 실패 -1(*error 에 Windows 오류, NULL 허용).
 */
int history_set_autosave(const wchar_t *dir, int enabled, unsigned long *error);

/*
 * dir 안의 스냅숏 파일 수. history_file_for 가 만드는 이름(<16자리 16진수>.snap)의 일반 파일만 센다.
 * 폴더가 없으면 0.
 */
size_t history_count(const wchar_t *dir);

typedef struct {
    size_t removed;      /* 지운 스냅숏 파일 수 */
    size_t failed;       /* 지우지 못한 파일 수 */
    unsigned long error; /* 처음 실패한 Windows 오류(없으면 0) */
} HistoryClearResult;

/*
 * 저장된 스냅숏을 모두 지운다(되돌릴 수 없음, 확인은 호출자 몫).
 *   - history_file_for 형식의 이름인 일반 파일(<hex>.snap 과 남은 <hex>.snap.tmp)만 지운다.
 *     설정 파일, 형식이 다른 파일, 폴더, 링크(리파스 포인트)는 건드리지 않는다.
 *   - 한 파일을 지우지 못해도 나머지는 계속 지우고 out 에 개수를 남긴다.
 * 모두 지웠으면(지울 것이 없거나 폴더가 없어도) 0, 하나라도 실패했거나 폴더를 읽지 못하면 -1.
 */
int history_clear(const wchar_t *dir, HistoryClearResult *out);

typedef enum {
    HISTORY_GROWN = 0,
    HISTORY_SHRUNK,
    HISTORY_ADDED,  /* 직전 스냅숏에 없던 폴더(새 폴더 안의 하위 폴더는 따로 나오지 않는다) */
    HISTORY_REMOVED /* 지금은 없는 폴더(사라진 폴더 안의 하위 폴더는 따로 나오지 않는다) */
} HistoryKind;

typedef struct {
    wchar_t *rel; /* 상대 경로(루트는 "") */
    HistoryKind kind;
    uint32_t depth;
    uint64_t old_bytes; /* ADDED 이면 0 */
    uint64_t new_bytes; /* REMOVED 이면 0 */
    int64_t delta;      /* new - old */
    int uncertain;      /* 어느 한쪽이라도 다 읽지 못해 정확하지 않을 수 있음 */
    size_t node;        /* 현재 모델의 노드(REMOVED 이면 SCAN_NODE_NONE) */
} HistoryChange;

typedef struct {
    HistoryChange *items; /* |delta| 내림차순, 같으면 경로순. 루트는 넣지 않는다 */
    size_t len;
    size_t cap;
    uint64_t old_time;
    uint64_t old_total; /* 루트 용량 */
    uint64_t new_total;
    int total_uncertain;
    size_t grown, shrunk, added, removed;
} HistoryDiff;

void history_diff_init(HistoryDiff *d);
void history_diff_free(HistoryDiff *d);

/*
 * 직전 스냅숏 old 와 지금 스냅숏 cur 를 비교한다(두 스냅숏 깊이 중 작은 깊이까지).
 * 대상 폴더가 다르면 -1, 메모리가 부족하면 -1. 용량이 같은 폴더는 넣지 않는다.
 */
int history_diff(const HistorySnapshot *old, const HistorySnapshot *cur, HistoryDiff *out);

/* 상대 경로를 루트 기준 전체 경로로(malloc). rel 이 "" 이면 root 복사. */
wchar_t *history_full_path(const wchar_t *root, const wchar_t *rel);

/* 식별자 이름("grown", "shrunk", "added", "removed"). */
const wchar_t *history_kind_name(HistoryKind kind);

#endif /* BYTESIEVE_SCAN_HISTORY_H */
