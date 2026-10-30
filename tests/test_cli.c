#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cli.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

#define CHECK_WSTR(actual, expected)                                    \
    do {                                                                \
        const wchar_t *a_ = (actual), *e_ = (expected);                 \
        if (wcscmp(a_, e_) != 0) {                                      \
            fprintf(stderr, "%s:%d: expected \"%ls\", got \"%ls\"\n",   \
                    __FILE__, __LINE__, e_, a_);                        \
            g_failures++;                                               \
        }                                                               \
    } while (0)

#define ARGC(a) ((int)(sizeof(a) / sizeof((a)[0])))

static CliParseResult parse(int argc, wchar_t **argv, CliOptions *o, wchar_t *err)
{
    return cli_parse_args(argc, argv, o, err, 256);
}

static void test_parse_ok(void)
{
    CliOptions o;
    wchar_t err[256];

    {
        wchar_t *a[] = {L"disk-scanner", L"C:\\data"};
        CHECK(parse(ARGC(a), a, &o, err) == CLI_PARSE_OK);
        CHECK_WSTR(o.target, L"C:\\data");
        CHECK(o.max_depth == SCAN_DEPTH_UNLIMITED);
    }
    {
        wchar_t *a[] = {L"disk-scanner", L"--max-depth", L"3", L"C:\\data"};
        CHECK(parse(ARGC(a), a, &o, err) == CLI_PARSE_OK);
        CHECK_WSTR(o.target, L"C:\\data");
        CHECK(o.max_depth == 3);
    }
    {
        /* 옵션은 대상 뒤에 와도 되고 = 형식도 받는다. */
        wchar_t *a[] = {L"disk-scanner", L"C:\\data", L"--max-depth=0"};
        CHECK(parse(ARGC(a), a, &o, err) == CLI_PARSE_OK);
        CHECK_WSTR(o.target, L"C:\\data");
        CHECK(o.max_depth == 0);
    }
    {
        /* "--" 뒤의 '-' 로 시작하는 이름은 대상이다. */
        wchar_t *a[] = {L"disk-scanner", L"--", L"-weird"};
        CHECK(parse(ARGC(a), a, &o, err) == CLI_PARSE_OK);
        CHECK_WSTR(o.target, L"-weird");
    }
    {
        wchar_t *a[] = {L"disk-scanner", L"--max-depth", L"4294967294", L"x"};
        CHECK(parse(ARGC(a), a, &o, err) == CLI_PARSE_OK);
        CHECK(o.max_depth == 4294967294u);
        CHECK(o.jsonl == 0);
    }
    {
        wchar_t *a[] = {L"disk-scanner", L"x", L"--jsonl", L"--max-depth=2"};
        CHECK(parse(ARGC(a), a, &o, err) == CLI_PARSE_OK);
        CHECK_WSTR(o.target, L"x");
        CHECK(o.jsonl == 1 && o.max_depth == 2);
    }
    {
        wchar_t *a[] = {L"disk-scanner", L"--jsonl", L"--jsonl", L"x"};
        CHECK(parse(ARGC(a), a, &o, err) == CLI_PARSE_ERROR);
        CHECK_WSTR(err, L"--jsonl given more than once");
    }
}

static void test_parse_help(void)
{
    CliOptions o;
    wchar_t err[256];
    wchar_t *a1[] = {L"disk-scanner", L"-h"};
    wchar_t *a2[] = {L"disk-scanner", L"C:\\x", L"--help"};
    wchar_t *a3[] = {L"disk-scanner", L"--bogus", L"--help"};
    wchar_t *a4[] = {L"disk-scanner", L"--", L"--help"};

    CHECK(parse(ARGC(a1), a1, &o, err) == CLI_PARSE_HELP);
    CHECK(parse(ARGC(a2), a2, &o, err) == CLI_PARSE_HELP);
    CHECK(parse(ARGC(a3), a3, &o, err) == CLI_PARSE_HELP);
    /* "--" 뒤의 --help 는 대상 이름이다. */
    CHECK(parse(ARGC(a4), a4, &o, err) == CLI_PARSE_OK);
}

static void check_error(int argc, wchar_t **argv, const wchar_t *expected_prefix)
{
    CliOptions o;
    wchar_t err[256];

    CHECK(parse(argc, argv, &o, err) == CLI_PARSE_ERROR);
    if (wcsncmp(err, expected_prefix, wcslen(expected_prefix)) != 0) {
        fprintf(stderr, "expected error starting \"%ls\", got \"%ls\"\n",
                expected_prefix, err);
        g_failures++;
    }
}

