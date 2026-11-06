#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <stdlib.h>
#include <string.h>

#include "cleanup_rules.h"
#include "path.h"

/* 길이를 지정한 대소문자 무시 비교(Windows 파일 이름 규칙). */
static int name_eq_n(const wchar_t *a, size_t la, const wchar_t *b, size_t lb)
{
    if (la != lb) {
        return 0;
    }
    if (la == 0) {
        return 1;
    }
    return CompareStringOrdinal(a, (int)la, b, (int)lb, TRUE) == CSTR_EQUAL;
}

static int name_eq(const wchar_t *a, const wchar_t *b)
{
    return name_eq_n(a, wcslen(a), b, wcslen(b));
}

/* name 이 suffix 로 끝나는지(대소문자 무시). */
static int has_suffix(const wchar_t *name, const wchar_t *suffix)
{
    size_t len = wcslen(name), slen = wcslen(suffix);

    return len >= slen && name_eq_n(name + len - slen, slen, suffix, slen);
}

/* name 이 prefix 로 시작하고 더 긴지(대소문자 무시). */
static int has_prefix(const wchar_t *name, const wchar_t *prefix)
{
    size_t len = wcslen(name), plen = wcslen(prefix);

    return len > plen && name_eq_n(name, plen, prefix, plen);
}

#define ECO(e) CLEANUP_ECO_##e
#define BIT(e) CLEANUP_ECO_BIT(CLEANUP_ECO_##e)

static const CleanupEco prefer_dist[] = {ECO(NODE), ECO(PYTHON), ECO(GO), ECO(CPP), ECO(GENERIC)};
static const CleanupEco prefer_build[] = {ECO(JAVA), ECO(CPP), ECO(NODE), ECO(PYTHON),
                                          ECO(GENERIC)};
static const CleanupEco prefer_out[] = {ECO(CPP), ECO(JAVA), ECO(NODE), ECO(GO), ECO(GENERIC)};
static const CleanupEco prefer_cache[] = {ECO(NODE), ECO(PYTHON), ECO(GENERIC)};
static const CleanupEco prefer_target[] = {ECO(RUST), ECO(JAVA), ECO(GENERIC)};

static const struct {
    const wchar_t *name;
    CleanupDirRule rule;
} g_dir_rules[] = {
    {L"node_modules", {CLEANUP_NODE_MODULES, ECO(NODE), 0, NULL}},
    {L"dist", {CLEANUP_BUILD, ECO(GENERIC), 0, prefer_dist}},
    {L"build", {CLEANUP_BUILD, ECO(GENERIC), 0, prefer_build}},
    {L"out", {CLEANUP_BUILD, ECO(GENERIC), 0, prefer_out}},
    {L"target", {CLEANUP_BUILD, ECO(GENERIC), BIT(RUST) | BIT(JAVA), prefer_target}},
    {L"bin", {CLEANUP_BUILD, ECO(DOTNET), BIT(DOTNET), NULL}},
    {L"obj", {CLEANUP_BUILD, ECO(DOTNET), BIT(DOTNET), NULL}},
    {L".cache", {CLEANUP_CACHE, ECO(GENERIC), 0, prefer_cache}},
    {L".next", {CLEANUP_CACHE, ECO(NODE), 0, NULL}},
    {L".nuxt", {CLEANUP_CACHE, ECO(NODE), 0, NULL}},
    {L".vite", {CLEANUP_CACHE, ECO(NODE), 0, NULL}},
    {L".parcel-cache", {CLEANUP_CACHE, ECO(NODE), 0, NULL}},
    {L".turbo", {CLEANUP_CACHE, ECO(NODE), 0, NULL}},
    {L".svelte-kit", {CLEANUP_CACHE, ECO(NODE), 0, NULL}},
    {L".angular", {CLEANUP_CACHE, ECO(NODE), 0, NULL}},
    {L"__pycache__", {CLEANUP_CACHE, ECO(PYTHON), 0, NULL}},
    {L".pytest_cache", {CLEANUP_CACHE, ECO(PYTHON), 0, NULL}},
    {L".mypy_cache", {CLEANUP_CACHE, ECO(PYTHON), 0, NULL}},
    {L".ruff_cache", {CLEANUP_CACHE, ECO(PYTHON), 0, NULL}},
    {L".tox", {CLEANUP_CACHE, ECO(PYTHON), 0, NULL}},
    {L".nox", {CLEANUP_CACHE, ECO(PYTHON), 0, NULL}},
    {L".hypothesis", {CLEANUP_CACHE, ECO(PYTHON), 0, NULL}},
    {L".gradle", {CLEANUP_CACHE, ECO(JAVA), BIT(JAVA), NULL}},
    {L".vs", {CLEANUP_CACHE, ECO(DOTNET), BIT(DOTNET), NULL}},
    {L".tmp", {CLEANUP_TEMP, ECO(GENERIC), 0, NULL}},
    {L".temp", {CLEANUP_TEMP, ECO(GENERIC), 0, NULL}},
};

