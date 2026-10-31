#ifndef BYTESIEVE_DEV_CACHE_H
#define BYTESIEVE_DEV_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include "cleanup_rules.h"
#include "cleanup_scan.h"
#include "cleanup_trash.h"
#include "scanner.h"

/*
 * 공용 개발 캐시 탐지. 프로젝트 밖에서 여러 프로젝트가 함께 쓰는 패키지 관리자·빌드 도구의
 * 캐시 폴더를 사용자 환경(환경 변수·설정 파일)에 맞게 찾아 용량·파일 수를 센다.
 * 읽기만 하며 아무것도 지우지 않는다(정리는 cleanup_trash 로 휴지통에 옮긴다).
 *
 * 찾는 위치(앞에서부터 먼저 정해지는 것을 쓴다. 기본 위치의 USERPROFILE·LOCALAPPDATA·APPDATA 는
 * 환경 변수, 없으면 알려진 폴더):
 *   npm          npm_config_cache → .npmrc(NPM_CONFIG_USERCONFIG 또는 %USERPROFILE%\.npmrc)의
 *                cache= → %LOCALAPPDATA%\npm-cache
 *   pip          PIP_CACHE_DIR → pip.ini 의 [global] cache-dir(PIP_CONFIG_FILE,
 *                %APPDATA%\pip\pip.ini, %USERPROFILE%\pip\pip.ini 순) → %LOCALAPPDATA%\pip\Cache
 *   Gradle       (GRADLE_USER_HOME 또는 %USERPROFILE%\.gradle)\caches, \wrapper\dists
 *   Cargo        (CARGO_HOME 또는 %USERPROFILE%\.cargo)\registry, \git
 *   NuGet        NUGET_PACKAGES → %APPDATA%\NuGet\NuGet.Config 의 globalPackagesFolder →
 *                %USERPROFILE%\.nuget\packages
 *                NUGET_HTTP_CACHE_PATH → %LOCALAPPDATA%\NuGet\v3-cache, \http-cache(있는 것 모두)
 *   설정 값의 %VAR%·${VAR} 는 환경 변수로 바꾸고, 상대 경로는 설정 파일 폴더 기준으로 본다.
 *
 * 안전 규칙:
 *   - 도구 폴더 자체(.gradle·.cargo·.nuget 등, 설정·인증 정보·설치한 도구가 있음)는 항목이 되지
 *     않는다. 그 안의 캐시 하위 폴더만 항목이다(.cargo\bin, gradle.properties 는 건드리지 않음).
 *   - 환경 변수·설정 파일이 가리킨 폴더는 그 도구의 캐시 구조(예: npm 은 _cacache)가 보일 때만
 *     FOUND 이고, 아니면 UNRECOGNIZED 로 정리 대상에서 뺀다(잘못 설정된 경로 보호).
 *   - 링크(정션·심볼릭 링크)인 캐시 폴더는 LINK 로 정리하지 않는다.
 *   - 각 항목의 탐지 경로(home)는 캐시 폴더의 부모다. 정리할 때 cleanup_guard_check 가
 *     중요 경로(사용자 프로필·문서 등)·시스템 경로 여부를 다시 판정한다.
 */

typedef enum {
    DEV_CACHE_NPM = 0,
    DEV_CACHE_PIP,
    DEV_CACHE_GRADLE,         /* Gradle 의존성·빌드 캐시(caches) */
    DEV_CACHE_GRADLE_WRAPPER, /* Gradle 래퍼 배포판(wrapper\dists) */
    DEV_CACHE_CARGO_REGISTRY, /* Cargo 레지스트리(색인·압축 파일·소스) */
    DEV_CACHE_CARGO_GIT,      /* Cargo git 의존성 */
    DEV_CACHE_NUGET_PACKAGES, /* NuGet 전역 패키지 폴더 */
    DEV_CACHE_NUGET_HTTP,     /* NuGet HTTP 캐시 */
    DEV_CACHE_KIND_COUNT
} DevCacheKind;

typedef enum {
    DEV_CACHE_FOUND = 0,    /* 캐시 폴더가 있음(정리 후보) */
    DEV_CACHE_NOT_FOUND,    /* 경로에 아무것도 없음 */
    DEV_CACHE_NOT_DIR,      /* 폴더가 아님 */
    DEV_CACHE_LINK,         /* 링크(정션·심볼릭 링크)라 정리하지 않음 */
    DEV_CACHE_UNRECOGNIZED, /* 설정한 경로에 캐시 구조가 보이지 않아 정리하지 않음 */
    DEV_CACHE_NO_PATH,      /* 위치를 정할 수 없음(사용자 폴더를 모름 등) */
    DEV_CACHE_STATUS_COUNT
} DevCacheStatus;

/* 화면용 설명("있음", "없음", "링크라 정리하지 않음" 등). */
const wchar_t *dev_cache_status_text(DevCacheStatus status);
/* 식별자 이름("found", "not_found", "not_dir", "link", "unrecognized", "no_path"). */
const wchar_t *dev_cache_status_name(DevCacheStatus status);

/* 위치를 정한 근거. */
typedef enum {
    DEV_CACHE_SRC_DEFAULT = 0, /* 도구의 기본 위치 */
    DEV_CACHE_SRC_ENV,         /* 환경 변수(origin 에 이름) */
    DEV_CACHE_SRC_CONFIG       /* 설정 파일(origin 에 파일 경로) */
} DevCacheSource;

