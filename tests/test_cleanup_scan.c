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

/* 개발 환경 분류: 기존 픽스처를 지운 뒤 별도 폴더에서 확인한다. */
static int setup_eco_fixture(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_cleanup_eco_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    swprintf(g_root_ext, MAX_PATH, L"\\\\?\\%ls", g_root);
    remove_tree(g_root_ext);
    if (!CreateDirectoryW(g_root_ext, NULL)) {
        return 0;
    }
    return make_dir(L"web") && write_file(L"web\\package.json", 2) &&
           make_dir(L"web\\node_modules") && write_file(L"web\\node_modules\\x.js", 10) &&
           make_dir(L"web\\build") && write_file(L"web\\build\\b.js", 11) &&
           make_dir(L"web\\.next") && write_file(L"web\\.next\\c", 12) &&
           make_dir(L"py") && write_file(L"py\\pyproject.toml", 2) &&
           make_dir(L"py\\__pycache__") && write_file(L"py\\__pycache__\\m.pyc", 13) &&
           make_dir(L"py\\pkg") && make_dir(L"py\\pkg\\__pycache__") &&
           write_file(L"py\\pkg\\__pycache__\\n.pyc", 14) &&
           make_dir(L"py\\dist") && write_file(L"py\\dist\\w.whl", 15) &&
           make_dir(L"rs") && make_dir(L"rs\\target") && make_dir(L"rs\\target\\debug") &&
           write_file(L"rs\\target\\debug\\a", 16) &&
           /* 표시 파일이 폴더보다 뒤에 열거되어도 판정은 같다. */
           write_file(L"rs\\Cargo.toml", 2) &&
           make_dir(L"mvn") && write_file(L"mvn\\pom.xml", 2) && make_dir(L"mvn\\target") &&
           write_file(L"mvn\\target\\k.class", 17) &&
           make_dir(L"cs") && write_file(L"cs\\App.csproj", 2) && make_dir(L"cs\\bin") &&
           make_dir(L"cs\\bin\\Debug") && write_file(L"cs\\bin\\Debug\\a.dll", 18) &&
           make_dir(L"cs\\obj") && write_file(L"cs\\obj\\p", 19) && make_dir(L"cs\\.vs") &&
           write_file(L"cs\\.vs\\v", 20) &&
           make_dir(L"gradle") && write_file(L"gradle\\build.gradle", 2) &&
           make_dir(L"gradle\\.gradle") && write_file(L"gradle\\.gradle\\g", 21) &&
           make_dir(L"gradle\\build") && write_file(L"gradle\\build\\h", 22) &&
           make_dir(L"cpp") && write_file(L"cpp\\CMakeLists.txt", 2) &&
           make_dir(L"cpp\\cmake-build-debug") && write_file(L"cpp\\cmake-build-debug\\o", 23) &&
           make_dir(L"cpp\\build") && write_file(L"cpp\\build\\q", 24) &&
           /* 표시 파일이 없어 후보가 아닌 폴더(사용자 bin, 홈의 .gradle, 아무 target) */
           make_dir(L"home") && make_dir(L"home\\bin") && write_file(L"home\\bin\\tool.exe", 25) &&
           write_file(L"home\\bin\\x.log", 26) && make_dir(L"home\\.gradle") &&
           write_file(L"home\\.gradle\\gradle.properties", 27) && make_dir(L"home\\target") &&
           write_file(L"home\\target\\t.txt", 28) &&
           make_dir(L"plain") && make_dir(L"plain\\dist") && write_file(L"plain\\dist\\d.js", 29) &&
           make_dir(L"plain\\.cache") && write_file(L"plain\\.cache\\c", 30);
}

static void check_eco(const CleanupResult *r, const wchar_t *rel, CleanupCategory category,
                      CleanupEco eco, uint64_t bytes)
{
    const CleanupItem *it = find_item(r, rel);

    CHECK(it != NULL);
    if (it == NULL) {
        fwprintf(stderr, L"  missing item: %ls\n", rel);
        return;
    }
    CHECK(it->category == category);
    CHECK(it->eco == eco);
    CHECK_U64(it->bytes, bytes);
    if (it->eco != eco) {
        fwprintf(stderr, L"  %ls: eco %ls\n", rel, cleanup_eco_name(it->eco));
    }
}

