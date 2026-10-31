#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "cli.h"
#include "size_format.h"

void cli_print_usage(FILE *out)
{
    fwprintf(out,
             L"Usage: disk-scanner [--max-depth N] [--jsonl] [--track [--history-dir DIR]]\n"
             L"                    [--threads N] <directory>\n"
             L"       disk-scanner -h | --help\n"
             L"\n"
             L"Counts files, directories, total size and size on disk under <directory>,\n"
             L"including all subdirectories.\n"
             L"\n"
             L"Options:\n"
             L"  --max-depth N  descend at most N levels below <directory>\n"
             L"                 (0 = only <directory> itself; default: unlimited)\n"
             L"  --jsonl        print start/progress/result/complete events as\n"
             L"                 UTF-8 JSON Lines on stdout\n"
             L"  --track        compare folder sizes with the previous --track scan of\n"
             L"                 <directory> and save a new snapshot\n"
             L"  --history-dir DIR\n"
             L"                 snapshot folder (default: BYTESIEVE_HISTORY_DIR, or\n"
             L"                 %%LOCALAPPDATA%%\\ByteSieve\\history)\n"
             L"  --threads N    directory enumeration threads (0 = auto, 1 = no\n"
             L"                 parallel prefetch; default: auto)\n"
             L"  -h, --help     show this help\n"
             L"  --             treat the next argument as <directory>\n"
             L"\n"
             L"Exit codes:\n"
             L"  0  complete: every directory was scanned\n"
             L"  1  partial: some directories could not be read\n"
             L"  2  fatal: <directory> could not be scanned, or invalid arguments\n");
}

/*
 * arg 가 "--name" 또는 "--name=값" 이면 값을 *value 에 넣고 1. "--name" 이면 다음 인자를
 * 값으로 쓴다(*i 를 늘림). 값이 없으면 err 에 쓰고 -1. 다른 옵션이면 0.
 */
