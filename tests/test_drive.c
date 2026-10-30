#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "drive.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static DriveInfo ok_drive(uint64_t total, uint64_t free_bytes)
{
    DriveInfo d;

    memset(&d, 0, sizeof(d));
    d.status = DRIVE_STATUS_OK;
    d.total_bytes = total;
    d.free_bytes = free_bytes;
    d.available_bytes = free_bytes;
    return d;
}

static void test_root_from_letter(void)
{
    wchar_t buf[DRIVE_ROOT_LEN];

    CHECK(drive_root_from_letter(L'C', buf, DRIVE_ROOT_LEN) == 0);
    CHECK(wcscmp(buf, L"C:\\") == 0);
    CHECK(drive_root_from_letter(L'z', buf, DRIVE_ROOT_LEN) == 0);
    CHECK(wcscmp(buf, L"Z:\\") == 0);
    CHECK(drive_root_from_letter(L'1', buf, DRIVE_ROOT_LEN) == -1);
    CHECK(drive_root_from_letter(L'C', buf, DRIVE_ROOT_LEN - 1) == -1);
    CHECK(drive_root_from_letter(L'C', NULL, DRIVE_ROOT_LEN) == -1);
}

static void test_status_from_error(void)
{
    CHECK(drive_status_from_error(ERROR_SUCCESS) == DRIVE_STATUS_OK);
    /* 미디어가 없는 카드 리더·광학 드라이브 */
    CHECK(drive_status_from_error(ERROR_NOT_READY) == DRIVE_STATUS_NOT_READY);
    CHECK(drive_status_from_error(ERROR_NO_MEDIA_IN_DRIVE) == DRIVE_STATUS_NOT_READY);
    CHECK(drive_status_from_error(ERROR_UNRECOGNIZED_VOLUME) == DRIVE_STATUS_NOT_READY);
    CHECK(drive_status_from_error(ERROR_ACCESS_DENIED) == DRIVE_STATUS_ACCESS_DENIED);
    /* 목록을 얻은 뒤 뽑힌 USB */
    CHECK(drive_status_from_error(ERROR_PATH_NOT_FOUND) == DRIVE_STATUS_MISSING);
    CHECK(drive_status_from_error(ERROR_INVALID_DRIVE) == DRIVE_STATUS_MISSING);
    CHECK(drive_status_from_error(ERROR_DEVICE_NOT_CONNECTED) == DRIVE_STATUS_MISSING);
    CHECK(drive_status_from_error(ERROR_BAD_NETPATH) == DRIVE_STATUS_DISCONNECTED);
    CHECK(drive_status_from_error(ERROR_NETNAME_DELETED) == DRIVE_STATUS_DISCONNECTED);
    CHECK(drive_status_from_error(ERROR_GEN_FAILURE) == DRIVE_STATUS_ERROR);
}

static void test_type_from_win32(void)
{
    CHECK(drive_type_from_win32(DRIVE_FIXED) == DRIVE_TYPE_FIXED);
    CHECK(drive_type_from_win32(DRIVE_REMOVABLE) == DRIVE_TYPE_REMOVABLE);
    CHECK(drive_type_from_win32(DRIVE_REMOTE) == DRIVE_TYPE_REMOTE);
    CHECK(drive_type_from_win32(DRIVE_CDROM) == DRIVE_TYPE_CDROM);
    CHECK(drive_type_from_win32(DRIVE_RAMDISK) == DRIVE_TYPE_RAMDISK);
    CHECK(drive_type_from_win32(DRIVE_NO_ROOT_DIR) == DRIVE_TYPE_NO_ROOT);
    CHECK(drive_type_from_win32(DRIVE_UNKNOWN) == DRIVE_TYPE_UNKNOWN);
    CHECK(drive_type_from_win32(99) == DRIVE_TYPE_UNKNOWN);
}

