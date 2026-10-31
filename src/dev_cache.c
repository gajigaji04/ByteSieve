#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "dev_cache.h"
#include "path.h"

/* 설정 파일을 읽을 최대 크기(손상된 큰 파일로 메모리를 쓰지 않도록). */
#define CONFIG_MAX_BYTES ((DWORD)1024 * 1024)
/* 캐시 구조를 확인할 때 살펴보는 하위 폴더 수. */
#define SIGNATURE_PROBE 64

/* ---- 종류·상태 ---- */

static const DevCacheKindInfo g_kinds[DEV_CACHE_KIND_COUNT] = {
    {L"npm", L"npm", L"패키지 캐시", CLEANUP_ECO_NODE, 0,
     L"npm이 내려받은 패키지 캐시입니다. 다음 설치 때 다시 내려받습니다('npm cache clean "
     L"--force'와 같은 효과). npm·npx가 실행 중이면 끝난 뒤 정리하세요."},
    {L"pip", L"pip", L"패키지 캐시", CLEANUP_ECO_PYTHON, 0,
     L"pip이 내려받은 패키지·빌드한 휠 캐시입니다. 다음 설치 때 다시 내려받거나 빌드합니다"
     L"('pip cache purge'와 같은 효과). 휠을 직접 빌드하는 패키지는 설치가 느려질 수 있습니다."},
    {L"gradle", L"Gradle", L"의존성·빌드 캐시", CLEANUP_ECO_JAVA, 0,
     L"Gradle이 내려받은 의존성과 빌드 캐시입니다. 다음 빌드 때 다시 내려받아 첫 빌드가 느려집니다. "
     L"Gradle 데몬이 실행 중이면 사용 중 오류가 날 수 있으니 'gradle --stop' 후 정리하세요. "
     L"설정(gradle.properties)은 건드리지 않습니다."},
    {L"gradle_wrapper", L"Gradle", L"래퍼 배포판", CLEANUP_ECO_JAVA, 1,
     L"Gradle 래퍼가 내려받은 Gradle 배포판입니다. 다음 빌드 때 배포판(수백 MB)을 다시 "
     L"내려받으므로 인터넷이 없으면 빌드할 수 없습니다."},
    {L"cargo_registry", L"Cargo", L"레지스트리 캐시", CLEANUP_ECO_RUST, 0,
     L"crates.io 색인·크레이트 압축 파일·풀어 둔 소스입니다. 다음 빌드 때 다시 내려받습니다. "
     L"cargo가 실행 중이면 끝난 뒤 정리하세요. 설치한 도구(.cargo\\bin)와 설정은 건드리지 "
     L"않습니다."},
    {L"cargo_git", L"Cargo", L"git 의존성", CLEANUP_ECO_RUST, 0,
     L"git 저장소에서 받은 의존성입니다. 다음 빌드 때 다시 받습니다. 설치한 도구(.cargo\\bin)와 "
     L"설정은 건드리지 않습니다."},
    {L"nuget_packages", L"NuGet", L"전역 패키지", CLEANUP_ECO_DOTNET, 1,
     L"NuGet 전역 패키지 폴더입니다. 다음 복원(restore) 때 모든 패키지를 다시 내려받으므로 "
     L"인터넷이 없으면 빌드할 수 없습니다. Visual Studio·dotnet이 실행 중이면 사용 중 오류가 날 "
     L"수 있습니다('dotnet nuget locals global-packages --clear'와 같은 효과)."},
    {L"nuget_http", L"NuGet", L"HTTP 캐시", CLEANUP_ECO_DOTNET, 0,
     L"NuGet 피드 응답 캐시입니다. 다음 복원 때 다시 내려받습니다('dotnet nuget locals "
     L"http-cache --clear'와 같은 효과)."},
};

const DevCacheKindInfo *dev_cache_kind_info(DevCacheKind kind)
{
    return (unsigned)kind < DEV_CACHE_KIND_COUNT ? &g_kinds[kind] : NULL;
}

const wchar_t *dev_cache_status_text(DevCacheStatus status)
{
    switch (status) {
    case DEV_CACHE_FOUND:
        return L"있음";
    case DEV_CACHE_NOT_FOUND:
        return L"없음";
    case DEV_CACHE_NOT_DIR:
        return L"폴더가 아님";
    case DEV_CACHE_LINK:
        return L"링크라 정리하지 않음";
    case DEV_CACHE_UNRECOGNIZED:
        return L"캐시 구조가 아니라 정리하지 않음";
    case DEV_CACHE_NO_PATH:
        return L"위치를 정할 수 없음";
    default:
        return L"";
    }
}

