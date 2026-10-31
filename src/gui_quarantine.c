#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <process.h>
#include <shellapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "gui_quarantine.h"
#include "path.h"
#include "quarantine.h"
#include "size_format.h"

#define QUARANTINE_PANEL_CLASS L"ByteSieveQuarantinePanel"
#define WM_APP_PURGE_DONE (WM_APP + 1)

/* 확인 창·결과 창에 나열하는 최대 항목 수(나머지는 "외 n개"). */
#define CONFIRM_LIST_MAX 12
#define FAILURE_LIST_MAX 10

enum {
    IDC_Q_REFRESH = 100,
    IDC_Q_RESTORE,
    IDC_Q_PURGE,
    IDC_Q_OPEN,
    IDC_Q_SUMMARY,
    IDC_Q_INFO,
    IDC_Q_LIST,
    IDC_Q_STATUS
};

/* 목록 열. */
enum { COL_STATE, COL_SIZE, COL_ORIGINAL, COL_TIME, COL_LOCATION, COL_NOTE, COL_COUNT };

typedef struct {
    HFONT font;
    int dpi;
} PanelParams;

/* 영구 삭제 작업(스레드가 끝날 때까지 entries 는 바뀌지 않는다). */
typedef struct {
    HANDLE thread;
    HWND hwnd;
    const QuarantineEntry *entries; /* 패널 목록의 항목들 */
    size_t *rows;
    size_t count;
    QuarantineStatus *status;
    uint32_t *code;
} PurgeJob;

typedef struct {
    HWND hwnd;
    HWND refresh, restore, purge, open, summary, info, list, status;
    HFONT font;
    int dpi;

    wchar_t *root; /* NULL 이면 모든 드라이브 */
    QuarantineList entries;
    wchar_t **notes; /* 항목별 마지막 작업 결과(entries.len 개, NULL 가능) */
    size_t unreadable; /* 읽지 못한 드라이브 수 */
    size_t conflicts;  /* 마지막 복원에서 같은 이름 때문에 건너뛴 수 */

    PurgeJob *job;

    wchar_t cell[MAX_PATH + 64]; /* LVN_GETDISPINFO 용 */
} QuarantinePanel;

/* 작업 결과를 목록을 다시 읽은 뒤에도 붙이려고 항목 폴더 경로로 기억한다. */
typedef struct {
    wchar_t *entry;
    wchar_t *note;
} PendingNote;

static QuarantinePanel *panel_of(HWND hwnd)
{
    return (QuarantinePanel *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static int pscale(const QuarantinePanel *p, int px)
{
    return MulDiv(px, p->dpi, 96);
}

/* ---- 표시 문자열 ---- */

static const wchar_t *state_label(QuarantineEntryState state)
{
    switch (state) {
    case QUARANTINE_ENTRY_OK:
        return L"격리됨";
    case QUARANTINE_ENTRY_MISSING:
        return L"기록만 있음(파일 없음)";
    default:
        return L"기록 없음(원래 위치 모름)";
    }
}

static void time_text(uint64_t ft, wchar_t *buf, size_t len)
{
    FILETIME utc, local;
    SYSTEMTIME st;

    buf[0] = L'\0';
    if (ft == 0) {
        return;
    }
    utc.dwLowDateTime = (DWORD)ft;
    utc.dwHighDateTime = (DWORD)(ft >> 32);
    if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &st)) {
        swprintf(buf, len, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond);
    }
}

/* 결과 설명: "복원하지 못함: 접근 거부 ... (시스템 설명)". */
static wchar_t *result_note(const wchar_t *head, QuarantineStatus st, uint32_t code)
{
    wchar_t why[160], *buf = malloc(512 * sizeof(*buf));
    DWORD n = 0;

    if (buf == NULL) {
        return NULL;
    }
    if (code != 0 && st != QUARANTINE_CONFLICT) {
        n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                           0, why, 160, NULL);
    }
    while (n > 0 && (why[n - 1] == L'\r' || why[n - 1] == L'\n' || why[n - 1] == L'.' ||
                     why[n - 1] == L' ')) {
        n--;
    }
    why[n] = L'\0';
    if (n > 0) {
        swprintf(buf, 512, L"%ls: %ls - %ls", head, quarantine_status_text(st), why);
    } else {
        swprintf(buf, 512, L"%ls: %ls", head, quarantine_status_text(st));
    }
    return buf;
}

