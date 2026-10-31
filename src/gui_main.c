#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "gui_cache.h"
#include "gui_cleanup.h"
#include "gui_drives.h"
#include "gui_monitor.h"
#include "gui_quarantine.h"
#include "path.h"
#include "scan_export.h"
#include "scan_history.h"
#include "scan_job.h"
#include "scan_model.h"
#include "size_format.h"

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
/* --self-test-monitor 가 검증 전에 기다리는 측정 횟수. */
#define MONITOR_SELF_TEST_SAMPLES 4

enum {
    IDC_PATH = 100,
    IDC_BROWSE,
    IDC_SCAN,
    IDC_CANCEL,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_TREE,
    IDC_LIST,
    IDC_TABS,
    IDC_DRIVES,
    IDC_CLEANUP,
    IDC_MONITOR,
    IDC_VIEW,
    IDC_ANALYSIS,
    IDC_QUARANTINE,
    IDC_AUTOSAVE,
    IDC_CLEAR_HISTORY,
    IDC_EXPORT_CSV,
    IDC_EXPORT_HTML,
    IDC_CACHE,
    IDC_PRECISE
};

/* 내보내기 형식. */
typedef enum { EXPORT_KIND_CSV, EXPORT_KIND_HTML } ExportKind;

/* 탭 순서. */
enum { TAB_SCAN, TAB_DRIVES, TAB_CLEANUP, TAB_CACHE, TAB_QUARANTINE, TAB_MONITOR };

/* 순위 목록 열. */
/* COL_UNIQUE·COL_DUP 는 정밀 스캔 결과일 때만 보인다(기본 스캔이면 너비 0). */
enum {
    COL_RANK,
    COL_PATH,
    COL_SIZE,
    COL_UNIQUE,
    COL_DUP,
    COL_CHANGE,
    COL_ALLOC,
    COL_FILES,
    COL_SHARE,
    COL_COUNT
};

/* 오른쪽 목록 보기(보기 콤보 상자 순서). */
enum { VIEW_SIZE, VIEW_ALLOC, VIEW_CHANGE, VIEW_UNIQUE, VIEW_DUP, VIEW_COUNT };

/* 직전 스캔과의 비교·기록 상태. */
typedef enum {
    HIST_NONE = 0,   /* 스캔 전, 또는 스캔 실패 */
    HIST_BASELINE,   /* 직전 스냅숏이 없어 이번 스캔을 기준으로 저장했다 */
    HIST_COMPARED,   /* 직전 스냅숏과 비교했고 새 스냅숏을 저장했다 */
    HIST_CANCELLED,  /* 취소되어 저장하지 않았다(직전과 비교만 했을 수 있음) */
    HIST_FAILED,     /* 기록 위치를 정하지 못했거나 저장하지 못했다 */
    HIST_DISABLED,   /* 자동 저장이 꺼져 있어 저장하지 않았다(직전과 비교만 했을 수 있음) */
    HIST_CLEARED     /* 스캔 뒤 [기록 삭제]로 이 대상의 기록을 지웠다(변화 표시도 지움) */
} HistoryState;

typedef enum {
    SELF_TEST_NONE = 0,
    SELF_TEST_SCAN,  /* 스캔 → 화면 검증 → 종료 */
    SELF_TEST_CANCEL, /* 스캔 시작 직후 취소 → 화면 검증 → 종료 */
    SELF_TEST_DRIVES, /* 드라이브 탭 새로고침 → 화면 검증 → 종료 */
    SELF_TEST_CLEANUP,       /* 정리 후보 탭 탐지 → 화면 검증 → 종료 */
    SELF_TEST_CLEANUP_CANCEL, /* 정리 후보 탐지 시작 직후 취소 → 화면 검증 → 종료 */
    SELF_TEST_TRASH, /* 정리 후보 탐지 → 모두 선택 → 휴지통 이동(확인 창 없이) → 화면 검증 → 종료 */
    SELF_TEST_MONITOR, /* 시스템 탭 측정 몇 회 → 화면·갱신 간격·실패 표시 검증 → 종료 */
    SELF_TEST_QUARANTINE, /* 정리(격리 폴더로) → 격리 폴더 탭 → 복원(같은 이름 1개는 건너뜀) →
                           * 남은 항목 영구 삭제 → 각 단계 화면 검증 → 종료 */
    SELF_TEST_CHANGES,  /* 스캔 → 직전 스냅숏과 비교한 변화 보기 검증·출력 → 종료 */
    SELF_TEST_NO_AUTOSAVE, /* 처음 설정 출력 → 자동 저장 끔 → 스캔 → 저장하지 않았는지 검증 → 종료 */
    SELF_TEST_CLEAR_HISTORY, /* 스캔 → 기록 삭제(확인 창 없이) → 화면 반영 검증·출력 → 종료 */
    SELF_TEST_EXPORT, /* 스캔 → CSV·HTML 내보내기(대화 상자 없이) → 화면 검증·출력 → 종료 */
    SELF_TEST_PRECISE, /* [정밀 스캔] 체크 → 스캔 → 하드 링크 보정 열·보기·분석 줄 검증·출력 → 종료 */
    SELF_TEST_CACHE   /* 공용 캐시 탭 탐지 → 픽스처 안인지·화면 검증 → 정리 가능한 캐시를 모두
                       * 휴지통으로(확인 창 없이) → 결과 검증 → 종료 */
} SelfTest;

typedef struct {
    HWND hwnd;
    HWND path, browse, scan, cancel, progress, status, tree, list, view, analysis;
    HWND precise; /* [정밀 스캔(하드 링크)] 체크 상자 */
    HWND autosave, clear_history, export_csv, export_html;
    HWND tabs, drives, cleanup, cache, quarantine, monitor;
    HFONT font;
    int dpi;

    ScanJob *job;
    ScanModel model;
    int has_model;
    int progress_pos;
    ExportInfo scan_info; /* 마지막 스캔의 완료 시각·걸린 시간(보고서용) */

    /* 폴더별 용량 변화(직전 스캔 대비) */
    HistoryState history;
    unsigned long history_error;
    HistoryDiff diff;        /* history 가 COMPARED(또는 비교 뒤 CANCELLED)일 때 */
    int has_diff;
    size_t *change_of_node;  /* 노드 → diff 항목 번호(없으면 SCAN_NODE_NONE) */
    int autosave_on;         /* 스캔 기록 자동 저장(설정 파일에 저장된 값과 같게 유지) */
    HistoryClearResult cleared; /* 마지막 [기록 삭제] 결과 */

    SelfTest self_test;
    int self_test_exit;
    size_t trash_expect; /* SELF_TEST_TRASH·QUARANTINE: 선택해 옮기기 시작한 항목 수 */
    const wchar_t *quarantine_root;     /* SELF_TEST_QUARANTINE: 탐지 경로(가짜 볼륨 루트) */
    const wchar_t *quarantine_conflict; /* SELF_TEST_QUARANTINE: 복원 전에 만들 같은 이름 파일 */
    const wchar_t *export_prefix;  /* SELF_TEST_EXPORT: 내보낼 파일 경로(확장자 제외) */
    const wchar_t *cache_root;     /* SELF_TEST_CACHE: 찾은 캐시가 모두 이 폴더 안이어야 정리한다 */
    size_t cache_expect;           /* SELF_TEST_CACHE: 선택해 옮기기 시작한 캐시 수 */
    int export_enabled_before_scan; /* SELF_TEST_EXPORT: 스캔 전 내보내기 버튼이 켜져 있었으면 1 */
} App;

static App g_app;

static int scale(int px)
{
    return MulDiv(px, g_app.dpi, 96);
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
    int m = scale(8), h = scale(24), bw = scale(84), gap = scale(6), view_w = scale(170);
    int check_w = scale(150), export_w = scale(104), precise_w = scale(160);
    int x, w, y, path_w, bar_w, body_h, tree_w;

    /* 탭 컨트롤이 창 전체를 차지하고, 각 탭의 컨트롤은 탭의 표시 영역 안에 놓는다. */
    GetClientRect(g_app.hwnd, &rc);
    MoveWindow(g_app.tabs, rc.left + scale(4), rc.top + scale(4), rc.right - scale(8),
               rc.bottom - scale(8), TRUE);
    GetWindowRect(g_app.tabs, &rc);
    MapWindowPoints(NULL, g_app.hwnd, (POINT *)&rc, 2);
    TabCtrl_AdjustRect(g_app.tabs, FALSE, &rc);
    x = rc.left;
    w = rc.right - rc.left;
    if (w < 0) {
        w = 0;
    }

    /* [경로] [찾아보기] [스캔] [취소]  [v] 정밀 스캔(하드 링크)  [보기 ▼] */
    y = rc.top + m;
    path_w = w - 2 * m - 3 * (bw + gap) - (precise_w + gap) - (view_w + gap);
    if (path_w < scale(80)) {
        path_w = scale(80);
    }
    MoveWindow(g_app.path, x + m, y, path_w, h, TRUE);
    MoveWindow(g_app.browse, x + m + path_w + gap, y, bw, h, TRUE);
    MoveWindow(g_app.scan, x + m + path_w + gap + (bw + gap), y, bw, h, TRUE);
    MoveWindow(g_app.cancel, x + m + path_w + gap + 2 * (bw + gap), y, bw, h, TRUE);
    MoveWindow(g_app.precise, x + m + path_w + gap + 3 * (bw + gap) + gap, y, precise_w, h, TRUE);
    /* 콤보 상자 높이는 펼친 목록 높이까지 포함한다. */
    MoveWindow(g_app.view, x + m + path_w + gap + 3 * (bw + gap) + precise_w + gap, y, view_w,
               scale(160), TRUE);

    /* 진행 막대 오른쪽에 내보내기와 기록 설정:
     * [CSV 내보내기...] [HTML 보고서...]  [v] 스캔 기록 자동 저장  [기록 삭제...] */
    y += h + gap;
    bar_w = w - 2 * m - 2 * (export_w + gap) - check_w - bw - 2 * gap;
    if (bar_w < scale(40)) {
        bar_w = scale(40);
    }
    MoveWindow(g_app.progress, x + m, y + (h - scale(16)) / 2, bar_w, scale(16), TRUE);
    {
        int cx = x + m + bar_w + gap;
        MoveWindow(g_app.export_csv, cx, y, export_w, h, TRUE);
        cx += export_w + gap;
        MoveWindow(g_app.export_html, cx, y, export_w, h, TRUE);
        cx += export_w + gap + gap;
        MoveWindow(g_app.autosave, cx, y, check_w, h, TRUE);
        cx += check_w + gap;
        MoveWindow(g_app.clear_history, cx, y, bw, h, TRUE);
    }
    y += h + gap;
    MoveWindow(g_app.status, x + m, y, w - 2 * m, scale(20), TRUE);
    y += scale(20);
    MoveWindow(g_app.analysis, x + m, y, w - 2 * m, scale(20), TRUE);
    y += scale(20) + gap;

    body_h = rc.bottom - y - m;
    if (body_h < 0) {
        body_h = 0;
    }
    tree_w = (w - 2 * m - gap) * 2 / 5;
    MoveWindow(g_app.tree, x + m, y, tree_w, body_h, TRUE);
    MoveWindow(g_app.list, x + m + tree_w + gap, y, w - 2 * m - gap - tree_w, body_h, TRUE);

    body_h = rc.bottom - rc.top - 2 * m;
    MoveWindow(g_app.drives, x + m, rc.top + m, w - 2 * m > 0 ? w - 2 * m : 0,
               body_h > 0 ? body_h : 0, TRUE);
    MoveWindow(g_app.cleanup, x + m, rc.top + m, w - 2 * m > 0 ? w - 2 * m : 0,
               body_h > 0 ? body_h : 0, TRUE);
    MoveWindow(g_app.cache, x + m, rc.top + m, w - 2 * m > 0 ? w - 2 * m : 0,
               body_h > 0 ? body_h : 0, TRUE);
    MoveWindow(g_app.quarantine, x + m, rc.top + m, w - 2 * m > 0 ? w - 2 * m : 0,
               body_h > 0 ? body_h : 0, TRUE);
    MoveWindow(g_app.monitor, x + m, rc.top + m, w - 2 * m > 0 ? w - 2 * m : 0,
               body_h > 0 ? body_h : 0, TRUE);
}

