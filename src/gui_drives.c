#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <process.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "drive.h"
#include "gui_drives.h"
#include "size_format.h"

#define DRIVE_PANEL_CLASS L"ByteSieveDrivePanel"
#define WM_APP_DRIVES_DONE (WM_APP + 1)

/* 이 사용률(천분율) 이상이면 막대를 경고 색으로 그린다. */
#define USAGE_WARN_PERMILLE 900

enum { IDC_REFRESH = 100, IDC_SUMMARY, IDC_DRIVE_LIST };

/* 목록 열. */
enum {
    COL_ROOT,
    COL_LABEL,
    COL_TYPE,
    COL_FS,
    COL_TOTAL,
    COL_USED,
    COL_FREE,
    COL_USAGE,
    COL_COUNT
};

typedef struct {
    HWND hwnd, refresh, summary, list;
    HFONT font;
    int dpi;

    DriveList drives;
    int loaded;
    int last_rc;            /* 마지막 drive_list_query 결과 */
    unsigned long last_error;
    int busy;
    unsigned generation;    /* 오래된 조회 결과를 버리기 위한 번호 */
} DrivePanel;

/* CreateWindowExW 로 넘기는 생성 인자. */
typedef struct {
    HFONT font;
    int dpi;
} PanelParams;

/* 작업 스레드가 채워서 패널로 넘기는 결과. 받은 쪽이 free 한다. */
typedef struct {
    HWND panel;
    unsigned generation;
    int rc;
    unsigned long error;
    DriveList drives;
} RefreshJob;

