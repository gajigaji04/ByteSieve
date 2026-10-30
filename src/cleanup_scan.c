#include <stdlib.h>
#include <string.h>

#include "cleanup_scan.h"
#include "path.h"

#define ITEM_NONE ((size_t)-1)

/*
 * 탐색 상태. 폴더 id 는 그 폴더를 차지한 항목 번호 + 1 이고, 아무 항목에도 속하지
 * 않으면 0 이다. 그래서 폴더마다 따로 메모리를 잡지 않는다.
 */
typedef struct {
    CleanupResult *r;
    const CleanupProtect *protect;
    int skip_protected;
    cleanup_progress_fn progress;
    cleanup_cancel_fn cancel;
    void *ctx;
    uint64_t dirs_scanned;
    uint64_t found_bytes; /* 지금까지 항목에 더한 용량(진행률 표시용) */

    /* 지금 열거 중인 폴더(on_dir_begin ~ on_dir_end) */
    wchar_t *cur_dir;  /* 표시용 경로 */
    size_t cur_first;  /* 이 폴더에서 처음 만든 항목 번호 */
    int cur_marker;    /* 이 폴더에 프로젝트 표시 파일이 있었는지 */
} DetectCtx;

void cleanup_result_init(CleanupResult *r)
{
    memset(r, 0, sizeof(*r));
    scan_report_init(&r->report);
}

void cleanup_result_free(CleanupResult *r)
{
    size_t i;

    if (r == NULL) {
        return;
    }
    for (i = 0; i < r->len; i++) {
        free(r->items[i].path);
    }
    free(r->items);
    free(r->root);
    scan_report_free(&r->report);
    cleanup_result_init(r);
}

static size_t add_item(DetectCtx *c, const wchar_t *name, CleanupCategory category,
                       uint32_t flags, uint64_t bytes, uint64_t files)
{
    CleanupResult *r = c->r;
    CleanupItem *item;
    wchar_t *path;

    path = c->cur_dir != NULL ? path_join(c->cur_dir, name) : NULL;
    if (path == NULL) {
        r->out_of_memory = 1;
        return ITEM_NONE;
    }
    if (r->len == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 64;
        CleanupItem *grown = realloc(r->items, cap * sizeof(*grown));
        if (grown == NULL) {
            free(path);
            r->out_of_memory = 1;
            return ITEM_NONE;
        }
        r->items = grown;
        r->cap = cap;
    }
    item = &r->items[r->len];
    memset(item, 0, sizeof(*item));
    item->path = path;
    item->category = category;
    item->flags = flags;
    item->bytes = bytes;
    item->files = files;
    return r->len++;
}

static uintptr_t on_entry(const ScanEntry *entry, uintptr_t parent, int will_descend,
                          void *ctx)
{
    DetectCtx *c = ctx;
    CleanupCategory category;
    size_t index;

    /* 링크는 대상이 다른 곳에 있으므로 세지 않는다(삭제해도 링크만 사라진다). */
    if (entry->is_reparse) {
        return 0;
    }

    /* 이미 항목에 속한 폴더 안: 용량만 더한다. */
    if (parent != 0) {
        CleanupItem *owner = &c->r->items[parent - 1];
        if (entry->is_dir) {
            owner->dirs++;
            if (will_descend) {
                owner->pending++;
                return parent;
            }
            return 0;
        }
        owner->files++;
        owner->bytes += entry->size;
        c->found_bytes += entry->size;
        return 0;
    }

    if (!entry->is_dir) {
        if (cleanup_is_project_marker(entry->name)) {
            c->cur_marker = 1;
        }
        category = cleanup_classify_file(entry->name);
        if (category != CLEANUP_NONE &&
            add_item(c, entry->name, category, 0, entry->size, 1) != ITEM_NONE) {
            c->found_bytes += entry->size;
        }
        return 0;
    }

    category = cleanup_classify_dir(entry->name);
    if (category == CLEANUP_NONE) {
        return 0;
    }
    index = add_item(c, entry->name, category, CLEANUP_ITEM_DIR, 0, 0);
    if (index == ITEM_NONE || !will_descend) {
        return 0;
    }
    c->r->items[index].pending = 1;
    return (uintptr_t)index + 1;
}

static void on_dir_begin(const ScanDirInfo *dir, void *ctx)
{
    DetectCtx *c = ctx;

    free(c->cur_dir);
    c->cur_dir = path_strip_extended(dir->path);
    c->cur_first = c->r->len;
    c->cur_marker = 0;

    if (c->progress != NULL) {
        CleanupProgress p;
        const ScanReport *rep = &c->r->report;

        memset(&p, 0, sizeof(p));
        p.files = rep->file_count;
        p.dirs = rep->dir_count;
        p.errors = rep->error_count;
        p.dirs_scanned = c->dirs_scanned;
        p.dirs_pending = dir->pending;
        p.items = c->r->len;
        p.item_bytes = c->found_bytes;
        p.current_dir = c->cur_dir != NULL ? c->cur_dir : dir->path;
        c->progress(&p, c->ctx);
    }
}

