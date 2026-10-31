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
    /*
     * 디스크 사용량(파일 할당 크기) 분석. 할당 크기는 폴더 열거 결과(AllocationSize)에서 얻으며
     * 파일을 열지 않는다. 압축·스파스 파일만 GetCompressedFileSizeW 로 실제 점유량을 다시 구한다.
     * 할당 크기를 얻지 못한 파일(대체 열거)은 논리 크기로 alloc_bytes 에 더한다.
     */
    uint64_t alloc_bytes;          /* 파일 할당 크기의 합 */
    uint64_t alloc_unknown_files;  /* 할당 크기를 얻지 못한 파일 */
    uint64_t slack_bytes;          /* 할당 > 논리 인 파일의 차이 합(클러스터 단위 반올림 낭비) */
    uint64_t compressed_files;     /* NTFS 압축으로 할당 < 논리 인 파일 */
    uint64_t compressed_saved_bytes;
    uint64_t sparse_files;         /* 스파스 파일 */
    uint64_t sparse_saved_bytes;
    uint64_t cloud_files;          /* 클라우드 전용·오프라인(OneDrive 자리표시자 등) */
    uint64_t cloud_saved_bytes;
    uint64_t other_saved_files;    /* 그 밖에 할당 < 논리(MFT 상주 작은 파일, 중복 제거 등) */
    uint64_t other_saved_bytes;
    uint32_t cluster_bytes;        /* 첫 대상 볼륨의 클러스터 크기(모르면 0) */
    uint64_t reparse_count; /* 하위 탐색에서 제외한 재분석 지점 디렉터리 */
    uint64_t error_count;
    uint64_t skipped_dir_count; /* 열거하지 못해 건너뛴 디렉터리(하위 트리 포함 누락) */
    uint64_t excluded_dir_count; /* should_skip_dir 훅이 제외한 디렉터리(오류 아님) */
    int aborted;                /* SCAN_FATAL 로 중단된 스캔이 있었으면 1 */
    int cancelled;              /* should_cancel 로 도중에 멈춘 스캔이 있었으면 1(상태는 SCAN_PARTIAL) */
    /*
     * 하드 링크 중복 보정(정밀 스캔, SCAN_OPT_HARDLINKS 일 때만 채운다). 위의 file_count·
     * total_bytes·alloc_bytes 는 기본 스캔과 똑같이 경로마다 센 값이고, 아래는 그중 같은 실제 파일을
     * 다른 경로에서 이미 센 몫이다. 따라서
     *   중복 제외 용량      = total_bytes - hardlink_dup_bytes
     *   실제 디스크 사용량  = alloc_bytes - hardlink_dup_alloc
     */
    int hardlinks_checked;          /* 정밀 스캔으로 하드 링크를 확인했으면 1 */
    int hardlinks_unsupported;      /* 파일 시스템이 하드 링크를 지원하지 않아 확인할 것이 없음 */
    uint64_t hardlink_dup_files;    /* 이미 다른 경로로 센 실제 파일(하드 링크 중복 경로) 수 */
    uint64_t hardlink_dup_bytes;    /* 그 논리 크기 합 */
    uint64_t hardlink_dup_alloc;    /* 그 할당 크기 합 */
    uint64_t hardlink_unknown_files; /* 파일 ID를 얻지 못해 중복인지 모르는 파일(경로마다 셈) */
    /*
     * 병렬 열거 진단(scan_tree_ex 한 번 기준, 여러 번 부르면 최대값·합계가 쌓인다). 스캐너 내부의
     * 폴더 작업만 센다(결과 모델·훅이 쓰는 메모리 제외). 성능 측정·메모리 상한 테스트용.
     */
    uint64_t peak_tasks;          /* 동시에 살아 있던 폴더 작업 수의 최대 */
    uint64_t peak_task_bytes;     /* 그 작업들(구조·경로·열거 결과·자식 목록)이 붙잡은 메모리의 최대 */
    uint64_t prefetched_dirs;     /* 작업 스레드가 미리 열거한 폴더 */
    uint64_t self_fetched_dirs;   /* 호출 스레드가 직접 열거한 폴더 */
    uint64_t prefetch_waits;      /* 호출 스레드가 열거 중인 작업 스레드를 기다린 횟수 */
    uint64_t discarded_dirs;      /* 만들었다가 쓰지 않고 버린 폴더 작업(제외·취소) */
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
    uint64_t alloc_size; /* 디스크 할당 크기. alloc_known 이 0 이면 size 와 같다(디렉터리는 0) */
    int alloc_known;     /* 할당 크기를 얻었으면 1 */
    int hardlink_dup;    /* 정밀 스캔에서 같은 실제 파일을 이미 다른 경로로 셌으면 1(하드 링크) */
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
 * 파일 하나의 논리 크기·할당 크기를 report 의 디스크 사용량 분석에 더한다(file_count·
 * total_bytes 는 건드리지 않는다). 할당 < 논리 이면 속성으로 원인을 나눈다:
 * 클라우드 전용(OFFLINE·RECALL_ON_*) > 압축 > 스파스 > 기타 순.
 */
