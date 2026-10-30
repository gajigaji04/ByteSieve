#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cleanup_job.h"
#include "cleanup_scan.h"
#include "path.h"

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

static wchar_t g_root[MAX_PATH];     /* 일반 경로 */
static wchar_t g_root_ext[MAX_PATH]; /* \\?\ 경로(긴 경로 픽스처용) */
static wchar_t *g_deep;              /* 260자를 넘는 깊은 폴더(상대 경로) */

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root_ext, name);
}

static int write_file(const wchar_t *name, DWORD len)
{
    static const char data[128] = {0};
    wchar_t *p = fixture(name);
    HANDLE h = p ? CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL)
                 : INVALID_HANDLE_VALUE;
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
             L"cmd /c mklink /J \"%ls\\%ls\" \"%ls\\%ls\" >nul", g_root, link, g_root,
             target);
    return _wsystem(cmd) == 0;
}

/* 모든 사용자에게 모든 접근을 거부(deny=0 이면 해제). */
static int set_deny_all(const wchar_t *name, int deny)
{
    SECURITY_DESCRIPTOR sd;
    BYTE sid_buf[SECURITY_MAX_SID_SIZE];
    DWORD sid_len = sizeof(sid_buf);
    BYTE acl_buf[256];
    PACL acl = (PACL)acl_buf;
    wchar_t *p = fixture(name);
    BOOL ok = p != NULL && InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);

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

/* "deep\<50자>\<50자>..." 를 만들어 g_deep 에 둔다(전체 경로 260자 초과). */
static int make_deep(void)
{
    wchar_t seg[51];
    wchar_t *cur = path_dup(L"deep");
    int i;

    wmemset(seg, L'd', 50);
    seg[50] = L'\0';
    if (cur == NULL || !make_dir(cur)) {
        free(cur);
        return 0;
    }
    for (i = 0; i < 6; i++) {
        wchar_t *next = path_join(cur, seg);
        free(cur);
        cur = next;
        if (cur == NULL || !make_dir(cur)) {
            free(cur);
            return 0;
        }
    }
    g_deep = cur;
    return 1;
}

static int deep_file(const wchar_t *rest, DWORD len)
{
    wchar_t *p = path_join(g_deep, rest);
    int ok = p != NULL && write_file(p, len);
    free(p);
    return ok;
}

static int deep_dir(const wchar_t *rest)
{
    wchar_t *p = path_join(g_deep, rest);
    int ok = p != NULL && make_dir(p);
    free(p);
    return ok;
}

static int setup_fixture(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_cleanup_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    swprintf(g_root_ext, MAX_PATH, L"\\\\?\\%ls", g_root);
    remove_tree(g_root_ext);
    if (!CreateDirectoryW(g_root_ext, NULL)) {
        return 0;
    }

    /*
     * root\
     *   app\package.json                 2   (프로젝트 표시 파일)
     *   app\node_modules\a\index.js      10  ┐
     *   app\node_modules\a\dist\x.js     5   │ node_modules 하나로 18 B (확인됨, 일부 누락)
     *   app\node_modules\debug.log       3   │
     *   app\node_modules\locked\         –   ┘ (접근 거부)
     *   app\dist\bundle.js               100 빌드 (확인됨)
     *   app\src\main.c                   1
     *   app\src\out\o.bin                7   빌드 (추정: src 에 표시 파일 없음)
     *   app\.next\cache\c                20  캐시
     *   logs\server.log                  50  로그
     *   logs\old.LOG                     4   로그
     *   notes.tmp                        6   임시 파일
     *   .temp\t                          8   임시 폴더
     *   report.log.bak                   9   (해당 없음)
     *   buildx\y                         1   (해당 없음)
     *   sys\build\z                      11  (보호 경로로 지정해 제외)
     *   linked\node_modules -> app\node_modules (정션, 무시)
     *   loop\back -> .                  (정션 순환, 따라가지 않음)
     *   deep\<50>x6\node_modules\f       1   긴 경로 안의 node_modules (추정)
     */
    return make_dir(L"app") && write_file(L"app\\package.json", 2) &&
           make_dir(L"app\\node_modules") && make_dir(L"app\\node_modules\\a") &&
           write_file(L"app\\node_modules\\a\\index.js", 10) &&
           make_dir(L"app\\node_modules\\a\\dist") &&
           write_file(L"app\\node_modules\\a\\dist\\x.js", 5) &&
           write_file(L"app\\node_modules\\debug.log", 3) &&
           make_dir(L"app\\node_modules\\locked") &&
           make_dir(L"app\\dist") && write_file(L"app\\dist\\bundle.js", 100) &&
           make_dir(L"app\\src") && write_file(L"app\\src\\main.c", 1) &&
           make_dir(L"app\\src\\out") && write_file(L"app\\src\\out\\o.bin", 7) &&
           make_dir(L"app\\.next") && make_dir(L"app\\.next\\cache") &&
           write_file(L"app\\.next\\cache\\c", 20) &&
           make_dir(L"logs") && write_file(L"logs\\server.log", 50) &&
           write_file(L"logs\\old.LOG", 4) &&
           write_file(L"notes.tmp", 6) &&
           make_dir(L".temp") && write_file(L".temp\\t", 8) &&
           write_file(L"report.log.bak", 9) &&
           make_dir(L"buildx") && write_file(L"buildx\\y", 1) &&
           make_dir(L"sys") && make_dir(L"sys\\build") && write_file(L"sys\\build\\z", 11) &&
           make_dir(L"linked") && make_junction(L"linked\\node_modules", L"app\\node_modules") &&
           make_dir(L"loop") && make_junction(L"loop\\back", L".") &&
           make_deep() && deep_dir(L"node_modules") && deep_file(L"node_modules\\f", 1) &&
           set_deny_all(L"app\\node_modules\\locked", 1);
}

