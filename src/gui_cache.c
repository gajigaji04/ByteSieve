#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "cleanup_trash.h"
#include "dev_cache.h"
#include "gui_cache.h"
#include "path.h"
#include "size_format.h"

#define CACHE_PANEL_CLASS L"ByteSieveCachePanel"
#define WM_APP_CACHE_PROGRESS (WM_APP + 1)
#define WM_APP_CACHE_DONE (WM_APP + 2)
#define WM_APP_TRASH_PROGRESS (WM_APP + 3)
#define WM_APP_TRASH_DONE (WM_APP + 4)

#define PROGRESS_INTERVAL_MS 100
#define PROGRESS_RANGE 1000

/* 실패 요약 창에 나열하는 최대 항목 수. */
#define FAILURE_LIST_MAX 10

/* 체크 상자 상태 그림 번호(0 이면 체크 상자 없음). */
#define CHECK_NONE 0
#define CHECK_OFF 1
#define CHECK_ON 2

enum {
    IDC_K_REFRESH = 100,
    IDC_K_CANCEL,
    IDC_K_STATUS,
    IDC_K_PROGRESS,
    IDC_K_INFO,
    IDC_K_CHECK_ALL,
    IDC_K_CHECK_NONE,
    IDC_K_TRASH,
    IDC_K_SELECTION,
    IDC_K_LIST,
    IDC_K_DETAIL
};

/* 목록 열. 첫 열(용량)에 체크 상자가 붙는다. */
enum {
    COL_SIZE,
    COL_TOOL,
    COL_NAME,
    COL_PATH,
    COL_FILES,
    COL_STATE,
    COL_SOURCE,
    COL_NOTE,
    COL_COUNT
};

typedef struct {
    HFONT font;
    int dpi;
} PanelParams;

/* 캐시 항목마다 붙는 정리 상태(caches.items 와 같은 인덱스). */
typedef struct {
    int checked;
    CleanupTrashStatus deny;   /* 정리 판정(PENDING 이면 옮길 수 있음) */
    CleanupTrashStatus result; /* 휴지통·격리 폴더 이동 결과(PENDING 이면 아직 시도 안 함) */
    uint32_t code;
    wchar_t *recycled;         /* 휴지통·격리 폴더 안의 위치(옮겼을 때) */
} ItemState;

typedef struct {
    HWND hwnd;
    HWND refresh, cancel, status, progress, info, check_all, check_none, trash_btn, selection,
        list, detail;
    HFONT font;
    int dpi;

    DevCacheJob *job;
    DevCacheList caches;
    int has_result;
    int loaded;
    ULONGLONG started;

    ItemState *states; /* caches.len 개 */
    size_t *order;     /* 목록 행 → caches 인덱스 */
    int sort_col;
    int sort_desc;

    CleanupGuard guard;
    size_t checked_count;
    uint64_t checked_bytes;
    size_t moved_count;
    uint64_t moved_bytes;
    size_t quarantined_count;

    CleanupTrashJob *trash;
    size_t *trash_index; /* 작업의 n 번째 항목 → caches 인덱스 */
    size_t trash_count;
    int trash_interactive;
    ULONGLONG trash_started;

    wchar_t cell[512]; /* LVN_GETDISPINFO 용 */
} CachePanel;

static CachePanel *panel_of(HWND hwnd)
{
    return (CachePanel *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
}

static int pscale(const CachePanel *p, int px)
{
    return MulDiv(px, p->dpi, 96);
}

static const DevCacheKindInfo *info_of(const DevCache *c)
{
    return dev_cache_kind_info(c->kind);
}

/* ---- 정리 상태 ---- */

/* 실패 코드(HRESULT 또는 Windows 오류)의 시스템 설명. 없으면 빈 문자열. */
static void error_text(uint32_t code, wchar_t *buf, size_t len)
{
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                             code, 0, buf, (DWORD)len, NULL);

    while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n' || buf[n - 1] == L'.' ||
                     buf[n - 1] == L' ')) {
        n--;
    }
    buf[n] = L'\0';
}

static int can_check(const ItemState *st)
{
    return st->deny == CLEANUP_TRASH_PENDING &&
           (st->result == CLEANUP_TRASH_PENDING || st->result == CLEANUP_TRASH_FAILED ||
            st->result == CLEANUP_TRASH_QUARANTINE_FAILED ||
            st->result == CLEANUP_TRASH_CANCELLED);
}

/* 정리 열: 정리 가능 여부(주의 포함), 정리하지 않는 이유, 이동 결과·실패 사유. */
static void state_text(const CachePanel *p, size_t index, wchar_t *buf, size_t len)
{
    const DevCache *c = &p->caches.items[index];
    const ItemState *st = &p->states[index];
    wchar_t why[160];

    if (st->result == CLEANUP_TRASH_FAILED || st->result == CLEANUP_TRASH_QUARANTINE_FAILED) {
        error_text(st->code, why, 160);
        swprintf(buf, len, L"실패: %ls%ls%ls (0x%08X)",
                 st->result == CLEANUP_TRASH_QUARANTINE_FAILED
                     ? cleanup_trash_status_text(CLEANUP_TRASH_QUARANTINE_FAILED)
                     : L"",
                 st->result == CLEANUP_TRASH_QUARANTINE_FAILED ? L" - " : L"",
                 why[0] ? why : L"이동하지 못함", (unsigned)st->code);
    } else if (cleanup_trash_is_denied(st->result) || st->result == CLEANUP_TRASH_NO_RECYCLE) {
        swprintf(buf, len, L"건너뜀: %ls", cleanup_trash_status_text(st->result));
    } else if (st->result != CLEANUP_TRASH_PENDING) {
        swprintf(buf, len, L"%ls", cleanup_trash_status_text(st->result));
    } else if (c->status != DEV_CACHE_FOUND) {
        swprintf(buf, len, L"%ls", dev_cache_status_text(c->status));
    } else if (st->deny == CLEANUP_TRASH_DENY_INCOMPLETE) {
        swprintf(buf, len, L"정리 불가: %ls",
                 c->measured ? L"일부를 읽지 못함(사용 중·권한)" : L"용량을 세지 못함");
    } else if (st->deny != CLEANUP_TRASH_PENDING) {
        swprintf(buf, len, L"보호: %ls", cleanup_trash_status_text(st->deny));
    } else if (c->bytes == 0) {
        swprintf(buf, len, L"정리 가능(비어 있음)");
    } else {
        swprintf(buf, len, L"%ls", info_of(c)->caution ? L"정리 가능 · 주의" : L"정리 가능");
    }
}

