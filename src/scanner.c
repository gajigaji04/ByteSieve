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

ScanErrorKind scan_classify_error(uint32_t code)
{
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_PRIVILEGE_NOT_HELD:
        return SCAN_ERR_ACCESS_DENIED;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_NOT_READY:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return SCAN_ERR_NOT_FOUND;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY:
    case ERROR_INVALID_PARAMETER:
        return SCAN_ERR_INVALID_PATH;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_BUFFER_OVERFLOW:
        return SCAN_ERR_PATH_TOO_LONG;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return SCAN_ERR_NO_MEMORY;
    default:
        return SCAN_ERR_OTHER;
    }
}

const wchar_t *scan_status_name(ScanStatus status)
{
    switch (status) {
    case SCAN_COMPLETE:
        return L"complete";
    case SCAN_PARTIAL:
        return L"partial";
    default:
        return L"fatal";
    }
}

const wchar_t *scan_error_kind_name(ScanErrorKind kind)
{
    switch (kind) {
    case SCAN_ERR_ACCESS_DENIED:
        return L"access_denied";
    case SCAN_ERR_NOT_FOUND:
        return L"not_found";
    case SCAN_ERR_INVALID_PATH:
        return L"invalid_path";
    case SCAN_ERR_PATH_TOO_LONG:
        return L"path_too_long";
    case SCAN_ERR_NO_MEMORY:
        return L"no_memory";
    default:
        return L"other";
    }
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
    report->errors[report->errors_len].kind = scan_classify_error((uint32_t)code);
    report->errors_len++;
}

