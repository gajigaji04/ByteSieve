#ifndef BYTESIEVE_SYSMON_H
#define BYTESIEVE_SYSMON_H

#include <stdint.h>
#include <wchar.h>

/*
 * RAM·CPU 사용률 측정(GlobalMemoryStatusEx, GetSystemTimes).
 * UI 에 의존하지 않으며 GUI 와 테스트가 함께 쓴다.
 */

/* 측정 결과 상태. OK 가 아니면 값 필드는 0 이다. */
typedef enum {
    SYSMON_OK = 0,
    SYSMON_PENDING, /* CPU: 비교할 직전 측정값이 아직 없음(첫 측정, 실패 직후) */
    SYSMON_INVALID, /* 측정은 됐지만 값이 앞뒤가 맞지 않음(카운터 역행 등) */
    SYSMON_ERROR    /* Windows API 실패. error 에 오류 코드 */
} SysmonStatus;

typedef struct {
    SysmonStatus status;
    unsigned long error;
    uint64_t total_bytes;     /* 실제 물리 메모리 */
    uint64_t available_bytes; /* 바로 쓸 수 있는 물리 메모리 */
    uint64_t used_bytes;      /* 전체 - 사용 가능 */
    unsigned usage_permille;  /* 0~1000, 내림 */
} SysmonMemory;

/* GetSystemTimes 누적값(100ns 단위). kernel 은 idle 을 포함한다. */
typedef struct {
    uint64_t idle;
    uint64_t kernel;
    uint64_t user;
} SysmonCpuTimes;

typedef struct {
    SysmonStatus status;
    unsigned long error;
    unsigned usage_permille; /* 0~1000, 반올림 */
} SysmonCpu;

typedef struct {
    SysmonMemory memory;
    SysmonCpu cpu;
    uint32_t tick;     /* 측정 시각(GetTickCount) */
    unsigned sequence; /* 1 부터 하나씩 증가 */
} SysmonSample;

/* 측정 함수. 성공 0, 실패하면 -1 이고 *error 에 Windows 오류 코드를 넣는다. */
typedef int (*SysmonMemoryFn)(uint64_t *total, uint64_t *available, unsigned long *error,
                              void *ctx);
typedef int (*SysmonCpuFn)(SysmonCpuTimes *out, unsigned long *error, void *ctx);

int sysmon_memory_system(uint64_t *total, uint64_t *available, unsigned long *error,
                         void *ctx);
int sysmon_cpu_system(SysmonCpuTimes *out, unsigned long *error, void *ctx);

/* 전체·사용 가능 용량으로 메모리 정보를 만든다. 사용 가능이 전체보다 크면 INVALID. */
SysmonMemory sysmon_memory_from(uint64_t total, uint64_t available);

/*
 * 두 누적값 사이의 CPU 사용률(1 - idle 증가분 / (kernel + user) 증가분).
 * 카운터가 줄었거나 증가분이 0 이면 INVALID. idle 이 전체보다 크게 늘면 0% 로 본다.
 */
SysmonCpu sysmon_cpu_between(const SysmonCpuTimes *prev, const SysmonCpuTimes *cur);

/* 측정기. 직전 CPU 누적값을 기억한다. */
typedef struct {
    SysmonMemoryFn memory_fn;
    SysmonCpuFn cpu_fn;
    void *ctx;
    SysmonCpuTimes prev;
    int has_prev;
    unsigned sequence;
} SysMonitor;

/* 측정 함수가 NULL 이면 실제 시스템을 측정한다. */
void sysmon_init(SysMonitor *m, SysmonMemoryFn memory_fn, SysmonCpuFn cpu_fn, void *ctx);

/*
 * RAM·CPU 를 한 번 측정해 out 을 채운다. 실패해도 out 의 상태로 알릴 뿐 멈추지 않는다.
 * CPU 측정이 실패하면 기준값을 버려 다음 측정은 PENDING 이 되고, 값이 맞지 않으면(INVALID)
 * 이번 누적값을 새 기준으로 삼아 다음 측정부터 다시 계산한다.
 */
void sysmon_sample(SysMonitor *m, SysmonSample *out);

/* 식별자 이름("ok", "pending" 등). 화면 문구는 GUI 가 따로 정한다. */
const wchar_t *sysmon_status_name(SysmonStatus status);

#endif /* BYTESIEVE_SYSMON_H */