/* CLion 이 프로필마다 만드는 빌드 폴더(cmake-build-debug 등). */
static const CleanupDirRule g_cmake_build_rule = {CLEANUP_BUILD, ECO(CPP), 0, NULL};

int cleanup_dir_rule(const wchar_t *name, CleanupDirRule *rule)
{
    size_t i;

    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof(g_dir_rules) / sizeof(g_dir_rules[0]); i++) {
        if (name_eq(name, g_dir_rules[i].name)) {
            if (rule != NULL) {
                *rule = g_dir_rules[i].rule;
            }
            return 1;
        }
    }
    if (has_prefix(name, L"cmake-build-")) {
        if (rule != NULL) {
            *rule = g_cmake_build_rule;
        }
        return 1;
    }
    return 0;
}

CleanupCategory cleanup_classify_dir(const wchar_t *name)
{
    CleanupDirRule rule;

    return cleanup_dir_rule(name, &rule) ? rule.category : CLEANUP_NONE;
}

/* 이름이 숫자 min_digits 자 이상과 suffix 로만 이뤄졌는지("000003.log"). */
static int is_numbered(const wchar_t *name, const wchar_t *suffix, size_t min_digits)
{
    size_t n = wcslen(name), s = wcslen(suffix), i;

    if (n < s + min_digits || !has_suffix(name, suffix)) {
        return 0;
    }
    for (i = 0; i < n - s; i++) {
        if (name[i] < L'0' || name[i] > L'9') {
            return 0;
        }
    }
    return 1;
}

CleanupCategory cleanup_classify_file(const wchar_t *name)
{
    if (name == NULL) {
        return CLEANUP_NONE;
    }
    if (has_suffix(name, L".tmp")) {
        return CLEANUP_TEMP;
    }
    /* LevelDB 쓰기 로그("%06llu.log")는 데이터이므로 로그가 아니다. */
    if (has_suffix(name, L".log") && !is_numbered(name, L".log", 6)) {
        return CLEANUP_LOG;
    }
    return CLEANUP_NONE;
}

int cleanup_is_leveldb_marker(const wchar_t *name)
{
    return name != NULL &&
           (name_eq(name, L"CURRENT") || name_eq(name, L"LOCK") || has_prefix(name, L"MANIFEST-"));
}

int cleanup_needs_marker(CleanupCategory category)
{
    return category == CLEANUP_NODE_MODULES || category == CLEANUP_BUILD;
}

unsigned cleanup_marker_ecos(const wchar_t *name)
{
    static const struct {
        const wchar_t *name;
        unsigned ecos;
    } markers[] = {
        {L"package.json", BIT(NODE)},
        {L"pyproject.toml", BIT(PYTHON)},
        {L"setup.py", BIT(PYTHON)},
        {L"setup.cfg", BIT(PYTHON)},
        {L"requirements.txt", BIT(PYTHON)},
        {L"Pipfile", BIT(PYTHON)},
        {L"tox.ini", BIT(PYTHON)},
        {L"Cargo.toml", BIT(RUST)},
        {L"pom.xml", BIT(JAVA)},
        {L"build.gradle", BIT(JAVA)},
        {L"build.gradle.kts", BIT(JAVA)},
        {L"settings.gradle", BIT(JAVA)},
        {L"settings.gradle.kts", BIT(JAVA)},
        {L"CMakeLists.txt", BIT(CPP)},
        {L"Makefile", BIT(CPP)},
        {L"go.mod", BIT(GO)},
    };
    static const wchar_t *const dotnet_suffixes[] = {L".csproj", L".fsproj", L".vbproj", L".sln"};
    size_t i;

    if (name == NULL) {
        return 0;
    }
    for (i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        if (name_eq(name, markers[i].name)) {
            return markers[i].ecos;
        }
    }
    for (i = 0; i < sizeof(dotnet_suffixes) / sizeof(dotnet_suffixes[0]); i++) {
        /* 확장자만 있는 이름(".sln")은 표시 파일로 보지 않는다. */
        if (wcslen(name) > wcslen(dotnet_suffixes[i]) && has_suffix(name, dotnet_suffixes[i])) {
            return BIT(DOTNET);
        }
    }
    return 0;
}