static int is_dot_entry(const wchar_t *name)
{
    return name[0] == L'.' &&
           (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

static void account_entry(const WIN32_FIND_DATAW *fd, ScanReport *report,
                          ScanEntry *entry)
{
    entry->name = fd->cFileName;
    entry->attributes = (uint32_t)fd->dwFileAttributes;
    entry->is_dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry->is_reparse = (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry->descend = scan_should_descend(entry->attributes);

    if (entry->is_dir) {
        entry->size = 0;
        report->dir_count++;
        if (entry->is_reparse) {
            report->reparse_count++;
        }
    } else {
        entry->size = scan_combine_size((uint32_t)fd->nFileSizeHigh,
                                        (uint32_t)fd->nFileSizeLow);
        report->file_count++;
        report->total_bytes += entry->size;
    }
}

/* DFS 에서 아직 열거하지 않은 디렉터리(확장 경로, 소유). */
typedef struct {
    wchar_t *path;
    uint32_t depth;
    uintptr_t id;
} DirFrame;

typedef struct {
    DirFrame *items;
    size_t len;
    size_t cap;
} DirStack;

/* path 의 소유권을 가져간다. 실패하면 path 를 해제하고 0 을 반환한다. */
static int stack_push(DirStack *stack, wchar_t *path, uint32_t depth,
                      uintptr_t id)
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
    stack->items[stack->len].id = id;
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
 * stack 에 쌓는다. 디렉터리를 열지 못하면 SCAN_FATAL, 도중 오류는 SCAN_PARTIAL.
 * 열지 못한 디렉터리와 스택에 넣지 못한 하위 디렉터리는 skipped_dir_count 로 센다.
 */
static ScanStatus enumerate_dir(const DirFrame *frame, uint32_t max_depth,
                                DirStack *stack, ScanReport *report,
                                const ScanHooks *hooks, void *ctx)
{
    WIN32_FIND_DATAW fd;
    HANDLE find;
    wchar_t *pattern;
    DWORD err;
    ScanStatus status = SCAN_COMPLETE;

    pattern = path_join(frame->path, L"*");
    if (pattern == NULL) {
        record_error(report, frame->path, ERROR_FILENAME_EXCED_RANGE);
        report->skipped_dir_count++;
        return SCAN_FATAL;
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
        report->skipped_dir_count++;
        return SCAN_FATAL;
    }

    do {
        ScanEntry entry;
        uintptr_t child_id = 0;
        int will_descend;

        if (is_dot_entry(fd.cFileName)) {
            continue;
        }
        account_entry(&fd, report, &entry);
        will_descend = frame->depth < max_depth && entry.descend;
        if (hooks->on_entry != NULL) {
            child_id = hooks->on_entry(&entry, frame->id, will_descend, ctx);
        }

        if (will_descend) {
            wchar_t *child = path_join(frame->path, fd.cFileName);
            if (child == NULL) {
                /* path_join 은 길이 초과와 메모리 부족을 구분하지 않는다. */
                record_error(report, frame->path, ERROR_FILENAME_EXCED_RANGE);
                report->skipped_dir_count++;
                status = SCAN_PARTIAL;
            } else if (!stack_push(stack, child, frame->depth + 1, child_id)) {
                record_error(report, frame->path, ERROR_NOT_ENOUGH_MEMORY);
                report->skipped_dir_count++;
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

/* 훅 호출로 감싼 enumerate_dir. */
static ScanStatus visit_dir(const DirFrame *frame, uint32_t max_depth,
                            DirStack *stack, ScanReport *report,
                            const ScanHooks *hooks, void *ctx)
{
    ScanDirInfo info;
    ScanStatus result;

    info.id = frame->id;
    info.path = frame->path;
    info.depth = frame->depth;
    info.pending = stack->len;
    if (hooks->on_dir_begin != NULL) {
        hooks->on_dir_begin(&info, ctx);
    }
    result = enumerate_dir(frame, max_depth, stack, report, hooks, ctx);
    if (hooks->on_dir_end != NULL) {
        info.pending = stack->len;
        hooks->on_dir_end(&info, result, ctx);
    }
    return result;
}

ScanStatus scan_tree_ex(const wchar_t *dir, uint32_t max_depth, uintptr_t root_id,
                        ScanReport *report, const ScanHooks *hooks, void *ctx)
{
    static const ScanHooks no_hooks = {NULL, NULL, NULL};
    DirStack stack = {0};
    DirFrame frame;
    ScanStatus status = SCAN_COMPLETE;
    ScanStatus result;
    wchar_t *ext;

    if (report == NULL) {
        return SCAN_FATAL;
    }
    if (hooks == NULL) {
        hooks = &no_hooks;
    }

    ext = open_root(dir, report);
    if (ext == NULL) {
        report->aborted = 1;
        return SCAN_FATAL;
    }

    frame.path = ext;
    frame.depth = 0;
    frame.id = root_id;
    result = visit_dir(&frame, max_depth, &stack, report, hooks, ctx);
    free(ext);
    if (result == SCAN_FATAL) {
        /* 루트를 열지 못하면 하위 트리 전체를 알 수 없으므로 중단한다. */
        report->aborted = 1;
        stack_free(&stack);
        return SCAN_FATAL;
    }
    if (result != SCAN_COMPLETE) {
        status = SCAN_PARTIAL;
    }

    /*
     * 마지막에 쌓인 디렉터리부터 꺼내므로 깊이 우선으로 진행된다.
     * 하위 디렉터리 실패(SCAN_FATAL 포함)는 그 디렉터리만 건너뛰고 계속한다.
     */
    while (stack.len > 0) {
        frame = stack.items[--stack.len];
        result = visit_dir(&frame, max_depth, &stack, report, hooks, ctx);
        if (result != SCAN_COMPLETE) {
            status = SCAN_PARTIAL;
        }
        free(frame.path);
    }

    stack_free(&stack);
    return status;
}

/* scan_tree 의 visit 콜백을 on_entry 훅으로 옮긴다. */
typedef struct {
    scan_visit_fn visit;
    void *ctx;
} VisitAdapter;

static uintptr_t adapt_visit(const ScanEntry *entry, uintptr_t parent,
                             int will_descend, void *ctx)
{
    VisitAdapter *a = ctx;

    (void)parent;
    (void)will_descend;
    a->visit(entry, a->ctx);
    return 0;
}

ScanStatus scan_tree(const wchar_t *dir, uint32_t max_depth, ScanReport *report,
                     scan_visit_fn visit, void *ctx)
{
    ScanHooks hooks = {NULL, NULL, NULL};
    VisitAdapter adapter;

    adapter.visit = visit;
    adapter.ctx = ctx;
    if (visit != NULL) {
        hooks.on_entry = adapt_visit;
    }
    return scan_tree_ex(dir, max_depth, 0, report, &hooks, &adapter);
}

ScanStatus scan_directory(const wchar_t *dir, ScanReport *report,
                          scan_visit_fn visit, void *ctx)
{
    return scan_tree(dir, 0, report, visit, ctx);
}