const wchar_t *dev_cache_status_name(DevCacheStatus status)
{
    switch (status) {
    case DEV_CACHE_FOUND:
        return L"found";
    case DEV_CACHE_NOT_FOUND:
        return L"not_found";
    case DEV_CACHE_NOT_DIR:
        return L"not_dir";
    case DEV_CACHE_LINK:
        return L"link";
    case DEV_CACHE_UNRECOGNIZED:
        return L"unrecognized";
    case DEV_CACHE_NO_PATH:
        return L"no_path";
    default:
        return L"unknown";
    }
}

/* ---- 목록 ---- */

void dev_cache_list_init(DevCacheList *list)
{
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

void dev_cache_list_free(DevCacheList *list)
{
    size_t i;

    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->len; i++) {
        free(list->items[i].origin);
        free(list->items[i].path);
        free(list->items[i].home);
    }
    free(list->items);
    dev_cache_list_init(list);
}

/* ---- 경로 ---- */

static int is_absolute(const wchar_t *s)
{
    return (iswalpha(s[0]) && s[1] == L':' && path_is_separator(s[2])) ||
           (path_is_separator(s[0]) && path_is_separator(s[1]));
}

/* 끝 구분자를 뗀다("C:\" 는 그대로). */
static void trim_trailing(wchar_t *s)
{
    size_t n = wcslen(s);

    while (n > 3 && path_is_separator(s[n - 1])) {
        s[--n] = L'\0';
    }
}

/*
 * 설정·환경 변수 값을 경로로 만든다(malloc). 앞뒤 공백·따옴표를 떼고 ${VAR}·%VAR% 를 바꾸며,
 * 상대 경로는 base(NULL 이면 받지 않음) 기준으로 본다. 경로가 될 수 없으면 NULL.
 */
static wchar_t *value_to_path(const wchar_t *value, const wchar_t *base)
{
    wchar_t *buf, *expanded, *joined, *full;
    const wchar_t *s = value;
    size_t len, i, o = 0;
    DWORD n;

    while (*s == L' ' || *s == L'\t' || *s == L'"' || *s == L'\'') {
        s++;
    }
    len = wcslen(s);
    while (len > 0 && (s[len - 1] == L' ' || s[len - 1] == L'\t' || s[len - 1] == L'"' ||
                       s[len - 1] == L'\'' || s[len - 1] == L'\r')) {
        len--;
    }
    if (len == 0) {
        return NULL;
    }
    /* ${VAR} → %VAR% (npm 설정 문법) */
    buf = malloc((len + 1) * sizeof(*buf));
    if (buf == NULL) {
        return NULL;
    }
    for (i = 0; i < len; i++) {
        if (s[i] == L'$' && i + 1 < len && s[i + 1] == L'{') {
            const wchar_t *end = wmemchr(s + i + 2, L'}', len - i - 2);
            if (end != NULL) {
                size_t name = (size_t)(end - (s + i + 2));
                buf[o++] = L'%';
                wmemcpy(buf + o, s + i + 2, name);
                o += name;
                buf[o++] = L'%';
                i = (size_t)(end - s);
                continue;
            }
        }
        buf[o++] = s[i];
    }
    buf[o] = L'\0';
    n = ExpandEnvironmentStringsW(buf, NULL, 0);
    expanded = n > 0 ? malloc((size_t)n * sizeof(*expanded)) : NULL;
    if (expanded == NULL || ExpandEnvironmentStringsW(buf, expanded, n) == 0) {
        free(buf);
        free(expanded);
        return NULL;
    }
    free(buf);
    /* 바꾸지 못한 변수가 남았으면 쓰지 않는다. */
    if (wcschr(expanded, L'%') != NULL) {
        free(expanded);
        return NULL;
    }
    if (!is_absolute(expanded)) {
        if (base == NULL || expanded[0] == L'\\' || expanded[0] == L'/') {
            free(expanded);
            return NULL;
        }
        joined = path_join(base, expanded);
        free(expanded);
        expanded = joined;
        if (expanded == NULL) {
            return NULL;
        }
    }
    full = path_full(expanded);
    free(expanded);
    if (full != NULL) {
        trim_trailing(full);
    }
    return full;
}