static void test_ecosystems(void)
{
    CleanupResult r;
    size_t order[16], n, i;
    uint64_t bytes;

    if (!setup_eco_fixture()) {
        CHECK(0);
        remove_tree(g_root_ext);
        return;
    }
    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, g_root, NULL, NULL, NULL, NULL) == SCAN_COMPLETE);

    check_eco(&r, L"web\\node_modules", CLEANUP_NODE_MODULES, CLEANUP_ECO_NODE, 10);
    check_eco(&r, L"web\\build", CLEANUP_BUILD, CLEANUP_ECO_NODE, 11);
    check_eco(&r, L"web\\.next", CLEANUP_CACHE, CLEANUP_ECO_NODE, 12);
    check_eco(&r, L"py\\__pycache__", CLEANUP_CACHE, CLEANUP_ECO_PYTHON, 13);
    check_eco(&r, L"py\\pkg\\__pycache__", CLEANUP_CACHE, CLEANUP_ECO_PYTHON, 14);
    check_eco(&r, L"py\\dist", CLEANUP_BUILD, CLEANUP_ECO_PYTHON, 15);
    check_eco(&r, L"rs\\target", CLEANUP_BUILD, CLEANUP_ECO_RUST, 16);
    check_eco(&r, L"mvn\\target", CLEANUP_BUILD, CLEANUP_ECO_JAVA, 17);
    check_eco(&r, L"cs\\bin", CLEANUP_BUILD, CLEANUP_ECO_DOTNET, 18);
    check_eco(&r, L"cs\\obj", CLEANUP_BUILD, CLEANUP_ECO_DOTNET, 19);
    check_eco(&r, L"cs\\.vs", CLEANUP_CACHE, CLEANUP_ECO_DOTNET, 20);
    check_eco(&r, L"gradle\\.gradle", CLEANUP_CACHE, CLEANUP_ECO_JAVA, 21);
    check_eco(&r, L"gradle\\build", CLEANUP_BUILD, CLEANUP_ECO_JAVA, 22);
    check_eco(&r, L"cpp\\cmake-build-debug", CLEANUP_BUILD, CLEANUP_ECO_CPP, 23);
    check_eco(&r, L"cpp\\build", CLEANUP_BUILD, CLEANUP_ECO_CPP, 24);
    check_eco(&r, L"plain\\dist", CLEANUP_BUILD, CLEANUP_ECO_GENERIC, 29);
    check_eco(&r, L"plain\\.cache", CLEANUP_CACHE, CLEANUP_ECO_GENERIC, 30);
    /* 표시 파일로 확인한 빌드 폴더 */
    CHECK(find_item(&r, L"rs\\target") != NULL &&
          (find_item(&r, L"rs\\target")->flags & CLEANUP_ITEM_CONFIRMED));
    CHECK(find_item(&r, L"plain\\dist") != NULL &&
          !(find_item(&r, L"plain\\dist")->flags & CLEANUP_ITEM_CONFIRMED));

    /* 표시 파일이 없는 bin·.gradle·target 은 항목이 아니고, 그 안은 일반 폴더처럼 탐지한다. */
    CHECK(find_item(&r, L"home\\bin") == NULL);
    CHECK(find_item(&r, L"home\\.gradle") == NULL);
    CHECK(find_item(&r, L"home\\target") == NULL);
    check_eco(&r, L"home\\bin\\x.log", CLEANUP_LOG, CLEANUP_ECO_GENERIC, 26);

    CHECK_U64(r.len, 18);
    CHECK_U64(r.eco_count[CLEANUP_ECO_NODE], 3);
    CHECK_U64(r.eco_bytes[CLEANUP_ECO_NODE], 10 + 11 + 12);
    CHECK_U64(r.eco_count[CLEANUP_ECO_PYTHON], 3);
    CHECK_U64(r.eco_bytes[CLEANUP_ECO_PYTHON], 13 + 14 + 15);
    CHECK_U64(r.eco_count[CLEANUP_ECO_JAVA], 3);
    CHECK_U64(r.eco_count[CLEANUP_ECO_DOTNET], 3);
    CHECK_U64(r.eco_count[CLEANUP_ECO_RUST], 1);
    CHECK_U64(r.eco_count[CLEANUP_ECO_CPP], 2);
    CHECK_U64(r.eco_count[CLEANUP_ECO_GO], 0);
    CHECK_U64(r.eco_count[CLEANUP_ECO_GENERIC], 3);
    CHECK_U64(r.total_bytes, 10 + 11 + 12 + 13 + 14 + 15 + 16 + 17 + 18 + 19 + 20 + 21 + 22 + 23 +
                                 24 + 26 + 29 + 30);

    /* 개발 환경 필터 */
    n = cleanup_select_ex(&r, CLEANUP_MASK_ALL, CLEANUP_ECO_BIT(CLEANUP_ECO_NODE), order, 16,
                          &bytes);
    CHECK_U64(n, 3);
    CHECK_U64(bytes, 33);
    for (i = 0; i < n && i < 16; i++) {
        CHECK(r.items[order[i]].eco == CLEANUP_ECO_NODE);
    }
    /* 용량 내림차순 */
    CHECK(n == 3 && r.items[order[0]].bytes == 12 && r.items[order[2]].bytes == 10);
    /* 유형 × 환경: Java 빌드 결과물 */
    n = cleanup_select_ex(&r, CLEANUP_MASK(CLEANUP_BUILD), CLEANUP_ECO_BIT(CLEANUP_ECO_JAVA), order,
                          16, &bytes);
    CHECK_U64(n, 2);
    CHECK_U64(bytes, 17 + 22);
    /* 기존 cleanup_select 는 모든 환경 */
    CHECK_U64(cleanup_select(&r, CLEANUP_MASK_ALL, NULL, 0, &bytes), 18);
    CHECK_U64(bytes, r.total_bytes);

    cleanup_result_free(&r);
    remove_tree(g_root_ext);
}

