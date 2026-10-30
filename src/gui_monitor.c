#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <process.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "gui_monitor.h"
#include "size_format.h"
#include "sysmon.h"

#define MONITOR_PANEL_CLASS L"ByteSieveMonitorPanel"
#define WM_APP_MONITOR_SAMPLE (WM_APP + 1)

/* 그래프에 남기는 측정 개수(1초 간격이면 최근 60초). */
#define HISTORY_LEN 60
/* 측정하지 못한 칸. 그래프에서 선을 끊는다. */
#define HISTORY_GAP (-1)
/* 이 사용률(천분율) 이상이면 막대를 경고 상태로 그린다. */
#define USAGE_WARN_PERMILLE 900

#define CPU_COLOR RGB(38, 160, 218)
#define RAM_COLOR RGB(136, 84, 208)

enum {
    IDC_SUMMARY = 100,
    IDC_RAM_TITLE,
    IDC_RAM_TEXT,
    IDC_RAM_BAR,
    IDC_CPU_TITLE,
    IDC_CPU_TEXT,
    IDC_CPU_BAR
};

typedef struct {
    HWND hwnd, summary, ram_title, ram_text, ram_bar, cpu_title, cpu_text, cpu_bar;
    HFONT font;
    int dpi;
    RECT graph;            /* 그래프 영역(패널 클라이언트 좌표) */
    unsigned processors;   /* 논리 프로세서 수(모르면 0) */

    /* 측정 스레드와 공유한다. lock 으로 보호. */
    CRITICAL_SECTION lock;
    SysmonSample latest;
    int notify_pending;    /* 알림을 보냈고 아직 처리되지 않았으면 1(중복 Post 방지) */

    HANDLE stop;           /* 측정 스레드 종료 요청(수동 재설정 이벤트) */
    HANDLE thread;

    /* UI 스레드 전용. */
    SysmonSample current;
    int has_current;
    unsigned samples;      /* 화면에 반영한 측정 횟수 */
    uint32_t first_tick, last_tick;
    short cpu_history[HISTORY_LEN];
    short ram_history[HISTORY_LEN];
    int history_head;      /* 다음에 쓸 칸 */
    int history_len;
} MonitorPanel;

typedef struct {
    HFONT font;
    int dpi;
} PanelParams;