static int value_option(const wchar_t *name, int argc, wchar_t **argv, int *i,
                        const wchar_t **value, wchar_t *err, size_t err_len)
{
    const wchar_t *arg = argv[*i];
    size_t n = wcslen(name);

    if (wcsncmp(arg, name, n) != 0 || (arg[n] != L'\0' && arg[n] != L'=')) {
        return 0;
    }
    if (arg[n] == L'=') {
        *value = arg + n + 1;
    } else if (*i + 1 < argc) {
        *value = argv[++*i];
    } else {
        swprintf(err, err_len, L"%ls requires a value", name);
        return -1;
    }
    return 1;
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
    opts->track = 0;
    opts->history_dir = NULL;
    opts->threads = -1;
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

        if (!options_done) {
            int r = value_option(L"--history-dir", argc, argv, &i, &value, err, err_len);
            if (r < 0) {
                return CLI_PARSE_ERROR;
            }
            if (r > 0) {
                if (opts->history_dir != NULL) {
                    swprintf(err, err_len, L"--history-dir given more than once");
                    return CLI_PARSE_ERROR;
                }
                if (value[0] == L'\0') {
                    swprintf(err, err_len, L"--history-dir must not be empty");
                    return CLI_PARSE_ERROR;
                }
                opts->history_dir = value;
                continue;
            }
            r = value_option(L"--threads", argc, argv, &i, &value, err, err_len);
            if (r < 0) {
                return CLI_PARSE_ERROR;
            }
            if (r > 0) {
                uint32_t n;
                if (opts->threads >= 0) {
                    swprintf(err, err_len, L"--threads given more than once");
                    return CLI_PARSE_ERROR;
                }
                if (!parse_depth(value, &n) || n > SCAN_MAX_THREADS) {
                    swprintf(err, err_len, L"invalid --threads value: '%ls' "
                             L"(expected 0 to %d)", value, SCAN_MAX_THREADS);
                    return CLI_PARSE_ERROR;
                }
                opts->threads = (int)n;
                continue;
            }
        }

        if (!options_done && wcscmp(arg, L"--track") == 0) {
            if (opts->track) {
                swprintf(err, err_len, L"--track given more than once");
                return CLI_PARSE_ERROR;
            }
            opts->track = 1;
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
    if (opts->history_dir != NULL && !opts->track) {
        swprintf(err, err_len, L"--history-dir requires --track");
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

/* 원인 하나(값이 0 이면 쓰지 않는다). */
static void print_saving(FILE *out, const wchar_t *label, uint64_t saved, uint64_t files,
                         const wchar_t *note)
{
    wchar_t size[64];

    if (saved == 0) {
        return;
    }
    cli_format_size(saved, size, sizeof(size) / sizeof(size[0]));
    fwprintf(out, L"  %-13ls -%ls in %llu file(s)%ls\n", label, size, (unsigned long long)files,
             note);
}

/* 논리 크기와 디스크 할당 크기의 차이를 원인별로 쓴다. */
static void print_disk_usage(FILE *out, const ScanReport *report)
{
    wchar_t size[64], cluster[32];

    cli_format_size(report->alloc_bytes, size, sizeof(size) / sizeof(size[0]));
    fwprintf(out, L"On disk:        %ls\n", size);
    if (report->slack_bytes > 0) {
        cli_format_size(report->slack_bytes, size, sizeof(size) / sizeof(size[0]));
        if (report->cluster_bytes > 0) {
            format_size(report->cluster_bytes, cluster, sizeof(cluster) / sizeof(cluster[0]));
            fwprintf(out, L"  %-13ls +%ls (cluster size %ls)\n", L"Slack:", size, cluster);
        } else {
            fwprintf(out, L"  %-13ls +%ls\n", L"Slack:", size);
        }
    }
    print_saving(out, L"Compressed:", report->compressed_saved_bytes, report->compressed_files,
                 L"");
    print_saving(out, L"Sparse:", report->sparse_saved_bytes, report->sparse_files, L"");
    print_saving(out, L"Cloud-only:", report->cloud_saved_bytes, report->cloud_files,
                 L" (not stored locally)");
    print_saving(out, L"Other:", report->other_saved_bytes, report->other_saved_files,
                 L" (MFT-resident or deduplicated)");
    if (report->alloc_unknown_files > 0) {
        fwprintf(out, L"  %-13ls %llu file(s) (allocation size not reported; counted at "
                      L"logical size)\n",
                 L"Unknown:", (unsigned long long)report->alloc_unknown_files);
    }
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
    print_disk_usage(out, report);
    fwprintf(out, L"Reparse points: %llu (not descended)\n",
             (unsigned long long)report->reparse_count);
    print_error_summary(out, report);
    fwprintf(out, L"Skipped dirs:   %llu\n", (unsigned long long)report->skipped_dir_count);
    fwprintf(out, L"Aborted:        %ls\n", report->aborted ? L"yes" : L"no");
    print_status(out, report, status);
}

/* 부호 있는 용량("+1.20 GiB", "-300 B", "0 B"). */
static void format_delta(int64_t delta, wchar_t *buf, size_t len)
{
    wchar_t size[32];
    uint64_t mag = delta < 0 ? (uint64_t)0 - (uint64_t)delta : (uint64_t)delta;

    format_size(mag, size, sizeof(size) / sizeof(size[0]));
    swprintf(buf, len, L"%ls%ls", delta > 0 ? L"+" : delta < 0 ? L"-" : L"", size);
}

void cli_print_changes(FILE *out, const HistoryDiff *diff, size_t limit)
{
    FILETIME utc, local;
    SYSTEMTIME st;
    wchar_t delta[48], from[32], to[32];
    size_t i, shown;

    utc.dwLowDateTime = (DWORD)(diff->old_time & 0xFFFFFFFFu);
    utc.dwHighDateTime = (DWORD)(diff->old_time >> 32);
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) {
        memset(&st, 0, sizeof(st));
    }
    format_delta((int64_t)(diff->new_total - diff->old_total), delta,
                 sizeof(delta) / sizeof(delta[0]));
    format_size(diff->old_total, from, sizeof(from) / sizeof(from[0]));
    format_size(diff->new_total, to, sizeof(to) / sizeof(to[0]));
    fwprintf(out, L"Changes since %04u-%02u-%02u %02u:%02u: %ls (%ls -> %ls)%ls\n",
             (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay, (unsigned)st.wHour,
             (unsigned)st.wMinute, delta, from, to,
             diff->total_uncertain ? L" (some folders could not be fully read)" : L"");
    fwprintf(out, L"  grown %llu, shrunk %llu, added %llu, removed %llu\n",
             (unsigned long long)diff->grown, (unsigned long long)diff->shrunk,
             (unsigned long long)diff->added, (unsigned long long)diff->removed);
    shown = diff->len < limit ? diff->len : limit;
    for (i = 0; i < shown; i++) {
        const HistoryChange *c = &diff->items[i];
        format_delta(c->delta, delta, sizeof(delta) / sizeof(delta[0]));
        fwprintf(out, L"  %12ls  %-7ls  %ls%ls\n", delta, history_kind_name(c->kind), c->rel,
                 c->uncertain ? L" (?)" : L"");
    }
    if (diff->len > shown) {
        fwprintf(out, L"  ... and %llu more folder(s)\n", (unsigned long long)(diff->len - shown));
    }
}