int cleanup_is_project_marker(const wchar_t *name)
{
    return cleanup_marker_ecos(name) != 0;
}

CleanupEco cleanup_resolve_eco(const CleanupDirRule *rule, unsigned markers)
{
    const CleanupEco *p;

    if (rule == NULL) {
        return CLEANUP_ECO_GENERIC;
    }
    if (rule->eco != CLEANUP_ECO_GENERIC || rule->prefer == NULL) {
        return rule->eco;
    }
    for (p = rule->prefer; *p != CLEANUP_ECO_GENERIC; p++) {
        if (markers & CLEANUP_ECO_BIT(*p)) {
            return *p;
        }
    }
    return CLEANUP_ECO_GENERIC;
}

#undef ECO
#undef BIT

const wchar_t *cleanup_eco_name(CleanupEco eco)
{
    switch (eco) {
    case CLEANUP_ECO_NODE:
        return L"node";
    case CLEANUP_ECO_PYTHON:
        return L"python";
    case CLEANUP_ECO_RUST:
        return L"rust";
    case CLEANUP_ECO_JAVA:
        return L"java";
    case CLEANUP_ECO_DOTNET:
        return L"dotnet";
    case CLEANUP_ECO_CPP:
        return L"cpp";
    case CLEANUP_ECO_GO:
        return L"go";
    default:
        return L"generic";
    }
}

const wchar_t *cleanup_category_name(CleanupCategory category)
{
    switch (category) {
    case CLEANUP_NODE_MODULES:
        return L"node_modules";
    case CLEANUP_BUILD:
        return L"build";
    case CLEANUP_CACHE:
        return L"cache";
    case CLEANUP_TEMP:
        return L"temp";
    case CLEANUP_LOG:
        return L"log";
    default:
        return L"none";
    }
}

/* ---- 보호 경로 ---- */

/* 볼륨 루트 바로 아래에만 있는 시스템 폴더. */
static int is_root_system_name(const wchar_t *name, size_t len)
{
    static const wchar_t *const names[] = {L"$Recycle.Bin", L"System Volume Information",
                                           L"Recovery", L"Config.Msi"};
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (name_eq_n(name, len, names[i], wcslen(names[i]))) {
            return 1;
        }
    }
    return 0;
}

/*
 * 비교용으로 확장 접두사를 건너뛰고 끝 구분자를 뺀 범위를 구한다("\\?\C:\" -> "C:").
 * "\\?\UNC\..." 는 "\\..." 와 맞추기 어렵고 보호 경로는 로컬뿐이라 0 을 반환한다.
 */
static int local_span(const wchar_t *path, const wchar_t **start, size_t *len)
{
    const wchar_t *s = path;
    size_t n;

    if (path == NULL) {
        return 0;
    }
    if (path_is_extended(s)) {
        s += 4;
        if (_wcsnicmp(s, L"UNC\\", 4) == 0) {
            return 0;
        }
    }
    n = wcslen(s);
    while (n > 0 && path_is_separator(s[n - 1])) {
        n--;
    }
    *start = s;
    *len = n;
    return 1;
}

static int is_volume_root(const wchar_t *s, size_t len)
{
    return len == 2 && s[1] == L':';
}

void cleanup_protect_init(CleanupProtect *p)
{
    memset(p, 0, sizeof(*p));
}

void cleanup_protect_free(CleanupProtect *p)
{
    size_t i;

    if (p == NULL) {
        return;
    }
    for (i = 0; i < p->len; i++) {
        free(p->paths[i]);
    }
    free(p->paths);
    cleanup_protect_init(p);
}