void scan_report_account_alloc(ScanReport *report, uint64_t size, uint64_t alloc,
                               int alloc_known, uint32_t attributes);

/*
 * 폴더 열거를 미리 해 두는 병렬 작업 스레드 수(프로세스 전체 기본값).
 * 0 = 자동(논리 프로세서 수, 최대 SCAN_MAX_THREADS), 1 = 병렬 없이 호출 스레드에서만 열거.
 * 훅은 언제나 scan_tree_ex 를 부른 스레드에서, 병렬 여부와 상관없이 같은 순서로 호출된다.
 */
#define SCAN_MAX_THREADS 8
void scan_set_threads(unsigned threads);

/* 실제로 쓸 작업 스레드 수(1 이면 병렬 없음). */
unsigned scan_effective_threads(void);

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
 * 내려갈 수 없는 하위 디렉터리(경로 초과·메모리 부족)는 begin/end 없이 끝난다. 경로 초과는 부모를
 * 처리할 때, 작업을 만들 메모리가 없는 경우는 그 하위 디렉터리 차례에 오류로 기록한다.
 *
 * 열거: 폴더 핸들로 GetFileInformationByHandleEx(FileFullDirectoryInfo)를 써서 이름·크기·
 * 속성·할당 크기를 한 번에 받는다. 지원하지 않는 파일 시스템에서는 FindFirstFileExW 로
 * 대신한다(할당 크기 모름). 작업 스레드가 곧 필요한 폴더를 미리 열거해 두지만(미리 열거한 결과는
 * 폴더 1024개·2 MiB 안팎으로 제한), 훅 호출 순서(깊이 우선, 폴더 안은 열거 순서)는 병렬이 아닐 때와
 * 같다. 내려가기를 기다리는 하위 디렉터리마다 작업을 미리 만들지 않으므로 폴더가 아무리 넓어도
 * 스캐너가 붙잡는 작업 수는 상한 안에 머문다(ScanReport.peak_tasks).
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

/* scan_tree_opts 의 options 비트. */
enum {
    /*
     * 정밀 스캔(하드 링크 중복 보정). 폴더 열거로 파일마다 파일 ID(128비트)를 받고(파일을 열지 않음)
     * 폴더 핸들로 볼륨 일련번호를 구해, (볼륨, 파일 ID)가 같은 파일은 같은 실제 파일로 본다.
     *   - 처음 만난 경로(훅 호출 순서, 작업 스레드 수와 무관)만 실제 파일로 세고, 그 뒤 경로는
     *     ScanEntry.hardlink_dup = 1 로 알리며 report 의 hardlink_dup_* 에 더한다.
     *   - 경로마다 세는 기본 합계(file_count·total_bytes·alloc_bytes)는 그대로다.
     *   - 탐색 시작 볼륨이 하드 링크를 지원하지 않으면(FAT 등) 확인하지 않고
     *     hardlinks_unsupported 를 1 로 둔다. 파일 ID가 0 이거나 얻지 못한 파일은 중복으로 보지 않고
     *     hardlink_unknown_files 로 센다.
     *   - 지금까지 본 파일 ID를 모두 기억하므로 파일 하나에 약 40바이트를 더 쓴다. 메모리가 모자라면
     *     그 뒤 파일은 hardlink_unknown_files 로 센다.
     */
    SCAN_OPT_HARDLINKS = 1u << 0
};

/* scan_tree_ex 에 옵션(SCAN_OPT_*)을 더한 판. options 0 은 scan_tree_ex 와 같다. */
ScanStatus scan_tree_opts(const wchar_t *dir, uint32_t max_depth, uintptr_t root_id,
                          ScanReport *report, const ScanHooks *hooks, void *ctx,
                          unsigned options);

#endif /* BYTESIEVE_SCANNER_H */
