#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cleanup_job.h"
#include "cleanup_scan.h"
#include "gui_cleanup.h"
#include "size_format.h"

#define CLEANUP_PANEL_CLASS L"ByteSieveCleanupPanel"
#define WM_APP_CLEANUP_PROGRESS (WM_APP + 1)
#define WM_APP_CLEANUP_DONE (WM_APP + 2)

#define PROGRESS_INTERVAL_MS 100
#define PROGRESS_RANGE 1000

enum {
    IDC_C_PATH = 100,
    IDC_C_BROWSE,
    IDC_C_START,
    IDC_C_CANCEL,
    IDC_C_PROGRESS,
    IDC_C_STATUS,
    IDC_C_WARN,
    IDC_C_FILTER_LABEL,
    IDC_C_FILTER,
    IDC_C_SUMMARY,
    IDC_C_LIST
};

/* 목록 열. */
enum { COL_SIZE, COL_TYPE, COL_PATH, COL_FILES, COL_NOTE, COL_COUNT };

/* 유형 필터 콤보 상자의 항목 순서와 비트. */
static const struct {
    const wchar_t *label;
    unsigned mask;
} g_filters[] = {
    {L"전체", CLEANUP_MASK_ALL},
    {L"Node.js (node_modules)", CLEANUP_MASK(CLEANUP_NODE_MODULES)},
    {L"빌드 결과물 (dist·build·out)", CLEANUP_MASK(CLEANUP_BUILD)},
    {L"개발 캐시 (.cache·.next·.vite)", CLEANUP_MASK(CLEANUP_CACHE)},
    {L"임시 파일 (.tmp·.temp·*.tmp)", CLEANUP_MASK(CLEANUP_TEMP)},
    {L"로그 (*.log)", CLEANUP_MASK(CLEANUP_LOG)},
};
#define FILTER_COUNT (sizeof(g_filters) / sizeof(g_filters[0]))

/* CreateWindowExW 로 넘기는 생성 인자. */
typedef struct {
    HFONT font;
    int dpi;
} PanelParams;

typedef struct {
    HWND hwnd;
    HWND path, browse, start, cancel, progress, status, warn, filter_label, filter, summary,
        list;
    HFONT font;
    int dpi;

    CleanupJob *job;
    CleanupResult result;
    int has_result;
    int progress_pos;

    size_t *view; /* 현재 필터의 항목 인덱스(용량 내림차순) */
    size_t view_len;
    uint64_t view_bytes;

    wchar_t cell[64]; /* LVN_GETDISPINFO 용 */
} CleanupPanel;

