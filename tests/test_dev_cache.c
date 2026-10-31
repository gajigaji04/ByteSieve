#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cleanup_trash.h"
#include "dev_cache.h"
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

static wchar_t g_root[MAX_PATH];

/* 탐지가 보는 환경 변수(테스트가 바꾸고 끝에 지운다). */
static const wchar_t *const g_tool_env[] = {
    L"npm_config_cache", L"NPM_CONFIG_USERCONFIG", L"PIP_CACHE_DIR", L"PIP_CONFIG_FILE",
    L"GRADLE_USER_HOME", L"CARGO_HOME",            L"NUGET_PACKAGES", L"NUGET_HTTP_CACHE_PATH",
    NULL};

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

/* 경로의 상위 폴더를 모두 만든 뒤 len 바이트 파일을 쓴다(name 이 '\' 로 끝나면 폴더만). */
static int make(const wchar_t *name, DWORD len)
{
    static char data[4096];
    wchar_t *p = fixture(name), *s;
    size_t n;
    HANDLE h;
    DWORD written = 0;
    BOOL ok;

    if (p == NULL) {
        return 0;
    }
    for (s = p + wcslen(g_root) + 1; *s != L'\0'; s++) {
        if (*s == L'\\') {
            *s = L'\0';
            CreateDirectoryW(p, NULL);
            *s = L'\\';
        }
    }
    n = wcslen(p);
    if (n > 0 && p[n - 1] == L'\\') {
        free(p);
        return 1;
    }
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(p);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = len == 0 || WriteFile(h, data, len, &written, NULL);
    CloseHandle(h);
    return ok && written == len;
}

static int write_text(const wchar_t *name, const char *text)
{
    wchar_t *p = fixture(name);
    HANDLE h;
    DWORD written = 0;
    BOOL ok;

    if (p == NULL || !make(name, 0)) {
        free(p);
        return 0;
    }
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(p);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
    CloseHandle(h);
    return ok && written == strlen(text);
}

static void set_env(const wchar_t *name, const wchar_t *rel)
{
    wchar_t *p = rel != NULL ? fixture(rel) : NULL;

    SetEnvironmentVariableW(name, p);
    free(p);
}

static void clear_tool_env(void)
{
    size_t i;

    for (i = 0; g_tool_env[i] != NULL; i++) {
        SetEnvironmentVariableW(g_tool_env[i], NULL);
    }
}

static int make_junction(const wchar_t *link, const wchar_t *target)
{
    wchar_t cmd[3 * MAX_PATH];
    swprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"cmd /c mklink /J \"%ls\\%ls\" \"%ls\\%ls\" >nul",
             g_root, link, g_root, target);
    return _wsystem(cmd) == 0;
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

/* 종류의 n 번째 항목. 없으면 NULL. */
static DevCache *find(DevCacheList *l, DevCacheKind kind, size_t nth)
{
    size_t i;

    for (i = 0; i < l->len; i++) {
        if (l->items[i].kind == kind && nth-- == 0) {
            return &l->items[i];
        }
    }
    return NULL;
}

/* path 가 g_root\rel 과 같은지. */
static int is_at(const wchar_t *path, const wchar_t *rel)
{
    wchar_t *want = fixture(rel);
    int same = path != NULL && want != NULL && _wcsicmp(path, want) == 0;

    free(want);
    return same;
}

static size_t count_kind(const DevCacheList *l, DevCacheKind kind)
{
    size_t i, n = 0;

    for (i = 0; i < l->len; i++) {
        n += l->items[i].kind == kind;
    }
    return n;
}

