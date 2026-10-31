#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <knownfolders.h>
#include <process.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <stdlib.h>
#include <string.h>

#include "cleanup_trash.h"
#include "path.h"
#include "quarantine.h"

/* ---- 상태 이름 ---- */

const wchar_t *cleanup_trash_status_text(CleanupTrashStatus status)
{
    switch (status) {
    case CLEANUP_TRASH_MOVED:
        return L"휴지통으로 이동됨";
    case CLEANUP_TRASH_QUARANTINED:
        return L"격리 폴더로 이동됨";
    case CLEANUP_TRASH_DENY_PATH:
        return L"휴지통이 지원하지 않는 경로(네트워크·너무 긴 경로)";
    case CLEANUP_TRASH_DENY_OUTSIDE:
        return L"탐지 경로 밖";
    case CLEANUP_TRASH_DENY_PROTECTED:
        return L"시스템 보호 경로";
    case CLEANUP_TRASH_DENY_CRITICAL:
        return L"중요 폴더이거나 그 상위";
    case CLEANUP_TRASH_DENY_INCOMPLETE:
        return L"하위를 다 읽지 못한 항목";
    case CLEANUP_TRASH_DENY_DRIVE:
        return L"휴지통·격리 폴더를 쓸 수 없는 드라이브(네트워크·CD 등)";
    case CLEANUP_TRASH_MISSING:
        return L"이미 없음";
    case CLEANUP_TRASH_CHANGED:
        return L"탐지 후 바뀜(링크·종류·실제 위치)";
    case CLEANUP_TRASH_NO_RECYCLE:
        return L"휴지통에 넣을 수 없음(영구 삭제 방지)";
    case CLEANUP_TRASH_FAILED:
        return L"이동 실패";
    case CLEANUP_TRASH_QUARANTINE_FAILED:
        return L"격리 폴더로 옮기지 못함";
    case CLEANUP_TRASH_CANCELLED:
        return L"취소됨";
    default:
        return L"";
    }
}

const wchar_t *cleanup_trash_status_name(CleanupTrashStatus status)
{
    switch (status) {
    case CLEANUP_TRASH_MOVED:
        return L"moved";
    case CLEANUP_TRASH_QUARANTINED:
        return L"quarantined";
    case CLEANUP_TRASH_DENY_PATH:
        return L"deny_path";
    case CLEANUP_TRASH_DENY_OUTSIDE:
        return L"deny_outside";
    case CLEANUP_TRASH_DENY_PROTECTED:
        return L"deny_protected";
    case CLEANUP_TRASH_DENY_CRITICAL:
        return L"deny_critical";
    case CLEANUP_TRASH_DENY_INCOMPLETE:
        return L"deny_incomplete";
    case CLEANUP_TRASH_DENY_DRIVE:
        return L"deny_drive";
    case CLEANUP_TRASH_MISSING:
        return L"missing";
    case CLEANUP_TRASH_CHANGED:
        return L"changed";
    case CLEANUP_TRASH_NO_RECYCLE:
        return L"no_recycle";
    case CLEANUP_TRASH_FAILED:
        return L"failed";
    case CLEANUP_TRASH_QUARANTINE_FAILED:
        return L"quarantine_failed";
    case CLEANUP_TRASH_CANCELLED:
        return L"cancelled";
    default:
        return L"pending";
    }
}

int cleanup_trash_is_denied(CleanupTrashStatus status)
{
    switch (status) {
    case CLEANUP_TRASH_DENY_PATH:
    case CLEANUP_TRASH_DENY_OUTSIDE:
    case CLEANUP_TRASH_DENY_PROTECTED:
    case CLEANUP_TRASH_DENY_CRITICAL:
    case CLEANUP_TRASH_DENY_INCOMPLETE:
    case CLEANUP_TRASH_DENY_DRIVE:
    case CLEANUP_TRASH_MISSING:
    case CLEANUP_TRASH_CHANGED:
        return 1;
    default:
        return 0;
    }
}

/* ---- 경로 비교 ---- */

static int name_eq_n(const wchar_t *a, const wchar_t *b, size_t len)
{
    return len == 0 || CompareStringOrdinal(a, (int)len, b, (int)len, TRUE) == CSTR_EQUAL;
}

