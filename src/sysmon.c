#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#include "sysmon.h"

static uint64_t filetime_u64(const FILETIME *ft)
{
    return ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

/* part / whole 의 천분율. round 가 0 이 아니면 반올림, 아니면 내림. whole 이 0 이면 0. */
static unsigned ratio_permille(uint64_t part, uint64_t whole, int round)
{
    uint64_t r;

    /* part * 1000 이 넘치지 않도록 큰 값은 함께 줄인다(정밀도는 천분율이면 충분). */
    while (part > (UINT64_MAX - whole) / 1000) {
        part >>= 10;
        whole >>= 10;
    }
    if (whole == 0) {
        return 0;
    }
    r = (part * 1000 + (round ? whole / 2 : 0)) / whole;
    return r > 1000 ? 1000 : (unsigned)r;
}

int sysmon_memory_system(uint64_t *total, uint64_t *available, unsigned long *error,
                         void *ctx)
{
    MEMORYSTATUSEX ms;

    (void)ctx;
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) {
        *error = GetLastError();
        return -1;
    }
    *total = ms.ullTotalPhys;
    *available = ms.ullAvailPhys;
    return 0;
}

int sysmon_cpu_system(SysmonCpuTimes *out, unsigned long *error, void *ctx)
{
    FILETIME idle, kernel, user;

    (void)ctx;
    if (!GetSystemTimes(&idle, &kernel, &user)) {
        *error = GetLastError();
        return -1;
    }
    out->idle = filetime_u64(&idle);
    out->kernel = filetime_u64(&kernel);
    out->user = filetime_u64(&user);
    return 0;
}

SysmonMemory sysmon_memory_from(uint64_t total, uint64_t available)
{
    SysmonMemory m;

    memset(&m, 0, sizeof(m));
    if (total == 0 || available > total) {
        m.status = SYSMON_INVALID;
        return m;
    }
    m.status = SYSMON_OK;
    m.total_bytes = total;
    m.available_bytes = available;
    m.used_bytes = total - available;
    m.usage_permille = ratio_permille(m.used_bytes, total, 0);
    return m;
}

SysmonCpu sysmon_cpu_between(const SysmonCpuTimes *prev, const SysmonCpuTimes *cur)
{
    SysmonCpu c;
    uint64_t idle, busy_total;

    memset(&c, 0, sizeof(c));
    if (prev == NULL || cur == NULL) {
        c.status = SYSMON_PENDING;
        return c;
    }
    if (cur->idle < prev->idle || cur->kernel < prev->kernel || cur->user < prev->user) {
        c.status = SYSMON_INVALID;
        return c;
    }
    idle = cur->idle - prev->idle;
    busy_total = (cur->kernel - prev->kernel) + (cur->user - prev->user);
    if (busy_total == 0) {
        c.status = SYSMON_INVALID;
        return c;
    }
    c.status = SYSMON_OK;
    c.usage_permille = idle >= busy_total ? 0 : ratio_permille(busy_total - idle, busy_total, 1);
    return c;
}

void sysmon_init(SysMonitor *m, SysmonMemoryFn memory_fn, SysmonCpuFn cpu_fn, void *ctx)
{
    memset(m, 0, sizeof(*m));
    m->memory_fn = memory_fn != NULL ? memory_fn : sysmon_memory_system;
    m->cpu_fn = cpu_fn != NULL ? cpu_fn : sysmon_cpu_system;
    m->ctx = ctx;
}

void sysmon_sample(SysMonitor *m, SysmonSample *out)
{
    uint64_t total = 0, available = 0;
    unsigned long error = 0;
    SysmonCpuTimes now;

    memset(out, 0, sizeof(*out));
    out->tick = GetTickCount();
    out->sequence = ++m->sequence;

    if (m->memory_fn(&total, &available, &error, m->ctx) == 0) {
        out->memory = sysmon_memory_from(total, available);
    } else {
        out->memory.status = SYSMON_ERROR;
        out->memory.error = error != 0 ? error : ERROR_GEN_FAILURE;
    }

    error = 0;
    memset(&now, 0, sizeof(now));
    if (m->cpu_fn(&now, &error, m->ctx) != 0) {
        out->cpu.status = SYSMON_ERROR;
        out->cpu.error = error != 0 ? error : ERROR_GEN_FAILURE;
        m->has_prev = 0;
        return;
    }
    out->cpu = sysmon_cpu_between(m->has_prev ? &m->prev : NULL, &now);
    /* INVALID(카운터 역행 등)여도 이번 값을 새 기준으로 삼아 다음 측정부터 다시 계산한다. */
    m->prev = now;
    m->has_prev = 1;
}

const wchar_t *sysmon_status_name(SysmonStatus status)
{
    switch (status) {
    case SYSMON_OK:
        return L"ok";
    case SYSMON_PENDING:
        return L"pending";
    case SYSMON_INVALID:
        return L"invalid";
    default:
        return L"error";
    }
}