/* s[0..len) 을 목록에 넣는다(대소문자 무시로 같은 경로가 있으면 넣지 않고 성공). */
static int protect_add_span(CleanupProtect *p, const wchar_t *s, size_t len)
{
    size_t i;
    wchar_t *copy;

    for (i = 0; i < p->len; i++) {
        if (name_eq_n(p->paths[i], wcslen(p->paths[i]), s, len)) {
            return 0;
        }
    }
    if (p->len == p->cap) {
        size_t cap = p->cap ? p->cap * 2 : 8;
        wchar_t **grown = realloc(p->paths, cap * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        p->paths = grown;
        p->cap = cap;
    }
    copy = malloc((len + 1) * sizeof(*copy));
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, s, len * sizeof(*copy));
    copy[len] = L'\0';
    p->paths[p->len++] = copy;
    return 0;
}

int cleanup_protect_add(CleanupProtect *p, const wchar_t *path)
{
    const wchar_t *s;
    size_t len;

    if (p == NULL || !local_span(path, &s, &len) || len == 0) {
        return -1;
    }
    return protect_add_span(p, s, len);
}

static int is_trim_char(wchar_t c)
{
    return c == L' ' || c == L'\t' || c == L'"';
}

int cleanup_protect_add_install(CleanupProtect *p, const wchar_t *path)
{
    const wchar_t *s, *e;
    wchar_t *raw, *full;
    size_t len;
    int rc = -1;

    if (p == NULL || path == NULL) {
        return -1;
    }
    for (s = path; is_trim_char(*s); s++) {
    }
    for (e = s + wcslen(s); e > s && is_trim_char(e[-1]); e--) {
    }
    len = (size_t)(e - s);
    /* "C:x"·상대·UNC·확장 경로는 받지 않는다(path_full 이 현재 폴더 기준으로 바꾸지 않도록). */
    if (len < 4 || !((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')) ||
        s[1] != L':' || !path_is_separator(s[2])) {
        return -1;
    }
    raw = malloc((len + 1) * sizeof(*raw));
    if (raw == NULL) {
        return -1;
    }
    memcpy(raw, s, len * sizeof(*raw));
    raw[len] = L'\0';
    full = path_full(raw);
    free(raw);
    /* 정규화한 뒤에도 볼륨 루트("C:\..", "C:\.")가 아니어야 한다. */
    if (full != NULL && local_span(full, &s, &len) && len > 3) {
        rc = protect_add_span(p, s, len);
    }
    free(full);
    return rc;
}

int cleanup_is_dev_tool_product(const wchar_t *display_name)
{
    static const wchar_t *const prefixes[] = {L"Microsoft Visual Studio", L"Visual Studio "};
    size_t len, i;

    if (display_name == NULL) {
        return 0;
    }
    len = wcslen(display_name);
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t plen = wcslen(prefixes[i]);
        if (len >= plen && name_eq_n(display_name, plen, prefixes[i], plen)) {
            return 1;
        }
    }
    return 0;
}

/* 환경 변수 var 의 값 아래 tail 경로를 설치 경로로 추가한다. 변수가 없으면 건너뛴다. */
static void add_env_install(CleanupProtect *p, const wchar_t *var, const wchar_t *tail)
{
    wchar_t buf[MAX_PATH];
    wchar_t *joined;
    DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);

    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    joined = path_join(buf, tail);
    if (joined != NULL) {
        cleanup_protect_add_install(p, joined);
    }
    free(joined);
}

/*
 * 프로그램 제거 정보(root\...\Uninstall, view 는 KEY_WOW64_*)에서 개발 도구의 InstallLocation 을
 * 추가한다. 읽지 못한 키·값은 건너뛴다.
 */
