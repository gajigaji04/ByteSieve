#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path.h"
#include "scan_model.h"

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

#define CHECK_NAME(model, index, expected)                              \
    do {                                                                \
        const ScanNode *n_ = scan_model_node((model), (index));         \
        if (n_ == NULL || wcscmp(n_->name, (expected)) != 0) {          \
            fprintf(stderr, "%s:%d: node %u: expected \"%ls\", got \"%ls\"\n", \
                    __FILE__, __LINE__, (unsigned)(index), (expected),  \
                    n_ ? n_->name : L"(null)");                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static wchar_t g_root[MAX_PATH];

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

static int write_file(const wchar_t *name, DWORD len)
{
    wchar_t *p = fixture(name);
    HANDLE h = p ? CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
    char buf[1024];
    DWORD written = 0;
    BOOL ok;

    free(p);
    if (h == INVALID_HANDLE_VALUE || len > sizeof(buf)) {
        return 0;
    }
    memset(buf, 'x', len);
    ok = len == 0 || WriteFile(h, buf, len, &written, NULL);
    CloseHandle(h);
    return ok && written == len;
}

static int make_dir(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    BOOL ok = p != NULL && CreateDirectoryW(p, NULL);
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

static int set_locked(int deny)
{
    wchar_t *p = fixture(L"locked");
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

/*
 * root\
 *   a.txt              5 B
 *   big\b1.bin       100 B
 *   big\deep\d.bin  1000 B
 *   small\s.txt       10 B
 *   empty\
 *   link\ -> big      (정션: 집계만, 하위 탐색 안 함)
 *   locked\           (접근 거부)
 */
static int setup_fixture(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_model_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    if (!CreateDirectoryW(g_root, NULL)) {
        return 0;
    }
    return write_file(L"a.txt", 5) &&
           make_dir(L"big") && write_file(L"big\\b1.bin", 100) &&
           make_dir(L"big\\deep") && write_file(L"big\\deep\\d.bin", 1000) &&
           make_dir(L"small") && write_file(L"small\\s.txt", 10) &&
           make_dir(L"empty") &&
           make_junction(L"link", L"big") &&
           make_dir(L"locked") && set_locked(1);
}

static void teardown_fixture(void)
{
    set_locked(0);
    remove_tree(g_root);
}

/* 이름으로 자식 노드를 찾는다. */
static size_t find_child(const ScanModel *m, size_t parent, const wchar_t *name)
{
    size_t kids[16];
    size_t n = scan_model_children(m, parent, SCAN_ORDER_SCAN, kids, 16);
    size_t i;

    for (i = 0; i < n && i < 16; i++) {
        if (wcscmp(scan_model_node(m, kids[i])->name, name) == 0) {
            return kids[i];
        }
    }
    return SCAN_NODE_NONE;
}

static void test_tree(void)
{
    ScanModel m;
    size_t big, deep, small, empty, link, locked;
    const ScanNode *root;

    scan_model_init(&m);
    CHECK(scan_model_build(&m, g_root, SCAN_DEPTH_UNLIMITED, NULL, NULL) == SCAN_PARTIAL);
    CHECK(!m.out_of_memory);
    CHECK_U64(m.len, 7);

    root = scan_model_node(&m, 0);
    CHECK(root != NULL);
    if (root == NULL) {
        scan_model_free(&m);
        return;
    }
    CHECK(wcscmp(root->name, g_root) == 0);
    CHECK(root->parent == SCAN_NODE_NONE && root->depth == 0 && root->flags == 0);
    CHECK_U64(root->child_count, 5);
    CHECK_U64(root->own_files, 1);
    CHECK_U64(root->own_bytes, 5);

    /* 루트 합계는 scanner 집계와 정확히 같아야 한다. */
    CHECK_U64(root->total_files, m.report.file_count);
    CHECK_U64(root->total_dirs, m.report.dir_count);
    CHECK_U64(root->total_bytes, m.report.total_bytes);
    CHECK_U64(root->total_files, 4);
    CHECK_U64(root->total_dirs, 6);
    CHECK_U64(root->total_bytes, 5 + 100 + 1000 + 10);

    big = find_child(&m, 0, L"big");
    small = find_child(&m, 0, L"small");
    empty = find_child(&m, 0, L"empty");
    link = find_child(&m, 0, L"link");
    locked = find_child(&m, 0, L"locked");
    CHECK(big != SCAN_NODE_NONE && small != SCAN_NODE_NONE && empty != SCAN_NODE_NONE &&
          link != SCAN_NODE_NONE && locked != SCAN_NODE_NONE);
    if (big == SCAN_NODE_NONE || small == SCAN_NODE_NONE || empty == SCAN_NODE_NONE ||
        link == SCAN_NODE_NONE || locked == SCAN_NODE_NONE) {
        scan_model_free(&m);
        return;
    }
    deep = find_child(&m, big, L"deep");
    CHECK(deep != SCAN_NODE_NONE);
    if (deep == SCAN_NODE_NONE) {
        scan_model_free(&m);
        return;
    }

    CHECK(m.nodes[big].parent == 0 && m.nodes[big].depth == 1 && m.nodes[big].flags == 0);
    CHECK_U64(m.nodes[big].own_bytes, 100);
    CHECK_U64(m.nodes[big].total_bytes, 1100);
    CHECK_U64(m.nodes[big].total_files, 2);
    CHECK_U64(m.nodes[big].total_dirs, 1);
    CHECK(m.nodes[deep].parent == big && m.nodes[deep].depth == 2);
    CHECK_U64(m.nodes[deep].total_bytes, 1000);
    CHECK_U64(m.nodes[deep].child_count, 0);
    CHECK_U64(m.nodes[small].total_bytes, 10);
    CHECK_U64(m.nodes[empty].total_bytes, 0);
    CHECK(m.nodes[empty].flags == 0);

    /* 정션은 노드로 보이지만 따라가지 않아 내용이 0 이고, 거부된 폴더는 실패로 표시된다. */
    CHECK(m.nodes[link].flags == SCAN_NODE_REPARSE);
    CHECK_U64(m.nodes[link].total_bytes, 0);
    CHECK_U64(m.nodes[link].child_count, 0);
    CHECK(m.nodes[locked].flags == SCAN_NODE_FAILED);

    /* 부모 인덱스는 항상 자식보다 작다. */
    {
        size_t i;
        for (i = 1; i < m.len; i++) {
            CHECK(m.nodes[i].parent < i);
        }
    }

    {
        wchar_t *p = scan_model_path(&m, deep);
        wchar_t *expected = fixture(L"big\\deep");
        CHECK(p != NULL && expected != NULL && wcscmp(p, expected) == 0);
        free(p);
        free(expected);
        p = scan_model_path(&m, 0);
        CHECK(p != NULL && wcscmp(p, g_root) == 0);
        free(p);
        CHECK(scan_model_path(&m, 99) == NULL);
        CHECK(scan_model_node(&m, 99) == NULL);
    }

    scan_model_free(&m);
    CHECK(m.nodes == NULL && m.len == 0 && m.report.file_count == 0);
}

static void test_children_order(void)
{
    ScanModel m;
    size_t kids[8];
    size_t n;

    scan_model_init(&m);
    scan_model_build(&m, g_root, SCAN_DEPTH_UNLIMITED, NULL, NULL);

    CHECK_U64(scan_model_children(&m, 0, SCAN_ORDER_SIZE_DESC, NULL, 0), 5);

    /* 용량 내림차순, 같은 용량(0)은 이름순. */
    n = scan_model_children(&m, 0, SCAN_ORDER_SIZE_DESC, kids, 8);
    CHECK_U64(n, 5);
    CHECK_NAME(&m, kids[0], L"big");
    CHECK_NAME(&m, kids[1], L"small");
    CHECK_NAME(&m, kids[2], L"empty");
    CHECK_NAME(&m, kids[3], L"link");
    CHECK_NAME(&m, kids[4], L"locked");

    n = scan_model_children(&m, 0, SCAN_ORDER_NAME, kids, 8);
    CHECK_U64(n, 5);
    CHECK_NAME(&m, kids[0], L"big");
    CHECK_NAME(&m, kids[1], L"empty");
    CHECK_NAME(&m, kids[2], L"link");
    CHECK_NAME(&m, kids[3], L"locked");
    CHECK_NAME(&m, kids[4], L"small");

    /* cap 이 작아도 전체 개수를 돌려주고 앞부분만 채운다. */
    kids[2] = 12345;
    n = scan_model_children(&m, 0, SCAN_ORDER_SIZE_DESC, kids, 2);
    CHECK_U64(n, 5);
    CHECK_NAME(&m, kids[0], L"big");
    CHECK_NAME(&m, kids[1], L"small");
    CHECK(kids[2] == 12345);

    scan_model_free(&m);
}

static void test_rank(void)
{
    ScanModel m;
    size_t top[16];
    size_t n, i;

    scan_model_init(&m);
    scan_model_build(&m, g_root, SCAN_DEPTH_UNLIMITED, NULL, NULL);

    /* 하위 포함 용량: big(1100) > deep(1000) > small(10) > 나머지 0. 루트는 제외. */
    n = scan_model_rank(&m, SCAN_RANK_TOTAL_BYTES, top, 16);
    CHECK_U64(n, 6);
    CHECK_NAME(&m, top[0], L"big");
    CHECK_NAME(&m, top[1], L"deep");
    CHECK_NAME(&m, top[2], L"small");
    for (i = 0; i < n; i++) {
        CHECK(top[i] != 0);
        if (i > 0) {
            CHECK(m.nodes[top[i - 1]].total_bytes >= m.nodes[top[i]].total_bytes);
        }
    }
    for (i = 3; i < n; i++) {
        CHECK_U64(m.nodes[top[i]].total_bytes, 0);
        CHECK(top[i - 1] < top[i] || i == 3); /* 동점은 인덱스 순 */
    }

    /* 폴더 자체 용량: deep(1000) > big(100) > small(10). */
    n = scan_model_rank(&m, SCAN_RANK_OWN_BYTES, top, 3);
    CHECK_U64(n, 3);
    CHECK_NAME(&m, top[0], L"deep");
    CHECK_NAME(&m, top[1], L"big");
    CHECK_NAME(&m, top[2], L"small");

    CHECK_U64(scan_model_rank(&m, SCAN_RANK_TOTAL_BYTES, top, 0), 0);
    CHECK_U64(scan_model_rank(&m, SCAN_RANK_TOTAL_BYTES, NULL, 4), 0);

    scan_model_free(&m);
}

static void test_depth_limit(void)
{
    ScanModel m;
    size_t big, deep;

    scan_model_init(&m);
    CHECK(scan_model_build(&m, g_root, 1, NULL, NULL) == SCAN_PARTIAL);
    CHECK(m.max_depth == 1);
    big = find_child(&m, 0, L"big");
    CHECK(big != SCAN_NODE_NONE);
    if (big != SCAN_NODE_NONE) {
        deep = find_child(&m, big, L"deep");
        CHECK(deep != SCAN_NODE_NONE);
        if (deep != SCAN_NODE_NONE) {
            CHECK(m.nodes[deep].flags == SCAN_NODE_DEPTH_LIMIT);
            CHECK_U64(m.nodes[deep].total_bytes, 0);
        }
        CHECK_U64(m.nodes[big].total_bytes, 100);
    }
    CHECK_U64(m.nodes[0].total_bytes, m.report.total_bytes);
    CHECK_U64(m.nodes[0].total_bytes, 5 + 100 + 10);

    /* 같은 모델로 다시 만들면 이전 내용은 사라진다. */
    CHECK(scan_model_build(&m, g_root, 0, NULL, NULL) == SCAN_COMPLETE);
    CHECK_U64(m.len, 6);
    CHECK_U64(m.nodes[0].total_bytes, 5);
    CHECK_U64(m.report.error_count, 0);
    scan_model_free(&m);
}

typedef struct {
    int calls;
    int finished_calls;
    int prefixed;
    int out_of_range;
    int non_monotonic;
    uint64_t last_scanned;
    uint64_t last_files;
    ScanProgress final;
} ProgressLog;

static void on_progress(const ScanProgress *p, void *ctx)
{
    ProgressLog *log = ctx;
    double f = scan_progress_fraction(p);

    log->calls++;
    if (f < 0.0 || f > 1.0) {
        log->out_of_range++;
    }
    if (p->dirs_scanned < log->last_scanned || p->files < log->last_files) {
        log->non_monotonic++;
    }
    log->last_scanned = p->dirs_scanned;
    log->last_files = p->files;

    if (p->finished) {
        log->finished_calls++;
        log->final = *p;
        log->final.current_dir = NULL;
    } else if (p->current_dir == NULL || wcsncmp(p->current_dir, L"\\\\?\\", 4) == 0) {
        log->prefixed++;
    }
}

static void test_progress(void)
{
    ScanModel m;
    ProgressLog log;

    memset(&log, 0, sizeof(log));
    scan_model_init(&m);
    scan_model_build(&m, g_root, SCAN_DEPTH_UNLIMITED, on_progress, &log);

    /* 열거한 폴더(root, big, deep, small, empty, locked)마다 한 번 + 완료 한 번. */
    CHECK(log.calls == 7);
    CHECK(log.finished_calls == 1);
    CHECK(log.prefixed == 0);
    CHECK(log.out_of_range == 0);
    CHECK(log.non_monotonic == 0);
    CHECK(log.final.status == SCAN_PARTIAL);
    CHECK_U64(log.final.dirs_scanned, 6);
    CHECK_U64(log.final.dirs_pending, 0);
    CHECK_U64(log.final.files, 4);
    CHECK_U64(log.final.dirs, 6);
    CHECK_U64(log.final.bytes, 1115);
    CHECK_U64(log.final.errors, 1);
    CHECK(scan_progress_fraction(&log.final) == 1.0);
    scan_model_free(&m);

    {
        ScanProgress p;
        memset(&p, 0, sizeof(p));
        CHECK(scan_progress_fraction(&p) == 0.0);
        p.dirs_scanned = 1;
        p.dirs_pending = 3;
        CHECK(scan_progress_fraction(&p) == 0.25);
    }
}

static void test_fatal_root(void)
{
    ScanModel m;
    ProgressLog log;
    wchar_t *missing = fixture(L"no-such-dir");

    memset(&log, 0, sizeof(log));
    scan_model_init(&m);
    CHECK(scan_model_build(&m, missing, SCAN_DEPTH_UNLIMITED, on_progress, &log) == SCAN_FATAL);
    CHECK_U64(m.len, 1);
    CHECK(m.nodes[0].flags == SCAN_NODE_FAILED);
    CHECK_U64(m.nodes[0].total_bytes, 0);
    CHECK(m.report.aborted == 1);
    CHECK(log.calls == 1 && log.finished_calls == 1);
    CHECK(log.final.status == SCAN_FATAL);
    scan_model_free(&m);

    /* 루트 경로가 비어 있으면 노드 없이 FATAL. */
    scan_model_init(&m);
    CHECK(scan_model_build(&m, L"", SCAN_DEPTH_UNLIMITED, NULL, NULL) == SCAN_FATAL);
    CHECK_U64(m.len, 0);
    CHECK(scan_model_node(&m, 0) == NULL);
    CHECK_U64(scan_model_children(&m, 0, SCAN_ORDER_SCAN, NULL, 0), 0);
    scan_model_free(&m);
    free(missing);
}

/* MAX_PATH 를 넘는 깊은 체인에서도 트리와 경로가 맞아야 한다. */
static void test_deep_chain(void)
{
    enum { LEVELS = 150 };
    wchar_t base[MAX_PATH + 8];
    wchar_t *root, *cur;
    ScanModel m;
    int i, ok;

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
        wchar_t *next = path_join(cur, L"dir");
        free(cur);
        cur = next;
        ok = cur != NULL && CreateDirectoryW(cur, NULL);
    }
    free(cur);
    CHECK(ok);

    if (ok) {
        size_t last;
        wchar_t *p;

        scan_model_init(&m);
        CHECK(scan_model_build(&m, base, SCAN_DEPTH_UNLIMITED, NULL, NULL) == SCAN_COMPLETE);
        CHECK_U64(m.len, LEVELS + 1);
        last = m.len - 1;
        CHECK(m.nodes[last].depth == LEVELS);
        CHECK_U64(m.nodes[0].total_dirs, LEVELS);
        p = scan_model_path(&m, last);
        CHECK(p != NULL && wcslen(p) == wcslen(base) + LEVELS * 4);
        CHECK(p != NULL && wcsncmp(p, base, wcslen(base)) == 0);
        free(p);
        scan_model_free(&m);
    }

    remove_tree(root);
    free(root);
}

int main(void)
{
    if (!setup_fixture()) {
        fprintf(stderr, "fixture setup failed (error %lu)\n", GetLastError());
        teardown_fixture();
        return 1;
    }
    test_tree();
    test_children_order();
    test_rank();
    test_depth_limit();
    test_progress();
    test_fatal_root();
    test_deep_chain();
    teardown_fixture();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all scan model tests passed\n");
    return 0;
}