static void source_text(const DevCache *c, wchar_t *buf, size_t len)
{
    switch (c->source) {
    case DEV_CACHE_SRC_ENV:
        swprintf(buf, len, L"환경 변수 %ls", c->origin != NULL ? c->origin : L"");
        break;
    case DEV_CACHE_SRC_CONFIG:
        swprintf(buf, len, L"설정 파일 %ls", c->origin != NULL ? c->origin : L"");
        break;
    default:
        swprintf(buf, len, L"기본 위치");
        break;
    }
}

static void free_states(CachePanel *p)
{
    size_t i;

    if (p->states != NULL) {
        for (i = 0; i < p->caches.len; i++) {
            free(p->states[i].recycled);
        }
    }
    free(p->states);
    p->states = NULL;
    free(p->order);
    p->order = NULL;
    p->checked_count = 0;
    p->checked_bytes = 0;
    p->moved_count = 0;
    p->moved_bytes = 0;
    p->quarantined_count = 0;
}

static void recount_checked(CachePanel *p)
{
    size_t i;

    p->checked_count = 0;
    p->checked_bytes = 0;
    for (i = 0; p->states != NULL && i < p->caches.len; i++) {
        if (p->states[i].checked) {
            p->checked_count++;
            p->checked_bytes += p->caches.items[i].bytes;
        }
    }
}

static void set_checked(CachePanel *p, size_t index, int on)
{
    ItemState *st = &p->states[index];

    on = on && can_check(st);
    if (st->checked != on) {
        st->checked = on;
        recount_checked(p);
    }
}

/* ---- 정렬 ---- */

static int cmp_u64(uint64_t a, uint64_t b)
{
    return a < b ? -1 : a > b;
}

/* 열 기준 비교(오름차순). 같으면 0. */
static int compare_rows(const CachePanel *p, size_t a, size_t b)
{
    const DevCache *x = &p->caches.items[a], *y = &p->caches.items[b];

    switch (p->sort_col) {
    case COL_FILES:
        return cmp_u64(x->files, y->files);
    case COL_TOOL: {
        int r = _wcsicmp(info_of(x)->tool, info_of(y)->tool);
        return r != 0 ? r : _wcsicmp(info_of(x)->label, info_of(y)->label);
    }
    case COL_NAME:
        return _wcsicmp(info_of(x)->label, info_of(y)->label);
    case COL_PATH:
        return _wcsicmp(x->path != NULL ? x->path : L"", y->path != NULL ? y->path : L"");
    case COL_STATE:
        return (int)p->states[a].deny - (int)p->states[b].deny;
    case COL_SOURCE:
        return (int)x->source - (int)y->source;
    case COL_NOTE:
        return (int)info_of(x)->caution - (int)info_of(y)->caution;
    default:
        return cmp_u64(x->bytes, y->bytes);
    }
}

/* 지금 정렬 열·방향으로 행 순서를 만든다(항목이 적어 삽입 정렬, 같으면 종류 순서). */
static void sort_rows(CachePanel *p)
{
    size_t i, j, n = p->caches.len;
    HWND header = ListView_GetHeader(p->list);
    int col;

    for (i = 0; p->order != NULL && i < n; i++) {
        size_t cur = p->order[i] = i;
        for (j = i; j > 0; j--) {
            int r = compare_rows(p, p->order[j - 1], cur);
            if (p->sort_desc) {
                r = -r;
            }
            if (r <= 0) {
                break;
            }
            p->order[j] = p->order[j - 1];
        }
        p->order[j] = cur;
    }
    /* 머리글 화살표 */
    for (col = 0; header != NULL && col < COL_COUNT; col++) {
        HDITEMW hd;
        memset(&hd, 0, sizeof(hd));
        hd.mask = HDI_FORMAT;
        Header_GetItem(header, col, &hd);
        hd.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (col == p->sort_col) {
            hd.fmt |= p->sort_desc ? HDF_SORTDOWN : HDF_SORTUP;
        }
        Header_SetItem(header, col, &hd);
    }
    InvalidateRect(p->list, NULL, FALSE);
}

/* 머리글을 누르면 그 열로 정렬한다. 같은 열이면 방향을 바꾼다(용량·파일 수는 처음에 내림차순). */
static void sort_by(CachePanel *p, int col)
{
    if (p->sort_col == col) {
        p->sort_desc = !p->sort_desc;
    } else {
        p->sort_col = col;
        p->sort_desc = col == COL_SIZE || col == COL_FILES;
    }
    sort_rows(p);
}

/* ---- 표시 ---- */

static void update_selection(CachePanel *p)
{
    wchar_t text[256], size[32], moved[32];
    int busy = p->job != NULL || p->trash != NULL;

    format_size(p->checked_bytes, size, 32);
    if (p->moved_count > 0) {
        format_size(p->moved_bytes, moved, 32);
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"선택 %u개 · 예상 확보 %ls   /   휴지통으로 옮김 %u개 · %ls%ls",
                 (unsigned)p->checked_count, size, (unsigned)p->moved_count, moved,
                 p->quarantined_count > 0 ? L" (일부 격리 폴더)" : L"");
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"선택 %u개 · 예상 확보 %ls",
                 (unsigned)p->checked_count, size);
    }
    SetWindowTextW(p->selection, p->states != NULL ? text : L"");
    EnableWindow(p->check_all, !busy && p->states != NULL);
    EnableWindow(p->check_none, !busy && p->checked_count > 0);
    EnableWindow(p->trash_btn, !busy && p->checked_count > 0);
}

static void set_busy(CachePanel *p, int busy)
{
    EnableWindow(p->refresh, !busy);
    EnableWindow(p->cancel, busy);
    update_selection(p);
}