/* 환경 변수 값을 경로로(없거나 경로가 아니면 NULL). */
static wchar_t *env_path(const wchar_t *name)
{
    wchar_t local[MAX_PATH], *big, *out;
    DWORD n = GetEnvironmentVariableW(name, local, MAX_PATH);

    if (n == 0) {
        return NULL;
    }
    if (n < MAX_PATH) {
        return value_to_path(local, NULL);
    }
    big = malloc((size_t)n * sizeof(*big));
    if (big == NULL || GetEnvironmentVariableW(name, big, n) == 0) {
        free(big);
        return NULL;
    }
    out = value_to_path(big, NULL);
    free(big);
    return out;
}

/* 사용자 폴더: 환경 변수, 없으면 알려진 폴더. */
static wchar_t *user_dir(const wchar_t *env, const KNOWNFOLDERID *id)
{
    wchar_t *p = env_path(env), *k = NULL;

    if (p != NULL) {
        return p;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, NULL, &k))) {
        p = path_dup(k);
        CoTaskMemFree(k);
    }
    return p;
}

/* 부모 폴더(malloc). 볼륨 루트면 그대로 복사한다. */
static wchar_t *parent_of(const wchar_t *path)
{
    size_t n = wcslen(path);
    wchar_t *out;

    while (n > 0 && !path_is_separator(path[n - 1])) {
        n--;
    }
    if (n == 0) {
        return path_dup(path);
    }
    if (n > 1 && !(n == 3 && path[1] == L':')) {
        n--; /* 끝 구분자 제외("C:\" 는 남김) */
    }
    out = malloc((n + 1) * sizeof(*out));
    if (out != NULL) {
        wmemcpy(out, path, n);
        out[n] = L'\0';
    }
    return out;
}

/* ---- 설정 파일 ---- */

/* UTF-8(BOM 허용) 텍스트 파일을 읽는다(malloc). 없거나 너무 크면 NULL. */
static wchar_t *read_text(const wchar_t *file)
{
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER size;
    DWORD got = 0;
    char *raw;
    wchar_t *text = NULL;
    int skip, n;

    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart > CONFIG_MAX_BYTES ||
        (raw = malloc((size_t)size.QuadPart + 1)) == NULL) {
        CloseHandle(h);
        return NULL;
    }
    if (!ReadFile(h, raw, (DWORD)size.QuadPart, &got, NULL)) {
        got = 0;
    }
    CloseHandle(h);
    skip = got >= 3 && memcmp(raw, "\xEF\xBB\xBF", 3) == 0 ? 3 : 0;
    n = MultiByteToWideChar(CP_UTF8, 0, raw + skip, (int)got - skip, NULL, 0);
    if (n >= 0) {
        text = malloc(((size_t)n + 1) * sizeof(*text));
        if (text != NULL) {
            MultiByteToWideChar(CP_UTF8, 0, raw + skip, (int)got - skip, text, n);
            text[n] = L'\0';
        }
    }
    free(raw);
    return text;
}

/* 다음 줄(제자리에서 끊는다). 끝이면 NULL. */
static wchar_t *next_line(wchar_t **cursor)
{
    wchar_t *line = *cursor, *end;

    if (line == NULL || *line == L'\0') {
        return NULL;
    }
    end = wcschr(line, L'\n');
    if (end != NULL) {
        *end = L'\0';
        *cursor = end + 1;
    } else {
        *cursor = line + wcslen(line);
    }
    return line;
}

static wchar_t *skip_space(wchar_t *s)
{
    while (*s == L' ' || *s == L'\t') {
        s++;
    }
    return s;
}

/*
 * "key = value" 줄들에서 key(대소문자 무시) 의 마지막 값을 찾는다(malloc). section 이 NULL 이 아니면
 * "[section]" 안에서만 찾는다. 주석(; #)은 건너뛴다. 구분자는 '=' (colon 이 1 이면 ':' 도).
 */