/* ---- 목록 ---- */

static void free_notes(QuarantinePanel *p)
{
    size_t i;

    if (p->notes != NULL) {
        for (i = 0; i < p->entries.len; i++) {
            free(p->notes[i]);
        }
    }
    free(p->notes);
    p->notes = NULL;
}

static size_t selected_count(const QuarantinePanel *p)
{
    return (size_t)ListView_GetSelectedCount(p->list);
}

static void update_buttons(QuarantinePanel *p)
{
    int busy = p->job != NULL, any = selected_count(p) > 0;

    EnableWindow(p->refresh, !busy);
    EnableWindow(p->restore, !busy && any);
    EnableWindow(p->purge, !busy && any);
    EnableWindow(p->open, p->entries.len > 0);
}

static void update_summary(QuarantinePanel *p)
{
    wchar_t text[256], size[32];
    uint64_t bytes = 0;
    size_t i, ok = 0, other = 0;

    for (i = 0; i < p->entries.len; i++) {
        if (p->entries.items[i].state == QUARANTINE_ENTRY_OK) {
            ok++;
            bytes += p->entries.items[i].bytes;
        } else {
            other++;
        }
    }
    format_size(bytes, size, 32);
    if (other > 0) {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"격리 항목 %u개 · %ls   /   확인 필요 %u개", (unsigned)ok, size,
                 (unsigned)other);
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"격리 항목 %u개 · %ls", (unsigned)ok,
                 size);
    }
    SetWindowTextW(p->summary, text);
}

/* 목록을 다시 읽고, 기억해 둔 작업 결과를 항목 폴더 경로로 다시 붙인다. */
static void reload(QuarantinePanel *p, PendingNote *pending, size_t pending_len)
{
    size_t i, k;

    ListView_SetItemCountEx(p->list, 0, 0);
    free_notes(p);
    if (p->root != NULL) {
        quarantine_list_free(&p->entries);
        p->unreadable = quarantine_list_volume(p->root, &p->entries) == QUARANTINE_OK ? 0 : 1;
    } else {
        p->unreadable = quarantine_list_system(&p->entries);
    }
    if (p->entries.len > 0) {
        p->notes = calloc(p->entries.len, sizeof(*p->notes));
    }
    for (k = 0; k < pending_len && p->notes != NULL; k++) {
        for (i = 0; i < p->entries.len; i++) {
            if (pending[k].note != NULL && _wcsicmp(p->entries.items[i].entry, pending[k].entry) == 0) {
                p->notes[i] = pending[k].note;
                pending[k].note = NULL;
                break;
            }
        }
    }
    ListView_SetItemCountEx(p->list, (int)p->entries.len, 0);
    InvalidateRect(p->list, NULL, TRUE);
    update_summary(p);
    update_buttons(p);
}

static void free_pending(PendingNote *pending, size_t len)
{
    size_t i;

    if (pending == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        free(pending[i].entry);
        free(pending[i].note);
    }
    free(pending);
}

/* 선택한 행 번호들(malloc). */
static size_t *selected_rows(const QuarantinePanel *p, size_t *count)
{
    size_t n = selected_count(p), k = 0;
    size_t *rows = n > 0 ? malloc(n * sizeof(*rows)) : NULL;
    int row;

    *count = 0;
    if (rows == NULL) {
        return NULL;
    }
    for (row = ListView_GetNextItem(p->list, -1, LVNI_SELECTED); row >= 0 && k < n;
         row = ListView_GetNextItem(p->list, row, LVNI_SELECTED)) {
        if ((size_t)row < p->entries.len) {
            rows[k++] = (size_t)row;
        }
    }
    *count = k;
    return rows;
}

/* ---- 복원 ---- */

static void show_text(QuarantinePanel *p, const wchar_t *text, const wchar_t *title, UINT icon)
{
    MessageBoxW(GetAncestor(p->hwnd, GA_ROOT), text, title, MB_OK | icon);
}

/*
 * 같은 이름 때문에 복원하지 못한 항목에 대해 처리 방법을 안내하고, 다른 이름으로 복원할지 묻는다.
 * [예]면 1.
 */
