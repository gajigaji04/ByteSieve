#include <stdlib.h>
#include <string.h>

#include "cli.h"

void cli_print_usage(FILE *out)
{
    fwprintf(out,
             L"Usage: disk-scanner [--max-depth N] [--jsonl] <directory>\n"
             L"       disk-scanner -h | --help\n"
             L"\n"
             L"Counts files, directories and total size under <directory>,\n"
             L"including all subdirectories.\n"
             L"\n"
             L"Options:\n"
             L"  --max-depth N  descend at most N levels below <directory>\n"
             L"                 (0 = only <directory> itself; default: unlimited)\n"
             L"  --jsonl        print start/progress/result/complete events as\n"
             L"                 UTF-8 JSON Lines on stdout\n"
             L"  -h, --help     show this help\n"
             L"  --             treat the next argument as <directory>\n"
             L"\n"
             L"Exit codes:\n"
             L"  0  complete: every directory was scanned\n"
             L"  1  partial: some directories could not be read\n"
             L"  2  fatal: <directory> could not be scanned, or invalid arguments\n");
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

CliParseResult cli_parse_args(int argc, wchar_t **argv, CliOptions *opts,
                              wchar_t *err, size_t err_len)
{
    static const wchar_t depth_opt[] = L"--max-depth";
    const size_t depth_opt_len = sizeof(depth_opt) / sizeof(depth_opt[0]) - 1;
    int have_depth = 0;
    int options_done = 0;
    int i;

    opts->target = NULL;
    opts->max_depth = SCAN_DEPTH_UNLIMITED;
    opts->jsonl = 0;
    if (err_len > 0) {
        err[0] = L'\0';
    }

    /* 도움말은 다른 인자에 오류가 있어도 우선한다("--" 이후는 제외). */
    for (i = 1; i < argc && wcscmp(argv[i], L"--") != 0; i++) {
        if (is_help(argv[i])) {
            return CLI_PARSE_HELP;
        }
    }

    for (i = 1; i < argc; i++) {
        const wchar_t *arg = argv[i];
        const wchar_t *value = NULL;

        if (!options_done && wcscmp(arg, L"--") == 0) {
            options_done = 1;
            continue;
        }

        if (!options_done && wcsncmp(arg, depth_opt, depth_opt_len) == 0 &&
            (arg[depth_opt_len] == L'\0' || arg[depth_opt_len] == L'=')) {
            if (have_depth) {
                swprintf(err, err_len, L"--max-depth given more than once");
                return CLI_PARSE_ERROR;
            }
            if (arg[depth_opt_len] == L'=') {
                value = arg + depth_opt_len + 1;
            } else if (i + 1 < argc) {
                value = argv[++i];
            } else {
                swprintf(err, err_len, L"--max-depth requires a value");
                return CLI_PARSE_ERROR;
            }
            if (!parse_depth(value, &opts->max_depth)) {
                swprintf(err, err_len, L"invalid --max-depth value: '%ls' "
                         L"(expected a non-negative integer)", value);
                return CLI_PARSE_ERROR;
            }
            have_depth = 1;
            continue;
        }

        if (!options_done && wcscmp(arg, L"--jsonl") == 0) {
            if (opts->jsonl) {
                swprintf(err, err_len, L"--jsonl given more than once");
                return CLI_PARSE_ERROR;
            }
            opts->jsonl = 1;
            continue;
        }

        if (!options_done && arg[0] == L'-' && arg[1] != L'\0') {
            swprintf(err, err_len, L"unknown option: %ls", arg);
            return CLI_PARSE_ERROR;
        }

        if (opts->target != NULL) {
            swprintf(err, err_len, L"unexpected extra argument: %ls", arg);
            return CLI_PARSE_ERROR;
        }
        if (arg[0] == L'\0') {
            swprintf(err, err_len, L"<directory> must not be empty");
            return CLI_PARSE_ERROR;
        }
        opts->target = arg;
    }

    if (opts->target == NULL) {
        swprintf(err, err_len, L"missing <directory>");
        return CLI_PARSE_ERROR;
    }
    return CLI_PARSE_OK;
}

int cli_exit_code(ScanStatus status)
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

void cli_format_size(uint64_t bytes, wchar_t *buf, size_t buf_len)
{
    static const wchar_t *units[] = {L"KiB", L"MiB", L"GiB", L"TiB", L"PiB", L"EiB"};
    double value = (double)bytes;
    size_t unit = 0;

    if (bytes < 1024) {
        swprintf(buf, buf_len, L"%llu bytes", (unsigned long long)bytes);
        return;
    }
    value /= 1024.0;
    while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }
    swprintf(buf, buf_len, L"%.2f %ls (%llu bytes)", value, units[unit],
             (unsigned long long)bytes);
}