/*
 * 개발 도구 설치 경로 픽스처. 사용자 폴더·Program Files 환경 변수를 이 아래로 돌려
 * cleanup_protect_add_system 이 실제로 만드는 보호 목록으로 확인한다.
 */
static int setup_dev_tools_fixture(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_cleanup_dev_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    swprintf(g_root_ext, MAX_PATH, L"\\\\?\\%ls", g_root);
    remove_tree(g_root_ext);
    if (!CreateDirectoryW(g_root_ext, NULL)) {
        return 0;
    }
    return /* %LOCALAPPDATA%\Programs: VS Code 사용자 설치, Python */
           make_dir(L"local") && make_dir(L"local\\Programs") &&
           make_dir(L"local\\Programs\\Microsoft VS Code") &&
           write_file(L"local\\Programs\\Microsoft VS Code\\Code.exe", 31) &&
           write_file(L"local\\Programs\\Microsoft VS Code\\debug.log", 32) &&
           make_dir(L"local\\Programs\\Microsoft VS Code\\resources") &&
           make_dir(L"local\\Programs\\Microsoft VS Code\\resources\\app") &&
           write_file(L"local\\Programs\\Microsoft VS Code\\resources\\app\\package.json", 2) &&
           make_dir(L"local\\Programs\\Microsoft VS Code\\resources\\app\\node_modules") &&
           write_file(L"local\\Programs\\Microsoft VS Code\\resources\\app\\node_modules\\a.js",
                      33) &&
           make_dir(L"local\\Programs\\Microsoft VS Code\\resources\\app\\out") &&
           write_file(L"local\\Programs\\Microsoft VS Code\\resources\\app\\out\\main.js", 34) &&
           make_dir(L"local\\Programs\\Python") && make_dir(L"local\\Programs\\Python\\Lib") &&
           make_dir(L"local\\Programs\\Python\\Lib\\__pycache__") &&
           write_file(L"local\\Programs\\Python\\Lib\\__pycache__\\os.pyc", 35) &&
           /* 보호 경로가 아닌 같은 사용자 폴더의 임시 파일은 그대로 후보 */
           make_dir(L"local\\Temp") && write_file(L"local\\Temp\\x.tmp", 36) &&
           /* 설치한 VS Code 확장 */
           make_dir(L"home") && make_dir(L"home\\.vscode") &&
           make_dir(L"home\\.vscode\\extensions") &&
           make_dir(L"home\\.vscode\\extensions\\ms-python.python-2026.1.0") &&
           write_file(L"home\\.vscode\\extensions\\ms-python.python-2026.1.0\\package.json", 2) &&
           make_dir(L"home\\.vscode\\extensions\\ms-python.python-2026.1.0\\node_modules") &&
           write_file(L"home\\.vscode\\extensions\\ms-python.python-2026.1.0\\node_modules\\b.js",
                      37) &&
           /* %ProgramFiles%: Visual Studio 기본 위치 */
           make_dir(L"pf") && make_dir(L"pf\\Microsoft Visual Studio") &&
           make_dir(L"pf\\Microsoft Visual Studio\\2022") &&
           make_dir(L"pf\\Microsoft Visual Studio\\2022\\Community") &&
           make_dir(L"pf\\Microsoft Visual Studio\\2022\\Community\\Common7") &&
           make_dir(L"pf\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE") &&
           write_file(L"pf\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\devenv.exe",
                      38) &&
           write_file(L"pf\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\setup.log",
                      39) &&
           /* 레지스트리 InstallLocation 으로 알게 되는 다른 위치의 Visual Studio */
           make_dir(L"VS2022") && write_file(L"VS2022\\devenv.exe", 40) &&
           make_dir(L"VS2022\\Extensions") && make_dir(L"VS2022\\Extensions\\y") &&
           write_file(L"VS2022\\Extensions\\y\\package.json", 2) &&
           make_dir(L"VS2022\\Extensions\\y\\node_modules") &&
           write_file(L"VS2022\\Extensions\\y\\node_modules\\d.js", 41) &&
           make_dir(L"VS2022\\Python") && make_dir(L"VS2022\\Python\\__pycache__") &&
           write_file(L"VS2022\\Python\\__pycache__\\m.pyc", 42) &&
           /* 보호 경로 밖의 일반 프로젝트는 그대로 후보 */
           make_dir(L"proj") && write_file(L"proj\\package.json", 2) &&
           make_dir(L"proj\\node_modules") && write_file(L"proj\\node_modules\\e.js", 43);
}