static int ask_alternate(QuarantinePanel *p, const size_t *rows, size_t count)
{
    size_t cap = 2048 + (size_t)CONFIRM_LIST_MAX * (MAX_PATH + 16), used = 0, i;
    wchar_t *text = malloc(cap * sizeof(*text));
    int n, answer;

    if (text == NULL) {
        return 0;
    }
    n = swprintf(text, cap,
                 L"원래 위치에 같은 이름의 항목이 이미 있어 %u개를 복원하지 않았습니다. 기존 "
                 L"항목은 덮어쓰지 않았습니다.\n\n",
                 (unsigned)count);
    used = n > 0 ? (size_t)n : 0;
    for (i = 0; i < count && i < CONFIRM_LIST_MAX; i++) {
        n = swprintf(text + used, cap - used, L"%ls\n", p->entries.items[rows[i]].restore);
        used += n > 0 ? (size_t)n : 0;
    }
    if (count > CONFIRM_LIST_MAX) {
        n = swprintf(text + used, cap - used, L"... 외 %u개\n",
                     (unsigned)(count - CONFIRM_LIST_MAX));
        used += n > 0 ? (size_t)n : 0;
    }
    swprintf(text + used, cap - used,
             L"\n[예]: '이름 (복원됨)'처럼 다른 이름으로 같은 폴더에 복원합니다.\n"
             L"[아니요]: 격리 폴더에 그대로 둡니다. 원래 위치의 항목을 옮기거나 이름을 바꾼 "
             L"뒤 다시 [복원]을 누르면 원래 이름으로 복원됩니다.");
    answer = MessageBoxW(GetAncestor(p->hwnd, GA_ROOT), text, L"같은 이름이 있음",
                         MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
    free(text);
    return answer == IDYES;
}

static void show_failures(QuarantinePanel *p, const wchar_t *title, const wchar_t *head,
                          PendingNote *pending, size_t len, size_t failed)
{
    size_t cap = 1024 + (size_t)FAILURE_LIST_MAX * (MAX_PATH + 600), used = 0, i, shown = 0;
    wchar_t *text = malloc(cap * sizeof(*text));
    int n;

    if (text == NULL) {
        return;
    }
    n = swprintf(text, cap, L"%ls\n\n", head);
    used = n > 0 ? (size_t)n : 0;
    for (i = 0; i < len && shown < FAILURE_LIST_MAX; i++) {
        if (pending[i].note == NULL) {
            continue;
        }
        n = swprintf(text + used, cap - used, L"%ls\n    %ls\n", pending[i].entry, pending[i].note);
        used += n > 0 ? (size_t)n : 0;
        shown++;
    }
    if (failed > shown) {
        swprintf(text + used, cap - used, L"... 외 %u개", (unsigned)(failed - shown));
    }
    show_text(p, text, title, MB_ICONWARNING);
    free(text);
}

size_t quarantine_panel_restore(HWND panel, int interactive)
{
    QuarantinePanel *p = panel_of(panel);
    size_t count = 0, i, restored = 0, failed = 0, conflicts = 0, skipped = 0, pending_len = 0;
    size_t *rows, *conflict_rows = NULL;
    PendingNote *pending = NULL;
    wchar_t text[256];

    if (p == NULL || p->job != NULL) {
        return 0;
    }
    rows = selected_rows(p, &count);
    if (rows == NULL || count == 0) {
        free(rows);
        SetWindowTextW(p->status, L"복원할 항목을 목록에서 선택하세요.");
        return 0;
    }
    conflict_rows = malloc(count * sizeof(*conflict_rows));
    pending = calloc(count, sizeof(*pending));
    if (conflict_rows == NULL || pending == NULL) {
        free(rows);
        free(conflict_rows);
        free(pending);
        return 0;
    }
    for (i = 0; i < count; i++) {
        const QuarantineEntry *e = &p->entries.items[rows[i]];
        QuarantineStatus st;
        uint32_t code = 0;

        if (e->state != QUARANTINE_ENTRY_OK) {
            /* 기록이 없거나 파일이 없는 항목은 복원할 수 없다(그대로 둔다). */
            pending[pending_len].entry = path_dup(e->entry);
            pending[pending_len++].note =
                path_dup(e->state == QUARANTINE_ENTRY_ORPHAN
                             ? L"복원할 수 없음: 원래 위치 기록이 없습니다. [격리 폴더 열기]로 "
                               L"직접 확인하세요."
                             : L"복원할 수 없음: 격리된 파일이 없습니다(기록만 남음).");
            skipped++;
            continue;
        }
        st = quarantine_restore(e, NULL, &code);
        if (st == QUARANTINE_OK) {
            restored++;
        } else if (st == QUARANTINE_CONFLICT) {
            conflict_rows[conflicts++] = rows[i];
        } else {
            pending[pending_len].entry = path_dup(e->entry);
            pending[pending_len++].note = result_note(L"복원하지 못함", st, code);
            failed++;
        }
    }

    /* 같은 이름: 안내하고, 원하면 다른 이름으로 복원한다. */
    if (conflicts > 0 && interactive && ask_alternate(p, conflict_rows, conflicts)) {
        for (i = 0; i < conflicts; i++) {
            const QuarantineEntry *e = &p->entries.items[conflict_rows[i]];
            wchar_t *alt = quarantine_alternate_path(e->restore, e->is_dir);
            uint32_t code = 0;
            QuarantineStatus st = alt != NULL ? quarantine_restore(e, alt, &code)
                                              : QUARANTINE_FAILED;
            free(alt);
            if (st == QUARANTINE_OK) {
                restored++;
            } else {
                pending[pending_len].entry = path_dup(e->entry);
                pending[pending_len++].note = result_note(L"다른 이름으로 복원하지 못함", st, code);
                failed++;
            }
        }
        conflicts = 0;
    }
    for (i = 0; i < conflicts; i++) {
        pending[pending_len].entry = path_dup(p->entries.items[conflict_rows[i]].entry);
        pending[pending_len++].note =
            path_dup(L"건너뜀: 원래 위치에 같은 이름이 있음(덮어쓰지 않음). 기존 항목을 옮긴 뒤 "
                     L"다시 복원하세요.");
    }
    p->conflicts = conflicts;

    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"복원 완료: 성공 %u개 · 같은 이름이 있어 건너뜀 %u개 · 실패 %u개 · 복원할 수 없음 %u개",
             (unsigned)restored, (unsigned)conflicts, (unsigned)failed, (unsigned)skipped);
    if (interactive && failed > 0) {
        show_failures(p, L"복원 결과", L"일부 항목을 복원하지 못했습니다. 이 항목들은 격리 "
                                       L"폴더에 그대로 남아 있습니다.",
                      pending, pending_len, failed + skipped);
    }
    reload(p, pending, pending_len);
    SetWindowTextW(p->status, text);
    free_pending(pending, pending_len);
    free(conflict_rows);
    free(rows);
    return restored;
}

