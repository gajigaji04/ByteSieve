#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "path.h"

static const wchar_t EXT_PREFIX[] = L"\\\\?\\";        /* \\?\ */
static const wchar_t EXT_UNC_PREFIX[] = L"\\\\?\\UNC\\"; /* \\?\UNC\ */
static const wchar_t DEVICE_PREFIX[] = L"\\\\.\\";     /* \\.\ */

#define EXT_PREFIX_LEN 4
#define EXT_UNC_PREFIX_LEN 8

int path_is_separator(wchar_t c)
{
    return c == L'\\' || c == L'/';
}

static int has_prefix(const wchar_t *s, const wchar_t *prefix, size_t len)
{
    return wcsncmp(s, prefix, len) == 0;
}

static int has_prefix_ci(const wchar_t *s, const wchar_t *prefix, size_t len)
{
    return _wcsnicmp(s, prefix, len) == 0;
}

static int is_drive_letter(wchar_t c)
{
    return (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

int path_is_extended(const wchar_t *path)
{
    return path != NULL && has_prefix(path, EXT_PREFIX, EXT_PREFIX_LEN);
}

int path_is_unc(const wchar_t *path)
{
    if (path == NULL) {
        return 0;
    }
    if (path_is_extended(path)) {
        return has_prefix_ci(path, EXT_UNC_PREFIX, EXT_UNC_PREFIX_LEN);
    }
    /* "\\server..." 이지만 "\\?\" 와 "\\.\" 는 제외한다. */
    return path_is_separator(path[0]) && path_is_separator(path[1]) &&
           path[2] != L'\0' && !path_is_separator(path[2]) &&
           !((path[2] == L'?' || path[2] == L'.') && path_is_separator(path[3]));
}

/* len 문자 + NUL 을 담을 버퍼를 할당한다. */
static wchar_t *alloc_chars(size_t len)
{
    if (len > PATH_MAX_WIDE) {
        return NULL;
    }
    return malloc((len + 1) * sizeof(wchar_t));
}

wchar_t *path_dup(const wchar_t *path)
{
    size_t len;
    wchar_t *out;

    if (path == NULL) {
        return NULL;
    }
    len = wcslen(path);
    out = alloc_chars(len);
    if (out != NULL) {
        memcpy(out, path, (len + 1) * sizeof(wchar_t));
    }
    return out;
}

/* prefix 뒤에 rest 를 이어 붙인 새 문자열을 만든다. */
static wchar_t *concat2(const wchar_t *prefix, size_t prefix_len, const wchar_t *rest)
{
    size_t rest_len = wcslen(rest);
    wchar_t *out;

    if (rest_len > PATH_MAX_WIDE - prefix_len) {
        return NULL;
    }
    out = alloc_chars(prefix_len + rest_len);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, prefix, prefix_len * sizeof(wchar_t));
    memcpy(out + prefix_len, rest, (rest_len + 1) * sizeof(wchar_t));
    return out;
}

wchar_t *path_join(const wchar_t *base, const wchar_t *name)
{
    size_t base_len, name_len, total;
    int need_sep;
    wchar_t *out;

    if (base == NULL || name == NULL) {
        return NULL;
    }

    while (path_is_separator(*name)) {
        name++;
    }

    base_len = wcslen(base);
    name_len = wcslen(name);

    need_sep = base_len > 0 && name_len > 0 &&
               !path_is_separator(base[base_len - 1]) &&
               !(base_len == 2 && base[1] == L':' && is_drive_letter(base[0]));

    if (base_len > PATH_MAX_WIDE || name_len > PATH_MAX_WIDE) {
        return NULL;
    }
    total = base_len + (size_t)need_sep + name_len;
    out = alloc_chars(total);
    if (out == NULL) {
        return NULL;
    }

    memcpy(out, base, base_len * sizeof(wchar_t));
    if (need_sep) {
        out[base_len] = L'\\';
    }
    memcpy(out + base_len + need_sep, name, (name_len + 1) * sizeof(wchar_t));
    return out;
}

wchar_t *path_full(const wchar_t *path)
{
    DWORD needed, written;
    wchar_t *out;

    if (path == NULL || path[0] == L'\0') {
        return NULL;
    }

    /* 버퍼 크기를 먼저 구한 뒤 할당한다(현재 디렉터리 변경에 대비해 재시도). */
    needed = GetFullPathNameW(path, 0, NULL, NULL);
    for (;;) {
        if (needed == 0 || needed - 1 > PATH_MAX_WIDE) {
            return NULL;
        }
        out = malloc((size_t)needed * sizeof(wchar_t));
        if (out == NULL) {
            return NULL;
        }
        written = GetFullPathNameW(path, needed, out, NULL);
        if (written == 0) {
            free(out);
            return NULL;
        }
        if (written < needed) {
            return out;
        }
        free(out);
        needed = written;
    }
}

wchar_t *path_to_extended(const wchar_t *path)
{
    wchar_t *full;
    wchar_t *out;

    if (path == NULL || path[0] == L'\0') {
        return NULL;
    }
    /* 확장/장치 경로는 정규화하지 않고 그대로 둔다. */
    if (path_is_extended(path) || has_prefix(path, DEVICE_PREFIX, EXT_PREFIX_LEN)) {
        return path_dup(path);
    }

    /* "\\?\" 경로는 Win32 정규화를 건너뛰므로 먼저 절대 경로로 정규화한다. */
    full = path_full(path);
    if (full == NULL) {
        return NULL;
    }

    if (path_is_extended(full) || has_prefix(full, DEVICE_PREFIX, EXT_PREFIX_LEN)) {
        return full;
    }

    if (path_is_unc(full)) {
        out = concat2(EXT_UNC_PREFIX, EXT_UNC_PREFIX_LEN, full + 2);
    } else if (is_drive_letter(full[0]) && full[1] == L':' && full[2] == L'\\') {
        out = concat2(EXT_PREFIX, EXT_PREFIX_LEN, full);
    } else {
        return full;
    }

    free(full);
    return out;
}

wchar_t *path_strip_extended(const wchar_t *path)
{
    if (path == NULL) {
        return NULL;
    }
    if (has_prefix_ci(path, EXT_UNC_PREFIX, EXT_UNC_PREFIX_LEN)) {
        return concat2(L"\\\\", 2, path + EXT_UNC_PREFIX_LEN);
    }
    if (path_is_extended(path)) {
        return path_dup(path + EXT_PREFIX_LEN);
    }
    return path_dup(path);
}