/*
 * 앱 데이터·LevelDB 픽스처: local=%LOCALAPPDATA%, roaming=%APPDATA%, home=%USERPROFILE%.
 * 브라우저·편집기의 LevelDB 쓰기 로그(000003.log)와 앱별 폴더의 로그는 데이터일 수 있다.
 */
static int setup_app_data_fixture(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_cleanup_appdata_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    swprintf(g_root_ext, MAX_PATH, L"\\\\?\\%ls", g_root);
    remove_tree(g_root_ext);
    if (!CreateDirectoryW(g_root_ext, NULL)) {
        return 0;
    }
    return /* Chrome 저장소(LevelDB)와 진짜 로그 */
           make_dir(L"local") && make_dir(L"local\\Google") &&
           make_dir(L"local\\Google\\Chrome") && write_file(L"local\\Google\\Chrome\\debug.log", 11) &&
           make_dir(L"local\\Google\\Chrome\\leveldb") &&
           write_file(L"local\\Google\\Chrome\\leveldb\\000003.log", 12) &&
           write_file(L"local\\Google\\Chrome\\leveldb\\CURRENT", 1) &&
           write_file(L"local\\Google\\Chrome\\leveldb\\MANIFEST-000001", 1) &&
           /* IDE 가 내려받아 쓰는 도구(package.json + node_modules) */
           make_dir(L"local\\JetBrains") && make_dir(L"local\\JetBrains\\tool") &&
           write_file(L"local\\JetBrains\\tool\\package.json", 2) &&
           make_dir(L"local\\JetBrains\\tool\\node_modules") &&
           write_file(L"local\\JetBrains\\tool\\node_modules\\t.js", 13) &&
           /* 임시 폴더는 그대로 후보 */
           make_dir(L"local\\Temp") && write_file(L"local\\Temp\\y.tmp", 14) &&
           /* VS Code 사용자 데이터: 진짜 로그와 세션 저장소(LevelDB) */
           make_dir(L"roaming") && make_dir(L"roaming\\Code") &&
           make_dir(L"roaming\\Code\\logs") && write_file(L"roaming\\Code\\logs\\main.log", 15) &&
           make_dir(L"roaming\\Code\\Session Storage") &&
           write_file(L"roaming\\Code\\Session Storage\\000004.log", 16) &&
           /* %USERPROFILE%\AppData\LocalLow */
           make_dir(L"home") && make_dir(L"home\\AppData") &&
           make_dir(L"home\\AppData\\LocalLow") && make_dir(L"home\\AppData\\LocalLow\\Vendor") &&
           write_file(L"home\\AppData\\LocalLow\\Vendor\\game.log", 17) &&
           /* 앱 데이터 밖의 LevelDB(Electron 앱 데이터를 프로젝트에 둔 경우 등) */
           make_dir(L"proj") && make_dir(L"proj\\db") &&
           write_file(L"proj\\db\\000005.log", 18) && write_file(L"proj\\db\\other.log", 19) &&
           write_file(L"proj\\db\\CURRENT", 1) && make_dir(L"proj\\db2") &&
           write_file(L"proj\\db2\\debug.log", 20) && write_file(L"proj\\db2\\LOCK", 1) &&
           make_dir(L"proj\\numbered") && write_file(L"proj\\numbered\\123456.log", 21) &&
           /* 일반 로그는 그대로 후보 */
           make_dir(L"proj\\logs") && write_file(L"proj\\logs\\server.log", 22);
}