static DrivePanel *panel_of(HWND hwnd)
{
    return (DrivePanel *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static int pscale(const DrivePanel *p, int px)
{
    return MulDiv(px, p->dpi, 96);
}

static const wchar_t *type_label(DriveType type)
{
    switch (type) {
    case DRIVE_TYPE_REMOVABLE:
        return L"이동식";
    case DRIVE_TYPE_FIXED:
        return L"로컬 디스크";
    case DRIVE_TYPE_REMOTE:
        return L"네트워크";
    case DRIVE_TYPE_CDROM:
        return L"CD/DVD";
    case DRIVE_TYPE_RAMDISK:
        return L"RAM 디스크";
    case DRIVE_TYPE_NO_ROOT:
        return L"루트 없음";
    default:
        return L"알 수 없음";
    }
}

static const wchar_t *status_label(DriveStatus status)
{
    switch (status) {
    case DRIVE_STATUS_OK:
        return L"정상";
    case DRIVE_STATUS_NOT_READY:
        return L"미디어 없음/준비 안 됨";
    case DRIVE_STATUS_ACCESS_DENIED:
        return L"접근 거부";
    case DRIVE_STATUS_MISSING:
        return L"분리됨";
    case DRIVE_STATUS_DISCONNECTED:
        return L"연결 안 됨";
    default:
        return L"조회 실패";
    }
}

/* ---- 조회(작업 스레드) ---- */

static unsigned __stdcall refresh_thread(void *arg)
{
    RefreshJob *job = arg;

    job->rc = drive_list_query(&job->drives, &job->error);
    /* 패널이 이미 사라졌으면 결과를 받을 곳이 없으므로 여기서 해제한다. */
    if (!PostMessageW(job->panel, WM_APP_DRIVES_DONE, 0, (LPARAM)job)) {
        free(job);
    }
    return 0;
}

/* ---- 표시 ---- */

static void list_set(HWND list, int row, int col, const wchar_t *text)
{
    LVITEMW lv;

    memset(&lv, 0, sizeof(lv));
    lv.iSubItem = col;
    lv.pszText = (wchar_t *)text;
    SendMessageW(list, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&lv);
}

static void usage_text(const DriveInfo *d, wchar_t *buf, size_t len)
{
    if (d->status == DRIVE_STATUS_OK) {
        unsigned pm = drive_usage_permille(d);
        swprintf(buf, len, L"%u.%u%%", pm / 10, pm % 10);
    } else {
        swprintf(buf, len, L"%ls (오류 %lu)", status_label(d->status), d->error);
    }
}

static void fill_list(DrivePanel *p)
{
    size_t i;

    SendMessageW(p->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(p->list);
    for (i = 0; i < p->drives.len; i++) {
        const DriveInfo *d = &p->drives.items[i];
        wchar_t buf[DRIVE_LABEL_LEN + 64];
        LVITEMW lv;
        int row;

        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_TEXT | LVIF_PARAM;
        lv.iItem = (int)i;
        lv.pszText = (wchar_t *)d->root;
        lv.lParam = (LPARAM)i;
        row = (int)SendMessageW(p->list, LVM_INSERTITEMW, 0, (LPARAM)&lv);
        if (row < 0) {
            break;
        }
        list_set(p->list, row, COL_LABEL, d->label);
        list_set(p->list, row, COL_TYPE, type_label(d->type));
        list_set(p->list, row, COL_FS, d->fs);
        if (d->status == DRIVE_STATUS_OK) {
            format_size(d->total_bytes, buf, 64);
            list_set(p->list, row, COL_TOTAL, buf);
            format_size(drive_used_bytes(d), buf, 64);
            list_set(p->list, row, COL_USED, buf);
            format_size(d->free_bytes, buf, 64);
            list_set(p->list, row, COL_FREE, buf);
        }
        usage_text(d, buf, sizeof(buf) / sizeof(buf[0]));
        list_set(p->list, row, COL_USAGE, buf);
    }
    SendMessageW(p->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(p->list, NULL, TRUE);
}

static void update_summary(DrivePanel *p)
{
    wchar_t text[256], total[32], used[32], avail[32];
    uint64_t sum_total = 0, sum_used = 0, sum_free = 0;
    size_t i, failed = 0;
    SYSTEMTIME now;

    if (p->busy) {
        SetWindowTextW(p->summary, L"드라이브 정보를 가져오는 중...");
        return;
    }
    if (p->last_rc != 0) {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"드라이브 목록을 가져오지 못했습니다 (Windows 오류 %lu).", p->last_error);
        SetWindowTextW(p->summary, text);
        return;
    }
    for (i = 0; i < p->drives.len; i++) {
        const DriveInfo *d = &p->drives.items[i];
        if (d->status == DRIVE_STATUS_OK) {
            sum_total += d->total_bytes;
            sum_used += drive_used_bytes(d);
            sum_free += d->free_bytes;
        } else {
            failed++;
        }
    }
    format_size(sum_total, total, 32);
    format_size(sum_used, used, 32);
    format_size(sum_free, avail, 32);
    GetLocalTime(&now);
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"드라이브 %u개 · 전체 %ls · 사용 %ls · 여유 %ls · 조회 실패 %u개 · %02u:%02u:%02u 기준",
             (unsigned)p->drives.len, total, used, avail, (unsigned)failed,
             (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond);
    SetWindowTextW(p->summary, text);
}

/* 사용률 열을 막대로 그린다. 조회에 실패한 드라이브는 기본 그리기(오류 문구)를 쓴다. */
static LRESULT on_custom_draw(DrivePanel *p, NMLVCUSTOMDRAW *cd)
{
    const DriveInfo *d;
    size_t index;
    RECT rc, bar, fill;
    HBRUSH brush;
    wchar_t text[32];
    unsigned pm;
    int inset;

    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
        break;
    default:
        return CDRF_DODEFAULT;
    }

    index = (size_t)cd->nmcd.lItemlParam;
    if (cd->iSubItem != COL_USAGE || index >= p->drives.len) {
        return CDRF_DODEFAULT;
    }
    d = &p->drives.items[index];
    if (d->status != DRIVE_STATUS_OK) {
        return CDRF_DODEFAULT;
    }
    if (!ListView_GetSubItemRect(p->list, (int)cd->nmcd.dwItemSpec, COL_USAGE, LVIR_BOUNDS,
                                 &rc)) {
        return CDRF_DODEFAULT;
    }

    pm = drive_usage_permille(d);
    inset = pscale(p, 3);
    FillRect(cd->nmcd.hdc, &rc, GetSysColorBrush(COLOR_WINDOW));
    bar = rc;
    InflateRect(&bar, -inset, -inset / 2 - 1);
    if (bar.right <= bar.left || bar.bottom <= bar.top) {
        return CDRF_SKIPDEFAULT;
    }
    FillRect(cd->nmcd.hdc, &bar, GetSysColorBrush(COLOR_BTNFACE));
    fill = bar;
    fill.right = fill.left + MulDiv(bar.right - bar.left, (int)pm, 1000);
    if (fill.right > fill.left) {
        brush = CreateSolidBrush(pm >= USAGE_WARN_PERMILLE ? RGB(218, 38, 38) : RGB(38, 160, 218));
        if (brush != NULL) {
            FillRect(cd->nmcd.hdc, &fill, brush);
            DeleteObject(brush);
        }
    }
    FrameRect(cd->nmcd.hdc, &bar, GetSysColorBrush(COLOR_BTNSHADOW));

    usage_text(d, text, 32);
    SetBkMode(cd->nmcd.hdc, TRANSPARENT);
    SetTextColor(cd->nmcd.hdc, GetSysColor(COLOR_WINDOWTEXT));
    DrawTextW(cd->nmcd.hdc, text, -1, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return CDRF_SKIPDEFAULT;
}

static void on_refresh_done(DrivePanel *p, RefreshJob *job)
{
    if (job->generation != p->generation) {
        /* 더 최근에 시작한 조회가 있다. */
        free(job);
        return;
    }
    p->busy = 0;
    p->loaded = 1;
    p->last_rc = job->rc;
    p->last_error = job->error;
    if (job->rc == 0) {
        p->drives = job->drives;
    } else {
        memset(&p->drives, 0, sizeof(p->drives));
    }
    free(job);

    fill_list(p);
    update_summary(p);
    EnableWindow(p->refresh, TRUE);
    SendMessageW(GetParent(p->hwnd), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(p->hwnd), DPN_REFRESHED), (LPARAM)p->hwnd);
}

/* ---- 레이아웃·생성 ---- */

static void panel_layout(DrivePanel *p)
{
    RECT rc;
    int h = pscale(p, 24), bw = pscale(p, 96), gap = pscale(p, 6);
    int w, list_h;

    GetClientRect(p->hwnd, &rc);
    w = rc.right - rc.left;
    MoveWindow(p->refresh, 0, 0, bw, h, TRUE);
    MoveWindow(p->summary, bw + gap, pscale(p, 4), w - bw - gap > 0 ? w - bw - gap : 0,
               pscale(p, 20), TRUE);
    list_h = rc.bottom - h - gap;
    MoveWindow(p->list, 0, h + gap, w, list_h > 0 ? list_h : 0, TRUE);
}

static HWND panel_child(DrivePanel *p, const wchar_t *cls, const wchar_t *text, DWORD style,
                        DWORD ex, int id)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, p->hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)p->font, FALSE);
    return h;
}

