#include "json_log.h"
#include "config_manager.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* JSON-mode flag for debug_trace */
static bool g_json_mode = false;

void debug_trace_set_json_mode(bool enabled) {
    g_json_mode = enabled;
}

bool debug_trace_json_mode(void) {
    return g_json_mode;
}

/* Format a timestamp string into buffer */
static void format_timestamp(char *buf, size_t max) {
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    if (tm) {
        strftime(buf, max, "%Y-%m-%dT%H:%M:%S%z", tm);
    } else {
        snprintf(buf, max, "%ld", (long)now);
    }
}

/* Write a JSON string value with proper escaping into buffer.
 * Returns number of bytes written (excluding null terminator). */
static size_t json_escape_to_buf(char *buf, size_t max, const char *s) {
    if (!s || !buf || max == 0) return 0;
    size_t off = 0;
    buf[off++] = '"';
    if (max < 3) { buf[off] = '\0'; return off; }
    for (const char *p = s; *p && off + 6 < max; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  buf[off++] = '\\'; buf[off++] = '"'; break;
            case '\\': buf[off++] = '\\'; buf[off++] = '\\'; break;
            case '\n': buf[off++] = '\\'; buf[off++] = 'n'; break;
            case '\r': buf[off++] = '\\'; buf[off++] = 'r'; break;
            case '\t': buf[off++] = '\\'; buf[off++] = 't'; break;
            default:
                if (c < 0x20 || c > 0x7e) {
                    if (off + 7 < max) {
                        off += (size_t)snprintf(buf + off, max - off, "\\u%04x", c);
                    } else {
                        buf[off++] = '.';
                    }
                } else {
                    buf[off++] = (char)c;
                }
        }
    }
    buf[off++] = '"';
    buf[off] = '\0';
    return off;
}

/* Core JSON log emitter.
 * Writes a single JSON line to stderr with timestamp, level, event, and message.
 * If debug_log_path is configured, also appends to that file.
 * request_id and duration_ms are set to null (not available at module scope).
 */
void json_log(LogLevel level, const char *event, const char *fmt, ...) {
    if (!event || !fmt) return;

    /* Timestamp */
    char ts[64];
    format_timestamp(ts, sizeof(ts));

    /* Level string */
    const char *level_str = "";
    switch (level) {
        case LOG_LEVEL_INFO:  level_str = "info";  break;
        case LOG_LEVEL_WARN:  level_str = "warn";  break;
        case LOG_LEVEL_ERROR: level_str = "error"; break;
        case LOG_LEVEL_DEBUG: level_str = "debug"; break;
    }

    /* Format message */
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Determine output files */
    FILE *f_stderr = stderr;
    FILE *f_log = NULL;
    if (global_config.debug_log_path[0]) {
        f_log = fopen(global_config.debug_log_path, "a");
    }

    /* Build JSON line into buffer */
    char buf[8192];
    size_t off = 0;

    /* Opening object */
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "{\"ts\":\"%s\",\"level\":\"%s\",\"event\":",
                            ts, level_str);
    /* Event field (JSON string) */
    off += json_escape_to_buf(buf + off, sizeof(buf) - off, event);

    /* request_id: null (not available globally) */
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, ",\"request_id\":null");

    /* duration_ms: null (not available globally) */
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, ",\"duration_ms\":null");

    /* message field */
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, ",\"msg\":");
    off += json_escape_to_buf(buf + off, sizeof(buf) - off, msg);

    /* Close object */
    off += (size_t)snprintf(buf + off, sizeof(buf) - off, "}\n");

    /* Write to stderr */
    fputs(buf, f_stderr);
    fflush(f_stderr);

    /* Write to debug log if configured */
    if (f_log) {
        fputs(buf, f_log);
        fclose(f_log);
    }
}