static void test_parse_errors(void)
{
    wchar_t *none[] = {L"disk-scanner"};
    wchar_t *only_opt[] = {L"disk-scanner", L"--max-depth", L"2"};
    wchar_t *no_value[] = {L"disk-scanner", L"x", L"--max-depth"};
    wchar_t *negative[] = {L"disk-scanner", L"--max-depth", L"-1", L"x"};
    wchar_t *not_num[] = {L"disk-scanner", L"--max-depth=abc", L"x"};
    wchar_t *trailing[] = {L"disk-scanner", L"--max-depth=1x", L"x"};
    wchar_t *empty_val[] = {L"disk-scanner", L"--max-depth=", L"x"};
    wchar_t *overflow[] = {L"disk-scanner", L"--max-depth", L"4294967295", L"x"};
    wchar_t *dup[] = {L"disk-scanner", L"--max-depth=1", L"--max-depth=2", L"x"};
    wchar_t *unknown[] = {L"disk-scanner", L"--verbose", L"x"};
    wchar_t *prefix[] = {L"disk-scanner", L"--max-depthX", L"x"};
    wchar_t *extra[] = {L"disk-scanner", L"a", L"b"};
    wchar_t *empty[] = {L"disk-scanner", L""};

    check_error(ARGC(none), none, L"missing <directory>");
    check_error(ARGC(only_opt), only_opt, L"missing <directory>");
    check_error(ARGC(no_value), no_value, L"--max-depth requires a value");
    check_error(ARGC(negative), negative, L"invalid --max-depth value: '-1'");
    check_error(ARGC(not_num), not_num, L"invalid --max-depth value: 'abc'");
    check_error(ARGC(trailing), trailing, L"invalid --max-depth value: '1x'");
    check_error(ARGC(empty_val), empty_val, L"invalid --max-depth value: ''");
    check_error(ARGC(overflow), overflow, L"invalid --max-depth value");
    check_error(ARGC(dup), dup, L"--max-depth given more than once");
    check_error(ARGC(unknown), unknown, L"unknown option: --verbose");
    check_error(ARGC(prefix), prefix, L"unknown option: --max-depthX");
    check_error(ARGC(extra), extra, L"unexpected extra argument: b");
    check_error(ARGC(empty), empty, L"<directory> must not be empty");
}

static void test_exit_code(void)
{
    CHECK(cli_exit_code(SCAN_COMPLETE) == 0);
    CHECK(cli_exit_code(SCAN_PARTIAL) == 1);
    CHECK(cli_exit_code(SCAN_FATAL) == 2);
}

static void test_format_size(void)
{
    wchar_t buf[64];

    cli_format_size(0, buf, 64);
    CHECK_WSTR(buf, L"0 bytes");
    cli_format_size(1023, buf, 64);
    CHECK_WSTR(buf, L"1023 bytes");
    cli_format_size(1024, buf, 64);
    CHECK_WSTR(buf, L"1.00 KiB (1024 bytes)");
    cli_format_size(1536, buf, 64);
    CHECK_WSTR(buf, L"1.50 KiB (1536 bytes)");
    cli_format_size(5368709123ULL, buf, 64);
    CHECK_WSTR(buf, L"5.00 GiB (5368709123 bytes)");
    cli_format_size(0xFFFFFFFFFFFFFFFFULL, buf, 64);
    CHECK_WSTR(buf, L"16.00 EiB (18446744073709551615 bytes)");
}

/* 테스트용 보고서. errors 는 정적 배열이라 scan_report_free 를 쓰지 않는다. */
static ScanError g_errors[25];

static void make_report(ScanReport *r, size_t n_errors, uint64_t unrecorded)
{
    size_t i;

    scan_report_init(r);
    r->file_count = 7;
    r->dir_count = 2;
    r->total_bytes = 1536;
    r->reparse_count = 1;
    for (i = 0; i < n_errors; i++) {
        g_errors[i].path = L"C:\\x\\locked";
        g_errors[i].code = i % 2 ? 3 : 5;
        g_errors[i].kind = i % 2 ? SCAN_ERR_NOT_FOUND : SCAN_ERR_ACCESS_DENIED;
    }
    r->errors = g_errors;
    r->errors_len = n_errors;
    r->errors_cap = n_errors;
    r->error_count = n_errors + unrecorded;
}

static void test_count_error_kinds(void)
{
    ScanReport r;
    uint64_t counts[SCAN_ERR_KIND_COUNT];

    make_report(&r, 5, 0);
    g_errors[4].kind = SCAN_ERR_PATH_TOO_LONG;
    cli_count_error_kinds(&r, counts);
    CHECK(counts[SCAN_ERR_ACCESS_DENIED] == 2);
    CHECK(counts[SCAN_ERR_NOT_FOUND] == 2);
    CHECK(counts[SCAN_ERR_PATH_TOO_LONG] == 1);
    CHECK(counts[SCAN_ERR_OTHER] == 0);
}

/*
 * tmpfile 의 출력(MSVC 는 바이너리 스트림에 wide 문자를 UTF-16 그대로 쓴다)을
 * ASCII 로 좁혀 돌려준다. 테스트 문자열은 모두 ASCII 다.
 */
static void read_back(FILE *f, char *buf, size_t buf_len)
{
    wchar_t wide[4096];
    size_t n, i;

    rewind(f);
    n = fread(wide, sizeof(wchar_t), sizeof(wide) / sizeof(wide[0]), f);
    for (i = 0; i < n && i + 1 < buf_len; i++) {
        buf[i] = wide[i] < 0x80 ? (char)wide[i] : '?';
    }
    buf[i] = '\0';
}

