#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "cli.h"
#include "jsonl.h"
#include "scan_history.h"
#include "scan_model.h"
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

/*
 * --track: 직전 스냅숏과 비교하고 새 스냅숏을 저장한다. 비교 결과는 diff 에(직전 스냅숏이 없으면
 * has_previous 0). 저장 위치는 *file 에(malloc). 스캔이 중단·취소됐으면 저장하지 않는다.
 * 기록 관련 실패는 경고만 하고 스캔 결과(종료 코드)에는 영향을 주지 않는다.
 */
static void track_history(const CliOptions *opts, const ScanModel *model, HistoryDiff *diff,
                          int *has_previous, wchar_t **file, int *saved)
{
    wchar_t dir[MAX_PATH * 2];
    HistorySnapshot prev, cur;
    unsigned long err = 0;

    *has_previous = 0;
    *saved = 0;
    *file = NULL;
    if (opts->history_dir != NULL) {
        if (wcslen(opts->history_dir) >= sizeof(dir) / sizeof(dir[0])) {
            fwprintf(stderr, L"warning: --history-dir is too long; history not saved\n");
            return;
        }
        wcscpy_s(dir, sizeof(dir) / sizeof(dir[0]), opts->history_dir);
    } else if (history_default_dir(dir, sizeof(dir) / sizeof(dir[0])) != 0) {
        fwprintf(stderr, L"warning: cannot determine history folder; history not saved\n");
        return;
    }
    *file = history_file_for(dir, opts->target);
    if (*file == NULL) {
        fwprintf(stderr, L"warning: cannot build history file path; history not saved\n");
        return;
    }

    history_snapshot_init(&prev);
    history_snapshot_init(&cur);
    if (history_snapshot_from_model(&cur, model) != 0) {
        /* 대상 폴더를 읽지 못했다(SCAN_FATAL). */
        return;
    }
    if (history_load(&prev, *file) == 0 && history_diff(&prev, &cur, diff) == 0) {
        *has_previous = 1;
    }
    if (model->report.cancelled) {
        fwprintf(stderr, L"warning: scan was cancelled; history not saved\n");
    } else if (history_ensure_dir(dir) != 0 || history_save(&cur, *file, &err) != 0) {
        fwprintf(stderr, L"warning: cannot save history to %ls (Windows error %lu)\n", *file,
                 err != 0 ? err : (unsigned long)GetLastError());
    } else {
        *saved = 1;
    }
    history_snapshot_free(&prev);
    history_snapshot_free(&cur);
}

/* --track 텍스트 모드: 폴더 트리를 만들어야 하므로 모델로 스캔한다. */
static int run_scan_tracked(const CliOptions *opts)
{
    ScanModel model;
    ScanStatus status;
    HistoryDiff diff;
    wchar_t *file;
    int has_previous, saved;

    scan_model_init(&model);
    history_diff_init(&diff);
    status = scan_model_build(&model, opts->target, opts->max_depth, NULL, NULL);
    cli_print_report(stdout, stderr, opts->target, opts->max_depth, &model.report, status);
    track_history(opts, &model, &diff, &has_previous, &file, &saved);
    if (status != SCAN_FATAL) {
        if (has_previous) {
            cli_print_changes(stdout, &diff, CLI_MAX_LISTED_CHANGES);
        } else {
            fwprintf(stdout, L"Changes:        no previous snapshot (baseline)\n");
        }
        if (saved) {
            fwprintf(stdout, L"History:        saved to %ls\n", file);
        }
    }
    free(file);
    history_diff_free(&diff);
    scan_model_free(&model);
    return cli_exit_code(status);
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

    ScanModel model;
    const ScanReport *final_report = &report;

    scan_report_init(&report);
    scan_model_init(&model);
    jsonl_buf_init(&buf);

    jsonl_start(&buf, opts->target);
    jsonl_buf_flush(&buf, stdout);

    if (opts->track) {
        /* 변화 추적에는 폴더 트리가 필요하다(진행 이벤트는 내지 않는다). */
        status = scan_model_build(&model, opts->target, opts->max_depth, NULL, NULL);
        final_report = &model.report;
    } else {
        progress.buf = &buf;
        progress.report = &report;
        progress.out = stdout;
        progress.interval = JSONL_PROGRESS_INTERVAL;
        progress.seen = 0;
        status = scan_tree(opts->target, opts->max_depth, &report,
                           jsonl_progress_visit, &progress);
    }
    code = cli_exit_code(status);

    jsonl_result(&buf, opts->target, final_report);
    if (opts->track && status != SCAN_FATAL) {
        HistoryDiff diff;
        wchar_t *file;
        int has_previous, saved;
        history_diff_init(&diff);
        track_history(opts, &model, &diff, &has_previous, &file, &saved);
        jsonl_changes(&buf, has_previous ? &diff : NULL, saved ? file : NULL,
                      CLI_MAX_LISTED_CHANGES * 5);
        free(file);
        history_diff_free(&diff);
    }
    jsonl_complete(&buf, status, code);
    if (!jsonl_buf_flush(&buf, stdout)) {
        fwprintf(stderr, L"error: failed to write JSON output\n");
        code = EXIT_FATAL;
    }

    jsonl_buf_free(&buf);
    scan_report_free(&report);
    scan_model_free(&model);
    return code;
}

int wmain(int argc, wchar_t **argv)
{
    CliOptions opts;
    wchar_t err[512];
    CliParseResult parsed;

    set_wide_mode(stderr);
    parsed = cli_parse_args(argc, argv, &opts, err, sizeof(err) / sizeof(err[0]));
    if (parsed == CLI_PARSE_OK && opts.threads >= 0) {
        scan_set_threads((unsigned)opts.threads);
    }

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
        return opts.track ? run_scan_tracked(&opts) : run_scan(&opts);
    }
}