static CleanupPanel *panel_of(HWND hwnd)
{
    return (CleanupPanel *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static int pscale(const CleanupPanel *p, int px)
{
    return MulDiv(px, p->dpi, 96);
}

static const wchar_t *category_label(CleanupCategory category)
{
    switch (category) {
    case CLEANUP_NODE_MODULES:
        return L"Node.js";
    case CLEANUP_BUILD:
        return L"빌드 결과물";
    case CLEANUP_CACHE:
        return L"개발 캐시";
    case CLEANUP_TEMP:
        return L"임시 파일";
    case CLEANUP_LOG:
        return L"로그";
    default:
        return L"";
    }
}

/* 비고 열: 신뢰도, 일부 누락, 시스템 경로. */
static void note_text(const CleanupItem *item, wchar_t *buf, size_t len)
{
    size_t used = 0;

    buf[0] = L'\0';
    if (item->flags & CLEANUP_ITEM_PROTECTED) {
        used += (size_t)swprintf(buf + used, len - used, L"시스템 경로 · ");
    }
    if ((item->flags & CLEANUP_ITEM_DIR) && cleanup_needs_marker(item->category)) {
        used += (size_t)swprintf(buf + used, len - used, L"%ls · ",
                                 (item->flags & CLEANUP_ITEM_CONFIRMED) ? L"확인됨" : L"추정");
    }
    if (item->flags & CLEANUP_ITEM_INCOMPLETE) {
        used += (size_t)swprintf(buf + used, len - used, L"일부 누락 · ");
    }
    /* 끝의 " · " 제거 */
    if (used >= 3) {
        buf[used - 3] = L'\0';
    }
}

/* ---- 표시 ---- */

static unsigned current_mask(const CleanupPanel *p)
{
    LRESULT sel = SendMessageW(p->filter, CB_GETCURSEL, 0, 0);

    if (sel < 0 || (size_t)sel >= FILTER_COUNT) {
        sel = 0;
    }
    return g_filters[sel].mask;
}

static void update_summary(CleanupPanel *p)
{
    wchar_t text[256], shown[32], total[32];

    if (!p->has_result) {
        SetWindowTextW(p->summary, L"");
        return;
    }
    format_size(p->view_bytes, shown, 32);
    format_size(p->result.total_bytes, total, 32);
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"표시 %u개 · %ls   /   전체 %u개 · 예상 확보 %ls", (unsigned)p->view_len, shown,
             (unsigned)p->result.len, total);
    SetWindowTextW(p->summary, text);
}

/* 필터를 적용해 목록을 다시 만든다(가상 목록이라 개수만 바꾼다). */
static void apply_filter(CleanupPanel *p)
{
    unsigned mask = current_mask(p);
    size_t count;

    free(p->view);
    p->view = NULL;
    p->view_len = 0;
    p->view_bytes = 0;
    if (p->has_result) {
        count = cleanup_select(&p->result, mask, NULL, 0, NULL);
        if (count > 0) {
            p->view = malloc(count * sizeof(*p->view));
        }
        if (p->view != NULL) {
            p->view_len = cleanup_select(&p->result, mask, p->view, count, &p->view_bytes);
        }
    }
    ListView_SetItemCountEx(p->list, (int)p->view_len, 0);
    InvalidateRect(p->list, NULL, TRUE);
    update_summary(p);
}

static void set_warning(CleanupPanel *p)
{
    if (p->has_result && p->result.root_protected) {
        SetWindowTextW(p->warn, L"경고: 시스템 보호 경로 안입니다. 표시된 항목은 Windows나 "
                                L"프로그램에 필요할 수 있으니 정리 대상으로 삼지 마세요.");
    } else {
        SetWindowTextW(p->warn, L"읽기 전용 탐지입니다. 파일을 삭제하거나 수정하지 않습니다. "
                                L"시스템 폴더(Windows, Program Files 등)는 탐색에서 제외합니다.");
    }
    InvalidateRect(p->warn, NULL, TRUE);
}

static void set_busy(CleanupPanel *p, int busy)
{
    EnableWindow(p->start, !busy);
    EnableWindow(p->browse, !busy);
    EnableWindow(p->path, !busy);
    EnableWindow(p->cancel, busy);
}

/* ---- 탐지 ---- */

/* 작업 스레드에서 호출된다. 패널이 이미 없으면 PostMessage 가 실패할 뿐이다. */
static void on_job_notify(int finished, void *ctx)
{
    HWND hwnd = ctx;

    PostMessageW(hwnd, finished ? WM_APP_CLEANUP_DONE : WM_APP_CLEANUP_PROGRESS, 0, 0);
}

static void update_progress(CleanupPanel *p)
{
    CleanupJobSnapshot snap;
    wchar_t size[32], text[CLEANUP_JOB_DIR_LEN + 160];
    uint64_t done, all;
    int pos;

    if (p->job == NULL) {
        return;
    }
    cleanup_job_progress(p->job, &snap);
    if (snap.cancel_requested) {
        return;
    }
    /* 남은 폴더 수가 늘면 비율이 줄 수 있으므로 막대는 뒤로 가지 않게 한다. */
    done = snap.progress.dirs_scanned;
    all = done + snap.progress.dirs_pending;
    pos = all > 0 ? (int)(done * PROGRESS_RANGE / all) : 0;
    if (pos > p->progress_pos) {
        p->progress_pos = pos;
        SendMessageW(p->progress, PBM_SETPOS, (WPARAM)pos, 0);
    }
    format_size(snap.progress.item_bytes, size, 32);
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"탐지 중... 폴더 %llu · 파일 %llu · 후보 %u개 · %ls · 오류 %llu  |  %ls",
             (unsigned long long)snap.progress.dirs, (unsigned long long)snap.progress.files,
             (unsigned)snap.progress.items, size, (unsigned long long)snap.progress.errors,
             snap.current_dir);
    SetWindowTextW(p->status, text);
}

