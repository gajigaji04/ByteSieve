#ifndef BYTESIEVE_QUARANTINE_H
#define BYTESIEVE_QUARANTINE_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/*
 * 휴지통이 없는 드라이브(USB·외장 드라이브 등)용 격리 폴더. 정리할 항목을 같은 볼륨의
 * 격리 폴더로 이름만 바꿔 옮기고(복사 없음), 나중에 원래 위치로 복원하거나 사용자가 고른
 * 항목만 영구 삭제한다.
 *
 * 구조(드라이브마다 하나, 숨김 폴더):
 *   X:\.bytesieve-trash\                    격리 폴더(CLEANUP_QUARANTINE_DIR)
 *     20261031-153012-1\                    항목 폴더(격리한 시각 + 번호)
 *       entry.txt                           기록(UTF-8 "키=값" 줄)
 *       f\<원래 이름>                        격리된 파일·폴더
 *
 * 데이터 유실 방지:
 *   - 기록은 옮기기 전에 쓰고(state=moving) 옮긴 뒤 고친다(state=moved). 기록 파일은 임시
 *     파일에 쓴 뒤 바꿔치기한다.
 *   - 옮기기·복원은 MoveFileExW(덮어쓰기·복사 없음)라서 다른 볼륨이나 이미 있는 이름으로는
 *     실패할 뿐 아무것도 지우지 않는다.
 *   - 기록과 실제 파일이 어긋나도 파일을 지우지 않는다. 기록만 있으면 MISSING, 기록이 없거나
 *     손상됐으면 ORPHAN 으로 보여 주고 처리는 사용자에게 맡긴다.
 *   - 실패를 정리할 때 지우는 것은 기록 파일과 빈 폴더뿐이다(폴더는 비어 있을 때만 지운다).
 *   - 영구 삭제는 quarantine_purge 로 사용자가 고른 항목만, 격리 폴더 안인지 다시 확인한 뒤에
 *     한다. 재분석 지점(정션·심볼릭 링크)은 따라가지 않고 링크만 지운다.
 */

typedef enum {
    QUARANTINE_OK = 0,
    QUARANTINE_MISSING,       /* 옮길·복원할 항목이 없음 */
    QUARANTINE_CONFLICT,      /* 복원 위치에 같은 이름이 있음(덮어쓰지 않음) */
    QUARANTINE_ACCESS_DENIED, /* 권한 부족·쓰기 금지 */
    QUARANTINE_IN_USE,        /* 다른 프로그램이 사용 중 */
    QUARANTINE_NO_SPACE,      /* 디스크 공간 부족(기록을 쓰지 못함) */
    QUARANTINE_BAD_STORE,     /* 격리 폴더를 만들 수 없거나 링크·파일로 바뀌었거나 기록이 없음 */
    QUARANTINE_OTHER_VOLUME,  /* 다른 볼륨(이름 바꾸기로 옮길 수 없음) */
    QUARANTINE_FAILED,        /* 그 밖의 실패(code 참고) */
    QUARANTINE_STATUS_COUNT
} QuarantineStatus;

/* 화면용 짧은 설명("권한 부족" 등). */
const wchar_t *quarantine_status_text(QuarantineStatus status);

/* 식별자 이름("ok", "conflict" 등). */
const wchar_t *quarantine_status_name(QuarantineStatus status);

/* Windows 오류 코드를 상태로 바꾼다(0 이면 OK). */
QuarantineStatus quarantine_status_from_error(uint32_t error);

/*
 * path 를 격리할 때 쓸 볼륨 루트(malloc, "D:\"). 테스트용 환경 변수
 * BYTESIEVE_QUARANTINE_ROOT 가 path 를 포함하면 그 경로를 돌려준다. 실패 시 NULL.
 */
wchar_t *quarantine_volume_root(const wchar_t *path);

/*
 * 테스트용 환경 변수 BYTESIEVE_QUARANTINE_ROOT 가 path 를 포함하면 1. 이 경우 정리는
 * 휴지통이 없는 드라이브처럼 처리한다(cleanup_trash).
 */
int quarantine_test_root_contains(const wchar_t *path);

/* volume_root 의 격리 폴더 경로(malloc, "D:\.bytesieve-trash"). */
wchar_t *quarantine_store_path(const wchar_t *volume_root);

