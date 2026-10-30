#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path.h"
#include "scan_job.h"
#include "scan_model.h"

/* 공용 컨트롤 v6(비주얼 스타일). */
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_APP_SCAN_PROGRESS (WM_APP + 1)
#define WM_APP_SCAN_DONE (WM_APP + 2)

/* 진행률 알림 간격과 용량 순위 개수. */
#define PROGRESS_INTERVAL_MS 100
#define RANK_COUNT 100
#define PROGRESS_RANGE 1000

/* --self-test 의 전체 제한 시간. */
#define SELF_TEST_TIMEOUT_MS 60000
#define TIMER_SELF_TEST 1

enum {
    IDC_PATH = 100,
    IDC_BROWSE,
    IDC_SCAN,
    IDC_CANCEL,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_TREE,
    IDC_LIST
};

/* 순위 목록 열. */
enum { COL_RANK, COL_PATH, COL_SIZE, COL_FILES, COL_SHARE, COL_COUNT };

typedef enum {
    SELF_TEST_NONE = 0,
    SELF_TEST_SCAN,  /* 스캔 → 화면 검증 → 종료 */
    SELF_TEST_CANCEL /* 스캔 시작 직후 취소 → 화면 검증 → 종료 */
} SelfTest;

typedef struct {
    HWND hwnd;
    HWND path, browse, scan, cancel, progress, status, tree, list;
    HFONT font;
    int dpi;

    ScanJob *job;
    ScanModel model;
    int has_model;
    int progress_pos;

    SelfTest self_test;
    int self_test_exit;
} App;

static App g_app;

static int scale(int px)
{
    return MulDiv(px, g_app.dpi, 96);
}

/* "512 B", "1.5 KiB" 처럼 짧게 쓴다. */
static void format_size(uint64_t bytes, wchar_t *buf, size_t len)
{
    static const wchar_t *units[] = {L"KiB", L"MiB", L"GiB", L"TiB", L"PiB", L"EiB"};
    double value = (double)bytes;
    int unit = -1;

    if (bytes < 1024) {
        swprintf(buf, len, L"%llu B", (unsigned long long)bytes);
        return;
    }
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        unit++;
    }
    swprintf(buf, len, value < 10.0 ? L"%.2f %ls" : L"%.1f %ls", value, units[unit]);
}

static double share_of(uint64_t part, uint64_t whole)
{
    return whole > 0 ? 100.0 * (double)part / (double)whole : 0.0;
}

/* 트리·순위에 덧붙이는 상태 표시. 없으면 빈 문자열. */
static const wchar_t *flag_label(uint32_t flags)
{
    if (flags & SCAN_NODE_FAILED) {
        return L" [읽기 실패]";
    }
    if (flags & SCAN_NODE_CANCELLED) {
        return L" [취소됨]";
    }
    if (flags & SCAN_NODE_INCOMPLETE) {
        return L" [일부 누락]";
    }
    if (flags & SCAN_NODE_REPARSE) {
        return L" [링크]";
    }
    if (flags & SCAN_NODE_DEPTH_LIMIT) {
        return L" [깊이 제한]";
    }
    return L"";
}

/* ---- 레이아웃 ---- */

static void layout(void)
{
    RECT rc;
    int m = scale(8), h = scale(24), bw = scale(84), gap = scale(6);
    int w, y, path_w, body_h, tree_w;

    GetClientRect(g_app.hwnd, &rc);
    w = rc.right - rc.left;

    y = m;
    path_w = w - 2 * m - 3 * (bw + gap);
    if (path_w < scale(80)) {
        path_w = scale(80);
    }
    MoveWindow(g_app.path, m, y, path_w, h, TRUE);
    MoveWindow(g_app.browse, m + path_w + gap, y, bw, h, TRUE);
    MoveWindow(g_app.scan, m + path_w + gap + (bw + gap), y, bw, h, TRUE);
    MoveWindow(g_app.cancel, m + path_w + gap + 2 * (bw + gap), y, bw, h, TRUE);

    y += h + gap;
    MoveWindow(g_app.progress, m, y, w - 2 * m, scale(16), TRUE);
    y += scale(16) + gap;
    MoveWindow(g_app.status, m, y, w - 2 * m, scale(20), TRUE);
    y += scale(20) + gap;

    body_h = rc.bottom - y - m;
    if (body_h < 0) {
        body_h = 0;
    }
    tree_w = (w - 2 * m - gap) * 2 / 5;
    MoveWindow(g_app.tree, m, y, tree_w, body_h, TRUE);
    MoveWindow(g_app.list, m + tree_w + gap, y, w - 2 * m - gap - tree_w, body_h, TRUE);
}