/* 아무것도 없으면 종류마다 기본 위치 하나씩 NOT_FOUND. */
static void test_nothing(void)
{
    DevCacheList l;
    size_t i;

    dev_cache_list_init(&l);
    CHECK(dev_cache_detect(&l) == 0);
    CHECK(l.len == DEV_CACHE_KIND_COUNT);
    for (i = 0; i < l.len; i++) {
        CHECK(l.items[i].status == DEV_CACHE_NOT_FOUND);
        CHECK(l.items[i].source == DEV_CACHE_SRC_DEFAULT && l.items[i].origin == NULL);
        CHECK(l.items[i].kind == (DevCacheKind)i);
    }
    CHECK(is_at(find(&l, DEV_CACHE_NPM, 0)->path, L"local\\npm-cache"));
    CHECK(is_at(find(&l, DEV_CACHE_PIP, 0)->path, L"local\\pip\\Cache"));
    CHECK(is_at(find(&l, DEV_CACHE_GRADLE, 0)->path, L"home\\.gradle\\caches"));
    CHECK(is_at(find(&l, DEV_CACHE_GRADLE, 0)->home, L"home\\.gradle"));
    CHECK(is_at(find(&l, DEV_CACHE_GRADLE_WRAPPER, 0)->path, L"home\\.gradle\\wrapper\\dists"));
    CHECK(is_at(find(&l, DEV_CACHE_CARGO_REGISTRY, 0)->path, L"home\\.cargo\\registry"));
    CHECK(is_at(find(&l, DEV_CACHE_CARGO_GIT, 0)->path, L"home\\.cargo\\git"));
    CHECK(is_at(find(&l, DEV_CACHE_NUGET_PACKAGES, 0)->path, L"home\\.nuget\\packages"));
    CHECK(is_at(find(&l, DEV_CACHE_NUGET_HTTP, 0)->path, L"local\\NuGet\\v3-cache"));
    dev_cache_list_free(&l);
}

/* 기본 위치에 모두 있을 때: 상태·용량·파일 수, 도구 폴더 자체와 설정·도구는 항목이 아님. */
static void test_defaults(void)
{
    DevCacheList l;
    CleanupGuard guard;
    size_t i;
    DevCache *c;

    CHECK(make(L"local\\npm-cache\\_cacache\\index-v5\\a", 100) &&
          make(L"local\\npm-cache\\_cacache\\content-v2\\b", 1000));
    CHECK(make(L"local\\pip\\Cache\\http-v2\\x", 200));
    CHECK(make(L"home\\.gradle\\caches\\modules-2\\m.jar", 300) &&
          make(L"home\\.gradle\\gradle.properties", 10));
    CHECK(make(L"home\\.gradle\\wrapper\\dists\\gradle-8.5-bin\\g.zip", 400));
    CHECK(make(L"home\\.cargo\\registry\\cache\\c.crate", 500) &&
          make(L"home\\.cargo\\git\\db\\d", 50) && make(L"home\\.cargo\\bin\\cargo.exe", 900));
    CHECK(make(L"home\\.nuget\\packages\\newtonsoft.json\\13.0.1\\.nupkg.metadata", 60));
    CHECK(make(L"local\\NuGet\\v3-cache\\abc$ps_api\\x.dat", 70) &&
          make(L"local\\NuGet\\http-cache\\def$ps_api\\y.dat", 80));

    dev_cache_list_init(&l);
    cleanup_guard_init(&guard);
    cleanup_guard_add_system(&guard);
    CHECK(dev_cache_detect(&l) == 0);
    /* NuGet HTTP 캐시는 v3-cache·http-cache 둘 다 */
    CHECK(l.len == DEV_CACHE_KIND_COUNT + 1 && count_kind(&l, DEV_CACHE_NUGET_HTTP) == 2);
    for (i = 0; i < l.len; i++) {
        c = &l.items[i];
        CHECK(c->status == DEV_CACHE_FOUND);
        CHECK(!c->measured);
        /* 세기 전에는 정리하지 않는다 */
        CHECK(dev_cache_check(&guard, c) == CLEANUP_TRASH_DENY_INCOMPLETE);
        dev_cache_measure(c, NULL, NULL);
        CHECK(c->measured && !c->incomplete);
        CHECK(dev_cache_check(&guard, c) == CLEANUP_TRASH_PENDING);
        /* 도구 폴더 자체·설치한 도구는 항목이 아니다 */
        CHECK(!is_at(c->path, L"home\\.gradle") && !is_at(c->path, L"home\\.cargo") &&
              !is_at(c->path, L"home\\.nuget") && wcsstr(c->path, L"\\bin") == NULL);
    }
    c = find(&l, DEV_CACHE_NPM, 0);
    CHECK(c->bytes == 1100 && c->files == 2 && c->dirs == 3);
    CHECK(find(&l, DEV_CACHE_PIP, 0)->bytes == 200);
    CHECK(find(&l, DEV_CACHE_GRADLE, 0)->bytes == 300); /* gradle.properties 미포함 */
    CHECK(find(&l, DEV_CACHE_GRADLE_WRAPPER, 0)->bytes == 400);
    CHECK(find(&l, DEV_CACHE_CARGO_REGISTRY, 0)->bytes == 500); /* bin 미포함 */
    CHECK(find(&l, DEV_CACHE_CARGO_GIT, 0)->bytes == 50);
    CHECK(find(&l, DEV_CACHE_NUGET_PACKAGES, 0)->bytes == 60);
    CHECK(is_at(find(&l, DEV_CACHE_NUGET_HTTP, 0)->path, L"local\\NuGet\\v3-cache") &&
          find(&l, DEV_CACHE_NUGET_HTTP, 0)->bytes == 70);
    CHECK(is_at(find(&l, DEV_CACHE_NUGET_HTTP, 1)->path, L"local\\NuGet\\http-cache") &&
          find(&l, DEV_CACHE_NUGET_HTTP, 1)->bytes == 80);

    /* 정리용 항목: 폴더·개발 캐시·환경 */
    {
        CleanupItem item;
        c = find(&l, DEV_CACHE_CARGO_REGISTRY, 0);
        dev_cache_to_item(c, &item);
        CHECK(item.path == c->path && item.category == CLEANUP_CACHE &&
              item.eco == CLEANUP_ECO_RUST && item.flags == CLEANUP_ITEM_DIR &&
              item.bytes == 500 && item.files == 1);
    }
    cleanup_guard_free(&guard);
    dev_cache_list_free(&l);
}