static MonitorPanel *panel_of(HWND hwnd)
{
    return (MonitorPanel *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static int pscale(const MonitorPanel *p, int px)
{
    return MulDiv(px, p->dpi, 96);
}

/* ---- 측정(작업 스레드) ---- */

static unsigned __stdcall sample_thread(void *arg)
{
    MonitorPanel *p = arg;
    SysMonitor mon;
    DWORD next = GetTickCount();

    sysmon_init(&mon, NULL, NULL, NULL);
    for (;;) {
        SysmonSample s;
        DWORD now, wait;
        int notify;

        sysmon_sample(&mon, &s);
        EnterCriticalSection(&p->lock);
        p->latest = s;
        notify = !p->notify_pending;
        p->notify_pending = 1;
        LeaveCriticalSection(&p->lock);
        /* UI 가 바쁘면 알림을 쌓지 않고 최신 값만 덮어쓴다. */
        if (notify && !PostMessageW(p->hwnd, WM_APP_MONITOR_SAMPLE, 0, 0)) {
            EnterCriticalSection(&p->lock);
            p->notify_pending = 0;
            LeaveCriticalSection(&p->lock);
        }

        /* 측정에 걸린 시간만큼 기다림을 줄여 간격이 밀리지 않게 한다. 많이 늦었으면 다시 맞춘다. */
        next += MONITOR_INTERVAL_MS;
        now = GetTickCount();
        if ((int)(next - now) <= 0) {
            next = now;
            wait = 0;
        } else {
            wait = next - now;
            if (wait > MONITOR_INTERVAL_MS) {
                next = now + MONITOR_INTERVAL_MS;
                wait = MONITOR_INTERVAL_MS;
            }
        }
        if (WaitForSingleObject(p->stop, wait) != WAIT_TIMEOUT) {
            break;
        }
    }
    return 0;
}

static void stop_thread(MonitorPanel *p)
{
    if (p->thread != NULL) {
        SetEvent(p->stop);
        WaitForSingleObject(p->thread, INFINITE);
        CloseHandle(p->thread);
        p->thread = NULL;
    }
}

/* ---- 표시 ---- */

static void memory_text(const SysmonMemory *m, wchar_t *buf, size_t len)
{
    wchar_t used[32], total[32], avail[32];

    switch (m->status) {
    case SYSMON_OK:
        format_size(m->used_bytes, used, 32);
        format_size(m->total_bytes, total, 32);
        format_size(m->available_bytes, avail, 32);
        swprintf(buf, len, L"사용 %ls / 전체 %ls · 사용 가능 %ls · 사용률 %u.%u%%", used, total,
                 avail, m->usage_permille / 10, m->usage_permille % 10);
        break;
    case SYSMON_ERROR:
        swprintf(buf, len, L"측정 실패 (Windows 오류 %lu)", m->error);
        break;
    default:
        swprintf(buf, len, L"측정값이 올바르지 않습니다");
        break;
    }
}

static void cpu_text(const SysmonCpu *c, unsigned processors, wchar_t *buf, size_t len)
{
    switch (c->status) {
    case SYSMON_OK:
        if (processors > 0) {
            swprintf(buf, len, L"사용률 %u.%u%% · 논리 프로세서 %u개", c->usage_permille / 10,
                     c->usage_permille % 10, processors);
        } else {
            swprintf(buf, len, L"사용률 %u.%u%%", c->usage_permille / 10,
                     c->usage_permille % 10);
        }
        break;
    case SYSMON_PENDING:
        swprintf(buf, len, L"측정 중...");
        break;
    case SYSMON_ERROR:
        swprintf(buf, len, L"측정 실패 (Windows 오류 %lu)", c->error);
        break;
    default:
        swprintf(buf, len, L"측정값이 올바르지 않아 다시 측정합니다");
        break;
    }
}

/* 막대 위치와 상태. 측정하지 못했으면 비운다. */
static void set_bar(HWND bar, int ok, unsigned permille)
{
    unsigned pos = ok ? permille : 0;

    SendMessageW(bar, PBM_SETSTATE, pos >= USAGE_WARN_PERMILLE ? PBST_ERROR : PBST_NORMAL, 0);
    SendMessageW(bar, PBM_SETPOS, (WPARAM)pos, 0);
}

static void history_push(MonitorPanel *p, const SysmonSample *s)
{
    p->cpu_history[p->history_head] =
        s->cpu.status == SYSMON_OK ? (short)s->cpu.usage_permille : HISTORY_GAP;
    p->ram_history[p->history_head] =
        s->memory.status == SYSMON_OK ? (short)s->memory.usage_permille : HISTORY_GAP;
    p->history_head = (p->history_head + 1) % HISTORY_LEN;
    if (p->history_len < HISTORY_LEN) {
        p->history_len++;
    }
}

/* i 번째(0 = 가장 오래된) 기록. */
static short history_at(const MonitorPanel *p, const short *h, int i)
{
    return h[(p->history_head - p->history_len + i + HISTORY_LEN) % HISTORY_LEN];
}

static void update_summary(MonitorPanel *p)
{
    wchar_t text[160];
    SYSTEMTIME now;

    if (!p->has_current) {
        SetWindowTextW(p->summary, L"측정을 시작하는 중...");
        return;
    }
    GetLocalTime(&now);
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"%u초마다 갱신 · %02u:%02u:%02u 측정%ls", MONITOR_INTERVAL_MS / 1000,
             (unsigned)now.wHour, (unsigned)now.wMinute, (unsigned)now.wSecond,
             p->current.memory.status == SYSMON_OK &&
                     (p->current.cpu.status == SYSMON_OK ||
                      p->current.cpu.status == SYSMON_PENDING)
                 ? L""
                 : L" · 일부 값을 측정하지 못했습니다");
    SetWindowTextW(p->summary, text);
}

static void apply_sample(MonitorPanel *p, const SysmonSample *s)
{
    wchar_t text[160];

    p->current = *s;
    p->has_current = 1;
    memory_text(&s->memory, text, sizeof(text) / sizeof(text[0]));
    SetWindowTextW(p->ram_text, text);
    cpu_text(&s->cpu, p->processors, text, sizeof(text) / sizeof(text[0]));
    SetWindowTextW(p->cpu_text, text);
    set_bar(p->ram_bar, s->memory.status == SYSMON_OK, s->memory.usage_permille);
    set_bar(p->cpu_bar, s->cpu.status == SYSMON_OK, s->cpu.usage_permille);
    history_push(p, s);
    update_summary(p);
    InvalidateRect(p->hwnd, &p->graph, FALSE);
}

static void on_sample_ready(MonitorPanel *p)
{
    SysmonSample s;

    EnterCriticalSection(&p->lock);
    s = p->latest;
    p->notify_pending = 0;
    LeaveCriticalSection(&p->lock);

    if (p->has_current && s.sequence == p->current.sequence) {
        return;
    }
    if (p->samples == 0) {
        p->first_tick = s.tick;
    }
    p->last_tick = s.tick;
    p->samples++;
    apply_sample(p, &s);
    SendMessageW(GetParent(p->hwnd), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(p->hwnd), MPN_SAMPLED), (LPARAM)p->hwnd);
}