static wchar_t *config_value(const wchar_t *file, const wchar_t *section,
                             const wchar_t *const *keys, int colon)
{
    wchar_t *text = read_text(file), *cursor = text, *line, *found = NULL;
    int in_section = section == NULL;

    while ((line = next_line(&cursor)) != NULL) {
        wchar_t *p = skip_space(line), *sep, *end;
        size_t k;

        if (*p == L';' || *p == L'#' || *p == L'\0' || *p == L'\r') {
            continue;
        }
        if (*p == L'[') {
            end = wcschr(p, L']');
            if (section != NULL && end != NULL) {
                *end = L'\0';
                in_section = _wcsicmp(skip_space(p + 1), section) == 0;
            }
            continue;
        }
        if (!in_section) {
            continue;
        }
        sep = wcschr(p, L'=');
        if (colon) {
            wchar_t *c = wcschr(p, L':');
            /* "C:\..." 의 ':' 가 아니라 키 뒤의 ':' 만 구분자로 본다. */
            if (c != NULL && (sep == NULL || c < sep) && c > p && c[1] != L'\\' &&
                c[1] != L'/') {
                sep = c;
            }
        }
        if (sep == NULL) {
            continue;
        }
        end = sep;
        while (end > p && (end[-1] == L' ' || end[-1] == L'\t')) {
            end--;
        }
        for (k = 0; keys[k] != NULL; k++) {
            if ((size_t)(end - p) == wcslen(keys[k]) &&
                _wcsnicmp(p, keys[k], (size_t)(end - p)) == 0) {
                free(found);
                found = path_dup(sep + 1);
            }
        }
    }
    free(text);
    return found;
}

/* NuGet.Config 의 <add key="globalPackagesFolder" value="..." /> 값(malloc). */
static wchar_t *nuget_config_value(const wchar_t *file, const wchar_t *key)
{
    wchar_t *text = read_text(file), *p, *found = NULL;
    size_t klen = wcslen(key);

    for (p = text; p != NULL && *p != L'\0'; p++) {
        wchar_t *end, *v, quote, *close;
        if (_wcsnicmp(p, key, klen) != 0 || (p > text && p[-1] != L'"' && p[-1] != L'\'')) {
            continue;
        }
        end = wcschr(p, L'>');
        if (end == NULL) {
            break;
        }
        /* 같은 요소 안의 value= */
        for (v = p + klen; v < end && _wcsnicmp(v, L"value", 5) != 0; v++) {
        }
        if (v >= end) {
            continue;
        }
        v = skip_space(v + 5);
        if (*v != L'=') {
            continue;
        }
        v = skip_space(v + 1);
        quote = *v;
        if (quote != L'"' && quote != L'\'') {
            continue;
        }
        close = wcschr(v + 1, quote);
        if (close == NULL || close > end) {
            continue;
        }
        free(found);
        found = malloc(((size_t)(close - v)) * sizeof(*found));
        if (found != NULL) {
            wmemcpy(found, v + 1, (size_t)(close - v - 1));
            found[close - v - 1] = L'\0';
        }
        p = end;
    }
    free(text);
    return found;
}

/* ---- 캐시 구조 확인 ---- */

typedef enum {
    SIG_NAMES,        /* names 중 하나가 하위 폴더로 있음 */
    SIG_PREFIX,       /* names 중 하나로 시작하는 하위 폴더가 있음 */
    SIG_NUGET_PACKAGES, /* <id>\<버전>\.nupkg.metadata 또는 *.nupkg 가 있음 */
    SIG_DOLLAR        /* 이름에 '$' 가 든 하위 폴더가 있음(NuGet HTTP 캐시) */
} SigKind;

static int dir_exists(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);

    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* <ver> 폴더 안에 .nupkg.metadata 또는 *.nupkg 가 있는지. */