static int always_cancel(void *ctx)
{
    (void)ctx;
    return 1;
}

static void test_cancel_and_guard(void)
{
    DevCacheList l;
    CleanupGuard guard;
    DevCache fake;
    PWSTR profile = NULL;
    DevCache *c;

    dev_cache_list_init(&l);
    cleanup_guard_init(&guard);
    cleanup_guard_add_system(&guard);
    CHECK(dev_cache_detect(&l) == 0);
    c = find(&l, DEV_CACHE_NPM, 0);
    dev_cache_measure(c, always_cancel, NULL);
    CHECK(c->measured && c->incomplete);
    CHECK(dev_cache_check(&guard, c) == CLEANUP_TRASH_DENY_INCOMPLETE);

    /* 캐시 위치가 중요 경로(사용자 프로필)를 가리키면 정리하지 않는다 */
    CHECK(SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Profile, 0, NULL, &profile)));
    if (profile != NULL) {
        wchar_t *parent = path_dup(profile);
        wchar_t *slash = parent != NULL ? wcsrchr(parent, L'\\') : NULL;
        if (slash != NULL) {
            *slash = L'\0';
        }
        memset(&fake, 0, sizeof(fake));
        fake.kind = DEV_CACHE_PIP;
        fake.status = DEV_CACHE_FOUND;
        fake.path = profile;
        fake.home = parent;
        fake.measured = 1;
        CHECK(dev_cache_check(&guard, &fake) == CLEANUP_TRASH_DENY_CRITICAL);
        free(parent);
        CoTaskMemFree(profile);
    }
    cleanup_guard_free(&guard);
    dev_cache_list_free(&l);
}

