#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsonl.h"

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n",                \
                    __FILE__, __LINE__, #cond);                         \
            g_failures++;                                               \
        }                                                               \
    } while (0)

/* 버퍼 내용이 expected 와 같은지 확인하고 버퍼를 비운다. */
#define CHECK_BUF(buf, expected)                                        \
    do {                                                                \
        const char *e_ = (expected);                                    \
        const char *a_ = (buf)->data ? (buf)->data : "";                \
        if ((buf)->failed || strcmp(a_, e_) != 0) {                     \
            fprintf(stderr, "%s:%d: expected [%s]\n  got [%s]\n",       \
                    __FILE__, __LINE__, e_, a_);                        \
            g_failures++;                                               \
        }                                                               \
        (buf)->len = 0;                                                 \
        if ((buf)->data) {                                              \
            (buf)->data[0] = '\0';                                      \
        }                                                               \
    } while (0)

static void test_string_escape(void)
{
    JsonBuf b;

    jsonl_buf_init(&b);

    jsonl_append_string(&b, L"plain");
    CHECK_BUF(&b, "\"plain\"");

    jsonl_append_string(&b, L"");
    CHECK_BUF(&b, "\"\"");

    jsonl_append_string(&b, NULL);
    CHECK_BUF(&b, "null");

    /* Windows 경로의 역슬래시와 따옴표. */
    jsonl_append_string(&b, L"C:\\Users\\\"q\"\\a.txt");
    CHECK_BUF(&b, "\"C:\\\\Users\\\\\\\"q\\\"\\\\a.txt\"");

    jsonl_append_string(&b, L"\b\f\n\r\t\x01\x1f/");
    CHECK_BUF(&b, "\"\\b\\f\\n\\r\\t\\u0001\\u001f/\"");

    /* 비 ASCII 는 UTF-8 로 그대로: é(2바이트), 한글(3바이트), 😀(서로게이트 쌍 → 4바이트). */
    jsonl_append_string(&b, L"\x00e9\xd55c\xae00\xd83d\xde00");
    CHECK_BUF(&b, "\"\xc3\xa9\xed\x95\x9c\xea\xb8\x80\xf0\x9f\x98\x80\"");

    /* 짝 없는 서로게이트는 \u 이스케이프로 보존한다. */
    jsonl_append_string(&b, L"a\xd800" L"b\xdc00");
    CHECK_BUF(&b, "\"a\\ud800b\\udc00\"");

    jsonl_buf_free(&b);
    CHECK(b.data == NULL && b.len == 0);
}

static void test_start(void)
{
    JsonBuf b;

    jsonl_buf_init(&b);
    jsonl_start(&b, L"C:\\data\\한글");
    CHECK_BUF(&b, "{\"event\":\"start\",\"target\":\"C:\\\\data\\\\\xed\x95\x9c\xea\xb8\x80\"}\n");
    jsonl_buf_free(&b);
}

static void test_progress(void)
{
    JsonBuf b;
    ScanReport r;

    jsonl_buf_init(&b);
    scan_report_init(&r);
    r.file_count = 12;
    r.dir_count = 3;
    r.total_bytes = 0xFFFFFFFFFFFFFFFFULL;
    r.error_count = 1;
    jsonl_progress(&b, &r);
    CHECK_BUF(&b, "{\"event\":\"progress\",\"files\":12,\"dirs\":3,"
                  "\"bytes\":18446744073709551615,\"errors\":1}\n");
    jsonl_buf_free(&b);
}

static void test_progress_visit(void)
{
    JsonBuf b;
    ScanReport r;
    JsonlProgress p;
    ScanEntry e = {0};
    int i;

    jsonl_buf_init(&b);
    scan_report_init(&r);
    p.buf = &b;
    p.report = &r;
    p.out = NULL;
    p.interval = 2;
    p.seen = 0;

    for (i = 0; i < 5; i++) {
        r.file_count++;
        jsonl_progress_visit(&e, &p);
    }
    /* 2번째, 4번째 항목에서만 낸다. */
    CHECK_BUF(&b, "{\"event\":\"progress\",\"files\":2,\"dirs\":0,\"bytes\":0,\"errors\":0}\n"
                  "{\"event\":\"progress\",\"files\":4,\"dirs\":0,\"bytes\":0,\"errors\":0}\n");

    p.interval = 0;
    jsonl_progress_visit(&e, &p);
    CHECK(b.len == 0);

    jsonl_buf_free(&b);
}

