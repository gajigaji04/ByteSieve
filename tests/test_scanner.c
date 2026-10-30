#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "path.h"
#include "scanner.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

#define CHECK_U64(actual, expected)                                     \
    do {                                                                \
        unsigned long long a_ = (actual), e_ = (expected);              \
        if (a_ != e_) {                                                 \
            fprintf(stderr, "%s:%d: %s: expected %llu, got %llu\n",     \
                    __FILE__, __LINE__, #actual, e_, a_);               \
            g_failures++;                                               \
        }                                                               \
    } while (0)

/* 4 GiB 를 넘어 nFileSizeHigh 가 필요한 크기. */
#define BIG_SIZE ((5ULL << 30) + 3ULL)

static wchar_t g_root[MAX_PATH];

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

static int write_file(const wchar_t *name, const char *data, DWORD len)
{
    wchar_t *p = fixture(name);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok;

    free(p);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = len == 0 || WriteFile(h, data, len, &written, NULL);
    CloseHandle(h);
    return ok && written == len;
}

/* 디스크를 쓰지 않도록 희소 파일로 큰 파일을 만든다. */
static int make_sparse_file(const wchar_t *name, unsigned long long size)
{
    wchar_t *p = fixture(name);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD ret;
    LARGE_INTEGER pos;
    BOOL ok;

    free(p);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    pos.QuadPart = (LONGLONG)size;
    ok = DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL) &&
         SetFilePointerEx(h, pos, NULL, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
}

static int make_dir(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    BOOL ok = CreateDirectoryW(p, NULL);
    free(p);
    return ok;
}

static int make_junction(const wchar_t *link, const wchar_t *target)
{
    wchar_t cmd[3 * MAX_PATH];
    swprintf(cmd, sizeof(cmd) / sizeof(cmd[0]),
             L"cmd /c mklink /J \"%ls\\%ls\" \"%ls\\%ls\" >nul",
             g_root, link, g_root, target);
    return _wsystem(cmd) == 0;
}

/* 모든 사용자에게 모든 접근을 거부하는 DACL 을 설정(deny=0 이면 제한 해제). */
static int set_deny_all(const wchar_t *name, int deny)
{
    wchar_t *p = fixture(name);
    SECURITY_DESCRIPTOR sd;
    BYTE sid_buf[SECURITY_MAX_SID_SIZE];
    DWORD sid_len = sizeof(sid_buf);
    BYTE acl_buf[256];
    PACL acl = (PACL)acl_buf;
    BOOL ok;

    ok = InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    if (ok && deny) {
        ok = CreateWellKnownSid(WinWorldSid, NULL, sid_buf, &sid_len) &&
             InitializeAcl(acl, sizeof(acl_buf), ACL_REVISION) &&
             AddAccessDeniedAce(acl, ACL_REVISION, GENERIC_ALL, sid_buf) &&
             SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE);
    } else if (ok) {
        ok = SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    }
    ok = ok && SetFileSecurityW(p, DACL_SECURITY_INFORMATION, &sd);
    free(p);
    return ok;
}

