#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cleanup_rules.h"
#include "path.h"
#include "quarantine.h"

#define META_NAME L"entry.txt"
#define META_TMP_NAME L"entry.tmp"
#define DATA_NAME L"f"
#define META_HEADER L"ByteSieve quarantine 1"
#define META_MAX_BYTES (64 * 1024)
#define TEST_ROOT_ENV L"BYTESIEVE_QUARANTINE_ROOT"

/* ---- 상태 이름 ---- */

const wchar_t *quarantine_status_text(QuarantineStatus status)
{
    switch (status) {
    case QUARANTINE_OK:
        return L"완료";
    case QUARANTINE_MISSING:
        return L"항목이 없음";
    case QUARANTINE_CONFLICT:
        return L"같은 이름이 이미 있음";
    case QUARANTINE_ACCESS_DENIED:
        return L"접근 거부(권한 부족·쓰기 금지·사용 중일 수 있음)";
    case QUARANTINE_IN_USE:
        return L"다른 프로그램이 사용 중";
    case QUARANTINE_NO_SPACE:
        return L"디스크 공간 부족";
    case QUARANTINE_BAD_STORE:
        return L"격리 폴더를 쓸 수 없음(링크·파일이거나 만들 수 없음)";
    case QUARANTINE_OTHER_VOLUME:
        return L"다른 드라이브라 옮길 수 없음";
    case QUARANTINE_FAILED:
        return L"실패";
    default:
        return L"";
    }
}

const wchar_t *quarantine_status_name(QuarantineStatus status)
{
    switch (status) {
    case QUARANTINE_OK:
        return L"ok";
    case QUARANTINE_MISSING:
        return L"missing";
    case QUARANTINE_CONFLICT:
        return L"conflict";
    case QUARANTINE_ACCESS_DENIED:
        return L"access_denied";
    case QUARANTINE_IN_USE:
        return L"in_use";
    case QUARANTINE_NO_SPACE:
        return L"no_space";
    case QUARANTINE_BAD_STORE:
        return L"bad_store";
    case QUARANTINE_OTHER_VOLUME:
        return L"other_volume";
    default:
        return L"failed";
    }
}

QuarantineStatus quarantine_status_from_error(uint32_t error)
{
    switch (error) {
    case 0:
        return QUARANTINE_OK;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return QUARANTINE_MISSING;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return QUARANTINE_CONFLICT;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
    case ERROR_PRIVILEGE_NOT_HELD:
        return QUARANTINE_ACCESS_DENIED;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE:
        return QUARANTINE_IN_USE;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return QUARANTINE_NO_SPACE;
    case ERROR_NOT_SAME_DEVICE:
        return QUARANTINE_OTHER_VOLUME;
    default:
        return QUARANTINE_FAILED;
    }
}

static QuarantineStatus fail(uint32_t *code, DWORD err)
{
    if (code != NULL) {
        *code = err;
    }
    return quarantine_status_from_error(err);
}

/* ---- 경로 ---- */

static int name_eq_n(const wchar_t *a, const wchar_t *b, size_t len)
{
    return len == 0 || CompareStringOrdinal(a, (int)len, b, (int)len, TRUE) == CSTR_EQUAL;
}

/* 끝 구분자를 뺀 길이. */
static size_t trimmed_len(const wchar_t *s)
{
    size_t n = wcslen(s);

    while (n > 0 && path_is_separator(s[n - 1])) {
        n--;
    }
    return n;
}

/* path 가 root 아래에 있으면 root 다음 부분(구분자 뒤), 아니면 NULL. root 자체도 NULL. */
static const wchar_t *relative_to(const wchar_t *root, const wchar_t *path)
{
    size_t rlen = trimmed_len(root);
    const wchar_t *rest;

    if (rlen == 0 || wcslen(path) <= rlen || !name_eq_n(root, path, rlen) ||
        !path_is_separator(path[rlen])) {
        return NULL;
    }
    rest = path + rlen;
    while (path_is_separator(*rest)) {
        rest++;
    }
    return *rest != L'\0' ? rest : NULL;
}

/* 마지막 구성 요소. */
static const wchar_t *last_name(const wchar_t *path)
{
    const wchar_t *name = path;
    const wchar_t *s;

    for (s = path; *s != L'\0'; s++) {
        if (path_is_separator(*s) && s[1] != L'\0') {
            name = s + 1;
        }
    }
    return name;
}