/* 고른 행의 주의 사항·위치 근거를 아래 칸에 보여 준다. */
static void update_detail(CachePanel *p)
{
    int row = ListView_GetNextItem(p->list, -1, LVNI_SELECTED);
    const DevCache *c;
    wchar_t text[1200], source[MAX_PATH + 32], state[256];

    if (row < 0 || p->states == NULL || (size_t)row >= p->caches.len) {
        SetWindowTextW(p->detail, p->has_result ? L"항목을 고르면 정리했을 때 어떻게 되는지와 "
                                                  L"주의 사항이 여기 표시됩니다."
                                                : L"");
        return;
    }
    c = &p->caches.items[p->order[row]];
    source_text(c, source, sizeof(source) / sizeof(source[0]));
    state_text(p, p->order[row], state, 256);
    swprintf(text, sizeof(text) / sizeof(text[0]), L"[%ls %ls] %ls\r\n위치: %ls (%ls) · %ls",
             info_of(c)->tool, info_of(c)->label, info_of(c)->note,
             c->path != NULL ? c->path : L"(모름)", source, state);
    SetWindowTextW(p->detail, text);
}

/* ---- 탐지 ---- */

static void on_job_notify(int finished, void *ctx)
{
    PostMessageW((HWND)ctx, finished ? WM_APP_CACHE_DONE : WM_APP_CACHE_PROGRESS, 0, 0);
}

static void update_progress(CachePanel *p)
{
    DevCacheJobSnapshot snap;
    const DevCacheKindInfo *k;
    wchar_t size[32], text[256];

    if (p->job == NULL) {
        return;
    }
    dev_cache_job_progress(p->job, &snap);
    if (snap.cancel_requested) {
        return;
    }
    if (snap.total > 0) {
        SendMessageW(p->progress, PBM_SETPOS, (WPARAM)(snap.done * PROGRESS_RANGE / snap.total),
                     0);
    }
    format_size(snap.bytes, size, 32);
    k = dev_cache_kind_info(snap.current);
    if (snap.total > 0 && snap.done < snap.total && k != NULL) {
        swprintf(text, sizeof(text) / sizeof(text[0]),
                 L"용량을 세는 중... %ls %ls (%u/%u) · 지금까지 %ls", k->tool, k->label,
                 (unsigned)snap.done + 1, (unsigned)snap.total, size);
    } else {
        swprintf(text, sizeof(text) / sizeof(text[0]), L"캐시 위치를 찾는 중...");
    }
    SetWindowTextW(p->status, text);
}

static void finish_detect(CachePanel *p)
{
    int cancelled;
    size_t i, found = 0, cleanable = 0;
    uint64_t total = 0;
    wchar_t text[256], size[32];

    if (p->job == NULL) {
        return;
    }
    cancelled = dev_cache_job_finish(p->job, &p->caches);
    p->job = NULL;
    p->has_result = 1;
    p->states = calloc(p->caches.len > 0 ? p->caches.len : 1, sizeof(*p->states));
    p->order = malloc((p->caches.len > 0 ? p->caches.len : 1) * sizeof(*p->order));
    if (p->states == NULL || p->order == NULL) {
        free_states(p);
        SetWindowTextW(p->status, L"탐지 결과를 표시하지 못했습니다(메모리 부족).");
        set_busy(p, 0);
        return;
    }
    for (i = 0; i < p->caches.len; i++) {
        const DevCache *c = &p->caches.items[i];
        p->states[i].deny = dev_cache_check(&p->guard, c);
        found += c->status == DEV_CACHE_FOUND;
        cleanable += p->states[i].deny == CLEANUP_TRASH_PENDING;
        total += c->bytes;
    }
    format_size(total, size, 32);
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"%ls: 공용 캐시 %u개 찾음 · 합계 %ls · 정리 가능 %u개 · %.1f초",
             cancelled ? L"취소됨(다 세지 못한 캐시는 정리할 수 없음)" : L"완료", (unsigned)found,
             size, (unsigned)cleanable, (double)(GetTickCount64() - p->started) / 1000.0);
    SetWindowTextW(p->status, text);
    SendMessageW(p->progress, PBM_SETPOS, cancelled ? 0 : PROGRESS_RANGE, 0);
    sort_rows(p);
    ListView_SetItemCountEx(p->list, (int)p->caches.len, 0);
    set_busy(p, 0);
    update_detail(p);
    SendMessageW(GetParent(p->hwnd), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(p->hwnd), KPN_FINISHED), (LPARAM)p->hwnd);
}

/* ---- 휴지통 이동 ---- */

static void on_trash_notify(int finished, void *ctx)
{
    PostMessageW((HWND)ctx, finished ? WM_APP_TRASH_DONE : WM_APP_TRASH_PROGRESS, 0, 0);
}

static void update_trash_progress(CachePanel *p)
{
    CleanupTrashSnapshot snap;
    wchar_t size[32], text[256];

    if (p->trash == NULL) {
        return;
    }
    cleanup_trash_job_progress(p->trash, &snap);
    if (snap.cancel_requested) {
        return;
    }
    if (snap.total > 0) {
        SendMessageW(p->progress, PBM_SETPOS, (WPARAM)(snap.done * PROGRESS_RANGE / snap.total),
                     0);
    }
    format_size(snap.moved_bytes, size, 32);
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"휴지통으로 옮기는 중... %u/%u · 이동 %u개(%ls) · 실패 %u개 (큰 캐시는 오래 걸릴 수 "
             L"있습니다)",
             (unsigned)snap.done, (unsigned)snap.total, (unsigned)snap.moved, size,
             (unsigned)snap.failed);
    SetWindowTextW(p->status, text);
}

/*
 * 확인 창: 고른 캐시마다 종류·용량·경로·주의 사항과, 공용 캐시를 지우면 생기는 일을 알린다.
 * 기본 버튼은 [아니요]이며 [예]일 때만 1.
 */