/* ---- 탭 ---- */

static int current_tab(void)
{
    return TabCtrl_GetCurSel(g_app.tabs);
}

/* 고른 탭의 컨트롤만 보이게 한다. 드라이브·공용 캐시 탭을 처음 열면 바로 조회하고,
 * 격리 폴더 탭은 열 때마다 목록을 다시 읽으며,
 * 시스템 탭을 처음 열면 측정을 시작한다(이후 탭을 옮겨도 그래프 기록을 위해 계속 측정). */
static void show_tab(int tab)
{
    HWND scan_controls[] = {g_app.path,   g_app.browse, g_app.scan,     g_app.cancel,
                            g_app.view,   g_app.progress, g_app.status, g_app.analysis,
                            g_app.tree,   g_app.list,     g_app.autosave, g_app.clear_history,
                            g_app.export_csv, g_app.export_html, g_app.precise};
    int show_scan = tab == TAB_SCAN;
    size_t i;

    if (TabCtrl_GetCurSel(g_app.tabs) != tab) {
        TabCtrl_SetCurSel(g_app.tabs, tab);
    }
    for (i = 0; i < sizeof(scan_controls) / sizeof(scan_controls[0]); i++) {
        ShowWindow(scan_controls[i], show_scan ? SW_SHOW : SW_HIDE);
    }
    ShowWindow(g_app.drives, tab == TAB_DRIVES ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.cleanup, tab == TAB_CLEANUP ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.cache, tab == TAB_CACHE ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.quarantine, tab == TAB_QUARANTINE ? SW_SHOW : SW_HIDE);
    ShowWindow(g_app.monitor, tab == TAB_MONITOR ? SW_SHOW : SW_HIDE);
    if (tab == TAB_DRIVES && !drive_panel_loaded(g_app.drives)) {
        drive_panel_refresh(g_app.drives);
    }
    if (tab == TAB_CACHE && !cache_panel_loaded(g_app.cache)) {
        cache_panel_refresh(g_app.cache);
    }
    if (tab == TAB_QUARANTINE) {
        quarantine_panel_refresh(g_app.quarantine);
    }
    if (tab == TAB_MONITOR) {
        monitor_panel_start(g_app.monitor);
    }
}

/* ---- 결과 표시 ---- */

/* 지금 결과가 정밀 스캔(하드 링크 보정)으로 집계됐는지. */
static int precise_result(void)
{
    return g_app.has_model && (g_app.model.options & SCAN_OPT_HARDLINKS) != 0;
}

/* 하드 링크 중복을 뺀 용량(정밀 스캔이 아니면 경로마다 센 용량과 같다). */
static uint64_t unique_bytes(const ScanNode *n)
{
    return n->total_bytes - n->total_dup_bytes;
}

