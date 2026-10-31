#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path.h"
#include "scan_model.h"
#include "scanner.h"

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

/*
 * root\
 *   a\data.bin 5000, a\copy.bin  = data.bin 의 하드 링크(같은 폴더)
 *   b\link.bin = a\data.bin 의 하드 링크(다른 폴더), b\own.bin 300
 *   c\x.bin 700, d\x2.bin = c\x.bin 의 하드 링크
 *   e\zero.bin 0, e\zero2.bin = zero.bin 의 하드 링크(크기 0)
 * 경로마다: 파일 8개, 16700 B. 실제 파일: 4개, 6000 B. 중복 경로 4개, 10700 B.
 */
#define PATH_FILES 8
#define PATH_BYTES 16700
#define DUP_FILES 4
#define DUP_BYTES 10700
#define UNIQUE_BYTES (PATH_BYTES - DUP_BYTES)

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

static int make_dir(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    BOOL ok = p != NULL && CreateDirectoryW(p, NULL);

    free(p);
    return ok;
}

static int write_file(const wchar_t *name, DWORD len)
{
    static char data[8192];
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

static int hard_link(const wchar_t *link, const wchar_t *target)
{
    wchar_t *l = fixture(link), *t = fixture(target);
    BOOL ok = l != NULL && t != NULL && CreateHardLinkW(l, t, NULL);

    free(l);
    free(t);
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

/* 방문한 파일 이름과 중복 여부(작업 스레드 수와 상관없이 같은지 비교용). */
typedef struct {
    wchar_t names[32][64];
    int dup[32];
    size_t len;
    size_t dups;
} Visits;

static void on_visit(const ScanEntry *e, void *ctx)
{
    Visits *v = ctx;

    if (e->is_dir) {
        CHECK(!e->hardlink_dup);
        return;
    }
    if (v->len < 32) {
        swprintf(v->names[v->len], 64, L"%ls", e->name);
        v->dup[v->len] = e->hardlink_dup;
        v->len++;
    }
    v->dups += e->hardlink_dup != 0;
}

static void test_basic_mode_unchanged(void)
{
    ScanReport r;
    Visits v;

    memset(&v, 0, sizeof(v));
    scan_report_init(&r);
    CHECK(scan_tree(g_root, SCAN_DEPTH_UNLIMITED, &r, on_visit, &v) == SCAN_COMPLETE);
    CHECK_U64(r.file_count, PATH_FILES);
    CHECK_U64(r.total_bytes, PATH_BYTES);
    CHECK(!r.hardlinks_checked && !r.hardlinks_unsupported);
    CHECK_U64(r.hardlink_dup_files, 0);
    CHECK_U64(r.hardlink_dup_bytes, 0);
    CHECK_U64(r.hardlink_unknown_files, 0);
    CHECK_U64(v.dups, 0);
    scan_report_free(&r);
}

/* scan_tree 의 visit 과 같은 방문을 정밀 스캔(scan_tree_opts)에서 받는다. */
static uintptr_t visit_entry(const ScanEntry *e, uintptr_t parent, int will_descend, void *ctx)
{
    (void)parent;
    (void)will_descend;
    on_visit(e, ctx);
    return 1;
}

static ScanStatus run_precise(ScanReport *r, Visits *v, unsigned threads)
{
    ScanHooks hooks;
    ScanStatus st;

    memset(v, 0, sizeof(*v));
    memset(&hooks, 0, sizeof(hooks));
    hooks.on_entry = visit_entry;
    scan_report_init(r);
    scan_set_threads(threads);
    st = scan_tree_opts(g_root, SCAN_DEPTH_UNLIMITED, 1, r, &hooks, v, SCAN_OPT_HARDLINKS);
    scan_set_threads(0);
    return st;
}

static void test_precise_report(void)
{
    ScanReport r1, r8;
    Visits v1, v8;
    size_t i;

    CHECK(run_precise(&r1, &v1, 1) == SCAN_COMPLETE);
    CHECK(r1.hardlinks_checked && !r1.hardlinks_unsupported);
    /* 경로마다 센 기본 합계는 그대로 */
    CHECK_U64(r1.file_count, PATH_FILES);
    CHECK_U64(r1.total_bytes, PATH_BYTES);
    /* 중복 경로 */
    CHECK_U64(r1.hardlink_dup_files, DUP_FILES);
    CHECK_U64(r1.hardlink_dup_bytes, DUP_BYTES);
    CHECK_U64(r1.hardlink_unknown_files, 0);
    CHECK_U64(v1.dups, DUP_FILES);
    /* 실제 디스크 사용 = 할당 합 - 중복 할당. 중복 몫은 0 보다 크고 할당 합보다 작다 */
    CHECK(r1.hardlink_dup_alloc > 0 && r1.hardlink_dup_alloc < r1.alloc_bytes);
    CHECK(r1.alloc_bytes - r1.hardlink_dup_alloc >= UNIQUE_BYTES);

    /* 작업 스레드 수와 상관없이 같은 경로가 "처음"이다 */
    CHECK(run_precise(&r8, &v8, 8) == SCAN_COMPLETE);
    CHECK_U64(r8.hardlink_dup_files, DUP_FILES);
    CHECK_U64(r8.hardlink_dup_bytes, DUP_BYTES);
    CHECK_U64(r8.hardlink_dup_alloc, r1.hardlink_dup_alloc);
    CHECK(v1.len == v8.len);
    for (i = 0; i < v1.len && i < v8.len; i++) {
        CHECK(wcscmp(v1.names[i], v8.names[i]) == 0 && v1.dup[i] == v8.dup[i]);
    }
    /* 실제 파일마다 정확히 한 경로만 센다 */
    {
        int data_like = 0, x_like = 0, zero_like = 0;
        for (i = 0; i < v1.len; i++) {
            const wchar_t *n = v1.names[i];
            if (!v1.dup[i]) {
                data_like += wcscmp(n, L"data.bin") == 0 || wcscmp(n, L"copy.bin") == 0 ||
                             wcscmp(n, L"link.bin") == 0;
                x_like += wcscmp(n, L"x.bin") == 0 || wcscmp(n, L"x2.bin") == 0;
                zero_like += wcscmp(n, L"zero.bin") == 0 || wcscmp(n, L"zero2.bin") == 0;
            } else {
                CHECK(wcscmp(n, L"own.bin") != 0);
            }
        }
        CHECK(data_like == 1 && x_like == 1 && zero_like == 1);
    }
    scan_report_free(&r1);
    scan_report_free(&r8);
}

/* 이름으로 루트 바로 아래 노드를 찾는다. */
static const ScanNode *child(const ScanModel *m, const wchar_t *name)
{
    size_t i;

    for (i = 1; i < m->len; i++) {
        if (m->nodes[i].parent == 0 && wcscmp(m->nodes[i].name, name) == 0) {
            return &m->nodes[i];
        }
    }
    return NULL;
}

static ScanProgress g_last;

static void on_progress(const ScanProgress *p, void *ctx)
{
    (void)ctx;
    g_last = *p;
}

static void test_model(void)
{
    ScanModel basic, precise;
    const ScanNode *a, *b, *c, *d, *e, *root;
    size_t top, i;
    uint64_t sum_dup = 0;

    scan_model_init(&basic);
    scan_model_init(&precise);
    CHECK(scan_model_build(&basic, g_root, SCAN_DEPTH_UNLIMITED, NULL, NULL) == SCAN_COMPLETE);
    CHECK(basic.options == 0);
    for (i = 0; i < basic.len; i++) {
        CHECK(basic.nodes[i].total_dup_bytes == 0 && basic.nodes[i].own_dup_files == 0);
    }

    memset(&g_last, 0, sizeof(g_last));
    CHECK(scan_model_build_opts(&precise, g_root, SCAN_DEPTH_UNLIMITED, SCAN_OPT_HARDLINKS,
                                on_progress, NULL, NULL) == SCAN_COMPLETE);
    CHECK(precise.options == SCAN_OPT_HARDLINKS);
    /* 진행률에도 정밀 스캔과 중복이 보인다 */
    CHECK(g_last.finished && g_last.precise);
    CHECK_U64(g_last.dup_files, DUP_FILES);
    CHECK_U64(g_last.dup_bytes, DUP_BYTES);

    root = &precise.nodes[0];
    /* 기본 값(경로마다)은 기본 스캔과 같다 */
    CHECK_U64(root->total_bytes, basic.nodes[0].total_bytes);
    CHECK_U64(root->total_files, basic.nodes[0].total_files);
    CHECK_U64(root->total_alloc, basic.nodes[0].total_alloc);
    CHECK_U64(root->total_dup_files, DUP_FILES);
    CHECK_U64(root->total_dup_bytes, DUP_BYTES);
    CHECK_U64(root->total_dup_alloc, precise.report.hardlink_dup_alloc);
    CHECK_U64(root->total_bytes - root->total_dup_bytes, UNIQUE_BYTES);

    a = child(&precise, L"a");
    b = child(&precise, L"b");
    c = child(&precise, L"c");
    d = child(&precise, L"d");
    e = child(&precise, L"e");
    CHECK(a && b && c && d && e);
    if (a && b && c && d && e) {
        /* data 의 세 경로 중 하나만 실제 파일로: a·b 의 중복 합은 10000 */
        CHECK_U64(a->total_dup_bytes + b->total_dup_bytes, 10000);
        CHECK_U64(a->total_dup_files + b->total_dup_files, 2);
        /* own.bin 은 중복이 아니므로 b 의 중복 제외 용량은 300 이상 */
        CHECK(b->total_bytes - b->total_dup_bytes >= 300);
        CHECK_U64(c->total_dup_bytes + d->total_dup_bytes, 700);
        CHECK_U64(e->total_dup_files, 1);
        CHECK_U64(e->total_dup_bytes, 0);
        /* 자식들의 중복 합이 부모와 같다 */
        for (i = 1; i < precise.len; i++) {
            if (precise.nodes[i].parent == 0) {
                sum_dup += precise.nodes[i].total_dup_bytes;
            }
        }
        CHECK_U64(sum_dup + root->own_dup_bytes, root->total_dup_bytes);
    }

    /* 순위: 중복 용량이 가장 큰 폴더, 중복 제외 용량 순위 */
    CHECK(scan_model_rank(&precise, SCAN_RANK_TOTAL_DUP, &top, 1) == 1);
    CHECK(precise.nodes[top].total_dup_bytes >= 5000);
    CHECK(scan_model_rank(&precise, SCAN_RANK_TOTAL_UNIQUE, &top, 1) == 1);
    for (i = 1; i < precise.len; i++) {
        CHECK(precise.nodes[top].total_bytes - precise.nodes[top].total_dup_bytes >=
              precise.nodes[i].total_bytes - precise.nodes[i].total_dup_bytes);
    }
    /* 기본 스캔 모델의 중복 순위는 모두 0 */
    CHECK(scan_model_rank(&basic, SCAN_RANK_TOTAL_DUP, &top, 1) == 1 &&
          basic.nodes[top].total_dup_bytes == 0);

    scan_model_free(&basic);
    scan_model_free(&precise);
}

int main(void)
{
    wchar_t tmp[MAX_PATH];

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 1;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_hardlink_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    if (!CreateDirectoryW(g_root, NULL) || !make_dir(L"a") || !make_dir(L"b") ||
        !make_dir(L"c") || !make_dir(L"d") || !make_dir(L"e") ||
        !write_file(L"a\\data.bin", 5000) || !write_file(L"b\\own.bin", 300) ||
        !write_file(L"c\\x.bin", 700) || !write_file(L"e\\zero.bin", 0)) {
        fprintf(stderr, "fixture setup failed\n");
        remove_tree(g_root);
        return 1;
    }
    if (!hard_link(L"a\\copy.bin", L"a\\data.bin") || !hard_link(L"b\\link.bin", L"a\\data.bin") ||
        !hard_link(L"d\\x2.bin", L"c\\x.bin") || !hard_link(L"e\\zero2.bin", L"e\\zero.bin")) {
        fprintf(stderr, "could not create hard links (temp folder must be on NTFS)\n");
        remove_tree(g_root);
        return 1;
    }

    test_basic_mode_unchanged();
    test_precise_report();
    test_model();
    remove_tree(g_root);

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all hardlink tests passed\n");
    return 0;
}