/* 확장 접두사를 건너뛰고 끝 구분자를 뺀 범위. "\\?\UNC\..." 는 0. */
static int trim_span(const wchar_t *path, const wchar_t **start, size_t *len)
{
    const wchar_t *s = path;
    size_t n;

    if (path == NULL) {
        return 0;
    }
    if (path_is_extended(s)) {
        s += 4;
        if (_wcsnicmp(s, L"UNC\\", 4) == 0) {
            return 0;
        }
    }
    n = wcslen(s);
    while (n > 0 && path_is_separator(s[n - 1])) {
        n--;
    }
    *start = s;
    *len = n;
    return 1;
}

/* "X:" 또는 "X:\..." 형태의 로컬 드라이브 경로인지. */
static int is_drive_path(const wchar_t *s, size_t len)
{
    return len >= 2 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')) &&
           s[1] == L':' && (len == 2 || path_is_separator(s[2]));
}

/* b 가 a 와 같거나 a 아래에 있는지. */
static int within(const wchar_t *a, size_t alen, const wchar_t *b, size_t blen)
{
    return alen > 0 && alen <= blen && name_eq_n(a, b, alen) &&
           (alen == blen || path_is_separator(b[alen]));
}

/* path 가 list 의 어떤 경로와 같거나 그 상위인지. */
static int is_ancestor_of_any(const CleanupProtect *list, const wchar_t *s, size_t len)
{
    size_t i;

    for (i = 0; i < list->len; i++) {
        if (within(s, len, list->paths[i], wcslen(list->paths[i]))) {
            return 1;
        }
    }
    return 0;
}

/* ---- 보호 목록 ---- */

void cleanup_guard_init(CleanupGuard *g)
{
    cleanup_protect_init(&g->protect);
    cleanup_protect_init(&g->critical);
}

void cleanup_guard_free(CleanupGuard *g)
{
    if (g == NULL) {
        return;
    }
    cleanup_protect_free(&g->protect);
    cleanup_protect_free(&g->critical);
}

size_t cleanup_guard_add_system(CleanupGuard *g)
{
    static const KNOWNFOLDERID *const folders[] = {
        &FOLDERID_Profile,       &FOLDERID_UserProfiles,  &FOLDERID_Public,
        &FOLDERID_Desktop,       &FOLDERID_Documents,     &FOLDERID_Downloads,
        &FOLDERID_Pictures,      &FOLDERID_Music,         &FOLDERID_Videos,
        &FOLDERID_RoamingAppData, &FOLDERID_LocalAppData, &FOLDERID_LocalAppDataLow,
    };
    wchar_t exe[MAX_PATH];
    size_t added, before, i;
    DWORD n;

    if (g == NULL) {
        return 0;
    }
    added = cleanup_protect_add_system(&g->protect);
    before = g->critical.len;
    for (i = 0; i < sizeof(folders) / sizeof(folders[0]); i++) {
        PWSTR path = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(folders[i], KF_FLAG_DONT_VERIFY, NULL, &path))) {
            cleanup_protect_add(&g->critical, path);
        }
        CoTaskMemFree(path);
    }
    /* 실행 중인 프로그램이 들어 있는 폴더(예: 빌드 폴더 안의 bytesieve-gui.exe). */
    n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t *slash = wcsrchr(exe, L'\\');
        if (slash != NULL) {
            *slash = L'\0';
            cleanup_protect_add(&g->critical, exe);
        }
    }
    return added + (g->critical.len - before);
}