static DWORD attrs_of(const wchar_t *path, DWORD *err)
{
    wchar_t *ext = path_to_extended(path);
    DWORD attrs = INVALID_FILE_ATTRIBUTES;

    *err = ERROR_NOT_ENOUGH_MEMORY;
    if (ext != NULL) {
        attrs = GetFileAttributesW(ext);
        *err = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        free(ext);
    }
    return attrs;
}

static int exists(const wchar_t *path)
{
    DWORD err;

    /* 알 수 없는 오류는 "있다"로 본다(덮어쓰지 않기 위해). */
    return attrs_of(path, &err) != INVALID_FILE_ATTRIBUTES ||
           (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND);
}

static wchar_t *join3(const wchar_t *a, const wchar_t *b, const wchar_t *c)
{
    wchar_t *ab = path_join(a, b), *abc = NULL;

    if (ab != NULL) {
        abc = path_join(ab, c);
        free(ab);
    }
    return abc;
}

/* 파일 시스템 함수: 확장 경로로 바꿔 부른다. 실패하면 0 과 GetLastError. */
static BOOL create_dir(const wchar_t *path)
{
    wchar_t *ext = path_to_extended(path);
    BOOL ok = ext != NULL && CreateDirectoryW(ext, NULL);
    DWORD err = ext != NULL ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;

    free(ext);
    SetLastError(ok ? 0 : err);
    return ok;
}

static BOOL remove_dir(const wchar_t *path)
{
    wchar_t *ext = path_to_extended(path);
    BOOL ok = ext != NULL && RemoveDirectoryW(ext);

    free(ext);
    return ok;
}

static BOOL delete_file(const wchar_t *path)
{
    wchar_t *ext = path_to_extended(path);
    BOOL ok = ext != NULL && DeleteFileW(ext);

    free(ext);
    return ok;
}

/* 덮어쓰지 않고, 다른 볼륨으로 복사하지 않는 이동(이름 바꾸기). */
static DWORD rename_no_replace(const wchar_t *from, const wchar_t *to)
{
    wchar_t *a = path_to_extended(from), *b = path_to_extended(to);
    DWORD err = 0;

    if (a == NULL || b == NULL) {
        err = ERROR_NOT_ENOUGH_MEMORY;
    } else if (!MoveFileExW(a, b, MOVEFILE_WRITE_THROUGH)) {
        err = GetLastError();
    }
    free(a);
    free(b);
    return err;
}

/* 실제 위치가 path 와 같은지(중간에 정션·subst 가 끼면 다르다). */
static int same_final_path(const wchar_t *path)
{
    wchar_t *ext = path_to_extended(path), *final_path = NULL, *long_path = NULL;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD n;
    int same = 0;

    if (ext == NULL) {
        return 0;
    }
    h = CreateFileW(ext, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                    NULL);
    final_path = malloc((PATH_MAX_WIDE + 1) * sizeof(*final_path));
    long_path = malloc((PATH_MAX_WIDE + 1) * sizeof(*long_path));
    if (h != INVALID_HANDLE_VALUE && final_path != NULL && long_path != NULL) {
        n = GetFinalPathNameByHandleW(h, final_path, PATH_MAX_WIDE + 1,
                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n > 0 && n <= PATH_MAX_WIDE) {
            /* 짧은 이름(PROGRA~1)으로 만든 경로도 같은 위치로 보도록 긴 이름으로 바꾼다. */
            DWORD m = GetLongPathNameW(ext, long_path, PATH_MAX_WIDE + 1);
            if (m > 0 && m <= PATH_MAX_WIDE) {
                size_t alen = trimmed_len(final_path), blen = trimmed_len(long_path);
                same = alen == blen && name_eq_n(final_path, long_path, alen);
            }
        }
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    free(final_path);
    free(long_path);
    free(ext);
    return same;
}

/* ---- 볼륨 루트·격리 폴더 ---- */

/* 테스트용 루트(BYTESIEVE_QUARANTINE_ROOT, 정규화). 없으면 NULL. */
static wchar_t *test_root(void)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(TEST_ROOT_ENV, buf, MAX_PATH);

    if (n == 0 || n >= MAX_PATH) {
        return NULL;
    }
    return path_full(buf);
}