/* ---- 영구 삭제 ---- */

static unsigned __stdcall purge_thread(void *arg)
{
    PurgeJob *job = arg;
    size_t i;

    for (i = 0; i < job->count; i++) {
        job->status[i] = quarantine_purge(&job->entries[job->rows[i]], &job->code[i]);
    }
    PostMessageW(job->hwnd, WM_APP_PURGE_DONE, 0, 0);
    return 0;
}

static void job_free(PurgeJob *job)
{
    if (job != NULL) {
        free(job->rows);
        free(job->status);
        free(job->code);
        free(job);
    }
}

/* 두 번 확인한다: 대상 목록·용량·복구 불가 안내 → 마지막 확인. 둘 다 [예]면 1. */
static int confirm_purge(QuarantinePanel *p, const size_t *rows, size_t count)
{
    size_t cap = 2048 + (size_t)CONFIRM_LIST_MAX * (MAX_PATH + 64), used = 0, i, orphans = 0;
    wchar_t *text = malloc(cap * sizeof(*text)), size[32];
    uint64_t bytes = 0;
    int n, answer;

    if (text == NULL) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const QuarantineEntry *e = &p->entries.items[rows[i]];
        bytes += e->bytes;
        orphans += e->state == QUARANTINE_ENTRY_ORPHAN;
    }
    format_size(bytes, size, 32);
    n = swprintf(text, cap, L"선택한 %u개 항목을 영구 삭제합니다.\n기록된 용량: %ls\n\n",
                 (unsigned)count, size);
    used = n > 0 ? (size_t)n : 0;
    for (i = 0; i < count && i < CONFIRM_LIST_MAX; i++) {
        const QuarantineEntry *e = &p->entries.items[rows[i]];
        format_size(e->bytes, size, 32);
        n = swprintf(text + used, cap - used, L"%ls   %ls\n", size,
                     e->original != NULL ? e->original : e->item);
        used += n > 0 ? (size_t)n : 0;
    }
    if (count > CONFIRM_LIST_MAX) {
        n = swprintf(text + used, cap - used, L"... 외 %u개\n",
                     (unsigned)(count - CONFIRM_LIST_MAX));
        used += n > 0 ? (size_t)n : 0;
    }
    n = swprintf(text + used, cap - used,
                 L"\n영구 삭제한 항목은 휴지통을 거치지 않으며 ByteSieve나 Windows에서 "
                 L"복구할 수 없습니다.\n");
    used += n > 0 ? (size_t)n : 0;
    if (orphans > 0) {
        n = swprintf(text + used, cap - used,
                     L"\n주의: 기록이 없는 항목 %u개는 원래 위치를 알 수 없으며, 격리 폴더 안의 "
                     L"그 항목이 통째로 지워집니다.\n",
                     (unsigned)orphans);
        used += n > 0 ? (size_t)n : 0;
    }
    swprintf(text + used, cap - used, L"\n계속할까요?");
    answer = MessageBoxW(GetAncestor(p->hwnd, GA_ROOT), text, L"영구 삭제",
                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (answer == IDYES) {
        format_size(bytes, size, 32);
        swprintf(text, cap,
                 L"정말로 %u개 항목(%ls)을 영구 삭제할까요?\n\n이 작업은 되돌릴 수 없습니다.",
                 (unsigned)count, size);
        answer = MessageBoxW(GetAncestor(p->hwnd, GA_ROOT), text, L"영구 삭제 - 마지막 확인",
                             MB_YESNO | MB_ICONSTOP | MB_DEFBUTTON2);
    }
    free(text);
    return answer == IDYES;
}

