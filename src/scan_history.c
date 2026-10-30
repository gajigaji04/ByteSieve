#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "scan_history.h"

#define HISTORY_MAGIC "BYTESIEVE-HISTORY 1"
/* 손상된 파일로 메모리를 다 쓰지 않도록 읽을 크기를 제한한다. */
#define HISTORY_MAX_FILE_BYTES ((size_t)512 * 1024 * 1024)

/* 스냅숏 flags 로 남기는 SCAN_NODE_* */
#define KEPT_NODE_FLAGS                                                                         \
    (SCAN_NODE_REPARSE | SCAN_NODE_DEPTH_LIMIT | SCAN_NODE_FAILED | SCAN_NODE_INCOMPLETE |      \
     SCAN_NODE_CANCELLED)
#define MISSING_NODE_FLAGS (SCAN_NODE_FAILED | SCAN_NODE_INCOMPLETE | SCAN_NODE_CANCELLED)

void history_snapshot_init(HistorySnapshot *s)
{
    memset(s, 0, sizeof(*s));
    s->status = SCAN_COMPLETE;
}

void history_snapshot_free(HistorySnapshot *s)
{
    size_t i;

    if (s == NULL) {
        return;
    }
    for (i = 0; i < s->len; i++) {
        free(s->entries[i].rel);
    }
    free(s->entries);
    free(s->root);
    history_snapshot_init(s);
}

/* rel 의 소유권을 가져간다. 실패하면 rel 을 해제하고 -1. */
static int add_entry(HistorySnapshot *s, wchar_t *rel, uint32_t depth, uint32_t flags,
                     uint64_t bytes, uint64_t alloc, uint64_t files, size_t node)
{
    HistoryEntry *e;

    if (rel == NULL) {
        return -1;
    }
    if (s->len == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 256;
        HistoryEntry *grown = realloc(s->entries, cap * sizeof(*grown));
        if (grown == NULL) {
            free(rel);
            return -1;
        }
        s->entries = grown;
        s->cap = cap;
    }
    e = &s->entries[s->len++];
    e->rel = rel;
    e->depth = depth;
    e->flags = flags;
    e->bytes = bytes;
    e->alloc = alloc;
    e->files = files;
    e->node = node;
    return 0;
}

static int has_control_char(const wchar_t *s)
{
    for (; *s != L'\0'; s++) {
        if (*s < 0x20) {
            return 1;
        }
    }
    return 0;
}

static uint64_t now_filetime(void)
{
    FILETIME ft;

    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

int history_snapshot_from_model(HistorySnapshot *out, const ScanModel *model)
{
    size_t *entry_of = NULL; /* 노드 → 항목 번호(없으면 SCAN_NODE_NONE) */
    unsigned char *missing = NULL; /* 노드나 그 아래를 다 읽지 못했는지 */
    uint32_t depth;
    size_t i;

    history_snapshot_free(out);
    if (model == NULL || model->len == 0 || model->status == SCAN_FATAL) {
        return -1;
    }
    depth = model->max_depth < HISTORY_DEPTH ? model->max_depth : HISTORY_DEPTH;
    entry_of = malloc(model->len * sizeof(*entry_of));
    missing = calloc(model->len, 1);
    out->root = path_dup(model->nodes[0].name);
    if (entry_of == NULL || missing == NULL || out->root == NULL) {
        goto fail;
    }
    out->time = now_filetime();
    out->depth = depth;
    out->status = model->status;

    /* 자식은 부모보다 인덱스가 크므로 뒤에서부터 올리면 하위 트리 전체가 반영된다. */
    for (i = model->len; i-- > 0;) {
        const ScanNode *n = &model->nodes[i];
        if (n->flags & MISSING_NODE_FLAGS) {
            missing[i] = 1;
        }
        if (missing[i] && i > 0) {
            missing[n->parent] = 1;
        }
    }

    for (i = 0; i < model->len; i++) {
        const ScanNode *n = &model->nodes[i];
        wchar_t *rel;
        uint32_t flags = (n->flags & KEPT_NODE_FLAGS) | (missing[i] ? HISTORY_UNCERTAIN : 0u);

        entry_of[i] = SCAN_NODE_NONE;
        if (n->depth > depth) {
            continue;
        }
        if (i == 0) {
            rel = path_dup(L"");
        } else {
            size_t pe = entry_of[n->parent];
            if (pe == SCAN_NODE_NONE || has_control_char(n->name)) {
                continue;
            }
            rel = out->entries[pe].rel[0] == L'\0' ? path_dup(n->name)
                                                   : path_join(out->entries[pe].rel, n->name);
            if (rel == NULL) {
                /* 경로 한도를 넘는 폴더는 기록하지 않는다. */
                continue;
            }
        }
        if (add_entry(out, rel, n->depth, flags, n->total_bytes, n->total_alloc, n->total_files,
                      i) != 0) {
            goto fail;
        }
        entry_of[i] = out->len - 1;
    }
    free(entry_of);
    free(missing);
    return 0;

fail:
    free(entry_of);
    free(missing);
    history_snapshot_free(out);
    return -1;
}

/* ---- 저장·읽기 ---- */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} TextBuf;