int quarantine_test_root_contains(const wchar_t *path)
{
    wchar_t *root;
    int inside;

    if (path == NULL) {
        return 0;
    }
    root = test_root();
    inside = root != NULL && relative_to(root, path) != NULL;
    free(root);
    return inside;
}

wchar_t *quarantine_volume_root(const wchar_t *path)
{
    wchar_t buf[MAX_PATH + 1];
    wchar_t *root;

    if (path == NULL) {
        return NULL;
    }
    root = test_root();
    if (root != NULL && relative_to(root, path) != NULL) {
        return root;
    }
    free(root);
    if (!GetVolumePathNameW(path, buf, MAX_PATH + 1)) {
        return NULL;
    }
    return path_dup(buf);
}

wchar_t *quarantine_store_path(const wchar_t *volume_root)
{
    return volume_root != NULL ? path_join(volume_root, CLEANUP_QUARANTINE_DIR) : NULL;
}

/* 격리 폴더가 있으면 확인하고, 없으면 숨김 폴더로 만든다. */
static QuarantineStatus open_store(const wchar_t *store, uint32_t *code)
{
    DWORD err, attrs;

    if (create_dir(store)) {
        wchar_t *ext = path_to_extended(store);
        if (ext != NULL) {
            SetFileAttributesW(ext, FILE_ATTRIBUTE_HIDDEN);
            free(ext);
        }
        return QUARANTINE_OK;
    }
    err = GetLastError();
    if (err != ERROR_ALREADY_EXISTS) {
        QuarantineStatus st = fail(code, err);
        return st == QUARANTINE_FAILED || st == QUARANTINE_MISSING ? QUARANTINE_BAD_STORE : st;
    }
    attrs = attrs_of(store, &err);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (code != NULL) {
            *code = attrs == INVALID_FILE_ATTRIBUTES ? err : ERROR_CANT_ACCESS_FILE;
        }
        return QUARANTINE_BAD_STORE;
    }
    return QUARANTINE_OK;
}

/* ---- 기록 ---- */

typedef struct {
    wchar_t *original;
    wchar_t *relative;
    uint64_t time;
    uint64_t bytes;
    int is_dir;
} Meta;

static void meta_free(Meta *m)
{
    free(m->original);
    free(m->relative);
    memset(m, 0, sizeof(*m));
}