int quarantine_panel_purge(HWND panel, int confirm)
{
    QuarantinePanel *p = panel_of(panel);
    PurgeJob *job;
    size_t count = 0, *rows;

    if (p == NULL || p->job != NULL) {
        return 0;
    }
    rows = selected_rows(p, &count);
    if (rows == NULL || count == 0) {
        free(rows);
        SetWindowTextW(p->status, L"영구 삭제할 항목을 목록에서 선택하세요.");
        return 0;
    }
    if (confirm && !confirm_purge(p, rows, count)) {
        free(rows);
        SetWindowTextW(p->status, L"영구 삭제를 취소했습니다. 아무것도 바뀌지 않았습니다.");
        return 0;
    }
    job = calloc(1, sizeof(*job));
    if (job != NULL) {
        job->hwnd = panel;
        job->entries = p->entries.items;
        job->rows = rows;
        job->count = count;
        job->status = calloc(count, sizeof(*job->status));
        job->code = calloc(count, sizeof(*job->code));
    }
    if (job == NULL || job->status == NULL || job->code == NULL) {
        if (job == NULL) {
            free(rows);
        }
        job_free(job);
        SetWindowTextW(p->status, L"영구 삭제를 시작하지 못했습니다(메모리 부족).");
        return 0;
    }
    job->thread = (HANDLE)_beginthreadex(NULL, 0, purge_thread, job, 0, NULL);
    if (job->thread == NULL) {
        job_free(job);
        SetWindowTextW(p->status, L"영구 삭제를 시작하지 못했습니다(스레드 생성 실패).");
        return 0;
    }
    p->job = job;
    update_buttons(p);
    SetWindowTextW(p->status, L"영구 삭제하는 중...");
    return 1;
}