static void add_uninstall_dev_tools(CleanupProtect *p, HKEY root, REGSAM view)
{
    HKEY key;
    DWORD i;

    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                      KEY_READ | view, &key) != ERROR_SUCCESS) {
        return;
    }
    for (i = 0;; i++) {
        wchar_t sub[256], name[512], location[MAX_PATH];
        DWORD sublen = sizeof(sub) / sizeof(sub[0]), size;
        HKEY app;
        LSTATUS st = RegEnumKeyExW(key, i, sub, &sublen, NULL, NULL, NULL, NULL);

        if (st == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (st != ERROR_SUCCESS ||
            RegOpenKeyExW(key, sub, 0, KEY_QUERY_VALUE | view, &app) != ERROR_SUCCESS) {
            continue;
        }
        /* REG_EXPAND_SZ 는 RegGetValueW 가 환경 변수를 바꿔 준다. */
        size = sizeof(name);
        if (RegGetValueW(app, NULL, L"DisplayName", RRF_RT_REG_SZ, NULL, name, &size) ==
                ERROR_SUCCESS &&
            cleanup_is_dev_tool_product(name)) {
            size = sizeof(location);
            if (RegGetValueW(app, NULL, L"InstallLocation", RRF_RT_REG_SZ, NULL, location,
                             &size) == ERROR_SUCCESS) {
                cleanup_protect_add_install(p, location);
            }
        }
        RegCloseKey(app);
    }
    RegCloseKey(key);
}

/*
 * PEP 514 로 등록된 Python 설치 경로를 추가한다: root\base\<회사>\<태그>\InstallPath 의 기본 값
 * (view 는 KEY_WOW64_*). 읽지 못한 키·값은 건너뛴다.
 */
