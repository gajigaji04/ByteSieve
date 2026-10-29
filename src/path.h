#ifndef BYTESIEVE_PATH_H
#define BYTESIEVE_PATH_H

#include <stddef.h>
#include <wchar.h>

/* Windows 확장 경로(\\?\)의 최대 길이(문자 수, 종료 NUL 제외). */
#define PATH_MAX_WIDE 32767

/*
 * 반환되는 wchar_t * 는 모두 malloc으로 할당되며 호출자가 free 해야 한다.
 * 실패(NULL 인자, 메모리 부족, 길이 초과, API 실패) 시 NULL을 반환한다.
 */

/* '\\' 또는 '/' 인지 검사한다. */
int path_is_separator(wchar_t c);

/* "\\?\" 로 시작하는 확장 경로인지 검사한다. */
int path_is_extended(const wchar_t *path);

/* "\\server\share" 형태의 UNC 경로인지 검사한다("\\?\UNC\..." 포함). */
int path_is_unc(const wchar_t *path);

/* 문자열을 복제한다. */
wchar_t *path_dup(const wchar_t *path);

/*
 * base 와 name 을 구분자 하나로 결합한다.
 * base 가 구분자로 끝나거나 "C:" 처럼 드라이브만 있으면 구분자를 추가하지 않는다.
 * name 앞의 구분자는 제거한다. 결과가 PATH_MAX_WIDE 를 넘으면 NULL.
 */
wchar_t *path_join(const wchar_t *base, const wchar_t *name);

/* 상대 경로·'/'·"."/".." 를 정규화한 절대 경로를 반환한다(GetFullPathNameW). */
wchar_t *path_full(const wchar_t *path);

/*
 * 긴 경로 API 용 확장 경로로 변환한다.
 *   C:\dir           -> \\?\C:\dir
 *   \\server\share\x -> \\?\UNC\server\share\x
 * 이미 "\\?\" 또는 "\\.\" 로 시작하면 그대로 복제한다.
 */
wchar_t *path_to_extended(const wchar_t *path);

/* 출력용으로 확장 접두사를 제거한다(path_to_extended 의 역변환). */
wchar_t *path_strip_extended(const wchar_t *path);

#endif /* BYTESIEVE_PATH_H */