static void finish_purge(QuarantinePanel *p, int interactive)
{
    PurgeJob *job = p->job;
    PendingNote *pending;
    size_t i, done = 0, failed = 0, len = 0;
    wchar_t text[256];

    if (job == NULL) {
        return;
    }
    WaitForSingleObject(job->thread, INFINITE);
    CloseHandle(job->thread);
    p->job = NULL;
    pending = calloc(job->count > 0 ? job->count : 1, sizeof(*pending));
    for (i = 0; i < job->count; i++) {
        if (job->status[i] == QUARANTINE_OK) {
            done++;
        } else {
            failed++;
            if (pending != NULL) {
                pending[len].entry = path_dup(job->entries[job->rows[i]].entry);
                pending[len++].note = result_note(L"영구 삭제하지 못함", job->status[i],
                                                  job->code[i]);
            }
        }
    }
    swprintf(text, sizeof(text) / sizeof(text[0]), L"영구 삭제 완료: 성공 %u개 · 실패 %u개",
             (unsigned)done, (unsigned)failed);
    if (interactive && failed > 0 && pending != NULL) {
        show_failures(p, L"영구 삭제 결과",
                      L"일부 항목을 영구 삭제하지 못했습니다. 지우지 못한 파일은 격리 폴더에 "
                      L"남아 있습니다.",
                      pending, len, failed);
    }
    job_free(job);
    reload(p, pending, len);
    SetWindowTextW(p->status, text);
    free_pending(pending, len);
    SendMessageW(GetParent(p->hwnd), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(p->hwnd), QPN_PURGED), (LPARAM)p->hwnd);
}

/* 선택한 항목(없으면 첫 항목)의 항목 폴더를 탐색기로 연다. */
static void open_location(QuarantinePanel *p)
{
    int row = ListView_GetNextItem(p->list, -1, LVNI_SELECTED);
    const QuarantineEntry *e;
    const wchar_t *target;

    if (p->entries.len == 0) {
        return;
    }
    e = &p->entries.items[row >= 0 && (size_t)row < p->entries.len ? (size_t)row : 0];
    target = e->state == QUARANTINE_ENTRY_ORPHAN && !e->is_dir ? e->store : e->entry;
    ShellExecuteW(GetAncestor(p->hwnd, GA_ROOT), L"open", target, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- 가상 목록 ---- */

static void on_get_dispinfo(QuarantinePanel *p, NMLVDISPINFOW *di)
{
    const QuarantineEntry *e;
    size_t row = (size_t)di->item.iItem;
    size_t cell_len = sizeof(p->cell) / sizeof(p->cell[0]);

    if (row >= p->entries.len || !(di->item.mask & LVIF_TEXT)) {
        return;
    }
    e = &p->entries.items[row];
    switch (di->item.iSubItem) {
    case COL_STATE:
        di->item.pszText = (wchar_t *)state_label(e->state);
        break;
    case COL_SIZE:
        p->cell[0] = L'\0';
        if (e->state != QUARANTINE_ENTRY_ORPHAN || !e->is_dir) {
            format_size(e->bytes, p->cell, 64);
        }
        di->item.pszText = p->cell;
        break;
    case COL_ORIGINAL:
        di->item.pszText = e->restore != NULL ? e->restore : L"(알 수 없음)";
        break;
    case COL_TIME:
        time_text(e->time, p->cell, cell_len);
        di->item.pszText = p->cell;
        break;
    case COL_LOCATION:
        di->item.pszText = e->item != NULL ? e->item : e->entry;
        break;
    case COL_NOTE:
        di->item.pszText = p->notes != NULL && p->notes[row] != NULL ? p->notes[row] : L"";
        break;
    }
}

/* ---- 레이아웃·생성 ---- */

static void panel_layout(QuarantinePanel *p)
{
    RECT rc;
    int h = pscale(p, 24), bw = pscale(p, 84), wide = pscale(p, 120), gap = pscale(p, 6);
    int w, y, x, list_h;

    GetClientRect(p->hwnd, &rc);
    w = rc.right - rc.left;
    y = 0;
    x = 0;
    MoveWindow(p->refresh, x, y, bw, h, TRUE);
    x += bw + gap;
    MoveWindow(p->restore, x, y, bw, h, TRUE);
    x += bw + gap;
    MoveWindow(p->purge, x, y, wide, h, TRUE);
    x += wide + gap;
    MoveWindow(p->open, x, y, wide, h, TRUE);
    x += wide + gap * 2;
    MoveWindow(p->summary, x, y + pscale(p, 4), w - x > 0 ? w - x : 0, pscale(p, 20), TRUE);
    y += h + gap;
    MoveWindow(p->info, 0, y, w, pscale(p, 20), TRUE);
    y += pscale(p, 20);
    MoveWindow(p->status, 0, y, w, pscale(p, 20), TRUE);
    y += pscale(p, 20) + gap;
    list_h = rc.bottom - y;
    MoveWindow(p->list, 0, y, w, list_h > 0 ? list_h : 0, TRUE);
}

static HWND panel_child(QuarantinePanel *p, const wchar_t *cls, const wchar_t *text, DWORD style,
                        DWORD ex, int id)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, p->hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)p->font, FALSE);
    return h;
}

