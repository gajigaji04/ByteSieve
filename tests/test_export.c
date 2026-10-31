#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "path.h"
#include "scan_export.h"
#include "scan_model.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

/* 픽스처 안에 만드는 큰 폴더 수(CSV 가 쓰기 버퍼 64 KiB 를 넘게). */
#define MANY_DIRS 400

static wchar_t g_root[MAX_PATH];
static wchar_t g_out[MAX_PATH];

static wchar_t *fixture(const wchar_t *name)
{
    return path_join(g_root, name);
}

static int make_dir(const wchar_t *name)
{
    wchar_t *p = fixture(name);
    BOOL ok = p != NULL && (CreateDirectoryW(p, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);

    free(p);
    return ok;
}

static int write_bytes(const wchar_t *path, const char *data, DWORD len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           NULL);
    DWORD written = 0;
    BOOL ok;

    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = len == 0 || WriteFile(h, data, len, &written, NULL);
    CloseHandle(h);
    return ok && written == len;
}

static int write_file(const wchar_t *name, DWORD len)
{
    static char data[4096];
    wchar_t *p = fixture(name);
    int ok = p != NULL && write_bytes(p, data, len);

    free(p);
    return ok;
}

/* 파일 전체(malloc, NUL 로 끝남). 없으면 NULL. */
static char *read_all(const wchar_t *path, size_t *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    DWORD got = 0;
    char *buf;

    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    if (!GetFileSizeEx(h, &size) || (buf = malloc((size_t)size.QuadPart + 1)) == NULL) {
        CloseHandle(h);
        return NULL;
    }
    ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(h);
    buf[got] = '\0';
    if (len != NULL) {
        *len = got;
    }
    return buf;
}

static wchar_t *out_file(const wchar_t *name)
{
    return path_join(g_out, name);
}

