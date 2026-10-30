#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "sysmon.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

/* 가짜 측정 함수가 돌려줄 값. */
typedef struct {
    int memory_fail;
    unsigned long memory_error;
    uint64_t total, available;
    int cpu_fail;
    unsigned long cpu_error;
    SysmonCpuTimes times;
    int memory_calls, cpu_calls;
} Fake;

static int fake_memory(uint64_t *total, uint64_t *available, unsigned long *error, void *ctx)
{
    Fake *f = ctx;

    f->memory_calls++;
    if (f->memory_fail) {
        *error = f->memory_error;
        return -1;
    }
    *total = f->total;
    *available = f->available;
    return 0;
}

static int fake_cpu(SysmonCpuTimes *out, unsigned long *error, void *ctx)
{
    Fake *f = ctx;

    f->cpu_calls++;
    if (f->cpu_fail) {
        *error = f->cpu_error;
        return -1;
    }
    *out = f->times;
    return 0;
}

static SysmonCpuTimes times(uint64_t idle, uint64_t kernel, uint64_t user)
{
    SysmonCpuTimes t;

    t.idle = idle;
    t.kernel = kernel;
    t.user = user;
    return t;
}

static void test_memory_from(void)
{
    SysmonMemory m;

    m = sysmon_memory_from(1000, 250);
    CHECK(m.status == SYSMON_OK);
    CHECK(m.total_bytes == 1000 && m.available_bytes == 250 && m.used_bytes == 750);
    CHECK(m.usage_permille == 750);

    /* 천분율은 내림: 2/3 → 666 */
    m = sysmon_memory_from(3, 1);
    CHECK(m.usage_permille == 666);

    m = sysmon_memory_from(1000, 1000);
    CHECK(m.status == SYSMON_OK && m.used_bytes == 0 && m.usage_permille == 0);
    m = sysmon_memory_from(1000, 0);
    CHECK(m.status == SYSMON_OK && m.usage_permille == 1000);

    /* 16 GiB 중 4 GiB 사용 가능 → 75.0% */
    m = sysmon_memory_from(16ULL << 30, 4ULL << 30);
    CHECK(m.usage_permille == 750);

    /* 아주 큰 값도 넘치지 않는다. */
    m = sysmon_memory_from(UINT64_MAX, UINT64_MAX / 2);
    CHECK(m.status == SYSMON_OK);
    CHECK(m.usage_permille >= 499 && m.usage_permille <= 500);
    m = sysmon_memory_from(UINT64_MAX, 0);
    CHECK(m.usage_permille == 1000);

    /* 잘못된 값: 전체 0, 사용 가능 > 전체 */
    m = sysmon_memory_from(0, 0);
    CHECK(m.status == SYSMON_INVALID && m.total_bytes == 0 && m.usage_permille == 0);
    m = sysmon_memory_from(1000, 1001);
    CHECK(m.status == SYSMON_INVALID && m.used_bytes == 0 && m.usage_permille == 0);
}

static void test_cpu_between(void)
{
    SysmonCpuTimes a, b;
    SysmonCpu c;

    /* kernel 증가분 600(idle 400 포함) + user 400 = 1000, idle 400 → 60% */
    a = times(1000, 5000, 3000);
    b = times(1400, 5600, 3400);
    c = sysmon_cpu_between(&a, &b);
    CHECK(c.status == SYSMON_OK && c.usage_permille == 600);

    /* 완전히 쉼, 완전히 바쁨 */
    b = times(2000, 6000, 3000);
    c = sysmon_cpu_between(&a, &b);
    CHECK(c.status == SYSMON_OK && c.usage_permille == 0);
    b = times(1000, 5500, 3500);
    c = sysmon_cpu_between(&a, &b);
    CHECK(c.status == SYSMON_OK && c.usage_permille == 1000);

    /* 반올림: 2/3 → 667 */
    b = times(1001, 5002, 3001);
    c = sysmon_cpu_between(&a, &b);
    CHECK(c.status == SYSMON_OK && c.usage_permille == 667);

    /* idle 이 전체보다 크게 늘면(잘못 보고된 값) 0% 로 본다. */
    b = times(3000, 5500, 3000);
    c = sysmon_cpu_between(&a, &b);
    CHECK(c.status == SYSMON_OK && c.usage_permille == 0);

    /* 증가분 0, 카운터 역행 → INVALID */
    c = sysmon_cpu_between(&a, &a);
    CHECK(c.status == SYSMON_INVALID && c.usage_permille == 0);
    b = times(999, 6000, 4000);
    CHECK(sysmon_cpu_between(&a, &b).status == SYSMON_INVALID);
    b = times(2000, 4999, 4000);
    CHECK(sysmon_cpu_between(&a, &b).status == SYSMON_INVALID);
    b = times(2000, 6000, 2999);
    CHECK(sysmon_cpu_between(&a, &b).status == SYSMON_INVALID);

    /* 기준값 없음 */
    CHECK(sysmon_cpu_between(NULL, &b).status == SYSMON_PENDING);
    CHECK(sysmon_cpu_between(&a, NULL).status == SYSMON_PENDING);

    /* 큰 누적값(오래 켜 둔 시스템)도 넘치지 않는다. */
    a = times(UINT64_MAX / 4, UINT64_MAX / 4, UINT64_MAX / 4);
    b = times(a.idle + (UINT64_MAX / 8), a.kernel + (UINT64_MAX / 8), a.user + (UINT64_MAX / 8));
    c = sysmon_cpu_between(&a, &b);
    CHECK(c.status == SYSMON_OK && c.usage_permille >= 499 && c.usage_permille <= 501);
}