static int nuget_version_dir(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = path_join(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch,
                                          NULL, 0)
                       : INVALID_HANDLE_VALUE;
    int found = 0;
    size_t n;

    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        n = wcslen(fd.cFileName);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            (_wcsicmp(fd.cFileName, L".nupkg.metadata") == 0 ||
             (n > 6 && _wcsicmp(fd.cFileName + n - 6, L".nupkg") == 0))) {
            found = 1;
        }
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* dir 의 하위 폴더를 최대 SIGNATURE_PROBE 개 보며 조건을 확인한다. */
static int has_signature(const wchar_t *dir, SigKind kind, const wchar_t *const *names,
                         int depth)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = path_join(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch,
                                          NULL, 0)
                       : INVALID_HANDLE_VALUE;
    int found = 0, probed = 0;
    size_t k;

    free(pattern);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    do {
        const wchar_t *name = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || wcscmp(name, L".") == 0 ||
            wcscmp(name, L"..") == 0) {
            continue;
        }
        probed++;
        switch (kind) {
        case SIG_NAMES:
        case SIG_PREFIX:
            for (k = 0; names[k] != NULL && !found; k++) {
                found = kind == SIG_NAMES ? _wcsicmp(name, names[k]) == 0
                                          : _wcsnicmp(name, names[k], wcslen(names[k])) == 0;
            }
            break;
        case SIG_DOLLAR:
            found = wcschr(name, L'$') != NULL;
            break;
        case SIG_NUGET_PACKAGES: {
            wchar_t *child = path_join(dir, name);
            if (child != NULL) {
                /* <id>\<버전>\ 을 본다. */
                found = depth == 0 ? has_signature(child, SIG_NUGET_PACKAGES, names, 1)
                                   : nuget_version_dir(child);
                free(child);
            }
            break;
        }
        }
    } while (!found && probed < SIGNATURE_PROBE && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

/* 종류별 캐시 구조 확인. */
static int looks_like(DevCacheKind kind, const wchar_t *path)
{
    static const wchar_t *const npm[] = {L"_cacache", L"_npx", NULL};
    static const wchar_t *const pip[] = {L"http", L"http-v2", L"wheels", L"selfcheck", NULL};
    static const wchar_t *const gradle[] = {L"modules-2", L"build-cache-1", L"jars-",
                                            L"transforms-", L"journal-", L"0", L"1", L"2",
                                            L"3", L"4", L"5", L"6", L"7", L"8", L"9", NULL};
    static const wchar_t *const wrapper[] = {L"gradle-", NULL};
    static const wchar_t *const registry[] = {L"index", L"cache", L"src", NULL};
    static const wchar_t *const git[] = {L"db", L"checkouts", NULL};

    switch (kind) {
    case DEV_CACHE_NPM:
        return has_signature(path, SIG_NAMES, npm, 0);
    case DEV_CACHE_PIP:
        return has_signature(path, SIG_NAMES, pip, 0);
    case DEV_CACHE_GRADLE:
        return has_signature(path, SIG_PREFIX, gradle, 0);
    case DEV_CACHE_GRADLE_WRAPPER:
        return has_signature(path, SIG_PREFIX, wrapper, 0);
    case DEV_CACHE_CARGO_REGISTRY:
        return has_signature(path, SIG_NAMES, registry, 0);
    case DEV_CACHE_CARGO_GIT:
        return has_signature(path, SIG_NAMES, git, 0);
    case DEV_CACHE_NUGET_PACKAGES:
        return has_signature(path, SIG_NUGET_PACKAGES, NULL, 0);
    case DEV_CACHE_NUGET_HTTP:
        return has_signature(path, SIG_DOLLAR, NULL, 0);
    default:
        return 0;
    }
}

/* ---- 탐지 ---- */

/*
 * 항목을 추가한다. path 는 넘겨받아 소유한다(NULL 이면 NO_PATH). 상태는 실제 파일 시스템으로 정하며,
 * 기본 위치가 아닌 경로는 캐시 구조가 보여야 FOUND 다. 메모리가 부족하면 -1.
 */
static int add_entry(DevCacheList *l, DevCacheKind kind, DevCacheSource source,
                     const wchar_t *origin, wchar_t *path)
{
    DevCache *c;
    DWORD attrs;

    if (l->len == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        DevCache *grown = realloc(l->items, cap * sizeof(*grown));
        if (grown == NULL) {
            free(path);
            return -1;
        }
        l->items = grown;
        l->cap = cap;
    }
    c = &l->items[l->len];
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->source = source;
    c->path = path;
    if (origin != NULL && (c->origin = path_dup(origin)) == NULL) {
        free(path);
        return -1;
    }
    if (path == NULL) {
        c->status = DEV_CACHE_NO_PATH;
        l->len++;
        return 0;
    }
    if ((c->home = parent_of(path)) == NULL) {
        free(c->origin);
        free(path);
        return -1;
    }
    l->len++;
    attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        c->status = DEV_CACHE_NOT_FOUND;
    } else if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        c->status = DEV_CACHE_NOT_DIR;
    } else if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        c->status = DEV_CACHE_LINK;
    } else if (source != DEV_CACHE_SRC_DEFAULT && !looks_like(kind, path)) {
        c->status = DEV_CACHE_UNRECOGNIZED;
    } else {
        c->status = DEV_CACHE_FOUND;
    }
    return 0;
}

/* base\rel(malloc). base 가 NULL 이면 NULL. */
static wchar_t *under(const wchar_t *base, const wchar_t *rel)
{
    return base != NULL ? path_join(base, rel) : NULL;
}