static void test_usage(void)
{
    DriveInfo d;

    d = ok_drive(1000, 250);
    CHECK(drive_used_bytes(&d) == 750);
    CHECK(drive_usage_permille(&d) == 750);

    d = ok_drive(3, 2); /* 333.3‰ → 내림 */
    CHECK(drive_usage_permille(&d) == 333);

    d = ok_drive(1000, 0);
    CHECK(drive_usage_permille(&d) == 1000);

    d = ok_drive(1000, 1000);
    CHECK(drive_used_bytes(&d) == 0);
    CHECK(drive_usage_permille(&d) == 0);

    /* 전체가 0 이면 0 으로 나누지 않는다. */
    d = ok_drive(0, 0);
    CHECK(drive_usage_permille(&d) == 0);

    /* 여유가 전체보다 크게 보고돼도 음수(랩어라운드)가 되지 않는다. */
    d = ok_drive(100, 200);
    CHECK(drive_used_bytes(&d) == 0);
    CHECK(drive_usage_permille(&d) == 0);

    /* used * 1000 이 uint64 를 넘는 큰 값 */
    d = ok_drive(UINT64_MAX, UINT64_MAX / 4);
    CHECK(drive_usage_permille(&d) == 750);
    d = ok_drive(UINT64_MAX, 0);
    CHECK(drive_usage_permille(&d) == 1000);

    /* 조회에 실패한 드라이브 */
    d = ok_drive(1000, 250);
    d.status = DRIVE_STATUS_NOT_READY;
    CHECK(drive_used_bytes(&d) == 0);
    CHECK(drive_usage_permille(&d) == 0);

    CHECK(drive_used_bytes(NULL) == 0);
    CHECK(drive_usage_permille(NULL) == 0);
}

static void test_names(void)
{
    CHECK(wcscmp(drive_type_name(DRIVE_TYPE_FIXED), L"fixed") == 0);
    CHECK(wcscmp(drive_type_name(DRIVE_TYPE_REMOVABLE), L"removable") == 0);
    CHECK(wcscmp(drive_type_name((DriveType)99), L"unknown") == 0);
    CHECK(wcscmp(drive_status_name(DRIVE_STATUS_OK), L"ok") == 0);
    CHECK(wcscmp(drive_status_name(DRIVE_STATUS_NOT_READY), L"not_ready") == 0);
    CHECK(wcscmp(drive_status_name((DriveStatus)99), L"error") == 0);
}

/* 가짜 조회: C: 는 정상, E: 는 미디어 없는 이동식, F: 는 조회 도중 분리,
 * G: 는 실패를 보고하면서 용량 값을 남기는 잘못된 구현. */
typedef struct {
    int calls;
    wchar_t order[DRIVE_MAX + 1];
} FakeCtx;

static void fake_query(DriveInfo *out, void *ctx)
{
    FakeCtx *fc = ctx;

    fc->order[fc->calls++] = out->root[0];
    switch (out->root[0]) {
    case L'C':
        out->type = DRIVE_TYPE_FIXED;
        out->status = DRIVE_STATUS_OK;
        out->total_bytes = 1000;
        out->free_bytes = 400;
        out->available_bytes = 300;
        wcscpy_s(out->label, DRIVE_LABEL_LEN, L"Windows");
        wcscpy_s(out->fs, DRIVE_FS_LEN, L"NTFS");
        break;
    case L'E':
        out->type = DRIVE_TYPE_REMOVABLE;
        out->status = drive_status_from_error(ERROR_NOT_READY);
        out->error = ERROR_NOT_READY;
        break;
    case L'F':
        out->type = DRIVE_TYPE_NO_ROOT;
        out->status = drive_status_from_error(ERROR_PATH_NOT_FOUND);
        out->error = ERROR_PATH_NOT_FOUND;
        break;
    default:
        out->type = DRIVE_TYPE_REMOVABLE;
        out->status = DRIVE_STATUS_ERROR;
        out->error = ERROR_GEN_FAILURE;
        out->total_bytes = 123;
        out->free_bytes = 45;
        out->root[0] = L'?'; /* 루트를 망가뜨려도 목록은 되돌린다 */
        break;
    }
}

