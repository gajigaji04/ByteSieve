#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

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

/* 결과를 기대값과 비교하고 해제한다. expected 가 NULL 이면 NULL 을 기대한다. */
static void check_str(wchar_t *actual, const wchar_t *expected, int line)
{
    int ok = (expected == NULL) ? actual == NULL
                                : actual != NULL && wcscmp(actual, expected) == 0;
    if (!ok) {
        fprintf(stderr, "%s:%d: expected \"%ls\", got \"%ls\"\n", __FILE__, line,
                expected ? expected : L"(null)", actual ? actual : L"(null)");
        g_failures++;
    }
    free(actual);
}

#define CHECK_STR(actual, expected) check_str((actual), (expected), __LINE__)

static void test_predicates(void)
{
    CHECK(path_is_separator(L'\\'));
    CHECK(path_is_separator(L'/'));
    CHECK(!path_is_separator(L'a'));

    CHECK(path_is_extended(L"\\\\?\\C:\\x"));
    CHECK(!path_is_extended(L"C:\\x"));
    CHECK(!path_is_extended(NULL));

    CHECK(path_is_unc(L"\\\\server\\share"));
    CHECK(path_is_unc(L"//server/share"));
    CHECK(path_is_unc(L"\\\\?\\UNC\\server\\share"));
    CHECK(!path_is_unc(L"\\\\?\\C:\\x"));
    CHECK(!path_is_unc(L"\\\\.\\PhysicalDrive0"));
    CHECK(!path_is_unc(L"C:\\x"));
    CHECK(!path_is_unc(L"\\x"));
    CHECK(!path_is_unc(L"\\\\"));
}

static void test_join(void)
{
    CHECK_STR(path_join(L"C:\\dir", L"file.txt"), L"C:\\dir\\file.txt");
    CHECK_STR(path_join(L"C:\\dir\\", L"file.txt"), L"C:\\dir\\file.txt");
    CHECK_STR(path_join(L"C:\\dir/", L"file.txt"), L"C:\\dir/file.txt");
    CHECK_STR(path_join(L"C:\\dir", L"\\file.txt"), L"C:\\dir\\file.txt");
    CHECK_STR(path_join(L"C:\\", L"file.txt"), L"C:\\file.txt");
    CHECK_STR(path_join(L"C:", L"file.txt"), L"C:file.txt");
    CHECK_STR(path_join(L"", L"file.txt"), L"file.txt");
    CHECK_STR(path_join(L"C:\\dir", L""), L"C:\\dir");
    CHECK_STR(path_join(L"\\\\server\\share", L"a"), L"\\\\server\\share\\a");
    CHECK_STR(path_join(L"\\\\?\\C:\\dir", L"한글.txt"), L"\\\\?\\C:\\dir\\한글.txt");
    CHECK_STR(path_join(NULL, L"a"), NULL);
    CHECK_STR(path_join(L"a", NULL), NULL);
}

static void test_join_long(void)
{
    wchar_t *path = path_dup(L"C:\\root");
    wchar_t *next;
    int i;

    /* 30자 세그먼트 20개 -> MAX_PATH(260) 를 훨씬 넘는다. */
    for (i = 0; i < 20 && path != NULL; i++) {
        next = path_join(path, L"segment_abcdefghijklmnopqrstuv");
        free(path);
        path = next;
    }
    CHECK(path != NULL);
    if (path != NULL) {
        CHECK(wcslen(path) == 7 + 20 * 31);
        next = path_to_extended(path);
        CHECK(next != NULL);
        if (next != NULL) {
            CHECK(wcsncmp(next, L"\\\\?\\C:\\root\\segment_", 19) == 0);
            CHECK(wcslen(next) == wcslen(path) + 4);
            free(next);
        }
        free(path);
    }
}

static void test_join_limit(void)
{
    size_t n = PATH_MAX_WIDE - 1;
    wchar_t *big = malloc((n + 1) * sizeof(wchar_t));
    size_t i;

    CHECK(big != NULL);
    if (big == NULL) {
        return;
    }
    for (i = 0; i < n; i++) {
        big[i] = L'a';
    }
    big[n] = L'\0';

    /* n + 구분자 = PATH_MAX_WIDE 는 허용, 그 이상은 거부한다. */
    {
        wchar_t *ok = path_join(big, L"");
        CHECK(ok != NULL);
        free(ok);
    }
    CHECK_STR(path_join(big, L"b"), NULL);
    free(big);
}

static void test_to_extended(void)
{
    CHECK_STR(path_to_extended(L"C:\\dir\\file"), L"\\\\?\\C:\\dir\\file");
    CHECK_STR(path_to_extended(L"C:/dir/./sub/../file"), L"\\\\?\\C:\\dir\\file");
    CHECK_STR(path_to_extended(L"\\\\server\\share\\dir"),
              L"\\\\?\\UNC\\server\\share\\dir");
    CHECK_STR(path_to_extended(L"//server/share/a/../b"),
              L"\\\\?\\UNC\\server\\share\\b");
    CHECK_STR(path_to_extended(L"\\\\?\\C:\\keep\\..\\as-is"),
              L"\\\\?\\C:\\keep\\..\\as-is");
    CHECK_STR(path_to_extended(L"\\\\.\\PhysicalDrive0"), L"\\\\.\\PhysicalDrive0");
    CHECK_STR(path_to_extended(L""), NULL);
    CHECK_STR(path_to_extended(NULL), NULL);
}

static void test_relative_to_extended(void)
{
    wchar_t *ext = path_to_extended(L"relative\\file");

    CHECK(ext != NULL);
    if (ext != NULL) {
        size_t len = wcslen(ext);
        CHECK(path_is_extended(ext));
        CHECK(len > 14 && wcscmp(ext + len - 14, L"\\relative\\file") == 0);
        free(ext);
    }
}

static void test_strip_extended(void)
{
    CHECK_STR(path_strip_extended(L"\\\\?\\C:\\dir"), L"C:\\dir");
    CHECK_STR(path_strip_extended(L"\\\\?\\UNC\\server\\share\\x"),
              L"\\\\server\\share\\x");
    CHECK_STR(path_strip_extended(L"C:\\dir"), L"C:\\dir");
    CHECK_STR(path_strip_extended(NULL), NULL);
}

int main(void)
{
    test_predicates();
    test_join();
    test_join_long();
    test_join_limit();
    test_to_extended();
    test_relative_to_extended();
    test_strip_extended();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all path tests passed\n");
    return 0;
}