void cli_count_error_kinds(const ScanReport *report, uint64_t *counts)
{
    size_t i;

    memset(counts, 0, SCAN_ERR_KIND_COUNT * sizeof(*counts));
    for (i = 0; i < report->errors_len; i++) {
        ScanErrorKind kind = report->errors[i].kind;
        counts[(unsigned)kind < SCAN_ERR_KIND_COUNT ? kind : SCAN_ERR_OTHER]++;
    }
}

static void print_status(FILE *out, const ScanReport *report, ScanStatus status)
{
    fwprintf(out, L"Status:         %ls", scan_status_name(status));
    switch (status) {
    case SCAN_COMPLETE:
        fwprintf(out, L" (all directories scanned)\n");
        break;
    case SCAN_PARTIAL:
        if (report->skipped_dir_count > 0) {
            fwprintf(out, L" (%llu director%ls could not be read; totals are incomplete)\n",
                     (unsigned long long)report->skipped_dir_count,
                     report->skipped_dir_count == 1 ? L"y" : L"ies");
        } else {
            fwprintf(out, L" (some entries could not be read; totals may be incomplete)\n");
        }
        break;
    default:
        fwprintf(out, L" (scan aborted)\n");
        break;
    }
}

static void print_error_summary(FILE *out, const ScanReport *report)
{
    uint64_t counts[SCAN_ERR_KIND_COUNT];
    const wchar_t *sep = L" (";
    int kind;

    fwprintf(out, L"Errors:         %llu", (unsigned long long)report->error_count);
    if (report->error_count == 0) {
        fwprintf(out, L"\n");
        return;
    }

    cli_count_error_kinds(report, counts);
    for (kind = 0; kind < SCAN_ERR_KIND_COUNT; kind++) {
        if (counts[kind] > 0) {
            fwprintf(out, L"%ls%ls %llu", sep, scan_error_kind_name((ScanErrorKind)kind),
                     (unsigned long long)counts[kind]);
            sep = L", ";
        }
    }
    if (report->error_count > report->errors_len) {
        fwprintf(out, L"%lsunrecorded %llu", sep,
                 (unsigned long long)(report->error_count - report->errors_len));
    }
    fwprintf(out, L")\n");
}

void cli_print_report(FILE *out, FILE *err, const wchar_t *target,
                      uint32_t max_depth, const ScanReport *report,
                      ScanStatus status)
{
    wchar_t size[64];
    size_t i, listed;

    listed = report->errors_len < CLI_MAX_LISTED_ERRORS ? report->errors_len
                                                        : CLI_MAX_LISTED_ERRORS;
    for (i = 0; i < listed; i++) {
        fwprintf(err, L"error: %ls: %ls (Windows error %lu)\n",
                 report->errors[i].path ? report->errors[i].path : target,
                 scan_error_kind_name(report->errors[i].kind),
                 (unsigned long)report->errors[i].code);
    }
    if (report->error_count > listed) {
        fwprintf(err, L"error: ... and %llu more error(s)\n",
                 (unsigned long long)(report->error_count - listed));
    }

    cli_format_size(report->total_bytes, size, sizeof(size) / sizeof(size[0]));

    fwprintf(out, L"Target:         %ls\n", target);
    if (max_depth == SCAN_DEPTH_UNLIMITED) {
        fwprintf(out, L"Max depth:      unlimited\n");
    } else {
        fwprintf(out, L"Max depth:      %lu\n", (unsigned long)max_depth);
    }
    fwprintf(out, L"Files:          %llu\n", (unsigned long long)report->file_count);
    fwprintf(out, L"Directories:    %llu\n", (unsigned long long)report->dir_count);
    fwprintf(out, L"Total size:     %ls\n", size);
    fwprintf(out, L"Reparse points: %llu (not descended)\n",
             (unsigned long long)report->reparse_count);
    print_error_summary(out, report);
    fwprintf(out, L"Skipped dirs:   %llu\n", (unsigned long long)report->skipped_dir_count);
    fwprintf(out, L"Aborted:        %ls\n", report->aborted ? L"yes" : L"no");
    print_status(out, report, status);
}
