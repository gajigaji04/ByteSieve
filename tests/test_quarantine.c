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
#include "quarantine.h"
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

#define CHECK_Q(actual, expected)                                               \
    do {                                                                        \
        QuarantineStatus a_ = (actual), e_ = (expected);                        \
        if (a_ != e_) {                                                         \
            fprintf(stderr, "%s:%d: %s: expected %ls, got %ls\n", __FILE__,     \
                    __LINE__, #actual, quarantine_status_name(e_),              \
                    quarantine_status_name(a_));                                \
            g_failures++;                                                       \
        }                                                                       \
    } while (0)

/* 가짜 볼륨 루트(임시 폴더). 같은 볼륨이므로 이름 바꾸기로 옮길 수 있다. */
static wchar_t g_root[MAX_PATH];

/* ---- 픽스처 ---- */

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

static int write_text(const wchar_t *name, const char *text)
{
    wchar_t *p = fixture(name);
    HANDLE h = p ? CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               NULL)
                 : INVALID_HANDLE_VALUE;
    DWORD len = (DWORD)strlen(text), written = 0;
    BOOL ok;

    free(p);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = WriteFile(h, text, len, &written, NULL);
    CloseHandle(h);
    return ok && written == len;
}

/* 파일 내용이 text 와 같은지(절대 경로). */
static int has_text(const wchar_t *path, const char *text)
{
    char buf[256];
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    BOOL ok;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = ReadFile(h, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(h);
    buf[ok ? got : 0] = '\0';
    return ok && strcmp(buf, text) == 0;
}

static int fixture_has_text(const wchar_t *name, const char *text)
{
    wchar_t *p = fixture(name);
    int ok = p != NULL && has_text(p, text);
    free(p);
    return ok;
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

static int path_exists(const wchar_t *path)
{
    return path != NULL && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static int exists(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    int ok = path_exists(p);
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
            SetFileAttributesW(child, fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
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

/* 각 시험은 빈 가짜 볼륨에서 시작한다. */
static int reset_root(void)
{
    remove_tree(g_root);
    return CreateDirectoryW(g_root, NULL) != 0;
}

static QuarantineStatus move_rel(const wchar_t *rel, int is_dir, uint64_t bytes,
                                 wchar_t **moved, uint32_t *code)
{
    wchar_t *p = fixture(rel);
    QuarantineStatus st = quarantine_move(g_root, p, is_dir, bytes, moved, code);
    free(p);
    return st;
}

static size_t list_root(QuarantineList *list)
{
    quarantine_list_free(list);
    CHECK_Q(quarantine_list_volume(g_root, list), QUARANTINE_OK);
    return list->len;
}

/* 원래 경로(픽스처 상대)로 목록 항목을 찾는다. */
static const QuarantineEntry *find_entry(const QuarantineList *list, const wchar_t *rel)
{
    wchar_t *want = fixture(rel);
    const QuarantineEntry *found = NULL;
    size_t i;

    for (i = 0; want != NULL && i < list->len; i++) {
        if (list->items[i].original != NULL && _wcsicmp(list->items[i].original, want) == 0) {
            found = &list->items[i];
        }
    }
    free(want);
    return found;
}

/* 기록 파일을 직접 쓴다(손상·불일치 재현용). entry 는 항목 폴더 절대 경로. */
static int write_meta_raw(const wchar_t *entry, const char *text)
{
    wchar_t *p = path_join(entry, L"entry.txt");
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
    return ok;
}

/* ---- 상태 이름 ---- */

static void test_status(void)
{
    CHECK(quarantine_status_from_error(0) == QUARANTINE_OK);
    CHECK(quarantine_status_from_error(ERROR_FILE_NOT_FOUND) == QUARANTINE_MISSING);
    CHECK(quarantine_status_from_error(ERROR_ALREADY_EXISTS) == QUARANTINE_CONFLICT);
    CHECK(quarantine_status_from_error(ERROR_ACCESS_DENIED) == QUARANTINE_ACCESS_DENIED);
    CHECK(quarantine_status_from_error(ERROR_WRITE_PROTECT) == QUARANTINE_ACCESS_DENIED);
    CHECK(quarantine_status_from_error(ERROR_SHARING_VIOLATION) == QUARANTINE_IN_USE);
    CHECK(quarantine_status_from_error(ERROR_DISK_FULL) == QUARANTINE_NO_SPACE);
    CHECK(quarantine_status_from_error(ERROR_HANDLE_DISK_FULL) == QUARANTINE_NO_SPACE);
    CHECK(quarantine_status_from_error(ERROR_NOT_SAME_DEVICE) == QUARANTINE_OTHER_VOLUME);
    CHECK(quarantine_status_from_error(ERROR_GEN_FAILURE) == QUARANTINE_FAILED);
    CHECK(wcscmp(quarantine_status_name(QUARANTINE_CONFLICT), L"conflict") == 0);
    CHECK(quarantine_status_text(QUARANTINE_NO_SPACE)[0] != L'\0');
    CHECK(wcscmp(cleanup_trash_status_name(CLEANUP_TRASH_QUARANTINED), L"quarantined") == 0);
    CHECK(wcscmp(cleanup_trash_status_name(CLEANUP_TRASH_QUARANTINE_FAILED),
                 L"quarantine_failed") == 0);
    CHECK(!cleanup_trash_is_denied(CLEANUP_TRASH_QUARANTINED));
    CHECK(!cleanup_trash_is_denied(CLEANUP_TRASH_QUARANTINE_FAILED));
}

/* 격리 폴더 이름 판정과 보호 판정. */
static void test_names_and_guard(void)
{
    CleanupGuard g;
    CleanupItem item;

    CHECK(cleanup_is_quarantine_name(L".bytesieve-trash"));
    CHECK(cleanup_is_quarantine_name(L".ByteSieve-Trash"));
    CHECK(!cleanup_is_quarantine_name(L".bytesieve-trash2"));
    CHECK(cleanup_in_quarantine(L"D:\\.bytesieve-trash"));
    CHECK(cleanup_in_quarantine(L"D:\\.bytesieve-trash\\x\\f\\node_modules"));
    CHECK(cleanup_in_quarantine(L"\\\\?\\D:\\.bytesieve-trash\\"));
    CHECK(!cleanup_in_quarantine(L"D:\\proj\\.bytesieve-trash-old\\a"));
    CHECK(!cleanup_in_quarantine(L"D:\\proj\\node_modules"));

    /* 격리 폴더 안의 항목은 다시 정리할 수 없다. */
    cleanup_guard_init(&g);
    memset(&item, 0, sizeof(item));
    item.path = L"D:\\.bytesieve-trash\\20260101-000000-1\\f\\node_modules";
    item.flags = CLEANUP_ITEM_DIR;
    CHECK(cleanup_guard_check(&g, L"D:\\", &item) == CLEANUP_TRASH_DENY_PROTECTED);
    item.path = L"D:\\proj\\node_modules";
    CHECK(cleanup_guard_check(&g, L"D:\\", &item) == CLEANUP_TRASH_PENDING);
    cleanup_guard_free(&g);
}

/* ---- 격리 이동 ---- */

/* 파일·폴더를 옮기면 원래 자리에서 사라지고 기록과 함께 목록에 보인다. */
static void test_move_and_list(void)
{
    QuarantineList list;
    const QuarantineEntry *e;
    wchar_t *moved = NULL, *store;
    uint32_t code = 1;
    DWORD attrs;

    quarantine_list_init(&list);
    CHECK(reset_root());
    CHECK(make_dir(L"app") && make_dir(L"app\\node_modules") &&
          write_text(L"app\\node_modules\\i.js", "module") && write_text(L"app\\debug.log", "log"));

    /* 격리 폴더가 아직 없으면 빈 목록 */
    CHECK(list_root(&list) == 0);

    CHECK_Q(move_rel(L"app\\node_modules", 1, 6, &moved, &code), QUARANTINE_OK);
    CHECK(code == 0 && moved != NULL);
    CHECK(!exists(L"app\\node_modules"));
    CHECK(moved != NULL && cleanup_in_quarantine(moved));
    if (moved != NULL) {
        wchar_t *inner = path_join(moved, L"i.js");
        CHECK(inner != NULL && has_text(inner, "module"));
        free(inner);
    }
    free(moved);
    moved = NULL;
    CHECK_Q(move_rel(L"app\\debug.log", 0, 3, &moved, NULL), QUARANTINE_OK);
    CHECK(!exists(L"app\\debug.log") && moved != NULL && has_text(moved, "log"));
    free(moved);

    /* 격리 폴더는 숨김이고 볼륨 루트 바로 아래에 있다. */
    store = quarantine_store_path(g_root);
    attrs = store != NULL ? GetFileAttributesW(store) : INVALID_FILE_ATTRIBUTES;
    CHECK(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN) &&
          (attrs & FILE_ATTRIBUTE_DIRECTORY));

    CHECK(list_root(&list) == 2);
    e = find_entry(&list, L"app\\node_modules");
    CHECK(e != NULL);
    if (e != NULL) {
        wchar_t *restore = fixture(L"app\\node_modules");
        CHECK(e->state == QUARANTINE_ENTRY_OK && e->is_dir && e->bytes == 6 && e->time != 0);
        CHECK(restore != NULL && _wcsicmp(e->restore, restore) == 0);
        CHECK(_wcsicmp(e->store, store) == 0 && path_exists(e->item) && path_exists(e->entry));
        free(restore);
    }
    e = find_entry(&list, L"app\\debug.log");
    CHECK(e != NULL && e->state == QUARANTINE_ENTRY_OK && !e->is_dir && e->bytes == 3);

    free(store);
    quarantine_list_free(&list);
}

/* 같은 이름을 여러 번 격리해도 서로 다른 항목 폴더에 들어간다. */
static void test_duplicate_names(void)
{
    QuarantineList list;
    wchar_t *a = NULL, *b = NULL;

    quarantine_list_init(&list);
    CHECK(reset_root());
    CHECK(make_dir(L"a") && make_dir(L"b") && write_text(L"a\\x.log", "A") &&
          write_text(L"b\\x.log", "B"));
    CHECK_Q(move_rel(L"a\\x.log", 0, 1, &a, NULL), QUARANTINE_OK);
    CHECK_Q(move_rel(L"b\\x.log", 0, 1, &b, NULL), QUARANTINE_OK);
    CHECK(a != NULL && b != NULL && _wcsicmp(a, b) != 0);
    CHECK(a != NULL && has_text(a, "A") && b != NULL && has_text(b, "B"));
    CHECK(list_root(&list) == 2);

    /* 둘 다 각자 원래 자리로 복원된다. */
    CHECK_Q(quarantine_restore(find_entry(&list, L"a\\x.log"), NULL, NULL), QUARANTINE_OK);
    CHECK_Q(quarantine_restore(find_entry(&list, L"b\\x.log"), NULL, NULL), QUARANTINE_OK);
    CHECK(fixture_has_text(L"a\\x.log", "A") && fixture_has_text(L"b\\x.log", "B"));
    CHECK(list_root(&list) == 0);

    free(a);
    free(b);
    quarantine_list_free(&list);
}

/* 옮기지 못하면 원래 항목은 그대로이고 격리 폴더에 빈 항목이 남지 않는다. */
static void test_move_failures(void)
{
    QuarantineList list;
    wchar_t *moved = NULL, *locked, *other = NULL;
    uint32_t code = 0;
    HANDLE h;

    quarantine_list_init(&list);
    CHECK(reset_root());
    CHECK(make_dir(L"app") && write_text(L"app\\busy.log", "busy"));

    /* 없는 항목 */
    CHECK_Q(move_rel(L"app\\none.log", 0, 0, &moved, &code), QUARANTINE_MISSING);
    CHECK(moved == NULL && code == ERROR_FILE_NOT_FOUND);

    /* 다른 프로그램이 지우기 공유 없이 연 파일: 이름을 바꿀 수 없다 */
    locked = fixture(L"app\\busy.log");
    h = CreateFileW(locked, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    {
        QuarantineStatus st = quarantine_move(g_root, locked, 0, 4, &moved, &code);
        CHECK(st == QUARANTINE_IN_USE || st == QUARANTINE_ACCESS_DENIED);
        CHECK(moved == NULL && code != 0);
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
    }
    CHECK(fixture_has_text(L"app\\busy.log", "busy"));
    /* 실패한 시도의 기록·빈 폴더는 지워졌다 */
    CHECK(list_root(&list) == 0);

    /* 볼륨 루트 밖의 경로와 격리 폴더 안은 거부한다 */
    CHECK_Q(quarantine_move(L"Z:\\nowhere", locked, 0, 4, &moved, &code),
            QUARANTINE_OTHER_VOLUME);
    other = fixture(L".bytesieve-trash\\x");
    CHECK(quarantine_move(g_root, other, 0, 0, &moved, &code) != QUARANTINE_OK);
    CHECK(fixture_has_text(L"app\\busy.log", "busy"));

    free(other);
    free(locked);
    quarantine_list_free(&list);
}

/* 격리 폴더가 링크(정션)로 바뀌어 있으면 따라가지 않고 거부한다. */
static void test_store_is_link(void)
{
    QuarantineList list;
    wchar_t *moved = NULL;
    uint32_t code = 0;

    quarantine_list_init(&list);
    CHECK(reset_root());
    CHECK(make_dir(L"elsewhere") && make_junction(L".bytesieve-trash", L"elsewhere"));
    CHECK(write_text(L"a.log", "a"));
    CHECK_Q(move_rel(L"a.log", 0, 1, &moved, &code), QUARANTINE_BAD_STORE);
    CHECK(moved == NULL && fixture_has_text(L"a.log", "a"));
    CHECK_Q(quarantine_list_volume(g_root, &list), QUARANTINE_BAD_STORE);
    CHECK(list.len == 0);
    quarantine_list_free(&list);
}

/* ---- 복원 ---- */

static void test_restore(void)
{
    QuarantineList list;
    const QuarantineEntry *e;
    wchar_t *alt = NULL, *orig;
    uint32_t code = 0;

    quarantine_list_init(&list);
    CHECK(reset_root());
    CHECK(make_dir(L"app") && write_text(L"app\\x.log", "old") && make_dir(L"gone") &&
          make_dir(L"gone\\deep") && write_text(L"gone\\deep\\y.tmp", "y") &&
          make_dir(L"app\\build") && write_text(L"app\\build\\o.bin", "obj"));
    CHECK_Q(move_rel(L"app\\x.log", 0, 3, NULL, NULL), QUARANTINE_OK);
    CHECK_Q(move_rel(L"gone\\deep\\y.tmp", 0, 1, NULL, NULL), QUARANTINE_OK);
    CHECK_Q(move_rel(L"app\\build", 1, 3, NULL, NULL), QUARANTINE_OK);
    CHECK(list_root(&list) == 3);

    /* 원래 자리에 같은 이름이 생겼으면 덮어쓰지 않는다 */
    CHECK(write_text(L"app\\x.log", "new"));
    e = find_entry(&list, L"app\\x.log");
    CHECK(e != NULL);
    CHECK_Q(quarantine_restore(e, NULL, &code), QUARANTINE_CONFLICT);
    CHECK(code == ERROR_ALREADY_EXISTS);
    CHECK(fixture_has_text(L"app\\x.log", "new"));
    CHECK(e != NULL && path_exists(e->item) && has_text(e->item, "old"));

    /* 다른 이름으로 복원: "x (복원됨).log" */
    orig = fixture(L"app\\x.log");
    alt = quarantine_alternate_path(orig, 0);
    CHECK(alt != NULL && wcsstr(alt, L"x (복원됨).log") != NULL);
    CHECK_Q(quarantine_restore(e, alt, NULL), QUARANTINE_OK);
    CHECK(alt != NULL && has_text(alt, "old") && fixture_has_text(L"app\\x.log", "new"));
    /* 이미 있으면 다음 번호 */
    free(alt);
    alt = quarantine_alternate_path(orig, 0);
    CHECK(alt != NULL && wcsstr(alt, L"x (복원됨 2).log") != NULL);
    free(alt);
    alt = fixture(L"app\\build");
    {
        wchar_t *dir_alt = quarantine_alternate_path(alt, 1);
        CHECK(dir_alt != NULL && wcsstr(dir_alt, L"build (복원됨)") != NULL);
        free(dir_alt);
    }
    free(alt);
    free(orig);

    /* 상위 폴더가 사라졌으면 만들어서 복원한다 */
    {
        wchar_t *gone = fixture(L"gone");
        remove_tree(gone);
        free(gone);
    }
    CHECK(!exists(L"gone"));
    CHECK_Q(quarantine_restore(find_entry(&list, L"gone\\deep\\y.tmp"), NULL, NULL),
            QUARANTINE_OK);
    CHECK(fixture_has_text(L"gone\\deep\\y.tmp", "y"));

    /* 폴더 복원 */
    CHECK_Q(quarantine_restore(find_entry(&list, L"app\\build"), NULL, NULL), QUARANTINE_OK);
    CHECK(fixture_has_text(L"app\\build\\o.bin", "obj"));

    /* 복원한 항목은 목록과 격리 폴더에서 빠진다(빈 항목 폴더도 지운다) */
    CHECK(list_root(&list) == 0);
    quarantine_list_free(&list);
}

/* 기록의 원래 경로가 다른 드라이브 문자여도 지금 볼륨 기준(relative)으로 복원한다. */
static void test_restore_after_letter_change(void)
{
    QuarantineList list;
    wchar_t *entry;

    quarantine_list_init(&list);
    CHECK(reset_root());
    CHECK(make_dir(L".bytesieve-trash") && make_dir(L".bytesieve-trash\\e1") &&
          make_dir(L".bytesieve-trash\\e1\\f") && write_text(L".bytesieve-trash\\e1\\f\\a.log", "a"));
    entry = fixture(L".bytesieve-trash\\e1");
    CHECK(entry != NULL &&
          write_meta_raw(entry, "ByteSieve quarantine 1\noriginal=Z:\\old\\a.log\n"
                                "relative=old\\a.log\ntime=5\nkind=file\nbytes=1\nstate=moved\n"));
    CHECK(list_root(&list) == 1);
    if (list.len == 1) {
        wchar_t *want = fixture(L"old\\a.log");
        CHECK(list.items[0].state == QUARANTINE_ENTRY_OK);
        CHECK(wcscmp(list.items[0].original, L"Z:\\old\\a.log") == 0);
        CHECK(want != NULL && _wcsicmp(list.items[0].restore, want) == 0);
        CHECK_Q(quarantine_restore(&list.items[0], NULL, NULL), QUARANTINE_OK);
        CHECK(fixture_has_text(L"old\\a.log", "a"));
        free(want);
    }
    free(entry);
    quarantine_list_free(&list);
}

/* ---- 기록과 실제 상태가 어긋난 경우 ---- */

static void test_inconsistent(void)
{
    QuarantineList list;
    const QuarantineEntry *e;
    wchar_t *moved = NULL, *entry_copy = NULL, *orphan_item = NULL;
    size_t i, orphans = 0, missing = 0, ok = 0;

    quarantine_list_init(&list);
    CHECK(reset_root());
    CHECK(make_dir(L"p") && write_text(L"p\\gone.log", "g") && write_text(L"p\\keep.log", "k") &&
          write_text(L"p\\moving.log", "m"));
    CHECK_Q(move_rel(L"p\\gone.log", 0, 1, &moved, NULL), QUARANTINE_OK);
    /* 1) 격리된 파일만 사라짐 → MISSING(기록만) */
    CHECK(moved != NULL && DeleteFileW(moved));
    free(moved);
    moved = NULL;

    /* 2) 기록이 손상됨 → ORPHAN, 파일은 그대로 */
    CHECK_Q(move_rel(L"p\\keep.log", 0, 1, &moved, NULL), QUARANTINE_OK);
    CHECK(list_root(&list) == 2);
    e = find_entry(&list, L"p\\keep.log");
    CHECK(e != NULL && write_meta_raw(e->entry, "garbage\n"));

    /* 3) 기록은 "moving" 인데 파일은 옮겨져 있음(기록을 고치기 전에 멈춤) → 정상 */
    {
        wchar_t *m2 = NULL;
        CHECK_Q(move_rel(L"p\\moving.log", 0, 1, &m2, NULL), QUARANTINE_OK);
        CHECK(list_root(&list) == 3);
        e = find_entry(&list, L"p\\moving.log");
        if (e != NULL) {
            char text[1024];
            char orig[MAX_PATH * 3];
            WideCharToMultiByte(CP_UTF8, 0, e->original, -1, orig, sizeof(orig), NULL, NULL);
            snprintf(text, sizeof(text),
                     "ByteSieve quarantine 1\noriginal=%s\nrelative=p\\moving.log\ntime=9\n"
                     "kind=file\nbytes=1\nstate=moving\n",
                     orig);
            CHECK(write_meta_raw(e->entry, text));
        }
        free(m2);
    }

    /* 4) 격리 폴더에 사용자가 직접 넣은 파일 → ORPHAN */
    CHECK(write_text(L".bytesieve-trash\\stray.txt", "s"));
    /* 5) 기록의 상대 경로가 볼륨 밖을 가리킴 → 믿지 않고 ORPHAN */
    CHECK(make_dir(L".bytesieve-trash\\evil") && make_dir(L".bytesieve-trash\\evil\\f") &&
          write_text(L".bytesieve-trash\\evil\\f\\x", "x"));
    entry_copy = fixture(L".bytesieve-trash\\evil");
    CHECK(entry_copy != NULL &&
          write_meta_raw(entry_copy, "ByteSieve quarantine 1\noriginal=C:\\x\nrelative=..\\..\\x\n"
                                     "time=1\nkind=file\nbytes=1\nstate=moved\n"));

    CHECK(list_root(&list) == 5);
    for (i = 0; i < list.len; i++) {
        const QuarantineEntry *x = &list.items[i];
        if (x->state == QUARANTINE_ENTRY_ORPHAN) {
            orphans++;
            CHECK(x->original == NULL && x->restore == NULL && path_exists(x->item));
            /* 고아 항목은 원래 위치를 몰라 복원할 수 없다 */
            CHECK(quarantine_restore(x, NULL, NULL) != QUARANTINE_OK);
            CHECK(path_exists(x->item));
            if (wcsstr(x->entry, L"stray.txt") != NULL) {
                orphan_item = path_dup(x->item);
            }
        } else if (x->state == QUARANTINE_ENTRY_MISSING) {
            missing++;
            CHECK(x->item == NULL && path_exists(x->entry));
        } else {
            ok++;
        }
    }
    CHECK(orphans == 3 && missing == 1 && ok == 1);
    /* 목록을 읽는 것만으로는 아무것도 지우지 않는다 */
    CHECK(moved != NULL && has_text(moved, "k"));
    CHECK(exists(L".bytesieve-trash\\stray.txt") && exists(L".bytesieve-trash\\evil\\f\\x"));

    /* MISSING 영구 삭제는 기록만 지운다 */
    for (i = 0; i < list.len; i++) {
        if (list.items[i].state == QUARANTINE_ENTRY_MISSING) {
            CHECK_Q(quarantine_purge(&list.items[i], NULL), QUARANTINE_OK);
            CHECK(!path_exists(list.items[i].entry));
        }
    }
    /* 사용자가 고른 고아 항목만 지워진다 */
    for (i = 0; i < list.len; i++) {
        if (list.items[i].state == QUARANTINE_ENTRY_ORPHAN &&
            wcsstr(list.items[i].entry, L"stray.txt") != NULL) {
            CHECK_Q(quarantine_purge(&list.items[i], NULL), QUARANTINE_OK);
        }
    }
    CHECK(orphan_item != NULL && !path_exists(orphan_item));
    CHECK(moved != NULL && has_text(moved, "k"));
    CHECK(exists(L".bytesieve-trash\\evil\\f\\x"));
    CHECK(list_root(&list) == 3);

    free(orphan_item);
    free(entry_copy);
    free(moved);
    quarantine_list_free(&list);
}

/* ---- 영구 삭제 ---- */

static void test_purge(void)
{
    QuarantineList list;
    QuarantineEntry forged;
    const QuarantineEntry *e;
    wchar_t *ro;

    quarantine_list_init(&list);
    CHECK(reset_root());
    /* 격리할 폴더 안: 읽기 전용 파일, 깊은 폴더, 바깥을 가리키는 정션 */
    CHECK(make_dir(L"outside") && write_text(L"outside\\precious.txt", "keep") &&
          make_dir(L"proj") && make_dir(L"proj\\node_modules") &&
          make_dir(L"proj\\node_modules\\a") && make_dir(L"proj\\node_modules\\a\\b") &&
          write_text(L"proj\\node_modules\\a\\b\\c.js", "c") &&
          write_text(L"proj\\node_modules\\ro.js", "r") &&
          make_junction(L"proj\\node_modules\\link", L"outside"));
    ro = fixture(L"proj\\node_modules\\ro.js");
    CHECK(ro != NULL && SetFileAttributesW(ro, FILE_ATTRIBUTE_READONLY));
    free(ro);
    CHECK_Q(move_rel(L"proj\\node_modules", 1, 2, NULL, NULL), QUARANTINE_OK);
    CHECK(list_root(&list) == 1);

    /* 격리 폴더 밖을 가리키게 꾸민 항목은 거부하고 아무것도 지우지 않는다 */
    e = &list.items[0];
    forged = *e;
    forged.entry = fixture(L"outside");
    forged.item = forged.entry;
    CHECK_Q(quarantine_purge(&forged, NULL), QUARANTINE_BAD_STORE);
    forged.state = QUARANTINE_ENTRY_ORPHAN;
    CHECK_Q(quarantine_purge(&forged, NULL), QUARANTINE_BAD_STORE);
    CHECK(fixture_has_text(L"outside\\precious.txt", "keep"));
    free(forged.entry);

    CHECK_Q(quarantine_purge(e, NULL), QUARANTINE_OK);
    CHECK(!path_exists(e->item) && !path_exists(e->entry));
    /* 정션은 링크만 지우고 대상은 그대로 */
    CHECK(fixture_has_text(L"outside\\precious.txt", "keep"));
    CHECK(list_root(&list) == 0);
    quarantine_list_free(&list);
}

/* ---- 정리(cleanup_trash)·탐지·폴더 스캔과의 연동 ---- */

static void test_cleanup_integration(void)
{
    CleanupGuard g;
    CleanupResult r;
    CleanupTrashOutcome o;
    CleanupItem item;
    ScanModel model;
    QuarantineList list;
    wchar_t *app, *log;
    size_t i;

    quarantine_list_init(&list);
    memset(&o, 0, sizeof(o));
    CHECK(reset_root());
    CHECK(make_dir(L"app") && write_text(L"app\\package.json", "{}") &&
          make_dir(L"app\\node_modules") && write_text(L"app\\node_modules\\i.js", "1234") &&
          write_text(L"app\\server.log", "log"));
    /* 테스트용 루트 안은 휴지통이 없는 드라이브로 본다 */
    CHECK(SetEnvironmentVariableW(L"BYTESIEVE_QUARANTINE_ROOT", g_root));

    cleanup_guard_init(&g);
    cleanup_guard_add_system(&g);
    log = fixture(L"app\\server.log");
    memset(&item, 0, sizeof(item));
    item.path = log;
    item.category = CLEANUP_LOG;
    item.bytes = 3;
    CHECK(cleanup_trash_one(&g, g_root, &item, &o) == CLEANUP_TRASH_QUARANTINED);
    CHECK(o.code == 0 && o.recycled_path != NULL && cleanup_in_quarantine(o.recycled_path));
    CHECK(o.recycled_path != NULL && has_text(o.recycled_path, "log"));
    CHECK(!exists(L"app\\server.log"));
    /* 이미 없으면 MISSING */
    CHECK(cleanup_trash_one(&g, g_root, &item, &o) == CLEANUP_TRASH_MISSING);
    cleanup_trash_outcome_free(&o);
    free(log);

    /* 작업 스레드: 진행률에 격리 수가 따로 잡힌다 */
    app = fixture(L"app");
    cleanup_result_init(&r);
    CHECK(cleanup_scan(&r, app, NULL, NULL, NULL, NULL) == SCAN_COMPLETE);
    for (i = 0; i < r.len; i++) {
        if (cleanup_in_quarantine(r.items[i].path)) {
            CHECK(!"detected an item inside the quarantine folder");
        }
    }
    {
        const CleanupItem *items[1];
        CleanupTrashJob *job;
        CleanupTrashSnapshot snap;
        CleanupTrashOutcome *out = NULL;
        size_t n;

        memset(&item, 0, sizeof(item));
        item.path = fixture(L"app\\node_modules");
        item.flags = CLEANUP_ITEM_DIR;
        item.category = CLEANUP_NODE_MODULES;
        item.bytes = 4;
        items[0] = &item;
        job = cleanup_trash_job_start(g_root, items, 1, 0, NULL, NULL);
        CHECK(job != NULL);
        if (job != NULL) {
            do {
                cleanup_trash_job_progress(job, &snap);
                Sleep(5);
            } while (snap.done < snap.total);
            CHECK(snap.moved == 1 && snap.quarantined == 1 && snap.failed == 0 &&
                  snap.moved_bytes == 4);
            n = cleanup_trash_job_finish(job, &out);
            CHECK(n == 1 && out != NULL && out[0].status == CLEANUP_TRASH_QUARANTINED);
            cleanup_trash_outcomes_free(out, n);
        }
        free(item.path);
    }
    CHECK(!exists(L"app\\node_modules") && exists(L"app\\package.json"));
    CHECK(list_root(&list) == 2);

    /* 탐지·폴더 스캔은 격리 폴더에 들어가지 않는다(루트에서 시작해도) */
    cleanup_result_free(&r);
    CHECK(cleanup_scan(&r, g_root, NULL, NULL, NULL, NULL) == SCAN_COMPLETE);
    CHECK(r.report.excluded_dir_count >= 1);
    for (i = 0; i < r.len; i++) {
        CHECK(!cleanup_in_quarantine(r.items[i].path));
    }
    scan_model_init(&model);
    CHECK(scan_model_build(&model, g_root, SCAN_DEPTH_UNLIMITED, NULL, NULL) == SCAN_COMPLETE);
    /* package.json(2바이트)만 남았다: 격리된 7바이트는 세지 않는다 */
    CHECK(model.len > 0 && model.nodes[0].total_bytes == 2);
    CHECK(model.report.excluded_dir_count == 1);
    scan_model_free(&model);

    /* 격리 폴더 화면처럼 전부 복원한다 */
    for (i = 0; i < list.len; i++) {
        CHECK_Q(quarantine_restore(&list.items[i], NULL, NULL), QUARANTINE_OK);
    }
    CHECK(fixture_has_text(L"app\\server.log", "log"));
    CHECK(fixture_has_text(L"app\\node_modules\\i.js", "1234"));

    /* 시스템 목록에도 테스트용 루트가 포함된다(이제 비었음) */
    quarantine_list_system(&list);
    for (i = 0; i < list.len; i++) {
        CHECK(_wcsicmp(list.items[i].volume_root, g_root) != 0);
    }

    SetEnvironmentVariableW(L"BYTESIEVE_QUARANTINE_ROOT", NULL);
    free(app);
    cleanup_result_free(&r);
    cleanup_guard_free(&g);
    quarantine_list_free(&list);
}

int main(void)
{
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    wchar_t tmp[MAX_PATH];

    if (FAILED(init) || GetTempPathW(MAX_PATH, tmp) == 0) {
        fprintf(stderr, "setup failed (error %lu)\n", GetLastError());
        return 1;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_quarantine_%lu", tmp,
             (unsigned long)GetCurrentProcessId());

    test_status();
    test_names_and_guard();
    test_move_and_list();
    test_duplicate_names();
    test_move_failures();
    test_store_is_link();
    test_restore();
    test_restore_after_letter_change();
    test_inconsistent();
    test_purge();
    test_cleanup_integration();
    remove_tree(g_root);
    CoUninitialize();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all quarantine tests passed\n");
    return 0;
}