CleanupTrashStatus cleanup_guard_check(const CleanupGuard *g, const wchar_t *root,
                                       const CleanupItem *item)
{
    const wchar_t *s, *r;
    size_t len, rlen;

    if (item == NULL || item->path == NULL || !trim_span(item->path, &s, &len) ||
        !is_drive_path(s, len) || wcslen(item->path) >= MAX_PATH) {
        return CLEANUP_TRASH_DENY_PATH;
    }
    if (!trim_span(root, &r, &rlen) || !within(r, rlen, s, len) || len == rlen) {
        return CLEANUP_TRASH_DENY_OUTSIDE;
    }
    /* 격리 폴더 안은 이미 정리한 항목이다(복원·영구 삭제는 격리 폴더 화면에서). */
    if ((item->flags & CLEANUP_ITEM_PROTECTED) || cleanup_in_quarantine(item->path) ||
        cleanup_protect_contains(g != NULL ? &g->protect : NULL, item->path)) {
        return CLEANUP_TRASH_DENY_PROTECTED;
    }
    if (len <= 2 || (g != NULL && (is_ancestor_of_any(&g->critical, s, len) ||
                                   is_ancestor_of_any(&g->protect, s, len)))) {
        return CLEANUP_TRASH_DENY_CRITICAL;
    }
    if (item->flags & CLEANUP_ITEM_INCOMPLETE) {
        return CLEANUP_TRASH_DENY_INCOMPLETE;
    }
    return CLEANUP_TRASH_PENDING;
}

/* ---- 옮기기 직전 확인 ---- */

/* 실제 위치가 표시 경로와 같은지(중간에 정션·심볼릭 링크·subst 가 끼면 다르다). */
static int same_final_path(const wchar_t *path)
{
    wchar_t final_path[MAX_PATH + 8], long_path[MAX_PATH];
    const wchar_t *a, *b;
    size_t alen, blen;
    HANDLE h;
    DWORD n;

    /* FILE_READ_ATTRIBUTES 는 공유 모드와 충돌하지 않아 사용 중인 파일도 열린다. */
    h = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                    NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    n = GetFinalPathNameByHandleW(h, final_path, MAX_PATH + 8,
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= MAX_PATH + 8) {
        return 0;
    }
    /* 짧은 이름(PROGRA~1)으로 입력한 경로도 같은 위치로 보도록 긴 이름으로 바꾼다. */
    n = GetLongPathNameW(path, long_path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return 0;
    }
    if (!trim_span(final_path, &a, &alen) || !trim_span(long_path, &b, &blen)) {
        return 0;
    }
    return alen == blen && name_eq_n(a, b, alen);
}

/*
 * 드라이브 종류로 정리 방법을 정한다: 고정 디스크는 휴지통(*quarantine = 0), 이동식 드라이브는
 * 격리 폴더(*quarantine = 1). 테스트용 BYTESIEVE_QUARANTINE_ROOT 안은 격리 폴더로 본다.
 */
static CleanupTrashStatus pick_method(const CleanupItem *item, int *quarantine)
{
    const wchar_t *s;
    size_t len;
    wchar_t drive[4];
    UINT type;

    if (quarantine_test_root_contains(item->path)) {
        *quarantine = 1;
        return CLEANUP_TRASH_PENDING;
    }
    trim_span(item->path, &s, &len);
    drive[0] = s[0];
    drive[1] = L':';
    drive[2] = L'\\';
    drive[3] = L'\0';
    type = GetDriveTypeW(drive);
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
        return CLEANUP_TRASH_DENY_DRIVE;
    }
    *quarantine = type == DRIVE_REMOVABLE;
    return CLEANUP_TRASH_PENDING;
}

static CleanupTrashStatus verify_on_disk(const CleanupItem *item, uint32_t *code)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    int is_dir;

    if (!GetFileAttributesExW(item->path, GetFileExInfoStandard, &data)) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return CLEANUP_TRASH_MISSING;
        }
        *code = err;
        return CLEANUP_TRASH_FAILED;
    }
    is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        is_dir != ((item->flags & CLEANUP_ITEM_DIR) != 0) || !same_final_path(item->path)) {
        return CLEANUP_TRASH_CHANGED;
    }
    return CLEANUP_TRASH_PENDING;
}

/* ---- IFileOperation 진행 싱크 ---- */

/*
 * 셸이 항목을 휴지통 대신 영구 삭제하려 하면(PreDeleteItem 에 TSF_DELETE_RECYCLE_IF_POSSIBLE
 * 이 없으면) 거부하고, PostDeleteItem 에서 결과와 휴지통 안의 위치를 받는다.
 */
