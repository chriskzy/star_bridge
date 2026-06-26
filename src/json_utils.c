#include "json_utils.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/*  Parser wrappers                                                    */
/* ------------------------------------------------------------------ */

void *json_parse(const char *json) {
    if (!json) return NULL;
    return cJSON_Parse(json);
}

void json_free(void *obj) {
    if (obj) cJSON_Delete((cJSON *)obj);
}

bool json_get_string(const void *obj, const char *field, char *dest, size_t max_len) {
    if (!obj || !field || !dest) return false;
    cJSON *item = cJSON_GetObjectItem((const cJSON *)obj, field);
    if (!item || !cJSON_IsString(item) || !item->valuestring) return false;
    snprintf(dest, max_len, "%s", item->valuestring);
    return true;
}

const char *json_get_string_ptr(const void *obj, const char *field) {
    if (!obj || !field) return NULL;
    cJSON *item = cJSON_GetObjectItem((const cJSON *)obj, field);
    if (!item || !cJSON_IsString(item)) return NULL;
    return item->valuestring ? item->valuestring : "";
}

int json_get_int(const void *obj, const char *field) {
    if (!obj || !field) return 0;
    cJSON *item = cJSON_GetObjectItem((const cJSON *)obj, field);
    if (!item || !cJSON_IsNumber(item)) return 0;
    return (int)cJSON_GetNumberValue(item);
}

bool json_get_bool(const void *obj, const char *field) {
    if (!obj || !field) return false;
    cJSON *item = cJSON_GetObjectItem((const cJSON *)obj, field);
    if (!item || !cJSON_IsBool(item)) return false;
    return cJSON_IsTrue(item);
}

const void *json_get_object(const void *obj, const char *field) {
    if (!obj || !field) return NULL;
    cJSON *item = cJSON_GetObjectItem((const cJSON *)obj, field);
    if (!item || !cJSON_IsObject(item)) return NULL;
    return item;
}

const void *json_get_array(const void *obj, const char *field) {
    if (!obj || !field) return NULL;
    cJSON *item = cJSON_GetObjectItem((const cJSON *)obj, field);
    if (!item || !cJSON_IsArray(item)) return NULL;
    return item;
}

int json_array_count(const void *arr) {
    if (!arr) return 0;
    return cJSON_GetArraySize((const cJSON *)arr);
}

const void *json_array_item(const void *arr, int index) {
    if (!arr) return NULL;
    return cJSON_GetArrayItem((const cJSON *)arr, index);
}

bool json_array_get_string(const void *arr, int index, char *dest, size_t max_len) {
    if (!arr || !dest) return false;
    cJSON *item = cJSON_GetArrayItem((const cJSON *)arr, index);
    if (!item || !cJSON_IsString(item) || !item->valuestring) return false;
    snprintf(dest, max_len, "%s", item->valuestring);
    return true;
}

bool json_is_valid_object(const char *json) {
    if (!json) return false;
    cJSON *obj = cJSON_Parse(json);
    if (!obj) return false;
    bool ok = cJSON_IsObject(obj);
    cJSON_Delete(obj);
    return ok;
}

bool json_has_field(const char *json, const char *field) {
    if (!json || !field) return false;
    cJSON *obj = cJSON_Parse(json);
    if (!obj) return false;
    bool ok = cJSON_GetObjectItem(obj, field) != NULL;
    cJSON_Delete(obj);
    return ok;
}

