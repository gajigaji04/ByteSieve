#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cleanup_scan.h"
#include "cleanup_trash.h"
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

#define CHECK_STATUS(actual, expected)                                          \
    do {                                                                        \
        CleanupTrashStatus a_ = (actual), e_ = (expected);                      \
        if (a_ != e_) {                                                         \
            fprintf(stderr, "%s:%d: %s: expected %ls, got %ls\n", __FILE__,     \
                    __LINE__, #actual, cleanup_trash_status_name(e_),           \
                    cleanup_trash_status_name(a_));                             \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

static wchar_t g_root[MAX_PATH];

/* ---- 경로 판정(파일 시스템 접근 없음) ---- */

static CleanupItem make_item(const wchar_t *path, uint32_t flags)
{
    CleanupItem item;

    memset(&item, 0, sizeof(item));
    item.path = (wchar_t *)path;
    item.category = CLEANUP_NODE_MODULES;
    item.flags = flags;
    return item;
}

static CleanupTrashStatus check(const CleanupGuard *g, const wchar_t *root, const wchar_t *path,
                                uint32_t flags)
{
    CleanupItem item = make_item(path, flags);
    return cleanup_guard_check(g, root, &item);
}

static void test_guard_rules(void)
{
    CleanupGuard g;
    wchar_t longp[MAX_PATH + 16];
    size_t i;
    const uint32_t D = CLEANUP_ITEM_DIR;

    cleanup_guard_init(&g);
    CHECK(cleanup_protect_add(&g.protect, L"C:\\Windows") == 0);
    CHECK(cleanup_protect_add(&g.critical, L"C:\\Users\\me") == 0);
    CHECK(cleanup_protect_add(&g.critical, L"C:\\Users\\me\\Desktop") == 0);
    CHECK(cleanup_protect_add(&g.critical, L"D:\\tools\\app") == 0);

    /* 허용: 탐지 경로 안쪽의 일반 항목(끝 구분자·대소문자 무관) */
    CHECK_STATUS(check(&g, L"C:\\Users\\me\\proj", L"C:\\Users\\me\\proj\\node_modules", D),
                 CLEANUP_TRASH_PENDING);
    CHECK_STATUS(check(&g, L"C:\\Users\\me\\proj\\", L"C:\\Users\\me\\proj\\a.log", 0),
                 CLEANUP_TRASH_PENDING);
    CHECK_STATUS(check(&g, L"c:\\USERS\\ME\\PROJ", L"C:\\Users\\me\\proj\\dist", D),
                 CLEANUP_TRASH_PENDING);
    CHECK_STATUS(check(&g, L"D:\\", L"D:\\tools\\app\\build", D), CLEANUP_TRASH_PENDING);
    CHECK_STATUS(check(&g, L"\\\\?\\C:\\work", L"C:\\work\\out", D), CLEANUP_TRASH_PENDING);

    /* 탐지 경로 밖·탐지 경로 자체·이름만 비슷한 형제 폴더 */
    CHECK_STATUS(check(&g, L"C:\\Users\\me\\proj", L"C:\\Users\\me\\proj", D),
                 CLEANUP_TRASH_DENY_OUTSIDE);
    CHECK_STATUS(check(&g, L"C:\\Users\\me\\proj", L"C:\\Users\\me\\proj2\\build", D),
                 CLEANUP_TRASH_DENY_OUTSIDE);
    CHECK_STATUS(check(&g, L"C:\\Users\\me\\proj", L"D:\\Users\\me\\proj\\build", D),
                 CLEANUP_TRASH_DENY_OUTSIDE);
    CHECK_STATUS(check(&g, NULL, L"C:\\x\\build", D), CLEANUP_TRASH_DENY_OUTSIDE);

    /* 시스템 보호 경로와 그 아래, 볼륨 루트 시스템 폴더, 보호 표시가 붙은 항목 */
    CHECK_STATUS(check(&g, L"C:\\", L"C:\\Windows\\Temp\\.tmp", D),
                 CLEANUP_TRASH_DENY_PROTECTED);
    CHECK_STATUS(check(&g, L"C:\\", L"C:\\windows", D), CLEANUP_TRASH_DENY_PROTECTED);
    CHECK_STATUS(check(&g, L"C:\\", L"C:\\$Recycle.Bin\\x\\build", D),
                 CLEANUP_TRASH_DENY_PROTECTED);
    CHECK_STATUS(check(&g, L"C:\\work", L"C:\\work\\build", D | CLEANUP_ITEM_PROTECTED),
                 CLEANUP_TRASH_DENY_PROTECTED);
    CHECK_STATUS(check(&g, L"C:\\", L"C:\\WindowsApps\\build", D), CLEANUP_TRASH_PENDING);

    /* 중요 경로 자체와 그 상위(보호 경로의 상위 포함) */
    CHECK_STATUS(check(&g, L"C:\\", L"C:\\Users", D), CLEANUP_TRASH_DENY_CRITICAL);
    CHECK_STATUS(check(&g, L"C:\\Users", L"C:\\Users\\me", D), CLEANUP_TRASH_DENY_CRITICAL);
    CHECK_STATUS(check(&g, L"C:\\Users\\me", L"C:\\Users\\me\\Desktop", D),
                 CLEANUP_TRASH_DENY_CRITICAL);
    CHECK_STATUS(check(&g, L"D:\\", L"D:\\tools", D), CLEANUP_TRASH_DENY_CRITICAL);
    CHECK_STATUS(check(&g, L"D:\\tools", L"D:\\tools\\app", D), CLEANUP_TRASH_DENY_CRITICAL);
    CHECK_STATUS(check(&g, L"C:\\Users\\me", L"C:\\Users\\me\\Desktop2", D),
                 CLEANUP_TRASH_PENDING);

    /* 하위를 다 읽지 못한 항목 */
    CHECK_STATUS(check(&g, L"C:\\work", L"C:\\work\\node_modules", D | CLEANUP_ITEM_INCOMPLETE),
                 CLEANUP_TRASH_DENY_INCOMPLETE);

    /* 휴지통이 다룰 수 없는 경로: 네트워크, 상대 경로, MAX_PATH 이상, NULL */
    CHECK_STATUS(check(&g, L"\\\\server\\share", L"\\\\server\\share\\build", D),
                 CLEANUP_TRASH_DENY_PATH);
    CHECK_STATUS(check(&g, L"\\\\?\\UNC\\server\\share", L"\\\\?\\UNC\\server\\share\\out", D),
                 CLEANUP_TRASH_DENY_PATH);
    CHECK_STATUS(check(&g, L"proj", L"proj\\node_modules", D), CLEANUP_TRASH_DENY_PATH);
    wcscpy_s(longp, MAX_PATH + 16, L"C:\\work\\");
    for (i = wcslen(longp); i < MAX_PATH; i++) {
        longp[i] = L'a';
    }
    longp[MAX_PATH] = L'\0';
    CHECK_STATUS(check(&g, L"C:\\work", longp, 0), CLEANUP_TRASH_DENY_PATH);
    CHECK_STATUS(cleanup_guard_check(&g, L"C:\\", NULL), CLEANUP_TRASH_DENY_PATH);

    /* 보호 목록 없이도 볼륨 루트 시스템 폴더와 경로 규칙은 지킨다 */
    CHECK_STATUS(check(NULL, L"C:\\", L"C:\\System Volume Information\\x", D),
                 CLEANUP_TRASH_DENY_PROTECTED);
    CHECK_STATUS(check(NULL, L"C:\\a", L"C:\\a\\b", D), CLEANUP_TRASH_PENDING);
    cleanup_guard_free(&g);

    CHECK(wcscmp(cleanup_trash_status_name(CLEANUP_TRASH_MOVED), L"moved") == 0);
    CHECK(cleanup_trash_is_denied(CLEANUP_TRASH_CHANGED));
    CHECK(!cleanup_trash_is_denied(CLEANUP_TRASH_FAILED));
    CHECK(!cleanup_trash_is_denied(CLEANUP_TRASH_MOVED));
}

/* 현재 시스템의 보호 목록: Windows 폴더, 사용자 프로필, 실행 파일 폴더. */
static void test_guard_system(void)
{
    CleanupGuard g;
    wchar_t win[MAX_PATH], exe[MAX_PATH], profile[MAX_PATH], *slash;

    cleanup_guard_init(&g);
    CHECK(cleanup_guard_add_system(&g) > 0);

    CHECK(GetWindowsDirectoryW(win, MAX_PATH) > 0);
    win[3] = L'\0'; /* "C:\" */
    GetWindowsDirectoryW(exe, MAX_PATH);
    CHECK_STATUS(check(&g, win, exe, CLEANUP_ITEM_DIR), CLEANUP_TRASH_DENY_PROTECTED);

    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        wchar_t parent[MAX_PATH];
        wcscpy_s(parent, MAX_PATH, profile);
        slash = wcsrchr(parent, L'\\');
        if (slash != NULL) {
            *slash = L'\0';
            CHECK_STATUS(check(&g, parent, profile, CLEANUP_ITEM_DIR),
                         CLEANUP_TRASH_DENY_CRITICAL);
        }
    }

    /* build\Debug\test_cleanup_trash.exe: build 를 탐지했을 때 Debug 는 지울 수 없다. */
    CHECK(GetModuleFileNameW(NULL, exe, MAX_PATH) > 0);
    slash = wcsrchr(exe, L'\\');
    if (slash != NULL) {
        *slash = L'\0'; /* exe 폴더 */
        wcscpy_s(profile, MAX_PATH, exe);
        slash = wcsrchr(profile, L'\\');
        if (slash != NULL) {
            *slash = L'\0'; /* 그 부모 */
            CHECK_STATUS(check(&g, profile, exe, CLEANUP_ITEM_DIR), CLEANUP_TRASH_DENY_CRITICAL);
        }
    }
    cleanup_guard_free(&g);
}

/* ---- 픽스처 ---- */

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

static int write_file(const wchar_t *name, DWORD len)
{
    static const char data[64] = {0};
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

static int exists(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    int ok = p != NULL && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
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
 * 테스트가 휴지통에 넣은 항목을 지워 사용자 휴지통을 어지럽히지 않는다.
 * 휴지통 항목은 내용($R...)과 정보 파일($I...)로 이루어진다.
 */
static void purge_recycled(const wchar_t *recycled)
{
    wchar_t info[MAX_PATH];
    wchar_t *name;
    DWORD attrs;

    if (recycled == NULL || wcslen(recycled) >= MAX_PATH) {
        return;
    }
    /* 휴지통 밖은 절대 지우지 않는다. */
    if (wcsstr(recycled, L"\\$Recycle.Bin\\") == NULL &&
        wcsstr(recycled, L"\\$RECYCLE.BIN\\") == NULL) {
        fprintf(stderr, "unexpected recycled path: %ls\n", recycled);
        g_failures++;
        return;
    }
    attrs = GetFileAttributesW(recycled);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        remove_tree(recycled);
    } else {
        DeleteFileW(recycled);
    }
    wcscpy_s(info, MAX_PATH, recycled);
    name = wcsrchr(info, L'\\');
    if (name != NULL && name[1] == L'$' && (name[2] == L'R' || name[2] == L'r')) {
        name[2] = L'I';
        DeleteFileW(info);
    }
}

/*
 * root\
 *   app\package.json           2
 *   app\node_modules\a\i.js    10   → 휴지통
 *   app\debug.log              3    → 휴지통
 *   app\build\o.bin            7    → 휴지통
 *   app\sys\build\z            1    보호 경로로 지정(거부)
 *   app\out                    파일인데 폴더 항목으로 요청(바뀜)
 *   app\real\node_modules\f    1
 *   app\via -> app\real        정션: via\node_modules 는 실제 위치가 다름(바뀜)
 *   app\link -> app\real       정션 자체를 항목으로 요청(바뀜)
 *   later\dist\d, later\.cache\c, later\x.tmp  작업 스레드·취소 시험용
 */
static int setup_fixture(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_trash_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    if (!CreateDirectoryW(g_root, NULL)) {
        return 0;
    }
    return make_dir(L"app") && write_file(L"app\\package.json", 2) &&
           make_dir(L"app\\node_modules") && make_dir(L"app\\node_modules\\a") &&
           write_file(L"app\\node_modules\\a\\i.js", 10) && write_file(L"app\\debug.log", 3) &&
           make_dir(L"app\\build") && write_file(L"app\\build\\o.bin", 7) &&
           make_dir(L"app\\sys") && make_dir(L"app\\sys\\build") &&
           write_file(L"app\\sys\\build\\z", 1) && write_file(L"app\\out", 4) &&
           make_dir(L"app\\real") && make_dir(L"app\\real\\node_modules") &&
           write_file(L"app\\real\\node_modules\\f", 1) &&
           make_junction(L"app\\via", L"app\\real") && make_junction(L"app\\link", L"app\\real") &&
           make_dir(L"later") && make_dir(L"later\\dist") && write_file(L"later\\dist\\d", 5) &&
           make_dir(L"later\\.cache") && write_file(L"later\\.cache\\c", 6) &&
           write_file(L"later\\x.tmp", 2);
}

static const CleanupItem *find_item(const CleanupResult *r, const wchar_t *rel)
{
    wchar_t *want = fixture(rel);
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

/* 픽스처 경로로 항목을 직접 만든다(탐지 결과에 없는 상황 재현용). */
static CleanupTrashStatus trash_rel(const CleanupGuard *g, const wchar_t *rel, uint32_t flags,
                                    CleanupTrashOutcome *out)
{
    wchar_t *p = fixture(rel);
    CleanupItem item = make_item(p, flags);
    CleanupTrashStatus s = cleanup_trash_one(g, g_root, &item, out);
    free(p);
    return s;
}

/* 탐지 결과를 그대로 휴지통으로 옮긴다: 원본은 사라지고 휴지통에 있어야 한다. */
static void test_trash_detected(void)
{
    CleanupResult r;
    CleanupGuard g;
    CleanupProtect protect;
    CleanupTrashOutcome o;
    const wchar_t *names[] = {L"app\\node_modules", L"app\\debug.log", L"app\\build"};
    wchar_t *sys = fixture(L"app\\sys");
    wchar_t *app = fixture(L"app");
    size_t i;

    cleanup_result_init(&r);
    cleanup_protect_init(&protect);
    cleanup_guard_init(&g);
    cleanup_guard_add_system(&g);
    memset(&o, 0, sizeof(o));

    cleanup_protect_add(&protect, sys);
    CHECK(cleanup_scan(&r, app, &protect, NULL, NULL, NULL) == SCAN_COMPLETE);

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const CleanupItem *item = find_item(&r, names[i]);
        CHECK(item != NULL);
        if (item == NULL) {
            continue;
        }
        CHECK_STATUS(cleanup_guard_check(&g, r.root, item), CLEANUP_TRASH_PENDING);
        CHECK_STATUS(cleanup_trash_one(&g, r.root, item, &o), CLEANUP_TRASH_MOVED);
        CHECK(o.status == CLEANUP_TRASH_MOVED && o.code == 0);
        CHECK(!exists(names[i]));
        /* 영구 삭제가 아니라 휴지통 안에 있다 */
        CHECK(o.recycled_path != NULL);
        if (o.recycled_path != NULL) {
            DWORD attrs = GetFileAttributesW(o.recycled_path);
            CHECK(attrs != INVALID_FILE_ATTRIBUTES);
            CHECK(((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) ==
                  ((item->flags & CLEANUP_ITEM_DIR) != 0));
            purge_recycled(o.recycled_path);
        }
        cleanup_trash_outcome_free(&o);
    }
    /* 프로젝트 표시 파일과 다른 파일은 그대로 */
    CHECK(exists(L"app\\package.json"));
    CHECK(exists(L"app\\sys\\build\\z"));

    /* 이미 옮긴 항목을 다시 요청하면 '이미 없음' */
    if (r.len > 0) {
        const CleanupItem *item = find_item(&r, L"app\\build");
        if (item != NULL) {
            CHECK_STATUS(cleanup_trash_one(&g, r.root, item, &o), CLEANUP_TRASH_MISSING);
            CHECK(o.recycled_path == NULL);
        }
    }
    cleanup_trash_outcome_free(&o);
    cleanup_guard_free(&g);
    cleanup_protect_free(&protect);
    cleanup_result_free(&r);
    free(sys);
    free(app);
}

/* 거부되는 항목은 파일 시스템에서 아무것도 바뀌지 않는다. */
static void test_trash_denied(void)
{
    CleanupGuard g;
    CleanupTrashOutcome o;
    wchar_t *sys = fixture(L"app\\sys");

    cleanup_guard_init(&g);
    cleanup_guard_add_system(&g);
    cleanup_protect_add(&g.protect, sys);
    memset(&o, 0, sizeof(o));

    CHECK_STATUS(trash_rel(&g, L"app\\sys\\build", CLEANUP_ITEM_DIR, &o),
                 CLEANUP_TRASH_DENY_PROTECTED);
    CHECK(exists(L"app\\sys\\build\\z"));
    CHECK_STATUS(trash_rel(&g, L"app\\sys", CLEANUP_ITEM_DIR, &o), CLEANUP_TRASH_DENY_PROTECTED);

    /* 폴더였던 항목이 파일이 됨 */
    CHECK_STATUS(trash_rel(&g, L"app\\out", CLEANUP_ITEM_DIR, &o), CLEANUP_TRASH_CHANGED);
    CHECK(exists(L"app\\out"));
    /* 항목 자체가 정션 */
    CHECK_STATUS(trash_rel(&g, L"app\\link", CLEANUP_ITEM_DIR, &o), CLEANUP_TRASH_CHANGED);
    CHECK(exists(L"app\\link"));
    /* 중간 폴더가 정션이라 실제 위치가 다름(app\real\node_modules) */
    CHECK_STATUS(trash_rel(&g, L"app\\via\\node_modules", CLEANUP_ITEM_DIR, &o),
                 CLEANUP_TRASH_CHANGED);
    CHECK(exists(L"app\\real\\node_modules\\f"));
    /* 없는 항목, 하위를 다 읽지 못한 항목, 탐지 경로 자체 */
    CHECK_STATUS(trash_rel(&g, L"app\\nothing", CLEANUP_ITEM_DIR, &o), CLEANUP_TRASH_MISSING);
    CHECK_STATUS(trash_rel(&g, L"app\\real\\node_modules",
                           CLEANUP_ITEM_DIR | CLEANUP_ITEM_INCOMPLETE, &o),
                 CLEANUP_TRASH_DENY_INCOMPLETE);
    CHECK(exists(L"app\\real\\node_modules\\f"));
    CHECK_STATUS(trash_rel(&g, L"", CLEANUP_ITEM_DIR, &o), CLEANUP_TRASH_DENY_OUTSIDE);
    CHECK(o.recycled_path == NULL);

    cleanup_trash_outcome_free(&o);
    cleanup_guard_free(&g);
    free(sys);
}

static volatile LONG g_notified;
static volatile LONG g_finished;

static void job_notify(int finished, void *ctx)
{
    (void)ctx;
    InterlockedIncrement(&g_notified);
    if (finished) {
        InterlockedIncrement(&g_finished);
    }
}

/* 작업 스레드: 결과 순서·집계, 거부 항목 섞기, 취소. */
static void test_job(void)
{
    CleanupResult r;
    CleanupTrashJob *job;
    CleanupTrashSnapshot snap;
    CleanupTrashOutcome *out = NULL;
    const CleanupItem *items[4];
    CleanupItem missing;
    wchar_t *later = fixture(L"later");
    wchar_t *missing_path = fixture(L"later\\gone");
    size_t n, i, count = 0;
    uint64_t moved_bytes = 0;

    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, later, NULL, NULL, NULL, NULL) == SCAN_COMPLETE);
    CHECK(r.len == 3);
    for (i = 0; i < r.len && count < 3; i++) {
        items[count++] = &r.items[i];
        moved_bytes += r.items[i].bytes;
    }
    missing = make_item(missing_path, CLEANUP_ITEM_DIR);
    items[count++] = &missing;

    g_notified = 0;
    g_finished = 0;
    job = cleanup_trash_job_start(r.root, items, count, 0, job_notify, NULL);
    CHECK(job != NULL);
    if (job != NULL) {
        n = cleanup_trash_job_finish(job, &out);
        CHECK(n == count);
        CHECK(g_finished == 1 && g_notified >= 1);
        CHECK(out != NULL);
        for (i = 0; out != NULL && i + 1 < n; i++) {
            CHECK_STATUS(out[i].status, CLEANUP_TRASH_MOVED);
            CHECK(GetFileAttributesW(items[i]->path) == INVALID_FILE_ATTRIBUTES);
            purge_recycled(out[i].recycled_path);
        }
        if (out != NULL && n > 0) {
            CHECK_STATUS(out[n - 1].status, CLEANUP_TRASH_MISSING);
        }
        cleanup_trash_outcomes_free(out, n);
    }
    CHECK(cleanup_trash_job_finish(NULL, NULL) == 0);

    /* 시작 직후 취소: 처리된 항목은 옮겨졌고, 나머지는 손대지 않았다. */
    CHECK(make_dir(L"later\\dist") && write_file(L"later\\dist\\d", 5));
    CHECK(make_dir(L"later\\.cache") && write_file(L"later\\.cache\\c", 6));
    CHECK(write_file(L"later\\x.tmp", 2));
    job = cleanup_trash_job_start(r.root, items, 3, 0, NULL, NULL);
    CHECK(job != NULL);
    if (job != NULL) {
        size_t moved = 0;
        cleanup_trash_job_cancel(job);
        cleanup_trash_job_progress(job, &snap);
        CHECK(snap.cancel_requested && snap.total == 3);
        n = cleanup_trash_job_finish(job, &out);
        CHECK(n == 3 && out != NULL);
        for (i = 0; out != NULL && i < n; i++) {
            DWORD attrs = GetFileAttributesW(items[i]->path);
            CHECK(out[i].status == CLEANUP_TRASH_MOVED || out[i].status == CLEANUP_TRASH_CANCELLED);
            if (out[i].status == CLEANUP_TRASH_MOVED) {
                moved++;
                CHECK(attrs == INVALID_FILE_ATTRIBUTES);
                purge_recycled(out[i].recycled_path);
            } else {
                CHECK(attrs != INVALID_FILE_ATTRIBUTES);
            }
        }
        /* 취소 뒤에 다시 옮기는 일은 없다: 옮긴 항목은 앞쪽에 몰려 있다 */
        for (i = moved; out != NULL && i < n; i++) {
            CHECK_STATUS(out[i].status, CLEANUP_TRASH_CANCELLED);
        }
        cleanup_trash_outcomes_free(out, n);
    }
    (void)moved_bytes;
    cleanup_result_free(&r);
    free(later);
    free(missing_path);
}

/* 진행률 스냅샷이 결과와 맞는지(끝난 뒤 한 번 더 읽는다). */
static void test_job_snapshot(void)
{
    CleanupTrashJob *job;
    CleanupTrashSnapshot snap;
    CleanupItem a, b;
    const CleanupItem *items[2];
    wchar_t *pa, *pb;

    CHECK(make_dir(L"snap") && write_file(L"snap\\a.log", 9));
    pa = fixture(L"snap\\a.log");
    pb = fixture(L"snap\\b.log");
    a = make_item(pa, 0);
    a.category = CLEANUP_LOG;
    a.bytes = 9;
    b = make_item(pb, 0);
    b.bytes = 100;
    items[0] = &a;
    items[1] = &b;

    job = cleanup_trash_job_start(g_root, items, 2, 0, NULL, NULL);
    CHECK(job != NULL);
    if (job != NULL) {
        CleanupTrashOutcome *out = NULL;
        size_t n;
        /* finish 전에 끝까지 기다린 상태를 본다 */
        while (1) {
            cleanup_trash_job_progress(job, &snap);
            if (snap.done == snap.total) {
                break;
            }
            Sleep(5);
        }
        CHECK(snap.total == 2 && snap.moved == 1 && snap.failed == 1);
        CHECK(snap.moved_bytes == 9);
        n = cleanup_trash_job_finish(job, &out);
        CHECK(n == 2 && out != NULL);
        if (out != NULL) {
            CHECK_STATUS(out[0].status, CLEANUP_TRASH_MOVED);
            CHECK_STATUS(out[1].status, CLEANUP_TRASH_MISSING);
            purge_recycled(out[0].recycled_path);
        }
        cleanup_trash_outcomes_free(out, n);
    }
    free(pa);
    free(pb);
}

int main(void)
{
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    test_guard_rules();
    test_guard_system();
    if (FAILED(init) || !setup_fixture()) {
        fprintf(stderr, "fixture setup failed (error %lu)\n", GetLastError());
        remove_tree(g_root);
        return 1;
    }
    test_trash_detected();
    test_trash_denied();
    test_job();
    test_job_snapshot();
    remove_tree(g_root);
    CoUninitialize();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all cleanup_trash tests passed\n");
    return 0;
}