/* ---- 결과 표시 ---- */

/* 트리 항목 표시 문자열(malloc, 호출자가 free). */
static wchar_t *tree_item_text(size_t index)
{
    const ScanNode *n = scan_model_node(&g_app.model, index);
    uint64_t whole = g_app.model.nodes[0].total_bytes;
    size_t len = wcslen(n->name) + 64;
    wchar_t size[32], *buf = malloc(len * sizeof(*buf));

    if (buf == NULL) {
        return NULL;
    }
    format_size(n->total_bytes, size, 32);
    if (index == 0) {
        swprintf(buf, len, L"%ls  —  %ls%ls", n->name, size, flag_label(n->flags));
    } else {
        swprintf(buf, len, L"%ls  —  %ls (%.1f%%)%ls", n->name, size,
                 share_of(n->total_bytes, whole), flag_label(n->flags));
    }
    return buf;
}

static HTREEITEM tree_insert(HTREEITEM parent, size_t index)
{
    TVINSERTSTRUCTW ins;
    const ScanNode *n = scan_model_node(&g_app.model, index);
    wchar_t *text = tree_item_text(index);
    HTREEITEM item;

    if (text == NULL) {
        return NULL;
    }
    memset(&ins, 0, sizeof(ins));
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
    ins.item.pszText = text;
    ins.item.lParam = (LPARAM)index;
    /* 자식은 펼칠 때 채운다(지연 로딩). */
    ins.item.cChildren = n->child_count > 0 ? 1 : 0;
    item = (HTREEITEM)SendMessageW(g_app.tree, TVM_INSERTITEMW, 0, (LPARAM)&ins);
    free(text);
    return item;
}

/* item 의 자식을 아직 넣지 않았으면 용량 순으로 넣는다. */
static void tree_populate(HTREEITEM item, size_t index)
{
    size_t count, i, *kids;

    if (TreeView_GetChild(g_app.tree, item) != NULL) {
        return;
    }
    count = scan_model_children(&g_app.model, index, SCAN_ORDER_SIZE_DESC, NULL, 0);
    if (count == 0) {
        return;
    }
    kids = malloc(count * sizeof(*kids));
    if (kids == NULL) {
        return;
    }
    count = scan_model_children(&g_app.model, index, SCAN_ORDER_SIZE_DESC, kids, count);
    SendMessageW(g_app.tree, WM_SETREDRAW, FALSE, 0);
    for (i = 0; i < count; i++) {
        tree_insert(item, kids[i]);
    }
    SendMessageW(g_app.tree, WM_SETREDRAW, TRUE, 0);
    free(kids);
}

static size_t tree_item_index(HTREEITEM item)
{
    TVITEMW tv;

    memset(&tv, 0, sizeof(tv));
    tv.mask = TVIF_PARAM;
    tv.hItem = item;
    if (!TreeView_GetItem(g_app.tree, &tv)) {
        return SCAN_NODE_NONE;
    }
    return (size_t)tv.lParam;
}

static void list_set(int row, int col, const wchar_t *text)
{
    LVITEMW lv;

    memset(&lv, 0, sizeof(lv));
    lv.iSubItem = col;
    lv.pszText = (wchar_t *)text;
    SendMessageW(g_app.list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&lv);
}