static void panel_create_controls(DrivePanel *p)
{
    static const struct {
        const wchar_t *title;
        int width;
        int fmt;
    } cols[COL_COUNT] = {
        {L"드라이브", 64, LVCFMT_LEFT},
        {L"이름", 140, LVCFMT_LEFT},
        {L"종류", 84, LVCFMT_LEFT},
        {L"파일 시스템", 80, LVCFMT_LEFT},
        {L"전체", 84, LVCFMT_RIGHT},
        {L"사용", 84, LVCFMT_RIGHT},
        {L"여유", 84, LVCFMT_RIGHT},
        {L"사용률", 220, LVCFMT_LEFT},
    };
    int i;

    p->refresh = panel_child(p, L"BUTTON", L"새로고침(&R)", BS_PUSHBUTTON | WS_TABSTOP, 0,
                             IDC_REFRESH);
    p->summary = panel_child(p, L"STATIC", L"[새로고침]을 누르면 드라이브 정보를 가져옵니다.",
                             SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, IDC_SUMMARY);
    p->list = panel_child(p, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP,
                          WS_EX_CLIENTEDGE, IDC_DRIVE_LIST);
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
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DrivePanel *p = panel_of(hwnd);

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
        if (LOWORD(wp) == IDC_REFRESH) {
            drive_panel_refresh(hwnd);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR *)lp;
        if (p != NULL && hdr->idFrom == IDC_DRIVE_LIST && hdr->code == NM_CUSTOMDRAW) {
            return on_custom_draw(p, (NMLVCUSTOMDRAW *)lp);
        }
        break;
    }
    case WM_APP_DRIVES_DONE:
        if (p != NULL) {
            on_refresh_done(p, (RefreshJob *)lp);
        } else {
            free((RefreshJob *)lp);
        }
        return 0;
    case WM_NCDESTROY:
        /* 진행 중인 조회 스레드는 PostMessage 가 실패하면 결과를 스스로 해제한다. */
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        free(p);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- 공개 함수 ---- */

int drive_panel_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = DRIVE_PANEL_CLASS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 0 : -1;
}

HWND drive_panel_create(HWND parent, int id, HFONT font, int dpi)
{
    PanelParams params;

    params.font = font;
    params.dpi = dpi > 0 ? dpi : 96;
    /* WS_EX_CONTROLPARENT: 부모의 IsDialogMessage 가 패널 안의 컨트롤로 Tab 이동하게 한다. */
    return CreateWindowExW(WS_EX_CONTROLPARENT, DRIVE_PANEL_CLASS, L"",
                           WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), &params);
}