static void panel_create_controls(QuarantinePanel *p)
{
    static const struct {
        const wchar_t *title;
        int width;
        int fmt;
    } cols[COL_COUNT] = {
        {L"상태", 150, LVCFMT_LEFT},     {L"용량", 90, LVCFMT_RIGHT},
        {L"원래 위치", 360, LVCFMT_LEFT}, {L"격리한 시각", 140, LVCFMT_LEFT},
        {L"격리 위치", 300, LVCFMT_LEFT}, {L"결과", 300, LVCFMT_LEFT},
    };
    size_t i;

    p->refresh = panel_child(p, L"BUTTON", L"새로 고침", BS_PUSHBUTTON | WS_TABSTOP, 0,
                             IDC_Q_REFRESH);
    p->restore = panel_child(p, L"BUTTON", L"복원(&R)", BS_PUSHBUTTON | WS_TABSTOP, 0,
                             IDC_Q_RESTORE);
    p->purge = panel_child(p, L"BUTTON", L"영구 삭제(&P)...", BS_PUSHBUTTON | WS_TABSTOP, 0,
                           IDC_Q_PURGE);
    p->open = panel_child(p, L"BUTTON", L"격리 폴더 열기", BS_PUSHBUTTON | WS_TABSTOP, 0,
                          IDC_Q_OPEN);
    p->summary = panel_child(p, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0,
                             IDC_Q_SUMMARY);
    p->info = panel_child(p, L"STATIC",
                          L"휴지통이 없는 드라이브(USB 등)에서 정리한 항목을 같은 드라이브의 "
                          L"격리 폴더(.bytesieve-trash)에 보관합니다. 복원은 덮어쓰지 않으며, "
                          L"영구 삭제한 항목은 복구할 수 없습니다.",
                          SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, IDC_Q_INFO);
    p->status = panel_child(p, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0,
                            IDC_Q_STATUS);
    p->list = panel_child(p, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP,
                          WS_EX_CLIENTEDGE, IDC_Q_LIST);
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
    update_buttons(p);
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    QuarantinePanel *p = panel_of(hwnd);

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
        quarantine_list_init(&p->entries);
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
        case IDC_Q_REFRESH:
            quarantine_panel_refresh(hwnd);
            return 0;
        case IDC_Q_RESTORE:
            quarantine_panel_restore(hwnd, 1);
            return 0;
        case IDC_Q_PURGE:
            quarantine_panel_purge(hwnd, 1);
            return 0;
        case IDC_Q_OPEN:
            open_location(p);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR *)lp;
        if (p == NULL || hdr->idFrom != IDC_Q_LIST) {
            break;
        }
        if (hdr->code == LVN_GETDISPINFOW) {
            on_get_dispinfo(p, (NMLVDISPINFOW *)lp);
            return 0;
        }
        if (hdr->code == LVN_ITEMCHANGED || hdr->code == LVN_ODSTATECHANGED) {
            update_buttons(p);
            return 0;
        }
        break;
    }
    case WM_APP_PURGE_DONE:
        if (p != NULL) {
            finish_purge(p, IsWindowVisible(hwnd));
        }
        return 0;
    case WM_DESTROY:
        /* 진행 중인 영구 삭제는 끝날 때까지 기다린다(중간에 멈추면 일부만 지워진다). */
        if (p != NULL && p->job != NULL) {
            WaitForSingleObject(p->job->thread, INFINITE);
            CloseHandle(p->job->thread);
            job_free(p->job);
            p->job = NULL;
        }
        return 0;
    case WM_NCDESTROY:
        if (p != NULL) {
            free_notes(p);
            quarantine_list_free(&p->entries);
            free(p->root);
            free(p);
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- 공개 함수 ---- */

int quarantine_panel_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = QUARANTINE_PANEL_CLASS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 0 : -1;
}

