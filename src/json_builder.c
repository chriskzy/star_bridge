#include "json_builder.h"
#include <stdio.h>
#include <string.h>

void jsonb_init(JsonBuilder *b, char *buf, size_t max) {
    b->buf = buf;
    b->max = max;
    b->off = 0;
    b->depth = 0;
    b->failed = false;
    if (max > 0) buf[0] = '\0';
    for (int i = 0; i < 16; i++) b->first[i] = true;
}

static void jsonb_put(JsonBuilder *b, const char *s) {
    if (b->failed) return;
    size_t len = strlen(s);
    if (b->off + len >= b->max) {
        b->failed = true;
        return;
    }
    memcpy(b->buf + b->off, s, len);
    b->off += len;
    b->buf[b->off] = '\0';
}

static void jsonb_comma(JsonBuilder *b) {
    int d = b->depth;
    if (d < 0) d = 0;
    if (d >= 16) d = 15;
    if (!b->first[d]) jsonb_put(b, ",");
    b->first[d] = false;
}

void jsonb_object_begin(JsonBuilder *b) {
    jsonb_put(b, "{");
    if (b->depth < 16) b->first[b->depth] = true;
    b->depth++;
}

void jsonb_object_end(JsonBuilder *b) {
    b->depth--;
    jsonb_put(b, "}");
}

void jsonb_array_begin(JsonBuilder *b) {
    jsonb_put(b, "[");
    if (b->depth < 16) b->first[b->depth] = true;
    b->depth++;
}

void jsonb_array_end(JsonBuilder *b) {
    b->depth--;
    jsonb_put(b, "]");
}

static void jsonb_quoted(JsonBuilder *b, const char *s) {
    jsonb_put(b, "\"");
    jsonb_put(b, s);
    jsonb_put(b, "\"");
}

void jsonb_string(JsonBuilder *b, const char *key, const char *val) {
    jsonb_comma(b);
    jsonb_quoted(b, key);
    jsonb_put(b, ":");
    jsonb_quoted(b, val);
}

void jsonb_string_escape(JsonBuilder *b, const char *key, const char *val) {
    jsonb_comma(b);
    jsonb_quoted(b, key);
    jsonb_put(b, ":\"");
    /* Write val with JSON escaping */
    const char *p = val;
    while (*p) {
        if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r' || *p == '\t') {
            char esc[8];
            switch (*p) {
                case '"':  snprintf(esc, sizeof(esc), "\\\""); break;
                case '\\': snprintf(esc, sizeof(esc), "\\\\"); break;
                case '\n': snprintf(esc, sizeof(esc), "\\n"); break;
                case '\r': snprintf(esc, sizeof(esc), "\\r"); break;
                case '\t': snprintf(esc, sizeof(esc), "\\t"); break;
                default:   snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p); break;
            }
            jsonb_put(b, esc);
        } else {
            char ch[2] = { *p, '\0' };
            jsonb_put(b, ch);
        }
        p++;
    }
    jsonb_put(b, "\"");
}

void jsonb_number(JsonBuilder *b, const char *key, long val) {
    jsonb_comma(b);
    jsonb_quoted(b, key);
    jsonb_put(b, ":");
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", val);
    jsonb_put(b, buf);
}

void jsonb_bool(JsonBuilder *b, const char *key, bool val) {
    jsonb_comma(b);
    jsonb_quoted(b, key);
    jsonb_put(b, ":");
    jsonb_put(b, val ? "true" : "false");
}

void jsonb_raw(JsonBuilder *b, const char *key, const char *val) {
    jsonb_comma(b);
    jsonb_quoted(b, key);
    jsonb_put(b, ":");
    jsonb_put(b, val);
}

void jsonb_raw_value(JsonBuilder *b, const char *val) {
    jsonb_put(b, val);
}

void jsonb_element_string(JsonBuilder *b, const char *val) {
    jsonb_comma(b);
    jsonb_quoted(b, val);
}

void jsonb_element_number(JsonBuilder *b, long val) {
    jsonb_comma(b);
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", val);
    jsonb_put(b, buf);
}

void jsonb_element_bool(JsonBuilder *b, bool val) {
    jsonb_comma(b);
    jsonb_put(b, val ? "true" : "false");
}

void jsonb_element_raw(JsonBuilder *b, const char *val) {
    jsonb_comma(b);
    jsonb_put(b, val);
}
