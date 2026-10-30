#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static int deny_path(const wchar_t *p, int deny)
{
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
    return ok && SetFileSecurityW(p, DACL_SECURITY_INFORMATION, &sd);
}

static int set_deny_all(const wchar_t *name, int deny)
{
    wchar_t *p = fixture(name);
    int ok = p != NULL && deny_path(p, deny);
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
     *   sub\inner.txt          7 B (깊이 1)
     *   sub\d1\mid.txt         2 B (깊이 2)
     *   sub\d1\d2\leaf.txt     3 B (깊이 3)
     *   link\  -> sub  (정션, 하위 탐색 제외)
     *   denied\        (접근 거부)
     */
    return write_file(L"a.txt", "hello", 5) &&
           write_file(L"한글.bin", NULL, 0) &&
           make_sparse_file(L"big.dat", BIG_SIZE) &&
           make_dir(L"sub") &&
           write_file(L"sub\\inner.txt", "ignored", 7) &&
           make_dir(L"sub\\d1") &&
           write_file(L"sub\\d1\\mid.txt", "ab", 2) &&
           make_dir(L"sub\\d1\\d2") &&
           write_file(L"sub\\d1\\d2\\leaf.txt", "xyz", 3) &&
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
    CHECK(scan_directory(denied, &r, NULL, NULL) == SCAN_FATAL);
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
    CHECK(scan_directory(missing, &r, NULL, NULL) == SCAN_FATAL);
    CHECK(scan_directory(file, &r, NULL, NULL) == SCAN_FATAL);
    CHECK(scan_directory(L"", &r, NULL, NULL) == SCAN_FATAL);
    CHECK(scan_directory(NULL, &r, NULL, NULL) == SCAN_FATAL);
    CHECK(scan_directory(sub, NULL, NULL, NULL) == SCAN_FATAL);
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

static void test_tree_unlimited(void)
{
    ScanReport r;
    VisitLog log = {0};

    /* denied 는 열지 못해 부분 성공, link 는 따라가지 않아 inner.txt 는 한 번만. */
    scan_report_init(&r);
    CHECK(scan_tree(g_root, SCAN_DEPTH_UNLIMITED, &r, visit, &log) == SCAN_PARTIAL);
    CHECK_U64(r.file_count, 6);
    CHECK_U64(r.dir_count, 5);
    CHECK_U64(r.total_bytes, 5 + 0 + BIG_SIZE + 7 + 2 + 3);
    CHECK_U64(r.reparse_count, 1);
    CHECK_U64(r.error_count, 1);
    if (r.errors_len == 1) {
        wchar_t *denied = fixture(L"denied");
        CHECK(r.errors[0].code == ERROR_ACCESS_DENIED);
        CHECK(r.errors[0].path != NULL && wcscmp(r.errors[0].path, denied) == 0);
        free(denied);
    }
    CHECK(log.seen_sub == 1 && log.seen_link == 1 && log.seen_inner == 1);
    CHECK(log.seen_dot == 0);
    scan_report_free(&r);
}

static void test_tree_max_depth(void)
{
    static const struct {
        uint32_t depth;
        unsigned long long files, dirs, bytes;
    } cases[] = {
        {0, 3, 3, 5 + BIG_SIZE},
        {1, 4, 4, 5 + BIG_SIZE + 7},
        {2, 5, 5, 5 + BIG_SIZE + 7 + 2},
        {3, 6, 5, 5 + BIG_SIZE + 7 + 2 + 3},
        {100, 6, 5, 5 + BIG_SIZE + 7 + 2 + 3},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ScanReport r;
        ScanStatus st;

        scan_report_init(&r);
        st = scan_tree(g_root, cases[i].depth, &r, NULL, NULL);
        /* 깊이 0 에서는 denied 를 열지 않으므로 오류가 없다. */
        CHECK(st == (cases[i].depth == 0 ? SCAN_COMPLETE : SCAN_PARTIAL));
        CHECK_U64(r.file_count, cases[i].files);
        CHECK_U64(r.dir_count, cases[i].dirs);
        CHECK_U64(r.total_bytes, cases[i].bytes);
        scan_report_free(&r);
    }

    /* 오류 없는 하위 트리는 SCAN_COMPLETE. */
    {
        ScanReport r;
        wchar_t *sub = fixture(L"sub");

        scan_report_init(&r);
        CHECK(scan_tree(sub, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_COMPLETE);
        CHECK_U64(r.file_count, 3);
        CHECK_U64(r.dir_count, 2);
        CHECK_U64(r.total_bytes, 12);
        CHECK_U64(r.error_count, 0);
        scan_report_free(&r);
        free(sub);
    }
}

static void test_tree_invalid_root(void)
{
    ScanReport r;
    wchar_t *missing = fixture(L"does-not-exist");
    wchar_t *denied = fixture(L"denied");

    scan_report_init(&r);
    CHECK(scan_tree(missing, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_FATAL);
    CHECK(scan_tree(denied, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_FATAL);
    CHECK(scan_tree(NULL, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_FATAL);
    CHECK(scan_tree(g_root, SCAN_DEPTH_UNLIMITED, NULL, NULL, NULL) == SCAN_FATAL);
    CHECK_U64(r.error_count, 3);
    CHECK_U64(r.file_count, 0);
    scan_report_free(&r);
    free(missing);
    free(denied);
}

/* MAX_PATH 를 넘는 깊은 체인: 동적 스택과 긴 경로로 끝까지 내려가야 한다. */
static void test_tree_deep_chain(void)
{
    enum { LEVELS = 200 };
    wchar_t base[MAX_PATH + 8];
    wchar_t *root, *cur;
    int i, ok = 1;

    swprintf(base, sizeof(base) / sizeof(base[0]), L"%ls_deep", g_root);
    root = path_to_extended(base);
    CHECK(root != NULL);
    if (root == NULL) {
        return;
    }
    remove_tree(root);
    cur = path_dup(root);
    ok = cur != NULL && CreateDirectoryW(cur, NULL);
    for (i = 0; ok && i < LEVELS; i++) {
        wchar_t *next = path_join(cur, L"d");
        free(cur);
        cur = next;
        ok = cur != NULL && CreateDirectoryW(cur, NULL);
    }
    if (ok) {
        wchar_t *leaf = path_join(cur, L"leaf.txt");
        HANDLE h = leaf ? CreateFileW(leaf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, NULL)
                        : INVALID_HANDLE_VALUE;
        ok = h != INVALID_HANDLE_VALUE;
        if (ok) {
            CloseHandle(h);
        }
        free(leaf);
    }
    CHECK(ok);
    CHECK(cur != NULL && wcslen(cur) > MAX_PATH);
    free(cur);

    if (ok) {
        ScanReport r;

        scan_report_init(&r);
        CHECK(scan_tree(base, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_COMPLETE);
        CHECK_U64(r.dir_count, LEVELS);
        CHECK_U64(r.file_count, 1);
        CHECK_U64(r.error_count, 0);
        scan_report_free(&r);

        scan_report_init(&r);
        CHECK(scan_tree(base, 49, &r, NULL, NULL) == SCAN_COMPLETE);
        CHECK_U64(r.dir_count, 50);
        CHECK_U64(r.file_count, 0);
        scan_report_free(&r);
    }

    remove_tree(root);
    free(root);
}

static void test_classify_error(void)
{
    CHECK(scan_classify_error(ERROR_ACCESS_DENIED) == SCAN_ERR_ACCESS_DENIED);
    CHECK(scan_classify_error(ERROR_SHARING_VIOLATION) == SCAN_ERR_ACCESS_DENIED);
    CHECK(scan_classify_error(ERROR_FILE_NOT_FOUND) == SCAN_ERR_NOT_FOUND);
    CHECK(scan_classify_error(ERROR_PATH_NOT_FOUND) == SCAN_ERR_NOT_FOUND);
    CHECK(scan_classify_error(ERROR_NOT_READY) == SCAN_ERR_NOT_FOUND);
    CHECK(scan_classify_error(ERROR_BAD_NETPATH) == SCAN_ERR_NOT_FOUND);
    CHECK(scan_classify_error(ERROR_INVALID_NAME) == SCAN_ERR_INVALID_PATH);
    CHECK(scan_classify_error(ERROR_DIRECTORY) == SCAN_ERR_INVALID_PATH);
    CHECK(scan_classify_error(ERROR_INVALID_PARAMETER) == SCAN_ERR_INVALID_PATH);
    CHECK(scan_classify_error(ERROR_FILENAME_EXCED_RANGE) == SCAN_ERR_PATH_TOO_LONG);
    CHECK(scan_classify_error(ERROR_NOT_ENOUGH_MEMORY) == SCAN_ERR_NO_MEMORY);
    CHECK(scan_classify_error(ERROR_CRC) == SCAN_ERR_OTHER);

    CHECK(wcscmp(scan_status_name(SCAN_COMPLETE), L"complete") == 0);
    CHECK(wcscmp(scan_status_name(SCAN_PARTIAL), L"partial") == 0);
    CHECK(wcscmp(scan_status_name(SCAN_FATAL), L"fatal") == 0);
    CHECK(wcscmp(scan_error_kind_name(SCAN_ERR_ACCESS_DENIED), L"access_denied") == 0);
    CHECK(wcscmp(scan_error_kind_name(SCAN_ERR_OTHER), L"other") == 0);
}

/* 정상 스캔은 COMPLETE, 오류·건너뜀·중단 기록이 모두 0. */
static void test_status_complete(void)
{
    ScanReport r;
    wchar_t *sub = fixture(L"sub");

    scan_report_init(&r);
    CHECK(scan_tree(sub, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_COMPLETE);
    CHECK_U64(r.error_count, 0);
    CHECK_U64(r.skipped_dir_count, 0);
    CHECK(r.aborted == 0);
    scan_report_free(&r);
    free(sub);
}

/*
 * 여러 하위 디렉터리가 실패해도 나머지는 끝까지 탐색한다.
 *   E\a\f1.txt  1 B
 *   E\b\        접근 거부(깊이 1)
 *   E\c\x\      접근 거부(깊이 2)
 *   E\c\y.txt   2 B
 *   E\d.txt     3 B
 */
static void test_status_partial_continues(void)
{
    static const wchar_t *dirs[] = {L"a", L"b", L"c", L"c\\x"};
    static const struct {
        const wchar_t *name;
        const char *data;
    } files[] = {{L"a\\f1.txt", "x"}, {L"c\\y.txt", "yy"}, {L"d.txt", "ddd"}};
    wchar_t root[MAX_PATH + 8];
    wchar_t *b = NULL, *cx = NULL;
    ScanReport r;
    size_t i;
    int ok;

    swprintf(root, sizeof(root) / sizeof(root[0]), L"%ls_err", g_root);
    remove_tree(root);
    ok = CreateDirectoryW(root, NULL);
    for (i = 0; ok && i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        wchar_t *p = path_join(root, dirs[i]);
        ok = p != NULL && CreateDirectoryW(p, NULL);
        free(p);
    }
    for (i = 0; ok && i < sizeof(files) / sizeof(files[0]); i++) {
        wchar_t *p = path_join(root, files[i].name);
        HANDLE h = p ? CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, NULL)
                     : INVALID_HANDLE_VALUE;
        DWORD len = (DWORD)strlen(files[i].data), written = 0;
        ok = h != INVALID_HANDLE_VALUE &&
             WriteFile(h, files[i].data, len, &written, NULL) && written == len;
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
        free(p);
    }
    b = path_join(root, L"b");
    cx = path_join(root, L"c\\x");
    ok = ok && b && cx && deny_path(b, 1) && deny_path(cx, 1);
    CHECK(ok);

    if (ok) {
        int seen_b = 0, seen_cx = 0;

        scan_report_init(&r);
        CHECK(scan_tree(root, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_PARTIAL);
        CHECK_U64(r.file_count, 3);
        CHECK_U64(r.dir_count, 4);
        CHECK_U64(r.total_bytes, 6);
        CHECK_U64(r.error_count, 2);
        CHECK_U64(r.skipped_dir_count, 2);
        CHECK(r.aborted == 0);
        for (i = 0; i < r.errors_len; i++) {
            CHECK(r.errors[i].code == ERROR_ACCESS_DENIED);
            CHECK(r.errors[i].kind == SCAN_ERR_ACCESS_DENIED);
            if (r.errors[i].path != NULL) {
                seen_b += wcscmp(r.errors[i].path, b) == 0;
                seen_cx += wcscmp(r.errors[i].path, cx) == 0;
            }
        }
        CHECK(seen_b == 1 && seen_cx == 1);
        scan_report_free(&r);
    }

    if (b) {
        deny_path(b, 0);
    }
    if (cx) {
        deny_path(cx, 0);
    }
    remove_tree(root);
    free(b);
    free(cx);
}

/* 대상 자체의 오류는 FATAL 이며 중단 여부와 오류 분류를 기록한다. */
static void test_status_fatal(void)
{
    enum { LONG_LEN = PATH_MAX_WIDE + 16 };
    ScanReport r;
    wchar_t *missing = fixture(L"does-not-exist");
    wchar_t *bad_name = fixture(L"bad<name>");
    wchar_t *file = fixture(L"a.txt");
    wchar_t *denied = fixture(L"denied");
    wchar_t *too_long = malloc((LONG_LEN + 1) * sizeof(wchar_t));
    size_t i;

    CHECK(too_long != NULL);
    if (too_long == NULL) {
        return;
    }
    too_long[0] = L'C';
    too_long[1] = L':';
    too_long[2] = L'\\';
    for (i = 3; i < LONG_LEN; i++) {
        too_long[i] = L'a';
    }
    too_long[LONG_LEN] = L'\0';

    {
        static const ScanErrorKind expected[] = {
            SCAN_ERR_NOT_FOUND, SCAN_ERR_INVALID_PATH, SCAN_ERR_INVALID_PATH,
            SCAN_ERR_ACCESS_DENIED, SCAN_ERR_PATH_TOO_LONG, SCAN_ERR_INVALID_PATH,
        };
        const wchar_t *targets[] = {missing, bad_name, file, denied, too_long, L""};

        for (i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
            scan_report_init(&r);
            CHECK(scan_tree(targets[i], SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_FATAL);
            CHECK(r.aborted == 1);
            CHECK_U64(r.error_count, 1);
            CHECK_U64(r.file_count, 0);
            if (r.errors_len == 1 && r.errors[0].kind != expected[i]) {
                fprintf(stderr, "target %u: kind %d (code %lu), expected %d\n",
                        (unsigned)i, (int)r.errors[0].kind,
                        (unsigned long)r.errors[0].code, (int)expected[i]);
                g_failures++;
            }
            scan_report_free(&r);
            CHECK(r.aborted == 0);
        }
    }

    /* 열 수는 있지만 열거가 거부된 루트는 건너뛴 디렉터리로도 센다. */
    scan_report_init(&r);
    CHECK(scan_tree(denied, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL) == SCAN_FATAL);
    CHECK_U64(r.skipped_dir_count, 1);

    /* 중단 기록은 같은 보고서에 이어지는 정상 스캔 뒤에도 남는다. */
    CHECK(scan_tree(g_root, 0, &r, NULL, NULL) == SCAN_COMPLETE);
    CHECK(r.aborted == 1);
    CHECK_U64(r.error_count, 1);
    CHECK_U64(r.file_count, 3);
    scan_report_free(&r);

    free(missing);
    free(bad_name);
    free(file);
    free(denied);
    free(too_long);
}

typedef struct {
    int skip_calls;
    int bad_parent;
    int sub_will_descend;
} SkipLog;

/* sub 와 denied 는 들어가지 않는다. 부모는 항상 깊이 0 의 루트여야 한다. */
static int skip_hook(const ScanEntry *entry, const ScanDirInfo *parent, void *ctx)
{
    SkipLog *log = ctx;

    log->skip_calls++;
    if (parent->depth != 0 || parent->id != 7 || !path_is_extended(parent->path)) {
        log->bad_parent++;
    }
    return wcscmp(entry->name, L"sub") == 0 || wcscmp(entry->name, L"denied") == 0;
}

static uintptr_t skip_on_entry(const ScanEntry *entry, uintptr_t parent, int will_descend,
                               void *ctx)
{
    SkipLog *log = ctx;

    (void)parent;
    if (wcscmp(entry->name, L"sub") == 0) {
        log->sub_will_descend = will_descend;
    }
    return 0;
}

static void test_skip_dir_hook(void)
{
    ScanReport r;
    ScanHooks hooks = {skip_on_entry, NULL, NULL, NULL, skip_hook};
    SkipLog log = {0, 0, -1};

    scan_report_init(&r);
    CHECK(scan_tree_ex(g_root, SCAN_DEPTH_UNLIMITED, 7, &r, &hooks, &log) == SCAN_COMPLETE);
    /* 정션(link)은 원래 하위 탐색 대상이 아니므로 훅을 부르지 않는다. */
    CHECK(log.skip_calls == 2);
    CHECK(log.bad_parent == 0);
    CHECK(log.sub_will_descend == 0);
    CHECK_U64(r.excluded_dir_count, 2);
    CHECK_U64(r.file_count, 3); /* sub 아래 파일은 세지 않는다 */
    CHECK_U64(r.dir_count, 3);
    CHECK_U64(r.error_count, 0); /* 제외는 오류가 아니다(denied 도 열지 않음) */
    CHECK_U64(r.skipped_dir_count, 0);
    scan_report_free(&r);
}

int main(void)
{
    test_combine_size();
    test_should_descend();
    test_classify_error();

    if (!setup_fixture()) {
        fprintf(stderr, "fixture setup failed (error %lu)\n", GetLastError());
        teardown_fixture();
        return 1;
    }
    test_scan_fixture();
    test_access_denied_continues();
    test_invalid_targets();
    test_tree_unlimited();
    test_tree_max_depth();
    test_tree_invalid_root();
    test_tree_deep_chain();
    test_status_complete();
    test_status_partial_continues();
    test_status_fatal();
    test_skip_dir_hook();
    teardown_fixture();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all scanner tests passed\n");
    return 0;
}
