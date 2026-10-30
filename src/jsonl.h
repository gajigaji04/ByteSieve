#ifndef BYTESIEVE_JSONL_H
#define BYTESIEVE_JSONL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

#include "scan_history.h"
#include "scanner.h"

/*
 * JSON Lines 이벤트 출력. 이벤트 하나는 '\n' 으로 끝나는 UTF-8 JSON 객체 한 줄이다.
 *   {"event":"start","target":...}
 *   {"event":"progress","files":..,"dirs":..,"bytes":..,"errors":..}
 *   {"event":"result","target":...,"files":..,"dirs":..,"bytes":..,"alloc_bytes":..,
 *    "disk":{"slack":..,"compressed_saved":..,"sparse_saved":..,"cloud_saved":..,
 *            "other_saved":..,"unknown_files":..},...,"errors":[{"path":..,"code":..}]}
 *   {"event":"changes","previous":true|false,"since":<FILETIME>,"old_bytes":..,"new_bytes":..,
 *    "items":[{"path":..,"kind":"grown|shrunk|added|removed","old":..,"new":..,
 *              "delta":..,"uncertain":true|false}],"more":..,"saved_to":..}   (--track)
 *   {"event":"complete","status":"complete|partial|failed","exit_code":..}
 */

/* 이벤트를 쌓는 버퍼. 메모리가 부족하면 failed 를 세우고 이후 추가를 무시한다. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int failed;
} JsonBuf;

void jsonl_buf_init(JsonBuf *buf);
void jsonl_buf_free(JsonBuf *buf);

/* 쌓인 내용을 out 에 쓰고 버퍼를 비운다. 실패하면 0. */
int jsonl_buf_flush(JsonBuf *buf, FILE *out);

/*
 * UTF-16 문자열을 따옴표로 감싼 JSON 문자열(UTF-8)로 추가한다.
 * '"', '\\', 제어 문자는 이스케이프하고, 짝이 없는 서로게이트는 \uXXXX 로 보존한다.
 * s 가 NULL 이면 null 을 추가한다.
 */
void jsonl_append_string(JsonBuf *buf, const wchar_t *s);

void jsonl_start(JsonBuf *buf, const wchar_t *target);
void jsonl_progress(JsonBuf *buf, const ScanReport *report);
void jsonl_result(JsonBuf *buf, const wchar_t *target, const ScanReport *report);
void jsonl_complete(JsonBuf *buf, ScanStatus status, int exit_code);

/*
 * 변화 추적 결과. diff 가 NULL 이면 직전 스냅숏이 없는 것("previous":false, items 없음).
 * saved_to 가 NULL 이면 저장하지 못한 것(null). 항목은 최대 limit 개, 나머지는 "more" 로 센다.
 */
void jsonl_changes(JsonBuf *buf, const HistoryDiff *diff, const wchar_t *saved_to, size_t limit);

/* scan_directory 의 visit 콜백으로 써서 interval 항목마다 progress 이벤트를 낸다. */
typedef struct {
    JsonBuf *buf;
    const ScanReport *report;
    FILE *out;         /* NULL 이 아니면 이벤트마다 flush 한다. */
    uint64_t interval; /* 0 이면 progress 를 내지 않는다. */
    uint64_t seen;
} JsonlProgress;

void jsonl_progress_visit(const ScanEntry *entry, void *ctx);

#endif /* BYTESIEVE_JSONL_H */