static void on_dir_end(const ScanDirInfo *dir, ScanStatus result, void *ctx)
{
    DetectCtx *c = ctx;
    size_t i;

    c->dirs_scanned++;
    if (dir->id != 0) {
        CleanupItem *owner = &c->r->items[dir->id - 1];
        if (owner->pending > 0) {
            owner->pending--;
        }
        if (result != SCAN_COMPLETE) {
            owner->flags |= CLEANUP_ITEM_INCOMPLETE;
        }
        return;
    }
    /* 이 폴더의 직속 후보는 모두 [cur_first, len) 에 있다(하위 폴더는 이후에 열거된다). */
    if (c->cur_marker) {
        for (i = c->cur_first; i < c->r->len; i++) {
            CleanupItem *item = &c->r->items[i];
            if ((item->flags & CLEANUP_ITEM_DIR) && cleanup_needs_marker(item->category)) {
                item->flags |= CLEANUP_ITEM_CONFIRMED;
            }
        }
    }
}

static int should_cancel(void *ctx)
{
    DetectCtx *c = ctx;

    return c->cancel(c->ctx);
}

static int should_skip_dir(const ScanEntry *entry, const ScanDirInfo *parent, void *ctx)
{
    DetectCtx *c = ctx;

    return c->skip_protected && cleanup_protect_match(c->protect, parent->path, entry->name);
}

static void finalize(CleanupResult *r)
{
    size_t i;

    for (i = 0; i < r->len; i++) {
        CleanupItem *item = &r->items[i];
        /* 열거하지 못하고 남은 하위 폴더가 있으면 용량이 실제보다 작다. */
        if (item->pending > 0) {
            item->flags |= CLEANUP_ITEM_INCOMPLETE;
            item->pending = 0;
        }
        if (r->root_protected) {
            item->flags |= CLEANUP_ITEM_PROTECTED;
        }
        r->total_bytes += item->bytes;
        r->category_bytes[item->category] += item->bytes;
        r->category_count[item->category]++;
    }
}

ScanStatus cleanup_scan(CleanupResult *out, const wchar_t *root, const CleanupProtect *protect,
                        cleanup_progress_fn progress, cleanup_cancel_fn cancel, void *ctx)
{
    ScanHooks hooks = {on_entry, on_dir_begin, on_dir_end, NULL, should_skip_dir};
    DetectCtx c;

    if (out == NULL) {
        return SCAN_FATAL;
    }
    cleanup_result_free(out);
    if (root == NULL) {
        out->report.aborted = 1;
        out->status = SCAN_FATAL;
        return SCAN_FATAL;
    }
    out->root = path_full(root);
    if (out->root == NULL) {
        out->root = path_dup(root);
    }
    out->root_protected = cleanup_protect_contains(protect, out->root);

    memset(&c, 0, sizeof(c));
    c.r = out;
    c.protect = protect;
    /* 사용자가 보호 경로 안을 직접 골랐으면 제외하지 않고 경고만 한다. */
    c.skip_protected = !out->root_protected;
    c.progress = progress;
    c.cancel = cancel;
    c.ctx = ctx;
    if (cancel != NULL) {
        hooks.should_cancel = should_cancel;
    }

    out->status = scan_tree_ex(root, SCAN_DEPTH_UNLIMITED, 0, &out->report, &hooks, &c);
    free(c.cur_dir);
    finalize(out);

    if (progress != NULL) {
        CleanupProgress p;
        memset(&p, 0, sizeof(p));
        p.files = out->report.file_count;
        p.dirs = out->report.dir_count;
        p.errors = out->report.error_count;
        p.dirs_scanned = c.dirs_scanned;
        p.items = out->len;
        p.item_bytes = out->total_bytes;
        p.finished = 1;
        progress(&p, ctx);
    }
    return out->status;
}

typedef struct {
    uint64_t bytes;
    const wchar_t *path;
    size_t index;
} SortItem;

static int cmp_bytes_desc(const void *a, const void *b)
{
    const SortItem *x = a, *y = b;
    int by_path;

    if (x->bytes != y->bytes) {
        return x->bytes > y->bytes ? -1 : 1;
    }
    by_path = _wcsicmp(x->path, y->path);
    if (by_path != 0) {
        return by_path;
    }
    return x->index < y->index ? -1 : x->index > y->index;
}

size_t cleanup_select(const CleanupResult *r, unsigned mask, size_t *out, size_t cap,
                      uint64_t *bytes)
{
    SortItem *sorted;
    size_t i, count = 0, n;
    uint64_t sum = 0;

    if (bytes != NULL) {
        *bytes = 0;
    }
    if (r == NULL) {
        return 0;
    }
    for (i = 0; i < r->len; i++) {
        if (mask & CLEANUP_MASK(r->items[i].category)) {
            count++;
            sum += r->items[i].bytes;
        }
    }
    if (bytes != NULL) {
        *bytes = sum;
    }
    if (out == NULL || cap == 0 || count == 0) {
        return count;
    }

    sorted = malloc(count * sizeof(*sorted));
    n = 0;
    for (i = 0; i < r->len; i++) {
        if (!(mask & CLEANUP_MASK(r->items[i].category))) {
            continue;
        }
        if (sorted == NULL) {
            /* 정렬할 메모리가 없으면 발견 순서대로라도 돌려준다. */
            if (n < cap) {
                out[n] = i;
            }
            n++;
            continue;
        }
        sorted[n].bytes = r->items[i].bytes;
        sorted[n].path = r->items[i].path;
        sorted[n].index = i;
        n++;
    }
    if (sorted != NULL) {
        qsort(sorted, count, sizeof(*sorted), cmp_bytes_desc);
        for (i = 0; i < count && i < cap; i++) {
            out[i] = sorted[i].index;
        }
        free(sorted);
    }
    return count;
}
