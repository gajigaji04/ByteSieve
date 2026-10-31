#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cleanup_scan.h"
#include "scan_model.h"
#include "scanner.h"
#include "size_format.h"

/*
 * 대용량 폴더 스캔의 실행 시간·메모리 측정 도구(bytesieve-bench).
 *
 *   bytesieve-bench [--mode tree|model|cleanup] [--runs N] [--threads N] <directory>
 *
 *   tree     scan_tree 집계만(열거 비용)
 *   model    scan_model_build(GUI 폴더 스캔과 같은 트리 구성)
 *   cleanup  cleanup_scan(정리 후보 탐지)
 *
 *   --threads N  폴더 열거 작업 스레드 수(0 = 자동, 1 = 병렬 없음). scan_set_threads 참고.
 *
 * 실행마다 경과 시간·CPU 시간(사용자+커널)·결과 구조 해제 시간과 스캐너 진단 값(동시에 살아 있던
 * 폴더 작업 수·메모리 최대, 미리 열거·직접 열거·기다림·버린 작업 수)을, 끝에 프로세스 최대
 * 작업 집합과 최대 전용 메모리(커밋)를 출력한다. 최대값은 프로세스 전체 기준이므로
 * 모드를 비교할 때는 모드마다 따로 실행한다. 첫 실행은 디스크 캐시가 차지 않았을 수 있다.
 */

typedef enum { MODE_TREE, MODE_MODEL, MODE_CLEANUP } BenchMode;

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static double cpu_ms(void)
{
    FILETIME c, e, k, u;
    ULARGE_INTEGER kk, uu;

    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        return 0.0;
    }
    kk.LowPart = k.dwLowDateTime;
    kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime;
    uu.HighPart = u.dwHighDateTime;
    return (double)(kk.QuadPart + uu.QuadPart) / 10000.0;
}

static void memory(PROCESS_MEMORY_COUNTERS_EX *pmc)
{
    memset(pmc, 0, sizeof(*pmc));
    pmc->cb = sizeof(*pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)pmc, sizeof(*pmc));
}

static void usage(void)
{
    fwprintf(stderr, L"Usage: bytesieve-bench [--mode tree|model|cleanup] [--runs N] [--threads N] "
                     L"<directory>\n");
}

int wmain(int argc, wchar_t **argv)
{
    BenchMode mode = MODE_MODEL;
    const wchar_t *target = NULL;
    int runs = 3, i;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    wchar_t a[32], b[32];
    int status_code = 0;

    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);
    for (i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--mode") == 0 && i + 1 < argc) {
            const wchar_t *m = argv[++i];
            if (wcscmp(m, L"tree") == 0) {
                mode = MODE_TREE;
            } else if (wcscmp(m, L"model") == 0) {
                mode = MODE_MODEL;
            } else if (wcscmp(m, L"cleanup") == 0) {
                mode = MODE_CLEANUP;
            } else {
                usage();
                return 2;
            }
        } else if (wcscmp(argv[i], L"--runs") == 0 && i + 1 < argc) {
            runs = _wtoi(argv[++i]);
            if (runs < 1 || runs > 100) {
                usage();
                return 2;
            }
        } else if (wcscmp(argv[i], L"--threads") == 0 && i + 1 < argc) {
            int n = _wtoi(argv[++i]);
            if (n < 0 || n > SCAN_MAX_THREADS) {
                usage();
                return 2;
            }
            scan_set_threads((unsigned)n);
        } else if (argv[i][0] == L'-') {
            usage();
            return 2;
        } else {
            target = argv[i];
        }
    }
    if (target == NULL) {
        usage();
        return 2;
    }

    wprintf(L"target:  %ls\nmode:    %ls\nthreads: %u\n", target,
            mode == MODE_TREE ? L"tree" : mode == MODE_MODEL ? L"model" : L"cleanup",
            scan_effective_threads());
    for (i = 0; i < runs; i++) {
        double t0 = now_ms(), c0 = cpu_ms(), t1, c1, t2;
        uint64_t files = 0, dirs = 0, bytes = 0, extra = 0;
        ScanReport diag; /* 진단 값만 복사해 둔다(errors 는 쓰지 않음) */
        ScanStatus st;

        if (mode == MODE_TREE) {
            ScanReport r;
            scan_report_init(&r);
            st = scan_tree(target, SCAN_DEPTH_UNLIMITED, &r, NULL, NULL);
            t1 = now_ms();
            c1 = cpu_ms();
            files = r.file_count;
            dirs = r.dir_count;
            bytes = r.total_bytes;
            diag = r;
            memory(&pmc);
            scan_report_free(&r);
        } else if (mode == MODE_MODEL) {
            ScanModel m;
            scan_model_init(&m);
            st = scan_model_build(&m, target, SCAN_DEPTH_UNLIMITED, NULL, NULL);
            t1 = now_ms();
            c1 = cpu_ms();
            files = m.report.file_count;
            dirs = m.report.dir_count;
            bytes = m.report.total_bytes;
            extra = m.len;
            diag = m.report;
            memory(&pmc);
            scan_model_free(&m);
        } else {
            CleanupResult r;
            cleanup_result_init(&r);
            st = cleanup_scan(&r, target, NULL, NULL, NULL, NULL);
            t1 = now_ms();
            c1 = cpu_ms();
            files = r.report.file_count;
            dirs = r.report.dir_count;
            bytes = r.report.total_bytes;
            extra = r.len;
            diag = r.report;
            memory(&pmc);
            cleanup_result_free(&r);
        }
        t2 = now_ms();
        format_size(bytes, a, 32);
        format_size(pmc.PrivateUsage, b, 32);
        wprintf(L"run %d: %.1f ms (cpu %.1f ms, free %.1f ms) files=%llu dirs=%llu bytes=%ls "
                L"%ls=%llu private=%ls status=%ls\n",
                i + 1, t1 - t0, c1 - c0, t2 - t1, (unsigned long long)files,
                (unsigned long long)dirs, a, mode == MODE_CLEANUP ? L"items" : L"nodes",
                (unsigned long long)extra, b, scan_status_name(st));
        format_size(diag.peak_task_bytes, a, 32);
        wprintf(L"       scanner: peak tasks %llu (%ls) · prefetched %llu · self %llu · waits %llu · "
                L"discarded %llu\n",
                (unsigned long long)diag.peak_tasks, a, (unsigned long long)diag.prefetched_dirs,
                (unsigned long long)diag.self_fetched_dirs, (unsigned long long)diag.prefetch_waits,
                (unsigned long long)diag.discarded_dirs);
        if (st == SCAN_FATAL) {
            status_code = 1;
        }
    }
    memory(&pmc);
    format_size(pmc.PeakWorkingSetSize, a, 32);
    format_size(pmc.PeakPagefileUsage, b, 32);
    wprintf(L"peak working set: %ls\npeak private:     %ls\n", a, b);
    return status_code;
}
