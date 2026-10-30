#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "scanner.h"

void scan_report_init(ScanReport *report)
{
    memset(report, 0, sizeof(*report));
}

void scan_report_free(ScanReport *report)
{
    size_t i;

    if (report == NULL) {
        return;
    }
    for (i = 0; i < report->errors_len; i++) {
        free(report->errors[i].path);
    }
    free(report->errors);
    scan_report_init(report);
}

uint64_t scan_combine_size(uint32_t high, uint32_t low)
{
    return ((uint64_t)high << 32) | (uint64_t)low;
}

int scan_should_descend(uint32_t attributes)
{
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

/* 오류를 기록한다. 메모리가 부족하면 개수만 센다. */
static void record_error(ScanReport *report, const wchar_t *path, DWORD code)
{
    report->error_count++;

    if (report->errors_len == report->errors_cap) {
        size_t cap = report->errors_cap ? report->errors_cap * 2 : 8;
        ScanError *grown = realloc(report->errors, cap * sizeof(*grown));
        if (grown == NULL) {
            return;
        }
        report->errors = grown;
        report->errors_cap = cap;
    }
    report->errors[report->errors_len].path = path_strip_extended(path);
    report->errors[report->errors_len].code = (uint32_t)code;
    report->errors_len++;
}

static int is_dot_entry(const wchar_t *name)
{
    return name[0] == L'.' &&
           (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

static void account_entry(const WIN32_FIND_DATAW *fd, ScanReport *report,
                          scan_visit_fn visit, void *ctx)
{
    ScanEntry entry;

    entry.name = fd->cFileName;
    entry.attributes = (uint32_t)fd->dwFileAttributes;
    entry.is_dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.is_reparse = (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry.descend = scan_should_descend(entry.attributes);

    if (entry.is_dir) {
        entry.size = 0;
        report->dir_count++;
        if (entry.is_reparse) {
            report->reparse_count++;
        }
    } else {
        entry.size = scan_combine_size((uint32_t)fd->nFileSizeHigh,
                                       (uint32_t)fd->nFileSizeLow);
        report->file_count++;
        report->total_bytes += entry.size;
    }

    if (visit != NULL) {
        visit(&entry, ctx);
    }
}

ScanStatus scan_directory(const wchar_t *dir, ScanReport *report,
                          scan_visit_fn visit, void *ctx)
{
    WIN32_FIND_DATAW fd;
    HANDLE find;
    wchar_t *ext;
    wchar_t *pattern;
    DWORD attrs, err;
    ScanStatus status = SCAN_COMPLETE;

    if (report == NULL) {
        return SCAN_FAILED;
    }
    if (dir == NULL || dir[0] == L'\0') {
        record_error(report, dir, ERROR_INVALID_PARAMETER);
        return SCAN_FAILED;
    }

    ext = path_to_extended(dir);
    if (ext == NULL) {
        record_error(report, dir, ERROR_FILENAME_EXCED_RANGE);
        return SCAN_FAILED;
    }

    /* 빈 디렉터리와 존재하지 않는 경로를 구분하기 위해 먼저 확인한다. */
    attrs = GetFileAttributesW(ext);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        record_error(report, ext, GetLastError());
        free(ext);
        return SCAN_FAILED;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        record_error(report, ext, ERROR_DIRECTORY);
        free(ext);
        return SCAN_FAILED;
    }

    pattern = path_join(ext, L"*");
    if (pattern == NULL) {
        record_error(report, ext, ERROR_FILENAME_EXCED_RANGE);
        free(ext);
        return SCAN_FAILED;
    }

    find = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        /* 드라이브 루트처럼 "." 조차 없는 빈 디렉터리. */
        if (err == ERROR_FILE_NOT_FOUND) {
            free(ext);
            return SCAN_COMPLETE;
        }
        record_error(report, ext, err);
        free(ext);
        return SCAN_FAILED;
    }

    do {
        if (!is_dot_entry(fd.cFileName)) {
            account_entry(&fd, report, visit, ctx);
        }
    } while (FindNextFileW(find, &fd));

    err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) {
        record_error(report, ext, err);
        status = SCAN_PARTIAL;
    }

    FindClose(find);
    free(ext);
    return status;
}
