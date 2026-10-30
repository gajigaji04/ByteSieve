#include <stdlib.h>
#include <string.h>

#include "jsonl.h"

void jsonl_buf_init(JsonBuf *buf)
{
    memset(buf, 0, sizeof(*buf));
}

void jsonl_buf_free(JsonBuf *buf)
{
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    jsonl_buf_init(buf);
}

int jsonl_buf_flush(JsonBuf *buf, FILE *out)
{
    int ok = !buf->failed;

    if (ok && buf->len > 0) {
        ok = fwrite(buf->data, 1, buf->len, out) == buf->len && fflush(out) == 0;
    }
    buf->len = 0;
    buf->failed = 0;
    return ok;
}

static void append(JsonBuf *buf, const char *s, size_t n)
{
    if (buf->failed) {
        return;
    }
    /* 종료 NUL 자리를 항상 남겨 data 를 C 문자열로도 읽을 수 있게 한다. */
    if (buf->len + n + 1 > buf->cap) {
        size_t cap = buf->cap ? buf->cap : 256;
        char *grown;
        while (buf->len + n + 1 > cap) {
            cap *= 2;
        }
        grown = realloc(buf->data, cap);
        if (grown == NULL) {
            buf->failed = 1;
            return;
        }
        buf->data = grown;
        buf->cap = cap;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
}

static void append_str(JsonBuf *buf, const char *s)
{
    append(buf, s, strlen(s));
}

static void append_u64(JsonBuf *buf, uint64_t value)
{
    char num[24];
    int n = snprintf(num, sizeof(num), "%llu", (unsigned long long)value);
    append(buf, num, (size_t)n);
}

/* "key": 형태로 키를 추가한다. 첫 키가 아니면 앞에 쉼표를 붙인다. */
static void append_key(JsonBuf *buf, const char *key)
{
    if (buf->len > 0 && buf->data[buf->len - 1] != '{') {
        append(buf, ",", 1);
    }
    append(buf, "\"", 1);
    append_str(buf, key);
    append(buf, "\":", 2);
}

static void append_u64_field(JsonBuf *buf, const char *key, uint64_t value)
{
    append_key(buf, key);
    append_u64(buf, value);
}

static void append_utf8(JsonBuf *buf, uint32_t cp)
{
    char out[4];
    size_t n;

    if (cp < 0x80) {
        out[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    append(buf, out, n);
}

static void append_unicode_escape(JsonBuf *buf, unsigned value)
{
    char esc[8];
    snprintf(esc, sizeof(esc), "\\u%04x", value & 0xFFFFu);
    append(buf, esc, 6);
}

void jsonl_append_string(JsonBuf *buf, const wchar_t *s)
{
    if (s == NULL) {
        append_str(buf, "null");
        return;
    }

    append(buf, "\"", 1);
    for (; *s != L'\0'; s++) {
        unsigned c = (unsigned)*s;

        switch (c) {
        case '"':  append(buf, "\\\"", 2); continue;
        case '\\': append(buf, "\\\\", 2); continue;
        case '\b': append(buf, "\\b", 2); continue;
        case '\f': append(buf, "\\f", 2); continue;
        case '\n': append(buf, "\\n", 2); continue;
        case '\r': append(buf, "\\r", 2); continue;
        case '\t': append(buf, "\\t", 2); continue;
        default: break;
        }

        if (c < 0x20) {
            append_unicode_escape(buf, c);
        } else if (c >= 0xD800 && c <= 0xDBFF &&
                   (unsigned)s[1] >= 0xDC00 && (unsigned)s[1] <= 0xDFFF) {
            uint32_t cp = 0x10000 + (((uint32_t)c - 0xD800) << 10) +
                          ((uint32_t)s[1] - 0xDC00);
            append_utf8(buf, cp);
            s++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            /* NTFS 이름에는 짝 없는 서로게이트가 있을 수 있다. UTF-8 로는 표현할 수 없다. */
            append_unicode_escape(buf, c);
        } else {
            append_utf8(buf, c);
        }
    }
    append(buf, "\"", 1);
}

static void begin_event(JsonBuf *buf, const char *name)
{
    append_str(buf, "{\"event\":\"");
    append_str(buf, name);
    append(buf, "\"", 1);
}

static void end_event(JsonBuf *buf)
{
    append(buf, "}\n", 2);
}

void jsonl_start(JsonBuf *buf, const wchar_t *target)
{
    begin_event(buf, "start");
    append_key(buf, "target");
    jsonl_append_string(buf, target);
    end_event(buf);
}

void jsonl_progress(JsonBuf *buf, const ScanReport *report)
{
    begin_event(buf, "progress");
    append_u64_field(buf, "files", report->file_count);
    append_u64_field(buf, "dirs", report->dir_count);
    append_u64_field(buf, "bytes", report->total_bytes);
    append_u64_field(buf, "errors", report->error_count);
    end_event(buf);
}

void jsonl_result(JsonBuf *buf, const wchar_t *target, const ScanReport *report)
{
    size_t i;

    begin_event(buf, "result");
    append_key(buf, "target");
    jsonl_append_string(buf, target);
    append_u64_field(buf, "files", report->file_count);
    append_u64_field(buf, "dirs", report->dir_count);
    append_u64_field(buf, "bytes", report->total_bytes);
    append_u64_field(buf, "reparse_points", report->reparse_count);
    append_u64_field(buf, "error_count", report->error_count);
    append_key(buf, "errors");
    append(buf, "[", 1);
    for (i = 0; i < report->errors_len; i++) {
        if (i > 0) {
            append(buf, ",", 1);
        }
        append_str(buf, "{\"path\":");
        jsonl_append_string(buf, report->errors[i].path);
        append_str(buf, ",\"code\":");
        append_u64(buf, report->errors[i].code);
        append(buf, "}", 1);
    }
    append(buf, "]", 1);
    end_event(buf);
}

void jsonl_complete(JsonBuf *buf, ScanStatus status, int exit_code)
{
    const char *name;

    switch (status) {
    case SCAN_COMPLETE:
        name = "complete";
        break;
    case SCAN_PARTIAL:
        name = "partial";
        break;
    default:
        name = "failed";
        break;
    }

    begin_event(buf, "complete");
    append_key(buf, "status");
    append(buf, "\"", 1);
    append_str(buf, name);
    append(buf, "\"", 1);
    append_key(buf, "exit_code");
    if (exit_code < 0) {
        append(buf, "-", 1);
        exit_code = -exit_code;
    }
    append_u64(buf, (uint64_t)exit_code);
    end_event(buf);
}

void jsonl_progress_visit(const ScanEntry *entry, void *ctx)
{
    JsonlProgress *p = ctx;

    (void)entry;
    if (p->interval == 0) {
        return;
    }
    p->seen++;
    if (p->seen % p->interval == 0) {
        jsonl_progress(p->buf, p->report);
        if (p->out != NULL) {
            jsonl_buf_flush(p->buf, p->out);
        }
    }
}