typedef struct {
    wchar_t *profile; /* %USERPROFILE% */
    wchar_t *local;   /* %LOCALAPPDATA% */
    wchar_t *roaming; /* %APPDATA% */
} Bases;

/* 설정 파일 file 에서 경로 값을 찾아 CONFIG 항목으로 추가했으면 1(실패 -1, 없으면 0). */
static int add_from_config(DevCacheList *l, DevCacheKind kind, const wchar_t *file,
                           wchar_t *value)
{
    wchar_t *dir, *path;

    if (value == NULL) {
        return 0;
    }
    dir = parent_of(file);
    path = dir != NULL ? value_to_path(value, dir) : NULL;
    free(dir);
    free(value);
    if (path == NULL) {
        return 0;
    }
    return add_entry(l, kind, DEV_CACHE_SRC_CONFIG, file, path) == 0 ? 1 : -1;
}

static int detect_npm(DevCacheList *l, const Bases *b)
{
    static const wchar_t *const keys[] = {L"cache", NULL};
    wchar_t *p = env_path(L"npm_config_cache"), *rc;
    int r;

    if (p != NULL) {
        return add_entry(l, DEV_CACHE_NPM, DEV_CACHE_SRC_ENV, L"npm_config_cache", p);
    }
    rc = env_path(L"NPM_CONFIG_USERCONFIG");
    if (rc == NULL) {
        rc = under(b->profile, L".npmrc");
    }
    if (rc != NULL) {
        r = add_from_config(l, DEV_CACHE_NPM, rc, config_value(rc, NULL, keys, 0));
        free(rc);
        if (r != 0) {
            return r < 0 ? -1 : 0;
        }
    }
    return add_entry(l, DEV_CACHE_NPM, DEV_CACHE_SRC_DEFAULT, NULL, under(b->local, L"npm-cache"));
}

static int detect_pip(DevCacheList *l, const Bases *b)
{
    static const wchar_t *const keys[] = {L"cache-dir", L"cache_dir", NULL};
    wchar_t *files[3];
    wchar_t *p = env_path(L"PIP_CACHE_DIR");
    size_t i;
    int r = 0;

    if (p != NULL) {
        return add_entry(l, DEV_CACHE_PIP, DEV_CACHE_SRC_ENV, L"PIP_CACHE_DIR", p);
    }
    files[0] = env_path(L"PIP_CONFIG_FILE");
    files[1] = under(b->roaming, L"pip\\pip.ini");
    files[2] = under(b->profile, L"pip\\pip.ini");
    for (i = 0; i < 3 && r == 0; i++) {
        if (files[i] != NULL) {
            r = add_from_config(l, DEV_CACHE_PIP, files[i],
                                config_value(files[i], L"global", keys, 1));
        }
    }
    for (i = 0; i < 3; i++) {
        free(files[i]);
    }
    if (r != 0) {
        return r < 0 ? -1 : 0;
    }
    return add_entry(l, DEV_CACHE_PIP, DEV_CACHE_SRC_DEFAULT, NULL,
                     under(b->local, L"pip\\Cache"));
}

/* 도구 폴더(환경 변수 또는 기본) 아래 캐시 하위 폴더 항목. */
static int detect_home_sub(DevCacheList *l, DevCacheKind kind, const wchar_t *env,
                           const wchar_t *def, const wchar_t *sub, const Bases *b)
{
    wchar_t *home = env_path(env);
    DevCacheSource src = home != NULL ? DEV_CACHE_SRC_ENV : DEV_CACHE_SRC_DEFAULT;
    int r;

    if (home == NULL) {
        home = under(b->profile, def);
    }
    r = add_entry(l, kind, src, src == DEV_CACHE_SRC_ENV ? env : NULL, under(home, sub));
    free(home);
    return r;
}