/* 캐시 종류마다 고정된 정보. */
typedef struct {
    const wchar_t *id;    /* 식별자("npm", "gradle_wrapper" 등) */
    const wchar_t *tool;  /* 도구 이름("npm", "Gradle" 등) */
    const wchar_t *label; /* 캐시 이름("패키지 캐시", "래퍼 배포판" 등) */
    CleanupEco eco;
    int caution;          /* 1 이면 정리 뒤 오프라인 빌드가 안 되는 등 더 주의할 것 */
    const wchar_t *note;  /* 정리하면 어떻게 되는지·주의 사항(한 문단) */
} DevCacheKindInfo;

const DevCacheKindInfo *dev_cache_kind_info(DevCacheKind kind);

typedef struct {
    DevCacheKind kind;
    DevCacheStatus status;
    DevCacheSource source;
    wchar_t *origin; /* 환경 변수 이름 또는 설정 파일 경로(DEFAULT 이면 NULL) */
    wchar_t *path;   /* 캐시 폴더(표시용 전체 경로, NO_PATH 이면 NULL) */
    wchar_t *home;   /* 캐시 폴더의 부모(정리 판정의 탐지 경로, NO_PATH 이면 NULL) */
    int measured;    /* 용량을 셌으면 1 */
    uint64_t bytes;  /* 하위 전체 파일의 논리 크기 */
    uint64_t files;
    uint64_t dirs;
    uint64_t errors;
    int incomplete;  /* 다 세지 못함(읽기 오류·취소) — 실제 용량이 더 클 수 있고 정리하지 않는다 */
} DevCache;

typedef struct {
    DevCache *items; /* 종류 순서(한 종류에 여러 개일 수 있음) */
    size_t len;
    size_t cap;
} DevCacheList;

void dev_cache_list_init(DevCacheList *list);
void dev_cache_list_free(DevCacheList *list);

/*
 * 캐시 위치를 찾아 list 를 채운다(이전 내용은 지운다). 용량은 세지 않는다(measured = 0).
 * 모든 종류가 적어도 한 항목으로 나온다(없으면 NOT_FOUND·NO_PATH). 메모리가 부족하면 -1.
 */
int dev_cache_detect(DevCacheList *list);

typedef int (*dev_cache_cancel_fn)(void *ctx);

/*
 * FOUND 항목 하나의 용량·파일 수를 센다(링크는 따라가지 않음). cancel 이 0 이 아닌 값을 돌려주면
 * 멈추고 incomplete 를 1 로 둔다. cancel 은 NULL 일 수 있다.
 */
void dev_cache_measure(DevCache *cache, dev_cache_cancel_fn cancel, void *ctx);

/*
 * 정리용 항목으로 바꾼다(path 는 cache->path 를 빌려 쓰므로 cache 보다 오래 쓰지 않는다).
 * 폴더·CLEANUP_CACHE·종류의 개발 환경이며, 다 세지 못했으면 CLEANUP_ITEM_INCOMPLETE.
 */
void dev_cache_to_item(const DevCache *cache, CleanupItem *item);

/*
 * 정리할 수 있는지 판정한다: FOUND 이고 용량을 셌으며 cleanup_guard_check(home 기준)를
 * 통과해야 한다. 가능하면 CLEANUP_TRASH_PENDING, 아니면 거부 사유:
 *   없음·폴더 아님·위치 모름 → MISSING, 링크·캐시 구조 아님 → CHANGED,
 *   아직 세지 않았거나 다 세지 못함 → DENY_INCOMPLETE, 그 밖에는 cleanup_guard_check 의 DENY_*.
 */
CleanupTrashStatus dev_cache_check(const CleanupGuard *guard, const DevCache *cache);

/* ---- 작업 스레드(탐지 + 용량 세기) ---- */

typedef struct DevCacheJob DevCacheJob;

/* finished 가 1 이면 마지막 호출이며 이후 dev_cache_job_finish 가 바로 반환된다. */
typedef void (*dev_cache_notify_fn)(int finished, void *ctx);

typedef struct {
    size_t total;            /* 용량을 셀 FOUND 항목 수(탐지 전에는 0) */
    size_t done;             /* 다 센 항목 수 */
    DevCacheKind current;    /* 지금 세는 종류(done < total 일 때) */
    uint64_t bytes;          /* 지금까지 센 용량(끝난 항목 합) */
    uint64_t elapsed_ms;
    int cancel_requested;
} DevCacheJobSnapshot;

/*
 * 탐지와 용량 세기를 새 스레드에서 시작한다. notify 는 항목 하나를 마칠 때마다, 끝나면 반드시
 * 한 번 호출된다(작업 스레드에서, 막히지 않아야 함). 실패하면 NULL.
 */
DevCacheJob *dev_cache_job_start(dev_cache_notify_fn notify, void *ctx);

/* 남은 항목을 세지 않게 한다(세던 항목은 incomplete). 어느 스레드에서든 된다. */
void dev_cache_job_cancel(DevCacheJob *job);

void dev_cache_job_progress(DevCacheJob *job, DevCacheJobSnapshot *out);

/*
 * 스레드가 끝날 때까지 기다린 뒤 결과를 out 으로 옮기고(out 의 이전 내용은 해제) job 을 해제한다.
 * out 이 NULL 이면 결과를 버린다. 취소됐으면 1, 아니면 0.
 */
int dev_cache_job_finish(DevCacheJob *job, DevCacheList *out);

#endif /* BYTESIEVE_DEV_CACHE_H */