/* 트리 항목 표시 문자열(malloc, 호출자가 free). 정밀 스캔에서 중복이 있으면 중복 제외 용량을 덧붙인다. */
static wchar_t *tree_item_text(size_t index)
{
    const ScanNode *n = scan_model_node(&g_app.model, index);
    uint64_t whole = g_app.model.nodes[0].total_bytes;
    size_t len = wcslen(n->name) + 128, used;
    wchar_t size[32], *buf = malloc(len * sizeof(*buf));

    if (buf == NULL) {
        return NULL;
    }
    format_size(n->total_bytes, size, 32);
    if (index == 0) {
        swprintf(buf, len, L"%ls  —  %ls", n->name, size);
    } else {
        swprintf(buf, len, L"%ls  —  %ls (%.1f%%)", n->name, size,
                 share_of(n->total_bytes, whole));
    }
    used = wcslen(buf);
    if (precise_result() && n->total_dup_bytes > 0) {
        format_size(unique_bytes(n), size, 32);
        swprintf(buf + used, len - used, L" · 중복 제외 %ls", size);
        used = wcslen(buf);
    }
    swprintf(buf + used, len - used, L"%ls", flag_label(n->flags));
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

static int current_view(void)
{
    LRESULT sel = SendMessageW(g_app.view, CB_GETCURSEL, 0, 0);

    return sel >= 0 && sel < VIEW_COUNT ? (int)sel : VIEW_SIZE;
}

/* 부호 있는 용량("+1.20 GiB", "-300 B"). */
static void format_delta(int64_t delta, wchar_t *buf, size_t len)
{
    wchar_t size[32];
    uint64_t mag = delta < 0 ? (uint64_t)0 - (uint64_t)delta : (uint64_t)delta;

    format_size(mag, size, 32);
    swprintf(buf, len, L"%ls%ls", delta > 0 ? L"+" : delta < 0 ? L"-" : L"", size);
}

/* 변화 열 문구. */
static void change_text(const HistoryChange *c, wchar_t *buf, size_t len)
{
    wchar_t delta[48];

    format_delta(c->delta, delta, 48);
    swprintf(buf, len, L"%ls%ls%ls",
             c->kind == HISTORY_ADDED     ? L"새 폴더 "
             : c->kind == HISTORY_REMOVED ? L"삭제됨 "
                                          : L"",
             delta, c->uncertain ? L" (?)" : L"");
}

/* 한 줄을 넣는다. node 가 SCAN_NODE_NONE 이면(삭제된 폴더) 용량 열을 비운다. */
static void insert_row(int index, size_t node, const wchar_t *path, const wchar_t *flags,
                       const HistoryChange *change, uint64_t whole)
{
    wchar_t buf[96];
    LVITEMW lv;
    int row;

    swprintf(buf, 96, L"%u", (unsigned)(index + 1));
    memset(&lv, 0, sizeof(lv));
    lv.mask = LVIF_TEXT | LVIF_PARAM;
    lv.iItem = index;
    lv.pszText = buf;
    lv.lParam = (LPARAM)node;
    row = (int)SendMessageW(g_app.list, LVM_INSERTITEMW, 0, (LPARAM)&lv);
    if (row < 0) {
        return;
    }
    if (path != NULL) {
        size_t plen = wcslen(path) + wcslen(flags) + 1;
        wchar_t *text = malloc(plen * sizeof(*text));
        if (text != NULL) {
            swprintf(text, plen, L"%ls%ls", path, flags);
            list_set(row, COL_PATH, text);
            free(text);
        }
    }
    if (node != SCAN_NODE_NONE) {
        const ScanNode *n = &g_app.model.nodes[node];
        format_size(n->total_bytes, buf, 96);
        list_set(row, COL_SIZE, buf);
        format_size(n->total_alloc, buf, 96);
        list_set(row, COL_ALLOC, buf);
        swprintf(buf, 96, L"%llu", (unsigned long long)n->total_files);
        list_set(row, COL_FILES, buf);
        swprintf(buf, 96, L"%.1f%%", share_of(n->total_bytes, whole));
        list_set(row, COL_SHARE, buf);
        /* 정밀 스캔: 중복 제외 용량, 하드 링크로 중복 집계된 용량(기본 스캔 대비 차이) */
        if (precise_result()) {
            format_size(unique_bytes(n), buf, 96);
            list_set(row, COL_UNIQUE, buf);
            if (n->total_dup_bytes > 0 || n->total_dup_files > 0) {
                wchar_t size[32];
                format_size(n->total_dup_bytes, size, 32);
                swprintf(buf, 96, L"-%ls (%llu개)", size, (unsigned long long)n->total_dup_files);
                list_set(row, COL_DUP, buf);
            }
        }
    } else {
        list_set(row, COL_SIZE, L"-");
        list_set(row, COL_ALLOC, L"-");
        list_set(row, COL_FILES, L"-");
        list_set(row, COL_SHARE, L"-");
    }
    if (change != NULL) {
        change_text(change, buf, 96);
        list_set(row, COL_CHANGE, buf);
    }
}

/* 보기에 맞는 순위 기준. */
static ScanRankKey view_rank_key(int view)
{
    switch (view) {
    case VIEW_ALLOC:
        return SCAN_RANK_TOTAL_ALLOC;
    case VIEW_UNIQUE:
        return SCAN_RANK_TOTAL_UNIQUE;
    case VIEW_DUP:
        return SCAN_RANK_TOTAL_DUP;
    default:
        return SCAN_RANK_TOTAL_BYTES;
    }
}

/*
 * 오른쪽 목록을 보기에 맞게 채운다.
 *   용량·디스크 사용 순위: 상위 RANK_COUNT 개 폴더(변화 열은 직전 스캔 대비, 없으면 빈칸)
 *   변화 순위: 직전 스캔 대비 변화가 큰 폴더(삭제된 폴더 포함)
 *   중복 제외 용량 순위: 하드 링크 중복을 뺀 용량 순(기본 스캔이면 용량 순위와 같다)
 *   하드 링크 중복 순위: 중복 집계된 용량이 있는 폴더만(기본 스캔이면 비어 있다)
 */
static void fill_rank(void)
{
    size_t top[RANK_COUNT];
    size_t count, i;
    uint64_t whole;
    int view = current_view();

    ListView_DeleteAllItems(g_app.list);
    if (!g_app.has_model || g_app.model.len == 0) {
        return;
    }
    whole = g_app.model.nodes[0].total_bytes;
    SendMessageW(g_app.list, WM_SETREDRAW, FALSE, 0);
    if (view == VIEW_CHANGE) {
        count = g_app.has_diff ? (g_app.diff.len < RANK_COUNT ? g_app.diff.len : RANK_COUNT) : 0;
        for (i = 0; i < count; i++) {
            const HistoryChange *c = &g_app.diff.items[i];
            wchar_t *path = history_full_path(g_app.model.nodes[0].name, c->rel);
            const wchar_t *flags = c->node != SCAN_NODE_NONE
                                       ? flag_label(g_app.model.nodes[c->node].flags)
                                       : L"";
            insert_row((int)i, c->node, path, flags, c, whole);
            free(path);
        }
    } else {
        count = scan_model_rank(&g_app.model, view_rank_key(view), top, RANK_COUNT);
        if (view == VIEW_DUP) {
            /* 중복이 없는 폴더는 빼낸다(순위가 내림차순이므로 앞부분만 남는다). */
            while (count > 0 && g_app.model.nodes[top[count - 1]].total_dup_files == 0) {
                count--;
            }
        }
        for (i = 0; i < count; i++) {
            const HistoryChange *c = NULL;
            wchar_t *path = scan_model_path(&g_app.model, top[i]);
            if (g_app.change_of_node != NULL &&
                g_app.change_of_node[top[i]] != SCAN_NODE_NONE) {
                c = &g_app.diff.items[g_app.change_of_node[top[i]]];
            }
            insert_row((int)i, top[i], path, flag_label(g_app.model.nodes[top[i]].flags), c,
                       whole);
            free(path);
        }
    }
    SendMessageW(g_app.list, WM_SETREDRAW, TRUE, 0);
}

/* 디스크 사용량 분석과 직전 스캔 대비 변화를 둘째 상태 줄에 쓴다. */
static void update_analysis(void)
{
    const ScanReport *r = &g_app.model.report;
    wchar_t text[1024], a[32], b[32], c[32];
    size_t used = 0;
    int n;

#define APPEND(...)                                                                          \
    do {                                                                                     \
        n = swprintf(text + used, sizeof(text) / sizeof(text[0]) - used, __VA_ARGS__);       \
        if (n > 0) {                                                                         \
            used += (size_t)n;                                                               \
        }                                                                                    \
    } while (0)

    text[0] = L'\0';
    if (!g_app.has_model || g_app.model.status == SCAN_FATAL) {
        SetWindowTextW(g_app.analysis, L"");
        return;
    }
    format_size(r->alloc_bytes, a, 32);
    format_delta((int64_t)(r->alloc_bytes - r->total_bytes), b, 32);
    APPEND(L"디스크 사용 %ls(논리 크기 대비 %ls)", a, b);
    /* 정밀 스캔: 기본 집계(경로마다)와 하드 링크 중복을 뺀 집계의 차이 */
    if (precise_result() && r->hardlinks_unsupported) {
        APPEND(L" · 정밀 스캔: 하드 링크가 없는 파일 시스템이라 보정할 것이 없음");
    } else if (precise_result()) {
        format_size(r->total_bytes - r->hardlink_dup_bytes, a, 32);
        format_size(r->total_bytes, b, 32);
        format_size(r->hardlink_dup_bytes, c, 32);
        APPEND(L" · 정밀 스캔: 중복 제외 %ls(기본 %ls, 하드 링크 중복 %llu개 -%ls)", a, b,
               (unsigned long long)r->hardlink_dup_files, c);
        format_size(r->alloc_bytes - r->hardlink_dup_alloc, a, 32);
        APPEND(L" · 실제 디스크 사용 %ls", a);
        if (r->hardlink_unknown_files > 0) {
            APPEND(L" · 파일 ID 모름 %llu개(경로마다 셈)",
                   (unsigned long long)r->hardlink_unknown_files);
        }
    }
    if (r->slack_bytes > 0) {
        format_size(r->slack_bytes, a, 32);
        APPEND(L" · 클러스터 낭비 +%ls", a);
    }
    if (r->compressed_saved_bytes > 0) {
        format_size(r->compressed_saved_bytes, a, 32);
        APPEND(L" · 압축 -%ls(%llu개)", a, (unsigned long long)r->compressed_files);
    }
    if (r->sparse_saved_bytes > 0) {
        format_size(r->sparse_saved_bytes, a, 32);
        APPEND(L" · 스파스 -%ls(%llu개)", a, (unsigned long long)r->sparse_files);
    }
    if (r->cloud_saved_bytes > 0) {
        format_size(r->cloud_saved_bytes, a, 32);
        APPEND(L" · 클라우드 전용 -%ls(%llu개)", a, (unsigned long long)r->cloud_files);
    }
    if (r->other_saved_bytes > 0) {
        format_size(r->other_saved_bytes, a, 32);
        APPEND(L" · 기타 -%ls(%llu개)", a, (unsigned long long)r->other_saved_files);
    }
    APPEND(L"   |   ");
    if (g_app.has_diff) {
        FILETIME utc, local;
        SYSTEMTIME st;
        utc.dwLowDateTime = (DWORD)(g_app.diff.old_time & 0xFFFFFFFFu);
        utc.dwHighDateTime = (DWORD)(g_app.diff.old_time >> 32);
        if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) {
            memset(&st, 0, sizeof(st));
        }
        format_delta((int64_t)(g_app.diff.new_total - g_app.diff.old_total), a, 32);
        format_size(g_app.diff.old_total, b, 32);
        format_size(g_app.diff.new_total, c, 32);
        APPEND(L"직전 스캔(%02u-%02u %02u:%02u) 대비 %ls(%ls → %ls) · 늘어남 %u · 줄어듦 %u · "
               L"새 폴더 %u · 삭제 %u",
               (unsigned)st.wMonth, (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute,
               a, b, c, (unsigned)g_app.diff.grown, (unsigned)g_app.diff.shrunk,
               (unsigned)g_app.diff.added, (unsigned)g_app.diff.removed);
    }
    switch (g_app.history) {
    case HIST_BASELINE:
        APPEND(L"첫 스캔: 기준으로 저장했습니다(다음 스캔부터 변화를 비교)");
        break;
    case HIST_CANCELLED:
        APPEND(L"%ls취소되어 기록하지 않았습니다", g_app.has_diff ? L" · " : L"");
        break;
    case HIST_FAILED:
        APPEND(L"%ls변화 기록을 저장하지 못했습니다(오류 %lu)", g_app.has_diff ? L" · " : L"",
               g_app.history_error);
        break;
    case HIST_DISABLED:
        APPEND(L"%ls자동 저장이 꺼져 있어 이번 스캔은 기록하지 않았습니다",
               g_app.has_diff ? L" · " : L"");
        break;
    case HIST_CLEARED:
        APPEND(L"저장된 스캔 기록을 삭제했습니다(직전 스캔과 비교한 변화 표시도 지움)");
        break;
    default:
        break;
    }
#undef APPEND
    SetWindowTextW(g_app.analysis, text);
}

/*
 * 직전 스냅숏과 비교하고, 취소되지 않았고 자동 저장이 켜져 있으면 새 스냅숏을 저장한다.
 * 기록 위치: BYTESIEVE_HISTORY_DIR 또는 %LOCALAPPDATA%\ByteSieve\history.
 */
static void track_history(void)
{
    wchar_t dir[MAX_PATH * 2], *file;
    HistorySnapshot prev, cur;
    size_t i;

    g_app.history = HIST_NONE;
    g_app.history_error = 0;
    if (g_app.model.status == SCAN_FATAL || g_app.model.len == 0) {
        return;
    }
    history_snapshot_init(&prev);
    history_snapshot_init(&cur);
    if (history_default_dir(dir, sizeof(dir) / sizeof(dir[0])) != 0 ||
        (file = history_file_for(dir, g_app.model.nodes[0].name)) == NULL) {
        g_app.history = HIST_FAILED;
        g_app.history_error = ERROR_PATH_NOT_FOUND;
        return;
    }
    if (history_snapshot_from_model(&cur, &g_app.model) != 0) {
        g_app.history = HIST_FAILED;
        g_app.history_error = ERROR_NOT_ENOUGH_MEMORY;
        free(file);
        return;
    }
    if (history_load(&prev, file) == 0 && history_diff(&prev, &cur, &g_app.diff) == 0) {
        g_app.has_diff = 1;
        g_app.change_of_node = malloc(g_app.model.len * sizeof(*g_app.change_of_node));
        if (g_app.change_of_node != NULL) {
            for (i = 0; i < g_app.model.len; i++) {
                g_app.change_of_node[i] = SCAN_NODE_NONE;
            }
            for (i = 0; i < g_app.diff.len; i++) {
                if (g_app.diff.items[i].node < g_app.model.len) {
                    g_app.change_of_node[g_app.diff.items[i].node] = i;
                }
            }
        }
    }
    if (g_app.model.report.cancelled) {
        g_app.history = HIST_CANCELLED;
    } else if (!g_app.autosave_on) {
        g_app.history = HIST_DISABLED;
    } else if (history_ensure_dir(dir) != 0 ||
               history_save(&cur, file, &g_app.history_error) != 0) {
        g_app.history = HIST_FAILED;
        if (g_app.history_error == 0) {
            g_app.history_error = GetLastError();
        }
    } else {
        g_app.history = g_app.has_diff ? HIST_COMPARED : HIST_BASELINE;
    }
    free(file);
    history_snapshot_free(&prev);
    history_snapshot_free(&cur);
}

static void clear_results(void)
{
    TreeView_DeleteAllItems(g_app.tree);
    ListView_DeleteAllItems(g_app.list);
    scan_model_free(&g_app.model);
    g_app.has_model = 0;
    history_diff_free(&g_app.diff);
    g_app.has_diff = 0;
    free(g_app.change_of_node);
    g_app.change_of_node = NULL;
    g_app.history = HIST_NONE;
    SetWindowTextW(g_app.analysis, L"");
    EnableWindow(g_app.export_csv, FALSE);
    EnableWindow(g_app.export_html, FALSE);
}

/* 정밀 스캔 열(중복 제외·하드 링크 중복)은 정밀 스캔 결과일 때만 보인다. */
static void update_precise_columns(void)
{
    int on = precise_result();

    ListView_SetColumnWidth(g_app.list, COL_UNIQUE, on ? scale(90) : 0);
    ListView_SetColumnWidth(g_app.list, COL_DUP, on ? scale(120) : 0);
}

static void show_results(void)
{
    HTREEITEM root;

    update_precise_columns();
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

/* [기록 삭제]는 스캔 중이 아니고 저장된 기록이 있을 때만 누를 수 있다. */
static void update_clear_button(void)
{
    wchar_t dir[MAX_PATH * 2];
    size_t count = 0;

    if (g_app.job == NULL && history_default_dir(dir, sizeof(dir) / sizeof(dir[0])) == 0) {
        count = history_count(dir);
    }
    EnableWindow(g_app.clear_history, count > 0);
}

/* 내보낼 스캔 결과가 있는지(스캔 중이 아니고, 결과가 있고, 대상 폴더를 열지 못한 것이 아님). */
static int can_export(void)
{
    return g_app.job == NULL && g_app.has_model && g_app.model.len > 0 &&
           g_app.model.status != SCAN_FATAL;
}

static void set_scanning(int scanning)
{
    EnableWindow(g_app.scan, !scanning);
    EnableWindow(g_app.browse, !scanning);
    EnableWindow(g_app.path, !scanning);
    EnableWindow(g_app.cancel, scanning);
    EnableWindow(g_app.precise, !scanning);
    EnableWindow(g_app.export_csv, can_export());
    EnableWindow(g_app.export_html, can_export());
    update_clear_button();
}

static void set_status(const wchar_t *text)
{
    SetWindowTextW(g_app.status, text);
}

/* ---- 기록 설정 ---- */

/*
 * 자동 저장 설정을 바꾸고 기록 폴더의 설정 파일에 저장한다(다음 실행에도 유지).
 * 저장하지 못하면 체크 상태를 이전 값으로 되돌리고 상태 줄에 알린다. 성공하면 1.
 */
static int apply_autosave(int on)
{
    wchar_t dir[MAX_PATH * 2], text[256];
    unsigned long error = ERROR_PATH_NOT_FOUND;

    on = on != 0;
    if (history_default_dir(dir, sizeof(dir) / sizeof(dir[0])) == 0 &&
        history_set_autosave(dir, on, &error) == 0) {
        g_app.autosave_on = on;
        SendMessageW(g_app.autosave, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        set_status(on ? L"스캔 기록 자동 저장을 켰습니다. 다음 스캔부터 결과를 기록합니다."
                      : L"스캔 기록 자동 저장을 껐습니다. 이후 스캔 결과는 저장하지 않습니다"
                        L"(이미 저장된 기록은 그대로 둡니다).");
        return 1;
    }
    SendMessageW(g_app.autosave, BM_SETCHECK, g_app.autosave_on ? BST_CHECKED : BST_UNCHECKED, 0);
    swprintf(text, 256, L"자동 저장 설정을 저장하지 못했습니다(오류 %lu). 설정은 바뀌지 않았습니다.",
             error);
    set_status(text);
    return 0;
}

/*
 * 저장된 스캔 기록을 모두 삭제한다. confirm 이 1 이면 개수·위치·되돌릴 수 없다는 안내로 먼저
 * 확인한다. 지금 보이는 변화가 지운 기록과 비교한 것이면 변화 열·변화 순위·분석 줄에서 지운다.
 * 일부 파일을 지우지 못해도 나머지는 지우고 결과를 상태 줄에 쓴다.
 * 삭제를 실행했으면 1, 기록이 없거나 스캔 중이거나 취소했으면 0.
 */
static int clear_history(int confirm)
{
    wchar_t dir[MAX_PATH * 2], text[MAX_PATH * 2 + 512], *file;
    size_t count;
    int rc;

    if (g_app.job != NULL) {
        return 0;
    }
    if (history_default_dir(dir, sizeof(dir) / sizeof(dir[0])) != 0) {
        set_status(L"기록 폴더를 정하지 못해 기록을 삭제할 수 없습니다.");
        return 0;
    }
    count = history_count(dir);
    if (count == 0) {
        set_status(L"삭제할 스캔 기록이 없습니다.");
        update_clear_button();
        return 0;
    }
    if (confirm) {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"저장된 스캔 기록 %u개를 삭제할까요?\n\n기록 폴더: %ls\n\n"
                 L"삭제하면 직전 스캔과 비교한 변화를 더 볼 수 없고, 다음 스캔은 새 기준이 됩니다. "
                 L"같은 폴더를 쓰는 CLI --track 기록도 함께 삭제됩니다.\n"
                 L"스캔한 폴더와 파일은 건드리지 않으며, 자동 저장 설정은 그대로 둡니다.\n\n"
                 L"삭제한 기록은 되돌릴 수 없습니다.",
                 (unsigned)count, dir);
        if (MessageBoxW(g_app.hwnd, text, L"스캔 기록 삭제",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            set_status(L"기록 삭제를 취소했습니다. 아무것도 바뀌지 않았습니다.");
            return 0;
        }
    }

    rc = history_clear(dir, &g_app.cleared);
    /* 지금 대상의 기록이 사라졌으면 그 기록과 비교한 변화 표시도 지운다. */
    if (g_app.has_model && g_app.model.len > 0 &&
        (file = history_file_for(dir, g_app.model.nodes[0].name)) != NULL) {
        if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
            history_diff_free(&g_app.diff);
            g_app.has_diff = 0;
            free(g_app.change_of_node);
            g_app.change_of_node = NULL;
            g_app.history = HIST_CLEARED;
        }
        free(file);
    }
    if (rc == 0) {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"스캔 기록 %u개를 삭제했습니다.",
                 (unsigned)g_app.cleared.removed);
    } else if (g_app.cleared.failed == 0) {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"기록 폴더를 읽지 못해 기록을 삭제하지 못했습니다(오류 %lu).",
                 g_app.cleared.error);
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"스캔 기록 %u개를 삭제했고 %u개는 삭제하지 못했습니다(오류 %lu: 사용 중이거나 "
                 L"권한이 없을 수 있음). 다시 시도하세요.",
                 (unsigned)g_app.cleared.removed, (unsigned)g_app.cleared.failed,
                 g_app.cleared.error);
    }
    set_status(text);
    update_analysis();
    fill_rank();
    update_clear_button();
    return 1;
}