static void finish_detect(CleanupPanel *p)
{
    CleanupJobSnapshot snap;
    const ScanReport *r;
    ScanStatus status;
    wchar_t text[512];
    const wchar_t *head;

    if (p->job == NULL) {
        return;
    }
    cleanup_job_progress(p->job, &snap);
    status = cleanup_job_finish(p->job, &p->result);
    p->job = NULL;
    p->has_result = 1;
    r = &p->result.report;

    if (r->cancelled) {
        head = L"취소됨(그때까지 찾은 결과)";
    } else if (status == SCAN_COMPLETE) {
        head = L"완료";
    } else if (status == SCAN_PARTIAL) {
        head = L"완료(일부 폴더를 읽지 못함)";
    } else {
        head = L"탐지 실패";
    }
    if (status == SCAN_FATAL) {
        const wchar_t *kind = r->errors_len > 0 ? scan_error_kind_name(r->errors[0].kind)
                                                : L"other";
        swprintf(text, sizeof(text) / sizeof(text[0]), L"%ls: 폴더를 열 수 없습니다 (%ls).",
                 head, kind);
        SendMessageW(p->progress, PBM_SETPOS, 0, 0);
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"%ls: 폴더 %llu · 파일 %llu · 오류 %llu · 건너뛴 폴더 %llu · "
                 L"제외한 시스템 폴더 %llu · 링크 %llu · %.1f초",
                 head, (unsigned long long)r->dir_count, (unsigned long long)r->file_count,
                 (unsigned long long)r->error_count, (unsigned long long)r->skipped_dir_count,
                 (unsigned long long)r->excluded_dir_count,
                 (unsigned long long)r->reparse_count, (double)snap.elapsed_ms / 1000.0);
        if (!r->cancelled) {
            SendMessageW(p->progress, PBM_SETPOS, PROGRESS_RANGE, 0);
        }
    }
    SetWindowTextW(p->status, text);
    set_busy(p, 0);
    set_warning(p);
    apply_filter(p);
    SendMessageW(GetParent(p->hwnd), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(p->hwnd), CPN_FINISHED), (LPARAM)p->hwnd);
}

static void browse_folder(CleanupPanel *p)
{
    BROWSEINFOW bi;
    PIDLIST_ABSOLUTE pidl;
    wchar_t path[MAX_PATH];

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = GetAncestor(p->hwnd, GA_ROOT);
    bi.lpszTitle = L"정리 후보를 찾을 폴더를 선택하세요.";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl == NULL) {
        return;
    }
    if (SHGetPathFromIDListW(pidl, path)) {
        SetWindowTextW(p->path, path);
    }
    CoTaskMemFree(pidl);
}

/* ---- 가상 목록 ---- */

static void on_get_dispinfo(CleanupPanel *p, NMLVDISPINFOW *di)
{
    const CleanupItem *item;
    size_t row = (size_t)di->item.iItem;

    if (!(di->item.mask & LVIF_TEXT) || row >= p->view_len) {
        return;
    }
    item = &p->result.items[p->view[row]];
    switch (di->item.iSubItem) {
    case COL_SIZE:
        format_size(item->bytes, p->cell, 64);
        di->item.pszText = p->cell;
        break;
    case COL_TYPE:
        di->item.pszText = (wchar_t *)category_label(item->category);
        break;
    case COL_PATH:
        /* 결과가 바뀌기 전까지 유효한 문자열을 그대로 넘긴다. */
        di->item.pszText = item->path;
        break;
    case COL_FILES:
        swprintf(p->cell, 64, L"%llu", (unsigned long long)item->files);
        di->item.pszText = p->cell;
        break;
    case COL_NOTE:
        note_text(item, p->cell, 64);
        di->item.pszText = p->cell;
        break;
    }
}

/* ---- 레이아웃·생성 ---- */