/*
 * path 를 volume_root 의 격리 폴더로 옮긴다. path 는 volume_root 아래의 절대 경로여야 한다.
 * is_dir·bytes 는 기록용이다. 성공하면 *moved_path(NULL 가능)에 격리된 위치를 넘긴다(malloc).
 * 실패하면 원래 항목은 그대로이고 *code 에 Windows 오류 코드가 남는다.
 */
QuarantineStatus quarantine_move(const wchar_t *volume_root, const wchar_t *path, int is_dir,
                                 uint64_t bytes, wchar_t **moved_path, uint32_t *code);

/* ---- 목록 ---- */

typedef enum {
    QUARANTINE_ENTRY_OK = 0, /* 기록과 격리된 항목이 모두 있음(복원·영구 삭제 가능) */
    QUARANTINE_ENTRY_MISSING, /* 기록만 있고 격리된 항목이 없음(영구 삭제는 기록만 지움) */
    QUARANTINE_ENTRY_ORPHAN   /* 기록이 없거나 손상됨(원래 위치를 몰라 복원 불가, 파일은 둠) */
} QuarantineEntryState;

typedef struct {
    QuarantineEntryState state;
    wchar_t *volume_root; /* "D:\" */
    wchar_t *store;       /* 격리 폴더 */
    wchar_t *entry;       /* 항목 폴더(ORPHAN 이면 격리 폴더 바로 아래의 그 항목) */
    wchar_t *item;        /* 격리된 실제 항목(MISSING 이면 NULL, ORPHAN 이면 entry 와 같음) */
    wchar_t *original;    /* 기록된 원래 경로(ORPHAN 이면 NULL) */
    wchar_t *restore;     /* 지금 볼륨 루트 기준 복원 위치(드라이브 문자가 바뀌어도 맞음, ORPHAN 이면 NULL) */
    uint64_t time;        /* 격리한 시각(FILETIME, UTC). 모르면 0 */
    uint64_t bytes;       /* 기록된 용량 */
    int is_dir;
} QuarantineEntry;

typedef struct {
    QuarantineEntry *items;
    size_t len;
    size_t cap;
} QuarantineList;

void quarantine_list_init(QuarantineList *list);
void quarantine_list_free(QuarantineList *list);

/*
 * volume_root 의 격리 폴더 항목을 list 에 덧붙인다. 격리 폴더가 없으면 아무것도 붙이지 않고
 * OK. 격리 폴더가 링크이거나 읽지 못하면 그 상태를 반환한다.
 */
QuarantineStatus quarantine_list_volume(const wchar_t *volume_root, QuarantineList *list);

/*
 * 고정·이동식 드라이브(와 테스트용 BYTESIEVE_QUARANTINE_ROOT)의 격리 항목을 모두 모은다.
 * list 의 이전 내용은 해제한다. 격리한 시각의 내림차순(최근 것 먼저). 읽지 못한 드라이브 수를
 * 반환한다.
 */
size_t quarantine_list_system(QuarantineList *list);

/* ---- 복원·영구 삭제 ---- */

/*
 * 격리된 항목을 target(NULL 이면 e->restore)으로 옮긴다. 덮어쓰지 않으며 같은 이름이 있으면
 * CONFLICT. 없는 상위 폴더는 만든다. 성공하면 기록과 빈 항목 폴더를 지운다.
 */
QuarantineStatus quarantine_restore(const QuarantineEntry *e, const wchar_t *target,
                                    uint32_t *code);

/*
 * 복원 위치가 이미 있을 때 쓸 수 있는 다른 이름(malloc): "이름 (복원됨).확장자",
 * "이름 (복원됨 2).확장자" ... 중 아직 없는 첫 번째. 실패 시 NULL.
 */
wchar_t *quarantine_alternate_path(const wchar_t *path, int is_dir);

/*
 * 항목을 영구 삭제한다(되돌릴 수 없음). OK 면 격리된 항목과 기록을, MISSING 이면 기록만,
 * ORPHAN 이면 그 항목 전체를 지운다. 격리 폴더 바로 아래가 아니거나 격리 폴더가 링크면
 * BAD_STORE 로 거부한다. 일부만 지웠으면 실패를 반환하고 남은 항목은 다음 목록에 보인다.
 */
QuarantineStatus quarantine_purge(const QuarantineEntry *e, uint32_t *code);

#endif /* BYTESIEVE_QUARANTINE_H */