/* 기록 파일을 임시 파일에 쓴 뒤 바꿔치기한다. 성공 0, 실패 Windows 오류. */
static DWORD write_meta(const wchar_t *entry, const Meta *m, const wchar_t *state)
{
    wchar_t *text, *tmp = path_join(entry, META_TMP_NAME), *dst = path_join(entry, META_NAME);
    wchar_t *tmp_ext = tmp != NULL ? path_to_extended(tmp) : NULL;
    wchar_t *dst_ext = dst != NULL ? path_to_extended(dst) : NULL;
    size_t cap = wcslen(m->original) + wcslen(m->relative) + 256;
    char *utf8 = NULL;
    int len, n;
    HANDLE h;
    DWORD err = 0, written;

    text = malloc(cap * sizeof(*text));
    if (text == NULL || tmp_ext == NULL || dst_ext == NULL) {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    n = swprintf(text, cap,
                 META_HEADER L"\noriginal=%ls\nrelative=%ls\ntime=%llu\nkind=%ls\nbytes=%llu\n"
                             L"state=%ls\n",
                 m->original, m->relative, (unsigned long long)m->time,
                 m->is_dir ? L"dir" : L"file", (unsigned long long)m->bytes, state);
    len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, text, n, NULL, 0, NULL, NULL) : 0;
    utf8 = len > 0 ? malloc((size_t)len) : NULL;
    if (utf8 == NULL || WideCharToMultiByte(CP_UTF8, 0, text, n, utf8, len, NULL, NULL) != len) {
        err = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    h = CreateFileW(tmp_ext, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        goto done;
    }
    if (!WriteFile(h, utf8, (DWORD)len, &written, NULL) || written != (DWORD)len ||
        !FlushFileBuffers(h)) {
        err = GetLastError();
        if (err == 0) {
            err = ERROR_WRITE_FAULT;
        }
    }
    CloseHandle(h);
    if (err == 0 &&
        !MoveFileExW(tmp_ext, dst_ext, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = GetLastError();
    }
    if (err != 0) {
        DeleteFileW(tmp_ext);
    }
done:
    free(text);
    free(utf8);
    free(tmp);
    free(dst);
    free(tmp_ext);
    free(dst_ext);
    return err;
}

/* 복원 위치로 써도 되는 상대 경로인지("..", 드라이브, 격리 폴더 이름이 없음). */
static int valid_relative(const wchar_t *rel)
{
    const wchar_t *s = rel;

    if (rel[0] == L'\0' || path_is_separator(rel[0]) || wcschr(rel, L':') != NULL ||
        cleanup_in_quarantine(rel)) {
        return 0;
    }
    while (*s != L'\0') {
        size_t len = 0;
        while (s[len] != L'\0' && !path_is_separator(s[len])) {
            len++;
        }
        if ((len == 1 && s[0] == L'.') || (len == 2 && s[0] == L'.' && s[1] == L'.')) {
            return 0;
        }
        s += len;
        while (path_is_separator(*s)) {
            s++;
        }
    }
    return 1;
}

static int parse_u64(const wchar_t *s, uint64_t *out)
{
    wchar_t *end;

    if (*s < L'0' || *s > L'9') {
        return 0;
    }
    *out = (uint64_t)_wcstoui64(s, &end, 10);
    return *end == L'\0';
}

/* 기록을 읽는다. 없거나 형식이 틀리면 0(고아로 다룬다). */
static int read_meta(const wchar_t *entry, Meta *m)
{
    wchar_t *path = path_join(entry, META_NAME), *ext = path != NULL ? path_to_extended(path) : NULL;
    wchar_t *text = NULL, *line, *next;
    char *buf = NULL;
    HANDLE h = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    DWORD got = 0;
    int wlen, ok = 0, kind = -1, header = 0;

    memset(m, 0, sizeof(*m));
    if (ext != NULL) {
        h = CreateFileW(ext, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > META_MAX_BYTES) {
        goto done;
    }
    buf = malloc((size_t)size.QuadPart);
    if (buf == NULL || !ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) ||
        got != (DWORD)size.QuadPart) {
        goto done;
    }
    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, (int)got, NULL, 0);
    text = wlen > 0 ? malloc(((size_t)wlen + 1) * sizeof(*text)) : NULL;
    if (text == NULL || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf, (int)got, text,
                                            wlen) != wlen) {
        goto done;
    }
    text[wlen] = L'\0';

    for (line = text; line != NULL && *line != L'\0'; line = next) {
        wchar_t *eq;
        size_t n;

        next = wcschr(line, L'\n');
        if (next != NULL) {
            *next++ = L'\0';
        }
        n = wcslen(line);
        if (n > 0 && line[n - 1] == L'\r') {
            line[--n] = L'\0';
        }
        if (!header) {
            if (wcscmp(line, META_HEADER) != 0) {
                goto done;
            }
            header = 1;
            continue;
        }
        eq = wcschr(line, L'=');
        if (eq == NULL) {
            continue;
        }
        *eq++ = L'\0';
        if (wcscmp(line, L"original") == 0 && m->original == NULL) {
            m->original = path_dup(eq);
        } else if (wcscmp(line, L"relative") == 0 && m->relative == NULL) {
            m->relative = path_dup(eq);
        } else if (wcscmp(line, L"time") == 0) {
            parse_u64(eq, &m->time);
        } else if (wcscmp(line, L"bytes") == 0) {
            parse_u64(eq, &m->bytes);
        } else if (wcscmp(line, L"kind") == 0) {
            kind = wcscmp(eq, L"dir") == 0 ? 1 : wcscmp(eq, L"file") == 0 ? 0 : -1;
        }
    }
    if (header && m->original != NULL && m->relative != NULL && kind >= 0 &&
        valid_relative(m->relative)) {
        m->is_dir = kind;
        ok = 1;
    }
done:
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    if (!ok) {
        meta_free(m);
    }
    free(text);
    free(buf);
    free(ext);
    free(path);
    return ok;
}

/* 기록 파일과 빈 폴더만 지운다(폴더 안에 다른 것이 있으면 그대로 둔다). */
static void discard_entry(const wchar_t *entry)
{
    wchar_t *meta = path_join(entry, META_NAME), *tmp = path_join(entry, META_TMP_NAME);
    wchar_t *data = path_join(entry, DATA_NAME);

    if (meta != NULL) {
        delete_file(meta);
    }
    if (tmp != NULL) {
        delete_file(tmp);
    }
    if (data != NULL) {
        remove_dir(data);
    }
    remove_dir(entry);
    free(meta);
    free(tmp);
    free(data);
}

