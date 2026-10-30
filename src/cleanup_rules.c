#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

static int in_list(const wchar_t *name, const wchar_t *const *list, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (name_eq(name, list[i])) {
            return 1;
        }
    }
    return 0;
}

/* name 이 suffix 로 끝나는지(대소문자 무시). */
static int has_suffix(const wchar_t *name, const wchar_t *suffix)
{
    size_t len = wcslen(name), slen = wcslen(suffix);

    return len >= slen && name_eq_n(name + len - slen, slen, suffix, slen);
}

CleanupCategory cleanup_classify_dir(const wchar_t *name)
{
    static const wchar_t *const build[] = {L"dist", L"build", L"out"};
    static const wchar_t *const cache[] = {L".cache", L".next", L".vite"};
    static const wchar_t *const temp[] = {L".tmp", L".temp"};

    if (name == NULL) {
        return CLEANUP_NONE;
    }
    if (name_eq(name, L"node_modules")) {
        return CLEANUP_NODE_MODULES;
    }
    if (in_list(name, build, sizeof(build) / sizeof(build[0]))) {
        return CLEANUP_BUILD;
    }
    if (in_list(name, cache, sizeof(cache) / sizeof(cache[0]))) {
        return CLEANUP_CACHE;
    }
    if (in_list(name, temp, sizeof(temp) / sizeof(temp[0]))) {
        return CLEANUP_TEMP;
    }
    return CLEANUP_NONE;
}

CleanupCategory cleanup_classify_file(const wchar_t *name)
{
    if (name == NULL) {
        return CLEANUP_NONE;
    }
    if (has_suffix(name, L".tmp")) {
        return CLEANUP_TEMP;
    }
    if (has_suffix(name, L".log")) {
        return CLEANUP_LOG;
    }
    return CLEANUP_NONE;
}

int cleanup_needs_marker(CleanupCategory category)
{
    return category == CLEANUP_NODE_MODULES || category == CLEANUP_BUILD;
}

int cleanup_is_project_marker(const wchar_t *name)
{
    static const wchar_t *const markers[] = {
        L"package.json", L"CMakeLists.txt", L"Makefile",       L"Cargo.toml",
        L"pom.xml",      L"build.gradle",   L"build.gradle.kts", L"pyproject.toml",
        L"go.mod",
    };

    if (name == NULL) {
        return 0;
    }
    return in_list(name, markers, sizeof(markers) / sizeof(markers[0])) ||
           has_suffix(name, L".csproj") || has_suffix(name, L".sln");
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

int cleanup_protect_add(CleanupProtect *p, const wchar_t *path)
{
    const wchar_t *s;
    size_t len, i;
    wchar_t *copy;

    if (p == NULL || !local_span(path, &s, &len) || len == 0) {
        return -1;
    }
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

size_t cleanup_protect_add_system(CleanupProtect *p)
{
    static const wchar_t *const vars[] = {L"ProgramFiles", L"ProgramFiles(x86)",
                                          L"ProgramW6432", L"ProgramData"};
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
    return p->len - before;
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