static void fill_rank(void)
{
    size_t top[RANK_COUNT];
    size_t count, i;
    uint64_t whole = g_app.model.nodes[0].total_bytes;

    count = scan_model_rank(&g_app.model, SCAN_RANK_TOTAL_BYTES, top, RANK_COUNT);
    SendMessageW(g_app.list, WM_SETREDRAW, FALSE, 0);
    for (i = 0; i < count; i++) {
        const ScanNode *n = &g_app.model.nodes[top[i]];
        wchar_t buf[64], *path;
        LVITEMW lv;
        int row;

        swprintf(buf, 64, L"%u", (unsigned)(i + 1));
        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_TEXT | LVIF_PARAM;
        lv.iItem = (int)i;
        lv.pszText = buf;
        lv.lParam = (LPARAM)top[i];
        row = (int)SendMessageW(g_app.list, LVM_INSERTITEMW, 0, (LPARAM)&lv);
        if (row < 0) {
            break;
        }

        path = scan_model_path(&g_app.model, top[i]);
        if (path != NULL) {
            size_t plen = wcslen(path) + 16;
            wchar_t *text = malloc(plen * sizeof(*text));
            if (text != NULL) {
                swprintf(text, plen, L"%ls%ls", path, flag_label(n->flags));
                list_set(row, COL_PATH, text);
                free(text);
            }
            free(path);
        }
        format_size(n->total_bytes, buf, 64);
        list_set(row, COL_SIZE, buf);
        swprintf(buf, 64, L"%llu", (unsigned long long)n->total_files);
        list_set(row, COL_FILES, buf);
        swprintf(buf, 64, L"%.1f%%", share_of(n->total_bytes, whole));
        list_set(row, COL_SHARE, buf);
    }
    SendMessageW(g_app.list, WM_SETREDRAW, TRUE, 0);
}

static void clear_results(void)
{
    TreeView_DeleteAllItems(g_app.tree);
    ListView_DeleteAllItems(g_app.list);
    scan_model_free(&g_app.model);
    g_app.has_model = 0;
}

static void show_results(void)
{
    HTREEITEM root;

    if (g_app.model.len == 0) {
        return;
    }
    root = tree_insert(TVI_ROOT, 0);
    if (root != NULL) {
        tree_populate(root, 0);
        TreeView_Expand(g_app.tree, root, TVE_EXPAND);
        TreeView_SelectItem(g_app.tree, root);
    }
    fill_rank();
}

/* 순위 목록에서 고른 폴더를 트리에서 펼쳐 보여 준다. */
static void reveal_in_tree(size_t index)
{
    const ScanNode *n = scan_model_node(&g_app.model, index);
    size_t *chain, depth, i;
    HTREEITEM item;

    if (n == NULL) {
        return;
    }
    depth = n->depth;
    chain = malloc((depth + 1) * sizeof(*chain));
    if (chain == NULL) {
        return;
    }
    for (i = depth + 1; i-- > 0;) {
        chain[i] = index;
        index = g_app.model.nodes[index].parent;
    }

    item = TreeView_GetRoot(g_app.tree);
    for (i = 1; item != NULL && i <= depth; i++) {
        HTREEITEM child;
        tree_populate(item, chain[i - 1]);
        TreeView_Expand(g_app.tree, item, TVE_EXPAND);
        for (child = TreeView_GetChild(g_app.tree, item); child != NULL;
             child = TreeView_GetNextSibling(g_app.tree, child)) {
            if (tree_item_index(child) == chain[i]) {
                break;
            }
        }
        item = child;
    }
    free(chain);

    if (item != NULL) {
        TreeView_SelectItem(g_app.tree, item);
        TreeView_EnsureVisible(g_app.tree, item);
        SetFocus(g_app.tree);
    }
}

/* ---- 스캔 ---- */

static void set_scanning(int scanning)
{
    EnableWindow(g_app.scan, !scanning);
    EnableWindow(g_app.browse, !scanning);
    EnableWindow(g_app.path, !scanning);
    EnableWindow(g_app.cancel, scanning);
}

static void set_status(const wchar_t *text)
{
    SetWindowTextW(g_app.status, text);
}