/* 앱 데이터 폴더와 LevelDB 저장소의 *.log 는 정리 후보가 아니다(데이터 유실 방지). */
static void test_app_data(void)
{
    static const wchar_t *const vars[] = {L"LOCALAPPDATA", L"APPDATA", L"USERPROFILE"};
    static const wchar_t *const rels[] = {L"local", L"roaming", L"home"};
    static const wchar_t *const leveldb_logs[] = {
        L"local\\Google\\Chrome\\leveldb\\000003.log",
        L"roaming\\Code\\Session Storage\\000004.log",
        L"proj\\db\\000005.log",
        L"proj\\db\\other.log",
        L"proj\\db2\\debug.log",
        L"proj\\numbered\\123456.log",
    };
    wchar_t saved[3][MAX_PATH];
    int had[3];
    CleanupProtect protect;
    CleanupResult r;
    CleanupJob *job;
    wchar_t *google;
    size_t i;

    if (!setup_app_data_fixture()) {
        CHECK(0);
        remove_tree(g_root_ext);
        return;
    }
    for (i = 0; i < 3; i++) {
        DWORD n = GetEnvironmentVariableW(vars[i], saved[i], MAX_PATH);
        wchar_t *p = path_join(g_root, rels[i]);
        had[i] = n > 0 && n < MAX_PATH;
        SetEnvironmentVariableW(vars[i], p);
        free(p);
    }
    cleanup_result_init(&r);

    /* 앱 데이터 목록 없이도 LevelDB 쓰기 로그와 LevelDB 폴더의 *.log 는 후보가 아니다 */
    cleanup_protect_init(&protect);
    cleanup_protect_add_system(&protect);
    CHECK(cleanup_scan(&r, g_root, &protect, NULL, NULL, NULL) == SCAN_COMPLETE);
    for (i = 0; i < sizeof(leveldb_logs) / sizeof(leveldb_logs[0]); i++) {
        CHECK(find_item(&r, leveldb_logs[i]) == NULL);
    }
    CHECK(find_item(&r, L"proj\\logs\\server.log") != NULL);
    /* 앱별 폴더의 진짜 로그·도구는 이 목록만으로는 후보가 된다(아래 앱 데이터 목록이 막는다) */
    CHECK(find_item(&r, L"roaming\\Code\\logs\\main.log") != NULL);
    CHECK(find_item(&r, L"local\\JetBrains\\tool\\node_modules") != NULL);
    cleanup_result_free(&r);

    /* 앱 데이터 목록: 앱별 폴더 4개(Google, JetBrains, Code, LocalLow\Vendor), Temp 는 넣지 않음 */
    CHECK_U64(cleanup_protect_add_app_data(&protect), 4);
    CHECK(cleanup_scan(&r, g_root, &protect, NULL, NULL, NULL) == SCAN_COMPLETE);
    CHECK_U64(r.len, 2);
    CHECK(find_item(&r, L"local\\Temp\\y.tmp") != NULL);
    CHECK(find_item(&r, L"proj\\logs\\server.log") != NULL);
    CHECK_U64(r.total_bytes, 14 + 22);
    cleanup_result_free(&r);

    /* 앱 폴더 안을 직접 고르면 보여 주되 보호 표시(정리 판정에서 거부) */
    google = path_join(g_root, L"local\\Google");
    CHECK(cleanup_scan(&r, google, &protect, NULL, NULL, NULL) == SCAN_COMPLETE);
    CHECK(r.root_protected);
    CHECK_U64(r.len, 1);
    CHECK(r.len == 1 && (r.items[0].flags & CLEANUP_ITEM_PROTECTED));
    CHECK(find_item(&r, L"local\\Google\\Chrome\\debug.log") != NULL);
    cleanup_result_free(&r);
    free(google);
    cleanup_protect_free(&protect);

    /* GUI 가 쓰는 작업 스레드도 앱 데이터 목록으로 탐지한다 */
    job = cleanup_job_start(g_root, 0, NULL, NULL);
    CHECK(job != NULL);
    if (job != NULL) {
        CHECK(cleanup_job_finish(job, &r) == SCAN_COMPLETE);
        CHECK_U64(r.len, 2);
        CHECK(find_item(&r, L"local\\Temp\\y.tmp") != NULL);
        CHECK(find_item(&r, L"proj\\logs\\server.log") != NULL);
        cleanup_result_free(&r);
    }

    for (i = 0; i < 3; i++) {
        SetEnvironmentVariableW(vars[i], had[i] ? saved[i] : NULL);
    }
    remove_tree(g_root_ext);
}