static void teardown_fixture(void)
{
    set_deny_all(L"app\\node_modules\\locked", 0);
    remove_tree(g_root_ext);
    free(g_deep);
}

/* 표시 경로가 root\rel 인 항목을 찾는다. */
static const CleanupItem *find_item(const CleanupResult *r, const wchar_t *rel)
{
    wchar_t *want = path_join(g_root, rel);
    const CleanupItem *found = NULL;
    size_t i;

    for (i = 0; want != NULL && i < r->len; i++) {
        if (_wcsicmp(r->items[i].path, want) == 0) {
            found = &r->items[i];
            break;
        }
    }
    free(want);
    return found;
}

static void make_protect(CleanupProtect *p)
{
    wchar_t *sys = path_join(g_root, L"sys");

    cleanup_protect_init(p);
    cleanup_protect_add(p, sys);
    free(sys);
}

static void test_detect(void)
{
    CleanupResult r;
    CleanupProtect protect;
    const CleanupItem *it;
    size_t i;

    make_protect(&protect);
    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, g_root, &protect, NULL, NULL, NULL) == SCAN_PARTIAL);
    CHECK(r.status == SCAN_PARTIAL);
    CHECK(!r.root_protected);
    CHECK(!r.out_of_memory);
    CHECK(r.root != NULL && _wcsicmp(r.root, g_root) == 0);

    CHECK_U64(r.len, 9);
    CHECK_U64(r.total_bytes, 18 + 100 + 7 + 20 + 50 + 4 + 6 + 8 + 1);
    CHECK_U64(r.category_count[CLEANUP_NODE_MODULES], 2);
    CHECK_U64(r.category_bytes[CLEANUP_NODE_MODULES], 19);
    CHECK_U64(r.category_count[CLEANUP_BUILD], 2);
    CHECK_U64(r.category_bytes[CLEANUP_BUILD], 107);
    CHECK_U64(r.category_count[CLEANUP_CACHE], 1);
    CHECK_U64(r.category_bytes[CLEANUP_CACHE], 20);
    CHECK_U64(r.category_count[CLEANUP_TEMP], 2);
    CHECK_U64(r.category_bytes[CLEANUP_TEMP], 14);
    CHECK_U64(r.category_count[CLEANUP_LOG], 2);
    CHECK_U64(r.category_bytes[CLEANUP_LOG], 54);

    /* 중첩된 dist·log 는 node_modules 에 포함되고, 접근 거부로 일부 누락 표시 */
    it = find_item(&r, L"app\\node_modules");
    CHECK(it != NULL);
    if (it != NULL) {
        CHECK(it->category == CLEANUP_NODE_MODULES);
        CHECK_U64(it->bytes, 18);
        CHECK_U64(it->files, 3);
        CHECK_U64(it->dirs, 3);
        CHECK(it->flags & CLEANUP_ITEM_DIR);
        CHECK(it->flags & CLEANUP_ITEM_CONFIRMED);
        CHECK(it->flags & CLEANUP_ITEM_INCOMPLETE);
    }
    CHECK(find_item(&r, L"app\\node_modules\\a\\dist") == NULL);
    CHECK(find_item(&r, L"app\\node_modules\\debug.log") == NULL);

    it = find_item(&r, L"app\\dist");
    CHECK(it != NULL && it->bytes == 100 && (it->flags & CLEANUP_ITEM_CONFIRMED) &&
          !(it->flags & CLEANUP_ITEM_INCOMPLETE));
    it = find_item(&r, L"app\\src\\out");
    CHECK(it != NULL && it->category == CLEANUP_BUILD && it->bytes == 7 &&
          !(it->flags & CLEANUP_ITEM_CONFIRMED));
    it = find_item(&r, L"app\\.next");
    CHECK(it != NULL && it->category == CLEANUP_CACHE && it->bytes == 20);
    it = find_item(&r, L"logs\\server.log");
    CHECK(it != NULL && it->category == CLEANUP_LOG && it->bytes == 50 && it->files == 1 &&
          !(it->flags & CLEANUP_ITEM_DIR));
    it = find_item(&r, L"logs\\old.LOG");
    CHECK(it != NULL && it->category == CLEANUP_LOG);
    it = find_item(&r, L"notes.tmp");
    CHECK(it != NULL && it->category == CLEANUP_TEMP && it->bytes == 6);
    it = find_item(&r, L".temp");
    CHECK(it != NULL && it->category == CLEANUP_TEMP && (it->flags & CLEANUP_ITEM_DIR));

    /* 이름이 비슷할 뿐인 항목, 정션, 보호 경로는 없다 */
    CHECK(find_item(&r, L"report.log.bak") == NULL);
    CHECK(find_item(&r, L"buildx") == NULL);
    CHECK(find_item(&r, L"linked\\node_modules") == NULL);
    CHECK(find_item(&r, L"sys\\build") == NULL);
    CHECK_U64(r.report.excluded_dir_count, 1);
    CHECK_U64(r.report.reparse_count, 2);

    /* 긴 경로 안의 node_modules */
    {
        wchar_t *rel = path_join(g_deep, L"node_modules");
        it = rel != NULL ? find_item(&r, rel) : NULL;
        CHECK(it != NULL && it->bytes == 1 && wcslen(it->path) > MAX_PATH &&
              !(it->flags & CLEANUP_ITEM_CONFIRMED));
        free(rel);
    }

    /* 접근 거부는 기존 정책대로 기록하고 계속한다 */
    CHECK_U64(r.report.error_count, 1);
    CHECK(r.report.errors_len == 1 && r.report.errors[0].kind == SCAN_ERR_ACCESS_DENIED);

    for (i = 0; i < r.len; i++) {
        CHECK(!(r.items[i].flags & CLEANUP_ITEM_PROTECTED));
    }
    cleanup_result_free(&r);
    cleanup_protect_free(&protect);
}