/* 스캔 스레드에서 호출된다. 창이 이미 없으면 PostMessage 가 실패할 뿐이다. */
static void on_job_notify(int finished, void *ctx)
{
    HWND hwnd = ctx;

    PostMessageW(hwnd, finished ? WM_APP_SCAN_DONE : WM_APP_SCAN_PROGRESS, 0, 0);
}

static void start_scan(void)
{
    wchar_t *target;
    int len;

    if (g_app.job != NULL) {
        return;
    }
    len = GetWindowTextLengthW(g_app.path);
    target = malloc(((size_t)len + 1) * sizeof(*target));
    if (target == NULL) {
        return;
    }
    GetWindowTextW(g_app.path, target, len + 1);
    if (target[0] == L'\0') {
        free(target);
        set_status(L"스캔할 폴더를 입력하거나 [찾아보기]로 선택하세요.");
        return;
    }

    clear_results();
    g_app.progress_pos = 0;
    SendMessageW(g_app.progress, PBM_SETPOS, 0, 0);
    g_app.job = scan_job_start(target, SCAN_DEPTH_UNLIMITED, PROGRESS_INTERVAL_MS,
                               on_job_notify, g_app.hwnd);
    free(target);
    if (g_app.job == NULL) {
        set_status(L"스캔을 시작하지 못했습니다(메모리 또는 스레드 생성 실패).");
        return;
    }
    set_scanning(1);
    set_status(L"스캔 중...");
}

static void cancel_scan(void)
{
    if (g_app.job != NULL) {
        scan_job_cancel(g_app.job);
        EnableWindow(g_app.cancel, FALSE);
        set_status(L"취소하는 중...");
    }
}

static void update_progress(void)
{
    ScanJobSnapshot snap;
    wchar_t size[32], text[SCAN_JOB_DIR_LEN + 160];
    int pos;

    if (g_app.job == NULL) {
        return;
    }
    scan_job_progress(g_app.job, &snap);
    if (snap.cancel_requested) {
        return;
    }
    /* 남은 폴더 수가 늘면 비율이 줄 수 있으므로 막대는 뒤로 가지 않게 한다. */
    pos = (int)(scan_progress_fraction(&snap.progress) * PROGRESS_RANGE);
    if (pos > g_app.progress_pos) {
        g_app.progress_pos = pos;
        SendMessageW(g_app.progress, PBM_SETPOS, (WPARAM)pos, 0);
    }
    format_size(snap.progress.bytes, size, 32);
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"스캔 중... 파일 %llu · 폴더 %llu · %ls · 오류 %llu  |  %ls",
             (unsigned long long)snap.progress.files, (unsigned long long)snap.progress.dirs,
             size, (unsigned long long)snap.progress.errors, snap.current_dir);
    set_status(text);
}

static void finish_scan(void)
{
    ScanJobSnapshot snap;
    ScanStatus status;
    const ScanReport *r;
    wchar_t size[32], text[512];
    const wchar_t *head;

    if (g_app.job == NULL) {
        return;
    }
    scan_job_progress(g_app.job, &snap);
    status = scan_job_finish(g_app.job, &g_app.model);
    g_app.job = NULL;
    g_app.has_model = 1;
    r = &g_app.model.report;

    if (r->cancelled) {
        head = L"취소됨(그때까지 집계)";
    } else if (status == SCAN_COMPLETE) {
        head = L"완료";
    } else if (status == SCAN_PARTIAL) {
        head = L"완료(일부 폴더를 읽지 못함)";
    } else {
        head = L"스캔 실패";
    }

    if (status == SCAN_FATAL) {
        const wchar_t *kind = r->errors_len > 0 ? scan_error_kind_name(r->errors[0].kind)
                                                : L"other";
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%ls: 폴더를 열 수 없습니다 (%ls).",
                 head, kind);
        SendMessageW(g_app.progress, PBM_SETPOS, 0, 0);
    } else {
        format_size(r->total_bytes, size, 32);
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"%ls: 파일 %llu · 폴더 %llu · %ls · 오류 %llu · 건너뛴 폴더 %llu · %.1f초",
                 head, (unsigned long long)r->file_count, (unsigned long long)r->dir_count,
                 size, (unsigned long long)r->error_count,
                 (unsigned long long)r->skipped_dir_count, (double)snap.elapsed_ms / 1000.0);
        if (!r->cancelled) {
            SendMessageW(g_app.progress, PBM_SETPOS, PROGRESS_RANGE, 0);
        }
    }
    set_status(text);
    set_scanning(0);
    show_results();
}