static void test_list_build(void)
{
    DriveList list;
    FakeCtx fc;
    /* C:, E:, F:, G: */
    unsigned long mask = (1UL << 2) | (1UL << 4) | (1UL << 5) | (1UL << 6);

    memset(&fc, 0, sizeof(fc));
    CHECK(drive_list_build(mask, fake_query, &fc, &list) == 0);
    CHECK(list.len == 4);
    CHECK(fc.calls == 4);
    CHECK(wcscmp(fc.order, L"CEFG") == 0);

    CHECK(wcscmp(list.items[0].root, L"C:\\") == 0);
    CHECK(list.items[0].status == DRIVE_STATUS_OK);
    CHECK(list.items[0].total_bytes == 1000);
    CHECK(drive_used_bytes(&list.items[0]) == 600);
    CHECK(drive_usage_permille(&list.items[0]) == 600);
    CHECK(wcscmp(list.items[0].label, L"Windows") == 0);

    /* 이동식 드라이브 오류가 있어도 나머지 목록은 정상이고, 실패 항목은 용량 0. */
    CHECK(wcscmp(list.items[1].root, L"E:\\") == 0);
    CHECK(list.items[1].status == DRIVE_STATUS_NOT_READY);
    CHECK(list.items[1].error == ERROR_NOT_READY);
    CHECK(list.items[1].total_bytes == 0);

    CHECK(list.items[2].status == DRIVE_STATUS_MISSING);

    CHECK(wcscmp(list.items[3].root, L"G:\\") == 0);
    CHECK(list.items[3].status == DRIVE_STATUS_ERROR);
    CHECK(list.items[3].total_bytes == 0 && list.items[3].free_bytes == 0);

    /* 빈 마스크, 26번째 비트(Z:) 너머는 무시 */
    memset(&fc, 0, sizeof(fc));
    CHECK(drive_list_build(0, fake_query, &fc, &list) == 0);
    CHECK(list.len == 0 && fc.calls == 0);

    memset(&fc, 0, sizeof(fc));
    CHECK(drive_list_build(0xFFFFFFFFUL, fake_query, &fc, &list) == 0);
    CHECK(list.len == DRIVE_MAX);
    CHECK(wcscmp(list.items[0].root, L"A:\\") == 0);
    CHECK(wcscmp(list.items[DRIVE_MAX - 1].root, L"Z:\\") == 0);

    CHECK(drive_list_build(mask, NULL, NULL, &list) == -1);
    CHECK(drive_list_build(mask, fake_query, &fc, NULL) == -1);
}

/* 존재하지 않는 드라이브 문자를 실제로 조회하면 실패 상태가 되고 프로그램은 계속된다. */
static void test_query_missing(void)
{
    DWORD mask = GetLogicalDrives();
    DriveInfo d;
    int i;

    for (i = DRIVE_MAX - 1; i >= 0; i--) {
        if ((mask & (1UL << i)) == 0) {
            break;
        }
    }
    if (i < 0) {
        return; /* 모든 문자가 사용 중 */
    }
    memset(&d, 0, sizeof(d));
    drive_root_from_letter((wchar_t)(L'A' + i), d.root, DRIVE_ROOT_LEN);
    drive_query_system(&d, NULL);
    CHECK(d.status != DRIVE_STATUS_OK);
    CHECK(d.error != 0);
    CHECK(d.total_bytes == 0 && d.free_bytes == 0 && d.available_bytes == 0);
}

/* 실제 시스템: Windows 가 설치된 드라이브는 반드시 정상 조회돼야 한다. */
static void test_query_system(void)
{
    DriveList list;
    unsigned long error = 12345;
    wchar_t windir[MAX_PATH];
    int found = 0;
    size_t i;

    CHECK(drive_list_query(&list, &error) == 0);
    CHECK(error == 0);
    CHECK(list.len > 0);
    CHECK(GetWindowsDirectoryW(windir, MAX_PATH) > 0);

    for (i = 0; i < list.len; i++) {
        const DriveInfo *d = &list.items[i];
        if (d->status == DRIVE_STATUS_OK) {
            CHECK(d->free_bytes <= d->total_bytes);
            CHECK(d->available_bytes <= d->total_bytes);
            CHECK(drive_used_bytes(d) + d->free_bytes == d->total_bytes);
        } else {
            CHECK(d->error != 0);
            CHECK(d->total_bytes == 0);
        }
        if (towupper(windir[0]) == d->root[0]) {
            found = 1;
            CHECK(d->type == DRIVE_TYPE_FIXED);
            CHECK(d->status == DRIVE_STATUS_OK);
            CHECK(d->total_bytes > 0);
            CHECK(d->fs[0] != L'\0');
        }
        printf("%ls %-9ls %-13ls total=%llu free=%llu usage=%u.%u%%\n", d->root,
               drive_type_name(d->type), drive_status_name(d->status),
               (unsigned long long)d->total_bytes, (unsigned long long)d->free_bytes,
               drive_usage_permille(d) / 10, drive_usage_permille(d) % 10);
    }
    CHECK(found);
    CHECK(drive_list_query(NULL, &error) == -1);
}

int main(void)
{
    test_root_from_letter();
    test_status_from_error();
    test_type_from_win32();
    test_usage();
    test_names();
    test_list_build();
    test_query_missing();
    test_query_system();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all drive tests passed\n");
    return 0;
}
