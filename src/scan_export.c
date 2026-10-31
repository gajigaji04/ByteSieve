#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "path.h"
#include "scan_export.h"
#include "size_format.h"

/* ---- 버퍼로 나눠 쓰기(임시 파일 → 바꿔치기) ---- */

#define WRITER_BUF ((size_t)64 * 1024)

typedef struct {
    HANDLE h;
    char *buf;
    size_t len;
    unsigned long error; /* 처음 실패한 Windows 오류(0 이면 아직 성공) */
    wchar_t *tmp_path;
} Writer;

static void w_fail(Writer *w, unsigned long error)
{
    if (w->error == 0) {
        w->error = error != 0 ? error : ERROR_WRITE_FAULT;
    }
}

static void w_flush(Writer *w)
{
    DWORD written = 0;

    if (w->error == 0 && w->len > 0 &&
        (!WriteFile(w->h, w->buf, (DWORD)w->len, &written, NULL) || written != w->len)) {
        w_fail(w, GetLastError());
    }
    w->len = 0;
}

static void w_bytes(Writer *w, const char *s, size_t n)
{
    while (w->error == 0 && n > 0) {
        size_t room = WRITER_BUF - w->len, take = n < room ? n : room;
        memcpy(w->buf + w->len, s, take);
        w->len += take;
        s += take;
        n -= take;
        if (w->len == WRITER_BUF) {
            w_flush(w);
        }
    }
}

static void w_str(Writer *w, const char *s)
{
    w_bytes(w, s, strlen(s));
}

static void w_fmt(Writer *w, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0) {
        w_bytes(w, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
    }
}

/* UTF-16 → UTF-8(malloc). 빈 문자열이면 "" 복사본. */
static char *utf8_of(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *out;

    if (n <= 0) {
        return NULL;
    }
    out = malloc((size_t)n);
    if (out != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    }
    return out;
}

typedef enum { TEXT_RAW, TEXT_HTML, TEXT_CSV } TextMode;