static int confirm_trash(CachePanel *p, const size_t *index, size_t count)
{
    size_t i, caution = 0, cap = 4096, used = 0;
    wchar_t size[32], *text;
    int answer;

    for (i = 0; i < count; i++) {
        const DevCache *c = &p->caches.items[index[i]];
        cap += wcslen(c->path) + wcslen(info_of(c)->note) + 128;
        caution += info_of(c)->caution;
    }
    text = malloc(cap * sizeof(*text));
    if (text == NULL) {
        return 0;
    }
#define APPEND(...)                                                                   \
    do {                                                                              \
        int n_ = swprintf(text + used, cap - used, __VA_ARGS__);                      \
        if (n_ > 0) {                                                                 \
            used += (size_t)n_;                                                       \
        }                                                                             \
    } while (0)

    format_size(p->checked_bytes, size, 32);
    APPEND(L"선택한 공용 개발 캐시 %u개를 Windows 휴지통으로 옮깁니다.\n예상 확보 용량: %ls\n\n",
           (unsigned)count, size);
    for (i = 0; i < count; i++) {
        const DevCache *c = &p->caches.items[index[i]];
        format_size(c->bytes, size, 32);
        APPEND(L"■ %ls %ls%ls — %ls\n   %ls\n   %ls\n\n", info_of(c)->tool, info_of(c)->label,
               info_of(c)->caution ? L" [주의]" : L"", size, c->path, info_of(c)->note);
    }
    APPEND(L"공용 캐시는 여러 프로젝트가 함께 씁니다. 정리하면 다음 빌드·설치 때 다시 내려받아 "
           L"시간이 걸리고, 인터넷이 없으면 빌드할 수 없을 수 있습니다.\n"
           L"해당 도구(npm·Gradle 데몬·cargo·Visual Studio 등)가 실행 중이면 끝낸 뒤 진행하세요. "
           L"사용 중인 파일이 있으면 옮기지 못하고 실패로 알려 드립니다.\n"
           L"영구 삭제하지 않으며 휴지통에서 복원할 수 있습니다. 휴지통에 넣을 수 없으면 같은 "
           L"드라이브의 격리 폴더(.bytesieve-trash)로 옮기며 [격리 폴더] 탭에서 복원할 수 "
           L"있습니다.\n");
    if (caution > 0) {
        APPEND(L"\n특히 주의: [주의] 표시 %u개는 정리한 뒤 인터넷 없이는 빌드할 수 없습니다.\n",
               (unsigned)caution);
    }
    APPEND(L"\n계속할까요?");
#undef APPEND

    answer = MessageBoxW(GetAncestor(p->hwnd, GA_ROOT), text, L"공용 캐시 정리",
                         MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    free(text);
    return answer == IDYES;
}

static void show_failures(CachePanel *p, size_t failed)
{
    size_t i, shown = 0, cap = 1024 + (size_t)FAILURE_LIST_MAX * (MAX_PATH + 600), used = 0;
    wchar_t *text = malloc(cap * sizeof(*text)), why[512];
    int n;

    if (text == NULL) {
        return;
    }
    n = swprintf(text, cap, L"%u개 캐시를 휴지통으로 옮기지 못했습니다. 이 캐시들은 그대로 "
                            L"남아 있습니다(일부 파일만 옮겨졌을 수 있습니다). 도구를 끝낸 뒤 "
                            L"다시 시도하세요.\n\n",
                 (unsigned)failed);
    used = n > 0 ? (size_t)n : 0;
    for (i = 0; i < p->trash_count && shown < FAILURE_LIST_MAX; i++) {
        size_t index = p->trash_index[i];
        const ItemState *st = &p->states[index];
        if (st->result == CLEANUP_TRASH_MOVED || st->result == CLEANUP_TRASH_QUARANTINED ||
            st->result == CLEANUP_TRASH_CANCELLED) {
            continue;
        }
        state_text(p, index, why, 512);
        n = swprintf(text + used, cap - used, L"%ls\n    %ls\n", p->caches.items[index].path,
                     why);
        used += n > 0 ? (size_t)n : 0;
        shown++;
    }
    MessageBoxW(GetAncestor(p->hwnd, GA_ROOT), text, L"공용 캐시 정리 결과",
                MB_OK | MB_ICONWARNING);
    free(text);
}

static void finish_trash(CachePanel *p)
{
    CleanupTrashOutcome *out = NULL;
    size_t n, i, moved = 0, quarantined = 0, failed = 0, cancelled = 0;
    uint64_t bytes = 0;
    wchar_t text[320], size[32];

    if (p->trash == NULL) {
        return;
    }
    n = cleanup_trash_job_finish(p->trash, &out);
    p->trash = NULL;
    for (i = 0; i < n && i < p->trash_count; i++) {
        size_t index = p->trash_index[i];
        ItemState *st = &p->states[index];

        st->result = out != NULL ? out[i].status : CLEANUP_TRASH_FAILED;
        st->code = out != NULL ? out[i].code : (uint32_t)E_OUTOFMEMORY;
        free(st->recycled);
        st->recycled = NULL;
        if (st->result == CLEANUP_TRASH_MOVED || st->result == CLEANUP_TRASH_QUARANTINED) {
            st->recycled = out[i].recycled_path;
            out[i].recycled_path = NULL;
            moved++;
            quarantined += st->result == CLEANUP_TRASH_QUARANTINED;
            bytes += p->caches.items[index].bytes;
        } else if (st->result == CLEANUP_TRASH_CANCELLED) {
            cancelled++;
        } else {
            failed++;
        }
        if (!can_check(st)) {
            st->checked = 0;
        }
    }
    cleanup_trash_outcomes_free(out, n);
    p->moved_count += moved;
    p->moved_bytes += bytes;
    p->quarantined_count += quarantined;
    recount_checked(p);

    format_size(bytes, size, 32);
    swprintf(text, sizeof(text) / sizeof(text[0]),
             L"휴지통으로 이동 완료: 성공 %u개(%ls 확보) · 실패 %u개 · 취소 %u개 · %.1f초%ls",
             (unsigned)moved, size, (unsigned)failed, (unsigned)cancelled,
             (double)(GetTickCount64() - p->trash_started) / 1000.0,
             quarantined > 0 ? L"  |  일부는 격리 폴더로 옮김([격리 폴더] 탭)" : L"");
    SetWindowTextW(p->status, text);
    SendMessageW(p->progress, PBM_SETPOS, cancelled == 0 ? PROGRESS_RANGE : 0, 0);
    set_busy(p, 0);
    InvalidateRect(p->list, NULL, TRUE);
    update_detail(p);
    if (p->trash_interactive && failed > 0) {
        show_failures(p, failed);
    }
    SendMessageW(GetParent(p->hwnd), WM_COMMAND,
                 MAKEWPARAM(GetDlgCtrlID(p->hwnd), KPN_TRASHED), (LPARAM)p->hwnd);
}

/* 체크 상자: 클릭한 행, 또는 Space 로 선택한 행들을 바꾼다. */
static void toggle_rows(CachePanel *p, int clicked)
{
    int row, on = -1;

    if (p->states == NULL || p->job != NULL || p->trash != NULL) {
        return;
    }
    if (clicked >= 0) {
        if ((size_t)clicked < p->caches.len) {
            size_t index = p->order[clicked];
            set_checked(p, index, !p->states[index].checked);
            ListView_RedrawItems(p->list, clicked, clicked);
        }
    } else {
        for (row = ListView_GetNextItem(p->list, -1, LVNI_SELECTED); row >= 0;
             row = ListView_GetNextItem(p->list, row, LVNI_SELECTED)) {
            const ItemState *st = &p->states[p->order[row]];
            if (can_check(st) && !st->checked) {
                on = 1;
            }
        }
        for (row = ListView_GetNextItem(p->list, -1, LVNI_SELECTED); row >= 0;
             row = ListView_GetNextItem(p->list, row, LVNI_SELECTED)) {
            set_checked(p, p->order[row], on == 1);
        }
        InvalidateRect(p->list, NULL, FALSE);
    }
    update_selection(p);
}

/* ---- 가상 목록 ---- */

static void on_get_dispinfo(CachePanel *p, NMLVDISPINFOW *di)
{
    size_t row = (size_t)di->item.iItem, index;
    const DevCache *c;

    if (p->order == NULL || row >= p->caches.len) {
        return;
    }
    index = p->order[row];
    c = &p->caches.items[index];
    if (di->item.mask & LVIF_STATE) {
        int image = CHECK_NONE;
        if (can_check(&p->states[index])) {
            image = p->states[index].checked ? CHECK_ON : CHECK_OFF;
        }
        di->item.state = INDEXTOSTATEIMAGEMASK(image);
        di->item.stateMask = LVIS_STATEIMAGEMASK;
    }
    if (!(di->item.mask & LVIF_TEXT)) {
        return;
    }
    p->cell[0] = L'\0';
    di->item.pszText = p->cell;
    switch (di->item.iSubItem) {
    case COL_SIZE:
        if (c->measured) {
            format_size(c->bytes, p->cell, 64);
        } else {
            swprintf(p->cell, 64, L"-");
        }
        break;
    case COL_TOOL:
        di->item.pszText = (wchar_t *)info_of(c)->tool;
        break;
    case COL_NAME:
        di->item.pszText = (wchar_t *)info_of(c)->label;
        break;
    case COL_PATH:
        di->item.pszText = c->path != NULL ? c->path : (wchar_t *)L"(위치 모름)";
        break;
    case COL_FILES:
        if (c->measured) {
            swprintf(p->cell, 64, L"%llu", (unsigned long long)c->files);
        } else {
            swprintf(p->cell, 64, L"-");
        }
        break;
    case COL_STATE:
        state_text(p, index, p->cell, sizeof(p->cell) / sizeof(p->cell[0]));
        break;
    case COL_SOURCE:
        source_text(c, p->cell, sizeof(p->cell) / sizeof(p->cell[0]));
        break;
    case COL_NOTE:
        di->item.pszText = (wchar_t *)info_of(c)->note;
        break;
    }
}

/* ---- 레이아웃·생성 ---- */

static void panel_layout(CachePanel *p)
{
    RECT rc;
    int h = pscale(p, 24), bw = pscale(p, 84), gap = pscale(p, 6), trash_w = pscale(p, 140);
    int w, y, x, list_h, detail_h = pscale(p, 52);

    GetClientRect(p->hwnd, &rc);
    w = rc.right - rc.left;
    y = 0;
    MoveWindow(p->refresh, 0, y, bw, h, TRUE);
    MoveWindow(p->cancel, bw + gap, y, bw, h, TRUE);
    x = 2 * (bw + gap) + gap;
    MoveWindow(p->status, x, y + pscale(p, 4), w - x > 0 ? w - x : 0, pscale(p, 20), TRUE);
    y += h + gap;
    MoveWindow(p->progress, 0, y, w, pscale(p, 16), TRUE);
    y += pscale(p, 16) + gap;
    MoveWindow(p->info, 0, y, w, pscale(p, 34), TRUE);
    y += pscale(p, 34) + gap;

    /* 선택 줄: [모두 선택] [선택 해제] [휴지통으로 이동...]  선택 n개 · 예상 확보 */
    MoveWindow(p->check_all, 0, y, bw, h, TRUE);
    MoveWindow(p->check_none, bw + gap, y, bw, h, TRUE);
    MoveWindow(p->trash_btn, 2 * (bw + gap), y, trash_w, h, TRUE);
    x = 2 * (bw + gap) + trash_w + gap * 2;
    MoveWindow(p->selection, x, y + pscale(p, 4), w - x > 0 ? w - x : 0, pscale(p, 20), TRUE);
    y += h + gap;

    list_h = rc.bottom - y - detail_h - gap;
    MoveWindow(p->list, 0, y, w, list_h > 0 ? list_h : 0, TRUE);
    MoveWindow(p->detail, 0, rc.bottom - detail_h, w, detail_h, TRUE);
}

static HWND panel_child(CachePanel *p, const wchar_t *cls, const wchar_t *text, DWORD style,
                        DWORD ex, int id)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, p->hwnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)p->font, FALSE);
    return h;
}

