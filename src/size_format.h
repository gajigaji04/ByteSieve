#ifndef BYTESIEVE_SIZE_FORMAT_H
#define BYTESIEVE_SIZE_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

/*
 * 바이트 수를 "512 B", "1.50 KiB", "12.3 GiB" 처럼 짧게 쓴다.
 * 1024 미만은 정수 바이트, 그 이상은 10 미만이면 소수 둘째 자리, 아니면 첫째 자리까지.
 * buf 가 작으면 swprintf 규칙대로 잘린다(len 이 0 이면 아무것도 쓰지 않는다).
 */
void format_size(uint64_t bytes, wchar_t *buf, size_t len);

#endif /* BYTESIEVE_SIZE_FORMAT_H */