static void remove_tree(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = path_join(dir, L"*");
    HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;

    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            child = path_join(dir, fd.cFileName);
            if (child == NULL) {
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                /* 정션은 대상 내용을 지우지 않도록 링크 자체만 삭제한다. */
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    remove_tree(child);
                }
                RemoveDirectoryW(child);
            } else {
                DeleteFileW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static int setup_fixture(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_scan_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    if (!CreateDirectoryW(g_root, NULL)) {
        return 0;
    }

    /*
     * root\
     *   a.txt          5 B
     *   한글.bin        0 B
     *   big.dat        5 GiB + 3 B (희소)
     *   sub\inner.txt  (비재귀이므로 집계 제외)
     *   link\  -> sub  (정션, 하위 탐색 제외)
     *   denied\        (접근 거부)
     */
    return write_file(L"a.txt", "hello", 5) &&
           write_file(L"한글.bin", NULL, 0) &&
           make_sparse_file(L"big.dat", BIG_SIZE) &&
           make_dir(L"sub") &&
           write_file(L"sub\\inner.txt", "ignored", 7) &&
           make_junction(L"link", L"sub") &&
           make_dir(L"denied") &&
           set_deny_all(L"denied", 1);
}

static void teardown_fixture(void)
{
    set_deny_all(L"denied", 0);
    remove_tree(g_root);
}

static void test_combine_size(void)
{
    CHECK_U64(scan_combine_size(0, 0), 0);
    CHECK_U64(scan_combine_size(0, 0xFFFFFFFFu), 0xFFFFFFFFULL);
    CHECK_U64(scan_combine_size(1, 0), 0x100000000ULL);
    CHECK_U64(scan_combine_size(1, 0x40000003u), BIG_SIZE);
    CHECK_U64(scan_combine_size(0xFFFFFFFFu, 0xFFFFFFFFu), 0xFFFFFFFFFFFFFFFFULL);
}

static void test_should_descend(void)
{
    CHECK(scan_should_descend(FILE_ATTRIBUTE_DIRECTORY));
    CHECK(!scan_should_descend(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    CHECK(!scan_should_descend(FILE_ATTRIBUTE_NORMAL));
    CHECK(!scan_should_descend(FILE_ATTRIBUTE_REPARSE_POINT));
}

typedef struct {
    int seen_sub, seen_link, seen_big, seen_dot, seen_inner;
} VisitLog;

static void visit(const ScanEntry *e, void *ctx)
{
    VisitLog *log = ctx;

    if (wcscmp(e->name, L".") == 0 || wcscmp(e->name, L"..") == 0) {
        log->seen_dot++;
    } else if (wcscmp(e->name, L"sub") == 0) {
        log->seen_sub++;
        CHECK(e->is_dir && !e->is_reparse && e->descend);
    } else if (wcscmp(e->name, L"link") == 0) {
        log->seen_link++;
        CHECK(e->is_dir && e->is_reparse && !e->descend);
    } else if (wcscmp(e->name, L"big.dat") == 0) {
        log->seen_big++;
        CHECK(!e->is_dir && !e->descend);
        CHECK_U64(e->size, BIG_SIZE);
    } else if (wcscmp(e->name, L"inner.txt") == 0) {
        log->seen_inner++;
    }
}

static void test_scan_fixture(void)
{
    ScanReport r;
    VisitLog log = {0};

    scan_report_init(&r);
    CHECK(scan_directory(g_root, &r, visit, &log) == SCAN_COMPLETE);
    CHECK_U64(r.file_count, 3);
    CHECK_U64(r.dir_count, 3);
    CHECK_U64(r.total_bytes, 5 + 0 + BIG_SIZE);
    CHECK_U64(r.reparse_count, 1);
    CHECK_U64(r.error_count, 0);
    CHECK(log.seen_sub == 1 && log.seen_link == 1 && log.seen_big == 1);
    CHECK(log.seen_dot == 0 && log.seen_inner == 0);
    scan_report_free(&r);
}

static void test_access_denied_continues(void)
{
    ScanReport r;
    wchar_t *denied = fixture(L"denied");

    scan_report_init(&r);
    CHECK(scan_directory(denied, &r, NULL, NULL) == SCAN_FAILED);
    CHECK_U64(r.error_count, 1);
    CHECK(r.errors_len == 1);
    if (r.errors_len == 1) {
        CHECK(r.errors[0].code == ERROR_ACCESS_DENIED);
        CHECK(r.errors[0].path != NULL && wcscmp(r.errors[0].path, denied) == 0);
    }

    /* 오류 후에도 같은 보고서로 스캔을 계속할 수 있고 집계가 누적된다. */
    CHECK(scan_directory(g_root, &r, NULL, NULL) == SCAN_COMPLETE);
    CHECK_U64(r.file_count, 3);
    CHECK_U64(r.dir_count, 3);
    CHECK_U64(r.error_count, 1);

    scan_report_free(&r);
    CHECK(r.errors == NULL && r.error_count == 0);
    free(denied);
}

static void test_invalid_targets(void)
{
    ScanReport r;
    wchar_t *missing = fixture(L"does-not-exist");
    wchar_t *file = fixture(L"a.txt");
    wchar_t *sub = fixture(L"sub");

    scan_report_init(&r);
    CHECK(scan_directory(missing, &r, NULL, NULL) == SCAN_FAILED);
    CHECK(scan_directory(file, &r, NULL, NULL) == SCAN_FAILED);
    CHECK(scan_directory(L"", &r, NULL, NULL) == SCAN_FAILED);
    CHECK(scan_directory(NULL, &r, NULL, NULL) == SCAN_FAILED);
    CHECK(scan_directory(sub, NULL, NULL, NULL) == SCAN_FAILED);
    CHECK_U64(r.error_count, 4);
    if (r.errors_len == 4) {
        CHECK(r.errors[0].code == ERROR_FILE_NOT_FOUND ||
              r.errors[0].code == ERROR_PATH_NOT_FOUND);
        CHECK(r.errors[1].code == ERROR_DIRECTORY);
        CHECK(r.errors[2].code == ERROR_INVALID_PARAMETER);
    }
    CHECK_U64(r.file_count, 0);

    /* 하위 디렉터리 단독 스캔(상대 경로 아님, 확장 경로 입력). */
    {
        wchar_t *ext = path_to_extended(sub);
        CHECK(scan_directory(ext, &r, NULL, NULL) == SCAN_COMPLETE);
        CHECK_U64(r.file_count, 1);
        CHECK_U64(r.total_bytes, 7);
        free(ext);
    }

    scan_report_free(&r);
    free(missing);
    free(file);
    free(sub);
}

int main(void)
{
    test_combine_size();
    test_should_descend();

    if (!setup_fixture()) {
        fprintf(stderr, "fixture setup failed (error %lu)\n", GetLastError());
        teardown_fixture();
        return 1;
    }
    test_scan_fixture();
    test_access_denied_continues();
    test_invalid_targets();
    teardown_fixture();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all scanner tests passed\n");
    return 0;
}
