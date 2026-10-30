#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "scan_model.h"

/* 열거를 시작한 노드 표시(build 내부에서만 쓰고 끝나면 지운다). */
#define NODE_BEGUN (1u << 31)

/* 훅 id 는 노드 인덱스 + 1 이다(0 은 "노드 없음"). */
#define ID_OF(index) ((uintptr_t)(index) + 1)
#define INDEX_OF(id) ((size_t)(id) - 1)

/* 이름 덩어리 하나의 기본 크기(문자 수). 이보다 긴 이름은 따로 덩어리를 만든다. */
#define NAME_CHUNK_CHARS 32768

struct ScanNameChunk {
    ScanNameChunk *next;
    size_t used;
    size_t cap;
    wchar_t data[];
};

/* name 의 앞 len 문자를 이름 저장소에 복사한다. 메모리가 부족하면 NULL. */
static wchar_t *name_dup(ScanModel *m, const wchar_t *name, size_t len)
{
    ScanNameChunk *c = m->names;
    wchar_t *dst;

    if (c == NULL || c->cap - c->used < len + 1) {
        size_t cap = len + 1 > NAME_CHUNK_CHARS ? len + 1 : NAME_CHUNK_CHARS;
        c = malloc(sizeof(*c) + cap * sizeof(wchar_t));
        if (c == NULL) {
            return NULL;
        }
        c->next = m->names;
        c->used = 0;
        c->cap = cap;
        m->names = c;
    }
    dst = c->data + c->used;
    memcpy(dst, name, len * sizeof(wchar_t));
    dst[len] = L'\0';
    c->used += len + 1;
    return dst;
}

void scan_model_init(ScanModel *model)
{
    memset(model, 0, sizeof(*model));
    scan_report_init(&model->report);
    model->status = SCAN_COMPLETE;
}

void scan_model_free(ScanModel *model)
{
    ScanNameChunk *c;

    if (model == NULL) {
        return;
    }
    for (c = model->names; c != NULL;) {
        ScanNameChunk *next = c->next;
        free(c);
        c = next;
    }
    free(model->nodes);
    scan_report_free(&model->report);
    scan_model_init(model);
}

/* name 을 이름 저장소에 복사해 노드를 붙인다. 실패하면 SCAN_NODE_NONE. */
static size_t add_node(ScanModel *m, const wchar_t *name, size_t parent, uint32_t flags)
{
    ScanNode *node;
    size_t index;
    wchar_t *copy;

    if (name == NULL) {
        m->out_of_memory = 1;
        return SCAN_NODE_NONE;
    }
    if (m->len == m->cap) {
        /* 1.5배씩 늘려 큰 트리에서 재할당 순간의 최대 메모리를 줄인다. */
        size_t cap = m->cap ? m->cap + m->cap / 2 : 64;
        ScanNode *grown = realloc(m->nodes, cap * sizeof(*grown));
        if (grown == NULL) {
            m->out_of_memory = 1;
            return SCAN_NODE_NONE;
        }
        m->nodes = grown;
        m->cap = cap;
    }
    copy = name_dup(m, name, wcslen(name));
    if (copy == NULL) {
        m->out_of_memory = 1;
        return SCAN_NODE_NONE;
    }

    index = m->len++;
    node = &m->nodes[index];
    memset(node, 0, sizeof(*node));
    node->name = copy;
    node->parent = parent;
    node->first_child = SCAN_NODE_NONE;
    node->last_child = SCAN_NODE_NONE;
    node->next_sibling = SCAN_NODE_NONE;
    node->flags = flags;

    if (parent != SCAN_NODE_NONE) {
        ScanNode *p = &m->nodes[parent];
        node->depth = p->depth + 1;
        if (p->last_child == SCAN_NODE_NONE) {
            p->first_child = index;
        } else {
            m->nodes[p->last_child].next_sibling = index;
        }
        p->last_child = index;
        p->child_count++;
    }
    return index;
}

typedef struct {
    ScanModel *model;
    scan_progress_fn progress;
    scan_cancel_fn cancel;
    void *ctx;
    uint64_t dirs_scanned;
} BuildCtx;

static void fill_progress(const BuildCtx *b, ScanProgress *p)
{
    const ScanReport *r = &b->model->report;

    memset(p, 0, sizeof(*p));
    p->files = r->file_count;
    p->dirs = r->dir_count;
    p->bytes = r->total_bytes;
    p->errors = r->error_count;
    p->dirs_scanned = b->dirs_scanned;
}

