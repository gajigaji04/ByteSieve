#ifndef BYTESIEVE_SCANNER_H
#define BYTESIEVE_SCANNER_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef enum {
    SCAN_COMPLETE = 0, /* 오류 없이 끝까지 열거했다. */
    SCAN_PARTIAL = 1,  /* 열거 도중(하위 디렉터리 포함) 오류가 있었지만 읽은 항목은 집계했다. */
    SCAN_FATAL = 2,    /* 대상 자체를 열지 못해 스캔을 중단했다(인자 오류 포함). */
    SCAN_FAILED = SCAN_FATAL /* 이전 이름(호환용). */
} ScanStatus;

/* GetLastError() 값을 처리 방식에 따라 나눈 분류. */
typedef enum {
    SCAN_ERR_OTHER = 0,
    SCAN_ERR_ACCESS_DENIED, /* 권한 없음, 공유 위반 */
    SCAN_ERR_NOT_FOUND,     /* 경로·드라이브·네트워크 경로 없음(스캔 중 삭제 포함) */
    SCAN_ERR_INVALID_PATH,  /* 잘못된 이름, 디렉터리가 아님, 잘못된 인자 */
    SCAN_ERR_PATH_TOO_LONG, /* 확장 경로 한도(PATH_MAX_WIDE) 초과 */
    SCAN_ERR_NO_MEMORY,
    SCAN_ERR_KIND_COUNT /* 종류 개수(배열 크기용) */
} ScanErrorKind;

/* 기록된 오류 하나. path 는 표시용(확장 접두사 제거) 경로이며 NULL 일 수 있다. */
typedef struct {
    wchar_t *path;
    uint32_t code; /* GetLastError() 값 */
    ScanErrorKind kind;
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
    uint64_t skipped_dir_count; /* 열거하지 못해 건너뛴 디렉터리(하위 트리 포함 누락) */
    uint64_t excluded_dir_count; /* should_skip_dir 훅이 제외한 디렉터리(오류 아님) */
    int aborted;                /* SCAN_FATAL 로 중단된 스캔이 있었으면 1 */
    int cancelled;              /* should_cancel 로 도중에 멈춘 스캔이 있었으면 1(상태는 SCAN_PARTIAL) */
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

ScanErrorKind scan_classify_error(uint32_t code);

/* 표시·직렬화용 소문자 이름("complete", "access_denied" 등). */
const wchar_t *scan_status_name(ScanStatus status);
const wchar_t *scan_error_kind_name(ScanErrorKind kind);

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

/* scan_tree 의 max_depth 로 깊이 제한 없음을 나타낸다. */
#define SCAN_DEPTH_UNLIMITED UINT32_MAX

/*
 * dir 아래 모든 하위 디렉터리를 동적 스택 기반 DFS 로 탐색해 report 에 누적한다.
 * dir 자신의 항목은 깊이 0 이며, 깊이가 max_depth 인 디렉터리까지 열거한다
 * (max_depth 0 은 scan_directory 와 같다). 재분석 지점 디렉터리는 집계만 하고
 * 하위로 들어가지 않는다.
 *
 * 상태:
 *   SCAN_COMPLETE  오류 없음.
 *   SCAN_PARTIAL   일부 디렉터리를 열지 못했거나(skipped_dir_count 증가) 열거 도중
 *                  오류가 있었다. 나머지 디렉터리는 끝까지 탐색한다.
 *   SCAN_FATAL     dir 자체가 잘못됐거나 열 수 없다. 아무것도 집계하지 않고
 *                  report->aborted 를 1 로 만든다(report 가 NULL 이면 기록 없이 반환).
 */
ScanStatus scan_tree(const wchar_t *dir, uint32_t max_depth, ScanReport *report,
                     scan_visit_fn visit, void *ctx);

/* scan_tree_ex 가 디렉터리 하나를 열거하기 전후에 넘기는 정보(콜백 호출 동안만 유효). */
typedef struct {
    uintptr_t id;        /* 이 디렉터리에 대해 on_entry 가 돌려준 값(루트는 root_id) */
    const wchar_t *path; /* 확장 경로("\\?\...") */
    uint32_t depth;      /* 루트 0 */
    size_t pending;      /* 아직 열거하지 않고 스택에 남은 디렉터리 수 */
} ScanDirInfo;

/*
 * 트리 구성·진행률용 훅. 모든 필드는 NULL 일 수 있다.
 *   on_entry      항목마다 호출. parent 는 그 항목을 담은 디렉터리의 id.
 *                 will_descend 가 1 이면(재분석 지점·깊이 제한이 아님) 반환값이
 *                 이 하위 디렉터리의 id 가 된다. 그 밖의 경우 반환값은 무시한다.
 *   on_dir_begin  디렉터리 열거 시작 직전.
 *   on_dir_end    열거 종료. result 는 SCAN_COMPLETE, SCAN_PARTIAL(도중 오류),
 *                 SCAN_FATAL(열지 못함, 항목 없음) 중 하나.
 *   should_cancel 디렉터리를 열거하기 전과 열거 도중(SCAN_CANCEL_CHECK_INTERVAL 항목마다)
 *                 호출한다. 0 이 아니면 탐색을 멈추고 SCAN_PARTIAL 을 반환하며
 *                 report->cancelled 를 1 로, 열거하지 못한 디렉터리를 skipped_dir_count 에
 *                 더한다. 다른 스레드에서 멈추게 할 때 쓴다.
 *   should_skip_dir
 *                 하위 탐색 대상 디렉터리마다 on_entry 보다 먼저 호출한다. parent 는 그 항목을
 *                 담은 디렉터리. 0 이 아니면 들어가지 않고(on_entry 의 will_descend 가 0)
 *                 report->excluded_dir_count 를 늘린다. 오류로 치지 않는다.
 * 스택에 넣지 못한 하위 디렉터리(경로 초과·메모리 부족)는 begin/end 없이 끝난다.
 */
typedef struct {
    uintptr_t (*on_entry)(const ScanEntry *entry, uintptr_t parent,
                          int will_descend, void *ctx);
    void (*on_dir_begin)(const ScanDirInfo *dir, void *ctx);
    void (*on_dir_end)(const ScanDirInfo *dir, ScanStatus result, void *ctx);
    int (*should_cancel)(void *ctx);
    int (*should_skip_dir)(const ScanEntry *entry, const ScanDirInfo *parent, void *ctx);
} ScanHooks;

/* 한 디렉터리 안에서 should_cancel 을 확인하는 항목 간격. */
#define SCAN_CANCEL_CHECK_INTERVAL 256

/* scan_tree 와 같지만 디렉터리 단위 훅을 받는다. hooks 가 NULL 이면 집계만 한다. */
ScanStatus scan_tree_ex(const wchar_t *dir, uint32_t max_depth, uintptr_t root_id,
                        ScanReport *report, const ScanHooks *hooks, void *ctx);

#endif /* BYTESIEVE_SCANNER_H */
