#ifndef BYTESIEVE_DRIVE_H
#define BYTESIEVE_DRIVE_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/*
 * 논리 드라이브 목록과 용량 조회(GetLogicalDrives, GetDiskFreeSpaceExW).
 * UI 에 의존하지 않으며 GUI 와 테스트가 함께 쓴다.
 */

/* 논리 드라이브는 A: ~ Z: 최대 26개. */
#define DRIVE_MAX 26
/* "C:\" + NUL */
#define DRIVE_ROOT_LEN 4
/* GetVolumeInformationW 권장 버퍼 크기(MAX_PATH + 1). */
#define DRIVE_LABEL_LEN 261
#define DRIVE_FS_LEN 32

typedef enum {
    DRIVE_TYPE_UNKNOWN = 0,
    DRIVE_TYPE_NO_ROOT,   /* 루트 경로가 잘못됨(조회 도중 사라진 드라이브 등) */
    DRIVE_TYPE_REMOVABLE, /* USB 메모리, 카드 리더 등 */
    DRIVE_TYPE_FIXED,
    DRIVE_TYPE_REMOTE,    /* 네트워크 드라이브 */
    DRIVE_TYPE_CDROM,
    DRIVE_TYPE_RAMDISK
} DriveType;

/* 용량 조회 결과. OK 가 아니면 용량 필드는 0 이다. */
typedef enum {
    DRIVE_STATUS_OK = 0,
    DRIVE_STATUS_NOT_READY,     /* 미디어 없음, 장치 준비 안 됨 */
    DRIVE_STATUS_ACCESS_DENIED,
    DRIVE_STATUS_MISSING,       /* 드라이브가 없어짐(분리됨) */
    DRIVE_STATUS_DISCONNECTED,  /* 네트워크 경로에 연결할 수 없음 */
    DRIVE_STATUS_ERROR          /* 기타 */
} DriveStatus;

typedef struct {
    wchar_t root[DRIVE_ROOT_LEN];  /* "C:\" */
    DriveType type;
    DriveStatus status;
    unsigned long error;           /* status 가 OK 가 아닐 때의 Windows 오류 코드 */
    wchar_t label[DRIVE_LABEL_LEN]; /* 볼륨 이름. 모르면 빈 문자열 */
    wchar_t fs[DRIVE_FS_LEN];      /* "NTFS" 등. 모르면 빈 문자열 */
    uint64_t total_bytes;          /* 호출자에게 보이는 전체 용량(할당량이 있으면 할당량) */
    uint64_t free_bytes;           /* 볼륨 전체의 여유 공간 */
    uint64_t available_bytes;      /* 호출자가 실제로 쓸 수 있는 여유 공간 */
} DriveInfo;

typedef struct {
    DriveInfo items[DRIVE_MAX];
    size_t len;
} DriveList;

/*
 * 드라이브 하나를 조회해 out 을 채운다. out->root 는 호출 전에 채워져 있어야 한다.
 * 실패해도 out->status/error 에 기록할 뿐 프로그램을 멈추지 않는다.
 */
typedef void (*DriveQueryFn)(DriveInfo *out, void *ctx);

/* GetDriveTypeW·GetDiskFreeSpaceExW·GetVolumeInformationW 로 실제 드라이브를 조회한다.
 * 이동식·광학 드라이브에 미디어가 없어도 시스템 오류 대화상자를 띄우지 않는다. */
void drive_query_system(DriveInfo *out, void *ctx);

/*
 * mask(비트 0 = A:, 비트 25 = Z:)에 있는 드라이브마다 query 를 호출해 목록을 만든다.
 * 목록은 드라이브 문자 순서다. 성공 0, 인자가 잘못되면 -1.
 */
int drive_list_build(unsigned long mask, DriveQueryFn query, void *ctx, DriveList *out);

/*
 * 시스템의 논리 드라이브 목록을 조회한다(GetLogicalDrives + drive_query_system).
 * 성공 0. GetLogicalDrives 가 실패하면 -1 이고 *error 에 Windows 오류 코드를 넣는다(NULL 허용).
 * 개별 드라이브의 실패는 각 항목의 status 로 알린다.
 */
int drive_list_query(DriveList *out, unsigned long *error);

/* "X:\" 를 buf(DRIVE_ROOT_LEN 이상)에 쓴다. letter 는 'A'~'Z'(소문자 허용). 실패 -1. */
int drive_root_from_letter(wchar_t letter, wchar_t *buf, size_t len);

/* Windows 오류 코드를 상태로 분류한다(0 은 OK). */
DriveStatus drive_status_from_error(unsigned long error);

/* GetDriveTypeW 반환값을 DriveType 으로 바꾼다. */
DriveType drive_type_from_win32(unsigned int type);

/* 사용 용량(전체 - 여유). 여유가 전체보다 크게 보고되면 0. */
uint64_t drive_used_bytes(const DriveInfo *d);

/* 사용률(천분율, 0~1000, 내림). 전체가 0 이거나 OK 가 아니면 0. */
unsigned drive_usage_permille(const DriveInfo *d);

/* 식별자 이름("fixed", "not_ready" 등). 화면 문구는 GUI 가 따로 정한다. */
const wchar_t *drive_type_name(DriveType type);
const wchar_t *drive_status_name(DriveStatus status);

#endif /* BYTESIEVE_DRIVE_H */