/* cli_print_report 출력을 임시 파일로 받아 문자열로 돌려준다. */
static void render(const ScanReport *r, ScanStatus st, uint32_t depth,
                   char *out, size_t out_len, char *err, size_t err_len)
{
    FILE *fo = tmpfile();
    FILE *fe = tmpfile();

    out[0] = err[0] = '\0';
    CHECK(fo != NULL && fe != NULL);
    if (fo == NULL || fe == NULL) {
        return;
    }
    cli_print_report(fo, fe, L"C:\\x", depth, r, st);
    read_back(fo, out, out_len);
    read_back(fe, err, err_len);
    fclose(fo);
    fclose(fe);
}

#define CHECK_HAS(text, needle)                                         \
    do {                                                                \
        if (strstr((text), (needle)) == NULL) {                         \
            fprintf(stderr, "%s:%d: missing \"%s\" in:\n%s\n",          \
                    __FILE__, __LINE__, (needle), (text));              \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static void test_print_complete(void)
{
    ScanReport r;
    char out[2048], err[2048];

    make_report(&r, 0, 0);
    render(&r, SCAN_COMPLETE, SCAN_DEPTH_UNLIMITED, out, sizeof(out), err, sizeof(err));
    CHECK_HAS(out, "Target:         C:\\x\n");
    CHECK_HAS(out, "Max depth:      unlimited\n");
    CHECK_HAS(out, "Files:          7\n");
    CHECK_HAS(out, "Directories:    2\n");
    CHECK_HAS(out, "Total size:     1.50 KiB (1536 bytes)\n");
    CHECK_HAS(out, "Reparse points: 1 (not descended)\n");
    CHECK_HAS(out, "Errors:         0\n");
    CHECK_HAS(out, "Skipped dirs:   0\n");
    CHECK_HAS(out, "Aborted:        no\n");
    CHECK_HAS(out, "Status:         complete (all directories scanned)\n");
    CHECK(err[0] == '\0');
}

static void test_print_partial(void)
{
    ScanReport r;
    char out[2048], err[4096];

    make_report(&r, 3, 0);
    r.skipped_dir_count = 2;
    render(&r, SCAN_PARTIAL, 5, out, sizeof(out), err, sizeof(err));
    CHECK_HAS(out, "Max depth:      5\n");
    CHECK_HAS(out, "Errors:         3 (access_denied 2, not_found 1)\n");
    CHECK_HAS(out, "Skipped dirs:   2\n");
    CHECK_HAS(out, "Status:         partial (2 directories could not be read; "
                   "totals are incomplete)\n");
    CHECK_HAS(err, "error: C:\\x\\locked: access_denied (Windows error 5)\n");
    CHECK_HAS(err, "error: C:\\x\\locked: not_found (Windows error 3)\n");
    CHECK(strstr(err, "more error(s)") == NULL);

    /* 건너뛴 디렉터리 없이 열거 도중 오류만 있었던 경우. */
    make_report(&r, 1, 0);
    render(&r, SCAN_PARTIAL, 5, out, sizeof(out), err, sizeof(err));
    CHECK_HAS(out, "Status:         partial (some entries could not be read; "
                   "totals may be incomplete)\n");
}

static void test_print_error_cap(void)
{
    ScanReport r;
    char out[2048], err[8192];
    const char *p;
    int lines = 0;

    /* 25개 저장 + 기록 못한 5개: 20개만 나열하고 나머지 10개는 한 줄로. */
    make_report(&r, 25, 5);
    r.skipped_dir_count = 1;
    render(&r, SCAN_PARTIAL, SCAN_DEPTH_UNLIMITED, out, sizeof(out), err, sizeof(err));
    for (p = err; (p = strstr(p, "(Windows error")) != NULL; p++) {
        lines++;
    }
    CHECK(lines == CLI_MAX_LISTED_ERRORS);
    CHECK_HAS(err, "error: ... and 10 more error(s)\n");
    CHECK_HAS(out, "Errors:         30 (access_denied 13, not_found 12, unrecorded 5)\n");
    CHECK_HAS(out, "(1 directory could not be read; totals are incomplete)\n");
}

static void test_print_fatal(void)
{
    ScanReport r;
    char out[2048], err[2048];

    make_report(&r, 1, 0);
    r.file_count = r.dir_count = r.total_bytes = r.reparse_count = 0;
    r.skipped_dir_count = 1;
    r.aborted = 1;
    render(&r, SCAN_FATAL, SCAN_DEPTH_UNLIMITED, out, sizeof(out), err, sizeof(err));
    CHECK_HAS(out, "Total size:     0 bytes\n");
    CHECK_HAS(out, "Aborted:        yes\n");
    CHECK_HAS(out, "Status:         fatal (scan aborted)\n");
    CHECK_HAS(err, "error: C:\\x\\locked: access_denied (Windows error 5)\n");
}

int main(void)
{
    test_parse_ok();
    test_parse_help();
    test_parse_errors();
    test_exit_code();
    test_format_size();
    test_count_error_kinds();
    test_print_complete();
    test_print_partial();
    test_print_error_cap();
    test_print_fatal();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all cli tests passed\n");
    return 0;
}