static int detect_nuget(DevCacheList *l, const Bases *b)
{
    wchar_t *p = env_path(L"NUGET_PACKAGES"), *config;
    int r = 0;

    if (p != NULL) {
        r = add_entry(l, DEV_CACHE_NUGET_PACKAGES, DEV_CACHE_SRC_ENV, L"NUGET_PACKAGES", p);
    } else {
        config = under(b->roaming, L"NuGet\\NuGet.Config");
        if (config != NULL) {
            r = add_from_config(l, DEV_CACHE_NUGET_PACKAGES, config,
                                nuget_config_value(config, L"globalPackagesFolder"));
            free(config);
        }
        if (r == 0) {
            r = add_entry(l, DEV_CACHE_NUGET_PACKAGES, DEV_CACHE_SRC_DEFAULT, NULL,
                          under(b->profile, L".nuget\\packages"));
        } else {
            r = r < 0 ? -1 : 0;
        }
    }
    if (r != 0) {
        return r;
    }

    /* HTTP 캐시: 버전에 따라 v3-cache 또는 http-cache. 있는 것은 모두, 없으면 v3-cache 하나. */
    p = env_path(L"NUGET_HTTP_CACHE_PATH");
    if (p != NULL) {
        return add_entry(l, DEV_CACHE_NUGET_HTTP, DEV_CACHE_SRC_ENV, L"NUGET_HTTP_CACHE_PATH", p);
    }
    {
        wchar_t *v3 = under(b->local, L"NuGet\\v3-cache");
        wchar_t *http = under(b->local, L"NuGet\\http-cache");
        int has_http = http != NULL && GetFileAttributesW(http) != INVALID_FILE_ATTRIBUTES;
        int has_v3 = v3 != NULL && GetFileAttributesW(v3) != INVALID_FILE_ATTRIBUTES;

        if (has_v3 || !has_http) {
            r = add_entry(l, DEV_CACHE_NUGET_HTTP, DEV_CACHE_SRC_DEFAULT, NULL, v3);
            v3 = NULL;
        }
        if (r == 0 && has_http) {
            r = add_entry(l, DEV_CACHE_NUGET_HTTP, DEV_CACHE_SRC_DEFAULT, NULL, http);
            http = NULL;
        }
        free(v3);
        free(http);
    }
    return r;
}

int dev_cache_detect(DevCacheList *list)
{
    Bases b;
    int rc = 0;

    dev_cache_list_free(list);
    b.profile = user_dir(L"USERPROFILE", &FOLDERID_Profile);
    b.local = user_dir(L"LOCALAPPDATA", &FOLDERID_LocalAppData);
    b.roaming = user_dir(L"APPDATA", &FOLDERID_RoamingAppData);

    if (detect_npm(list, &b) != 0 || detect_pip(list, &b) != 0 ||
        detect_home_sub(list, DEV_CACHE_GRADLE, L"GRADLE_USER_HOME", L".gradle", L"caches",
                        &b) != 0 ||
        detect_home_sub(list, DEV_CACHE_GRADLE_WRAPPER, L"GRADLE_USER_HOME", L".gradle",
                        L"wrapper\\dists", &b) != 0 ||
        detect_home_sub(list, DEV_CACHE_CARGO_REGISTRY, L"CARGO_HOME", L".cargo", L"registry",
                        &b) != 0 ||
        detect_home_sub(list, DEV_CACHE_CARGO_GIT, L"CARGO_HOME", L".cargo", L"git", &b) != 0 ||
        detect_nuget(list, &b) != 0) {
        rc = -1;
    }
    free(b.profile);
    free(b.local);
    free(b.roaming);
    return rc;
}

/* ---- 용량 ---- */

typedef struct {
    dev_cache_cancel_fn cancel;
    void *ctx;
} CancelHook;

static int measure_cancel(void *ctx)
{
    const CancelHook *h = ctx;

    return h->cancel != NULL && h->cancel(h->ctx);
}

void dev_cache_measure(DevCache *cache, dev_cache_cancel_fn cancel, void *ctx)
{
    ScanReport report;
    ScanHooks hooks;
    CancelHook hook;
    ScanStatus status;

    if (cache == NULL || cache->status != DEV_CACHE_FOUND) {
        return;
    }
    memset(&hooks, 0, sizeof(hooks));
    hooks.should_cancel = measure_cancel;
    hook.cancel = cancel;
    hook.ctx = ctx;
    scan_report_init(&report);
    status = scan_tree_ex(cache->path, SCAN_DEPTH_UNLIMITED, 0, &report, &hooks, &hook);
    cache->measured = 1;
    cache->bytes = report.total_bytes;
    cache->files = report.file_count;
    cache->dirs = report.dir_count;
    cache->errors = report.error_count;
    cache->incomplete = status != SCAN_COMPLETE || report.cancelled;
    scan_report_free(&report);
}

void dev_cache_to_item(const DevCache *cache, CleanupItem *item)
{
    memset(item, 0, sizeof(*item));
    item->path = cache->path;
    item->category = CLEANUP_CACHE;
    item->eco = g_kinds[cache->kind].eco;
    item->flags = CLEANUP_ITEM_DIR | (cache->incomplete || !cache->measured
                                          ? CLEANUP_ITEM_INCOMPLETE
                                          : 0u);
    item->bytes = cache->bytes;
    item->files = cache->files;
    item->dirs = cache->dirs;
}