/* ---- 내보내기 ---- */

/* 기본 파일 이름 "ByteSieve-<대상 폴더 이름>-YYYYMMDD-HHMM.<확장자>"(이름에 못 쓰는 글자는 _). */
static void default_export_name(ExportKind kind, wchar_t *buf, size_t len)
{
    const wchar_t *root = g_app.model.nodes[0].name;
    wchar_t label[64];
    size_t n = wcslen(root), start, i, out = 0;
    SYSTEMTIME st;

    while (n > 0 && path_is_separator(root[n - 1])) {
        n--;
    }
    start = n;
    while (start > 0 && !path_is_separator(root[start - 1])) {
        start--;
    }
    for (i = start; i < n && out + 1 < sizeof(label) / sizeof(label[0]); i++) {
        label[out++] = wcschr(L"\\/:*?\"<>|", root[i]) != NULL || root[i] < 32 ? L'_' : root[i];
    }
    while (out > 0 && label[out - 1] == L'_') {
        out--; /* "D:" → "D" */
    }
    label[out] = L'\0';
    GetLocalTime(&st);
    swprintf(buf, len, L"ByteSieve-%ls%ls%04u%02u%02u-%02u%02u.%ls", label,
             out > 0 ? L"-" : L"", (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
             (unsigned)st.wHour, (unsigned)st.wMinute, kind == EXPORT_KIND_CSV ? L"csv" : L"html");
}

/* 내보내기 실패 원인(사용자용). */
static const wchar_t *export_error_text(unsigned long error)
{
    switch (error) {
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
        return L"저장 위치에 쓸 권한이 없습니다(읽기 전용 파일이거나 보호된 폴더).";
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return L"다른 프로그램(Excel 등)이 파일을 사용 중입니다. 파일을 닫고 다시 시도하세요.";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return L"디스크 공간이 부족합니다.";
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_INVALID_NAME:
        return L"저장 위치를 찾을 수 없거나 파일 이름이 올바르지 않습니다.";
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return L"메모리가 부족합니다.";
    case ERROR_INVALID_PARAMETER:
        return L"내보낼 스캔 결과가 없습니다.";
    default:
        return L"파일을 쓰지 못했습니다.";
    }
}

/*
 * 지금 스캔 결과를 kind 형식으로 file 에 쓴다. 결과는 상태 줄에 쓰고, 실패했고 show_error 가 1 이면
 * 원인을 창으로도 알린다(기존 파일은 바뀌지 않음). 성공하면 1.
 */
static int export_to(ExportKind kind, const wchar_t *file, int show_error)
{
    const wchar_t *what = kind == EXPORT_KIND_CSV ? L"CSV" : L"HTML 보고서";
    unsigned long error = ERROR_INVALID_PARAMETER;
    size_t len = wcslen(file) + 256;
    wchar_t *text;
    int ok = 0;

    if (can_export()) {
        ok = (kind == EXPORT_KIND_CSV
                  ? export_csv(&g_app.model, file, &error)
                  : export_html(&g_app.model, &g_app.scan_info, file, &error)) == 0;
    }
    text = malloc(len * sizeof(*text));
    if (text == NULL) {
        return ok;
    }
    if (ok) {
        swprintf(text, len, L"%ls(으)로 내보냈습니다(폴더 %u개): %ls", what,
                 (unsigned)g_app.model.len, file);
        set_status(text);
    } else {
        swprintf(text, len, L"%ls 내보내기 실패(오류 %lu): %ls", what, error,
                 export_error_text(error));
        set_status(text);
        if (show_error) {
            swprintf(text, len,
                     L"%ls(으)로 내보내지 못했습니다.\n\n%ls\n(오류 %lu)\n\n파일: %ls\n\n"
                     L"이미 있던 파일은 바뀌지 않았습니다.",
                     what, export_error_text(error), error, file);
            MessageBoxW(g_app.hwnd, text, L"내보내기 실패", MB_OK | MB_ICONWARNING);
        }
    }
    free(text);
    return ok;
}

/* 저장 위치를 고르게 한 뒤(덮어쓰기는 대화 상자가 확인) 내보낸다. */
static void export_results(ExportKind kind)
{
    OPENFILENAMEW ofn;
    wchar_t file[MAX_PATH * 4];
    DWORD dlg_error;

    if (!can_export()) {
        set_status(L"내보낼 스캔 결과가 없습니다. 먼저 폴더를 스캔하세요.");
        return;
    }
    default_export_name(kind, file, sizeof(file) / sizeof(file[0]));
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_app.hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)(sizeof(file) / sizeof(file[0]));
    if (kind == EXPORT_KIND_CSV) {
        ofn.lpstrFilter = L"CSV 파일 (*.csv)\0*.csv\0모든 파일 (*.*)\0*.*\0";
        ofn.lpstrDefExt = L"csv";
        ofn.lpstrTitle = L"스캔 결과를 CSV로 내보내기";
    } else {
        ofn.lpstrFilter = L"HTML 보고서 (*.html)\0*.html;*.htm\0모든 파일 (*.*)\0*.*\0";
        ofn.lpstrDefExt = L"html";
        ofn.lpstrTitle = L"스캔 결과를 HTML 보고서로 저장";
    }
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        dlg_error = CommDlgExtendedError();
        if (dlg_error != 0) {
            wchar_t text[128];
            swprintf(text, 128, L"저장 위치 대화 상자를 열지 못했습니다(오류 0x%lx).",
                     (unsigned long)dlg_error);
            set_status(text);
        }
        return; /* 취소 */
    }
    export_to(kind, file, 1);
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
    int len, precise;

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
    precise = SendMessageW(g_app.precise, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_app.job = scan_job_start_ex(target, SCAN_DEPTH_UNLIMITED,
                                  precise ? (unsigned)SCAN_OPT_HARDLINKS : 0u,
                                  PROGRESS_INTERVAL_MS, on_job_notify, g_app.hwnd);
    free(target);
    if (g_app.job == NULL) {
        set_status(L"스캔을 시작하지 못했습니다(메모리 또는 스레드 생성 실패).");
        return;
    }
    set_scanning(1);
    set_status(precise ? L"정밀 스캔 중(하드 링크 확인)..." : L"스캔 중...");
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
    if (snap.progress.precise) {
        /* 정밀 스캔: 지금까지 찾은 하드 링크 중복도 보여 준다. */
        wchar_t dup[32];
        format_size(snap.progress.dup_bytes, dup, 32);
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"정밀 스캔 중... 파일 %llu · 폴더 %llu · %ls · 하드 링크 중복 %llu개(-%ls) · "
                 L"오류 %llu  |  %ls",
                 (unsigned long long)snap.progress.files, (unsigned long long)snap.progress.dirs,
                 size, (unsigned long long)snap.progress.dup_files, dup,
                 (unsigned long long)snap.progress.errors, snap.current_dir);
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"스캔 중... 파일 %llu · 폴더 %llu · %ls · 오류 %llu  |  %ls",
                 (unsigned long long)snap.progress.files, (unsigned long long)snap.progress.dirs,
                 size, (unsigned long long)snap.progress.errors, snap.current_dir);
    }
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
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        g_app.scan_info.finished_time = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
        g_app.scan_info.elapsed_ms = snap.elapsed_ms;
    }

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
                 L"%ls%ls: 파일 %llu · 폴더 %llu · %ls · 오류 %llu · 건너뛴 폴더 %llu · %.1f초",
                 r->hardlinks_checked ? L"정밀 스캔 " : L"", head,
                 (unsigned long long)r->file_count, (unsigned long long)r->dir_count, size,
                 (unsigned long long)r->error_count, (unsigned long long)r->skipped_dir_count,
                 (double)snap.elapsed_ms / 1000.0);
        if (!r->cancelled) {
            SendMessageW(g_app.progress, PBM_SETPOS, PROGRESS_RANGE, 0);
        }
    }
    set_status(text);
    set_scanning(0);
    track_history();
    update_clear_button();
    update_analysis();
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

