#include <stdio.h>
#include <wchar.h>

#include "size_format.h"

static int g_failures = 0;

static void check_size(uint64_t bytes, const wchar_t *expected, int line)
{
    wchar_t buf[32];

    format_size(bytes, buf, 32);
    if (wcscmp(buf, expected) != 0) {
        fprintf(stderr, "%s:%d: expected \"%ls\", got \"%ls\"\n", __FILE__, line, expected,
                buf);
        g_failures++;
    }
}

#define CHECK_SIZE(bytes, expected) check_size((bytes), (expected), __LINE__)

int main(void)
{
    wchar_t small[4] = L"xyz";

    CHECK_SIZE(0, L"0 B");
    CHECK_SIZE(1023, L"1023 B");
    CHECK_SIZE(1024, L"1.00 KiB");
    CHECK_SIZE(1536, L"1.50 KiB");
    CHECK_SIZE(10 * 1024, L"10.0 KiB");
    CHECK_SIZE(1024 * 1024, L"1.00 MiB");
    CHECK_SIZE(512ULL * 1024 * 1024 * 1024, L"512.0 GiB");
    CHECK_SIZE(2ULL * 1024 * 1024 * 1024 * 1024, L"2.00 TiB");
    CHECK_SIZE(UINT64_MAX, L"16.0 EiB");

    /* len 0 이면 버퍼를 건드리지 않는다. */
    format_size(1024, small, 0);
    if (wcscmp(small, L"xyz") != 0) {
        fprintf(stderr, "%s:%d: buffer modified with len 0\n", __FILE__, __LINE__);
        g_failures++;
    }
    format_size(1024, NULL, 32);

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all size_format tests passed\n");
    return 0;
}