HWND quarantine_panel_create(HWND parent, int id, HFONT font, int dpi)
{
    PanelParams params;

    params.font = font;
    params.dpi = dpi > 0 ? dpi : 96;
    return CreateWindowExW(WS_EX_CONTROLPARENT, QUARANTINE_PANEL_CLASS, L"",
                           WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), &params);
}

int quarantine_panel_refresh(HWND panel)
{
    QuarantinePanel *p = panel_of(panel);
    wchar_t text[128];

    if (p == NULL || p->job != NULL) {
        return 0;
    }
    reload(p, NULL, 0);
    if (p->unreadable > 0) {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"격리 폴더를 읽지 못한 드라이브가 %u개 있습니다(링크로 바뀌었거나 접근 거부).",
                 (unsigned)p->unreadable);
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 p->entries.len > 0 ? L"복원하거나 영구 삭제할 항목을 선택하세요."
                                    : L"격리 폴더에 항목이 없습니다.");
    }
    SetWindowTextW(p->status, text);
    return 1;
}

void quarantine_panel_set_root(HWND panel, const wchar_t *volume_root)
{
    QuarantinePanel *p = panel_of(panel);

    if (p == NULL || p->job != NULL) {
        return;
    }
    free(p->root);
    p->root = volume_root != NULL ? path_dup(volume_root) : NULL;
    quarantine_panel_refresh(panel);
}

int quarantine_panel_busy(HWND panel)
{
    QuarantinePanel *p = panel_of(panel);

    return p != NULL && p->job != NULL;
}

size_t quarantine_panel_select_all(HWND panel)
{
    QuarantinePanel *p = panel_of(panel);

    if (p == NULL) {
        return 0;
    }
    ListView_SetItemState(p->list, -1, LVIS_SELECTED, LVIS_SELECTED);
    update_buttons(p);
    return selected_count(p);
}

size_t quarantine_panel_conflicts(HWND panel)
{
    QuarantinePanel *p = panel_of(panel);

    return p != NULL ? p->conflicts : 0;
}

#define PANEL_CHECK(cond)                                                            \
    do {                                                                             \
        if (!(cond)) {                                                               \
            fwprintf(stderr, L"quarantine self-test failed: %hs (line %d)\n", #cond, \
                     __LINE__);                                                      \
            failures++;                                                              \
        }                                                                            \
    } while (0)

int quarantine_panel_verify(HWND panel, size_t expect_rows, size_t expect_ok)
{
    QuarantinePanel *p = panel_of(panel);
    wchar_t buf[MAX_PATH + 64];
    size_t i, ok = 0;
    int failures = 0;

    PANEL_CHECK(p != NULL);
    if (p == NULL) {
        return failures;
    }
    PANEL_CHECK(p->job == NULL && IsWindowEnabled(p->refresh));
    PANEL_CHECK(p->entries.len == expect_rows);
    PANEL_CHECK(ListView_GetItemCount(p->list) == (int)expect_rows);
    for (i = 0; i < p->entries.len; i++) {
        const QuarantineEntry *e = &p->entries.items[i];
        ok += e->state == QUARANTINE_ENTRY_OK;
        ListView_GetItemText(p->list, (int)i, COL_STATE, buf, MAX_PATH + 64);
        PANEL_CHECK(wcscmp(buf, state_label(e->state)) == 0);
        if (e->state == QUARANTINE_ENTRY_OK) {
            ListView_GetItemText(p->list, (int)i, COL_ORIGINAL, buf, MAX_PATH + 64);
            PANEL_CHECK(e->restore != NULL && wcscmp(buf, e->restore) == 0);
            PANEL_CHECK(GetFileAttributesW(e->item) != INVALID_FILE_ATTRIBUTES);
        }
    }
    PANEL_CHECK(ok == expect_ok);
    GetWindowTextW(p->summary, buf, MAX_PATH + 64);
    PANEL_CHECK(wcsncmp(buf, L"격리 항목 ", wcslen(L"격리 항목 ")) == 0);
    PANEL_CHECK(IsWindowEnabled(p->open) == (p->entries.len > 0));
    return failures;
}