CleanupTrashStatus dev_cache_check(const CleanupGuard *guard, const DevCache *cache)
{
    CleanupItem item;

    switch (cache->status) {
    case DEV_CACHE_FOUND:
        break;
    case DEV_CACHE_LINK:
    case DEV_CACHE_UNRECOGNIZED:
        return CLEANUP_TRASH_CHANGED;
    default:
        return CLEANUP_TRASH_MISSING;
    }
    dev_cache_to_item(cache, &item);
    return cleanup_guard_check(guard, cache->home, &item);
}

/* ---- 작업 스레드 ---- */

struct DevCacheJob {
    HANDLE thread;
    dev_cache_notify_fn notify;
    void *ctx;
    volatile LONG cancel;
    ULONGLONG started;

    CRITICAL_SECTION lock; /* snapshot 보호 */
    DevCacheJobSnapshot snapshot;
    DevCacheList list; /* 스레드가 끝난 뒤에만 읽는다 */
};

static int job_cancelled(void *ctx)
{
    DevCacheJob *job = ctx;

    return InterlockedCompareExchange(&job->cancel, 0, 0) != 0;
}

static unsigned __stdcall cache_thread(void *arg)
{
    DevCacheJob *job = arg;
    size_t i, total = 0;

    dev_cache_detect(&job->list);
    for (i = 0; i < job->list.len; i++) {
        total += job->list.items[i].status == DEV_CACHE_FOUND;
    }
    EnterCriticalSection(&job->lock);
    job->snapshot.total = total;
    LeaveCriticalSection(&job->lock);
    if (job->notify != NULL) {
        job->notify(0, job->ctx);
    }
    for (i = 0; i < job->list.len; i++) {
        DevCache *c = &job->list.items[i];
        if (c->status != DEV_CACHE_FOUND) {
            continue;
        }
        EnterCriticalSection(&job->lock);
        job->snapshot.current = c->kind;
        LeaveCriticalSection(&job->lock);
        if (job_cancelled(job)) {
            c->incomplete = 1; /* 세지 않음(measured = 0) */
        } else {
            dev_cache_measure(c, job_cancelled, job);
        }
        EnterCriticalSection(&job->lock);
        job->snapshot.done++;
        job->snapshot.bytes += c->bytes;
        LeaveCriticalSection(&job->lock);
        if (job->notify != NULL) {
            job->notify(0, job->ctx);
        }
    }
    if (job->notify != NULL) {
        job->notify(1, job->ctx);
    }
    return 0;
}

DevCacheJob *dev_cache_job_start(dev_cache_notify_fn notify, void *ctx)
{
    DevCacheJob *job = calloc(1, sizeof(*job));

    if (job == NULL) {
        return NULL;
    }
    InitializeCriticalSection(&job->lock);
    dev_cache_list_init(&job->list);
    job->notify = notify;
    job->ctx = ctx;
    job->started = GetTickCount64();
    job->thread = (HANDLE)_beginthreadex(NULL, 0, cache_thread, job, 0, NULL);
    if (job->thread == NULL) {
        DeleteCriticalSection(&job->lock);
        free(job);
        return NULL;
    }
    return job;
}

void dev_cache_job_cancel(DevCacheJob *job)
{
    if (job != NULL) {
        InterlockedExchange(&job->cancel, 1);
    }
}

void dev_cache_job_progress(DevCacheJob *job, DevCacheJobSnapshot *out)
{
    EnterCriticalSection(&job->lock);
    *out = job->snapshot;
    LeaveCriticalSection(&job->lock);
    out->elapsed_ms = GetTickCount64() - job->started;
    out->cancel_requested = job_cancelled(job);
}

int dev_cache_job_finish(DevCacheJob *job, DevCacheList *out)
{
    int cancelled;

    if (job == NULL) {
        return 0;
    }
    WaitForSingleObject(job->thread, INFINITE);
    CloseHandle(job->thread);
    cancelled = job_cancelled(job);
    if (out != NULL) {
        dev_cache_list_free(out);
        *out = job->list;
    } else {
        dev_cache_list_free(&job->list);
    }
    DeleteCriticalSection(&job->lock);
    free(job);
    return cancelled;
}
