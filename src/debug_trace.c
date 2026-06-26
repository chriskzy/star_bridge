#include "debug_trace.h"
#include "config_manager.h"
#include "json_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void redact_line_value(char *text, const char *needle) {
    if (!text || !needle) return;
    char *p = text;
    while ((p = strstr(p, needle)) != NULL) {
        char *line_end = p;
        while (*line_end && *line_end != '\r' && *line_end != '\n') line_end++;
        const char *redacted = "[redacted]";
        size_t redacted_len = strlen(redacted);
        size_t tail_len = strlen(line_end);
        memmove(p + redacted_len, line_end, tail_len + 1);
        memcpy(p, redacted, redacted_len);
        p += redacted_len;
    }
}

void debug_trace_redact_text(char *text) {
    static const char *needles[] = {
        "Authorization:",
        "authorization:",
        "Cookie:",
        "cookie:",
        "Set-Cookie:",
        "set-cookie:",
        "OPENAI_API_KEY",
        "GOOGLE_SEARCH_API_KEY",
        "GOOGLE_SEARCH_CX",
        "api_key",
        "access_token",
        "refresh_token"
    };
    if (!text) return;
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        redact_line_value(text, needles[i]);
    }
}

void debug_trace_compact_text(const char *src, char *dest, size_t max_len) {
    if (!dest || max_len == 0) return;
    size_t out = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    while (*p && out + 1 < max_len) {
        unsigned char c = *p++;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c < 0x20 || c > 0x7e) c = '.';
        dest[out++] = (char)c;
    }
    dest[out] = '\0';
    debug_trace_redact_text(dest);
}

void debug_trace_append(const char *fmt, ...) {
    if (!global_config.debug_log_enabled || !global_config.debug_log_path[0]) return;

    /* When JSON mode is enabled, delegate to json_log */
    if (debug_trace_json_mode()) {
        va_list ap;
        va_start(ap, fmt);
        char msg[4096];
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        json_log(LOG_LEVEL_DEBUG, "trace", "%s", msg);
        return;
    }

    FILE *f = fopen(global_config.debug_log_path, "a");
    if (!f) return;

    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    if (tm) {
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", tm);
        fprintf(f, "ts=%s ", ts);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void debug_trace_fixture_capture(unsigned long request_number, const char *raw_request, size_t raw_len, const char *body, size_t body_len) {
    if (!global_config.debug_log_enabled || !global_config.debug_log_path[0]) return;
    FILE *f = fopen(global_config.debug_log_path, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    if (tm) {
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", tm);
        fprintf(f, "ts=%s ", ts);
    }
    fprintf(f, "fixture=1 request=%lu raw_bytes=%zu body_bytes=%zu headers=\"", request_number, raw_len, body_len);
    /* Write sanitized headers: find first header line, redact sensitive values */
    const char *h = raw_request ? raw_request : "";
    size_t hlen = raw_len;
    if (hlen > 4096) hlen = 4096;
    for (size_t i = 0; i < hlen; i++) {
        unsigned char c = (unsigned char)h[i];
        if (c == '\r') { fprintf(f, "\\r"); }
        else if (c == '\n') { fprintf(f, "\\n"); }
        else if (c == '"') { fprintf(f, "\\\""); }
        else if (c < 0x20 || c > 0x7e) { fprintf(f, "\\x%02x", c); }
        else { fputc(c, f); }
    }
    fprintf(f, "\" body=\"");
    /* Write sanitized body snippet (first 2048 bytes) */
    size_t blen = body_len;
    if (blen > 2048) blen = 2048;
    for (size_t i = 0; i < blen; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c == '\r') { fprintf(f, "\\r"); }
        else if (c == '\n') { fprintf(f, "\\n"); }
        else if (c == '"') { fprintf(f, "\\\""); }
        else if (c < 0x20 || c > 0x7e) { fprintf(f, "\\x%02x", c); }
        else { fputc(c, f); }
    }
    if (body_len > 2048) fprintf(f, "...");
    fprintf(f, "\"\n");
    fclose(f);
}

void debug_trace_body_probe(unsigned long request_number, const char *body, size_t body_len) {
    const unsigned char *p = (const unsigned char *)(body ? body : "");
    size_t skipped = 0;
    while (skipped < body_len && (p[skipped] == ' ' || p[skipped] == '\t' || p[skipped] == '\r' || p[skipped] == '\n')) {
        skipped++;
    }
    if (skipped >= body_len) {
        debug_trace_append("request=%lu body_len=%zu body_first_non_ws=(none)", request_number, body_len);
        return;
    }
    unsigned char c = p[skipped];
    debug_trace_append("request=%lu body_len=%zu body_first_non_ws=0x%02x body_first_printable=%c",
                       request_number,
                       body_len,
                       c,
                       (c >= 0x20 && c <= 0x7e) ? (char)c : '.');
}