static void browse_folder(void)
{
    BROWSEINFOW bi;
    PIDLIST_ABSOLUTE pidl;
    wchar_t path[MAX_PATH];

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_app.hwnd;
    bi.lpszTitle = L"스캔할 폴더를 선택하세요.";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl == NULL) {
        return;
    }
    if (SHGetPathFromIDListW(pidl, path)) {
        SetWindowTextW(g_app.path, path);
    }
    CoTaskMemFree(pidl);
}

/* ---- 자체 검사(--self-test) ---- */

static int count_tree_children(HTREEITEM item)
{
    int n = 0;
    HTREEITEM child;

    for (child = TreeView_GetChild(g_app.tree, item); child != NULL;
         child = TreeView_GetNextSibling(g_app.tree, child)) {
        n++;
    }
    return n;
}

#define SELF_CHECK(cond)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            wchar_t msg_[256];                                               \
            swprintf(msg_, 256, L"self-test failed: %hs (line %d)\n", #cond, \
                     __LINE__);                                              \
            OutputDebugStringW(msg_);                                        \
            fwprintf(stderr, L"%ls", msg_);                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* 스캔이 끝난 뒤 화면 상태가 모델과 맞는지 확인하고 종료 코드를 정한다. */
static int verify_screen(void)
{
    int failures = 0;
    HTREEITEM root = TreeView_GetRoot(g_app.tree);
    size_t expect_rank = g_app.model.len > 0 ? g_app.model.len - 1 : 0;
    int rows = ListView_GetItemCount(g_app.list);
    int pos = (int)SendMessageW(g_app.progress, PBM_GETPOS, 0, 0);

    if (expect_rank > RANK_COUNT) {
        expect_rank = RANK_COUNT;
    }
    SELF_CHECK(g_app.job == NULL);
    SELF_CHECK(g_app.has_model);
    SELF_CHECK(IsWindowEnabled(g_app.scan) && !IsWindowEnabled(g_app.cancel));
    SELF_CHECK(g_app.model.len > 0 && root != NULL);
    SELF_CHECK(rows == (int)expect_rank);

    if (root != NULL && g_app.model.len > 0) {
        size_t first;
        HTREEITEM child = TreeView_GetChild(g_app.tree, root);

        SELF_CHECK(tree_item_index(root) == 0);
        SELF_CHECK(count_tree_children(root) == (int)g_app.model.nodes[0].child_count);
        /* 트리 자식은 용량 내림차순. */
        if (child != NULL &&
            scan_model_children(&g_app.model, 0, SCAN_ORDER_SIZE_DESC, &first, 1) > 0) {
            SELF_CHECK(tree_item_index(child) == first);
        }
    }
    if (rows > 0) {
        size_t top;
        LVITEMW lv;
        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_PARAM;
        lv.iItem = 0;
        SendMessageW(g_app.list, LVM_GETITEMW, 0, (LPARAM)&lv);
        SELF_CHECK(scan_model_rank(&g_app.model, SCAN_RANK_TOTAL_BYTES, &top, 1) == 1 &&
                   (size_t)lv.lParam == top);

        /* 가장 깊은 순위 항목을 트리에서 찾아 선택할 수 있어야 한다. */
        reveal_in_tree((size_t)lv.lParam);
        SELF_CHECK(tree_item_index(TreeView_GetSelection(g_app.tree)) == (size_t)lv.lParam);
    }

    if (g_app.self_test == SELF_TEST_SCAN) {
        SELF_CHECK(g_app.model.report.cancelled == 0);
        SELF_CHECK(g_app.model.status != SCAN_FATAL);
        SELF_CHECK(pos == PROGRESS_RANGE);
    } else if (g_app.model.report.cancelled) {
        size_t i;
        /* 취소 직후라면 열거하지 못한 폴더가 남는다. */
        SELF_CHECK(g_app.model.status == SCAN_PARTIAL);
        for (i = 1; i < g_app.model.len; i++) {
            if (g_app.model.nodes[i].flags & SCAN_NODE_CANCELLED) {
                break;
            }
        }
        SELF_CHECK(g_app.model.report.skipped_dir_count == 0 || i < g_app.model.len);
    }
    return failures == 0 ? 0 : 1;
}

