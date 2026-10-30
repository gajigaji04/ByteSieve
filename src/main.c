#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <wchar.h>

#include "jsonl.h"
#include "scanner.h"

/* JSON Lines 모드에서 progress 이벤트를 내는 항목 간격. */
#define JSONL_PROGRESS_INTERVAL 1000

enum {
    EXIT_COMPLETE = 0,
    EXIT_PARTIAL = 1,
    EXIT_FATAL = 2
};

static void print_usage(FILE *out)
{
    fwprintf(out,
             L"Usage: disk-scanner [--jsonl] <directory>\n"
             L"       disk-scanner -h | --help\n"
             L"\n"
             L"Counts files, directories and total size under <directory>.\n"
             L"\n"
             L"  --jsonl  print start/progress/result/complete events as\n"
             L"           UTF-8 JSON Lines on stdout\n");
}

static int is_help(const wchar_t *arg)
{
    return wcscmp(arg, L"-h") == 0 || wcscmp(arg, L"--help") == 0;
}

static int exit_code_for(ScanStatus status)
{
    switch (status) {
    case SCAN_COMPLETE:
        return EXIT_COMPLETE;
    case SCAN_PARTIAL:
        return EXIT_PARTIAL;
    default:
        return EXIT_FATAL;
    }
}

/* stdout 은 바이너리 모드여야 한다(UTF-8 바이트를 그대로, 줄바꿈은 LF). */
static int run_scan_jsonl(const wchar_t *target)
{
    ScanReport report;
    ScanStatus status;
    JsonBuf buf;
    JsonlProgress progress;
    int code;

    scan_report_init(&report);
    jsonl_buf_init(&buf);

    jsonl_start(&buf, target);
    jsonl_buf_flush(&buf, stdout);

    progress.buf = &buf;
    progress.report = &report;
    progress.out = stdout;
    progress.interval = JSONL_PROGRESS_INTERVAL;
    progress.seen = 0;
    status = scan_directory(target, &report, jsonl_progress_visit, &progress);
    code = exit_code_for(status);

    jsonl_result(&buf, target, &report);
    jsonl_complete(&buf, status, code);
    if (!jsonl_buf_flush(&buf, stdout)) {
        fwprintf(stderr, L"error: failed to write JSON output\n");
        code = EXIT_FATAL;
    }

    jsonl_buf_free(&buf);
    scan_report_free(&report);
    return code;
}

static int run_scan(const wchar_t *target)
{
    ScanReport report;
    ScanStatus status;
    size_t i;

    scan_report_init(&report);
    status = scan_directory(target, &report, NULL, NULL);

    for (i = 0; i < report.errors_len; i++) {
        fwprintf(stderr, L"error: %ls: Windows error %lu\n",
                 report.errors[i].path ? report.errors[i].path : target,
                 (unsigned long)report.errors[i].code);
    }

    wprintf(L"Target:         %ls\n", target);
    wprintf(L"Files:          %llu\n", (unsigned long long)report.file_count);
    wprintf(L"Directories:    %llu\n", (unsigned long long)report.dir_count);
    wprintf(L"Total size:     %llu bytes\n", (unsigned long long)report.total_bytes);
    wprintf(L"Reparse points: %llu (not descended)\n",
            (unsigned long long)report.reparse_count);
    wprintf(L"Errors:         %llu\n", (unsigned long long)report.error_count);

    scan_report_free(&report);
    return exit_code_for(status);
}

int wmain(int argc, wchar_t **argv)
{
    int jsonl = argc == 3 && wcscmp(argv[1], L"--jsonl") == 0;
    const wchar_t *target = argv[argc - 1];

    /*
     * 이후 stderr 와 일반 모드 stdout 에는 wide 출력 함수만 사용한다(printf와 혼용 금지).
     * JSON Lines 모드의 stdout 은 UTF-8 바이트를 fwrite 로만 쓴다.
     */
    _setmode(_fileno(stdout), jsonl ? _O_BINARY : _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    if (argc != 2 && !jsonl) {
        print_usage(stderr);
        return EXIT_FATAL;
    }

    if (!jsonl && is_help(target)) {
        print_usage(stdout);
        return EXIT_COMPLETE;
    }

    if (target[0] == L'-') {
        fwprintf(stderr, L"error: unknown option: %ls\n", target);
        print_usage(stderr);
        return EXIT_FATAL;
    }

    return jsonl ? run_scan_jsonl(target) : run_scan(target);
}