static void test_select(void)
{
    CleanupResult r;
    size_t idx[16], n;
    uint64_t bytes;
    size_t i;

    cleanup_result_init(&r);
    cleanup_scan(&r, g_root, NULL, NULL, NULL, NULL);

    /* 보호 목록이 없으면 sys\build 도 후보다 */
    CHECK(find_item(&r, L"sys\\build") != NULL);
    CHECK_U64(r.len, 10);

    n = cleanup_select(&r, CLEANUP_MASK_ALL, idx, 16, &bytes);
    CHECK_U64(n, 10);
    CHECK_U64(bytes, r.total_bytes);
    for (i = 1; i < n; i++) {
        CHECK(r.items[idx[i - 1]].bytes >= r.items[idx[i]].bytes);
    }
    CHECK(r.items[idx[0]].bytes == 100);

    n = cleanup_select(&r, CLEANUP_MASK(CLEANUP_LOG), idx, 16, &bytes);
    CHECK_U64(n, 2);
    CHECK_U64(bytes, 54);
    CHECK(r.items[idx[0]].bytes == 50 && r.items[idx[1]].bytes == 4);

    n = cleanup_select(&r, CLEANUP_MASK(CLEANUP_BUILD) | CLEANUP_MASK(CLEANUP_CACHE), idx, 2,
                       &bytes);
    CHECK_U64(n, 4); /* 전체 개수, 쓴 것은 cap 만큼 */
    CHECK_U64(bytes, 100 + 7 + 11 + 20);
    CHECK(r.items[idx[0]].bytes == 100 && r.items[idx[1]].bytes == 20);

    CHECK_U64(cleanup_select(&r, 0, idx, 16, &bytes), 0);
    CHECK_U64(bytes, 0);
    CHECK_U64(cleanup_select(&r, CLEANUP_MASK_ALL, NULL, 0, NULL), 10);
    CHECK_U64(cleanup_select(NULL, CLEANUP_MASK_ALL, idx, 16, &bytes), 0);
    cleanup_result_free(&r);
}