static void panel_layout(CleanupPanel *p)
{
    RECT rc;
    int h = pscale(p, 24), bw = pscale(p, 84), gap = pscale(p, 6);
    int w, y, path_w, label_w = pscale(p, 40), combo_w = pscale(p, 230), list_h, sum_x;

    GetClientRect(p->hwnd, &rc);
    w = rc.right - rc.left;

    y = 0;
    path_w = w - 3 * (bw + gap);
    if (path_w < pscale(p, 80)) {
        path_w = pscale(p, 80);
    }
    MoveWindow(p->path, 0, y, path_w, h, TRUE);
    MoveWindow(p->browse, path_w + gap, y, bw, h, TRUE);
    MoveWindow(p->start, path_w + gap + (bw + gap), y, bw, h, TRUE);
    MoveWindow(p->cancel, path_w + gap + 2 * (bw + gap), y, bw, h, TRUE);

    y += h + gap;
    MoveWindow(p->progress, 0, y, w, pscale(p, 16), TRUE);
    y += pscale(p, 16) + gap;
    MoveWindow(p->status, 0, y, w, pscale(p, 20), TRUE);
    y += pscale(p, 20);
    MoveWindow(p->warn, 0, y, w, pscale(p, 20), TRUE);
    y += pscale(p, 20) + gap;

    MoveWindow(p->filter_label, 0, y + pscale(p, 4), label_w, pscale(p, 20), TRUE);
    /* 콤보 상자 높이는 펼친 목록 높이까지 포함한다. */
    MoveWindow(p->filter, label_w, y, combo_w, pscale(p, 200), TRUE);
    sum_x = label_w + combo_w + gap * 2;
    MoveWindow(p->summary, sum_x, y + pscale(p, 4), w - sum_x > 0 ? w - sum_x : 0,
               pscale(p, 20), TRUE);
    y += h + gap;

    list_h = rc.bottom - y;
    MoveWindow(p->list, 0, y, w, list_h > 0 ? list_h : 0, TRUE);
}

static HWND panel_child(CleanupPanel *p, const wchar_t *cls, const wchar_t *text, DWORD style,
                        DWORD ex, int id)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, p->hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)p->font, FALSE);
    return h;
}

