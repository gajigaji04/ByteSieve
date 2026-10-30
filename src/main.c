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
             L"Usage: disk-scanner [--max-depth N] <directory>\n"
             L"       disk-scanner -h | --help\n"
             L"\n"
             L"Counts files, directories and total size under <directory>,\n"
             L"including all subdirectories.\n"
             L"\n"
             L"  --max-depth N  descend at most N levels below <directory>\n"
             L"                 (0 = only <directory> itself; default: unlimited)\n");
}

static int is_help(const wchar_t *arg)
{
    return wcscmp(arg, L"-h") == 0 || wcscmp(arg, L"--help") == 0;
}

/* 0 이상의 10진 정수만 허용한다. */
static int parse_depth(const wchar_t *arg, uint32_t *out)
{
    wchar_t *end;
    unsigned long long value;

    if (arg[0] < L'0' || arg[0] > L'9') {
        return 0;
    }
    value = wcstoull(arg, &end, 10);
    if (*end != L'\0' || value >= SCAN_DEPTH_UNLIMITED) {
        return 0;
    }
    *out = (uint32_t)value;
    return 1;
}

static int run_scan(const wchar_t *target, uint32_t max_depth)
{
    ScanReport report;
    ScanStatus status;
    size_t i;

    scan_report_init(&report);
    status = scan_tree(target, max_depth, &report, NULL, NULL);

    for (i = 0; i < report.errors_len; i++) {
        fwprintf(stderr, L"error: %ls: %ls (Windows error %lu)\n",
                 report.errors[i].path ? report.errors[i].path : target,
                 scan_error_kind_name(report.errors[i].kind),
                 (unsigned long)report.errors[i].code);
    }
    if (report.error_count > report.errors_len) {
        fwprintf(stderr, L"error: %llu more error(s) not recorded (out of memory)\n",
                 (unsigned long long)(report.error_count - report.errors_len));
    }

    wprintf(L"Target:         %ls\n", target);
    if (max_depth == SCAN_DEPTH_UNLIMITED) {
        wprintf(L"Max depth:      unlimited\n");
    } else {
        wprintf(L"Max depth:      %lu\n", (unsigned long)max_depth);
    }
    wprintf(L"Files:          %llu\n", (unsigned long long)report.file_count);
    wprintf(L"Directories:    %llu\n", (unsigned long long)report.dir_count);
    wprintf(L"Total size:     %llu bytes\n", (unsigned long long)report.total_bytes);
    wprintf(L"Reparse points: %llu (not descended)\n",
            (unsigned long long)report.reparse_count);
    wprintf(L"Errors:         %llu\n", (unsigned long long)report.error_count);
    wprintf(L"Skipped dirs:   %llu\n", (unsigned long long)report.skipped_dir_count);
    wprintf(L"Aborted:        %ls\n", report.aborted ? L"yes" : L"no");
    wprintf(L"Status:         %ls\n", scan_status_name(status));

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
    uint32_t max_depth = SCAN_DEPTH_UNLIMITED;
    int i = 1;

    /* 이후 wide 출력 함수만 사용한다(printf와 혼용 금지). */
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    if (argc == 2 && is_help(argv[1])) {
        print_usage(stdout);
        return EXIT_COMPLETE;
    }

    if (argc == 4 && wcscmp(argv[1], L"--max-depth") == 0) {
        if (!parse_depth(argv[2], &max_depth)) {
            fwprintf(stderr, L"error: invalid --max-depth value: %ls\n", argv[2]);
            print_usage(stderr);
            return EXIT_FATAL;
        }
        i = 3;
    } else if (argc != 2) {
        print_usage(stderr);
        return EXIT_FATAL;
    }

    if (argv[i][0] == L'-') {
        fwprintf(stderr, L"error: unknown option: %ls\n", argv[i]);
        print_usage(stderr);
        return EXIT_FATAL;
    }

    return run_scan(argv[i], max_depth);
}