/*
 * 드라이브·C:\Users 를 통째로 탐지할 때: 다른 사용자 프로필(Default·Public·다른 계정)과 PEP 514 로
 * 등록된 Python 설치 폴더(Program Files 밖)는 후보가 아니다.
 *   Users\me = %USERPROFILE%, py\Python314 = HKCU\<테스트 키>\TestCo\3.14\InstallPath
 */
static int setup_profiles_fixture(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_cleanup_profiles_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    swprintf(g_root_ext, MAX_PATH, L"\\\\?\\%ls", g_root);
    remove_tree(g_root_ext);
    if (!CreateDirectoryW(g_root_ext, NULL)) {
        return 0;
    }
    return make_dir(L"Users") && make_dir(L"Users\\me") && make_dir(L"Users\\me\\proj") &&
           write_file(L"Users\\me\\proj\\package.json", 2) &&
           make_dir(L"Users\\me\\proj\\node_modules") &&
           write_file(L"Users\\me\\proj\\node_modules\\m.js", 31) &&
           /* 새 계정의 틀, 공용, 다른 계정 */
           make_dir(L"Users\\Default") && make_dir(L"Users\\Default\\AppData") &&
           write_file(L"Users\\Default\\AppData\\helper.log", 32) && make_dir(L"Users\\Public") &&
           make_dir(L"Users\\Public\\Logs") && write_file(L"Users\\Public\\Logs\\daemon.log", 33) &&
           make_dir(L"Users\\other") && make_dir(L"Users\\other\\proj") &&
           write_file(L"Users\\other\\proj\\package.json", 2) &&
           make_dir(L"Users\\other\\proj\\node_modules") &&
           write_file(L"Users\\other\\proj\\node_modules\\o.js", 34) &&
           /* Program Files 밖에 설치한 Python */
           make_dir(L"py") && make_dir(L"py\\Python314") && write_file(L"py\\Python314\\python.exe", 35) &&
           make_dir(L"py\\Python314\\Lib") && make_dir(L"py\\Python314\\Lib\\__pycache__") &&
           write_file(L"py\\Python314\\Lib\\__pycache__\\os.cpython-314.pyc", 36) &&
           /* 일반 프로젝트는 그대로 후보 */
           make_dir(L"work") && write_file(L"work\\CMakeLists.txt", 2) && make_dir(L"work\\build") &&
           write_file(L"work\\build\\a.obj", 37);
}

