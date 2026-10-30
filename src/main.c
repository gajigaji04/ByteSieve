#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <wchar.h>

#include "cli.h"
#include "jsonl.h"
#include "scanner.h"

/* JSON Lines 모드에서 progress 이벤트를 내는 항목 간격. */
#define JSONL_PROGRESS_INTERVAL 1000

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

/* stdout 은 바이너리 모드여야 한다(UTF-8 바이트를 그대로, 줄바꿈은 LF). */
static int run_scan_jsonl(const CliOptions *opts)
{
    ScanReport report;
    ScanStatus status;
    JsonBuf buf;
    JsonlProgress progress;
    int code;

    scan_report_init(&report);
    jsonl_buf_init(&buf);

    jsonl_start(&buf, opts->target);
    jsonl_buf_flush(&buf, stdout);

    progress.buf = &buf;
    progress.report = &report;
    progress.out = stdout;
    progress.interval = JSONL_PROGRESS_INTERVAL;
    progress.seen = 0;
    status = scan_tree(opts->target, opts->max_depth, &report,
                       jsonl_progress_visit, &progress);
    code = cli_exit_code(status);

    jsonl_result(&buf, opts->target, &report);
    jsonl_complete(&buf, status, code);
    if (!jsonl_buf_flush(&buf, stdout)) {
        fwprintf(stderr, L"error: failed to write JSON output\n");
        code = EXIT_FATAL;
    }

    jsonl_buf_free(&buf);
    scan_report_free(&report);
    return code;
}

int wmain(int argc, wchar_t **argv)
{
    CliOptions opts;
    wchar_t err[512];
    CliParseResult parsed;

    set_wide_mode(stderr);
    parsed = cli_parse_args(argc, argv, &opts, err, sizeof(err) / sizeof(err[0]));

    /* JSON Lines 모드의 stdout 은 UTF-8 바이트를 fwrite 로만 쓴다. */
    if (parsed == CLI_PARSE_OK && opts.jsonl) {
        _setmode(_fileno(stdout), _O_BINARY);
        return run_scan_jsonl(&opts);
    }
    set_wide_mode(stdout);

    switch (parsed) {
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