static void tb_append(TextBuf *b, const char *s, size_t n)
{
    if (b->failed) {
        return;
    }
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64 * 1024;
        char *grown;
        while (cap < b->len + n) {
            cap *= 2;
        }
        grown = realloc(b->data, cap);
        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void tb_str(TextBuf *b, const char *s)
{
    tb_append(b, s, strlen(s));
}

/* UTF-16 을 UTF-8 로 붙인다. */
static void tb_wide(TextBuf *b, const wchar_t *s)
{
    int n;
    char local[512];
    char *tmp;

    if (*s == L'\0') {
        return;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        b->failed = 1;
        return;
    }
    tmp = n <= (int)sizeof(local) ? local : malloc((size_t)n);
    if (tmp == NULL) {
        b->failed = 1;
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, s, -1, tmp, n, NULL, NULL);
    tb_append(b, tmp, (size_t)n - 1);
    if (tmp != local) {
        free(tmp);
    }
}

int history_save(const HistorySnapshot *s, const wchar_t *file, unsigned long *error)
{
    TextBuf b = {0};
    char line[160];
    wchar_t *tmp_path;
    HANDLE h;
    DWORD written = 0;
    size_t i;
    int ok;

    if (error != NULL) {
        *error = 0;
    }
    if (s == NULL || s->root == NULL || file == NULL) {
        if (error != NULL) {
            *error = ERROR_INVALID_PARAMETER;
        }
        return -1;
    }
    tb_str(&b, HISTORY_MAGIC "\nroot\t");
    tb_wide(&b, s->root);
    snprintf(line, sizeof(line), "\ntime\t%llu\ndepth\t%u\nstatus\t%ls\nentries\t%llu\n",
             (unsigned long long)s->time, s->depth, scan_status_name(s->status),
             (unsigned long long)s->len);
    tb_str(&b, line);
    for (i = 0; i < s->len; i++) {
        const HistoryEntry *e = &s->entries[i];
        snprintf(line, sizeof(line), "%llu\t%llu\t%llu\t%u\t%u\t", (unsigned long long)e->bytes,
                 (unsigned long long)e->alloc, (unsigned long long)e->files, e->flags, e->depth);
        tb_str(&b, line);
        tb_wide(&b, e->rel);
        tb_str(&b, "\n");
    }
    if (b.failed) {
        free(b.data);
        if (error != NULL) {
            *error = ERROR_NOT_ENOUGH_MEMORY;
        }
        return -1;
    }

    tmp_path = malloc((wcslen(file) + 5) * sizeof(wchar_t));
    if (tmp_path == NULL) {
        free(b.data);
        if (error != NULL) {
            *error = ERROR_NOT_ENOUGH_MEMORY;
        }
        return -1;
    }
    swprintf(tmp_path, wcslen(file) + 5, L"%ls.tmp", file);
    h = CreateFileW(tmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = h != INVALID_HANDLE_VALUE;
    if (ok) {
        ok = b.len <= MAXDWORD && WriteFile(h, b.data, (DWORD)b.len, &written, NULL) &&
             written == b.len;
        if (!ok && error != NULL) {
            *error = GetLastError();
        }
        CloseHandle(h);
    } else if (error != NULL) {
        *error = GetLastError();
    }
    if (ok && !MoveFileExW(tmp_path, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error != NULL) {
            *error = GetLastError();
        }
        ok = 0;
    }
    if (!ok) {
        DeleteFileW(tmp_path);
    }
    free(tmp_path);
    free(b.data);
    return ok ? 0 : -1;
}

/* 줄 하나를 잘라 낸다(끝의 '\r' 은 뺀다). 없으면 NULL. */
static wchar_t *next_line(wchar_t **cursor)
{
    wchar_t *start = *cursor, *end;

    if (start == NULL || *start == L'\0') {
        return NULL;
    }
    end = wcschr(start, L'\n');
    if (end != NULL) {
        *end = L'\0';
        *cursor = end + 1;
    } else {
        *cursor = start + wcslen(start);
    }
    end = start + wcslen(start);
    if (end > start && end[-1] == L'\r') {
        end[-1] = L'\0';
    }
    return start;
}

/* "key\tvalue" 줄에서 value 를 돌려준다. key 가 다르면 NULL. */
static const wchar_t *header_value(const wchar_t *line, const wchar_t *key)
{
    size_t n = wcslen(key);

    if (line == NULL || wcsncmp(line, key, n) != 0 || line[n] != L'\t') {
        return NULL;
    }
    return line + n + 1;
}

static int parse_u64(const wchar_t *s, wchar_t **end, uint64_t *out)
{
    if (s == NULL || *s < L'0' || *s > L'9') {
        return 0;
    }
    *out = _wcstoui64(s, end, 10);
    return 1;
}

int history_load(HistorySnapshot *out, const wchar_t *file)
{
    HANDLE h;
    LARGE_INTEGER size;
    char *raw = NULL;
    wchar_t *text = NULL, *cursor, *line;
    const wchar_t *v;
    DWORD got = 0;
    int wlen;
    uint64_t count, num, i;

    history_snapshot_free(out);
    h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        (uint64_t)size.QuadPart > HISTORY_MAX_FILE_BYTES) {
        CloseHandle(h);
        return -1;
    }
    raw = malloc((size_t)size.QuadPart);
    if (raw == NULL || !ReadFile(h, raw, (DWORD)size.QuadPart, &got, NULL) ||
        got != (DWORD)size.QuadPart) {
        CloseHandle(h);
        free(raw);
        return -1;
    }
    CloseHandle(h);
    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, (int)got, NULL, 0);
    if (wlen <= 0 || (text = malloc(((size_t)wlen + 1) * sizeof(wchar_t))) == NULL) {
        free(raw);
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, (int)got, text, wlen);
    text[wlen] = L'\0';
    free(raw);

    cursor = text;
    line = next_line(&cursor);
    if (line == NULL || wcscmp(line, L"" HISTORY_MAGIC) != 0) {
        goto fail;
    }
    if ((v = header_value(next_line(&cursor), L"root")) == NULL || *v == L'\0' ||
        (out->root = path_dup(v)) == NULL) {
        goto fail;
    }
    if (!parse_u64(header_value(next_line(&cursor), L"time"), NULL, &out->time)) {
        goto fail;
    }
    if (!parse_u64(header_value(next_line(&cursor), L"depth"), NULL, &num) || num > 64) {
        goto fail;
    }
    out->depth = (uint32_t)num;
    if ((v = header_value(next_line(&cursor), L"status")) == NULL) {
        goto fail;
    }
    out->status = wcscmp(v, L"complete") == 0  ? SCAN_COMPLETE
                  : wcscmp(v, L"partial") == 0 ? SCAN_PARTIAL
                                               : SCAN_FATAL;
    if (!parse_u64(header_value(next_line(&cursor), L"entries"), NULL, &count)) {
        goto fail;
    }
    for (i = 0; i < count; i++) {
        uint64_t bytes, alloc, files, flags, depth;
        wchar_t *p;
        line = next_line(&cursor);
        if (line == NULL || !parse_u64(line, &p, &bytes) || *p++ != L'\t' ||
            !parse_u64(p, &p, &alloc) || *p++ != L'\t' || !parse_u64(p, &p, &files) ||
            *p++ != L'\t' || !parse_u64(p, &p, &flags) || *p++ != L'\t' ||
            !parse_u64(p, &p, &depth) || *p++ != L'\t' || depth > out->depth) {
            goto fail;
        }
        /* 첫 항목은 루트("")여야 한다. */
        if ((i == 0) != (*p == L'\0')) {
            goto fail;
        }
        if (add_entry(out, path_dup(p), (uint32_t)depth, (uint32_t)flags, bytes, alloc, files,
                      SCAN_NODE_NONE) != 0) {
            goto fail;
        }
    }
    if (out->len == 0) {
        goto fail;
    }
    free(text);
    return 0;

fail:
    free(text);
    history_snapshot_free(out);
    return -1;
}