static uintptr_t on_entry(const ScanEntry *entry, uintptr_t parent_id,
                          int will_descend, void *ctx)
{
    BuildCtx *b = ctx;
    ScanModel *m = b->model;
    size_t parent, index;
    uint32_t flags = 0;

    /* 부모 노드를 만들지 못한 하위 트리는 report 에만 집계된다. */
    if (parent_id == 0) {
        return 0;
    }
    parent = INDEX_OF(parent_id);

    if (!entry->is_dir) {
        m->nodes[parent].own_files++;
        m->nodes[parent].own_bytes += entry->size;
        m->nodes[parent].own_alloc += entry->alloc_size;
        return 0;
    }

    if (entry->is_reparse) {
        flags |= SCAN_NODE_REPARSE;
    } else if (!will_descend) {
        flags |= SCAN_NODE_DEPTH_LIMIT;
    }
    index = add_node(m, entry->name, parent, flags);
    return index == SCAN_NODE_NONE ? 0 : ID_OF(index);
}

static void on_dir_begin(const ScanDirInfo *dir, void *ctx)
{
    BuildCtx *b = ctx;
    ScanProgress p;
    wchar_t *display;

    if (dir->id != 0) {
        b->model->nodes[INDEX_OF(dir->id)].flags |= NODE_BEGUN;
    }
    if (b->progress == NULL) {
        return;
    }
    display = path_strip_extended(dir->path);
    fill_progress(b, &p);
    p.dirs_pending = dir->pending;
    p.depth = dir->depth;
    p.current_dir = display != NULL ? display : dir->path;
    b->progress(&p, b->ctx);
    free(display);
}

static void on_dir_end(const ScanDirInfo *dir, ScanStatus result, void *ctx)
{
    BuildCtx *b = ctx;

    b->dirs_scanned++;
    if (dir->id == 0) {
        return;
    }
    if (result == SCAN_FATAL) {
        b->model->nodes[INDEX_OF(dir->id)].flags |= SCAN_NODE_FAILED;
    } else if (result == SCAN_PARTIAL) {
        b->model->nodes[INDEX_OF(dir->id)].flags |= SCAN_NODE_INCOMPLETE;
    }
}

static int should_cancel(void *ctx)
{
    BuildCtx *b = ctx;

    return b->cancel(b->ctx);
}

/* 플래그를 마무리하고 하위 트리 합계를 계산한다(재귀 없이 역순 한 번). */
static void finalize(ScanModel *m)
{
    size_t i;

    for (i = 0; i < m->len; i++) {
        ScanNode *n = &m->nodes[i];
        /* 들어가야 했지만 스택에 넣지 못했거나 취소로 열거하지 못한 폴더. */
        if (!(n->flags & (NODE_BEGUN | SCAN_NODE_REPARSE | SCAN_NODE_DEPTH_LIMIT))) {
            n->flags |= m->report.cancelled ? SCAN_NODE_CANCELLED : SCAN_NODE_FAILED;
        }
        n->flags &= ~NODE_BEGUN;
        n->total_files = n->own_files;
        n->total_bytes = n->own_bytes;
        n->total_alloc = n->own_alloc;
        n->total_dirs = 0;
    }

    /* 자식은 항상 부모보다 인덱스가 크므로 뒤에서부터 부모로 더하면 된다. */
    for (i = m->len; i-- > 1;) {
        const ScanNode *n = &m->nodes[i];
        ScanNode *p = &m->nodes[n->parent];
        p->total_files += n->total_files;
        p->total_bytes += n->total_bytes;
        p->total_alloc += n->total_alloc;
        p->total_dirs += n->total_dirs + 1;
    }
}

ScanStatus scan_model_build(ScanModel *model, const wchar_t *root, uint32_t max_depth,
                            scan_progress_fn progress, void *ctx)
{
    return scan_model_build_ex(model, root, max_depth, progress, NULL, ctx);
}

ScanStatus scan_model_build_ex(ScanModel *model, const wchar_t *root, uint32_t max_depth,
                               scan_progress_fn progress, scan_cancel_fn cancel,
                               void *ctx)
{
    ScanHooks hooks = {on_entry, on_dir_begin, on_dir_end, NULL};
    BuildCtx b;
    size_t root_index = SCAN_NODE_NONE;
    wchar_t *full;

    scan_model_free(model);
    model->max_depth = max_depth;

    full = root != NULL && root[0] != L'\0' ? path_full(root) : NULL;
    if (full != NULL) {
        wchar_t *display = path_strip_extended(full);
        root_index = add_node(model, display, SCAN_NODE_NONE, 0);
        free(display);
        free(full);
    }

    b.model = model;
    b.progress = progress;
    b.cancel = cancel;
    b.ctx = ctx;
    if (cancel != NULL) {
        hooks.should_cancel = should_cancel;
    }
    b.dirs_scanned = 0;
    model->status = scan_tree_ex(root, max_depth,
                                 root_index == SCAN_NODE_NONE ? 0 : ID_OF(root_index),
                                 &model->report, &hooks, &b);
    finalize(model);

    if (progress != NULL) {
        ScanProgress p;
        fill_progress(&b, &p);
        p.finished = 1;
        p.status = model->status;
        progress(&p, ctx);
    }
    return model->status;
}