static void panel_create_controls(CleanupPanel *p)
{
    static const struct {
        const wchar_t *title;
        int width;
        int fmt;
    } cols[COL_COUNT] = {
        {L"용량", 90, LVCFMT_RIGHT},
        {L"유형", 90, LVCFMT_LEFT},
        {L"경로", 480, LVCFMT_LEFT},
        {L"파일 수", 80, LVCFMT_RIGHT},
        {L"비고", 150, LVCFMT_LEFT},
    };
    size_t i;

    p->path = panel_child(p, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                          IDC_C_PATH);
    p->browse = panel_child(p, L"BUTTON", L"찾아보기...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                            IDC_C_BROWSE);
    p->start = panel_child(p, L"BUTTON", L"탐지", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_C_START);
    p->cancel = panel_child(p, L"BUTTON", L"취소", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_C_CANCEL);
    p->progress = panel_child(p, PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, IDC_C_PROGRESS);
    p->status = panel_child(p, L"STATIC", L"폴더를 선택하고 [탐지]를 누르세요.",
                            SS_LEFTNOWORDWRAP | SS_PATHELLIPSIS, 0, IDC_C_STATUS);
    p->warn = panel_child(p, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, IDC_C_WARN);
    p->filter_label = panel_child(p, L"STATIC", L"유형:", SS_LEFT, 0, IDC_C_FILTER_LABEL);
    p->filter = panel_child(p, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0,
                            IDC_C_FILTER);
    p->summary = panel_child(p, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0,
                             IDC_C_SUMMARY);
    p->list = panel_child(p, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
                              WS_TABSTOP,
                          WS_EX_CLIENTEDGE, IDC_C_LIST);

    SendMessageW(p->path, EM_SETCUEBANNER, TRUE, (LPARAM)L"정리 후보를 찾을 폴더 경로");
    SendMessageW(p->progress, PBM_SETRANGE32, 0, PROGRESS_RANGE);
    for (i = 0; i < FILTER_COUNT; i++) {
        SendMessageW(p->filter, CB_ADDSTRING, 0, (LPARAM)g_filters[i].label);
    }
    SendMessageW(p->filter, CB_SETCURSEL, 0, 0);
    ListView_SetExtendedListViewStyle(p->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < COL_COUNT; i++) {
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = (wchar_t *)cols[i].title;
        col.cx = pscale(p, cols[i].width);
        col.fmt = cols[i].fmt;
        SendMessageW(p->list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
    set_busy(p, 0);
    set_warning(p);
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CleanupPanel *p = panel_of(hwnd);

    switch (msg) {
    case WM_NCCREATE: {
        /* 상태는 창과 수명을 같이 한다(WM_NCDESTROY 에서 해제). */
        const PanelParams *params = ((CREATESTRUCTW *)lp)->lpCreateParams;
        p = calloc(1, sizeof(*p));
        if (p == NULL) {
            return FALSE;
        }
        p->hwnd = hwnd;
        p->font = params->font;
        p->dpi = params->dpi;
        cleanup_result_init(&p->result);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
        break;
    }
    case WM_CREATE:
        panel_create_controls(p);
        return 0;
    case WM_SIZE:
        panel_layout(p);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_C_BROWSE:
            browse_folder(p);
            return 0;
        case IDC_C_START:
            cleanup_panel_start(hwnd, NULL);
            return 0;
        case IDC_C_CANCEL:
            cleanup_panel_cancel(hwnd);
            return 0;
        case IDC_C_FILTER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                apply_filter(p);
            }
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
        /* 보호 경로 경고는 빨간 글씨로. */
        if (p != NULL && (HWND)lp == p->warn && p->has_result && p->result.root_protected) {
            HDC dc = (HDC)wp;
            SetTextColor(dc, RGB(200, 0, 0));
            SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR *)lp;
        if (p != NULL && hdr->idFrom == IDC_C_LIST && hdr->code == LVN_GETDISPINFOW) {
            on_get_dispinfo(p, (NMLVDISPINFOW *)lp);
            return 0;
        }
        break;
    }
    case WM_APP_CLEANUP_PROGRESS:
        if (p != NULL) {
            update_progress(p);
        }
        return 0;
    case WM_APP_CLEANUP_DONE:
        if (p != NULL) {
            finish_detect(p);
        }
        return 0;
    case WM_DESTROY:
        /* 진행 중인 탐지는 멈추고 스레드가 끝날 때까지 기다린다. */
        if (p != NULL && p->job != NULL) {
            cleanup_job_cancel(p->job);
            cleanup_job_finish(p->job, NULL);
            p->job = NULL;
        }
        return 0;
    case WM_NCDESTROY:
        if (p != NULL) {
            free(p->view);
            cleanup_result_free(&p->result);
            free(p);
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- 공개 함수 ---- */

int cleanup_panel_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLEANUP_PANEL_CLASS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 0 : -1;
}

HWND cleanup_panel_create(HWND parent, int id, HFONT font, int dpi)
{
    PanelParams params;

    params.font = font;
    params.dpi = dpi > 0 ? dpi : 96;
    /* WS_EX_CONTROLPARENT: 부모의 IsDialogMessage 가 패널 안의 컨트롤로 Tab 이동하게 한다. */
    return CreateWindowExW(WS_EX_CONTROLPARENT, CLEANUP_PANEL_CLASS, L"",
                           WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), &params);
}

int cleanup_panel_start(HWND panel, const wchar_t *root)
{
    CleanupPanel *p = panel_of(panel);
    wchar_t *target;
    int len;

    if (p == NULL || p->job != NULL) {
        return 0;
    }
    if (root != NULL) {
        SetWindowTextW(p->path, root);
    }
    len = GetWindowTextLengthW(p->path);
    target = malloc(((size_t)len + 1) * sizeof(*target));
    if (target == NULL) {
        return 0;
    }
    GetWindowTextW(p->path, target, len + 1);
    if (target[0] == L'\0') {
        free(target);
        SetWindowTextW(p->status, L"탐지할 폴더를 입력하거나 [찾아보기]로 선택하세요.");
        return 0;
    }

    /* 이전 결과를 지운다. 가상 목록이 옛 항목을 읽지 않도록 개수부터 0 으로 만든다. */
    ListView_SetItemCountEx(p->list, 0, 0);
    free(p->view);
    p->view = NULL;
    p->view_len = 0;
    cleanup_result_free(&p->result);
    p->has_result = 0;
    update_summary(p);
    set_warning(p);
    p->progress_pos = 0;
    SendMessageW(p->progress, PBM_SETPOS, 0, 0);

    p->job = cleanup_job_start(target, PROGRESS_INTERVAL_MS, on_job_notify, panel);
    free(target);
    if (p->job == NULL) {
        SetWindowTextW(p->status, L"탐지를 시작하지 못했습니다(메모리 또는 스레드 생성 실패).");
        return 0;
    }
    set_busy(p, 1);
    SetWindowTextW(p->status, L"탐지 중...");
    return 1;
}

void cleanup_panel_cancel(HWND panel)
{
    CleanupPanel *p = panel_of(panel);

    if (p != NULL && p->job != NULL) {
        cleanup_job_cancel(p->job);
        EnableWindow(p->cancel, FALSE);
        SetWindowTextW(p->status, L"취소하는 중...");
    }
}

int cleanup_panel_busy(HWND panel)
{
    CleanupPanel *p = panel_of(panel);

    return p != NULL && p->job != NULL;
}

#define PANEL_CHECK(cond)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fwprintf(stderr, L"cleanup self-test failed: %hs (line %d)\n", #cond, \
                     __LINE__);                                                   \
            failures++;                                                           \
        }                                                                         \
    } while (0)