/* ---- 저장 위치 ---- */

int history_default_dir(wchar_t *buf, size_t len)
{
    wchar_t base[MAX_PATH];
    DWORD n;

    n = GetEnvironmentVariableW(L"BYTESIEVE_HISTORY_DIR", buf, (DWORD)len);
    if (n > 0 && n < len) {
        while (n > 1 && path_is_separator(buf[n - 1]) && buf[n - 2] != L':') {
            buf[--n] = L'\0';
        }
        return 0;
    }
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return -1;
    }
    if (swprintf(buf, len, L"%ls\\ByteSieve\\history", base) < 0) {
        return -1;
    }
    return 0;
}

int history_ensure_dir(const wchar_t *dir)
{
    wchar_t *copy, *p;
    int ok = 1;

    if (dir == NULL || dir[0] == L'\0') {
        return -1;
    }
    copy = path_to_extended(dir);
    if (copy == NULL) {
        return -1;
    }
    /* "\\?\C:\" 또는 "\\?\UNC\server\share\" 다음부터 한 단계씩 만든다. */
    p = copy + 4;
    if (_wcsnicmp(p, L"UNC\\", 4) == 0) {
        p = wcschr(p + 4, L'\\');
        p = p != NULL ? wcschr(p + 1, L'\\') : NULL;
    } else {
        p = wcschr(p, L'\\');
    }
    while (p != NULL && ok) {
        wchar_t *next = wcschr(p + 1, L'\\');
        if (next != NULL) {
            *next = L'\0';
        }
        if (p[1] != L'\0' && !CreateDirectoryW(copy, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            ok = 0;
        }
        if (next != NULL) {
            *next = L'\\';
        }
        p = next;
    }
    if (ok) {
        DWORD attrs = GetFileAttributesW(copy);
        ok = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
    }
    free(copy);
    return ok ? 0 : -1;
}

/* 대소문자를 무시하도록 대문자로 바꾼 복사본(malloc). */
static wchar_t *upper_dup(const wchar_t *s, size_t len)
{
    wchar_t *u = malloc((len + 1) * sizeof(wchar_t));

    if (u == NULL) {
        return NULL;
    }
    memcpy(u, s, len * sizeof(wchar_t));
    u[len] = L'\0';
    if (len > 0) {
        CharUpperBuffW(u, (DWORD)len);
    }
    return u;
}

static uint64_t fnv1a(const wchar_t *s)
{
    uint64_t h = 1469598103934665603ULL;

    for (; *s != L'\0'; s++) {
        h ^= (uint64_t)(unsigned)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

wchar_t *history_file_for(const wchar_t *dir, const wchar_t *root)
{
    wchar_t name[32], *full, *upper, *joined;
    size_t len;

    if (dir == NULL || root == NULL) {
        return NULL;
    }
    full = path_full(root);
    if (full == NULL) {
        full = path_dup(root);
    }
    if (full == NULL) {
        return NULL;
    }
    len = wcslen(full);
    while (len > 0 && path_is_separator(full[len - 1]) && !(len >= 2 && full[len - 2] == L':')) {
        len--;
    }
    if (len > 0 && path_is_separator(full[len - 1])) {
        len--; /* "C:\" 와 "C:" 를 같게 본다 */
    }
    upper = upper_dup(full, len);
    free(full);
    if (upper == NULL) {
        return NULL;
    }
    swprintf(name, 32, L"%016llx.snap", (unsigned long long)fnv1a(upper));
    free(upper);
    joined = path_join(dir, name);
    return joined;
}

/* ---- 비교 ---- */

void history_diff_init(HistoryDiff *d)
{
    memset(d, 0, sizeof(*d));
}

void history_diff_free(HistoryDiff *d)
{
    size_t i;

    if (d == NULL) {
        return;
    }
    for (i = 0; i < d->len; i++) {
        free(d->items[i].rel);
    }
    free(d->items);
    history_diff_init(d);
}

/* 대문자 상대 경로 → 항목 번호 해시표(개방 주소법). */
typedef struct {
    wchar_t **keys;
    size_t *values;
    size_t cap; /* 2 의 거듭제곱 */
} RelMap;

static void map_free(RelMap *m)
{
    size_t i;

    for (i = 0; i < m->cap; i++) {
        free(m->keys[i]);
    }
    free(m->keys);
    free(m->values);
}

/* 스냅숏의 상대 경로(깊이 limit 이하)로 해시표를 만든다. 실패하면 -1. */
static int map_build(RelMap *m, const HistorySnapshot *s, uint32_t limit)
{
    size_t i;

    m->cap = 64;
    while (m->cap < s->len * 2) {
        m->cap *= 2;
    }
    m->keys = calloc(m->cap, sizeof(*m->keys));
    m->values = malloc(m->cap * sizeof(*m->values));
    if (m->keys == NULL || m->values == NULL) {
        free(m->keys);
        free(m->values);
        m->keys = NULL;
        m->values = NULL;
        m->cap = 0;
        return -1;
    }
    for (i = 0; i < s->len; i++) {
        size_t slot;
        wchar_t *key;
        if (s->entries[i].depth > limit) {
            continue;
        }
        key = upper_dup(s->entries[i].rel, wcslen(s->entries[i].rel));
        if (key == NULL) {
            return -1;
        }
        for (slot = (size_t)fnv1a(key) & (m->cap - 1); m->keys[slot] != NULL;
             slot = (slot + 1) & (m->cap - 1)) {
            if (wcscmp(m->keys[slot], key) == 0) {
                break;
            }
        }
        if (m->keys[slot] != NULL) {
            free(key); /* 같은 경로가 두 번 나오면 처음 것을 쓴다 */
            continue;
        }
        m->keys[slot] = key;
        m->values[slot] = i;
    }
    return 0;
}

/* 없으면 SCAN_NODE_NONE. key 는 대문자여야 한다. */
static size_t map_get(const RelMap *m, const wchar_t *key)
{
    size_t slot;

    for (slot = (size_t)fnv1a(key) & (m->cap - 1); m->keys[slot] != NULL;
         slot = (slot + 1) & (m->cap - 1)) {
        if (wcscmp(m->keys[slot], key) == 0) {
            return m->values[slot];
        }
    }
    return SCAN_NODE_NONE;
}

/* rel 의 부모 상대 경로가 m 에 있는지(루트 바로 아래면 루트 ""). */
static int parent_in(const RelMap *m, const wchar_t *rel)
{
    const wchar_t *slash = wcsrchr(rel, L'\\');
    wchar_t *key;
    size_t found;

    key = upper_dup(rel, slash != NULL ? (size_t)(slash - rel) : 0);
    if (key == NULL) {
        return 1; /* 모르면 상위가 있다고 보고 그대로 넣는다 */
    }
    found = map_get(m, key);
    free(key);
    return found != SCAN_NODE_NONE;
}

static int add_change(HistoryDiff *d, const wchar_t *rel, HistoryKind kind, uint32_t depth,
                      uint64_t old_bytes, uint64_t new_bytes, int uncertain, size_t node)
{
    HistoryChange *c;

    if (d->len == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 64;
        HistoryChange *grown = realloc(d->items, cap * sizeof(*grown));
        if (grown == NULL) {
            return -1;
        }
        d->items = grown;
        d->cap = cap;
    }
    c = &d->items[d->len];
    c->rel = path_dup(rel);
    if (c->rel == NULL) {
        return -1;
    }
    c->kind = kind;
    c->depth = depth;
    c->old_bytes = old_bytes;
    c->new_bytes = new_bytes;
    c->delta = (int64_t)(new_bytes - old_bytes);
    c->uncertain = uncertain;
    c->node = node;
    d->len++;
    switch (kind) {
    case HISTORY_GROWN:
        d->grown++;
        break;
    case HISTORY_SHRUNK:
        d->shrunk++;
        break;
    case HISTORY_ADDED:
        d->added++;
        break;
    default:
        d->removed++;
        break;
    }
    return 0;
}

static int cmp_change(const void *a, const void *b)
{
    const HistoryChange *x = a, *y = b;
    uint64_t ax = x->delta < 0 ? (uint64_t)0 - (uint64_t)x->delta : (uint64_t)x->delta;
    uint64_t ay = y->delta < 0 ? (uint64_t)0 - (uint64_t)y->delta : (uint64_t)y->delta;

    if (ax != ay) {
        return ax > ay ? -1 : 1;
    }
    return _wcsicmp(x->rel, y->rel);
}

static int same_root(const wchar_t *a, const wchar_t *b)
{
    size_t la = wcslen(a), lb = wcslen(b);

    while (la > 0 && path_is_separator(a[la - 1])) {
        la--;
    }
    while (lb > 0 && path_is_separator(b[lb - 1])) {
        lb--;
    }
    return la == lb && (la == 0 || CompareStringOrdinal(a, (int)la, b, (int)lb, TRUE) == CSTR_EQUAL);
}

int history_diff(const HistorySnapshot *old, const HistorySnapshot *cur, HistoryDiff *out)
{
    RelMap old_map = {0}, cur_map = {0};
    uint32_t limit;
    size_t i;
    int rc = -1;

    history_diff_free(out);
    if (old == NULL || cur == NULL || old->len == 0 || cur->len == 0 || old->root == NULL ||
        cur->root == NULL || !same_root(old->root, cur->root)) {
        return -1;
    }
    limit = old->depth < cur->depth ? old->depth : cur->depth;
    if (map_build(&old_map, old, limit) != 0 || map_build(&cur_map, cur, limit) != 0) {
        goto done;
    }
    out->old_time = old->time;
    out->old_total = old->entries[0].bytes;
    out->new_total = cur->entries[0].bytes;
    out->total_uncertain =
        ((old->entries[0].flags | cur->entries[0].flags) & HISTORY_UNCERTAIN) != 0;

    /* 지금 있는 폴더: 늘어남·줄어듦·새로 생김 */
    for (i = 1; i < cur->len; i++) {
        const HistoryEntry *e = &cur->entries[i];
        wchar_t *key;
        size_t oi;
        if (e->depth > limit) {
            continue;
        }
        key = upper_dup(e->rel, wcslen(e->rel));
        if (key == NULL) {
            goto done;
        }
        oi = map_get(&old_map, key);
        free(key);
        if (oi == SCAN_NODE_NONE) {
            /* 새 폴더 안의 하위 폴더는 새 폴더 하나로 보여 준다. */
            if (parent_in(&old_map, e->rel) &&
                add_change(out, e->rel, HISTORY_ADDED, e->depth, 0, e->bytes,
                           (e->flags & HISTORY_UNCERTAIN) != 0, e->node) != 0) {
                goto done;
            }
        } else {
            const HistoryEntry *o = &old->entries[oi];
            if (o->bytes != e->bytes &&
                add_change(out, e->rel, e->bytes > o->bytes ? HISTORY_GROWN : HISTORY_SHRUNK,
                           e->depth, o->bytes, e->bytes,
                           ((o->flags | e->flags) & HISTORY_UNCERTAIN) != 0, e->node) != 0) {
                goto done;
            }
        }
    }
    /* 사라진 폴더 */
    for (i = 1; i < old->len; i++) {
        const HistoryEntry *o = &old->entries[i];
        wchar_t *key;
        size_t ci;
        if (o->depth > limit) {
            continue;
        }
        key = upper_dup(o->rel, wcslen(o->rel));
        if (key == NULL) {
            goto done;
        }
        ci = map_get(&cur_map, key);
        free(key);
        if (ci == SCAN_NODE_NONE && parent_in(&cur_map, o->rel) &&
            add_change(out, o->rel, HISTORY_REMOVED, o->depth, o->bytes, 0,
                       (o->flags & HISTORY_UNCERTAIN) != 0, SCAN_NODE_NONE) != 0) {
            goto done;
        }
    }
    qsort(out->items, out->len, sizeof(*out->items), cmp_change);
    rc = 0;

done:
    map_free(&old_map);
    map_free(&cur_map);
    if (rc != 0) {
        history_diff_free(out);
    }
    return rc;
}

wchar_t *history_full_path(const wchar_t *root, const wchar_t *rel)
{
    if (root == NULL || rel == NULL) {
        return NULL;
    }
    return rel[0] == L'\0' ? path_dup(root) : path_join(root, rel);
}

const wchar_t *history_kind_name(HistoryKind kind)
{
    switch (kind) {
    case HISTORY_GROWN:
        return L"grown";
    case HISTORY_SHRUNK:
        return L"shrunk";
    case HISTORY_ADDED:
        return L"added";
    default:
        return L"removed";
    }
}