double scan_progress_fraction(const ScanProgress *progress)
{
    double known;

    if (progress->finished) {
        return 1.0;
    }
    known = (double)progress->dirs_scanned + (double)progress->dirs_pending;
    return known > 0.0 ? (double)progress->dirs_scanned / known : 0.0;
}

const ScanNode *scan_model_node(const ScanModel *model, size_t index)
{
    return model != NULL && index < model->len ? &model->nodes[index] : NULL;
}

typedef struct {
    uint64_t bytes;
    const wchar_t *name;
    size_t index;
} SortItem;

static int cmp_size_desc(const void *a, const void *b)
{
    const SortItem *x = a, *y = b;
    int by_name;

    if (x->bytes != y->bytes) {
        return x->bytes > y->bytes ? -1 : 1;
    }
    by_name = _wcsicmp(x->name, y->name);
    if (by_name != 0) {
        return by_name;
    }
    return x->index < y->index ? -1 : x->index > y->index;
}

static int cmp_name(const void *a, const void *b)
{
    const SortItem *x = a, *y = b;
    int by_name = _wcsicmp(x->name, y->name);

    if (by_name != 0) {
        return by_name;
    }
    return x->index < y->index ? -1 : x->index > y->index;
}

size_t scan_model_children(const ScanModel *model, size_t index, ScanChildOrder order,
                           size_t *out, size_t cap)
{
    const ScanNode *node = scan_model_node(model, index);
    SortItem *items;
    size_t count, i, child;

    if (node == NULL) {
        return 0;
    }
    count = node->child_count;
    if (out == NULL || cap == 0) {
        return count;
    }

    if (order == SCAN_ORDER_SCAN) {
        for (i = 0, child = node->first_child; i < cap && child != SCAN_NODE_NONE;
             i++, child = model->nodes[child].next_sibling) {
            out[i] = child;
        }
        return count;
    }

    items = malloc(count * sizeof(*items));
    if (items == NULL) {
        return 0;
    }
    for (i = 0, child = node->first_child; child != SCAN_NODE_NONE;
         i++, child = model->nodes[child].next_sibling) {
        items[i].bytes = model->nodes[child].total_bytes;
        items[i].name = model->nodes[child].name;
        items[i].index = child;
    }
    qsort(items, count, sizeof(*items),
          order == SCAN_ORDER_NAME ? cmp_name : cmp_size_desc);
    for (i = 0; i < count && i < cap; i++) {
        out[i] = items[i].index;
    }
    free(items);
    return count;
}

/* 깊은 트리에서도 스택을 쓰지 않도록 조상 목록을 모아 앞에서부터 잇는다. */
wchar_t *scan_model_path(const ScanModel *model, size_t index)
{
    const ScanNode *node = scan_model_node(model, index);
    size_t *chain;
    size_t depth, i;
    wchar_t *path;

    if (node == NULL) {
        return NULL;
    }
    depth = node->depth;
    chain = malloc((depth + 1) * sizeof(*chain));
    if (chain == NULL) {
        return NULL;
    }
    for (i = depth + 1; i-- > 0;) {
        chain[i] = index;
        index = model->nodes[index].parent;
    }

    path = path_dup(model->nodes[chain[0]].name);
    for (i = 1; path != NULL && i <= depth; i++) {
        wchar_t *joined = path_join(path, model->nodes[chain[i]].name);
        free(path);
        path = joined;
    }
    free(chain);
    return path;
}

typedef struct {
    uint64_t key;
    size_t index;
} RankItem;

static int cmp_rank(const void *a, const void *b)
{
    const RankItem *x = a, *y = b;

    if (x->key != y->key) {
        return x->key > y->key ? -1 : 1;
    }
    return x->index < y->index ? -1 : x->index > y->index;
}

size_t scan_model_rank(const ScanModel *model, ScanRankKey key, size_t *out, size_t n)
{
    RankItem *items;
    size_t count, i;

    if (model == NULL || model->len <= 1 || out == NULL || n == 0) {
        return 0;
    }
    count = model->len - 1;
    items = malloc(count * sizeof(*items));
    if (items == NULL) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const ScanNode *node = &model->nodes[i + 1];
        items[i].key = key == SCAN_RANK_OWN_BYTES     ? node->own_bytes
                       : key == SCAN_RANK_TOTAL_ALLOC ? node->total_alloc
                                                      : node->total_bytes;
        items[i].index = i + 1;
    }
    qsort(items, count, sizeof(*items), cmp_rank);
    for (i = 0; i < count && i < n; i++) {
        out[i] = items[i].index;
    }
    free(items);
    return i;
}