/* 문자열을 UTF-8 로 쓴다. HTML 은 &<>"' 를, CSV 는 " 를 이스케이프한다(모두 ASCII 라 바이트 단위로 안전). */
static void w_text(Writer *w, const wchar_t *s, TextMode mode)
{
    char *u, *p, *run;

    if (w->error != 0) {
        return;
    }
    u = utf8_of(s != NULL ? s : L"");
    if (u == NULL) {
        w_fail(w, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    if (mode == TEXT_RAW) {
        w_str(w, u);
        free(u);
        return;
    }
    for (p = run = u; *p != '\0'; p++) {
        const char *esc = NULL;
        if (mode == TEXT_CSV) {
            esc = *p == '"' ? "\"\"" : NULL;
        } else {
            switch (*p) {
            case '&': esc = "&amp;"; break;
            case '<': esc = "&lt;"; break;
            case '>': esc = "&gt;"; break;
            case '"': esc = "&quot;"; break;
            case '\'': esc = "&#39;"; break;
            default: break;
            }
        }
        if (esc != NULL) {
            w_bytes(w, run, (size_t)(p - run));
            w_str(w, esc);
            run = p + 1;
        }
    }
    w_bytes(w, run, (size_t)(p - run));
    free(u);
}

/* "<file>.tmp" 을 새로 만든다. 실패하면 -1(*error). */
static int writer_open(Writer *w, const wchar_t *file, unsigned long *error)
{
    size_t len = wcslen(file) + 5;

    memset(w, 0, sizeof(*w));
    w->h = INVALID_HANDLE_VALUE;
    w->buf = malloc(WRITER_BUF);
    w->tmp_path = malloc(len * sizeof(wchar_t));
    if (w->buf == NULL || w->tmp_path == NULL) {
        free(w->buf);
        free(w->tmp_path);
        *error = ERROR_NOT_ENOUGH_MEMORY;
        return -1;
    }
    swprintf(w->tmp_path, len, L"%ls.tmp", file);
    w->h = CreateFileW(w->tmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       NULL);
    if (w->h == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        free(w->buf);
        free(w->tmp_path);
        return -1;
    }
    return 0;
}

/* 남은 버퍼를 쓰고 닫은 뒤 성공했으면 file 로 바꿔치기, 실패했으면 임시 파일을 지운다. */
static int writer_close(Writer *w, const wchar_t *file, unsigned long *error)
{
    w_flush(w);
    CloseHandle(w->h);
    if (w->error == 0 &&
        !MoveFileExW(w->tmp_path, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        w_fail(w, GetLastError());
    }
    if (w->error != 0) {
        DeleteFileW(w->tmp_path);
    }
    *error = w->error;
    free(w->buf);
    free(w->tmp_path);
    return w->error == 0 ? 0 : -1;
}

/* ---- 공통 ---- */

const wchar_t *export_node_state(uint32_t flags)
{
    if (flags & SCAN_NODE_FAILED) {
        return L"읽기 실패";
    }
    if (flags & SCAN_NODE_CANCELLED) {
        return L"취소됨";
    }
    if (flags & SCAN_NODE_INCOMPLETE) {
        return L"일부 누락";
    }
    if (flags & SCAN_NODE_REPARSE) {
        return L"링크";
    }
    if (flags & SCAN_NODE_DEPTH_LIMIT) {
        return L"깊이 제한";
    }
    return L"정상";
}

static int check_args(const ScanModel *model, const wchar_t *file, unsigned long *error)
{
    if (model == NULL || model->len == 0 || model->status == SCAN_FATAL || file == NULL ||
        file[0] == L'\0') {
        *error = ERROR_INVALID_PARAMETER;
        return -1;
    }
    return 0;
}

static double share_of(uint64_t part, uint64_t whole)
{
    return whole > 0 ? 100.0 * (double)part / (double)whole : 0.0;
}

/* 루트의 표시 이름: 전체 경로의 마지막 구성 요소("C:\Work\Proj" → "Proj", "D:\" → "D:\"). */
static const wchar_t *root_label(const wchar_t *root, wchar_t *buf, size_t len)
{
    size_t n = wcslen(root), start;

    while (n > 0 && path_is_separator(root[n - 1]) && !(n >= 2 && root[n - 2] == L':')) {
        n--;
    }
    start = n;
    while (start > 0 && !path_is_separator(root[start - 1])) {
        start--;
    }
    if (start == n) {
        return root; /* 드라이브 루트("D:\") */
    }
    if (n - start >= len) {
        n = start + len - 1;
    }
    wmemcpy(buf, root + start, n - start);
    buf[n - start] = L'\0';
    return buf;
}

/* 루트 다음에 나머지 폴더를 크기 내림차순으로 담은 배열(malloc). *count 는 전체 행 수. */
static size_t *size_order(const ScanModel *model, size_t *count)
{
    size_t rest = model->len - 1;
    size_t *order = malloc(model->len * sizeof(*order));

    if (order == NULL) {
        return NULL;
    }
    order[0] = 0;
    if (rest > 0 && scan_model_rank(model, SCAN_RANK_TOTAL_BYTES, order + 1, rest) != rest) {
        free(order);
        return NULL;
    }
    *count = model->len;
    return order;
}

/* ---- CSV ---- */

/* 따옴표로 감싼 문자열 칸. 수식으로 해석될 수 있는 첫 글자는 ' 로 막는다. */
static void csv_field(Writer *w, const wchar_t *s)
{
    w_str(w, "\"");
    if (s != NULL && (s[0] == L'=' || s[0] == L'+' || s[0] == L'-' || s[0] == L'@' ||
                      s[0] == L'\t' || s[0] == L'\r')) {
        w_str(w, "'");
    }
    w_text(w, s, TEXT_CSV);
    w_str(w, "\"");
}

int export_csv(const ScanModel *model, const wchar_t *file, unsigned long *error)
{
    unsigned long dummy;
    Writer w;
    size_t *order, count = 0, i;
    uint64_t whole;
    wchar_t size[32], label[MAX_PATH];

    if (error == NULL) {
        error = &dummy;
    }
    *error = 0;
    if (check_args(model, file, error) != 0) {
        return -1;
    }
    order = size_order(model, &count);
    if (order == NULL) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        return -1;
    }
    if (writer_open(&w, file, error) != 0) {
        free(order);
        return -1;
    }
    whole = model->nodes[0].total_bytes;
    /* UTF-8 BOM: Excel 이 한글을 UTF-8 로 읽게 한다. */
    w_str(&w, "\xEF\xBB\xBF");
    w_text(&w,
           L"경로,폴더명,깊이,크기(바이트),크기,디스크 사용(바이트),파일 수(하위 포함),하위 폴더 수,"
           L"바로 아래 파일 수,바로 아래 파일 크기(바이트),비율(%),상태\r\n",
           TEXT_RAW);
    for (i = 0; i < count && w.error == 0; i++) {
        const ScanNode *n = &model->nodes[order[i]];
        wchar_t *path = scan_model_path(model, order[i]);
        if (path == NULL) {
            w_fail(&w, ERROR_NOT_ENOUGH_MEMORY);
            break;
        }
        csv_field(&w, path);
        w_str(&w, ",");
        csv_field(&w, order[i] == 0 ? root_label(n->name, label, MAX_PATH) : n->name);
        w_fmt(&w, ",%u,%llu,", n->depth, (unsigned long long)n->total_bytes);
        format_size(n->total_bytes, size, 32);
        csv_field(&w, size);
        w_fmt(&w, ",%llu,%llu,%llu,%llu,%llu,%.1f,", (unsigned long long)n->total_alloc,
              (unsigned long long)n->total_files, (unsigned long long)n->total_dirs,
              (unsigned long long)n->own_files, (unsigned long long)n->own_bytes,
              share_of(n->total_bytes, whole));
        csv_field(&w, export_node_state(n->flags));
        w_str(&w, "\r\n");
        free(path);
    }
    free(order);
    return writer_close(&w, file, error);
}

/* ---- HTML ---- */

/* 세 자리마다 쉼표("12,345"). */
static const char *fmt_count(uint64_t v, char *buf, size_t len)
{
    char digits[32];
    int n = snprintf(digits, sizeof(digits), "%llu", (unsigned long long)v), i, o = 0;

    for (i = 0; i < n && (size_t)o + 2 < len; i++) {
        if (i > 0 && (n - i) % 3 == 0) {
            buf[o++] = ',';
        }
        buf[o++] = digits[i];
    }
    buf[o] = '\0';
    return buf;
}

static void w_size(Writer *w, uint64_t bytes)
{
    wchar_t size[32];

    format_size(bytes, size, 32);
    w_text(w, size, TEXT_RAW);
}

/* 크기 칸: 보이는 값은 짧게, 마우스를 올리면 정확한 바이트 수. */
static void w_size_cell(Writer *w, uint64_t bytes)
{
    char exact[40];

    w_fmt(w, "<td class=\"n\" title=\"%s B\">", fmt_count(bytes, exact, sizeof(exact)));
    w_size(w, bytes);
    w_str(w, "</td>");
}

static void w_count_cell(Writer *w, uint64_t v)
{
    char buf[40];

    w_fmt(w, "<td class=\"n\">%s</td>", fmt_count(v, buf, sizeof(buf)));
}

static void w_bar_cell(Writer *w, double share)
{
    w_fmt(w, "<td><div class=\"bar\"><i style=\"width:%.1f%%\"></i></div></td>"
             "<td class=\"n\">%.1f%%</td>",
          share > 100.0 ? 100.0 : share, share);
}

/* 이름-값 표 한 줄(값은 이미 만든 UTF-8/HTML). */
static void w_kv(Writer *w, const wchar_t *key, const char *value_html)
{
    w_str(w, "<tr><th scope=\"row\">");
    w_text(w, key, TEXT_HTML);
    w_str(w, "</th><td>");
    w_str(w, value_html);
    w_str(w, "</td></tr>\n");
}

static void w_kv_size(Writer *w, const wchar_t *key, uint64_t bytes)
{
    char exact[40];
    wchar_t size[32];
    char *u;

    format_size(bytes, size, 32);
    u = utf8_of(size);
    w_str(w, "<tr><th scope=\"row\">");
    w_text(w, key, TEXT_HTML);
    w_fmt(w, "</th><td>%s <span class=\"muted\">(%s B)</span></td></tr>\n", u != NULL ? u : "",
          fmt_count(bytes, exact, sizeof(exact)));
    free(u);
}

static void w_kv_count(Writer *w, const wchar_t *key, uint64_t v)
{
    char buf[40];

    w_kv(w, key, fmt_count(v, buf, sizeof(buf)));
}

static void w_kv_text(Writer *w, const wchar_t *key, const wchar_t *value)
{
    w_str(w, "<tr><th scope=\"row\">");
    w_text(w, key, TEXT_HTML);
    w_str(w, "</th><td>");
    w_text(w, value, TEXT_HTML);
    w_str(w, "</td></tr>\n");
}

static void w_card(Writer *w, const wchar_t *label, const char *value_utf8)
{
    w_fmt(w, "<div class=\"card\"><b>%s</b><span>", value_utf8);
    w_text(w, label, TEXT_HTML);
    w_str(w, "</span></div>\n");
}

static void w_card_size(Writer *w, const wchar_t *label, uint64_t bytes)
{
    wchar_t size[32];
    char *u;

    format_size(bytes, size, 32);
    u = utf8_of(size);
    w_card(w, label, u != NULL ? u : "");
    free(u);
}

/* FILETIME(UTC) → 지역 시각 "YYYY-MM-DD HH:MM:SS". */
static void local_time_text(uint64_t ft, wchar_t *buf, size_t len)
{
    FILETIME utc, local;
    SYSTEMTIME st;

    utc.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFu);
    utc.dwHighDateTime = (DWORD)(ft >> 32);
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) {
        swprintf(buf, len, L"-");
        return;
    }
    swprintf(buf, len, L"%04u-%02u-%02u %02u:%02u:%02u", (unsigned)st.wYear, (unsigned)st.wMonth,
             (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
}

static const wchar_t *scan_state_text(const ScanModel *model)
{
    if (model->report.cancelled) {
        return L"취소됨(그때까지 집계)";
    }
    return model->status == SCAN_COMPLETE ? L"완료" : L"완료(일부 폴더를 읽지 못함)";
}

static const char HTML_HEAD[] =
    "<!DOCTYPE html>\n<html lang=\"ko\">\n<head>\n<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<meta name=\"generator\" content=\"ByteSieve\">\n";

static const char HTML_STYLE[] =
    "<style>\n"
    ":root{--bg:#fff;--fg:#1f2328;--muted:#656d76;--line:#d0d7de;--head:#f6f8fa;"
    "--bar:#2f81f7;--warn:#9a6700}\n"
    "@media (prefers-color-scheme: dark){:root{--bg:#0d1117;--fg:#e6edf3;--muted:#8d96a0;"
    "--line:#30363d;--head:#161b22;--bar:#4493f8;--warn:#d29922}}\n"
    "body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);"
    "font:14px/1.5 \"Segoe UI\",\"Malgun Gothic\",sans-serif}\n"
    "main{max-width:1100px;margin:0 auto}\n"
    "h1{font-size:22px;margin:0 0 4px}h2{font-size:17px;margin:32px 0 8px}\n"
    ".muted,.sub{color:var(--muted)}.sub{margin:0 0 16px;word-break:break-all}\n"
    ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px}\n"
    ".card{border:1px solid var(--line);border-radius:6px;padding:10px 12px}\n"
    ".card b{display:block;font-size:20px;font-variant-numeric:tabular-nums}"
    ".card span{color:var(--muted);font-size:12px}\n"
    ".wrap{overflow-x:auto}\n"
    "table{border-collapse:collapse;width:100%}\n"
    "th,td{border-bottom:1px solid var(--line);padding:6px 8px;text-align:left;vertical-align:top}\n"
    "thead th{background:var(--head);white-space:nowrap}\n"
    "table.kv th{width:16em;font-weight:600}\n"
    ".n{text-align:right;white-space:nowrap;font-variant-numeric:tabular-nums}\n"
    "td.p{word-break:break-all;min-width:16em}\n"
    ".bar{background:var(--head);height:8px;border-radius:4px;min-width:80px;margin-top:6px}\n"
    ".bar i{display:block;height:100%;background:var(--bar);border-radius:4px}\n"
    ".warn{color:var(--warn)}\n"
    "footer{color:var(--muted);margin-top:32px;font-size:12px}\n"
    "</style>\n</head>\n<body>\n<main>\n";

/* 순위 표 머리글. */
static void w_rank_head(Writer *w, const wchar_t *first)
{
    w_str(w, "<div class=\"wrap\"><table>\n<thead><tr><th class=\"n\">순위</th><th>");
    w_text(w, first, TEXT_HTML);
    w_str(w, "</th><th class=\"n\">크기</th><th>비율</th><th class=\"n\"></th>"
             "<th class=\"n\">디스크 사용</th><th class=\"n\">파일 수</th>"
             "<th class=\"n\">하위 폴더</th><th>상태</th></tr></thead>\n<tbody>\n");
}

static void w_rank_row(Writer *w, size_t rank, const wchar_t *label, const ScanNode *n,
                       uint64_t whole)
{
    w_fmt(w, "<tr><td class=\"n\">%llu</td><td class=\"p\">", (unsigned long long)rank);
    w_text(w, label, TEXT_HTML);
    w_str(w, "</td>");
    w_size_cell(w, n->total_bytes);
    w_bar_cell(w, share_of(n->total_bytes, whole));
    w_size_cell(w, n->total_alloc);
    w_count_cell(w, n->total_files);
    w_count_cell(w, n->total_dirs);
    w_str(w, (n->flags & (SCAN_NODE_FAILED | SCAN_NODE_INCOMPLETE | SCAN_NODE_CANCELLED))
                 ? "<td class=\"warn\">"
                 : "<td>");
    w_text(w, export_node_state(n->flags), TEXT_HTML);
    w_str(w, "</td></tr>\n");
}

/* 최상위 폴더 구성: 대상 폴더의 자식(크기순)과 바로 아래 파일. */
static void w_top_level(Writer *w, const ScanModel *model)
{
    const ScanNode *root = &model->nodes[0];
    size_t count = scan_model_children(model, 0, SCAN_ORDER_SIZE_DESC, NULL, 0), shown, i;
    size_t *kids = count > 0 ? malloc(count * sizeof(*kids)) : NULL;
    uint64_t rest_bytes = 0, rest_files = 0;
    char a[40];

    if (count > 0 && kids == NULL) {
        w_fail(w, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    if (count > 0) {
        count = scan_model_children(model, 0, SCAN_ORDER_SIZE_DESC, kids, count);
    }
    shown = count < EXPORT_RANK_ROWS ? count : EXPORT_RANK_ROWS;
    w_str(w, "<h2 id=\"top\">최상위 폴더 구성</h2>\n");
    if (count == 0 && root->own_files == 0) {
        w_str(w, "<p class=\"muted\">하위 폴더와 파일이 없습니다.</p>\n");
        free(kids);
        return;
    }
    w_rank_head(w, L"폴더");
    for (i = 0; i < shown; i++) {
        w_rank_row(w, i + 1, model->nodes[kids[i]].name, &model->nodes[kids[i]],
                   root->total_bytes);
    }
    for (i = shown; i < count; i++) {
        rest_bytes += model->nodes[kids[i]].total_bytes;
        rest_files += model->nodes[kids[i]].total_files;
    }
    if (count > shown) {
        w_fmt(w, "<tr><td></td><td class=\"muted\">그 밖의 폴더 %s개</td>",
              fmt_count(count - shown, a, sizeof(a)));
        w_size_cell(w, rest_bytes);
        w_bar_cell(w, share_of(rest_bytes, root->total_bytes));
        w_str(w, "<td></td>");
        w_count_cell(w, rest_files);
        w_str(w, "<td></td><td></td></tr>\n");
    }
    if (root->own_files > 0) {
        w_str(w, "<tr><td></td><td class=\"muted\">(대상 폴더 바로 아래 파일)</td>");
        w_size_cell(w, root->own_bytes);
        w_bar_cell(w, share_of(root->own_bytes, root->total_bytes));
        w_size_cell(w, root->own_alloc);
        w_count_cell(w, root->own_files);
        w_str(w, "<td></td><td></td></tr>\n");
    }
    w_str(w, "</tbody></table></div>\n");
    free(kids);
}

int export_html(const ScanModel *model, const ExportInfo *info, const wchar_t *file,
                unsigned long *error)
{
    unsigned long dummy;
    const ScanReport *r;
    const ScanNode *root;
    Writer w;
    size_t top[EXPORT_RANK_ROWS], count, i, own_top;
    uint64_t empty = 0, problem = 0, max_depth = 0;
    wchar_t text[64];
    char a[40], b[40];
    SYSTEMTIME now;

    if (error == NULL) {
        error = &dummy;
    }
    *error = 0;
    if (check_args(model, file, error) != 0) {
        return -1;
    }
    if (writer_open(&w, file, error) != 0) {
        return -1;
    }
    r = &model->report;
    root = &model->nodes[0];
    for (i = 0; i < model->len; i++) {
        const ScanNode *n = &model->nodes[i];
        if (n->depth > max_depth) {
            max_depth = n->depth;
        }
        if (n->flags & (SCAN_NODE_FAILED | SCAN_NODE_INCOMPLETE | SCAN_NODE_CANCELLED)) {
            problem++;
        } else if (i > 0 && n->total_files == 0 && n->child_count == 0 &&
                   !(n->flags & (SCAN_NODE_REPARSE | SCAN_NODE_DEPTH_LIMIT))) {
            empty++;
        }
    }

    w_str(&w, HTML_HEAD);
    w_str(&w, "<title>ByteSieve 스캔 보고서 - ");
    w_text(&w, root->name, TEXT_HTML);
    w_str(&w, "</title>\n");
    w_str(&w, HTML_STYLE);

    /* 제목 */
    GetLocalTime(&now);
    w_str(&w, "<h1>ByteSieve 스캔 보고서</h1>\n<p class=\"sub\">");
    w_text(&w, root->name, TEXT_HTML);
    w_str(&w, " · ");
    w_text(&w, scan_state_text(model), TEXT_HTML);
    w_fmt(&w, " · 보고서 생성 %04u-%02u-%02u %02u:%02u</p>\n", (unsigned)now.wYear,
          (unsigned)now.wMonth, (unsigned)now.wDay, (unsigned)now.wHour, (unsigned)now.wMinute);

    /* 전체 요약 */
    w_str(&w, "<h2 id=\"summary\">전체 요약</h2>\n<div class=\"cards\">\n");
    w_card_size(&w, L"총 용량(논리 크기)", r->total_bytes);
    w_card_size(&w, L"디스크 사용", r->alloc_bytes);
    w_card(&w, L"파일", fmt_count(r->file_count, a, sizeof(a)));
    w_card(&w, L"폴더", fmt_count(r->dir_count, a, sizeof(a)));
    w_card(&w, L"오류", fmt_count(r->error_count, a, sizeof(a)));
    w_str(&w, "</div>\n<div class=\"wrap\"><table class=\"kv\">\n");
    w_kv_text(&w, L"대상 폴더", root->name);
    w_kv_text(&w, L"스캔 상태", scan_state_text(model));
    if (info != NULL && info->finished_time != 0) {
        local_time_text(info->finished_time, text, 64);
        w_kv_text(&w, L"스캔 완료 시각", text);
    }
    if (info != NULL && info->elapsed_ms != 0) {
        swprintf(text, 64, L"%.1f초", (double)info->elapsed_ms / 1000.0);
        w_kv_text(&w, L"걸린 시간", text);
    }
    w_kv_count(&w, L"건너뛴 폴더(읽지 못함·취소)", r->skipped_dir_count);
    w_kv_count(&w, L"들어가지 않은 링크(정션 등)", r->reparse_count);
    if (model->out_of_memory) {
        w_kv(&w, L"주의",
             "<span class=\"warn\">메모리가 부족해 폴더 트리 일부가 빠졌습니다(합계는 정확).</span>");
    }
    w_str(&w, "</table></div>\n");

    /* 파일·폴더 통계 */
    w_str(&w, "<h2 id=\"stats\">파일·폴더 통계</h2>\n<div class=\"wrap\"><table class=\"kv\">\n");
    w_kv_count(&w, L"파일 수", r->file_count);
    w_kv_count(&w, L"폴더 수(대상 폴더 제외)", r->dir_count);
    w_kv_size(&w, L"평균 파일 크기", r->file_count > 0 ? r->total_bytes / r->file_count : 0);
    swprintf(text, 64, L"%.1f", (double)r->file_count / (double)(r->dir_count + 1));
    w_kv_text(&w, L"폴더당 평균 파일 수", text);
    w_kv_count(&w, L"가장 깊은 폴더 깊이", max_depth);
    w_kv_count(&w, L"빈 폴더", empty);
    w_kv_count(&w, L"다 읽지 못한 폴더", problem);
    /* 순위는 루트를 빼므로 루트 바로 아래 파일과도 비교한다. */
    if (scan_model_rank(model, SCAN_RANK_OWN_BYTES, &own_top, 1) != 1 ||
        root->own_bytes > model->nodes[own_top].own_bytes) {
        own_top = 0;
    }
    if (model->nodes[own_top].own_bytes > 0) {
        wchar_t *path = scan_model_path(model, own_top);
        wchar_t size[32];
        if (path != NULL) {
            size_t len = wcslen(path) + 96;
            wchar_t *line = malloc(len * sizeof(*line));
            format_size(model->nodes[own_top].own_bytes, size, 32);
            if (line != NULL) {
                swprintf(line, len, L"%ls (%ls, 파일 %llu개)", path, size,
                         (unsigned long long)model->nodes[own_top].own_files);
                w_kv_text(&w, L"파일 용량이 가장 많이 모인 폴더", line);
                free(line);
            }
            free(path);
        }
    }
    w_str(&w, "</table></div>\n");

    /* 디스크 사용 분석 */
    w_str(&w, "<h2 id=\"disk\">디스크 사용 분석</h2>\n<div class=\"wrap\"><table class=\"kv\">\n");
    w_kv_size(&w, L"논리 크기", r->total_bytes);
    w_kv_size(&w, L"디스크 사용", r->alloc_bytes);
    w_kv_size(&w, L"클러스터 낭비(+)", r->slack_bytes);
    w_kv_size(&w, L"NTFS 압축 절약(-)", r->compressed_saved_bytes);
    w_kv_size(&w, L"스파스 절약(-)", r->sparse_saved_bytes);
    w_kv_size(&w, L"클라우드 전용(-)", r->cloud_saved_bytes);
    w_kv_size(&w, L"기타 절약(-)", r->other_saved_bytes);
    if (r->cluster_bytes > 0) {
        w_kv_size(&w, L"클러스터 크기", r->cluster_bytes);
    }
    w_str(&w, "</table></div>\n");

    w_top_level(&w, model);

    /* 폴더별 용량 순위 */
    count = scan_model_rank(model, SCAN_RANK_TOTAL_BYTES, top, EXPORT_RANK_ROWS);
    w_fmt(&w, "<h2 id=\"rank\">폴더별 용량 순위(상위 %s개)</h2>\n",
          fmt_count(count, a, sizeof(a)));
    if (count == 0) {
        w_str(&w, "<p class=\"muted\">하위 폴더가 없습니다.</p>\n");
    } else {
        w_rank_head(&w, L"폴더 경로");
        for (i = 0; i < count && w.error == 0; i++) {
            wchar_t *path = scan_model_path(model, top[i]);
            if (path == NULL) {
                w_fail(&w, ERROR_NOT_ENOUGH_MEMORY);
                break;
            }
            w_rank_row(&w, i + 1, path, &model->nodes[top[i]], root->total_bytes);
            free(path);
        }
        w_str(&w, "</tbody></table></div>\n");
    }

    /* 오류 */
    if (r->error_count > 0) {
        size_t shown = r->errors_len < EXPORT_ERROR_ROWS ? r->errors_len : EXPORT_ERROR_ROWS;
        w_fmt(&w, "<h2 id=\"errors\">오류(%s개", fmt_count(r->error_count, a, sizeof(a)));
        if (r->error_count > shown) {
            w_fmt(&w, ", 처음 %s개만 표시", fmt_count(shown, b, sizeof(b)));
        }
        w_str(&w, ")</h2>\n<div class=\"wrap\"><table>\n<thead><tr><th>종류</th>"
                  "<th class=\"n\">코드</th><th>경로</th></tr></thead>\n<tbody>\n");
        for (i = 0; i < shown; i++) {
            const ScanError *e = &r->errors[i];
            w_str(&w, "<tr><td>");
            w_text(&w, scan_error_kind_name(e->kind), TEXT_HTML);
            w_fmt(&w, "</td><td class=\"n\">%lu</td><td class=\"p\">", (unsigned long)e->code);
            w_text(&w, e->path != NULL ? e->path : L"", TEXT_HTML);
            w_str(&w, "</td></tr>\n");
        }
        w_str(&w, "</tbody></table></div>\n");
    }

    w_str(&w, "<footer>ByteSieve로 만든 보고서입니다. 크기는 1024 단위(KiB·MiB·GiB)이며, "
              "크기 칸에 마우스를 올리면 정확한 바이트 수가 보입니다. 비율은 대상 폴더 전체 "
              "용량 대비입니다.</footer>\n</main>\n</body>\n</html>\n");
    return writer_close(&w, file, error);
}