void drive_panel_refresh(HWND panel)
{
    DrivePanel *p = panel_of(panel);
    RefreshJob *job;
    HANDLE thread;

    if (p == NULL || p->busy) {
        return;
    }
    job = calloc(1, sizeof(*job));
    if (job == NULL) {
        SetWindowTextW(p->summary, L"새로고침을 시작하지 못했습니다(메모리 부족).");
        return;
    }
    job->panel = panel;
    job->generation = ++p->generation;
    thread = (HANDLE)_beginthreadex(NULL, 0, refresh_thread, job, 0, NULL);
    if (thread == NULL) {
        free(job);
        SetWindowTextW(p->summary, L"새로고침을 시작하지 못했습니다(스레드 생성 실패).");
        return;
    }
    /* 결과는 메시지로 받으므로 스레드를 기다리지 않는다. */
    CloseHandle(thread);
    p->busy = 1;
    EnableWindow(p->refresh, FALSE);
    update_summary(p);
}

int drive_panel_loaded(HWND panel)
{
    DrivePanel *p = panel_of(panel);

    return p != NULL && p->loaded;
}

#define PANEL_CHECK(cond)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fwprintf(stderr, L"drive self-test failed: %hs (line %d)\n", #cond, \
                     __LINE__);                                                 \
            failures++;                                                         \
        }                                                                       \
    } while (0)

int drive_panel_verify(HWND panel)
{
    DrivePanel *p = panel_of(panel);
    int failures = 0, rows, sys_found = 0;
    unsigned long mask;
    unsigned mask_count = 0;
    wchar_t windir[MAX_PATH];
    size_t i;

    PANEL_CHECK(p != NULL);
    if (p == NULL) {
        return failures;
    }
    PANEL_CHECK(p->loaded && !p->busy);
    PANEL_CHECK(p->last_rc == 0);
    PANEL_CHECK(IsWindowEnabled(p->refresh));

    rows = ListView_GetItemCount(p->list);
    PANEL_CHECK(rows == (int)p->drives.len);
    for (mask = GetLogicalDrives(); mask != 0; mask &= mask - 1) {
        mask_count++;
    }
    PANEL_CHECK(mask_count == (unsigned)p->drives.len);

    if (GetWindowsDirectoryW(windir, MAX_PATH) == 0) {
        windir[0] = L'\0';
    }
    for (i = 0; i < p->drives.len && (int)i < rows; i++) {
        const DriveInfo *d = &p->drives.items[i];
        wchar_t cell[DRIVE_LABEL_LEN + 64], expect[DRIVE_LABEL_LEN + 64];
        LVITEMW lv;

        memset(&lv, 0, sizeof(lv));
        lv.mask = LVIF_PARAM | LVIF_TEXT;
        lv.iItem = (int)i;
        lv.pszText = cell;
        lv.cchTextMax = (int)(sizeof(cell) / sizeof(cell[0]));
        SendMessageW(p->list, LVM_GETITEMW, 0, (LPARAM)&lv);
        PANEL_CHECK((size_t)lv.lParam == i);
        PANEL_CHECK(wcscmp(cell, d->root) == 0);

        ListView_GetItemText(p->list, (int)i, COL_USAGE, cell,
                             (int)(sizeof(cell) / sizeof(cell[0])));
        usage_text(d, expect, sizeof(expect) / sizeof(expect[0]));
        PANEL_CHECK(wcscmp(cell, expect) == 0);

        if (d->status == DRIVE_STATUS_OK) {
            PANEL_CHECK(d->free_bytes <= d->total_bytes);
            PANEL_CHECK(drive_used_bytes(d) + d->free_bytes == d->total_bytes);
            PANEL_CHECK(drive_usage_permille(d) <= 1000);
        } else {
            PANEL_CHECK(d->total_bytes == 0 && d->free_bytes == 0);
        }
        if (windir[0] != L'\0' && towupper(windir[0]) == d->root[0]) {
            sys_found = 1;
            PANEL_CHECK(d->status == DRIVE_STATUS_OK && d->total_bytes > 0);
            PANEL_CHECK(d->type == DRIVE_TYPE_FIXED);
        }
    }
    PANEL_CHECK(windir[0] == L'\0' || sys_found);
    return failures;
}