/* ---- 격리 ---- */

/* 격리한 시각 + 번호로 새 항목 폴더를 만든다. */
static wchar_t *create_entry(const wchar_t *store, DWORD *err)
{
    SYSTEMTIME st;
    wchar_t id[48];
    unsigned n;

    GetLocalTime(&st);
    for (n = 1; n < 10000; n++) {
        wchar_t *entry;
        swprintf(id, 48, L"%04u%02u%02u-%02u%02u%02u-%u", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond, n);
        entry = path_join(store, id);
        if (entry == NULL) {
            *err = ERROR_NOT_ENOUGH_MEMORY;
            return NULL;
        }
        if (create_dir(entry)) {
            return entry;
        }
        *err = GetLastError();
        free(entry);
        if (*err != ERROR_ALREADY_EXISTS) {
            return NULL;
        }
    }
    return NULL;
}

QuarantineStatus quarantine_move(const wchar_t *volume_root, const wchar_t *path, int is_dir,
                                 uint64_t bytes, wchar_t **moved_path, uint32_t *code)
{
    wchar_t *store = NULL, *entry = NULL, *data = NULL, *dest = NULL;
    const wchar_t *rel;
    QuarantineStatus st;
    FILETIME now;
    Meta m;
    DWORD err;

    if (code != NULL) {
        *code = 0;
    }
    if (moved_path != NULL) {
        *moved_path = NULL;
    }
    if (volume_root == NULL || path == NULL) {
        return fail(code, ERROR_INVALID_PARAMETER);
    }
    rel = relative_to(volume_root, path);
    if (rel == NULL) {
        return fail(code, ERROR_NOT_SAME_DEVICE);
    }
    if (cleanup_in_quarantine(path) || !valid_relative(rel)) {
        return fail(code, ERROR_INVALID_PARAMETER);
    }
    if (attrs_of(path, &err) == INVALID_FILE_ATTRIBUTES) {
        return fail(code, err);
    }

    store = quarantine_store_path(volume_root);
    if (store == NULL) {
        return fail(code, ERROR_NOT_ENOUGH_MEMORY);
    }
    st = open_store(store, code);
    if (st != QUARANTINE_OK) {
        free(store);
        return st;
    }
    entry = create_entry(store, &err);
    data = entry != NULL ? path_join(entry, DATA_NAME) : NULL;
    if (entry == NULL || data == NULL || !create_dir(data)) {
        if (entry != NULL) {
            err = data == NULL ? ERROR_NOT_ENOUGH_MEMORY : GetLastError();
            discard_entry(entry);
        }
        st = fail(code, err);
        goto done;
    }

    /* 옮기기 전에 기록부터 남긴다(옮기는 도중 멈춰도 원래 위치를 알 수 있게). */
    GetSystemTimeAsFileTime(&now);
    memset(&m, 0, sizeof(m));
    m.original = (wchar_t *)path;
    m.relative = (wchar_t *)rel;
    m.time = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    m.bytes = bytes;
    m.is_dir = is_dir != 0;
    err = write_meta(entry, &m, L"moving");
    dest = err == 0 ? path_join(data, last_name(path)) : NULL;
    if (err == 0 && dest == NULL) {
        err = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (err == 0) {
        err = rename_no_replace(path, dest);
    }
    if (err != 0) {
        /* 아무것도 옮기지 않았다: 기록과 빈 폴더만 지운다. */
        discard_entry(entry);
        st = fail(code, err);
        goto done;
    }
    /* 고치지 못해도 목록은 실제 파일이 있는지로 판단하므로 괜찮다. */
    write_meta(entry, &m, L"moved");
    st = QUARANTINE_OK;
    if (moved_path != NULL) {
        *moved_path = dest;
        dest = NULL;
    }
done:
    free(dest);
    free(data);
    free(entry);
    free(store);
    return st;
}

/* ---- 목록 ---- */

void quarantine_list_init(QuarantineList *list)
{
    memset(list, 0, sizeof(*list));
}

static void entry_free(QuarantineEntry *e)
{
    free(e->volume_root);
    free(e->store);
    free(e->entry);
    free(e->item);
    free(e->original);
    free(e->restore);
    memset(e, 0, sizeof(*e));
}

void quarantine_list_free(QuarantineList *list)
{
    size_t i;

    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->len; i++) {
        entry_free(&list->items[i]);
    }
    free(list->items);
    quarantine_list_init(list);
}