bool json_field_is_string(const char *json, const char *field) {
    if (!json || !field) return false;
    cJSON *obj = cJSON_Parse(json);
    if (!obj) return false;
    cJSON *item = cJSON_GetObjectItem(obj, field);
    bool ok = item && cJSON_IsString(item);
    cJSON_Delete(obj);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  JSON escaping                                                      */
/* ------------------------------------------------------------------ */

void json_escape(const char *src, char *dest, size_t max_len) {
    if (!src || !dest || max_len == 0) return;
    size_t i = 0, o = 0;
    char esc[8];

    while (src[i] && o + 8 < max_len) {
        unsigned char c = src[i];
        switch (c) {
            case '\"':  snprintf(esc, sizeof(esc), "\\\""); break;
            case '\\':  snprintf(esc, sizeof(esc), "\\\\"); break;
            case '\b':  snprintf(esc, sizeof(esc), "\\b");  break;
            case '\f':  snprintf(esc, sizeof(esc), "\\f");  break;
            case '\n':  snprintf(esc, sizeof(esc), "\\n");  break;
            case '\r':  snprintf(esc, sizeof(esc), "\\r");  break;
            case '\t':  snprintf(esc, sizeof(esc), "\\t");  break;
            default:
                if (c < 0x20) {
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                } else {
                    esc[0] = c;
                    esc[1] = '\0';
                }
                break;
        }
        size_t elen = strlen(esc);
        if (o + elen >= max_len - 1) break;
        memcpy(dest + o, esc, elen);
        o += elen;
        i++;
    }
    dest[o] = '\0';
}

/* ------------------------------------------------------------------ */
/*  JSON string decoding                                               */
/* ------------------------------------------------------------------ */

bool json_decode_string(const char **pp, char *dest, size_t max_len) {
    if (!pp || !*pp || !dest) return false;
    const char *p = *pp;
    if (*p != '"') return false;
    p++; /* skip opening quote */

    size_t o = 0;
    while (*p && o < max_len - 1) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  dest[o++] = '"';  break;
                case '\\': dest[o++] = '\\'; break;
                case '/':  dest[o++] = '/';  break;
                case 'b':  dest[o++] = '\b'; break;
                case 'f':  dest[o++] = '\f'; break;
                case 'n':  dest[o++] = '\n'; break;
                case 'r':  dest[o++] = '\r'; break;
                case 't':  dest[o++] = '\t'; break;
                case 'u': {
                    /* Simplified unicode escape - just copy as-is for now */
                    char hex[16] = "\\u";
                    size_t hi = 2;
                    p++;
                    for (int i = 0; i < 4 && *p; i++) {
                        hex[hi++] = *p++;
                    }
                    hex[hi] = '\0';
                    /* For now, keep the escape sequence */
                    if (o + hi < max_len - 1) {
                        memcpy(dest + o, hex, hi);
                        o += hi;
                    }
                    continue;
                }
                default:
                    dest[o++] = *p;
                    break;
            }
            p++;
        } else if (*p == '"') {
            p++;
            dest[o] = '\0';
            *pp = p;
            return true;
        } else {
            dest[o++] = *p++;
        }
    }
    dest[o] = '\0';
    /* Reached end without closing quote */
    return false;
}

/* ------------------------------------------------------------------ */
/*  Utility probes                                                     */
/* ------------------------------------------------------------------ */

char json_probe_first_byte(const char *body, size_t len) {
    if (!body || len == 0) return '\0';
    size_t i = 0;
    while (i < len && body[i]) {
        if (!isspace((unsigned char)body[i]))
            return body[i];
        i++;
    }
    return '\0';
}

/* ------------------------------------------------------------------ */
/*  String extraction helpers (simple strstr-based for top-level only)  */
/* ------------------------------------------------------------------ */

bool json_extract_string(const char *json, const char *field, char *dest, size_t max_len) {
    if (!json || !field || !dest) return false;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", field);
    const char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < max_len) {
        dest[o++] = *p++;
    }
    dest[o] = '\0';
    return o > 0;
}

int json_extract_int(const char *json, const char *field, int fallback) {
    if (!json || !field) return fallback;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    const char *p = strstr(json, pattern);
    if (!p) return fallback;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p < '0' || *p > '9') return fallback;
    int val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return val;
}

bool json_extract_bool(const char *json, const char *field, bool fallback) {
    if (!json || !field) return fallback;
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", field);
    const char *p = strstr(json, pattern);
    if (!p) return fallback;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return fallback;
}

bool json_string_matches(const char *str, ...) {
    if (!str) return false;
    va_list ap;
    va_start(ap, str);
    const char *candidate;
    while ((candidate = va_arg(ap, const char *)) != NULL) {
        if (strcmp(str, candidate) == 0) {
            va_end(ap);
            return true;
        }
    }
    va_end(ap);
    return false;
}