/* 환경 변수·설정 파일로 바꾼 위치, 캐시 구조 확인, 링크·파일·잘못된 값. */
static void test_overrides(void)
{
    DevCacheList l;
    DevCache *c;

    /* npm: .npmrc 의 cache=${LOCALAPPDATA}\npmc (주석·공백 허용) */
    CHECK(make(L"local\\npmc\\_cacache\\x", 10));
    CHECK(write_text(L"home\\.npmrc", "; comment\r\nregistry=https://example.invalid/\r\n"
                                      "cache = ${LOCALAPPDATA}\\npmc\r\n"));
    /* pip: pip.ini 의 [global] cache-dir(설정 파일 폴더 기준 상대 경로). 다른 구역은 무시 */
    CHECK(make(L"pipc\\http\\y", 20));
    CHECK(write_text(L"roaming\\pip\\pip.ini",
                     "[install]\ncache-dir = C:\\wrong\n[global]\ncache-dir = ..\\..\\pipc\n"));
    /* NuGet: NuGet.Config 의 globalPackagesFolder(상대 경로) */
    CHECK(make(L"nupkgs\\serilog\\3.0.0\\serilog.3.0.0.nupkg", 30));
    CHECK(write_text(L"roaming\\NuGet\\NuGet.Config",
                     "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<configuration>\n  <config>\n"
                     "    <add key=\"globalPackagesFolder\" value=\"..\\..\\nupkgs\" />\n"
                     "  </config>\n</configuration>\n"));
    /* Gradle: GRADLE_USER_HOME */
    CHECK(make(L"gh\\caches\\build-cache-1\\z", 40));
    set_env(L"GRADLE_USER_HOME", L"gh");
    /* Cargo: CARGO_HOME 이 파일이면 그 아래는 없다 */
    CHECK(make(L"cargo-file", 1));
    set_env(L"CARGO_HOME", L"cargo-file");
    /* NuGet HTTP: 캐시 구조가 아닌 폴더(사용자 문서 등을 잘못 가리킴) */
    CHECK(make(L"mydocs\\report.docx", 50));
    set_env(L"NUGET_HTTP_CACHE_PATH", L"mydocs");

    dev_cache_list_init(&l);
    CHECK(dev_cache_detect(&l) == 0);
    c = find(&l, DEV_CACHE_NPM, 0);
    CHECK(c->status == DEV_CACHE_FOUND && c->source == DEV_CACHE_SRC_CONFIG &&
          is_at(c->path, L"local\\npmc") && is_at(c->origin, L"home\\.npmrc") &&
          is_at(c->home, L"local"));
    c = find(&l, DEV_CACHE_PIP, 0);
    CHECK(c->status == DEV_CACHE_FOUND && c->source == DEV_CACHE_SRC_CONFIG &&
          is_at(c->path, L"pipc") && is_at(c->origin, L"roaming\\pip\\pip.ini"));
    c = find(&l, DEV_CACHE_NUGET_PACKAGES, 0);
    CHECK(c->status == DEV_CACHE_FOUND && c->source == DEV_CACHE_SRC_CONFIG &&
          is_at(c->path, L"nupkgs"));
    c = find(&l, DEV_CACHE_GRADLE, 0);
    CHECK(c->status == DEV_CACHE_FOUND && c->source == DEV_CACHE_SRC_ENV &&
          wcscmp(c->origin, L"GRADLE_USER_HOME") == 0 && is_at(c->path, L"gh\\caches") &&
          is_at(c->home, L"gh"));
    CHECK(find(&l, DEV_CACHE_GRADLE_WRAPPER, 0)->status == DEV_CACHE_NOT_FOUND);
    CHECK(find(&l, DEV_CACHE_CARGO_REGISTRY, 0)->status == DEV_CACHE_NOT_FOUND &&
          find(&l, DEV_CACHE_CARGO_REGISTRY, 0)->source == DEV_CACHE_SRC_ENV);
    c = find(&l, DEV_CACHE_NUGET_HTTP, 0);
    CHECK(c->status == DEV_CACHE_UNRECOGNIZED && count_kind(&l, DEV_CACHE_NUGET_HTTP) == 1);
    CHECK(dev_cache_check(NULL, c) == CLEANUP_TRASH_CHANGED);
    dev_cache_measure(c, NULL, NULL);
    CHECK(!c->measured); /* FOUND 가 아니면 세지 않는다 */
    dev_cache_list_free(&l);

    /* 환경 변수가 설정 파일보다 먼저. 파일이면 NOT_DIR, 정션이면 LINK */
    set_env(L"npm_config_cache", L"cargo-file");
    set_env(L"PIP_CACHE_DIR", L"pip-link");
    CHECK(make_junction(L"pip-link", L"pipc"));
    /* 바꾸지 못한 변수가 든 값은 쓰지 않고 다음 위치(기본)로 */
    SetEnvironmentVariableW(L"NUGET_PACKAGES", L"%BYTESIEVE_NO_SUCH_VAR%\\pkgs");
    CHECK(dev_cache_detect(&l) == 0);
    c = find(&l, DEV_CACHE_NPM, 0);
    CHECK(c->status == DEV_CACHE_NOT_DIR && c->source == DEV_CACHE_SRC_ENV &&
          wcscmp(c->origin, L"npm_config_cache") == 0);
    CHECK(dev_cache_check(NULL, c) == CLEANUP_TRASH_MISSING);
    c = find(&l, DEV_CACHE_PIP, 0);
    CHECK(c->status == DEV_CACHE_LINK && dev_cache_check(NULL, c) == CLEANUP_TRASH_CHANGED);
    c = find(&l, DEV_CACHE_NUGET_PACKAGES, 0);
    CHECK(c->source == DEV_CACHE_SRC_CONFIG && is_at(c->path, L"nupkgs"));
    dev_cache_list_free(&l);
    clear_tool_env();
}