/* 사용률 기록 하나를 꺾은선으로 그린다. 빈 칸에서는 선을 끊는다. */
static void draw_series(MonitorPanel *p, HDC dc, const RECT *plot, const short *h, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, pscale(p, 2), color);
    HGDIOBJ old;
    int i, drawing = 0;
    int w = plot->right - plot->left, ht = plot->bottom - plot->top;

    if (pen == NULL) {
        return;
    }
    old = SelectObject(dc, pen);
    for (i = 0; i < p->history_len; i++) {
        short v = history_at(p, h, i);
        /* 가장 최근 값이 오른쪽 끝에 오도록 맞춘다. */
        int slot = HISTORY_LEN - p->history_len + i;
        int x = plot->left + MulDiv(w, slot, HISTORY_LEN - 1);
        int y;

        if (v == HISTORY_GAP) {
            drawing = 0;
            continue;
        }
        y = plot->bottom - MulDiv(ht, v, 1000);
        if (drawing) {
            LineTo(dc, x, y);
        } else {
            MoveToEx(dc, x, y, NULL);
            /* 점 하나뿐이어도 보이게 한다. */
            LineTo(dc, x + 1, y);
            MoveToEx(dc, x, y, NULL);
            drawing = 1;
        }
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void legend_item(MonitorPanel *p, HDC dc, int *x, int y, COLORREF color,
                        const wchar_t *label)
{
    RECT sw = {*x, y + pscale(p, 4), *x + pscale(p, 10), y + pscale(p, 14)};
    HBRUSH brush = CreateSolidBrush(color);
    SIZE size;

    if (brush != NULL) {
        FillRect(dc, &sw, brush);
        DeleteObject(brush);
    }
    *x = sw.right + pscale(p, 4);
    TextOutW(dc, *x, y, label, (int)wcslen(label));
    GetTextExtentPoint32W(dc, label, (int)wcslen(label), &size);
    *x += size.cx + pscale(p, 12);
}

static void paint_graph(MonitorPanel *p, HDC dc, const RECT *rc)
{
    static const wchar_t title[] = L"최근 60초 사용률";
    RECT plot;
    HGDIOBJ old_font;
    HPEN grid;
    SIZE size;
    int i, x, label_w;

    FillRect(dc, rc, GetSysColorBrush(COLOR_WINDOW));
    FrameRect(dc, rc, GetSysColorBrush(COLOR_BTNSHADOW));
    old_font = SelectObject(dc, p->font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));

    x = rc->left + pscale(p, 8);
    TextOutW(dc, x, rc->top + pscale(p, 4), title, (int)wcslen(title));
    GetTextExtentPoint32W(dc, title, (int)wcslen(title), &size);
    x += size.cx + pscale(p, 16);
    legend_item(p, dc, &x, rc->top + pscale(p, 4), CPU_COLOR, L"CPU");
    legend_item(p, dc, &x, rc->top + pscale(p, 4), RAM_COLOR, L"메모리");

    GetTextExtentPoint32W(dc, L"100%", 4, &size);
    label_w = size.cx + pscale(p, 12);
    plot.left = rc->left + label_w;
    plot.right = rc->right - pscale(p, 10);
    plot.top = rc->top + pscale(p, 28);
    plot.bottom = rc->bottom - pscale(p, 10);
    if (plot.right - plot.left < pscale(p, 20) || plot.bottom - plot.top < pscale(p, 20)) {
        SelectObject(dc, old_font);
        return;
    }

    /* 0·25·50·75·100% 눈금. */
    grid = CreatePen(PS_SOLID, 1, GetSysColor(COLOR_BTNFACE));
    if (grid != NULL) {
        HGDIOBJ old_pen = SelectObject(dc, grid);
        for (i = 0; i <= 4; i++) {
            int y = plot.bottom - MulDiv(plot.bottom - plot.top, i, 4);
            wchar_t label[8];
            RECT lr = {rc->left, y - size.cy / 2, plot.left - pscale(p, 4), y + size.cy / 2};

            MoveToEx(dc, plot.left, y, NULL);
            LineTo(dc, plot.right, y);
            if (i % 2 == 0) {
                swprintf(label, 8, L"%d%%", i * 25);
                SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                DrawTextW(dc, label, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            }
        }
        SelectObject(dc, old_pen);
        DeleteObject(grid);
    }
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));

    draw_series(p, dc, &plot, p->ram_history, RAM_COLOR);
    draw_series(p, dc, &plot, p->cpu_history, CPU_COLOR);
    SelectObject(dc, old_font);
}