static void test_profiles_and_python(void)
{
    wchar_t saved_profile[MAX_PATH], key[96], tag_key[160];
    int had_profile;
    DWORD n;
    HKEY h;
    CleanupProtect protect;
    CleanupResult r;
    CleanupJob *job;
    wchar_t *me, *python;

    if (!setup_profiles_fixture()) {
        CHECK(0);
        remove_tree(g_root_ext);
        return;
    }
    n = GetEnvironmentVariableW(L"USERPROFILE", saved_profile, MAX_PATH);
    had_profile = n > 0 && n < MAX_PATH;
    me = path_join(g_root, L"Users\\me");
    python = path_join(g_root, L"py\\Python314");
    SetEnvironmentVariableW(L"USERPROFILE", me);
    {
        wchar_t *users = path_join(g_root, L"Users");
        SetEnvironmentVariableW(L"BYTESIEVE_TEST_PROFILES_DIR", users);
        free(users);
    }

    /* 실제 Python 키(Software\Python)가 아닌 테스트 전용 키에 PEP 514 구조를 만든다. */
    swprintf(key, 96, L"Software\\ByteSieveTest_%lu\\Python", (unsigned long)GetCurrentProcessId());
    swprintf(tag_key, 160, L"%ls\\TestCo\\3.14\\InstallPath", key);
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, tag_key, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL) ==
          ERROR_SUCCESS);
    if (python != NULL) {
        RegSetValueExW(h, NULL, 0, REG_SZ, (const BYTE *)python,
                       (DWORD)((wcslen(python) + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(h);
    SetEnvironmentVariableW(L"BYTESIEVE_TEST_PYTHON_KEY", key);

    cleanup_protect_init(&protect);
    cleanup_protect_add_system(&protect);
    CHECK(python != NULL && cleanup_protect_contains(&protect, python));
    /* 다른 프로필: Default, Public, other */
    CHECK_U64(cleanup_protect_add_other_profiles(&protect), 3);
    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, g_root, &protect, NULL, NULL, NULL) == SCAN_COMPLETE);
    CHECK_U64(r.len, 2);
    CHECK(find_item(&r, L"Users\\me\\proj\\node_modules") != NULL);
    CHECK(find_item(&r, L"work\\build") != NULL);
    CHECK(find_item(&r, L"py\\Python314\\Lib\\__pycache__") == NULL);
    CHECK(find_item(&r, L"Users\\Default\\AppData\\helper.log") == NULL);
    CHECK(find_item(&r, L"Users\\other\\proj\\node_modules") == NULL);
    cleanup_result_free(&r);
    cleanup_protect_free(&protect);

    /* GUI 가 쓰는 작업 스레드도 같다 */
    job = cleanup_job_start(g_root, 0, NULL, NULL);
    CHECK(job != NULL);
    if (job != NULL) {
        CHECK(cleanup_job_finish(job, &r) == SCAN_COMPLETE);
        CHECK_U64(r.len, 2);
        CHECK(find_item(&r, L"Users\\me\\proj\\node_modules") != NULL);
        CHECK(find_item(&r, L"work\\build") != NULL);
        cleanup_result_free(&r);
    }

    SetEnvironmentVariableW(L"BYTESIEVE_TEST_PYTHON_KEY", NULL);
    SetEnvironmentVariableW(L"BYTESIEVE_TEST_PROFILES_DIR", NULL);
    SetEnvironmentVariableW(L"USERPROFILE", had_profile ? saved_profile : NULL);
    swprintf(key, 96, L"Software\\ByteSieveTest_%lu", (unsigned long)GetCurrentProcessId());
    CHECK(RegDeleteTreeW(HKEY_CURRENT_USER, key) == ERROR_SUCCESS);
    RegDeleteKeyW(HKEY_CURRENT_USER, key);
    free(me);
    free(python);
    remove_tree(g_root_ext);
}

/* 개발 도구 설치 경로와 그 안의 실행 파일·하위 폴더는 정리 후보가 아니다. */
static void test_dev_tools(void)
{
    static const wchar_t *const vars[] = {L"LOCALAPPDATA", L"USERPROFILE", L"ProgramFiles"};
    static const wchar_t *const rels[] = {L"local", L"home", L"pf"};
    static const wchar_t *const protected_items[] = {
        L"local\\Programs\\Microsoft VS Code\\debug.log",
        L"local\\Programs\\Microsoft VS Code\\resources\\app\\node_modules",
        L"local\\Programs\\Microsoft VS Code\\resources\\app\\out",
        L"local\\Programs\\Python\\Lib\\__pycache__",
        L"home\\.vscode\\extensions\\ms-python.python-2026.1.0\\node_modules",
        L"pf\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\setup.log",
    };
    wchar_t saved[3][MAX_PATH], install[MAX_PATH + 8];
    int had[3];
    CleanupProtect protect;
    CleanupResult r;
    CleanupJob *job;
    wchar_t *vscode;
    size_t i;

    if (!setup_dev_tools_fixture()) {
        CHECK(0);
        remove_tree(g_root_ext);
        return;
    }
    for (i = 0; i < 3; i++) {
        DWORD n = GetEnvironmentVariableW(vars[i], saved[i], MAX_PATH);
        wchar_t *p = path_join(g_root, rels[i]);
        had[i] = n > 0 && n < MAX_PATH;
        SetEnvironmentVariableW(vars[i], p);
        free(p);
    }

    /* 보호 목록: 시스템 기준 + 레지스트리처럼 따옴표·끝 구분자가 붙은 설치 경로 */
    cleanup_protect_init(&protect);
    cleanup_protect_add_system(&protect);
    swprintf(install, sizeof(install) / sizeof(install[0]), L"\"%ls\\VS2022\\\"", g_root);
    CHECK(cleanup_protect_add_install(&protect, install) == 0);

    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, g_root, &protect, NULL, NULL, NULL) == SCAN_COMPLETE);
    CHECK(!r.root_protected);
    CHECK_U64(r.len, 2);
    CHECK(find_item(&r, L"local\\Temp\\x.tmp") != NULL);
    CHECK(find_item(&r, L"proj\\node_modules") != NULL);
    CHECK_U64(r.total_bytes, 36 + 43);
    /* Programs, .vscode\extensions, pf, VS2022 에는 들어가지 않는다 */
    CHECK_U64(r.report.excluded_dir_count, 4);
    for (i = 0; i < sizeof(protected_items) / sizeof(protected_items[0]); i++) {
        CHECK(find_item(&r, protected_items[i]) == NULL);
    }
    CHECK(find_item(&r, L"VS2022\\Extensions\\y\\node_modules") == NULL);
    CHECK(find_item(&r, L"VS2022\\Python\\__pycache__") == NULL);
    cleanup_result_free(&r);

    /* 설치 폴더 안을 직접 고르면 항목마다 보호 표시가 붙는다(정리 판정에서 거부) */
    vscode = path_join(g_root, L"local\\Programs\\Microsoft VS Code");
    CHECK(cleanup_scan(&r, vscode, &protect, NULL, NULL, NULL) == SCAN_COMPLETE);
    CHECK(r.root_protected);
    CHECK_U64(r.len, 3);
    for (i = 0; i < r.len; i++) {
        CHECK(r.items[i].flags & CLEANUP_ITEM_PROTECTED);
    }
    cleanup_result_free(&r);
    free(vscode);
    cleanup_protect_free(&protect);

    /* GUI 가 쓰는 작업 스레드도 같은 보호 목록(cleanup_protect_add_system)으로 탐지한다 */
    job = cleanup_job_start(g_root, 0, NULL, NULL);
    CHECK(job != NULL);
    if (job != NULL) {
        CHECK(cleanup_job_finish(job, &r) == SCAN_COMPLETE);
        CHECK(find_item(&r, L"proj\\node_modules") != NULL);
        for (i = 0; i < sizeof(protected_items) / sizeof(protected_items[0]); i++) {
            CHECK(find_item(&r, protected_items[i]) == NULL);
        }
        cleanup_result_free(&r);
    }

    for (i = 0; i < 3; i++) {
        SetEnvironmentVariableW(vars[i], had[i] ? saved[i] : NULL);
    }
    remove_tree(g_root_ext);
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
    test_ecosystems();
    test_dev_tools();
    test_app_data();
    test_profiles_and_python();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all cleanup_scan tests passed\n");
    return 0;
}
