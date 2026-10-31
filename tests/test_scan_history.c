#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path.h"
#include "scan_history.h"
#include "scan_model.h"

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

static wchar_t g_root[MAX_PATH];
static wchar_t g_hist[MAX_PATH];

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

static int make_dir(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    BOOL ok = p != NULL && (CreateDirectoryW(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);

    free(p);
    return ok;
}

static int write_file(const wchar_t *name, DWORD len)
{
    static char data[4096];
    wchar_t *p = fixture(name);
    HANDLE h = p ? CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               NULL)
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
                remove_tree(child);
            } else {
                DeleteFileW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void remove_file(const wchar_t *name)
{
    wchar_t *p = fixture(name);

    if (p != NULL) {
        DeleteFileW(p);
    }
    free(p);
}

/* 이름으로 변화를 찾는다. */
static const HistoryChange *find_change(const HistoryDiff *d, const wchar_t *rel)
{
    size_t i;

    for (i = 0; i < d->len; i++) {
        if (_wcsicmp(d->items[i].rel, rel) == 0) {
            return &d->items[i];
        }
    }
    return NULL;
}

static const HistoryEntry *find_entry(const HistorySnapshot *s, const wchar_t *rel)
{
    size_t i;

    for (i = 0; i < s->len; i++) {
        if (_wcsicmp(s->entries[i].rel, rel) == 0) {
            return &s->entries[i];
        }
    }
    return NULL;
}

static int snapshot_now(HistorySnapshot *s)
{
    ScanModel m;
    int rc;

    scan_model_init(&m);
    scan_model_build(&m, g_root, SCAN_DEPTH_UNLIMITED, NULL, NULL);
    rc = history_snapshot_from_model(s, &m);
    scan_model_free(&m);
    return rc;
}

static void test_snapshot_and_roundtrip(void)
{
    HistorySnapshot s, loaded;
    const HistoryEntry *e;
    wchar_t *file;
    unsigned long err = 0;
    size_t i;

    history_snapshot_init(&s);
    history_snapshot_init(&loaded);
    CHECK(snapshot_now(&s) == 0);
    CHECK(s.depth == HISTORY_DEPTH);
    CHECK(s.status == SCAN_COMPLETE);
    CHECK(s.len > 0 && s.entries[0].rel[0] == L'\0' && s.entries[0].depth == 0);
    CHECK(s.root != NULL && _wcsicmp(s.root, g_root) == 0);
    CHECK_U64(s.entries[0].bytes, 100 + 200 + 300 + 100 + 50 + 7);
    e = find_entry(&s, L"proj\\node_modules");
    CHECK(e != NULL && e->bytes == 200 && e->depth == 2 && e->node != SCAN_NODE_NONE);
    e = find_entry(&s, L"한글 폴더");
    CHECK(e != NULL && e->bytes == 7);
    /* 깊이 HISTORY_DEPTH(4) 아래 폴더는 기록하지 않지만 용량은 조상에 들어 있다. */
    CHECK(find_entry(&s, L"deep\\a\\b\\c") != NULL);
    CHECK(find_entry(&s, L"deep\\a\\b\\c\\d") == NULL);
    e = find_entry(&s, L"deep\\a\\b\\c");
    CHECK(e != NULL && e->bytes == 50);

    file = history_file_for(g_hist, g_root);
    CHECK(file != NULL);
    CHECK(history_ensure_dir(g_hist) == 0);
    CHECK(history_save(&s, file, &err) == 0 && err == 0);
    CHECK(history_load(&loaded, file) == 0);
    CHECK(loaded.len == s.len && loaded.depth == s.depth && loaded.time == s.time &&
          loaded.status == s.status && _wcsicmp(loaded.root, s.root) == 0);
    for (i = 0; i < s.len && i < loaded.len; i++) {
        CHECK(wcscmp(loaded.entries[i].rel, s.entries[i].rel) == 0);
        CHECK(loaded.entries[i].bytes == s.entries[i].bytes);
        CHECK(loaded.entries[i].alloc == s.entries[i].alloc);
        CHECK(loaded.entries[i].files == s.entries[i].files);
        CHECK(loaded.entries[i].flags == s.entries[i].flags);
        CHECK(loaded.entries[i].depth == s.entries[i].depth);
        CHECK(loaded.entries[i].node == SCAN_NODE_NONE);
    }
    free(file);
    history_snapshot_free(&s);
    history_snapshot_free(&loaded);
}

static void test_diff(void)
{
    HistorySnapshot old_s, new_s, again;
    HistoryDiff d;
    const HistoryChange *c;
    wchar_t *file;
    size_t i;

    history_snapshot_init(&old_s);
    history_snapshot_init(&new_s);
    history_snapshot_init(&again);
    history_diff_init(&d);
    file = history_file_for(g_hist, g_root);
    CHECK(file != NULL && history_load(&old_s, file) == 0);

    /*
     * 변화: proj\node_modules 200 → 1200(+1000), docs 300 → 0 파일(-300),
     * new\x\y 새 폴더(+40, 하위는 따로 안 나옴), old 폴더 삭제(-100, 하위 old\keep 도 따로 안 나옴),
     * deep\a\b\c\d\e 안의 파일 +5(깊이 4 의 deep\a\b\c 에서 보임)
     */
    CHECK(write_file(L"proj\\node_modules\\big.bin", 1000));
    remove_file(L"docs\\a.txt");
    CHECK(make_dir(L"new") && make_dir(L"new\\x") && make_dir(L"new\\x\\y") &&
          write_file(L"new\\x\\y\\n.bin", 40));
    {
        wchar_t *p = fixture(L"old");
        remove_tree(p);
        free(p);
    }
    CHECK(write_file(L"deep\\a\\b\\c\\d\\e\\more.bin", 5));

    CHECK(snapshot_now(&new_s) == 0);
    CHECK(history_diff(&old_s, &new_s, &d) == 0);
    CHECK_U64(d.old_total, 757);
    CHECK_U64(d.new_total, 757 + 1000 - 300 + 40 - 100 + 5);
    CHECK(!d.total_uncertain);
    CHECK(d.old_time == old_s.time);

    c = find_change(&d, L"proj\\node_modules");
    CHECK(c != NULL && c->kind == HISTORY_GROWN && c->delta == 1000 && c->old_bytes == 200 &&
          c->new_bytes == 1200 && c->node != SCAN_NODE_NONE && !c->uncertain);
    c = find_change(&d, L"proj");
    CHECK(c != NULL && c->kind == HISTORY_GROWN && c->delta == 1000);
    c = find_change(&d, L"docs");
    CHECK(c != NULL && c->kind == HISTORY_SHRUNK && c->delta == -300 && c->new_bytes == 0);
    c = find_change(&d, L"new");
    CHECK(c != NULL && c->kind == HISTORY_ADDED && c->delta == 40 && c->old_bytes == 0);
    CHECK(find_change(&d, L"new\\x") == NULL);
    CHECK(find_change(&d, L"new\\x\\y") == NULL);
    c = find_change(&d, L"old");
    CHECK(c != NULL && c->kind == HISTORY_REMOVED && c->delta == -100 &&
          c->node == SCAN_NODE_NONE);
    CHECK(find_change(&d, L"old\\keep") == NULL);
    c = find_change(&d, L"deep\\a\\b\\c");
    CHECK(c != NULL && c->kind == HISTORY_GROWN && c->delta == 5);
    /* 변화 없는 폴더는 없다 */
    CHECK(find_change(&d, L"한글 폴더") == NULL);
    CHECK(find_change(&d, L"") == NULL);
    CHECK_U64(d.added, 1);
    CHECK_U64(d.removed, 1);
    CHECK(d.grown >= 4); /* proj, proj\node_modules, deep, deep\a, deep\a\b, deep\a\b\c */
    CHECK_U64(d.shrunk, 1);

    /* |delta| 내림차순 */
    for (i = 1; i < d.len; i++) {
        int64_t a = d.items[i - 1].delta, b = d.items[i].delta;
        CHECK((a < 0 ? -a : a) >= (b < 0 ? -b : b));
    }
    CHECK(d.len > 0 && d.items[0].delta == 1000);

    /* 같은 스냅숏끼리는 변화가 없다 */
    CHECK(history_diff(&new_s, &new_s, &d) == 0);
    CHECK_U64(d.len, 0);

    /* 다른 대상 폴더와는 비교하지 않는다 */
    {
        wchar_t *saved = new_s.root;
        new_s.root = L"Z:\\elsewhere";
        CHECK(history_diff(&old_s, &new_s, &d) == -1);
        new_s.root = saved;
    }
    /* 대소문자·끝 구분자가 달라도 같은 대상 */
    {
        wchar_t *saved = new_s.root, alt[MAX_PATH];
        swprintf(alt, MAX_PATH, L"%ls\\", g_root);
        CharUpperBuffW(alt, (DWORD)wcslen(alt));
        new_s.root = alt;
        CHECK(history_diff(&old_s, &new_s, &d) == 0 && d.len > 0);
        new_s.root = saved;
    }

    history_diff_free(&d);
    history_snapshot_free(&old_s);
    history_snapshot_free(&new_s);
    history_snapshot_free(&again);
    free(file);
}

static void test_uncertain_and_depth(void)
{
    HistorySnapshot a, b;
    HistoryDiff d;
    const HistoryChange *c;

    history_snapshot_init(&a);
    history_snapshot_init(&b);
    history_diff_init(&d);
    CHECK(snapshot_now(&a) == 0);
    CHECK(snapshot_now(&b) == 0);
    /* 직전 스냅숏의 proj 아래가 다 읽히지 않았다고 표시하면 변화가 불확실하다. */
    {
        size_t i;
        for (i = 0; i < a.len; i++) {
            if (_wcsicmp(a.entries[i].rel, L"proj") == 0) {
                a.entries[i].flags |= HISTORY_UNCERTAIN;
                a.entries[i].bytes -= 10;
            }
        }
    }
    CHECK(history_diff(&a, &b, &d) == 0);
    c = find_change(&d, L"proj");
    CHECK(c != NULL && c->uncertain && c->delta == 10);

    /* 깊이가 얕은 쪽에 맞춘다: 깊이 1 까지만 비교 */
    a.depth = 1;
    CHECK(history_diff(&a, &b, &d) == 0);
    CHECK(find_change(&d, L"proj\\node_modules") == NULL);
    CHECK(find_change(&d, L"proj") != NULL);

    history_diff_free(&d);
    history_snapshot_free(&a);
    history_snapshot_free(&b);
}

static void test_files_and_errors(void)
{
    HistorySnapshot s;
    wchar_t dir[MAX_PATH], *f1, *f2, *f3, *bad;
    HANDLE h;
    DWORD n;
    ScanModel m;

    history_snapshot_init(&s);
    /* 같은 대상이면 대소문자·끝 구분자와 상관없이 같은 파일 */
    f1 = history_file_for(L"C:\\h", L"C:\\Work\\Proj");
    f2 = history_file_for(L"C:\\h", L"c:\\work\\proj\\");
    f3 = history_file_for(L"C:\\h", L"C:\\Work\\Other");
    CHECK(f1 != NULL && f2 != NULL && f3 != NULL);
    CHECK(f1 != NULL && f2 != NULL && wcscmp(f1, f2) == 0);
    CHECK(f1 != NULL && f3 != NULL && wcscmp(f1, f3) != 0);
    CHECK(f1 != NULL && wcsncmp(f1, L"C:\\h\\", 5) == 0 && wcsstr(f1, L".snap") != NULL);
    free(f1);
    free(f2);
    free(f3);
    f1 = history_file_for(L"C:\\h", L"D:\\");
    f2 = history_file_for(L"C:\\h", L"d:");
    CHECK(f1 != NULL && f2 != NULL && wcscmp(f1, f2) == 0);
    free(f1);
    free(f2);

    /* 기록 폴더: 환경 변수가 우선 */
    SetEnvironmentVariableW(L"BYTESIEVE_HISTORY_DIR", L"C:\\custom\\hist\\");
    CHECK(history_default_dir(dir, MAX_PATH) == 0 && wcscmp(dir, L"C:\\custom\\hist") == 0);
    SetEnvironmentVariableW(L"BYTESIEVE_HISTORY_DIR", NULL);
    CHECK(history_default_dir(dir, MAX_PATH) == 0 &&
          wcsstr(dir, L"\\ByteSieve\\history") != NULL);

    /* 하위까지 만든다 */
    swprintf(dir, MAX_PATH, L"%ls\\nested\\a\\b", g_hist);
    CHECK(history_ensure_dir(dir) == 0);
    CHECK(history_ensure_dir(dir) == 0);
    CHECK(history_ensure_dir(L"") == -1);

    /* 없는 파일, 손상된 파일 */
    bad = path_join(g_hist, L"bad.snap");
    CHECK(history_load(&s, L"C:\\no\\such\\file.snap") == -1);
    h = CreateFileW(bad, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    {
        static const char broken[] =
            "BYTESIEVE-HISTORY 1\nroot\tC:\\x\ntime\t1\ndepth\t4\nstatus\tcomplete\n"
            "entries\t2\n10\t10\t1\t0\t0\t\n5\t5\tbroken\n";
        WriteFile(h, broken, (DWORD)(sizeof(broken) - 1), &n, NULL);
    }
    CloseHandle(h);
    CHECK(history_load(&s, bad) == -1);
    CHECK(s.len == 0 && s.root == NULL);
    h = CreateFileW(bad, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(h, "not a snapshot\n", 15, &n, NULL);
    CloseHandle(h);
    CHECK(history_load(&s, bad) == -1);
    free(bad);

    /* 스캔하지 못한 모델로는 스냅숏을 만들지 않는다 */
    scan_model_init(&m);
    scan_model_build(&m, L"C:\\no-such-dir-bytesieve", SCAN_DEPTH_UNLIMITED, NULL, NULL);
    CHECK(history_snapshot_from_model(&s, &m) == -1);
    scan_model_free(&m);
    CHECK(history_snapshot_from_model(&s, NULL) == -1);

    CHECK(wcscmp(history_kind_name(HISTORY_ADDED), L"added") == 0);
    CHECK(wcscmp(history_kind_name(HISTORY_SHRUNK), L"shrunk") == 0);
    {
        wchar_t *p = history_full_path(L"C:\\r", L"a\\b");
        CHECK(p != NULL && wcscmp(p, L"C:\\r\\a\\b") == 0);
        free(p);
        p = history_full_path(L"C:\\r", L"");
        CHECK(p != NULL && wcscmp(p, L"C:\\r") == 0);
        free(p);
    }
    history_snapshot_free(&s);
}

/* dir 안에 name 파일을 text 내용으로 만든다. */
static int put_file(const wchar_t *dir, const wchar_t *name, const char *text)
{
    wchar_t *p = path_join(dir, name);
    HANDLE h = p ? CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               NULL)
                 : INVALID_HANDLE_VALUE;
    DWORD written = 0;
    BOOL ok;

    free(p);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = WriteFile(h, text, (DWORD)strlen(text), &written, NULL);
    CloseHandle(h);
    return ok && written == strlen(text);
}

static int file_exists(const wchar_t *dir, const wchar_t *name)
{
    wchar_t *p = path_join(dir, name);
    int ok = p != NULL && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;

    free(p);
    return ok;
}

/* 자동 저장 설정: 기본값, 저장·다시 읽기, 손상된 파일. */
static void test_autosave_setting(void)
{
    wchar_t dir[MAX_PATH];
    unsigned long error = 1;

    swprintf(dir, MAX_PATH, L"%ls\\settings\\nested", g_hist);
    /* 폴더도 설정 파일도 없으면 켬 */
    CHECK(history_autosave_enabled(dir) == 1);
    CHECK(history_autosave_enabled(NULL) == 1);

    /* 끄면 폴더를 만들어 저장하고, 다시 읽어도(재실행) 꺼져 있다 */
    CHECK(history_set_autosave(dir, 0, &error) == 0 && error == 0);
    CHECK(history_autosave_enabled(dir) == 0);
    CHECK(history_autosave_enabled(dir) == 0);
    CHECK(!file_exists(dir, HISTORY_SETTINGS_FILE L".tmp"));
    CHECK(history_set_autosave(dir, 1, NULL) == 0);
    CHECK(history_autosave_enabled(dir) == 1);
    CHECK(history_set_autosave(dir, 0, NULL) == 0);

    /* 손상·알 수 없는 형식이면 기본값(켬) */
    CHECK(put_file(dir, HISTORY_SETTINGS_FILE, "garbage\nautosave\t0\n"));
    CHECK(history_autosave_enabled(dir) == 1);
    CHECK(put_file(dir, HISTORY_SETTINGS_FILE, "BYTESIEVE-HISTORY-SETTINGS 1\nautosave\t01\n"));
    CHECK(history_autosave_enabled(dir) == 1);
    CHECK(put_file(dir, HISTORY_SETTINGS_FILE, ""));
    CHECK(history_autosave_enabled(dir) == 1);
    /* CRLF 로 고쳐 써도 읽는다 */
    CHECK(put_file(dir, HISTORY_SETTINGS_FILE,
                   "BYTESIEVE-HISTORY-SETTINGS 1\r\nautosave\t0\r\n") &&
          history_autosave_enabled(dir) == 0);
    CHECK(put_file(dir, HISTORY_SETTINGS_FILE,
                   "BYTESIEVE-HISTORY-SETTINGS 1\nother\tx\nautosave\t0\r\n"));
    CHECK(history_autosave_enabled(dir) == 0);

    /* 설정 파일 자리에 폴더가 있으면 쓰지 못하고 오류를 알린다 */
    swprintf(dir, MAX_PATH, L"%ls\\settings\\blocked", g_hist);
    CHECK(history_ensure_dir(dir) == 0);
    {
        wchar_t *sub = path_join(dir, HISTORY_SETTINGS_FILE);
        CHECK(sub != NULL && CreateDirectoryW(sub, NULL));
        free(sub);
    }
    error = 0;
    CHECK(history_set_autosave(dir, 0, &error) == -1 && error != 0);
    CHECK(history_autosave_enabled(dir) == 1);
}

/* 기록 삭제: 형식에 맞는 스냅숏만 지우고 설정·다른 파일·폴더는 남긴다. */
static void test_clear(void)
{
    wchar_t dir[MAX_PATH], sub[MAX_PATH];
    HistoryClearResult r;
    HistorySnapshot s;
    wchar_t *file;
    HANDLE locked;

    swprintf(dir, MAX_PATH, L"%ls\\clear", g_hist);
    /* 폴더가 없으면 0개, 지울 것도 없다 */
    CHECK(history_count(dir) == 0);
    memset(&r, 0xff, sizeof(r));
    CHECK(history_clear(dir, &r) == 0 && r.removed == 0 && r.failed == 0 && r.error == 0);

    /* 실제 스냅숏 두 개 + 남은 임시 파일 + 지우면 안 되는 것들 */
    CHECK(history_ensure_dir(dir) == 0);
    history_snapshot_init(&s);
    CHECK(snapshot_now(&s) == 0);
    file = history_file_for(dir, g_root);
    CHECK(file != NULL && history_save(&s, file, NULL) == 0);
    free(file);
    file = history_file_for(dir, L"C:\\Other\\Target");
    CHECK(file != NULL && history_save(&s, file, NULL) == 0);
    free(file);
    history_snapshot_free(&s);
    CHECK(put_file(dir, L"0123456789abcdef.snap.tmp", "partial"));
    CHECK(history_set_autosave(dir, 0, NULL) == 0);
    CHECK(put_file(dir, L"notes.snap", "user file"));
    CHECK(put_file(dir, L"0123456789abcdeg.snap", "not hex"));
    CHECK(put_file(dir, L"0123456789abcdef.snapshot", "other ext"));
    CHECK(put_file(dir, L"keep.txt", "keep"));
    swprintf(sub, MAX_PATH, L"%ls\\fedcba9876543210.snap", dir);
    CHECK(CreateDirectoryW(sub, NULL));
    CHECK(history_count(dir) == 2);

    /* 사용 중(공유 삭제 불가)인 스냅숏이 있어도 나머지는 지우고 실패를 센다 */
    CHECK(put_file(dir, L"aaaaaaaaaaaaaaaa.snap", "locked"));
    CHECK(history_count(dir) == 3);
    file = path_join(dir, L"aaaaaaaaaaaaaaaa.snap");
    locked = file ? CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)
                  : INVALID_HANDLE_VALUE;
    free(file);
    CHECK(locked != INVALID_HANDLE_VALUE);
    CHECK(history_clear(dir, &r) == -1);
    CHECK(r.removed == 2 && r.failed == 1 && r.error == ERROR_SHARING_VIOLATION);
    CHECK(history_count(dir) == 1);
    CHECK(!file_exists(dir, L"0123456789abcdef.snap.tmp"));
    CloseHandle(locked);

    /* 다시 지우면 남은 것까지 지운다 */
    CHECK(history_clear(dir, &r) == 0 && r.removed == 1 && r.failed == 0);
    CHECK(history_count(dir) == 0);

    /* 설정과 형식이 다른 파일·폴더는 그대로 */
    CHECK(history_autosave_enabled(dir) == 0);
    CHECK(file_exists(dir, L"notes.snap") && file_exists(dir, L"0123456789abcdeg.snap") &&
          file_exists(dir, L"0123456789abcdef.snapshot") && file_exists(dir, L"keep.txt") &&
          file_exists(dir, L"fedcba9876543210.snap"));
    CHECK(history_clear(NULL, &r) == -1 && r.error != 0);
}

static int setup(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_hist_%lu", tmp, (unsigned long)GetCurrentProcessId());
    swprintf(g_hist, MAX_PATH, L"%lsbytesieve_histdir_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    remove_tree(g_hist);
    if (!CreateDirectoryW(g_root, NULL)) {
        return 0;
    }
    /*
     * root\
     *   proj\package.json 100, proj\node_modules\m.js 200
     *   docs\a.txt 300
     *   old\keep\k.bin 100 (나중에 삭제)
     *   deep\a\b\c\d\e\f.bin 50 (깊이 4 아래)
     *   한글 폴더\h.txt 7
     */
    return make_dir(L"proj") && write_file(L"proj\\package.json", 100) &&
           make_dir(L"proj\\node_modules") && write_file(L"proj\\node_modules\\m.js", 200) &&
           make_dir(L"docs") && write_file(L"docs\\a.txt", 300) && make_dir(L"old") &&
           make_dir(L"old\\keep") && write_file(L"old\\keep\\k.bin", 100) && make_dir(L"deep") &&
           make_dir(L"deep\\a") && make_dir(L"deep\\a\\b") && make_dir(L"deep\\a\\b\\c") &&
           make_dir(L"deep\\a\\b\\c\\d") && make_dir(L"deep\\a\\b\\c\\d\\e") &&
           write_file(L"deep\\a\\b\\c\\d\\e\\f.bin", 50) && make_dir(L"한글 폴더") &&
           write_file(L"한글 폴더\\h.txt", 7);
}

int main(void)
{
    if (!setup()) {
        fprintf(stderr, "fixture setup failed\n");
        return 1;
    }
    test_snapshot_and_roundtrip();
    test_diff();
    test_uncertain_and_depth();
    test_files_and_errors();
    test_autosave_setting();
    test_clear();
    remove_tree(g_root);
    remove_tree(g_hist);

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all scan_history tests passed\n");
    return 0;
}
