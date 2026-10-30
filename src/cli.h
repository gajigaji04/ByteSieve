#ifndef BYTESIEVE_CLI_H
#define BYTESIEVE_CLI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "scanner.h"

enum {
    EXIT_COMPLETE = 0, /* 오류 없이 완료 */
    EXIT_PARTIAL = 1,  /* 일부 디렉터리를 읽지 못함 */
    EXIT_FATAL = 2     /* 스캔 불가 또는 잘못된 인자 */
};

/* 오류 요약에서 개별 오류를 이만큼만 나열한다. */
#define CLI_MAX_LISTED_ERRORS 20

typedef enum {
    CLI_PARSE_OK = 0,
    CLI_PARSE_HELP,
    CLI_PARSE_ERROR
} CliParseResult;

typedef struct {
    const wchar_t *target; /* argv 안의 문자열(복사하지 않음) */
    uint32_t max_depth;
} CliOptions;

/*
 * 인자를 해석한다. 옵션과 대상은 순서와 무관하다.
 *   -h, --help                 도움말(다른 인자보다 우선)
 *   --max-depth N, --max-depth=N
 *   --                         이후 인자는 모두 대상으로 취급
 * CLI_PARSE_ERROR 이면 err 에 사람이 읽을 메시지를 쓴다.
 */
CliParseResult cli_parse_args(int argc, wchar_t **argv, CliOptions *opts,
                              wchar_t *err, size_t err_len);

int cli_exit_code(ScanStatus status);

/* "512 bytes", "1.50 KiB (1536 bytes)" 형태로 쓴다. */
void cli_format_size(uint64_t bytes, wchar_t *buf, size_t buf_len);

/* errors 에 저장된 오류를 종류별로 센다(counts 길이는 SCAN_ERR_KIND_COUNT). */
void cli_count_error_kinds(const ScanReport *report, uint64_t *counts);

void cli_print_usage(FILE *out);

/* 집계·상태·오류 요약을 out 에, 개별 오류 목록을 err 에 쓴다. */
void cli_print_report(FILE *out, FILE *err, const wchar_t *target,
                      uint32_t max_depth, const ScanReport *report,
                      ScanStatus status);

#endif /* BYTESIEVE_CLI_H */
