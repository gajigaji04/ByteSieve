#ifndef BYTESIEVE_SCANNER_H
#define BYTESIEVE_SCANNER_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef enum {
    SCAN_COMPLETE = 0, /* 오류 없이 끝까지 열거했다. */
    SCAN_PARTIAL = 1,  /* 열거 도중 오류가 있었지만 읽은 항목은 집계했다. */
    SCAN_FAILED = 2    /* 디렉터리를 열지 못했다(인자 오류 포함). */
} ScanStatus;

/* 기록된 오류 하나. path 는 표시용(확장 접두사 제거) 경로이며 NULL 일 수 있다. */
typedef struct {
    wchar_t *path;
    uint32_t code; /* GetLastError() 값 */
} ScanError;

/*
 * 누적 집계. scan_directory 를 여러 번 호출하면 결과가 더해진다.
 * 오류가 나도 스캔은 계속되며, 오류는 errors 에 기록된다.
 * error_count 는 메모리 부족으로 errors 에 저장하지 못한 오류도 센다.
 */
typedef struct {
    uint64_t file_count;
    uint64_t dir_count;
    uint64_t total_bytes;   /* 파일 논리 크기의 합 */
    uint64_t reparse_count; /* 하위 탐색에서 제외한 재분석 지점 디렉터리 */
    uint64_t error_count;
    ScanError *errors;
    size_t errors_len;
    size_t errors_cap;
} ScanReport;

/* 열거된 항목 하나(콜백 호출 동안만 유효). */
typedef struct {
    const wchar_t *name;
    uint64_t size;       /* 디렉터리는 0 */
    uint32_t attributes; /* FILE_ATTRIBUTE_* */
    int is_dir;
    int is_reparse;
    int descend;         /* 하위 탐색 대상인지(디렉터리이면서 재분석 지점이 아님) */
} ScanEntry;

typedef void (*scan_visit_fn)(const ScanEntry *entry, void *ctx);

void scan_report_init(ScanReport *report);
void scan_report_free(ScanReport *report);

/* WIN32_FIND_DATAW 의 nFileSizeHigh/Low 를 64비트 크기로 합친다. */
uint64_t scan_combine_size(uint32_t high, uint32_t low);

/* 재분석 지점(정션·심볼릭 링크 등)이 아닌 디렉터리만 하위 탐색 대상이다. */
int scan_should_descend(uint32_t attributes);

/*
 * dir 바로 아래의 파일·폴더를 열거해 report 에 누적한다("."/".." 제외, 비재귀).
 * visit 이 NULL 이 아니면 각 항목마다 호출한다.
 * 긴 경로를 위해 내부적으로 "\\?\" 확장 경로를 사용한다.
 */
ScanStatus scan_directory(const wchar_t *dir, ScanReport *report,
                          scan_visit_fn visit, void *ctx);

#endif /* BYTESIEVE_SCANNER_H */