static void test_sampler_with_fakes(void)
{
    Fake f;
    SysMonitor m;
    SysmonSample s;

    memset(&f, 0, sizeof(f));
    f.total = 1000;
    f.available = 400;
    f.times = times(100, 200, 100);
    sysmon_init(&m, fake_memory, fake_cpu, &f);

    /* 첫 측정: RAM 은 바로, CPU 는 비교할 값이 없어 PENDING */
    sysmon_sample(&m, &s);
    CHECK(s.sequence == 1);
    CHECK(s.memory.status == SYSMON_OK && s.memory.usage_permille == 600);
    CHECK(s.cpu.status == SYSMON_PENDING && s.cpu.usage_permille == 0);

    /* 두 번째: idle 50 / 전체 200 → 75% */
    f.times = times(150, 300, 200);
    sysmon_sample(&m, &s);
    CHECK(s.sequence == 2);
    CHECK(s.cpu.status == SYSMON_OK && s.cpu.usage_permille == 750);

    /* RAM 측정 실패: 오류 코드를 남기고 CPU 는 계속 측정한다. */
    f.memory_fail = 1;
    f.memory_error = ERROR_ACCESS_DENIED;
    f.times = times(250, 400, 200);
    sysmon_sample(&m, &s);
    CHECK(s.memory.status == SYSMON_ERROR && s.memory.error == ERROR_ACCESS_DENIED);
    CHECK(s.memory.total_bytes == 0 && s.memory.usage_permille == 0);
    CHECK(s.cpu.status == SYSMON_OK && s.cpu.usage_permille == 0);

    /* 오류 코드 없이 실패해도 0 이 아닌 코드를 남긴다. */
    f.memory_error = 0;
    sysmon_sample(&m, &s);
    CHECK(s.memory.status == SYSMON_ERROR && s.memory.error != 0);
    f.memory_fail = 0;

    /* RAM 이 잘못된 값을 주면 INVALID */
    f.available = 2000;
    sysmon_sample(&m, &s);
    CHECK(s.memory.status == SYSMON_INVALID && s.memory.usage_permille == 0);
    f.available = 400;

    /* CPU 측정 실패 → 기준값을 버려 회복 직후는 PENDING, 그다음부터 OK */
    f.cpu_fail = 1;
    f.cpu_error = ERROR_INVALID_FUNCTION;
    sysmon_sample(&m, &s);
    CHECK(s.cpu.status == SYSMON_ERROR && s.cpu.error == ERROR_INVALID_FUNCTION);
    CHECK(s.memory.status == SYSMON_OK);
    f.cpu_fail = 0;
    f.times = times(1000, 2000, 1000);
    sysmon_sample(&m, &s);
    CHECK(s.cpu.status == SYSMON_PENDING);
    f.times = times(1100, 2100, 1100);
    sysmon_sample(&m, &s);
    CHECK(s.cpu.status == SYSMON_OK && s.cpu.usage_permille == 500);

    /* 카운터 역행 → INVALID, 그 값을 새 기준으로 다음에 다시 계산 */
    f.times = times(10, 20, 10);
    sysmon_sample(&m, &s);
    CHECK(s.cpu.status == SYSMON_INVALID);
    f.times = times(20, 40, 30);
    sysmon_sample(&m, &s);
    CHECK(s.cpu.status == SYSMON_OK && s.cpu.usage_permille == 750);

    CHECK(f.memory_calls == (int)s.sequence && f.cpu_calls == (int)s.sequence);
}

static void test_names(void)
{
    CHECK(wcscmp(sysmon_status_name(SYSMON_OK), L"ok") == 0);
    CHECK(wcscmp(sysmon_status_name(SYSMON_PENDING), L"pending") == 0);
    CHECK(wcscmp(sysmon_status_name(SYSMON_INVALID), L"invalid") == 0);
    CHECK(wcscmp(sysmon_status_name(SYSMON_ERROR), L"error") == 0);
}

/* 실제 시스템: 두 번 측정해 값의 범위와 일관성을 확인한다. */
static void test_system(void)
{
    SysMonitor m;
    SysmonSample s1, s2;
    MEMORYSTATUSEX ms;

    sysmon_init(&m, NULL, NULL, NULL);
    sysmon_sample(&m, &s1);
    CHECK(s1.memory.status == SYSMON_OK);
    CHECK(s1.memory.total_bytes > 0);
    CHECK(s1.memory.available_bytes <= s1.memory.total_bytes);
    CHECK(s1.memory.used_bytes + s1.memory.available_bytes == s1.memory.total_bytes);
    CHECK(s1.cpu.status == SYSMON_PENDING);

    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    CHECK(GlobalMemoryStatusEx(&ms));
    CHECK(ms.ullTotalPhys == s1.memory.total_bytes);

    Sleep(250);
    sysmon_sample(&m, &s2);
    CHECK(s2.sequence == s1.sequence + 1);
    CHECK(s2.memory.status == SYSMON_OK);
    CHECK(s2.memory.usage_permille <= 1000);
    CHECK(s2.cpu.status == SYSMON_OK);
    CHECK(s2.cpu.usage_permille <= 1000);
    CHECK(s2.tick - s1.tick >= 200);

    printf("ram total=%llu avail=%llu usage=%u.%u%% cpu=%u.%u%%\n",
           (unsigned long long)s2.memory.total_bytes,
           (unsigned long long)s2.memory.available_bytes, s2.memory.usage_permille / 10,
           s2.memory.usage_permille % 10, s2.cpu.usage_permille / 10, s2.cpu.usage_permille % 10);
}

int main(void)
{
    test_memory_from();
    test_cpu_between();
    test_sampler_with_fakes();
    test_names();
    test_system();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all sysmon tests passed\n");
    return 0;
}
