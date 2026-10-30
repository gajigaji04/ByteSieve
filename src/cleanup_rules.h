#ifndef BYTESIEVE_CLEANUP_RULES_H
#define BYTESIEVE_CLEANUP_RULES_H

#include <stddef.h>
#include <wchar.h>

/*
 * 정리 후보 탐지 규칙. 이름과 경로 문자열만 보며 파일 시스템에 접근하지 않는다.
 * 이름 비교는 Windows 파일 이름 규칙대로 대소문자를 구분하지 않는다.
 */

typedef enum {
    CLEANUP_NONE = 0,
    CLEANUP_NODE_MODULES, /* node_modules */
    CLEANUP_BUILD,        /* dist, build, out */
    CLEANUP_CACHE,        /* .cache, .next, .vite */
    CLEANUP_TEMP,         /* .tmp, .temp 폴더, *.tmp 파일 */
    CLEANUP_LOG,          /* *.log 파일 */
    CLEANUP_CATEGORY_COUNT
} CleanupCategory;

/* 유형 필터 비트. */
#define CLEANUP_MASK(category) (1u << (unsigned)(category))
#define CLEANUP_MASK_ALL                                                          \
    (CLEANUP_MASK(CLEANUP_NODE_MODULES) | CLEANUP_MASK(CLEANUP_BUILD) |           \
     CLEANUP_MASK(CLEANUP_CACHE) | CLEANUP_MASK(CLEANUP_TEMP) | CLEANUP_MASK(CLEANUP_LOG))

/* 폴더 이름이 규칙에 맞으면 유형, 아니면 CLEANUP_NONE. */
CleanupCategory cleanup_classify_dir(const wchar_t *name);

/* 파일 이름이 규칙(*.tmp, *.log)에 맞으면 유형, 아니면 CLEANUP_NONE. */
CleanupCategory cleanup_classify_file(const wchar_t *name);

/*
 * 이름이 흔해서 프로젝트 폴더 안에 있는지로 신뢰도를 매기는 유형인지
 * (node_modules, dist/build/out).
 */
int cleanup_needs_marker(CleanupCategory category);

/* 프로젝트 표시 파일(package.json, CMakeLists.txt, *.csproj 등)인지. */
int cleanup_is_project_marker(const wchar_t *name);

/* 식별자 이름("node_modules", "build", "cache", "temp", "log", "none"). */
const wchar_t *cleanup_category_name(CleanupCategory category);

/*
 * 시스템 보호 경로 목록. 탐색 중 이 경로와 같은 폴더를 만나면 들어가지 않는다.
 * 경로는 확장 접두사 없이, 끝 구분자 없이 저장한다("C:\Windows").
 */
typedef struct {
    wchar_t **paths;
    size_t len;
    size_t cap;
} CleanupProtect;

void cleanup_protect_init(CleanupProtect *p);
void cleanup_protect_free(CleanupProtect *p);

/* 경로 하나를 추가한다(확장 접두사·끝 구분자는 정리한다). 성공 0, 실패 -1. */
int cleanup_protect_add(CleanupProtect *p, const wchar_t *path);

/*
 * 현재 시스템의 보호 경로를 추가한다: Windows 폴더, Program Files(x86 포함), ProgramData.
 * 찾지 못한 항목은 건너뛴다. 추가한 개수를 반환한다.
 */
size_t cleanup_protect_add_system(CleanupProtect *p);

/*
 * dir 바로 아래의 name 폴더가 보호 대상인지.
 * dir 는 확장 경로("\\?\C:\x")여도 된다. 다음 중 하나면 1:
 *   - dir\name 이 목록의 경로와 같다
 *   - dir 가 볼륨 루트이고 name 이 $Recycle.Bin, System Volume Information,
 *     Recovery, Config.Msi 중 하나다
 */
int cleanup_protect_match(const CleanupProtect *p, const wchar_t *dir, const wchar_t *name);

/* path 가 보호 경로와 같거나 그 아래에 있는지(탐지 시작 경로 경고용). */
int cleanup_protect_contains(const CleanupProtect *p, const wchar_t *path);

#endif /* BYTESIEVE_CLEANUP_RULES_H */