/* 정리 연동: 항목마다 다른 탐지 경로로 판정한다(파일 시스템은 바꾸지 않는 경우만). */
static void test_trash_each(void)
{
    CleanupItem a, b;
    const CleanupItem *items[2];
    const wchar_t *roots[2];
    wchar_t *pa = fixture(L"local\\npmc"), *pb = fixture(L"gone\\cache");
    wchar_t *ra = fixture(L"home"), *rb = fixture(L"gone");
    CleanupTrashJob *job;
    CleanupTrashOutcome *out = NULL;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.path = pa; /* 탐지 경로(home) 밖 → 거부, 그대로 남음 */
    a.flags = b.flags = CLEANUP_ITEM_DIR;
    b.path = pb; /* 탐지 경로(gone) 안이지만 없음 */
    items[0] = &a;
    items[1] = &b;
    roots[0] = ra;
    roots[1] = rb;
    job = cleanup_trash_job_start_each(roots, items, 2, 10, NULL, NULL);
    CHECK(job != NULL);
    CHECK(cleanup_trash_job_finish(job, &out) == 2 && out != NULL);
    if (out != NULL) {
        CHECK(out[0].status == CLEANUP_TRASH_DENY_OUTSIDE);
        CHECK(out[1].status == CLEANUP_TRASH_MISSING);
    }
    CHECK(GetFileAttributesW(pa) != INVALID_FILE_ATTRIBUTES);
    cleanup_trash_outcomes_free(out, 2);
    free(pa);
    free(pb);
    free(ra);
    free(rb);
}

static void test_job(void)
{
    DevCacheList l;
    DevCacheJob *job;
    DevCacheJobSnapshot snap;
    size_t i, found = 0;

    dev_cache_list_init(&l);
    job = dev_cache_job_start(NULL, NULL);
    CHECK(job != NULL);
    CHECK(dev_cache_job_finish(job, &l) == 0);
    for (i = 0; i < l.len; i++) {
        if (l.items[i].status == DEV_CACHE_FOUND) {
            found++;
            CHECK(l.items[i].measured && !l.items[i].incomplete);
        }
    }
    CHECK(found == DEV_CACHE_KIND_COUNT + 1);

    /* 시작하자마자 취소하면 FOUND 항목은 다 세지 못한 것으로 남는다 */
    job = dev_cache_job_start(NULL, NULL);
    CHECK(job != NULL);
    dev_cache_job_cancel(job);
    dev_cache_job_progress(job, &snap);
    CHECK(snap.cancel_requested);
    CHECK(dev_cache_job_finish(job, &l) == 1);
    for (i = 0; i < l.len; i++) {
        if (l.items[i].status == DEV_CACHE_FOUND) {
            CHECK(l.items[i].incomplete);
        }
    }
    dev_cache_list_free(&l);
}

static void test_names(void)
{
    int i;

    for (i = 0; i < DEV_CACHE_STATUS_COUNT; i++) {
        CHECK(dev_cache_status_text((DevCacheStatus)i)[0] != L'\0');
        CHECK(wcscmp(dev_cache_status_name((DevCacheStatus)i), L"unknown") != 0);
    }
    for (i = 0; i < DEV_CACHE_KIND_COUNT; i++) {
        const DevCacheKindInfo *k = dev_cache_kind_info((DevCacheKind)i);
        CHECK(k != NULL && k->id[0] && k->tool[0] && k->label[0] && k->note[0]);
    }
    CHECK(dev_cache_kind_info(DEV_CACHE_KIND_COUNT) == NULL);
    CHECK(wcscmp(dev_cache_status_name(DEV_CACHE_UNRECOGNIZED), L"unrecognized") == 0);
}

int main(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 1;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_devcache_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    if (!CreateDirectoryW(g_root, NULL) || !make(L"home\\", 0) || !make(L"local\\", 0) ||
        !make(L"roaming\\", 0)) {
        fprintf(stderr, "fixture setup failed\n");
        return 1;
    }
    /* 사용자 폴더를 픽스처로 돌리고 도구 환경 변수를 지워 실제 캐시를 보지 않게 한다. */
    set_env(L"USERPROFILE", L"home");
    set_env(L"LOCALAPPDATA", L"local");
    set_env(L"APPDATA", L"roaming");
    clear_tool_env();

    test_names();
    test_nothing();
    test_defaults();
    test_cancel_and_guard();
    test_job();
    test_overrides();
    test_trash_each();
    remove_tree(g_root);

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all dev_cache tests passed\n");
    return 0;
}