/* 그래프는 메모리 DC 에 그린 뒤 한 번에 옮겨 깜박이지 않게 한다. */
static void on_paint(MonitorPanel *p)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(p->hwnd, &ps);
    RECT rc = p->graph, local;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;

    if (w > 0 && h > 0) {
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = mem != NULL ? CreateCompatibleBitmap(dc, w, h) : NULL;

        SetRect(&local, 0, 0, w, h);
        if (bmp != NULL) {
            HGDIOBJ old = SelectObject(mem, bmp);
            paint_graph(p, mem, &local);
            BitBlt(dc, rc.left, rc.top, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
        } else {
            /* 메모리가 부족하면 화면에 바로 그린다. */
            paint_graph(p, dc, &rc);
        }
        if (mem != NULL) {
            DeleteDC(mem);
        }
    }
    EndPaint(p->hwnd, &ps);
}

/* ---- 레이아웃·생성 ---- */

static void panel_layout(MonitorPanel *p)
{
    RECT rc;
    int line = pscale(p, 20), bar_h = pscale(p, 18), gap = pscale(p, 6);
    int title_w = pscale(p, 96);
    int w, y;

    GetClientRect(p->hwnd, &rc);
    w = rc.right - rc.left;
    y = pscale(p, 4);
    MoveWindow(p->summary, 0, y, w, line, TRUE);
    y += line + 2 * gap;

    MoveWindow(p->ram_title, 0, y, title_w, line, TRUE);
    MoveWindow(p->ram_text, title_w, y, w - title_w > 0 ? w - title_w : 0, line, TRUE);
    y += line + gap / 2;
    MoveWindow(p->ram_bar, 0, y, w, bar_h, TRUE);
    y += bar_h + 2 * gap;

    MoveWindow(p->cpu_title, 0, y, title_w, line, TRUE);
    MoveWindow(p->cpu_text, title_w, y, w - title_w > 0 ? w - title_w : 0, line, TRUE);
    y += line + gap / 2;
    MoveWindow(p->cpu_bar, 0, y, w, bar_h, TRUE);
    y += bar_h + 2 * gap;

    SetRect(&p->graph, 0, y, w, rc.bottom > y ? rc.bottom : y);
    InvalidateRect(p->hwnd, NULL, TRUE);
}

static HWND panel_child(MonitorPanel *p, const wchar_t *cls, const wchar_t *text, DWORD style,
                        int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, p->hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)p->font, FALSE);
    return h;
}