static void add_registered_pythons(CleanupProtect *p, HKEY root, REGSAM view, const wchar_t *base)
{
    HKEY key;
    DWORD i, j;

    if (RegOpenKeyExW(root, base, 0, KEY_READ | view, &key) != ERROR_SUCCESS) {
        return;
    }
    for (i = 0;; i++) {
        wchar_t company[256];
        DWORD clen = sizeof(company) / sizeof(company[0]);
        HKEY ck;
        LSTATUS st = RegEnumKeyExW(key, i, company, &clen, NULL, NULL, NULL, NULL);

        if (st == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (st != ERROR_SUCCESS ||
            RegOpenKeyExW(key, company, 0, KEY_READ | view, &ck) != ERROR_SUCCESS) {
            continue;
        }
        for (j = 0;; j++) {
            wchar_t tag[256], location[MAX_PATH];
            DWORD tlen = sizeof(tag) / sizeof(tag[0]), size = sizeof(location);
            HKEY tk;
            st = RegEnumKeyExW(ck, j, tag, &tlen, NULL, NULL, NULL, NULL);
            if (st == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (st != ERROR_SUCCESS ||
                RegOpenKeyExW(ck, tag, 0, KEY_READ | view, &tk) != ERROR_SUCCESS) {
                continue;
            }
            /* InstallPath 키의 기본 값이 설치 폴더다. */
            if (RegGetValueW(tk, L"InstallPath", NULL, RRF_RT_REG_SZ, NULL, location, &size) ==
                ERROR_SUCCESS) {
                cleanup_protect_add_install(p, location);
            }
            RegCloseKey(tk);
        }
        RegCloseKey(ck);
    }
    RegCloseKey(key);
}

size_t cleanup_protect_add_system(CleanupProtect *p)
{
    static const wchar_t *const vars[] = {L"ProgramFiles", L"ProgramFiles(x86)",
                                          L"ProgramW6432", L"ProgramData"};
    static const struct {
        const wchar_t *var;
        const wchar_t *tail;
    } installs[] = {
        {L"LOCALAPPDATA", L"Programs"},
        {L"USERPROFILE", L".vscode\\extensions"},
        {L"USERPROFILE", L".vscode-insiders\\extensions"},
    };
    wchar_t buf[MAX_PATH];
    size_t before, i;
    DWORD n;

    if (p == NULL) {
        return 0;
    }
    before = p->len;
    n = GetWindowsDirectoryW(buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        cleanup_protect_add(p, buf);
    }
    for (i = 0; i < sizeof(vars) / sizeof(vars[0]); i++) {
        n = GetEnvironmentVariableW(vars[i], buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            cleanup_protect_add(p, buf);
        }
    }
    for (i = 0; i < sizeof(installs) / sizeof(installs[0]); i++) {
        add_env_install(p, installs[i].var, installs[i].tail);
    }
    add_uninstall_dev_tools(p, HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);
    add_uninstall_dev_tools(p, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY);
    add_uninstall_dev_tools(p, HKEY_CURRENT_USER, 0);
    add_registered_pythons(p, HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"SOFTWARE\\Python");
    add_registered_pythons(p, HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"SOFTWARE\\Python");
    add_registered_pythons(p, HKEY_CURRENT_USER, 0, L"SOFTWARE\\Python");
    n = GetEnvironmentVariableW(L"BYTESIEVE_TEST_PYTHON_KEY", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        add_registered_pythons(p, HKEY_CURRENT_USER, 0, buf);
    }
    return p->len - before;
}

/* dir 바로 아래의 폴더(이름이 except 인 것 제외)를 목록에 넣는다. 읽지 못하면 건너뛴다. */
static void add_child_folders(CleanupProtect *p, const wchar_t *dir, const wchar_t *except)
{
    wchar_t *pattern = path_join(dir, L"*");
    wchar_t *ext = pattern != NULL ? path_to_extended(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE find = INVALID_HANDLE_VALUE;

    memset(&fd, 0, sizeof(fd));
    if (ext != NULL) {
        find = FindFirstFileExW(ext, FindExInfoBasic, &fd, FindExSearchLimitToDirectories, NULL,
                                0);
    }
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0 ||
                name_eq(fd.cFileName, except)) {
                continue;
            }
            child = path_join(dir, fd.cFileName);
            if (child != NULL) {
                cleanup_protect_add(p, child);
            }
            free(child);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    free(ext);
    free(pattern);
}

size_t cleanup_protect_add_app_data(CleanupProtect *p)
{
    static const struct {
        const wchar_t *var;
        const wchar_t *tail; /* NULL 이면 변수 값 그대로 */
    } roots[] = {
        {L"LOCALAPPDATA", NULL},
        {L"APPDATA", NULL},
        {L"USERPROFILE", L"AppData\\LocalLow"},
    };
    wchar_t buf[MAX_PATH];
    size_t before, i;

    if (p == NULL) {
        return 0;
    }
    before = p->len;
    for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        DWORD n = GetEnvironmentVariableW(roots[i].var, buf, MAX_PATH);
        wchar_t *dir;
        if (n == 0 || n >= MAX_PATH) {
            continue;
        }
        dir = roots[i].tail != NULL ? path_join(buf, roots[i].tail) : path_dup(buf);
        if (dir != NULL) {
            /* 임시 폴더는 원래 정리할 곳이라 계속 탐지한다. */
            add_child_folders(p, dir, L"Temp");
        }
        free(dir);
    }
    return p->len - before;
}

/* a 와 b 가 같은 로컬 경로인지(확장 접두사·끝 구분자·대소문자 무시). */
static int same_path(const wchar_t *a, const wchar_t *b)
{
    const wchar_t *sa, *sb;
    size_t la, lb;

    return a != NULL && b != NULL && local_span(a, &sa, &la) && local_span(b, &sb, &lb) &&
           name_eq_n(sa, la, sb, lb);
}

size_t cleanup_protect_add_other_profiles(CleanupProtect *p)
{
    wchar_t dir[MAX_PATH], env_me[MAX_PATH];
    wchar_t *pattern, *ext;
    PWSTR real_me = NULL;
    const wchar_t *d;
    WIN32_FIND_DATAW fd;
    HANDLE find = INVALID_HANDLE_VALUE;
    size_t before, dlen;
    DWORD n, size = sizeof(dir);

    if (p == NULL) {
        return 0;
    }
    /* 프로필 폴더(보통 C:\Users)는 Windows 가 정한 값을 쓴다(현재 프로필의 상위를 짐작하지 않음). */
    n = GetEnvironmentVariableW(L"BYTESIEVE_TEST_PROFILES_DIR", dir, MAX_PATH);
    if ((n == 0 || n >= MAX_PATH) &&
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList",
                     L"ProfilesDirectory", RRF_RT_REG_SZ, NULL, dir, &size) != ERROR_SUCCESS) {
        return 0;
    }
    /* 볼륨 루트면 드라이브의 모든 폴더가 들어가므로 하지 않는다. */
    if (!local_span(dir, &d, &dlen) || is_volume_root(d, dlen)) {
        return 0;
    }
    /* 현재 사용자: 환경 변수와 실제 프로필 위치 둘 다(환경 변수를 바꾼 경우에도 자기 파일은 탐지). */
    n = GetEnvironmentVariableW(L"USERPROFILE", env_me, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        env_me[0] = L'\0';
    }
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_Profile, KF_FLAG_DONT_VERIFY, NULL, &real_me))) {
        real_me = NULL;
    }

    before = p->len;
    pattern = path_join(dir, L"*");
    ext = pattern != NULL ? path_to_extended(pattern) : NULL;
    memset(&fd, 0, sizeof(fd));
    if (ext != NULL) {
        find = FindFirstFileExW(ext, FindExInfoBasic, &fd, FindExSearchLimitToDirectories, NULL,
                                0);
    }
    if (find != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            child = path_join(dir, fd.cFileName);
            if (child != NULL && !same_path(child, env_me[0] != L'\0' ? env_me : NULL) &&
                !same_path(child, real_me)) {
                cleanup_protect_add(p, child);
            }
            free(child);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    free(ext);
    free(pattern);
    CoTaskMemFree(real_me);
    return p->len - before;
}

