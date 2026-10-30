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

/* DFS 에서 아직 열거하지 않은 디렉터리(확장 경로, 소유). */
typedef struct {
    wchar_t *path;
    uint32_t depth;
} DirFrame;

typedef struct {
    DirFrame *items;
    size_t len;
    size_t cap;
} DirStack;

/* path 의 소유권을 가져간다. 실패하면 path 를 해제하고 0 을 반환한다. */
static int stack_push(DirStack *stack, wchar_t *path, uint32_t depth)
{
    if (stack->len == stack->cap) {
        size_t cap = stack->cap ? stack->cap * 2 : 16;
        DirFrame *grown = realloc(stack->items, cap * sizeof(*grown));
        if (grown == NULL) {
            free(path);
            return 0;
        }
        stack->items = grown;
        stack->cap = cap;
    }
    stack->items[stack->len].path = path;
    stack->items[stack->len].depth = depth;
    stack->len++;
    return 1;
}

static void stack_free(DirStack *stack)
{
    size_t i;

    for (i = 0; i < stack->len; i++) {
        free(stack->items[i].path);
    }
    free(stack->items);
}

/* 루트 경로를 검증해 확장 경로로 돌려준다. 실패하면 오류를 기록하고 NULL. */
static wchar_t *open_root(const wchar_t *dir, ScanReport *report)
{
    wchar_t *ext;
    DWORD attrs;

    if (dir == NULL || dir[0] == L'\0') {
        record_error(report, dir, ERROR_INVALID_PARAMETER);
        return NULL;
    }

    ext = path_to_extended(dir);
    if (ext == NULL) {
        record_error(report, dir, ERROR_FILENAME_EXCED_RANGE);
        return NULL;
    }

    /* 빈 디렉터리와 존재하지 않는 경로를 구분하기 위해 먼저 확인한다. */
    attrs = GetFileAttributesW(ext);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        record_error(report, ext, GetLastError());
        free(ext);
        return NULL;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        record_error(report, ext, ERROR_DIRECTORY);
        free(ext);
        return NULL;
    }
    return ext;
}

/*
 * frame 디렉터리 바로 아래를 열거해 집계하고, 깊이 제한 안의 하위 디렉터리를
 * stack 에 쌓는다. 디렉터리를 열지 못하면 SCAN_FAILED, 도중 오류는 SCAN_PARTIAL.
 */
static ScanStatus enumerate_dir(const DirFrame *frame, uint32_t max_depth,
                                DirStack *stack, ScanReport *report,
                                scan_visit_fn visit, void *ctx)
{
    WIN32_FIND_DATAW fd;
    HANDLE find;
    wchar_t *pattern;
    DWORD err;
    ScanStatus status = SCAN_COMPLETE;

    pattern = path_join(frame->path, L"*");
    if (pattern == NULL) {
        record_error(report, frame->path, ERROR_FILENAME_EXCED_RANGE);
        return SCAN_FAILED;
    }

    find = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        err = GetLastError();
        /* 드라이브 루트처럼 "." 조차 없는 빈 디렉터리. */
        if (err == ERROR_FILE_NOT_FOUND) {
            return SCAN_COMPLETE;
        }
        record_error(report, frame->path, err);
        return SCAN_FAILED;
    }

    do {
        if (is_dot_entry(fd.cFileName)) {
            continue;
        }
        account_entry(&fd, report, visit, ctx);

        if (frame->depth < max_depth &&
            scan_should_descend((uint32_t)fd.dwFileAttributes)) {
            wchar_t *child = path_join(frame->path, fd.cFileName);
            if (child == NULL) {
                /* path_join 은 길이 초과와 메모리 부족을 구분하지 않는다. */
                record_error(report, frame->path, ERROR_FILENAME_EXCED_RANGE);
                status = SCAN_PARTIAL;
            } else if (!stack_push(stack, child, frame->depth + 1)) {
                record_error(report, frame->path, ERROR_NOT_ENOUGH_MEMORY);
                status = SCAN_PARTIAL;
            }
        }
    } while (FindNextFileW(find, &fd));

    err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) {
        record_error(report, frame->path, err);
        status = SCAN_PARTIAL;
    }

    FindClose(find);
    return status;
}

ScanStatus scan_tree(const wchar_t *dir, uint32_t max_depth, ScanReport *report,
                     scan_visit_fn visit, void *ctx)
{
    DirStack stack = {0};
    DirFrame frame;
    ScanStatus status = SCAN_COMPLETE;
    ScanStatus result;
    wchar_t *ext;

    if (report == NULL) {
        return SCAN_FAILED;
    }

    ext = open_root(dir, report);
    if (ext == NULL) {
        return SCAN_FAILED;
    }

    frame.path = ext;
    frame.depth = 0;
    result = enumerate_dir(&frame, max_depth, &stack, report, visit, ctx);
    free(ext);
    if (result == SCAN_FAILED) {
        stack_free(&stack);
        return SCAN_FAILED;
    }
    if (result != SCAN_COMPLETE) {
        status = SCAN_PARTIAL;
    }

    /* 마지막에 쌓인 디렉터리부터 꺼내므로 깊이 우선으로 진행된다. */
    while (stack.len > 0) {
        frame = stack.items[--stack.len];
        result = enumerate_dir(&frame, max_depth, &stack, report, visit, ctx);
        if (result != SCAN_COMPLETE) {
            status = SCAN_PARTIAL;
        }
        free(frame.path);
    }

    stack_free(&stack);
    return status;
}

ScanStatus scan_directory(const wchar_t *dir, ScanReport *report,
                          scan_visit_fn visit, void *ctx)
{
    return scan_tree(dir, 0, report, visit, ctx);
}
