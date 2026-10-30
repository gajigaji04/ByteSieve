#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <wchar.h>

#include "cli.h"
#include "scanner.h"

/*
 * 이후 wide 출력 함수만 사용한다(printf와 혼용 금지).
 * 콘솔은 UTF-16 으로 써야 한글이 깨지지 않고, 파이프·파일로 넘길 때는 UTF-8 로 쓴다.
 */
static void set_wide_mode(FILE *stream)
{
    int fd = _fileno(stream);
    _setmode(fd, _isatty(fd) ? _O_U16TEXT : _O_U8TEXT);
}

static int run_scan(const CliOptions *opts)
{
    ScanReport report;
    ScanStatus status;

    scan_report_init(&report);
    status = scan_tree(opts->target, opts->max_depth, &report, NULL, NULL);
    cli_print_report(stdout, stderr, opts->target, opts->max_depth, &report, status);
    scan_report_free(&report);
    return cli_exit_code(status);
}

int wmain(int argc, wchar_t **argv)
{
    CliOptions opts;
    wchar_t err[512];

    set_wide_mode(stdout);
    set_wide_mode(stderr);

    switch (cli_parse_args(argc, argv, &opts, err, sizeof(err) / sizeof(err[0]))) {
    case CLI_PARSE_HELP:
        cli_print_usage(stdout);
        return EXIT_COMPLETE;
    case CLI_PARSE_ERROR:
        fwprintf(stderr, L"error: %ls\n\n", err);
        cli_print_usage(stderr);
        return EXIT_FATAL;
    default:
        return run_scan(&opts);
    }
}
