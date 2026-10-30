#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#include "drive.h"

int drive_root_from_letter(wchar_t letter, wchar_t *buf, size_t len)
{
    if (buf == NULL || len < DRIVE_ROOT_LEN) {
        return -1;
    }
    if (letter >= L'a' && letter <= L'z') {
        letter = (wchar_t)(letter - L'a' + L'A');
    }
    if (letter < L'A' || letter > L'Z') {
        return -1;
    }
    buf[0] = letter;
    buf[1] = L':';
    buf[2] = L'\\';
    buf[3] = L'\0';
    return 0;
}

DriveStatus drive_status_from_error(unsigned long error)
{
    switch (error) {
    case ERROR_SUCCESS:
        return DRIVE_STATUS_OK;
    case ERROR_NOT_READY:
    case ERROR_NO_MEDIA_IN_DRIVE:
    case ERROR_UNRECOGNIZED_MEDIA:
    case ERROR_UNRECOGNIZED_VOLUME:
    case ERROR_MEDIA_CHANGED:
    case ERROR_WRONG_DISK:
        return DRIVE_STATUS_NOT_READY;
    case ERROR_ACCESS_DENIED:
        return DRIVE_STATUS_ACCESS_DENIED;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_DEV_NOT_EXIST:
    case ERROR_DEVICE_REMOVED:
        return DRIVE_STATUS_MISSING;
    case ERROR_BAD_NETPATH:
    case ERROR_NETNAME_DELETED:
    case ERROR_BAD_NET_NAME:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_NO_NET_OR_BAD_PATH:
    case ERROR_NOT_CONNECTED:
    case ERROR_CONNECTION_UNAVAIL:
    case ERROR_SEM_TIMEOUT:
        return DRIVE_STATUS_DISCONNECTED;
    default:
        return DRIVE_STATUS_ERROR;
    }
}

DriveType drive_type_from_win32(unsigned int type)
{
    switch (type) {
    case DRIVE_NO_ROOT_DIR:
        return DRIVE_TYPE_NO_ROOT;
    case DRIVE_REMOVABLE:
        return DRIVE_TYPE_REMOVABLE;
    case DRIVE_FIXED:
        return DRIVE_TYPE_FIXED;
    case DRIVE_REMOTE:
        return DRIVE_TYPE_REMOTE;
    case DRIVE_CDROM:
        return DRIVE_TYPE_CDROM;
    case DRIVE_RAMDISK:
        return DRIVE_TYPE_RAMDISK;
    default:
        return DRIVE_TYPE_UNKNOWN;
    }
}

static void set_failure(DriveInfo *out, DWORD error)
{
    out->status = drive_status_from_error(error);
    if (out->status == DRIVE_STATUS_OK) {
        /* 실패했는데 오류 코드가 0 으로 남는 경우도 실패로 본다. */
        out->status = DRIVE_STATUS_ERROR;
    }
    out->error = error;
    out->total_bytes = 0;
    out->free_bytes = 0;
    out->available_bytes = 0;
}

void drive_query_system(DriveInfo *out, void *ctx)
{
    ULARGE_INTEGER avail, total, free_bytes;
    DWORD old_mode;
    BOOL mode_set;

    (void)ctx;
    if (out == NULL) {
        return;
    }
    /* 미디어가 없는 이동식·광학 드라이브에서 "디스크를 넣으세요" 대화상자가 뜨지 않게 한다.
     * 스레드 단위 설정이라 다른 스레드(스캔 작업 등)에는 영향을 주지 않는다. */
    mode_set = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &old_mode);

    out->type = drive_type_from_win32(GetDriveTypeW(out->root));
    out->label[0] = L'\0';
    out->fs[0] = L'\0';

    if (out->type == DRIVE_TYPE_NO_ROOT) {
        set_failure(out, ERROR_PATH_NOT_FOUND);
    } else if (!GetDiskFreeSpaceExW(out->root, &avail, &total, &free_bytes)) {
        set_failure(out, GetLastError());
    } else {
        out->status = DRIVE_STATUS_OK;
        out->error = 0;
        out->total_bytes = total.QuadPart;
        out->free_bytes = free_bytes.QuadPart;
        out->available_bytes = avail.QuadPart;
        /* 이름·파일 시스템은 부가 정보라 실패해도 용량 결과는 그대로 둔다. */
        if (!GetVolumeInformationW(out->root, out->label, DRIVE_LABEL_LEN, NULL, NULL, NULL,
                                   out->fs, DRIVE_FS_LEN)) {
            out->label[0] = L'\0';
            out->fs[0] = L'\0';
        }
    }

    if (mode_set) {
        SetThreadErrorMode(old_mode, NULL);
    }
}

