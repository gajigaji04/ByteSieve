#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <wchar.h>

#include "scanner.h"

enum {
    EXIT_COMPLETE = 0,
    EXIT_PARTIAL = 1,
    EXIT_FATAL = 2
};

static void print_usage(FILE *out)
{
    fwprintf(out,
             L"Usage: disk-scanner <directory>\n"
             L"       disk-scanner -h | --help\n"
             L"\n"
             L"Counts files, directories and total size under <directory>.\n");
}

static int is_help(const wchar_t *arg)
{
    return wcscmp(arg, L"-h") == 0 || wcscmp(arg, L"--help") == 0;
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

    switch (status) {
    case SCAN_COMPLETE:
        return EXIT_COMPLETE;
    case SCAN_PARTIAL:
        return EXIT_PARTIAL;
    default:
        return EXIT_FATAL;
    }
}

int wmain(int argc, wchar_t **argv)
{
    /* 이후 wide 출력 함수만 사용한다(printf와 혼용 금지). */
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    if (argc != 2) {
        print_usage(stderr);
        return EXIT_FATAL;
    }

    if (is_help(argv[1])) {
        print_usage(stdout);
        return EXIT_COMPLETE;
    }

    if (argv[1][0] == L'-') {
        fwprintf(stderr, L"error: unknown option: %ls\n", argv[1]);
        print_usage(stderr);
        return EXIT_FATAL;
    }

    return run_scan(argv[1]);
}