static void panel_create_controls(CachePanel *p)
{
    static const struct {
        const wchar_t *title;
        int width;
        int fmt;
    } cols[COL_COUNT] = {
        {L"용량", 90, LVCFMT_RIGHT},   {L"도구", 70, LVCFMT_LEFT},
        {L"캐시", 120, LVCFMT_LEFT},   {L"경로", 330, LVCFMT_LEFT},
        {L"파일 수", 80, LVCFMT_RIGHT}, {L"정리", 170, LVCFMT_LEFT},
        {L"위치 근거", 150, LVCFMT_LEFT}, {L"주의 사항", 400, LVCFMT_LEFT},
    };
    size_t i;

    p->refresh = panel_child(p, L"BUTTON", L"다시 탐지", BS_PUSHBUTTON | WS_TABSTOP, 0,
                             IDC_K_REFRESH);
    p->cancel = panel_child(p, L"BUTTON", L"취소", BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_K_CANCEL);
    p->status = panel_child(p, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0,
                            IDC_K_STATUS);
    p->progress = panel_child(p, PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, IDC_K_PROGRESS);
    p->info = panel_child(p, L"STATIC",
                          L"npm·pip·Gradle·Cargo·NuGet이 사용자 폴더에 두는 공용 캐시입니다(프로젝트 "
                          L"안의 캐시는 [정리 후보] 탭). 여러 프로젝트가 함께 쓰므로 정리하면 다음 "
                          L"빌드·설치 때 다시 내려받습니다. 체크한 항목만 확인을 거쳐 휴지통(없는 "
                          L"드라이브는 격리 폴더)으로 옮기며 영구 삭제하지 않습니다. 설정·설치한 "
                          L"도구는 건드리지 않습니다.",
                          SS_LEFT, 0, IDC_K_INFO);
    p->check_all = panel_child(p, L"BUTTON", L"모두 선택", BS_PUSHBUTTON | WS_TABSTOP, 0,
                               IDC_K_CHECK_ALL);
    p->check_none = panel_child(p, L"BUTTON", L"선택 해제", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                IDC_K_CHECK_NONE);
    p->trash_btn = panel_child(p, L"BUTTON", L"휴지통으로 이동(&D)...",
                               BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_K_TRASH);
    p->selection = panel_child(p, L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0,
                               IDC_K_SELECTION);
    p->list = panel_child(p, WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SINGLESEL |
                              WS_TABSTOP,
                          WS_EX_CLIENTEDGE, IDC_K_LIST);
    p->detail = panel_child(p, L"STATIC", L"", SS_LEFT | SS_EDITCONTROL, WS_EX_STATICEDGE,
                            IDC_K_DETAIL);

    SendMessageW(p->progress, PBM_SETRANGE32, 0, PROGRESS_RANGE);
    ListView_SetExtendedListViewStyle(p->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                                                   LVS_EX_CHECKBOXES | LVS_EX_INFOTIP);
    ListView_SetCallbackMask(p->list, LVIS_STATEIMAGEMASK);
    for (i = 0; i < COL_COUNT; i++) {
        LVCOLUMNW col;
        memset(&col, 0, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.pszText = (wchar_t *)cols[i].title;
        col.cx = pscale(p, cols[i].width);
        col.fmt = cols[i].fmt;
        SendMessageW(p->list, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
    p->sort_col = COL_SIZE;
    p->sort_desc = 1;
    set_busy(p, 0);
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    CachePanel *p = panel_of(hwnd);

    switch (msg) {
    case WM_NCCREATE: {
        const PanelParams *params = ((CREATESTRUCTW *)lp)->lpCreateParams;
        p = calloc(1, sizeof(*p));
        if (p == NULL) {
            return FALSE;
        }
        p->hwnd = hwnd;
        p->font = params->font;
        p->dpi = params->dpi;
        dev_cache_list_init(&p->caches);
        cleanup_guard_init(&p->guard);
        cleanup_guard_add_system(&p->guard);
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
        case IDC_K_REFRESH:
            cache_panel_refresh(hwnd);
            return 0;
        case IDC_K_CANCEL:
            cache_panel_cancel(hwnd);
            return 0;
        case IDC_K_CHECK_ALL:
            cache_panel_check_all(hwnd, 1);
            return 0;
        case IDC_K_CHECK_NONE:
            cache_panel_check_all(hwnd, 0);
            return 0;
        case IDC_K_TRASH:
            cache_panel_trash(hwnd, 1);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *hdr = (NMHDR *)lp;
        if (p == NULL || hdr->idFrom != IDC_K_LIST) {
            break;
        }
        if (hdr->code == LVN_GETDISPINFOW) {
            on_get_dispinfo(p, (NMLVDISPINFOW *)lp);
            return 0;
        }
        if (hdr->code == LVN_COLUMNCLICK && p->order != NULL) {
            sort_by(p, ((NMLISTVIEW *)lp)->iSubItem);
            update_detail(p);
            return 0;
        }
        if (hdr->code == LVN_ITEMCHANGED) {
            update_detail(p);
            return 0;
        }
        if (hdr->code == NM_CLICK || hdr->code == NM_DBLCLK) {
            LVHITTESTINFO ht;
            memset(&ht, 0, sizeof(ht));
            ht.pt = ((NMITEMACTIVATE *)lp)->ptAction;
            if (ListView_HitTest(p->list, &ht) >= 0 && (ht.flags & LVHT_ONITEMSTATEICON)) {
                toggle_rows(p, ht.iItem);
            }
            return 0;
        }
        if (hdr->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_SPACE) {
            toggle_rows(p, -1);
            return 0;
        }
        break;
    }
    case WM_APP_CACHE_PROGRESS:
        if (p != NULL) {
            update_progress(p);
        }
        return 0;
    case WM_APP_CACHE_DONE:
        if (p != NULL) {
            finish_detect(p);
        }
        return 0;
    case WM_APP_TRASH_PROGRESS:
        if (p != NULL) {
            update_trash_progress(p);
        }
        return 0;
    case WM_APP_TRASH_DONE:
        if (p != NULL) {
            finish_trash(p);
        }
        return 0;
    case WM_DESTROY:
        if (p != NULL && p->job != NULL) {
            dev_cache_job_cancel(p->job);
            dev_cache_job_finish(p->job, NULL);
            p->job = NULL;
        }
        if (p != NULL && p->trash != NULL) {
            cleanup_trash_job_cancel(p->trash);
            cleanup_trash_job_finish(p->trash, NULL);
            p->trash = NULL;
        }
        return 0;
    case WM_NCDESTROY:
        if (p != NULL) {
            free_states(p);
            free(p->trash_index);
            cleanup_guard_free(&p->guard);
            dev_cache_list_free(&p->caches);
            free(p);
        }
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- 공개 함수 ---- */

int cache_panel_register(HINSTANCE inst)
{
    WNDCLASSEXW wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = panel_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CACHE_PANEL_CLASS;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 0 : -1;
}

HWND cache_panel_create(HWND parent, int id, HFONT font, int dpi)
{
    PanelParams params;

    params.font = font;
    params.dpi = dpi > 0 ? dpi : 96;
    return CreateWindowExW(WS_EX_CONTROLPARENT, CACHE_PANEL_CLASS, L"",
                           WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), &params);
}

int cache_panel_refresh(HWND panel)
{
    CachePanel *p = panel_of(panel);

    if (p == NULL || p->job != NULL || p->trash != NULL) {
        return 0;
    }
    /* 가상 목록이 옛 항목을 읽지 않도록 개수부터 0 으로 만든다. */
    ListView_SetItemCountEx(p->list, 0, 0);
    free_states(p);
    dev_cache_list_free(&p->caches);
    p->has_result = 0;
    p->loaded = 1;
    SendMessageW(p->progress, PBM_SETPOS, 0, 0);
    update_detail(p);
    p->started = GetTickCount64();
    p->job = dev_cache_job_start(on_job_notify, panel);
    if (p->job == NULL) {
        SetWindowTextW(p->status, L"탐지를 시작하지 못했습니다(메모리 또는 스레드 생성 실패).");
        return 0;
    }
    set_busy(p, 1);
    SetWindowTextW(p->status, L"캐시 위치를 찾는 중...");
    return 1;
}

int cache_panel_loaded(HWND panel)
{
    CachePanel *p = panel_of(panel);

    return p != NULL && p->loaded;
}

void cache_panel_cancel(HWND panel)
{
    CachePanel *p = panel_of(panel);

    if (p != NULL && p->job != NULL) {
        dev_cache_job_cancel(p->job);
        EnableWindow(p->cancel, FALSE);
        SetWindowTextW(p->status, L"취소하는 중...");
    } else if (p != NULL && p->trash != NULL) {
        cleanup_trash_job_cancel(p->trash);
        EnableWindow(p->cancel, FALSE);
        SetWindowTextW(p->status, L"취소하는 중... 옮기고 있던 캐시는 끝까지 옮깁니다.");
    }
}

int cache_panel_busy(HWND panel)
{
    CachePanel *p = panel_of(panel);

    return p != NULL && (p->job != NULL || p->trash != NULL);
}

size_t cache_panel_check_all(HWND panel, int check)
{
    CachePanel *p = panel_of(panel);
    size_t i;

    if (p == NULL || p->states == NULL || p->job != NULL || p->trash != NULL) {
        return 0;
    }
    for (i = 0; i < p->caches.len; i++) {
        set_checked(p, i, check);
    }
    InvalidateRect(p->list, NULL, FALSE);
    update_selection(p);
    return p->checked_count;
}

int cache_panel_trash(HWND panel, int confirm)
{
    CachePanel *p = panel_of(panel);
    CleanupItem *items;
    const CleanupItem **item_ptrs;
    const wchar_t **roots;
    size_t *index, i, n = 0;

    if (p == NULL || p->states == NULL || p->job != NULL || p->trash != NULL) {
        return 0;
    }
    if (p->checked_count == 0) {
        SetWindowTextW(p->status, L"휴지통으로 옮길 캐시를 목록에서 체크하세요.");
        return 0;
    }
    index = malloc(p->checked_count * sizeof(*index));
    items = malloc(p->checked_count * sizeof(*items));
    item_ptrs = malloc(p->checked_count * sizeof(*item_ptrs));
    roots = malloc(p->checked_count * sizeof(*roots));
    if (index == NULL || items == NULL || item_ptrs == NULL || roots == NULL) {
        free(index);
        free(items);
        free((void *)item_ptrs);
        free((void *)roots);
        return 0;
    }
    /* 목록에 보이는 순서대로 옮긴다. */
    for (i = 0; i < p->caches.len && n < p->checked_count; i++) {
        if (p->states[p->order[i]].checked) {
            index[n++] = p->order[i];
        }
    }
    if (confirm && !confirm_trash(p, index, n)) {
        free(index);
        free(items);
        free((void *)item_ptrs);
        free((void *)roots);
        SetWindowTextW(p->status, L"정리를 취소했습니다. 아무것도 바뀌지 않았습니다.");
        return 0;
    }
    for (i = 0; i < n; i++) {
        dev_cache_to_item(&p->caches.items[index[i]], &items[i]);
        item_ptrs[i] = &items[i];
        roots[i] = p->caches.items[index[i]].home;
    }
    /* 항목마다 자기 도구 폴더를 탐지 경로로 해 같은 안전 장치를 거친다. */
    p->trash = cleanup_trash_job_start_each(roots, item_ptrs, n, PROGRESS_INTERVAL_MS,
                                            on_trash_notify, panel);
    free(items);
    free((void *)item_ptrs);
    free((void *)roots);
    if (p->trash == NULL) {
        free(index);
        SetWindowTextW(p->status, L"정리를 시작하지 못했습니다(메모리 또는 스레드 생성 실패).");
        return 0;
    }
    free(p->trash_index);
    p->trash_index = index;
    p->trash_count = n;
    p->trash_interactive = confirm;
    p->trash_started = GetTickCount64();
    SendMessageW(p->progress, PBM_SETPOS, 0, 0);
    set_busy(p, 1);
    update_trash_progress(p);
    return 1;
}

#define PANEL_CHECK(cond)                                                       \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fwprintf(stderr, L"cache self-test failed: %hs (line %d)\n", #cond, \
                     __LINE__);                                                 \
            failures++;                                                         \
        }                                                                       \
    } while (0)

/* path 가 root 와 같거나 그 아래인지(대소문자 무시). */
static int is_under(const wchar_t *path, const wchar_t *root)
{
    size_t n = wcslen(root);

    while (n > 0 && path_is_separator(root[n - 1])) {
        n--;
    }
    return path != NULL && _wcsnicmp(path, root, n) == 0 &&
           (path[n] == L'\0' || path_is_separator(path[n]));
}

int cache_panel_verify(HWND panel, const wchar_t *expect_root)
{
    CachePanel *p = panel_of(panel);
    int failures = 0, rows;
    size_t i;
    wchar_t buf[512], want[512];

    PANEL_CHECK(p != NULL && p->states != NULL);
    if (p == NULL || p->states == NULL) {
        return failures;
    }
    PANEL_CHECK(p->job == NULL && p->has_result && p->loaded);
    PANEL_CHECK(IsWindowEnabled(p->refresh) && !IsWindowEnabled(p->cancel));
    PANEL_CHECK(p->caches.len >= DEV_CACHE_KIND_COUNT);
    PANEL_CHECK(SendMessageW(p->progress, PBM_GETPOS, 0, 0) == PROGRESS_RANGE);

    /* 실제 사용자 캐시를 건드리지 않도록 찾은 캐시가 모두 픽스처 안인지 먼저 본다. */
    for (i = 0; i < p->caches.len; i++) {
        const DevCache *c = &p->caches.items[i];
        if (c->path != NULL && !is_under(c->path, expect_root)) {
            fwprintf(stderr, L"cache self-test: %ls is outside %ls\n", c->path, expect_root);
            failures++;
        }
        /* 판정과 체크 가능 여부가 같다 */
        PANEL_CHECK((p->states[i].deny == CLEANUP_TRASH_PENDING) == can_check(&p->states[i]));
        PANEL_CHECK(p->states[i].deny == dev_cache_check(&p->guard, c));
    }

    /* 기본 정렬: 용량 내림차순, 칸 내용 */
    rows = ListView_GetItemCount(p->list);
    PANEL_CHECK(rows == (int)p->caches.len);
    PANEL_CHECK(p->sort_col == COL_SIZE && p->sort_desc);
    for (i = 1; i < p->caches.len; i++) {
        PANEL_CHECK(p->caches.items[p->order[i - 1]].bytes >= p->caches.items[p->order[i]].bytes);
    }
    if (rows > 0) {
        const DevCache *top = &p->caches.items[p->order[0]];
        ListView_GetItemText(p->list, 0, COL_SIZE, buf, 512);
        format_size(top->bytes, want, 512);
        PANEL_CHECK(wcscmp(buf, want) == 0);
        ListView_GetItemText(p->list, 0, COL_TOOL, buf, 512);
        PANEL_CHECK(wcscmp(buf, info_of(top)->tool) == 0);
        ListView_GetItemText(p->list, 0, COL_PATH, buf, 512);
        PANEL_CHECK(top->path != NULL && wcscmp(buf, top->path) == 0);
        ListView_GetItemText(p->list, 0, COL_STATE, buf, 512);
        PANEL_CHECK(wcsncmp(buf, L"정리 가능", 5) == 0);
        ListView_GetItemText(p->list, 0, COL_NOTE, buf, 512);
        PANEL_CHECK(wcscmp(buf, info_of(top)->note) == 0);
    }

    /* 머리글: 파일 수 → 내림차순, 한 번 더 → 오름차순, 용량으로 돌아오면 내림차순 */
    sort_by(p, COL_FILES);
    PANEL_CHECK(p->sort_col == COL_FILES && p->sort_desc);
    sort_by(p, COL_FILES);
    for (i = 1; i < p->caches.len; i++) {
        PANEL_CHECK(p->caches.items[p->order[i - 1]].files <= p->caches.items[p->order[i]].files);
    }
    sort_by(p, COL_TOOL);
    for (i = 1; i < p->caches.len; i++) {
        PANEL_CHECK(_wcsicmp(info_of(&p->caches.items[p->order[i - 1]])->tool,
                             info_of(&p->caches.items[p->order[i]])->tool) <= 0);
    }
    sort_by(p, COL_SIZE);
    PANEL_CHECK(p->sort_col == COL_SIZE && p->sort_desc);

    /* 정리할 수 없는 항목은 체크되지 않는다 */
    for (i = 0; i < p->caches.len; i++) {
        if (!can_check(&p->states[i])) {
            set_checked(p, i, 1);
            PANEL_CHECK(!p->states[i].checked);
        }
    }
    PANEL_CHECK(p->checked_count == 0);
    return failures;
}

int cache_panel_verify_trash(HWND panel, size_t expect_moved)
{
    CachePanel *p = panel_of(panel);
    int failures = 0;
    size_t i, moved = 0;
    wchar_t buf[512];

    PANEL_CHECK(p != NULL && p->states != NULL);
    if (p == NULL || p->states == NULL) {
        return failures;
    }
    PANEL_CHECK(p->trash == NULL && p->job == NULL);
    PANEL_CHECK(IsWindowEnabled(p->refresh) && !IsWindowEnabled(p->cancel));
    PANEL_CHECK(expect_moved > 0 && p->trash_count == expect_moved);
    PANEL_CHECK(p->moved_count == expect_moved && p->checked_count == 0);
    PANEL_CHECK(!IsWindowEnabled(p->trash_btn));
    GetWindowTextW(p->status, buf, 512);
    PANEL_CHECK(wcsncmp(buf, L"휴지통으로 이동 완료: 성공 ", wcslen(L"휴지통으로 이동 완료: 성공 ")) == 0);

    for (i = 0; i < p->caches.len; i++) {
        const ItemState *st = &p->states[i];
        const DevCache *c = &p->caches.items[i];
        if (st->result == CLEANUP_TRASH_MOVED || st->result == CLEANUP_TRASH_QUARANTINED) {
            moved++;
            PANEL_CHECK(GetFileAttributesW(c->path) == INVALID_FILE_ATTRIBUTES);
            /* 도구 폴더(설정·설치한 도구)는 남는다 */
            PANEL_CHECK(GetFileAttributesW(c->home) != INVALID_FILE_ATTRIBUTES);
            PANEL_CHECK(st->recycled != NULL &&
                        GetFileAttributesW(st->recycled) != INVALID_FILE_ATTRIBUTES);
            if (st->recycled != NULL) {
                wprintf(L"recycled: %ls\n", st->recycled);
            }
        } else {
            PANEL_CHECK(st->deny != CLEANUP_TRASH_PENDING && st->result == CLEANUP_TRASH_PENDING);
            if (c->status == DEV_CACHE_FOUND || c->status == DEV_CACHE_UNRECOGNIZED) {
                PANEL_CHECK(GetFileAttributesW(c->path) != INVALID_FILE_ATTRIBUTES);
            }
        }
    }
    fflush(stdout);
    PANEL_CHECK(moved == expect_moved);
    for (i = 0; i < p->caches.len; i++) {
        const ItemState *st = &p->states[p->order[i]];
        ListView_GetItemText(p->list, (int)i, COL_STATE, buf, 512);
        if (st->result == CLEANUP_TRASH_MOVED || st->result == CLEANUP_TRASH_QUARANTINED) {
            PANEL_CHECK(wcscmp(buf, cleanup_trash_status_text(st->result)) == 0);
        }
    }
    GetWindowTextW(p->selection, buf, 512);
    PANEL_CHECK(wcsstr(buf, L"휴지통으로 옮김") != NULL);
    return failures;
}