/* 목록 칸의 글자를 읽는다(가상 목록이라 LVN_GETDISPINFO 를 거친다). */
static void cell_text(const CleanupPanel *p, int row, int col, wchar_t *buf, int len)
{
    buf[0] = L'\0';
    ListView_GetItemText(p->list, row, col, buf, len);
}

int cleanup_panel_verify(HWND panel, int expect_cancelled)
{
    CleanupPanel *p = panel_of(panel);
    int failures = 0, rows, i;
    size_t f;
    wchar_t buf[64];

    PANEL_CHECK(p != NULL);
    if (p == NULL) {
        return failures;
    }
    PANEL_CHECK(p->job == NULL && p->has_result);
    PANEL_CHECK(IsWindowEnabled(p->start) && !IsWindowEnabled(p->cancel));
    PANEL_CHECK(SendMessageW(p->filter, CB_GETCURSEL, 0, 0) == 0);

    if (expect_cancelled) {
        if (p->result.report.cancelled) {
            PANEL_CHECK(p->result.status == SCAN_PARTIAL);
        }
    } else {
        PANEL_CHECK(!p->result.report.cancelled);
        PANEL_CHECK(p->result.status != SCAN_FATAL);
        PANEL_CHECK(SendMessageW(p->progress, PBM_GETPOS, 0, 0) == PROGRESS_RANGE);
    }

    /* 전체 필터: 모든 항목, 용량 내림차순, 표시 합계 = 예상 확보 용량 */
    rows = ListView_GetItemCount(p->list);
    PANEL_CHECK(rows == (int)p->result.len && p->view_len == p->result.len);
    PANEL_CHECK(p->view_bytes == p->result.total_bytes);
    for (i = 1; i < rows; i++) {
        PANEL_CHECK(p->result.items[p->view[i - 1]].bytes >=
                    p->result.items[p->view[i]].bytes);
    }
    if (rows > 0) {
        const CleanupItem *top = &p->result.items[p->view[0]];
        wchar_t expect[64];

        cell_text(p, 0, COL_SIZE, buf, 64);
        format_size(top->bytes, expect, 64);
        PANEL_CHECK(wcscmp(buf, expect) == 0);
        cell_text(p, 0, COL_TYPE, buf, 64);
        PANEL_CHECK(wcscmp(buf, category_label(top->category)) == 0);
    }

    /* 유형별 필터: 개수·합계가 결과의 유형별 집계와 같고, 다른 유형은 없다 */
    for (f = 1; f < FILTER_COUNT; f++) {
        CleanupCategory category = CLEANUP_NONE;
        size_t c, k;

        for (c = 1; c < CLEANUP_CATEGORY_COUNT; c++) {
            if (g_filters[f].mask == CLEANUP_MASK(c)) {
                category = (CleanupCategory)c;
            }
        }
        SendMessageW(p->filter, CB_SETCURSEL, f, 0);
        apply_filter(p);
        PANEL_CHECK(category != CLEANUP_NONE);
        PANEL_CHECK(ListView_GetItemCount(p->list) == (int)p->result.category_count[category]);
        PANEL_CHECK(p->view_bytes == p->result.category_bytes[category]);
        for (k = 0; k < p->view_len; k++) {
            PANEL_CHECK(p->result.items[p->view[k]].category == category);
        }
    }
    SendMessageW(p->filter, CB_SETCURSEL, 0, 0);
    apply_filter(p);
    PANEL_CHECK(ListView_GetItemCount(p->list) == (int)p->result.len);
    return failures;
}