static void test_result(void)
{
    JsonBuf b;
    ScanReport r;
    ScanError errs[2];

    jsonl_buf_init(&b);
    scan_report_init(&r);

    jsonl_result(&b, L"D:\\", &r);
    CHECK_BUF(&b, "{\"event\":\"result\",\"target\":\"D:\\\\\",\"files\":0,\"dirs\":0,"
                  "\"bytes\":0,\"alloc_bytes\":0,\"disk\":{\"slack\":0,\"compressed_saved\":0,"
                  "\"sparse_saved\":0,\"cloud_saved\":0,\"other_saved\":0,\"unknown_files\":0},"
                  "\"reparse_points\":0,\"error_count\":0,\"errors\":[]}\n");

    errs[0].path = L"D:\\a \"b\"";
    errs[0].code = 5;
    errs[1].path = NULL;
    errs[1].code = 87;
    r.file_count = 3;
    r.dir_count = 2;
    r.total_bytes = 5368709123ULL;
    r.reparse_count = 1;
    r.error_count = 3; /* 저장하지 못한 오류도 센다. */
    r.errors = errs;
    r.errors_len = 2;
    r.alloc_bytes = 5368713216ULL;
    r.slack_bytes = 4093;
    r.compressed_saved_bytes = 1;
    r.sparse_saved_bytes = 2;
    r.cloud_saved_bytes = 3;
    r.other_saved_bytes = 4;
    r.alloc_unknown_files = 5;
    jsonl_result(&b, L"D:\\", &r);
    CHECK_BUF(&b, "{\"event\":\"result\",\"target\":\"D:\\\\\",\"files\":3,\"dirs\":2,"
                  "\"bytes\":5368709123,\"alloc_bytes\":5368713216,\"disk\":{\"slack\":4093,"
                  "\"compressed_saved\":1,\"sparse_saved\":2,\"cloud_saved\":3,"
                  "\"other_saved\":4,\"unknown_files\":5},\"reparse_points\":1,"
                  "\"error_count\":3,"
                  "\"errors\":[{\"path\":\"D:\\\\a \\\"b\\\"\",\"code\":5},"
                  "{\"path\":null,\"code\":87}]}\n");

    jsonl_buf_free(&b);
}

static void test_changes(void)
{
    JsonBuf b;
    HistoryDiff d;
    HistoryChange items[3];

    jsonl_buf_init(&b);
    jsonl_changes(&b, NULL, L"C:\\h\\a.snap", 10);
    CHECK_BUF(&b, "{\"event\":\"changes\",\"previous\":false,\"saved_to\":\"C:\\\\h\\\\a.snap\"}\n");

    history_diff_init(&d);
    memset(items, 0, sizeof(items));
    items[0].rel = L"proj\\node_modules";
    items[0].kind = HISTORY_GROWN;
    items[0].old_bytes = 200;
    items[0].new_bytes = 1200;
    items[0].delta = 1000;
    items[1].rel = L"old";
    items[1].kind = HISTORY_REMOVED;
    items[1].old_bytes = 100;
    items[1].delta = -100;
    items[1].uncertain = 1;
    items[2].rel = L"x";
    items[2].kind = HISTORY_ADDED;
    items[2].new_bytes = 5;
    items[2].delta = 5;
    d.items = items;
    d.len = 3;
    d.old_time = 133000000000000000ULL;
    d.old_total = 300;
    d.new_total = 1205;
    /* 최대 2개만 나열하고 나머지는 more 로 센다. 저장 실패면 saved_to 는 null. */
    jsonl_changes(&b, &d, NULL, 2);
    CHECK_BUF(&b, "{\"event\":\"changes\",\"previous\":true,\"since\":133000000000000000,"
                  "\"old_bytes\":300,\"new_bytes\":1205,\"items\":["
                  "{\"path\":\"proj\\\\node_modules\",\"kind\":\"grown\",\"old\":200,"
                  "\"new\":1200,\"delta\":1000,\"uncertain\":false},"
                  "{\"path\":\"old\",\"kind\":\"removed\",\"old\":100,\"new\":0,"
                  "\"delta\":-100,\"uncertain\":true}],\"more\":1,\"saved_to\":null}\n");
    jsonl_buf_free(&b);
}

static void test_complete(void)
{
    JsonBuf b;

    jsonl_buf_init(&b);
    jsonl_complete(&b, SCAN_COMPLETE, 0);
    CHECK_BUF(&b, "{\"event\":\"complete\",\"status\":\"complete\",\"exit_code\":0}\n");
    jsonl_complete(&b, SCAN_PARTIAL, 1);
    CHECK_BUF(&b, "{\"event\":\"complete\",\"status\":\"partial\",\"exit_code\":1}\n");
    jsonl_complete(&b, SCAN_FAILED, 2);
    CHECK_BUF(&b, "{\"event\":\"complete\",\"status\":\"failed\",\"exit_code\":2}\n");
    jsonl_buf_free(&b);
}

/* 여러 이벤트를 쌓으면 각자 한 줄이고, flush 는 그대로 쓰고 버퍼를 비운다. */
static void test_lines_and_flush(void)
{
    JsonBuf b;
    ScanReport r;
    FILE *f = tmpfile();
    char out[512];
    size_t n, i, lines = 0;

    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    jsonl_buf_init(&b);
    scan_report_init(&r);

    jsonl_start(&b, L"x\ny");
    jsonl_progress(&b, &r);
    jsonl_result(&b, L"x\ny", &r);
    jsonl_complete(&b, SCAN_COMPLETE, 0);
    for (i = 0; i < b.len; i++) {
        lines += b.data[i] == '\n';
    }
    CHECK(lines == 4); /* 경로 속 줄바꿈은 \n 으로 이스케이프되어 줄을 늘리지 않는다. */

    n = b.len;
    CHECK(jsonl_buf_flush(&b, f));
    CHECK(b.len == 0);
    rewind(f);
    CHECK(fread(out, 1, sizeof(out), f) == n);
    {
        static const char first[] = "{\"event\":\"start\",\"target\":\"x\\ny\"}\n";
        CHECK(strncmp(out, first, sizeof(first) - 1) == 0);
    }
    CHECK(out[n - 1] == '\n');

    fclose(f);
    jsonl_buf_free(&b);
}

int main(void)
{
    test_string_escape();
    test_start();
    test_progress();
    test_progress_visit();
    test_result();
    test_changes();
    test_complete();
    test_lines_and_flush();

    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("all jsonl tests passed\n");
    return 0;
}