/* 사용자가 보호 경로 안을 직접 고르면 제외하지 않고 경고 표시를 붙인다. */
static void test_protected_root(void)
{
    CleanupResult r;
    CleanupProtect protect;
    wchar_t *sys = path_join(g_root, L"sys");

    make_protect(&protect);
    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, sys, &protect, NULL, NULL, NULL) == SCAN_COMPLETE);
    CHECK(r.root_protected);
    CHECK_U64(r.len, 1);
    CHECK(r.len == 1 && r.items[0].category == CLEANUP_BUILD &&
          (r.items[0].flags & CLEANUP_ITEM_PROTECTED) && r.items[0].bytes == 11);
    CHECK_U64(r.report.excluded_dir_count, 0);
    cleanup_result_free(&r);
    cleanup_protect_free(&protect);
    free(sys);
}

static void test_invalid_root(void)
{
    CleanupResult r;
    wchar_t *missing = path_join(g_root, L"no-such-dir");

    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, missing, NULL, NULL, NULL, NULL) == SCAN_FATAL);
    CHECK(r.report.aborted);
    CHECK_U64(r.len, 0);
    CHECK(cleanup_scan(&r, NULL, NULL, NULL, NULL, NULL) == SCAN_FATAL);
    CHECK(cleanup_scan(NULL, g_root, NULL, NULL, NULL, NULL) == SCAN_FATAL);
    cleanup_result_free(&r);
    free(missing);
}

static int cancel_now(void *ctx)
{
    (void)ctx;
    return 1;
}

typedef struct {
    int calls;
    int finished;
    size_t last_items;
} ProgressLog;

static void on_progress(const CleanupProgress *p, void *ctx)
{
    ProgressLog *log = ctx;

    log->calls++;
    if (p->finished) {
        log->finished++;
        CHECK(p->current_dir == NULL);
    } else {
        CHECK(p->current_dir != NULL);
    }
    log->last_items = p->items;
}

/* 루트만 열거하고 취소: 그때까지의 후보는 남고, 열지 못한 폴더 항목은 일부 누락. */
static void test_cancel(void)
{
    CleanupResult r;
    ProgressLog log = {0};
    const CleanupItem *it;

    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, g_root, NULL, on_progress, cancel_now, &log) == SCAN_PARTIAL);
    CHECK(r.report.cancelled);
    CHECK(log.finished == 1 && log.calls == 2);
    CHECK(log.last_items == r.len);

    it = find_item(&r, L"notes.tmp");
    CHECK(it != NULL && !(it->flags & CLEANUP_ITEM_INCOMPLETE));
    it = find_item(&r, L".temp");
    CHECK(it != NULL && (it->flags & CLEANUP_ITEM_INCOMPLETE) && it->bytes == 0);
    CHECK(find_item(&r, L"app\\dist") == NULL);
    cleanup_result_free(&r);
}

typedef struct {
    volatile LONG finished;
} JobCtx;

static void job_notify(int finished, void *ctx)
{
    JobCtx *c = ctx;

    if (finished) {
        InterlockedIncrement(&c->finished);
    }
}

/* 작업 스레드: 끝까지 돌린 결과는 직접 호출과 같고, 즉시 취소해도 정상 종료한다. */
static void test_job(void)
{
    CleanupResult r;
    CleanupJobSnapshot snap;
    JobCtx ctx = {0};
    CleanupJob *job;

    job = cleanup_job_start(g_root, 0, job_notify, &ctx);
    CHECK(job != NULL);
    if (job == NULL) {
        return;
    }
    CHECK(cleanup_job_wait(job, 30000));
    cleanup_job_progress(job, &snap);
    CHECK(snap.progress.finished);
    CHECK(!snap.cancel_requested);
    cleanup_result_init(&r);
    CHECK(cleanup_job_finish(job, &r) == SCAN_PARTIAL);
    CHECK(ctx.finished == 1);
    /* 픽스처는 임시 폴더 아래이므로 시스템 보호 경로와 겹치지 않는다 */
    CHECK(!r.root_protected);
    CHECK_U64(r.len, 10);
    cleanup_result_free(&r);

    job = cleanup_job_start(g_root, 0, NULL, NULL);
    CHECK(job != NULL);
    if (job != NULL) {
        cleanup_job_cancel(job);
        cleanup_result_init(&r);
        cleanup_job_finish(job, &r);
        CHECK(r.status != SCAN_FATAL);
        cleanup_result_free(&r);
    }
    CHECK(cleanup_job_finish(NULL, NULL) == SCAN_FATAL);
}

int main(void)
{
    if (!setup_fixture()) {
        fprintf(stderr, "fixture setup failed (error %lu)\n", GetLastError());
        teardown_fixture();
        return 1;
    }
    test_detect();
    test_select();
    test_protected_root();
    test_invalid_root();
    test_cancel();
    test_job();
    teardown_fixture();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all cleanup_scan tests passed\n");
    return 0;
}