static int list_push(QuarantineList *list, QuarantineEntry *e)
{
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 16;
        QuarantineEntry *grown = realloc(list->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        list->items = grown;
        list->cap = cap;
    }
    list->items[list->len++] = *e;
    return 0;
}

static uint64_t filetime_u64(FILETIME ft)
{
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* 격리 폴더 바로 아래 항목 하나를 읽는다. */
static int read_entry(const wchar_t *volume_root, const wchar_t *store, const WIN32_FIND_DATAW *fd,
                      QuarantineEntry *e)
{
    int is_dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int is_link = (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    Meta m;

    memset(e, 0, sizeof(*e));
    e->volume_root = path_dup(volume_root);
    e->store = path_dup(store);
    e->entry = path_join(store, fd->cFileName);
    if (e->volume_root == NULL || e->store == NULL || e->entry == NULL) {
        return -1;
    }
    if (is_dir && !is_link && read_meta(e->entry, &m)) {
        e->original = m.original;
        e->restore = path_join(volume_root, m.relative);
        e->item = join3(e->entry, DATA_NAME, last_name(m.relative));
        e->time = m.time;
        e->bytes = m.bytes;
        e->is_dir = m.is_dir;
        m.original = NULL;
        meta_free(&m);
        if (e->restore == NULL || e->item == NULL) {
            return -1;
        }
        /* 기록이 "moving" 이어도 항목이 옮겨져 있으면 정상으로 본다. */
        if (!exists(e->item)) {
            free(e->item);
            e->item = NULL;
            e->state = QUARANTINE_ENTRY_MISSING;
        }
        return 0;
    }
    /* 기록이 없거나 손상됨: 원래 위치를 모르므로 그 항목 전체를 그대로 보여 준다. */
    e->state = QUARANTINE_ENTRY_ORPHAN;
    e->item = path_dup(e->entry);
    e->is_dir = is_dir;
    e->bytes = is_dir ? 0 : ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
    e->time = filetime_u64(fd->ftCreationTime);
    return e->item != NULL ? 0 : -1;
}

QuarantineStatus quarantine_list_volume(const wchar_t *volume_root, QuarantineList *list)
{
    wchar_t *store, *pattern, *ext;
    WIN32_FIND_DATAW fd;
    HANDLE find;
    DWORD attrs, err;
    QuarantineStatus st = QUARANTINE_OK;

    if (volume_root == NULL || list == NULL) {
        return QUARANTINE_FAILED;
    }
    store = quarantine_store_path(volume_root);
    if (store == NULL) {
        return QUARANTINE_FAILED;
    }
    attrs = attrs_of(store, &err);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        free(store);
        /* 격리 폴더가 없거나 미디어가 없는 드라이브 */
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
                       err == ERROR_NOT_READY
                   ? QUARANTINE_OK
                   : quarantine_status_from_error(err);
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY) || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(store);
        return QUARANTINE_BAD_STORE;
    }
    pattern = path_join(store, L"*");
    ext = pattern != NULL ? path_to_extended(pattern) : NULL;
    find = ext != NULL ? FindFirstFileExW(ext, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0)
                       : INVALID_HANDLE_VALUE;
    if (find == INVALID_HANDLE_VALUE) {
        err = ext != NULL ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
        st = err == ERROR_FILE_NOT_FOUND ? QUARANTINE_OK : quarantine_status_from_error(err);
    } else {
        do {
            QuarantineEntry e;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            if (read_entry(volume_root, store, &fd, &e) != 0 || list_push(list, &e) != 0) {
                entry_free(&e);
                st = QUARANTINE_FAILED;
                break;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    free(ext);
    free(pattern);
    free(store);
    return st;
}

static int by_time_desc(const void *a, const void *b)
{
    const QuarantineEntry *x = a, *y = b;

    if (x->time != y->time) {
        return x->time > y->time ? -1 : 1;
    }
    return wcscmp(x->entry, y->entry);
}

size_t quarantine_list_system(QuarantineList *list)
{
    DWORD drives = GetLogicalDrives();
    wchar_t root[4] = L"A:\\", *extra;
    size_t failed = 0;
    int i;

    quarantine_list_free(list);
    for (i = 0; i < 26; i++) {
        UINT type;
        if (!(drives & (1u << i))) {
            continue;
        }
        root[0] = (wchar_t)(L'A' + i);
        type = GetDriveTypeW(root);
        if ((type == DRIVE_FIXED || type == DRIVE_REMOVABLE) &&
            quarantine_list_volume(root, list) != QUARANTINE_OK) {
            failed++;
        }
    }
    extra = test_root();
    if (extra != NULL && quarantine_list_volume(extra, list) != QUARANTINE_OK) {
        failed++;
    }
    free(extra);
    if (list->len > 1) {
        qsort(list->items, list->len, sizeof(*list->items), by_time_desc);
    }
    return failed;
}

/* ---- 복원 ---- */

/* target 의 상위 폴더 중 없는 것을 만든다. 성공 0. */
static DWORD make_parents(const wchar_t *target)
{
    wchar_t *ext = path_to_extended(target), *s, *last;
    DWORD err = 0;

    if (ext == NULL) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    last = wcsrchr(ext, L'\\');
    /* "\\?\X:\" 다음부터 구분자마다 잘라 만든다(볼륨 루트 바로 아래면 만들 것이 없다). */
    for (s = wcslen(ext) > 7 ? wcschr(ext + 7, L'\\') : NULL; s != NULL && s <= last;
         s = wcschr(s + 1, L'\\')) {
        *s = L'\0';
        /* 쓰기 권한이 없는 상위 폴더(C:\Users 등)는 이미 있어도 ACCESS_DENIED 가 나므로 먼저 본다. */
        if (GetFileAttributesW(ext) == INVALID_FILE_ATTRIBUTES && !CreateDirectoryW(ext, NULL)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) {
                err = e;
                *s = L'\\';
                break;
            }
        }
        *s = L'\\';
    }
    free(ext);
    return err;
}

QuarantineStatus quarantine_restore(const QuarantineEntry *e, const wchar_t *target,
                                    uint32_t *code)
{
    DWORD err;

    if (code != NULL) {
        *code = 0;
    }
    if (e == NULL || e->state != QUARANTINE_ENTRY_OK || e->item == NULL) {
        return fail(code, ERROR_FILE_NOT_FOUND);
    }
    if (target == NULL) {
        target = e->restore;
    }
    if (target == NULL || cleanup_in_quarantine(target)) {
        return fail(code, ERROR_INVALID_PARAMETER);
    }
    /* 덮어쓰지 않는다. 확인할 수 없는 경우도 있는 것으로 본다. */
    if (exists(target)) {
        return fail(code, ERROR_ALREADY_EXISTS);
    }
    err = make_parents(target);
    if (err == 0) {
        err = rename_no_replace(e->item, target);
    }
    if (err != 0) {
        return fail(code, err);
    }
    discard_entry(e->entry);
    return QUARANTINE_OK;
}

wchar_t *quarantine_alternate_path(const wchar_t *path, int is_dir)
{
    const wchar_t *name, *dot = NULL;
    size_t stem, cap;
    wchar_t *buf;
    unsigned n;

    if (path == NULL) {
        return NULL;
    }
    name = last_name(path);
    if (!is_dir) {
        dot = wcsrchr(name, L'.');
        if (dot == name) {
            dot = NULL; /* ".gitignore" 는 확장자가 아니라 이름 */
        }
    }
    stem = dot != NULL ? (size_t)(dot - path) : trimmed_len(path);
    cap = wcslen(path) + 32;
    buf = malloc(cap * sizeof(*buf));
    if (buf == NULL) {
        return NULL;
    }
    for (n = 1; n < 1000; n++) {
        wchar_t suffix[24];
        if (n == 1) {
            swprintf(suffix, 24, L" (복원됨)");
        } else {
            swprintf(suffix, 24, L" (복원됨 %u)", n);
        }
        wmemcpy(buf, path, stem);
        swprintf(buf + stem, cap - stem, L"%ls%ls", suffix, dot != NULL ? dot : L"");
        if (!exists(buf)) {
            return buf;
        }
    }
    free(buf);
    return NULL;
}

/* ---- 영구 삭제 ---- */

/*
 * path 를 지운다(확장 경로). 재분석 지점은 따라가지 않고 링크만 지운다.
 * 지우지 못한 것이 있어도 나머지는 계속 지우고 첫 오류를 반환한다.
 */
static DWORD delete_tree(const wchar_t *ext)
{
    DWORD attrs = GetFileAttributesW(ext), first = 0;

    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ? 0 : err;
    }
    if (attrs & FILE_ATTRIBUTE_READONLY) {
        SetFileAttributesW(ext, attrs & ~FILE_ATTRIBUTE_READONLY);
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t *pattern = path_join(ext, L"*");
        WIN32_FIND_DATAW *fd = malloc(sizeof(*fd));
        HANDLE find = INVALID_HANDLE_VALUE;

        if (pattern != NULL && fd != NULL) {
            find = FindFirstFileExW(pattern, FindExInfoBasic, fd, FindExSearchNameMatch, NULL,
                                    FIND_FIRST_EX_LARGE_FETCH);
        }
        if (find == INVALID_HANDLE_VALUE) {
            first = pattern != NULL && fd != NULL ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
            if (first == ERROR_FILE_NOT_FOUND) {
                first = 0;
            }
        } else {
            do {
                wchar_t *child;
                DWORD err;
                if (wcscmp(fd->cFileName, L".") == 0 || wcscmp(fd->cFileName, L"..") == 0) {
                    continue;
                }
                child = path_join(ext, fd->cFileName);
                err = child != NULL ? delete_tree(child) : ERROR_NOT_ENOUGH_MEMORY;
                if (err != 0 && first == 0) {
                    first = err;
                }
                free(child);
            } while (FindNextFileW(find, fd));
            FindClose(find);
        }
        free(fd);
        free(pattern);
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        if (!RemoveDirectoryW(ext) && first == 0) {
            first = GetLastError();
        }
    } else if (!DeleteFileW(ext) && first == 0) {
        first = GetLastError();
    }
    return first;
}

/* e 가 격리 폴더 바로 아래 항목이고, 격리 폴더가 링크가 아닌 실제 폴더인지. */
static int purge_allowed(const QuarantineEntry *e)
{
    const wchar_t *id;
    DWORD attrs, err;

    if (e->store == NULL || e->entry == NULL || e->item == NULL ||
        !cleanup_is_quarantine_name(last_name(e->store))) {
        return 0;
    }
    id = relative_to(e->store, e->entry);
    if (id == NULL || wcschr(id, L'\\') != NULL || wcschr(id, L'/') != NULL) {
        return 0;
    }
    /* 격리된 항목은 항목 폴더와 같거나(고아) 그 안에 있어야 한다. */
    if (wcscmp(e->item, e->entry) != 0 && relative_to(e->entry, e->item) == NULL) {
        return 0;
    }
    attrs = attrs_of(e->store, &err);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) && same_final_path(e->store);
}