typedef struct {
    IFileOperationProgressSink iface;
    LONG refs;
    int vetoed;
    int deleted;     /* PostDeleteItem 이 불렸음 */
    HRESULT hr;      /* PostDeleteItem 의 결과 */
    wchar_t *recycled_path;
} TrashSink;

static TrashSink *sink_of(IFileOperationProgressSink *iface)
{
    return (TrashSink *)iface;
}

static HRESULT STDMETHODCALLTYPE sink_query(IFileOperationProgressSink *This, REFIID riid,
                                            void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IFileOperationProgressSink)) {
        *out = This;
        InterlockedIncrement(&sink_of(This)->refs);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE sink_add_ref(IFileOperationProgressSink *This)
{
    return (ULONG)InterlockedIncrement(&sink_of(This)->refs);
}

/* 싱크는 호출자 스택에 있으므로 참조가 0 이 되어도 해제하지 않는다. */
static ULONG STDMETHODCALLTYPE sink_release(IFileOperationProgressSink *This)
{
    return (ULONG)InterlockedDecrement(&sink_of(This)->refs);
}

static HRESULT STDMETHODCALLTYPE sink_start(IFileOperationProgressSink *This)
{
    (void)This;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_finish(IFileOperationProgressSink *This, HRESULT hr)
{
    (void)This;
    (void)hr;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_pre_rename(IFileOperationProgressSink *This, DWORD flags,
                                                 IShellItem *item, LPCWSTR name)
{
    (void)This;
    (void)flags;
    (void)item;
    (void)name;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_post_rename(IFileOperationProgressSink *This, DWORD flags,
                                                  IShellItem *item, LPCWSTR name, HRESULT hr,
                                                  IShellItem *created)
{
    (void)This;
    (void)flags;
    (void)item;
    (void)name;
    (void)hr;
    (void)created;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_pre_move(IFileOperationProgressSink *This, DWORD flags,
                                               IShellItem *item, IShellItem *dest, LPCWSTR name)
{
    (void)This;
    (void)flags;
    (void)item;
    (void)dest;
    (void)name;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_post_move(IFileOperationProgressSink *This, DWORD flags,
                                                IShellItem *item, IShellItem *dest, LPCWSTR name,
                                                HRESULT hr, IShellItem *created)
{
    (void)This;
    (void)flags;
    (void)item;
    (void)dest;
    (void)name;
    (void)hr;
    (void)created;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_pre_delete(IFileOperationProgressSink *This, DWORD flags,
                                                 IShellItem *item)
{
    (void)item;
    if (!(flags & TSF_DELETE_RECYCLE_IF_POSSIBLE)) {
        sink_of(This)->vetoed = 1;
        return E_ABORT;
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_post_delete(IFileOperationProgressSink *This, DWORD flags,
                                                  IShellItem *item, HRESULT hr,
                                                  IShellItem *created)
{
    TrashSink *s = sink_of(This);
    PWSTR name = NULL;

    (void)flags;
    (void)item;
    s->deleted = 1;
    s->hr = hr;
    if (SUCCEEDED(hr) && created != NULL && s->recycled_path == NULL &&
        SUCCEEDED(IShellItem_GetDisplayName(created, SIGDN_FILESYSPATH, &name))) {
        s->recycled_path = path_dup(name);
        CoTaskMemFree(name);
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_pre_new(IFileOperationProgressSink *This, DWORD flags,
                                              IShellItem *dest, LPCWSTR name)
{
    (void)This;
    (void)flags;
    (void)dest;
    (void)name;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_post_new(IFileOperationProgressSink *This, DWORD flags,
                                               IShellItem *dest, LPCWSTR name, LPCWSTR tmpl,
                                               DWORD attrs, HRESULT hr, IShellItem *created)
{
    (void)This;
    (void)flags;
    (void)dest;
    (void)name;
    (void)tmpl;
    (void)attrs;
    (void)hr;
    (void)created;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_update(IFileOperationProgressSink *This, UINT total,
                                             UINT done)
{
    (void)This;
    (void)total;
    (void)done;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_timer(IFileOperationProgressSink *This)
{
    (void)This;
    return S_OK;
}

static IFileOperationProgressSinkVtbl g_sink_vtbl = {
    .QueryInterface = sink_query,
    .AddRef = sink_add_ref,
    .Release = sink_release,
    .StartOperations = sink_start,
    .FinishOperations = sink_finish,
    .PreRenameItem = sink_pre_rename,
    .PostRenameItem = sink_post_rename,
    .PreMoveItem = sink_pre_move,
    .PostMoveItem = sink_post_move,
    .PreCopyItem = sink_pre_move, /* 복사와 이동의 인자 형태가 같다 */
    .PostCopyItem = sink_post_move,
    .PreDeleteItem = sink_pre_delete,
    .PostDeleteItem = sink_post_delete,
    .PreNewItem = sink_pre_new,
    .PostNewItem = sink_post_new,
    .UpdateProgress = sink_update,
    .ResetTimer = sink_timer,
    .PauseTimer = sink_timer,
    .ResumeTimer = sink_timer,
};

/*
 * 휴지통으로 옮긴다. *untouched 는 실패했을 때 셸이 항목을 건드리지 않았는지(삭제 단계에
 * 이르지 못했거나 거부했고 원래 경로가 그대로 있음) 알려 준다(격리 폴더로 바꿔 옮겨도 되는지).
 */
static CleanupTrashStatus recycle(const wchar_t *path, CleanupTrashOutcome *out, int *untouched)
{
    TrashSink sink;
    IFileOperation *op = NULL;
    IShellItem *item = NULL;
    DWORD cookie = 0;
    int advised = 0;
    HRESULT hr;

    memset(&sink, 0, sizeof(sink));
    sink.iface.lpVtbl = &g_sink_vtbl;
    sink.refs = 1;

    hr = CoCreateInstance(&CLSID_FileOperation, NULL, CLSCTX_ALL, &IID_IFileOperation,
                          (void **)&op);
    if (SUCCEEDED(hr)) {
        /* 확인 창·오류 창 없이, 휴지통으로(FOF_ALLOWUNDO + FOFX_RECYCLEONDELETE). */
        hr = IFileOperation_SetOperationFlags(op, FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT |
                                                      FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE);
    }
    if (SUCCEEDED(hr)) {
        hr = IFileOperation_Advise(op, &sink.iface, &cookie);
        advised = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr)) {
        hr = SHCreateItemFromParsingName(path, NULL, &IID_IShellItem, (void **)&item);
    }
    if (SUCCEEDED(hr)) {
        hr = IFileOperation_DeleteItem(op, item, NULL);
    }
    if (SUCCEEDED(hr)) {
        hr = IFileOperation_PerformOperations(op);
    }
    if (advised) {
        IFileOperation_Unadvise(op, cookie);
    }
    if (item != NULL) {
        IShellItem_Release(item);
    }
    if (op != NULL) {
        IFileOperation_Release(op);
    }

    *untouched = 0;
    if (sink.vetoed) {
        free(sink.recycled_path);
        out->code = (uint32_t)E_ABORT;
        *untouched = 1;
        return CLEANUP_TRASH_NO_RECYCLE;
    }
    /* 원래 경로가 남아 있으면(일부만 옮겨진 폴더 포함) 실패로 본다. */
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        free(sink.recycled_path);
        if (sink.deleted && FAILED(sink.hr)) {
            hr = sink.hr;
        }
        out->code = (uint32_t)(FAILED(hr) ? hr : E_FAIL);
        *untouched = !sink.deleted;
        return CLEANUP_TRASH_FAILED;
    }
    out->recycled_path = sink.recycled_path;
    return CLEANUP_TRASH_MOVED;
}

/* ---- 항목 하나 ---- */

void cleanup_trash_outcome_free(CleanupTrashOutcome *o)
{
    if (o != NULL) {
        free(o->recycled_path);
        memset(o, 0, sizeof(*o));
    }
}

/* 같은 드라이브의 격리 폴더로 옮긴다. */
static CleanupTrashStatus quarantine_item(const CleanupItem *item, CleanupTrashOutcome *out)
{
    wchar_t *volume = quarantine_volume_root(item->path);
    QuarantineStatus qs;
    uint32_t code = 0;

    out->code = 0;
    if (volume == NULL) {
        out->code = ERROR_PATH_NOT_FOUND;
        return CLEANUP_TRASH_QUARANTINE_FAILED;
    }
    qs = quarantine_move(volume, item->path, (item->flags & CLEANUP_ITEM_DIR) != 0, item->bytes,
                         &out->recycled_path, &code);
    free(volume);
    if (qs == QUARANTINE_OK) {
        return CLEANUP_TRASH_QUARANTINED;
    }
    if (qs == QUARANTINE_MISSING) {
        return CLEANUP_TRASH_MISSING;
    }
    out->code = code;
    return CLEANUP_TRASH_QUARANTINE_FAILED;
}

CleanupTrashStatus cleanup_trash_one(const CleanupGuard *g, const wchar_t *root,
                                     const CleanupItem *item, CleanupTrashOutcome *out)
{
    CleanupTrashStatus status;
    int quarantine = 0, untouched = 0;

    cleanup_trash_outcome_free(out);
    status = cleanup_guard_check(g, root, item);
    if (status == CLEANUP_TRASH_PENDING) {
        status = pick_method(item, &quarantine);
    }
    if (status == CLEANUP_TRASH_PENDING) {
        status = verify_on_disk(item, &out->code);
    }
    if (status == CLEANUP_TRASH_PENDING && !quarantine) {
        status = recycle(item->path, out, &untouched);
        /* 휴지통을 쓸 수 없었고 항목이 그대로면, 다시 확인한 뒤 격리 폴더로 옮긴다. */
        if ((status == CLEANUP_TRASH_NO_RECYCLE || status == CLEANUP_TRASH_FAILED) && untouched) {
            uint32_t code = 0;
            quarantine = verify_on_disk(item, &code) == CLEANUP_TRASH_PENDING;
        }
    }
    if (quarantine && (status == CLEANUP_TRASH_PENDING || status == CLEANUP_TRASH_NO_RECYCLE ||
                       status == CLEANUP_TRASH_FAILED)) {
        status = quarantine_item(item, out);
    }
    out->status = status;
    return status;
}

/* ---- 작업 스레드 ---- */

struct CleanupTrashJob {
    HANDLE thread;
    wchar_t **roots;    /* 항목마다 탐지 경로(복사본) */
    CleanupItem *items; /* 경로는 복사본 */
    CleanupTrashOutcome *outcomes;
    size_t count;
    CleanupGuard guard;
    uint32_t interval_ms;
    cleanup_trash_notify_fn notify;
    void *ctx;

    volatile LONG cancel;
    ULONGLONG last_notify;

    CRITICAL_SECTION lock; /* snapshot 보호 */
    CleanupTrashSnapshot snapshot;
};

static int job_cancelled(CleanupTrashJob *job)
{
    return InterlockedCompareExchange(&job->cancel, 0, 0) != 0;
}

static unsigned __stdcall trash_thread(void *arg)
{
    CleanupTrashJob *job = arg;
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    size_t i;

    for (i = 0; i < job->count; i++) {
        CleanupTrashOutcome *o = &job->outcomes[i];
        ULONGLONG now;

        if (job_cancelled(job)) {
            o->status = CLEANUP_TRASH_CANCELLED;
        } else if (FAILED(init)) {
            o->status = CLEANUP_TRASH_FAILED;
            o->code = (uint32_t)init;
        } else {
            cleanup_trash_one(&job->guard, job->roots[i], &job->items[i], o);
        }

        EnterCriticalSection(&job->lock);
        job->snapshot.done++;
        if (o->status == CLEANUP_TRASH_MOVED || o->status == CLEANUP_TRASH_QUARANTINED) {
            job->snapshot.moved++;
            job->snapshot.moved_bytes += job->items[i].bytes;
            job->snapshot.quarantined += o->status == CLEANUP_TRASH_QUARANTINED;
        } else {
            job->snapshot.failed++;
        }
        LeaveCriticalSection(&job->lock);

        now = GetTickCount64();
        if (job->notify != NULL && i + 1 < job->count &&
            now - job->last_notify >= job->interval_ms) {
            job->last_notify = now;
            job->notify(0, job->ctx);
        }
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    if (job->notify != NULL) {
        job->notify(1, job->ctx);
    }
    return 0;
}

static void job_free(CleanupTrashJob *job)
{
    size_t i;

    DeleteCriticalSection(&job->lock);
    for (i = 0; i < job->count; i++) {
        if (job->items != NULL) {
            free(job->items[i].path);
        }
        if (job->roots != NULL) {
            free(job->roots[i]);
        }
    }
    free(job->items);
    free(job->roots);
    cleanup_trash_outcomes_free(job->outcomes, job->count);
    cleanup_guard_free(&job->guard);
    free(job);
}

CleanupTrashJob *cleanup_trash_job_start(const wchar_t *root, const CleanupItem *const *items,
                                         size_t count, uint32_t interval_ms,
                                         cleanup_trash_notify_fn notify, void *ctx)
{
    const wchar_t **roots = malloc((count > 0 ? count : 1) * sizeof(*roots));
    CleanupTrashJob *job;
    size_t i;

    if (roots == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        roots[i] = root;
    }
    job = cleanup_trash_job_start_each(roots, items, count, interval_ms, notify, ctx);
    free((void *)roots);
    return job;
}

CleanupTrashJob *cleanup_trash_job_start_each(const wchar_t *const *roots,
                                              const CleanupItem *const *items, size_t count,
                                              uint32_t interval_ms,
                                              cleanup_trash_notify_fn notify, void *ctx)
{
    CleanupTrashJob *job = calloc(1, sizeof(*job));
    size_t i;

    if (job == NULL) {
        return NULL;
    }
    InitializeCriticalSection(&job->lock);
    cleanup_guard_init(&job->guard);
    cleanup_guard_add_system(&job->guard);
    job->count = count;
    job->snapshot.total = count;
    job->interval_ms = interval_ms;
    job->notify = notify;
    job->ctx = ctx;
    job->last_notify = GetTickCount64();
    job->roots = calloc(count > 0 ? count : 1, sizeof(*job->roots));
    job->items = calloc(count > 0 ? count : 1, sizeof(*job->items));
    job->outcomes = calloc(count > 0 ? count : 1, sizeof(*job->outcomes));
    if (job->roots == NULL || job->items == NULL || job->outcomes == NULL) {
        job_free(job);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        job->items[i] = *items[i];
        job->items[i].path = path_dup(items[i]->path != NULL ? items[i]->path : L"");
        job->roots[i] = path_dup(roots[i] != NULL ? roots[i] : L"");
        if (job->items[i].path == NULL || job->roots[i] == NULL) {
            job_free(job);
            return NULL;
        }
    }

    job->thread = (HANDLE)_beginthreadex(NULL, 0, trash_thread, job, 0, NULL);
    if (job->thread == NULL) {
        job_free(job);
        return NULL;
    }
    return job;
}

void cleanup_trash_job_cancel(CleanupTrashJob *job)
{
    if (job != NULL) {
        InterlockedExchange(&job->cancel, 1);
    }
}

void cleanup_trash_job_progress(CleanupTrashJob *job, CleanupTrashSnapshot *out)
{
    EnterCriticalSection(&job->lock);
    *out = job->snapshot;
    LeaveCriticalSection(&job->lock);
    out->cancel_requested = job_cancelled(job);
}

size_t cleanup_trash_job_finish(CleanupTrashJob *job, CleanupTrashOutcome **outcomes)
{
    size_t count;

    if (outcomes != NULL) {
        *outcomes = NULL;
    }
    if (job == NULL) {
        return 0;
    }
    WaitForSingleObject(job->thread, INFINITE);
    CloseHandle(job->thread);

    count = job->count;
    if (outcomes != NULL) {
        *outcomes = job->outcomes;
        job->outcomes = NULL;
    }
    job_free(job);
    return count;
}

void cleanup_trash_outcomes_free(CleanupTrashOutcome *outcomes, size_t count)
{
    size_t i;

    if (outcomes == NULL) {
        return;
    }
    for (i = 0; i < count; i++) {
        cleanup_trash_outcome_free(&outcomes[i]);
    }
    free(outcomes);
}