/* ---- 창 프로시저 ---- */

static HWND make_child(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
                       int id)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                             g_app.hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_app.font, FALSE);
    return h;
}

static void create_controls(void)
{
    static const struct {
        const wchar_t *title;
        int width;
        int fmt;
    } cols[COL_COUNT] = {
        {L"순위", 48, LVCFMT_RIGHT},
        {L"폴더", 360, LVCFMT_LEFT},
        {L"용량", 90, LVCFMT_RIGHT},
        {L"파일 수", 80, LVCFMT_RIGHT},
        {L"비율", 64, LVCFMT_RIGHT},
    };
    NONCLIENTMETRICSW ncm;
    int i;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_app.font = CreateFontIndirectW(&ncm.lfMessageFont);

    g_app.path = make_child(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                            IDC_PATH);
    g_app.browse = make_child(L"BUTTON", L"찾아보기...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                              IDC_BROWSE);
    g_app.scan = make_child(L"BUTTON", L"스캔", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDC_SCAN);
    g_app.cancel = make_child(L"BUTTON", L"취소", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_CANCEL);
    g_app.progress = make_child(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, IDC_PROGRESS);
    g_app.status = make_child(L"STATIC", L"폴더를 선택하고 [스캔]을 누르세요.",
                              SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS, 0, IDC_STATUS);
    g_app.tree = make_child(WC_TREEVIEWW, L"",
                            TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                                TVS_SHOWSELALWAYS | WS_TABSTOP,
                            WS_EX_CLIENTEDGE, IDC_TREE);
    g_app.list = make_child(WC_LISTVIEWW, L"",
                            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                            WS_EX_CLIENTEDGE, IDC_LIST);

    SendMessageW(g_app.path, EM_SETCUEBANNER, TRUE, (LPARAM)L"스캔할 폴더 경로");
    SendMessageW(g_app.progress, PBM_SETRANGE32, 0, PROGRESS_RANGE);
    ListView_SetExtendedListViewStyle(g_app.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < COL_COUNT; i++) {
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = (wchar_t *)cols[i].title;
        col.cx = scale(cols[i].width);
        col.fmt = cols[i].fmt;
        SendMessageW(g_app.list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
    set_scanning(0);
}

static LRESULT on_notify(NMHDR *hdr)
{
    if (hdr->idFrom == IDC_TREE && hdr->code == TVN_ITEMEXPANDINGW) {
        NMTREEVIEWW *tv = (NMTREEVIEWW *)hdr;
        if (tv->action == TVE_EXPAND && g_app.has_model) {
            tree_populate(tv->itemNew.hItem, (size_t)tv->itemNew.lParam);
        }
        return 0;
    }
    if (hdr->idFrom == IDC_LIST && (hdr->code == NM_DBLCLK || hdr->code == NM_RETURN)) {
        int row = ListView_GetNextItem(g_app.list, -1, LVNI_SELECTED);
        if (row >= 0 && g_app.has_model) {
            LVITEMW lv;
            memset(&lv, 0, sizeof(lv));
            lv.mask = LVIF_PARAM;
            lv.iItem = row;
            SendMessageW(g_app.list, LVM_GETITEMW, 0, (LPARAM)&lv);
            reveal_in_tree((size_t)lv.lParam);
        }
        return 0;
    }
    return 0;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        create_controls();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = scale(520);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = scale(320);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:
            browse_folder();
            return 0;
        case IDC_SCAN:
        case IDOK: /* 경로 칸에서 Enter */
            start_scan();
            return 0;
        case IDC_CANCEL:
            cancel_scan();
            return 0;
        }
        break;
    case WM_NOTIFY:
        return on_notify((NMHDR *)lp);
    case WM_APP_SCAN_PROGRESS:
        update_progress();
        return 0;
    case WM_APP_SCAN_DONE:
        finish_scan();
        if (g_app.self_test != SELF_TEST_NONE) {
            g_app.self_test_exit = verify_screen();
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_SELF_TEST) {
            fwprintf(stderr, L"self-test failed: timeout\n");
            g_app.self_test_exit = 1;
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        /* 진행 중인 스캔은 멈추고 스레드가 끝날 때까지 기다린다. */
        if (g_app.job != NULL) {
            scan_job_cancel(g_app.job);
            scan_job_finish(g_app.job, NULL);
            g_app.job = NULL;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_app.job != NULL) {
            scan_job_cancel(g_app.job);
            scan_job_finish(g_app.job, NULL);
            g_app.job = NULL;
        }
        clear_results();
        PostQuitMessage(g_app.self_test_exit);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/*
 * 사용법:
 *   bytesieve-gui [<directory>]          폴더를 채우고 바로 스캔
 *   bytesieve-gui --self-test <dir>       스캔 후 화면을 검증하고 종료(0 성공, 1 실패)
 *   bytesieve-gui --self-test-cancel <dir> 시작 직후 취소한 뒤 화면을 검증하고 종료
 */
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    MSG msg;
    HDC screen;
    wchar_t **argv;
    const wchar_t *target = NULL;
    int argc = 0;

    (void)prev;
    (void)cmd;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test") == 0) {
        g_app.self_test = SELF_TEST_SCAN;
        target = argv[2];
    } else if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test-cancel") == 0) {
        g_app.self_test = SELF_TEST_CANCEL;
        target = argv[2];
    } else if (argv != NULL && argc >= 2) {
        target = argv[1];
    }
    if (g_app.self_test != SELF_TEST_NONE) {
        /* GUI 서브시스템이라 상속받은 stderr 가 없으면 부모 콘솔에 실패 메시지를 쓴다. */
        HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
        if ((err == NULL || err == INVALID_HANDLE_VALUE) &&
            AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE *f;
            _wfreopen_s(&f, L"CONOUT$", L"w", stderr);
        }
    }

    SetProcessDPIAware();
    screen = GetDC(NULL);
    g_app.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    scan_model_init(&g_app.model);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS |
                ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ByteSieveMain";
    RegisterClassExW(&wc);

    if (CreateWindowExW(0, wc.lpszClassName, L"ByteSieve", WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, scale(1000), scale(640), NULL, NULL,
                        inst, NULL) == NULL) {
        LocalFree(argv);
        return 1;
    }
    /* 자체 검사는 화면에 띄우지 않고(컨트롤은 그대로 동작) 진행한다. */
    ShowWindow(g_app.hwnd, g_app.self_test != SELF_TEST_NONE ? SW_HIDE : show);
    UpdateWindow(g_app.hwnd);

    if (target != NULL) {
        SetWindowTextW(g_app.path, target);
        start_scan();
        if (g_app.self_test == SELF_TEST_CANCEL) {
            cancel_scan();
        }
        if (g_app.self_test != SELF_TEST_NONE) {
            if (g_app.job == NULL) {
                fwprintf(stderr, L"self-test failed: scan did not start\n");
                g_app.self_test_exit = 1;
                DestroyWindow(g_app.hwnd);
            } else {
                SetTimer(g_app.hwnd, TIMER_SELF_TEST, SELF_TEST_TIMEOUT_MS, NULL);
            }
        }
    }
    LocalFree(argv);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_app.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g_app.font);
    CoUninitialize();
    return (int)msg.wParam;
}