QuarantineStatus quarantine_purge(const QuarantineEntry *e, uint32_t *code)
{
    wchar_t *ext;
    DWORD err = 0;

    if (code != NULL) {
        *code = 0;
    }
    if (e == NULL) {
        return fail(code, ERROR_INVALID_PARAMETER);
    }
    if (e->state == QUARANTINE_ENTRY_MISSING) {
        /* 기록만 남은 항목: 기록과 빈 폴더만 지운다. */
        if (e->store == NULL || e->entry == NULL || relative_to(e->store, e->entry) == NULL) {
            return fail(code, ERROR_INVALID_PARAMETER);
        }
        discard_entry(e->entry);
        return QUARANTINE_OK;
    }
    if (!purge_allowed(e)) {
        if (code != NULL) {
            *code = ERROR_CANT_ACCESS_FILE;
        }
        return QUARANTINE_BAD_STORE;
    }
    ext = path_to_extended(e->item);
    if (ext == NULL) {
        return fail(code, ERROR_NOT_ENOUGH_MEMORY);
    }
    err = delete_tree(ext);
    free(ext);
    if (err != 0) {
        return fail(code, err);
    }
    if (e->state == QUARANTINE_ENTRY_OK) {
        discard_entry(e->entry);
    }
    return QUARANTINE_OK;
}