int cleanup_is_root_system_file(const wchar_t *dir, const wchar_t *name)
{
    static const wchar_t *const names[] = {L"DumpStack.log", L"DumpStack.log.tmp", L"pagefile.sys",
                                           L"hiberfil.sys", L"swapfile.sys"};
    const wchar_t *d;
    size_t dlen, i;

    if (name == NULL || !local_span(dir, &d, &dlen) || !is_volume_root(d, dlen)) {
        return 0;
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (name_eq(name, names[i])) {
            return 1;
        }
    }
    return 0;
}

int cleanup_protect_match(const CleanupProtect *p, const wchar_t *dir, const wchar_t *name)
{
    const wchar_t *d;
    size_t dlen, nlen, i;

    if (name == NULL || !local_span(dir, &d, &dlen)) {
        return 0;
    }
    nlen = wcslen(name);
    if (is_volume_root(d, dlen) && is_root_system_name(name, nlen)) {
        return 1;
    }
    if (p == NULL) {
        return 0;
    }
    /* dir\name 을 만들지 않고 "dir" + 구분자 + "name" 으로 나눠 비교한다. */
    for (i = 0; i < p->len; i++) {
        const wchar_t *q = p->paths[i];
        size_t qlen = wcslen(q);
        if (qlen == dlen + 1 + nlen && name_eq_n(q, dlen, d, dlen) &&
            path_is_separator(q[dlen]) && name_eq_n(q + dlen + 1, nlen, name, nlen)) {
            return 1;
        }
    }
    return 0;
}

int cleanup_protect_contains(const CleanupProtect *p, const wchar_t *path)
{
    const wchar_t *s;
    size_t len, i;

    if (!local_span(path, &s, &len)) {
        return 0;
    }
    /* "X:\<시스템 폴더>" 또는 그 아래 */
    if (len > 3 && s[1] == L':' && path_is_separator(s[2])) {
        size_t end = 3;
        while (end < len && !path_is_separator(s[end])) {
            end++;
        }
        if (is_root_system_name(s + 3, end - 3)) {
            return 1;
        }
    }
    if (p == NULL) {
        return 0;
    }
    for (i = 0; i < p->len; i++) {
        const wchar_t *q = p->paths[i];
        size_t qlen = wcslen(q);
        if (qlen <= len && name_eq_n(q, qlen, s, qlen) &&
            (qlen == len || path_is_separator(s[qlen]))) {
            return 1;
        }
    }
    return 0;
}

int cleanup_is_quarantine_name(const wchar_t *name)
{
    return name != NULL && name_eq(name, CLEANUP_QUARANTINE_DIR);
}

int cleanup_in_quarantine(const wchar_t *path)
{
    size_t qlen = wcslen(CLEANUP_QUARANTINE_DIR);
    const wchar_t *s;

    if (path == NULL) {
        return 0;
    }
    for (s = path; *s != L'\0';) {
        size_t len = 0;
        while (s[len] != L'\0' && !path_is_separator(s[len])) {
            len++;
        }
        if (name_eq_n(s, len, CLEANUP_QUARANTINE_DIR, qlen)) {
            return 1;
        }
        s += len;
        while (*s != L'\0' && path_is_separator(*s)) {
            s++;
        }
    }
    return 0;
}