static int exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void remove_tree(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = path_join(dir, L"*");
    HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;

    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            child = path_join(dir, fd.cFileName);
            if (child == NULL) {
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                remove_tree(child);
            } else {
                DeleteFileW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* UTF-16 → UTF-8(malloc). */
static char *u8(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *out = n > 0 ? malloc((size_t)n) : NULL;

    if (out != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    }
    return out;
}

static size_t count_str(const char *hay, const char *needle)
{
    size_t n = 0, len = strlen(needle);

    for (hay = strstr(hay, needle); hay != NULL; hay = strstr(hay + len, needle)) {
        n++;
    }
    return n;
}

/* 위치(없으면 SIZE_MAX). */
static size_t pos_of(const char *hay, const char *needle)
{
    const char *p = strstr(hay, needle);

    return p != NULL ? (size_t)(p - hay) : (size_t)-1;
}

static void test_csv(const ScanModel *m)
{
    wchar_t *file = out_file(L"result.csv"), *tmp = out_file(L"result.csv.tmp");
    unsigned long error = 1;
    size_t len = 0, lines;
    char *csv, *root8, *row;

    CHECK(export_csv(m, file, &error) == 0 && error == 0);
    CHECK(!exists(tmp));
    csv = read_all(file, &len);
    CHECK(csv != NULL);
    if (csv == NULL) {
        free(file);
        free(tmp);
        return;
    }
    /* Excel 용 UTF-8 BOM, 한국어 머리글, CRLF 만 사용 */
    CHECK(len > 3 && memcmp(csv, "\xEF\xBB\xBF", 3) == 0);
    CHECK(pos_of(csv, "경로,폴더명,깊이,크기(바이트),크기,디스크 사용(바이트),파일 수(하위 포함),"
                      "하위 폴더 수,바로 아래 파일 수,바로 아래 파일 크기(바이트),비율(%),상태\r\n") == 3);
    lines = count_str(csv, "\r\n");
    CHECK(lines == m->len + 1);
    CHECK(count_str(csv, "\n") == lines);
    CHECK(len > 64 * 1024); /* 쓰기 버퍼 경계를 넘었다 */

    /* 첫 데이터 행은 대상 폴더(폴더명은 마지막 구성 요소, 비율 100) */
    root8 = u8(g_root);
    row = strstr(csv, "\r\n");
    CHECK(row != NULL && root8 != NULL && row[2] == '"' &&
          strncmp(row + 3, root8, strlen(root8)) == 0 && row[3 + strlen(root8)] == '"');
    CHECK(pos_of(csv, ",100.0,\"정상\"\r\n") < pos_of(csv, ",\"big\","));

    /* 크기 내림차순: big(3000) > sub(1000) > R&D 한글(500) > a,b(20) > =cmd(10) > empty(0) */
    CHECK(pos_of(csv, ",\"big\",1,3000,\"2.93 KiB\",") != (size_t)-1);
    CHECK(pos_of(csv, ",\"big\",") < pos_of(csv, ",\"sub\",2,1000,"));
    CHECK(pos_of(csv, ",\"sub\",") < pos_of(csv, ",\"R&D 한글\",1,500,"));
    CHECK(pos_of(csv, ",\"R&D 한글\",") < pos_of(csv, ",\"a,b\",1,20,"));
    /* 수식으로 시작하는 이름은 ' 로 막고, 쉼표가 든 이름은 따옴표로 감싼다 */
    CHECK(pos_of(csv, ",\"a,b\",") < pos_of(csv, ",\"'=cmd\",1,10,"));
    CHECK(pos_of(csv, ",\"=cmd\",") == (size_t)-1);
    CHECK(pos_of(csv, ",\"'=cmd\",") < pos_of(csv, ",\"empty\",1,0,"));
    /* 하위 포함 파일 수·하위 폴더 수·바로 아래 파일: big 은 파일 2개, 하위 폴더 1개, 바로 아래 1개(2000) */
    CHECK(pos_of(csv, ",2,1,1,2000,") != (size_t)-1);

    free(root8);
    free(csv);
    free(file);
    free(tmp);
}

static void test_html(const ScanModel *m)
{
    wchar_t *file = out_file(L"report.html");
    ExportInfo info;
    unsigned long error = 1;
    char *html;

    info.finished_time = 0;
    info.elapsed_ms = 1500;
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        info.finished_time = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    }
    CHECK(export_html(m, &info, file, &error) == 0 && error == 0);
    html = read_all(file, NULL);
    CHECK(html != NULL);
    if (html == NULL) {
        free(file);
        return;
    }
    CHECK(strncmp(html, "<!DOCTYPE html>\n<html lang=\"ko\">", 31) == 0);
    CHECK(strstr(html, "<meta charset=\"utf-8\">") != NULL);
    CHECK(strstr(html, "<script") == NULL);
    CHECK(strlen(html) > 20 && strcmp(html + strlen(html) - 8, "</html>\n") == 0);
    /* 구역 */
    CHECK(strstr(html, "<h2 id=\"summary\">전체 요약</h2>") != NULL);
    CHECK(strstr(html, "<h2 id=\"stats\">파일·폴더 통계</h2>") != NULL);
    CHECK(strstr(html, "<h2 id=\"disk\">디스크 사용 분석</h2>") != NULL);
    CHECK(strstr(html, "<h2 id=\"top\">최상위 폴더 구성</h2>") != NULL);
    CHECK(strstr(html, "<h2 id=\"rank\">폴더별 용량 순위(상위 100개)</h2>") != NULL);
    CHECK(strstr(html, "id=\"errors\"") == NULL);
    CHECK(strstr(html, "스캔 완료 시각") != NULL && strstr(html, "<td>1.5초</td>") != NULL);
    /* 이름은 이스케이프한다 */
    CHECK(strstr(html, "R&amp;D 한글") != NULL && strstr(html, "R&D") == NULL);
    /* 순위 1위는 big(정확한 바이트 수는 title 로) */
    CHECK(pos_of(html, "<tr><td class=\"n\">1</td><td class=\"p\">") <
          pos_of(html, "\\big</td><td class=\"n\" title=\"3,000 B\">2.93 KiB</td>"));
    CHECK(strstr(html, "\\big</td><td class=\"n\" title=\"3,000 B\">") != NULL);
    /* 순위 표 행 수는 EXPORT_RANK_ROWS 로 제한 */
    CHECK(count_str(html, "<td class=\"n\">100</td><td class=\"p\">") == 1);
    CHECK(count_str(html, "<td class=\"n\">101</td><td class=\"p\">") == 0);
    /* 통계: 빈 폴더(empty + many 의 빈 폴더들), 최상위 폴더 구성의 나머지 줄과 바로 아래 파일 */
    CHECK(strstr(html, "<th scope=\"row\">빈 폴더</th><td>401</td>") != NULL);
    CHECK(strstr(html, "(대상 폴더 바로 아래 파일)") != NULL);
    free(html);

    /* info 없이도 쓴다 */
    CHECK(export_html(m, NULL, file, NULL) == 0);
    html = read_all(file, NULL);
    CHECK(html != NULL && strstr(html, "스캔 완료 시각") == NULL);
    free(html);
    free(file);
}

static void test_failures(const ScanModel *m)
{
    wchar_t *missing = out_file(L"no-such-dir\\x.csv");
    wchar_t *target = out_file(L"locked.csv"), *tmp = out_file(L"locked.csv.tmp");
    unsigned long error = 0;
    ScanModel empty, fatal;
    HANDLE h;
    char *text;

    /* 없는 폴더: 실패, 아무것도 만들지 않음 */
    CHECK(export_csv(m, missing, &error) == -1 && error == ERROR_PATH_NOT_FOUND);
    CHECK(export_html(m, NULL, missing, &error) == -1 && error == ERROR_PATH_NOT_FOUND);
    CHECK(!exists(missing));

    /* 빈 모델·대상 폴더를 열지 못한 스캔은 내보내지 않는다 */
    scan_model_init(&empty);
    CHECK(export_csv(&empty, target, &error) == -1 && error == ERROR_INVALID_PARAMETER);
    scan_model_init(&fatal);
    scan_model_build(&fatal, L"C:\\no-such-dir-bytesieve-export", SCAN_DEPTH_UNLIMITED, NULL,
                     NULL);
    CHECK(fatal.status == SCAN_FATAL);
    CHECK(export_html(&fatal, NULL, target, &error) == -1 && error == ERROR_INVALID_PARAMETER);
    CHECK(export_csv(NULL, target, &error) == -1 && export_csv(m, L"", &error) == -1);
    CHECK(!exists(target));
    scan_model_free(&fatal);

    /* 다른 프로그램이 연 파일(Excel 처럼 삭제 공유 없음): 실패하고 기존 내용·임시 파일 없음 */
    CHECK(write_bytes(target, "old", 3));
    h = CreateFileW(target, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    error = 0;
    CHECK(export_csv(m, target, &error) == -1);
    CHECK(error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED);
    CloseHandle(h);
    CHECK(!exists(tmp));
    text = read_all(target, NULL);
    CHECK(text != NULL && strcmp(text, "old") == 0);
    free(text);

    /* 닫힌 뒤에는 덮어쓴다 */
    CHECK(export_csv(m, target, NULL) == 0);
    text = read_all(target, NULL);
    CHECK(text != NULL && memcmp(text, "\xEF\xBB\xBF", 3) == 0);
    free(text);

    CHECK(wcscmp(export_node_state(0), L"정상") == 0);
    CHECK(wcscmp(export_node_state(SCAN_NODE_FAILED | SCAN_NODE_REPARSE), L"읽기 실패") == 0);
    CHECK(wcscmp(export_node_state(SCAN_NODE_INCOMPLETE), L"일부 누락") == 0);
    CHECK(wcscmp(export_node_state(SCAN_NODE_REPARSE), L"링크") == 0);

    free(missing);
    free(target);
    free(tmp);
}

static int setup(void)
{
    wchar_t tmp[MAX_PATH], name[160];
    int i;

    if (GetTempPathW(MAX_PATH, tmp) == 0) {
        return 0;
    }
    swprintf(g_root, MAX_PATH, L"%lsbytesieve_export_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    swprintf(g_out, MAX_PATH, L"%lsbytesieve_export_out_%lu", tmp,
             (unsigned long)GetCurrentProcessId());
    remove_tree(g_root);
    remove_tree(g_out);
    if (!CreateDirectoryW(g_root, NULL) || !CreateDirectoryW(g_out, NULL)) {
        return 0;
    }
    /*
     * root\
     *   top.txt 7
     *   big\a.bin 2000, big\sub\b.bin 1000
     *   R&D 한글\h.txt 500
     *   a,b\c.txt 20
     *   =cmd\x 10
     *   empty\
     *   many\<긴 이름 400개>\ (빈 폴더, CSV 를 64 KiB 넘게)
     */
    if (!(write_file(L"top.txt", 7) && make_dir(L"big") && write_file(L"big\\a.bin", 2000) &&
          make_dir(L"big\\sub") && write_file(L"big\\sub\\b.bin", 1000) &&
          make_dir(L"R&D 한글") && write_file(L"R&D 한글\\h.txt", 500) && make_dir(L"a,b") &&
          write_file(L"a,b\\c.txt", 20) && make_dir(L"=cmd") && write_file(L"=cmd\\x", 10) &&
          make_dir(L"empty") && make_dir(L"many"))) {
        return 0;
    }
    for (i = 0; i < MANY_DIRS; i++) {
        swprintf(name, 160, L"many\\%03d-%ls", i,
                 L"long-folder-name-to-fill-the-csv-buffer-beyond-sixty-four-kibibytes-xxxxxxxx");
        if (!make_dir(name)) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    ScanModel m;

    if (!setup()) {
        fprintf(stderr, "fixture setup failed\n");
        return 1;
    }
    scan_model_init(&m);
    scan_model_build(&m, g_root, SCAN_DEPTH_UNLIMITED, NULL, NULL);
    CHECK(m.status == SCAN_COMPLETE && m.len == 8 + MANY_DIRS);
    test_csv(&m);
    test_html(&m);
    test_failures(&m);
    scan_model_free(&m);
    remove_tree(g_root);
    remove_tree(g_out);

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all export tests passed\n");
    return 0;
}