/* 보기 콤보 상자: 디스크 사용 순위·변화 순위 목록이 모델·비교 결과와 맞는지. 실패 수. */
static int verify_views(void)
{
    int failures = 0, rows;
    size_t top, i, expect;
    LVITEMW lv;
    wchar_t cell[96], want[96];

    SELF_CHECK(current_view() == VIEW_SIZE);

    SendMessageW(g_app.view, CB_SETCURSEL, VIEW_ALLOC, 0);
    fill_rank();
    rows = ListView_GetItemCount(g_app.list);
    expect = g_app.model.len > 0 ? g_app.model.len - 1 : 0;
    SELF_CHECK(rows == (int)(expect < RANK_COUNT ? expect : RANK_COUNT));
    if (rows > 0) {
        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_PARAM;
        lv.iItem = 0;
        SendMessageW(g_app.list, LVM_GETITEMW, 0, (LPARAM)&lv);
        SELF_CHECK(scan_model_rank(&g_app.model, SCAN_RANK_TOTAL_ALLOC, &top, 1) == 1 &&
                   (size_t)lv.lParam == top);
    }

    SendMessageW(g_app.view, CB_SETCURSEL, VIEW_CHANGE, 0);
    fill_rank();
    rows = ListView_GetItemCount(g_app.list);
    expect = g_app.has_diff ? (g_app.diff.len < RANK_COUNT ? g_app.diff.len : RANK_COUNT) : 0;
    SELF_CHECK(rows == (int)expect);
    for (i = 0; i < expect && (int)i < rows; i++) {
        const HistoryChange *c = &g_app.diff.items[i];
        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_PARAM;
        lv.iItem = (int)i;
        SendMessageW(g_app.list, LVM_GETITEMW, 0, (LPARAM)&lv);
        SELF_CHECK((size_t)lv.lParam == c->node);
        ListView_GetItemText(g_app.list, (int)i, COL_CHANGE, cell, 96);
        change_text(c, want, 96);
        SELF_CHECK(wcscmp(cell, want) == 0);
        if (c->node != SCAN_NODE_NONE) {
            SELF_CHECK(g_app.change_of_node == NULL || g_app.change_of_node[c->node] == i);
        }
    }

    /* 중복 제외 용량 순위: 내림차순(기본 스캔이면 용량 순위와 같다) */
    SendMessageW(g_app.view, CB_SETCURSEL, VIEW_UNIQUE, 0);
    fill_rank();
    rows = ListView_GetItemCount(g_app.list);
    expect = g_app.model.len > 0 ? g_app.model.len - 1 : 0;
    SELF_CHECK(rows == (int)(expect < RANK_COUNT ? expect : RANK_COUNT));
    for (i = 0; (int)i < rows; i++) {
        size_t prev = 0;
        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_PARAM;
        lv.iItem = (int)i;
        SendMessageW(g_app.list, LVM_GETITEMW, 0, (LPARAM)&lv);
        if (i > 0) {
            LVITEMW pv;
            memset(&pv, 0, sizeof(pv));
            pv.mask = LVIF_PARAM;
            pv.iItem = (int)i - 1;
            SendMessageW(g_app.list, LVM_GETITEMW, 0, (LPARAM)&pv);
            prev = (size_t)pv.lParam;
            SELF_CHECK(unique_bytes(&g_app.model.nodes[prev]) >=
                       unique_bytes(&g_app.model.nodes[(size_t)lv.lParam]));
        }
    }

    /* 하드 링크 중복 순위: 중복이 있는 폴더만(기본 스캔이면 없음) */
    SendMessageW(g_app.view, CB_SETCURSEL, VIEW_DUP, 0);
    fill_rank();
    rows = ListView_GetItemCount(g_app.list);
    expect = 0;
    for (i = 1; i < g_app.model.len; i++) {
        expect += g_app.model.nodes[i].total_dup_files > 0;
    }
    SELF_CHECK(rows == (int)(expect < RANK_COUNT ? expect : RANK_COUNT));
    SELF_CHECK(precise_result() || rows == 0);
    for (i = 0; (int)i < rows; i++) {
        const ScanNode *n;
        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_PARAM;
        lv.iItem = (int)i;
        SendMessageW(g_app.list, LVM_GETITEMW, 0, (LPARAM)&lv);
        n = &g_app.model.nodes[(size_t)lv.lParam];
        SELF_CHECK(n->total_dup_files > 0);
        ListView_GetItemText(g_app.list, (int)i, COL_UNIQUE, cell, 96);
        format_size(unique_bytes(n), want, 96);
        SELF_CHECK(wcscmp(cell, want) == 0);
        ListView_GetItemText(g_app.list, (int)i, COL_DUP, cell, 96);
        SELF_CHECK(cell[0] == L'-');
    }

    /* 정밀 스캔 열은 정밀 스캔 결과일 때만 보이고, 기본 스캔이면 비어 있다 */
    SELF_CHECK((ListView_GetColumnWidth(g_app.list, COL_UNIQUE) > 0) == precise_result());
    SELF_CHECK((ListView_GetColumnWidth(g_app.list, COL_DUP) > 0) == precise_result());
    SendMessageW(g_app.view, CB_SETCURSEL, VIEW_SIZE, 0);
    fill_rank();
    if (!precise_result() && ListView_GetItemCount(g_app.list) > 0) {
        ListView_GetItemText(g_app.list, 0, COL_UNIQUE, cell, 96);
        SELF_CHECK(cell[0] == L'\0');
    }
    return failures;
}

/* --self-test-changes: 변화 목록을 stdout 에 쓴다(검사 스크립트가 확인). */
static void print_changes(void)
{
    size_t i;
    wchar_t delta[48];

    for (i = 0; i < g_app.diff.len; i++) {
        const HistoryChange *c = &g_app.diff.items[i];
        format_delta(c->delta, delta, 48);
        wprintf(L"change: %ls %ls %ls\n", history_kind_name(c->kind), delta, c->rel);
    }
    wprintf(L"changes: grown %u shrunk %u added %u removed %u\n", (unsigned)g_app.diff.grown,
            (unsigned)g_app.diff.shrunk, (unsigned)g_app.diff.added,
            (unsigned)g_app.diff.removed);
    fflush(stdout);
}

/*
 * SELF_TEST_NO_AUTOSAVE: 자동 저장을 끈 채 스캔했으니 저장하지 않았고 설정이 파일에 남았는지.
 * SELF_TEST_CLEAR_HISTORY: 직전 스냅숏과 비교한 뒤 기록을 삭제(확인 창 없이)하면 기록 파일이
 * 없어지고 변화 열·변화 순위·분석 줄·[기록 삭제] 버튼에 반영되는지. 지운 개수를 stdout 에 쓴다.
 * 실패 수.
 */
static int verify_history_settings(void)
{
    int failures = 0, rows;
    wchar_t dir[MAX_PATH * 2], text[512], cell[96];
    size_t before;

    SELF_CHECK(g_app.model.report.cancelled == 0);
    SELF_CHECK(g_app.model.status != SCAN_FATAL);
    SELF_CHECK(history_default_dir(dir, sizeof(dir) / sizeof(dir[0])) == 0);
    SELF_CHECK(history_autosave_enabled(dir) == g_app.autosave_on);
    SELF_CHECK((SendMessageW(g_app.autosave, BM_GETCHECK, 0, 0) == BST_CHECKED) ==
               (g_app.autosave_on != 0));
    if (g_app.self_test == SELF_TEST_NO_AUTOSAVE) {
        SELF_CHECK(!g_app.autosave_on);
        SELF_CHECK(g_app.history == HIST_DISABLED);
        GetWindowTextW(g_app.analysis, text, 512);
        SELF_CHECK(wcsstr(text, L"자동 저장이 꺼져") != NULL);
        return failures;
    }

    /* 기록 삭제 */
    SELF_CHECK(g_app.has_diff);
    before = history_count(dir);
    SELF_CHECK(before > 0 && IsWindowEnabled(g_app.clear_history));
    SELF_CHECK(clear_history(0) == 1);
    SELF_CHECK(g_app.cleared.failed == 0 && g_app.cleared.removed == before);
    SELF_CHECK(history_count(dir) == 0);
    SELF_CHECK(history_autosave_enabled(dir) == g_app.autosave_on);
    SELF_CHECK(!g_app.has_diff && g_app.change_of_node == NULL);
    SELF_CHECK(g_app.history == HIST_CLEARED);
    SELF_CHECK(!IsWindowEnabled(g_app.clear_history));
    GetWindowTextW(g_app.analysis, text, 512);
    SELF_CHECK(wcsstr(text, L"기록을 삭제했습니다") != NULL && wcsstr(text, L"직전 스캔(") == NULL);
    GetWindowTextW(g_app.status, text, 512);
    SELF_CHECK(wcsstr(text, L"삭제했습니다") != NULL);
    rows = ListView_GetItemCount(g_app.list);
    if (rows > 0) {
        ListView_GetItemText(g_app.list, 0, COL_CHANGE, cell, 96);
        SELF_CHECK(cell[0] == L'\0');
    }
    SendMessageW(g_app.view, CB_SETCURSEL, VIEW_CHANGE, 0);
    fill_rank();
    SELF_CHECK(ListView_GetItemCount(g_app.list) == 0);
    SendMessageW(g_app.view, CB_SETCURSEL, VIEW_SIZE, 0);
    fill_rank();
    /* 더 지울 것이 없으면 아무것도 하지 않는다. */
    SELF_CHECK(clear_history(0) == 0);
    wprintf(L"cleared: %u\n", (unsigned)g_app.cleared.removed);
    fflush(stdout);
    return failures;
}

/*
 * SELF_TEST_EXPORT: 스캔 전에는 꺼져 있던 내보내기 버튼이 켜졌는지, <prefix>.csv·<prefix>.html 로
 * 내보내지는지, 없는 폴더로 내보내면 실패를 알리고 아무것도 만들지 않는지. 내용은 검사 스크립트가
 * 확인하도록 폴더 수를 stdout 에 쓴다. 실패 수.
 */
static int verify_export(void)
{
    int failures = 0;
    size_t len = wcslen(g_app.export_prefix) + 32;
    wchar_t *file = malloc(len * sizeof(*file)), text[512];

    SELF_CHECK(!g_app.export_enabled_before_scan);
    SELF_CHECK(IsWindowEnabled(g_app.export_csv) && IsWindowEnabled(g_app.export_html));
    if (file == NULL) {
        return failures + 1;
    }
    swprintf(file, len, L"%ls.csv", g_app.export_prefix);
    SELF_CHECK(export_to(EXPORT_KIND_CSV, file, 0));
    GetWindowTextW(g_app.status, text, 512);
    SELF_CHECK(wcsstr(text, L"CSV(으)로 내보냈습니다") == text);
    swprintf(file, len, L"%ls.html", g_app.export_prefix);
    SELF_CHECK(export_to(EXPORT_KIND_HTML, file, 0));
    GetWindowTextW(g_app.status, text, 512);
    SELF_CHECK(wcsstr(text, L"HTML 보고서(으)로 내보냈습니다") == text);

    /* 없는 폴더: 실패를 상태 줄에 알리고 아무 파일도 남기지 않는다. */
    swprintf(file, len, L"%ls-missing\\out.csv", g_app.export_prefix);
    SELF_CHECK(!export_to(EXPORT_KIND_CSV, file, 0));
    GetWindowTextW(g_app.status, text, 512);
    SELF_CHECK(wcsstr(text, L"내보내기 실패") != NULL);
    SELF_CHECK(GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES);
    free(file);

    wprintf(L"exported folders: %u\n", (unsigned)g_app.model.len);
    fflush(stdout);
    return failures;
}