static void panel_create_controls(MonitorPanel *p)
{
    DWORD text_style = SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS;

    p->summary = panel_child(p, L"STATIC", L"탭을 열면 RAM·CPU 사용률 측정을 시작합니다.",
                             text_style, IDC_SUMMARY);
    p->ram_title = panel_child(p, L"STATIC", L"메모리(RAM)", text_style, IDC_RAM_TITLE);
    p->ram_text = panel_child(p, L"STATIC", L"-", text_style, IDC_RAM_TEXT);
    p->ram_bar = panel_child(p, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_RAM_BAR);
    p->cpu_title = panel_child(p, L"STATIC", L"CPU", text_style, IDC_CPU_TITLE);
    p->cpu_text = panel_child(p, L"STATIC", L"-", text_style, IDC_CPU_TEXT);
    p->cpu_bar = panel_child(p, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_CPU_BAR);
    SendMessageW(p->ram_bar, PBM_SETRANGE32, 0, 1000);
    SendMessageW(p->cpu_bar, PBM_SETRANGE32, 0, 1000);
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    MonitorPanel *p = panel_of(hwnd);

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
        p->processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        InitializeCriticalSection(&p->lock);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)p);
        break;
    }
    case WM_CREATE:
        panel_create_controls(p);
        return 0;
    case WM_SIZE:
        panel_layout(p);
        return 0;
    case WM_ERASEBKGND: {
        /* 그래프 영역은 WM_PAINT 가 통째로 그리므로 지우지 않는다(깜박임 방지).
         * 이 DC 는 BeginPaint 가 돌려줄 DC 와 같으므로 잘라 낸 영역을 되돌려 놓는다. */
        RECT rc;
        HDC dc = (HDC)wp;
        int saved;
        if (p == NULL) {
            break;
        }
        GetClientRect(hwnd, &rc);
        saved = SaveDC(dc);
        ExcludeClipRect(dc, p->graph.left, p->graph.top, p->graph.right, p->graph.bottom);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
        RestoreDC(dc, saved);
        return 1;
    }
    case WM_PAINT:
        if (p != NULL) {
            on_paint(p);
            return 0;
        }
        break;
    case WM_APP_MONITOR_SAMPLE:
        if (p != NULL) {
            on_sample_ready(p);
        }
        return 0;
    case WM_DESTROY:
        /* 측정 스레드는 패널 상태를 쓰므로 해제하기 전에 끝낸다. */
        if (p != NULL) {
            stop_thread(p);
        }
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        if (p != NULL) {
            stop_thread(p);
            if (p->stop != NULL) {
                CloseHandle(p->stop);
            }
            DeleteCriticalSection(&p->lock);
            free(p);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- 공개 함수 ---- */

int monitor_panel_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = MONITOR_PANEL_CLASS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 0 : -1;
}

HWND monitor_panel_create(HWND parent, int id, HFONT font, int dpi)
{
    PanelParams params;

    params.font = font;
    params.dpi = dpi > 0 ? dpi : 96;
    return CreateWindowExW(WS_EX_CONTROLPARENT, MONITOR_PANEL_CLASS, L"",
                           WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), &params);
}

int monitor_panel_start(HWND panel)
{
    MonitorPanel *p = panel_of(panel);

    if (p == NULL) {
        return 0;
    }
    if (p->thread != NULL) {
        return 1;
    }
    if (p->stop == NULL) {
        p->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (p->stop == NULL) {
            SetWindowTextW(p->summary, L"측정을 시작하지 못했습니다(이벤트 생성 실패).");
            return 0;
        }
    }
    ResetEvent(p->stop);
    p->thread = (HANDLE)_beginthreadex(NULL, 0, sample_thread, p, 0, NULL);
    if (p->thread == NULL) {
        SetWindowTextW(p->summary, L"측정을 시작하지 못했습니다(스레드 생성 실패).");
        return 0;
    }
    update_summary(p);
    return 1;
}

unsigned monitor_panel_samples(HWND panel)
{
    MonitorPanel *p = panel_of(panel);

    return p != NULL ? p->samples : 0;
}

#define PANEL_CHECK(cond)                                                         \
    do {                                                                          \
        if (!(cond)) {                                                            \
            fwprintf(stderr, L"monitor self-test failed: %hs (line %d)\n", #cond, \
                     __LINE__);                                                   \
            failures++;                                                           \
        }                                                                         \
    } while (0)

static int text_is(HWND h, const wchar_t *expect)
{
    wchar_t buf[160];

    GetWindowTextW(h, buf, (int)(sizeof(buf) / sizeof(buf[0])));
    return wcscmp(buf, expect) == 0;
}