int drive_list_build(unsigned long mask, DriveQueryFn query, void *ctx, DriveList *out)
{
    int i;

    if (out == NULL || query == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    for (i = 0; i < DRIVE_MAX; i++) {
        DriveInfo *d;

        if ((mask & (1UL << i)) == 0) {
            continue;
        }
        d = &out->items[out->len++];
        drive_root_from_letter((wchar_t)(L'A' + i), d->root, DRIVE_ROOT_LEN);
        query(d, ctx);
        /* 조회 함수가 루트를 덮어쓰더라도 목록 순서와 맞게 되돌린다. */
        drive_root_from_letter((wchar_t)(L'A' + i), d->root, DRIVE_ROOT_LEN);
        if (d->status != DRIVE_STATUS_OK) {
            d->total_bytes = 0;
            d->free_bytes = 0;
            d->available_bytes = 0;
        }
    }
    return 0;
}

int drive_list_query(DriveList *out, unsigned long *error)
{
    DWORD mask;

    if (error != NULL) {
        *error = 0;
    }
    if (out == NULL) {
        return -1;
    }
    SetLastError(ERROR_SUCCESS);
    mask = GetLogicalDrives();
    if (mask == 0) {
        DWORD code = GetLastError();
        memset(out, 0, sizeof(*out));
        if (code != ERROR_SUCCESS) {
            if (error != NULL) {
                *error = code;
            }
            return -1;
        }
        /* 드라이브가 정말 하나도 없는 경우. */
        return 0;
    }
    return drive_list_build(mask, drive_query_system, NULL, out);
}

uint64_t drive_used_bytes(const DriveInfo *d)
{
    if (d == NULL || d->status != DRIVE_STATUS_OK || d->free_bytes >= d->total_bytes) {
        return 0;
    }
    return d->total_bytes - d->free_bytes;
}

unsigned drive_usage_permille(const DriveInfo *d)
{
    uint64_t used = drive_used_bytes(d);
    uint64_t total;

    if (used == 0) {
        return 0;
    }
    total = d->total_bytes;
    /* used * 1000 이 넘치지 않도록 큰 값은 함께 줄인다(정밀도는 천분율이면 충분). */
    while (used > UINT64_MAX / 1000) {
        used >>= 10;
        total >>= 10;
    }
    if (total == 0) {
        return 0;
    }
    used = used * 1000 / total;
    return used > 1000 ? 1000 : (unsigned)used;
}

const wchar_t *drive_type_name(DriveType type)
{
    switch (type) {
    case DRIVE_TYPE_NO_ROOT:
        return L"no_root";
    case DRIVE_TYPE_REMOVABLE:
        return L"removable";
    case DRIVE_TYPE_FIXED:
        return L"fixed";
    case DRIVE_TYPE_REMOTE:
        return L"remote";
    case DRIVE_TYPE_CDROM:
        return L"cdrom";
    case DRIVE_TYPE_RAMDISK:
        return L"ramdisk";
    default:
        return L"unknown";
    }
}

const wchar_t *drive_status_name(DriveStatus status)
{
    switch (status) {
    case DRIVE_STATUS_OK:
        return L"ok";
    case DRIVE_STATUS_NOT_READY:
        return L"not_ready";
    case DRIVE_STATUS_ACCESS_DENIED:
        return L"access_denied";
    case DRIVE_STATUS_MISSING:
        return L"missing";
    case DRIVE_STATUS_DISCONNECTED:
        return L"disconnected";
    default:
        return L"error";
    }
}