/*
 * SELF_TEST_PRECISE: 정밀 스캔 결과가 화면(상태 줄·분석 줄·트리·열)에 맞게 나오는지 확인하고,
 * 검사 스크립트가 확인하도록 기본 집계와 중복 제외 집계를 stdout 에 쓴다. 실패 수.
 */
static int verify_precise(void)
{
    const ScanReport *r = &g_app.model.report;
    const ScanNode *root = &g_app.model.nodes[0];
    int failures = 0;
    wchar_t text[1024], want[64], size[32];
    TVITEMW tv;
    HTREEITEM item = TreeView_GetRoot(g_app.tree);

    SELF_CHECK(r->hardlinks_checked && !r->hardlinks_unsupported);
    SELF_CHECK(!IsWindowEnabled(g_app.precise) == (g_app.job != NULL));
    SELF_CHECK(root->total_dup_bytes == r->hardlink_dup_bytes &&
               root->total_dup_files == r->hardlink_dup_files);
    GetWindowTextW(g_app.status, text, 1024);
    /* "정밀 스캔 완료: …" 또는 "정밀 스캔 완료(일부 폴더를 읽지 못함): …" */
    SELF_CHECK(wcsstr(text, L"정밀 스캔 완료") == text);
    /* 분석 줄: 중복 제외(기본 대비 차이)와 실제 디스크 사용 */
    GetWindowTextW(g_app.analysis, text, 1024);
    format_size(r->total_bytes - r->hardlink_dup_bytes, size, 32);
    swprintf(want, 64, L"중복 제외 %ls(기본 ", size);
    SELF_CHECK(wcsstr(text, want) != NULL);
    format_size(r->alloc_bytes - r->hardlink_dup_alloc, size, 32);
    swprintf(want, 64, L"실제 디스크 사용 %ls", size);
    SELF_CHECK(wcsstr(text, want) != NULL);
    /* 트리 루트에 중복 제외 용량이 덧붙는다(중복이 있을 때) */
    memset(&tv, 0, sizeof(tv));
    tv.mask = TVIF_TEXT;
    tv.hItem = item;
    tv.pszText = text;
    tv.cchTextMax = 1024;
    SELF_CHECK(item != NULL && TreeView_GetItem(g_app.tree, &tv));
    SELF_CHECK((wcsstr(text, L" · 중복 제외 ") != NULL) == (root->total_dup_bytes > 0));

    wprintf(L"precise: files %llu bytes %llu dup_files %llu dup_bytes %llu unique %llu "
            L"unknown %llu\n",
            (unsigned long long)r->file_count, (unsigned long long)r->total_bytes,
            (unsigned long long)r->hardlink_dup_files, (unsigned long long)r->hardlink_dup_bytes,
            (unsigned long long)(r->total_bytes - r->hardlink_dup_bytes),
            (unsigned long long)r->hardlink_unknown_files);
    fflush(stdout);
    return failures;
}

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

    if (rows > 0) {
        /* 디스크 사용 열 */
        wchar_t cell[64], expect[64];
        LVITEMW lv;
        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_PARAM;
        lv.iItem = 0;
        SendMessageW(g_app.list, LVM_GETITEMW, 0, (LPARAM)&lv);
        ListView_GetItemText(g_app.list, 0, COL_ALLOC, cell, 64);
        format_size(g_app.model.nodes[(size_t)lv.lParam].total_alloc, expect, 64);
        SELF_CHECK(wcscmp(cell, expect) == 0);
    }
    failures += verify_views();
    /* 결과가 있으면(취소된 스캔 포함) 내보낼 수 있다. */
    SELF_CHECK(IsWindowEnabled(g_app.export_csv) == (g_app.model.status != SCAN_FATAL));
    SELF_CHECK(IsWindowEnabled(g_app.export_html) == (g_app.model.status != SCAN_FATAL));

    if (g_app.self_test == SELF_TEST_SCAN || g_app.self_test == SELF_TEST_CHANGES ||
        g_app.self_test == SELF_TEST_PRECISE) {
        wchar_t text[1024];
        SELF_CHECK(g_app.model.report.cancelled == 0);
        SELF_CHECK(g_app.model.status != SCAN_FATAL);
        SELF_CHECK(pos == PROGRESS_RANGE);
        /* 직전 스냅숏이 없으면 기준으로, 있으면 비교한 뒤 저장한다. */
        SELF_CHECK(g_app.history == HIST_BASELINE || g_app.history == HIST_COMPARED);
        SELF_CHECK((g_app.history == HIST_COMPARED) == (g_app.has_diff != 0));
        GetWindowTextW(g_app.analysis, text, 1024);
        SELF_CHECK(wcsstr(text, L"디스크 사용 ") == text);
        SELF_CHECK(wcsstr(text, g_app.has_diff ? L"직전 스캔(" : L"첫 스캔") != NULL);
        /* 정밀 스캔 요약은 정밀 스캔 결과에만 */
        SELF_CHECK((wcsstr(text, L"정밀 스캔: ") != NULL) == precise_result());
        SELF_CHECK(precise_result() == (g_app.self_test == SELF_TEST_PRECISE));
        if (g_app.self_test == SELF_TEST_CHANGES) {
            SELF_CHECK(g_app.has_diff);
            print_changes();
        }
        if (g_app.self_test == SELF_TEST_PRECISE) {
            failures += verify_precise();
        }
    } else if (g_app.self_test == SELF_TEST_NO_AUTOSAVE ||
               g_app.self_test == SELF_TEST_CLEAR_HISTORY) {
        failures += verify_history_settings();
    } else if (g_app.self_test == SELF_TEST_EXPORT) {
        SELF_CHECK(g_app.model.report.cancelled == 0 && g_app.model.status != SCAN_FATAL);
        failures += verify_export();
    } else if (g_app.model.report.cancelled) {
        SELF_CHECK(g_app.history == HIST_CANCELLED);
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

/* 창 자체의 표시 상태(부모가 숨겨져 있어도 판단할 수 있게 스타일을 본다). */
static int is_shown(HWND h)
{
    return (GetWindowLongW(h, GWL_STYLE) & WS_VISIBLE) != 0;
}

/* 드라이브 탭 새로고침이 끝난 뒤 탭 전환 상태와 패널 내용을 확인한다. */
static int verify_drive_tab(void)
{
    int failures = drive_panel_verify(g_app.drives);

    SELF_CHECK(current_tab() == TAB_DRIVES);
    SELF_CHECK(is_shown(g_app.drives));
    SELF_CHECK(!is_shown(g_app.tree) && !is_shown(g_app.list) && !is_shown(g_app.path));
    /* 탭 컨트롤이 맨 아래가 아니면 그 위의 패널이 가려진다. */
    SELF_CHECK(GetWindow(g_app.tabs, GW_HWNDNEXT) == NULL);
    return failures == 0 ? 0 : 1;
}

/* 정리 후보 탐지가 끝난 뒤 탭 전환 상태와 패널 내용을 확인한다. */
static int verify_cleanup_tab(void)
{
    int failures =
        cleanup_panel_verify(g_app.cleanup, g_app.self_test == SELF_TEST_CLEANUP_CANCEL);

    SELF_CHECK(current_tab() == TAB_CLEANUP);
    SELF_CHECK(is_shown(g_app.cleanup) && !is_shown(g_app.drives));
    SELF_CHECK(!is_shown(g_app.tree) && !is_shown(g_app.list) && !is_shown(g_app.path));
    SELF_CHECK(GetWindow(g_app.tabs, GW_HWNDNEXT) == NULL);
    /* 폴더 스캔은 건드리지 않는다. */
    SELF_CHECK(g_app.job == NULL && !g_app.has_model);
    return failures == 0 ? 0 : 1;
}

/* 시스템 탭에서 몇 번 측정한 뒤 탭 전환 상태와 패널 내용을 확인한다. */
static int verify_monitor_tab(void)
{
    int failures = monitor_panel_verify(g_app.monitor);

    SELF_CHECK(current_tab() == TAB_MONITOR);
    SELF_CHECK(is_shown(g_app.monitor) && !is_shown(g_app.drives) && !is_shown(g_app.cleanup) &&
               !is_shown(g_app.quarantine));
    SELF_CHECK(!is_shown(g_app.tree) && !is_shown(g_app.list) && !is_shown(g_app.path));
    SELF_CHECK(GetWindow(g_app.tabs, GW_HWNDNEXT) == NULL);
    return failures == 0 ? 0 : 1;
}

/* 공용 캐시 탭: 탭 전환 상태와 패널 내용(찾은 캐시가 모두 cache_root 안인지 포함). 실패 수. */
static int verify_cache_tab(void)
{
    int failures = cache_panel_verify(g_app.cache, g_app.cache_root);

    SELF_CHECK(current_tab() == TAB_CACHE);
    SELF_CHECK(is_shown(g_app.cache) && !is_shown(g_app.cleanup) && !is_shown(g_app.drives));
    SELF_CHECK(!is_shown(g_app.tree) && !is_shown(g_app.list) && !is_shown(g_app.path));
    SELF_CHECK(GetWindow(g_app.tabs, GW_HWNDNEXT) == NULL);
    return failures;
}

/* 격리 폴더 탭의 탭 전환 상태와 패널 내용을 확인한다. */
static int verify_quarantine_tab(size_t rows, size_t ok)
{
    int failures = quarantine_panel_verify(g_app.quarantine, rows, ok);

    SELF_CHECK(current_tab() == TAB_QUARANTINE);
    SELF_CHECK(is_shown(g_app.quarantine) && !is_shown(g_app.cleanup) && !is_shown(g_app.drives));
    SELF_CHECK(GetWindow(g_app.tabs, GW_HWNDNEXT) == NULL);
    return failures == 0 ? 0 : 1;
}

/* 파일을 만든다(같은 이름이 있는 복원 상황 재현용). */
static int write_conflict_file(const wchar_t *path)
{
    static const char text[] = "conflict";
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = WriteFile(h, text, (DWORD)(sizeof(text) - 1), &written, NULL);
    CloseHandle(h);
    return ok && written == sizeof(text) - 1;
}

/*
 * SELF_TEST_QUARANTINE: 정리가 끝난 뒤 격리 폴더 탭에서 목록 확인 → 같은 이름 파일을 하나 만든
 * 뒤 모두 복원(그 항목은 건너뜀) → 남은 항목 영구 삭제를 시작한다. 실패하면 0.
 */
static int self_test_quarantine_step(void)
{
    size_t restored;

    if (cleanup_panel_verify_trash(g_app.cleanup, g_app.trash_expect) != 0) {
        return 0;
    }
    show_tab(TAB_QUARANTINE);
    /* 사용자의 실제 격리 항목은 건드리지 않도록 픽스처만 보이게 한다. */
    quarantine_panel_set_root(g_app.quarantine, g_app.quarantine_root);
    if (verify_quarantine_tab(g_app.trash_expect, g_app.trash_expect) != 0) {
        return 0;
    }
    if (!write_conflict_file(g_app.quarantine_conflict)) {
        fwprintf(stderr, L"self-test failed: could not create %ls\n", g_app.quarantine_conflict);
        return 0;
    }
    if (quarantine_panel_select_all(g_app.quarantine) != g_app.trash_expect) {
        fwprintf(stderr, L"self-test failed: could not select quarantined items\n");
        return 0;
    }
    restored = quarantine_panel_restore(g_app.quarantine, 0);
    if (restored + 1 != g_app.trash_expect || quarantine_panel_conflicts(g_app.quarantine) != 1 ||
        verify_quarantine_tab(1, 1) != 0) {
        fwprintf(stderr, L"self-test failed: restored %u of %u (conflicts %u)\n",
                 (unsigned)restored, (unsigned)g_app.trash_expect,
                 (unsigned)quarantine_panel_conflicts(g_app.quarantine));
        return 0;
    }
    return quarantine_panel_select_all(g_app.quarantine) == 1 &&
           quarantine_panel_purge(g_app.quarantine, 0);
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
        {L"폴더", 220, LVCFMT_LEFT},
        {L"용량", 80, LVCFMT_RIGHT},
        {L"중복 제외", 0, LVCFMT_RIGHT},     /* 정밀 스캔일 때만 보인다 */
        {L"하드 링크 중복", 0, LVCFMT_RIGHT}, /* 정밀 스캔일 때만 보인다 */
        {L"변화", 110, LVCFMT_RIGHT},
        {L"디스크 사용", 84, LVCFMT_RIGHT},
        {L"파일 수", 64, LVCFMT_RIGHT},
        {L"비율", 56, LVCFMT_RIGHT},
    };
    NONCLIENTMETRICSW ncm;
    int i;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_app.font = CreateFontIndirectW(&ncm.lfMessageFont);

    g_app.tabs = make_child(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, 0, IDC_TABS);
    {
        TCITEMW tab;
        memset(&tab, 0, sizeof(tab));
        tab.mask = TCIF_TEXT;
        tab.pszText = L"폴더 스캔";
        SendMessageW(g_app.tabs, TCM_INSERTITEMW, TAB_SCAN, (LPARAM)&tab);
        tab.pszText = L"드라이브";
        SendMessageW(g_app.tabs, TCM_INSERTITEMW, TAB_DRIVES, (LPARAM)&tab);
        tab.pszText = L"정리 후보";
        SendMessageW(g_app.tabs, TCM_INSERTITEMW, TAB_CLEANUP, (LPARAM)&tab);
        tab.pszText = L"공용 캐시";
        SendMessageW(g_app.tabs, TCM_INSERTITEMW, TAB_CACHE, (LPARAM)&tab);
        tab.pszText = L"격리 폴더";
        SendMessageW(g_app.tabs, TCM_INSERTITEMW, TAB_QUARANTINE, (LPARAM)&tab);
        tab.pszText = L"시스템";
        SendMessageW(g_app.tabs, TCM_INSERTITEMW, TAB_MONITOR, (LPARAM)&tab);
    }

    g_app.path = make_child(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                            IDC_PATH);
    g_app.browse = make_child(L"BUTTON", L"찾아보기...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                              IDC_BROWSE);
    g_app.scan = make_child(L"BUTTON", L"스캔", BS_DEFPUSHBUTTON | WS_TABSTOP, 0, IDC_SCAN);
    g_app.cancel = make_child(L"BUTTON", L"취소", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_CANCEL);
    /* 정밀 스캔: 하드 링크로 같은 실제 파일을 여러 번 세지 않게 보정한다(조금 느리고 메모리를 더 씀). */
    g_app.precise = make_child(L"BUTTON", L"정밀 스캔(하드 링크)", BS_AUTOCHECKBOX | WS_TABSTOP,
                               0, IDC_PRECISE);
    g_app.progress = make_child(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, IDC_PROGRESS);
    g_app.status = make_child(L"STATIC", L"폴더를 선택하고 [스캔]을 누르세요.",
                              SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS, 0, IDC_STATUS);
    g_app.analysis = make_child(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0,
                                IDC_ANALYSIS);
    g_app.view = make_child(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                            IDC_VIEW);
    {
        static const wchar_t *const views[VIEW_COUNT] = {
            L"용량 순위", L"디스크 사용 순위", L"변화 순위(직전 스캔 대비)",
            L"중복 제외 용량 순위(정밀 스캔)", L"하드 링크 중복 순위(정밀 스캔)"};
        int v;
        for (v = 0; v < VIEW_COUNT; v++) {
            SendMessageW(g_app.view, CB_ADDSTRING, 0, (LPARAM)views[v]);
        }
        SendMessageW(g_app.view, CB_SETCURSEL, VIEW_SIZE, 0);
        SendMessageW(g_app.view, CB_SETDROPPEDWIDTH, (WPARAM)scale(240), 0);
    }
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
    /* 탭 순서가 화면 순서와 같도록 왼쪽 것부터 만든다. */
    g_app.export_csv = make_child(L"BUTTON", L"CSV 내보내기...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                  IDC_EXPORT_CSV);
    g_app.export_html = make_child(L"BUTTON", L"HTML 보고서...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                   IDC_EXPORT_HTML);
    g_app.autosave = make_child(L"BUTTON", L"스캔 기록 자동 저장", BS_AUTOCHECKBOX | WS_TABSTOP,
                                0, IDC_AUTOSAVE);
    g_app.clear_history = make_child(L"BUTTON", L"기록 삭제...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                     IDC_CLEAR_HISTORY);
    {
        /* 저장된 설정을 읽는다(없거나 읽을 수 없으면 켬). */
        wchar_t dir[MAX_PATH * 2];
        g_app.autosave_on = history_default_dir(dir, sizeof(dir) / sizeof(dir[0])) != 0 ||
                            history_autosave_enabled(dir);
        SendMessageW(g_app.autosave, BM_SETCHECK,
                     g_app.autosave_on ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    set_scanning(0);

    g_app.drives = drive_panel_create(g_app.hwnd, IDC_DRIVES, g_app.font, g_app.dpi);
    g_app.cleanup = cleanup_panel_create(g_app.hwnd, IDC_CLEANUP, g_app.font, g_app.dpi);
    g_app.cache = cache_panel_create(g_app.hwnd, IDC_CACHE, g_app.font, g_app.dpi);
    g_app.quarantine =
        quarantine_panel_create(g_app.hwnd, IDC_QUARANTINE, g_app.font, g_app.dpi);
    g_app.monitor = monitor_panel_create(g_app.hwnd, IDC_MONITOR, g_app.font, g_app.dpi);
    /* 새 자식 창은 Z 순서 맨 아래에 붙으므로, 모두 만든 뒤 탭 컨트롤을 맨 아래로 보내
     * 탭 위의 컨트롤이 가려지지 않게 한다. */
    SetWindowPos(g_app.tabs, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    show_tab(TAB_SCAN);
}

static LRESULT on_notify(NMHDR *hdr)
{
    if (hdr->idFrom == IDC_TABS && hdr->code == TCN_SELCHANGE) {
        show_tab(current_tab());
        return 0;
    }
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
            if ((size_t)lv.lParam < g_app.model.len) {
                reveal_in_tree((size_t)lv.lParam);
            }
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
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = scale(800);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = scale(320);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:
            browse_folder();
            return 0;
        case IDC_SCAN:
        case IDOK: /* 경로 칸에서 Enter */
            if (current_tab() == TAB_SCAN) {
                start_scan();
            } else if (current_tab() == TAB_CLEANUP) {
                cleanup_panel_start(g_app.cleanup, NULL);
            }
            return 0;
        case IDC_CANCEL:
            cancel_scan();
            return 0;
        case IDC_VIEW:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                fill_rank();
            }
            return 0;
        case IDC_AUTOSAVE:
            /* BS_AUTOCHECKBOX 가 이미 체크 상태를 바꿨다. 저장에 실패하면 되돌린다. */
            if (HIWORD(wp) == BN_CLICKED) {
                apply_autosave(SendMessageW(g_app.autosave, BM_GETCHECK, 0, 0) == BST_CHECKED);
            }
            return 0;
        case IDC_CLEAR_HISTORY:
            clear_history(1);
            return 0;
        case IDC_EXPORT_CSV:
            export_results(EXPORT_KIND_CSV);
            return 0;
        case IDC_EXPORT_HTML:
            export_results(EXPORT_KIND_HTML);
            return 0;
        case IDC_DRIVES:
            if (HIWORD(wp) == DPN_REFRESHED && g_app.self_test == SELF_TEST_DRIVES) {
                KillTimer(hwnd, TIMER_SELF_TEST);
                g_app.self_test_exit = verify_drive_tab();
                DestroyWindow(hwnd);
            }
            return 0;
        case IDC_CLEANUP:
            if (HIWORD(wp) == CPN_FINISHED && (g_app.self_test == SELF_TEST_CLEANUP ||
                                               g_app.self_test == SELF_TEST_CLEANUP_CANCEL)) {
                KillTimer(hwnd, TIMER_SELF_TEST);
                g_app.self_test_exit = verify_cleanup_tab();
                DestroyWindow(hwnd);
            } else if (HIWORD(wp) == CPN_FINISHED && (g_app.self_test == SELF_TEST_TRASH ||
                                                      g_app.self_test == SELF_TEST_QUARANTINE)) {
                /* 탐지 화면을 확인한 뒤, 옮길 수 있는 항목을 모두 체크해 휴지통으로 옮긴다. */
                g_app.trash_expect = cleanup_panel_check_all(g_app.cleanup, 1);
                if (verify_cleanup_tab() != 0 || g_app.trash_expect == 0 ||
                    !cleanup_panel_trash(g_app.cleanup, 0)) {
                    fwprintf(stderr, L"self-test failed: trash did not start (%u selected)\n",
                             (unsigned)g_app.trash_expect);
                    KillTimer(hwnd, TIMER_SELF_TEST);
                    g_app.self_test_exit = 1;
                    DestroyWindow(hwnd);
                }
            } else if (HIWORD(wp) == CPN_TRASHED && g_app.self_test == SELF_TEST_TRASH) {
                KillTimer(hwnd, TIMER_SELF_TEST);
                g_app.self_test_exit =
                    cleanup_panel_verify_trash(g_app.cleanup, g_app.trash_expect) == 0 ? 0 : 1;
                DestroyWindow(hwnd);
            } else if (HIWORD(wp) == CPN_TRASHED && g_app.self_test == SELF_TEST_QUARANTINE) {
                if (!self_test_quarantine_step()) {
                    KillTimer(hwnd, TIMER_SELF_TEST);
                    g_app.self_test_exit = 1;
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        case IDC_CACHE:
            if (HIWORD(wp) == KPN_FINISHED && g_app.self_test == SELF_TEST_CACHE) {
                /* 찾은 캐시가 모두 픽스처 안이고 화면이 맞을 때만 정리한다(실제 캐시 보호). */
                int failures = verify_cache_tab();
                g_app.cache_expect = failures == 0 ? cache_panel_check_all(g_app.cache, 1) : 0;
                if (failures != 0 || g_app.cache_expect == 0 ||
                    !cache_panel_trash(g_app.cache, 0)) {
                    fwprintf(stderr, L"self-test failed: cache cleanup did not start "
                                     L"(%u selected)\n",
                             (unsigned)g_app.cache_expect);
                    KillTimer(hwnd, TIMER_SELF_TEST);
                    g_app.self_test_exit = 1;
                    DestroyWindow(hwnd);
                }
            } else if (HIWORD(wp) == KPN_TRASHED && g_app.self_test == SELF_TEST_CACHE) {
                KillTimer(hwnd, TIMER_SELF_TEST);
                g_app.self_test_exit =
                    cache_panel_verify_trash(g_app.cache, g_app.cache_expect) == 0 ? 0 : 1;
                DestroyWindow(hwnd);
            }
            return 0;
        case IDC_QUARANTINE:
            /* 남은 항목(같은 이름 때문에 건너뛴 것)까지 영구 삭제했으면 목록이 비어야 한다. */
            if (HIWORD(wp) == QPN_PURGED && g_app.self_test == SELF_TEST_QUARANTINE) {
                KillTimer(hwnd, TIMER_SELF_TEST);
                g_app.self_test_exit = verify_quarantine_tab(0, 0);
                DestroyWindow(hwnd);
            }
            return 0;
        case IDC_MONITOR:
            /* 첫 측정(CPU 값 없음) 뒤 몇 번 더 받아 갱신 간격까지 확인한다. */
            if (HIWORD(wp) == MPN_SAMPLED && g_app.self_test == SELF_TEST_MONITOR &&
                monitor_panel_samples(g_app.monitor) >= MONITOR_SELF_TEST_SAMPLES) {
                KillTimer(hwnd, TIMER_SELF_TEST);
                g_app.self_test_exit = verify_monitor_tab();
                DestroyWindow(hwnd);
            }
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
 *   bytesieve-gui --self-test-drives      드라이브 탭을 조회한 뒤 화면을 검증하고 종료
 *   bytesieve-gui --self-test-cleanup <dir>        정리 후보를 탐지한 뒤 화면을 검증하고 종료
 *   bytesieve-gui --self-test-cleanup-cancel <dir> 탐지 시작 직후 취소한 뒤 화면을 검증하고 종료
 *   bytesieve-gui --self-test-trash <dir>          탐지 후 옮길 수 있는 항목을 모두 휴지통으로
 *                                                  옮기고(확인 창 없음) 결과 화면을 검증하고 종료
 *   bytesieve-gui --self-test-quarantine <dir> <file>
 *                                         탐지 후 모두 정리(BYTESIEVE_QUARANTINE_ROOT=<dir> 이면 격리
 *                                         폴더로) → 격리 폴더 탭 확인 → <file> 을 만든 뒤 모두 복원
 *                                         (그 항목은 같은 이름이라 건너뜀) → 남은 항목 영구 삭제 → 종료
 *   bytesieve-gui --self-test-cache <dir>  공용 캐시 탭에서 탐지한 캐시가 모두 <dir> 안인지 확인한 뒤
 *                                         정리 가능한 캐시를 모두 휴지통으로 옮기고(확인 창 없음)
 *                                         결과를 검증하고 종료(USERPROFILE 등을 <dir> 안으로 돌려 실행)
 *   bytesieve-gui --self-test-monitor     시스템 탭에서 RAM·CPU 를 몇 번 측정한 뒤 화면을 검증하고 종료
 *   bytesieve-gui --self-test-precise <dir> [정밀 스캔]을 체크하고 스캔한 뒤 하드 링크 보정 열·보기·
 *                                          분석 줄을 검증하고 기본·중복 제외 집계를 stdout 에 쓴 뒤 종료
 *   bytesieve-gui --self-test-changes <dir> 스캔 후 직전 스냅숏과 비교한 변화 보기를 검증하고
 *                                          변화 목록을 stdout 에 쓴 뒤 종료(직전 스냅숏 필요)
 *   bytesieve-gui --self-test-no-autosave <dir>
 *                                         처음 읽은 자동 저장 설정을 stdout 에 쓰고 끈 뒤 스캔해
 *                                         기록하지 않았는지 검증하고 종료
 *   bytesieve-gui --self-test-clear-history <dir>
 *                                         스캔(직전 스냅숏 필요) 후 기록을 삭제(확인 창 없음)하고
 *                                         화면 반영을 검증한 뒤 지운 개수를 stdout 에 쓰고 종료
 *   bytesieve-gui --self-test-export <dir> <prefix>
 *                                         스캔 후 <prefix>.csv·<prefix>.html 로 내보내고(대화 상자 없음)
 *                                         화면을 검증한 뒤 폴더 수를 stdout 에 쓰고 종료
 */
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    INITCOMMONCONTROLSEX icc;
    WNDCLASSEXW wc;
    MSG msg;
    HDC screen;
    wchar_t **argv;
    const wchar_t *target = NULL;
    const wchar_t *cleanup_target = NULL;
    int argc = 0;

    (void)prev;
    (void)cmd;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test") == 0) {
        g_app.self_test = SELF_TEST_SCAN;
        target = argv[2];
    } else if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test-precise") == 0) {
        g_app.self_test = SELF_TEST_PRECISE;
        target = argv[2];
    } else if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test-changes") == 0) {
        g_app.self_test = SELF_TEST_CHANGES;
        target = argv[2];
    } else if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test-no-autosave") == 0) {
        g_app.self_test = SELF_TEST_NO_AUTOSAVE;
        target = argv[2];
    } else if (argv != NULL && argc >= 3 &&
               wcscmp(argv[1], L"--self-test-clear-history") == 0) {
        g_app.self_test = SELF_TEST_CLEAR_HISTORY;
        target = argv[2];
    } else if (argv != NULL && argc >= 4 && wcscmp(argv[1], L"--self-test-export") == 0) {
        g_app.self_test = SELF_TEST_EXPORT;
        target = argv[2];
        g_app.export_prefix = argv[3];
    } else if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test-cancel") == 0) {
        g_app.self_test = SELF_TEST_CANCEL;
        target = argv[2];
    } else if (argv != NULL && argc >= 2 && wcscmp(argv[1], L"--self-test-drives") == 0) {
        g_app.self_test = SELF_TEST_DRIVES;
    } else if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test-cleanup") == 0) {
        g_app.self_test = SELF_TEST_CLEANUP;
        cleanup_target = argv[2];
    } else if (argv != NULL && argc >= 3 &&
               wcscmp(argv[1], L"--self-test-cleanup-cancel") == 0) {
        g_app.self_test = SELF_TEST_CLEANUP_CANCEL;
        cleanup_target = argv[2];
    } else if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test-trash") == 0) {
        g_app.self_test = SELF_TEST_TRASH;
        cleanup_target = argv[2];
    } else if (argv != NULL && argc >= 4 && wcscmp(argv[1], L"--self-test-quarantine") == 0) {
        g_app.self_test = SELF_TEST_QUARANTINE;
        cleanup_target = argv[2];
        g_app.quarantine_root = argv[2];
        g_app.quarantine_conflict = argv[3];
    } else if (argv != NULL && argc >= 3 && wcscmp(argv[1], L"--self-test-cache") == 0) {
        g_app.self_test = SELF_TEST_CACHE;
        g_app.cache_root = argv[2];
    } else if (argv != NULL && argc >= 2 && wcscmp(argv[1], L"--self-test-monitor") == 0) {
        g_app.self_test = SELF_TEST_MONITOR;
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
                ICC_STANDARD_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);
    drive_panel_register(inst);
    cleanup_panel_register(inst);
    cache_panel_register(inst);
    quarantine_panel_register(inst);
    monitor_panel_register(inst);

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

    if (g_app.self_test == SELF_TEST_NO_AUTOSAVE) {
        /* 처음 읽은 설정을 알려(재실행 뒤 유지 확인용) 자동 저장을 끈 다음 스캔한다.
         * 체크박스를 누른 것과 같은 경로(apply_autosave)를 탄다. */
        wprintf(L"autosave: %d\n", g_app.autosave_on);
        fflush(stdout);
        if (g_app.autosave_on) {
            SendMessageW(g_app.autosave, BM_SETCHECK, BST_UNCHECKED, 0);
            if (!apply_autosave(0)) {
                fwprintf(stderr, L"self-test failed: could not turn off autosave\n");
                target = NULL;
                g_app.self_test_exit = 1;
                DestroyWindow(g_app.hwnd);
            }
        }
    }
    if (g_app.self_test == SELF_TEST_PRECISE) {
        /* 사용자가 [정밀 스캔]을 체크한 것과 같다. */
        SendMessageW(g_app.precise, BM_SETCHECK, BST_CHECKED, 0);
    }
    if (target != NULL) {
        g_app.export_enabled_before_scan =
            IsWindowEnabled(g_app.export_csv) || IsWindowEnabled(g_app.export_html);
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
    if (g_app.self_test == SELF_TEST_DRIVES) {
        if (g_app.drives == NULL) {
            fwprintf(stderr, L"self-test failed: drive panel was not created\n");
            g_app.self_test_exit = 1;
            DestroyWindow(g_app.hwnd);
        } else {
            SetTimer(g_app.hwnd, TIMER_SELF_TEST, SELF_TEST_TIMEOUT_MS, NULL);
            show_tab(TAB_DRIVES);
        }
    }
    if (g_app.self_test == SELF_TEST_CACHE) {
        show_tab(TAB_CACHE);
        if (!cache_panel_busy(g_app.cache)) {
            fwprintf(stderr, L"self-test failed: cache detection did not start\n");
            g_app.self_test_exit = 1;
            DestroyWindow(g_app.hwnd);
        } else {
            SetTimer(g_app.hwnd, TIMER_SELF_TEST, SELF_TEST_TIMEOUT_MS, NULL);
        }
    }
    if (g_app.self_test == SELF_TEST_MONITOR) {
        show_tab(TAB_MONITOR);
        if (!monitor_panel_start(g_app.monitor)) {
            fwprintf(stderr, L"self-test failed: monitor did not start\n");
            g_app.self_test_exit = 1;
            DestroyWindow(g_app.hwnd);
        } else {
            SetTimer(g_app.hwnd, TIMER_SELF_TEST, SELF_TEST_TIMEOUT_MS, NULL);
        }
    }
    if (cleanup_target != NULL) {
        show_tab(TAB_CLEANUP);
        if (!cleanup_panel_start(g_app.cleanup, cleanup_target)) {
            fwprintf(stderr, L"self-test failed: cleanup detection did not start\n");
            g_app.self_test_exit = 1;
            DestroyWindow(g_app.hwnd);
        } else {
            if (g_app.self_test == SELF_TEST_CLEANUP_CANCEL) {
                cleanup_panel_cancel(g_app.cleanup);
            }
            SetTimer(g_app.hwnd, TIMER_SELF_TEST, SELF_TEST_TIMEOUT_MS, NULL);
        }
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_app.hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    /* 자체 검사의 경로 인자(quarantine_root 등)를 루프 동안 쓰므로 끝난 뒤 해제한다. */
    LocalFree(argv);
    DeleteObject(g_app.font);
    CoUninitialize();
    return (int)msg.wParam;
}