int monitor_panel_verify(HWND panel)
{
    MonitorPanel *p = panel_of(panel);
    const SysmonSample *s;
    wchar_t expect[160];
    MEMORYSTATUSEX ms;
    SysmonSample fail;
    unsigned avg;
    int failures = 0, before;

    PANEL_CHECK(p != NULL);
    if (p == NULL) {
        return failures;
    }
    PANEL_CHECK(p->thread != NULL);
    PANEL_CHECK(p->has_current && p->samples >= 2);
    if (!p->has_current || p->samples < 2) {
        return failures;
    }
    s = &p->current;

    /* RAM: 값이 앞뒤가 맞고 시스템 물리 메모리와 같아야 한다. */
    PANEL_CHECK(s->memory.status == SYSMON_OK);
    PANEL_CHECK(s->memory.total_bytes > 0);
    PANEL_CHECK(s->memory.available_bytes <= s->memory.total_bytes);
    PANEL_CHECK(s->memory.used_bytes + s->memory.available_bytes == s->memory.total_bytes);
    PANEL_CHECK(s->memory.usage_permille <= 1000);
    memset(&ms, 0, sizeof(ms));
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        PANEL_CHECK(ms.ullTotalPhys == s->memory.total_bytes);
    }
    memory_text(&s->memory, expect, sizeof(expect) / sizeof(expect[0]));
    PANEL_CHECK(text_is(p->ram_text, expect));
    PANEL_CHECK((unsigned)SendMessageW(p->ram_bar, PBM_GETPOS, 0, 0) ==
                s->memory.usage_permille);
    PANEL_CHECK((int)SendMessageW(p->ram_bar, PBM_GETSTATE, 0, 0) ==
                (s->memory.usage_permille >= USAGE_WARN_PERMILLE ? PBST_ERROR : PBST_NORMAL));

    /* CPU: 두 번째 측정부터는 값이 있어야 한다. */
    PANEL_CHECK(s->cpu.status == SYSMON_OK);
    PANEL_CHECK(s->cpu.usage_permille <= 1000);
    cpu_text(&s->cpu, p->processors, expect, sizeof(expect) / sizeof(expect[0]));
    PANEL_CHECK(text_is(p->cpu_text, expect));
    PANEL_CHECK((unsigned)SendMessageW(p->cpu_bar, PBM_GETPOS, 0, 0) ==
                (s->cpu.status == SYSMON_OK ? s->cpu.usage_permille : 0));

    /* 그래프 기록: 가장 최근 칸이 현재 값이다. */
    PANEL_CHECK(p->history_len > 0 && p->history_len <= HISTORY_LEN);
    PANEL_CHECK(p->history_len <= (int)p->samples);
    if (p->history_len > 0) {
        PANEL_CHECK(history_at(p, p->ram_history, p->history_len - 1) ==
                    (short)s->memory.usage_permille);
        PANEL_CHECK(history_at(p, p->cpu_history, p->history_len - 1) ==
                    (short)s->cpu.usage_permille);
    }

    /* 갱신 간격: 평균이 1초 근처여야 한다(부하가 큰 CI 를 고려해 넉넉히). */
    avg = (p->last_tick - p->first_tick) / (p->samples - 1);
    PANEL_CHECK(avg >= MONITOR_INTERVAL_MS * 7 / 10 && avg <= MONITOR_INTERVAL_MS * 2);

    /* 측정 실패: 오류를 문구로 보여 주고 막대를 비우며 그래프 선을 끊는다. */
    memset(&fail, 0, sizeof(fail));
    fail.memory.status = SYSMON_ERROR;
    fail.memory.error = ERROR_ACCESS_DENIED;
    fail.cpu.status = SYSMON_ERROR;
    fail.cpu.error = ERROR_INVALID_FUNCTION;
    fail.sequence = s->sequence;
    before = p->history_len;
    apply_sample(p, &fail);
    PANEL_CHECK(text_is(p->ram_text, L"측정 실패 (Windows 오류 5)"));
    PANEL_CHECK(text_is(p->cpu_text, L"측정 실패 (Windows 오류 1)"));
    PANEL_CHECK(SendMessageW(p->ram_bar, PBM_GETPOS, 0, 0) == 0);
    PANEL_CHECK(SendMessageW(p->cpu_bar, PBM_GETPOS, 0, 0) == 0);
    PANEL_CHECK(p->history_len == (before < HISTORY_LEN ? before + 1 : HISTORY_LEN));
    PANEL_CHECK(history_at(p, p->ram_history, p->history_len - 1) == HISTORY_GAP);
    PANEL_CHECK(history_at(p, p->cpu_history, p->history_len - 1) == HISTORY_GAP);
    /* 그래프가 빈 칸을 포함해도 그릴 수 있어야 한다. */
    UpdateWindow(p->hwnd);
    return failures;
}
