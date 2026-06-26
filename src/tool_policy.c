#include "tool_policy.h"

#include <stdio.h>
#include <string.h>

static const char *skip_ws(const char *p) {
    while (p && *p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

static bool decode_json_string_value(const char **pp, char *dest, size_t max_len) {
    const char *p = pp ? *pp : NULL;
    if (!p || *p != '"' || !dest || max_len == 0) return false;
    p++;
    size_t i = 0;
    while (*p) {
        if (*p == '"') {
            dest[i] = '\0';
            *pp = p + 1;
            return true;
        }
        if (*p == '\\') {
            p++;
            if (!*p) return false;
            switch (*p) {
                case '"': case '\\': case '/': dest[i++] = *p; break;
                case 'b': dest[i++] = '\b'; break;
                case 'f': dest[i++] = '\f'; break;
                case 'n': dest[i++] = '\n'; break;
                case 'r': dest[i++] = '\r'; break;
                case 't': dest[i++] = '\t'; break;
                default: return false;
            }
            p++;
            if (i + 1 >= max_len) return false;
            continue;
        }
        if ((unsigned char)*p < 0x20 || i + 1 >= max_len) return false;
        dest[i++] = *p++;
    }
    return false;
}

static bool tool_args_exact_object_field(const char *json, const char *field) {
    if (!json || !field || !field[0]) return false;
    const char *p = skip_ws(json);
    if (!p || *p != '{') return false;
    p++;
    p = skip_ws(p);
    char key[64];
    if (!decode_json_string_value(&p, key, sizeof(key)) || strcmp(key, field) != 0) return false;
    p = skip_ws(p);
    if (*p != ':') return false;
    p++;
    p = skip_ws(p);
    char value[2048];
    if (!decode_json_string_value(&p, value, sizeof(value))) return false;
    p = skip_ws(p);
    if (*p != '}') return false;
    p++;
    return *skip_ws(p) == '\0' && value[0] != '\0';
}

bool tool_policy_is_denied(const char *tool_name) {
    return tool_name && strcmp(tool_name, "shell") == 0;
}

bool tool_policy_is_safe_tool(const char *tool_name) {
    if (!tool_name) return false;
    return strcmp(tool_name, "google_search") == 0 ||
           strcmp(tool_name, "browse_url") == 0 ||
           strcmp(tool_name, "read") == 0 ||
           strcmp(tool_name, "list") == 0 ||
           strcmp(tool_name, "search") == 0 ||
           strcmp(tool_name, "visit_page") == 0;
}

bool tool_policy_requires_permission(const char *tool_name) {
    return !tool_policy_is_safe_tool(tool_name) && !tool_policy_is_denied(tool_name);
}

bool tool_policy_args_valid(const char *tool_name, const char *tool_args_json) {
    if (!tool_name || !tool_name[0] || !tool_args_json || !tool_args_json[0]) return false;
    if (strcmp(tool_name, "google_search") == 0) return tool_args_exact_object_field(tool_args_json, "query");
    if (strcmp(tool_name, "browse_url") == 0) return tool_args_exact_object_field(tool_args_json, "url");
    if (strcmp(tool_name, "shell_command") == 0) return tool_args_exact_object_field(tool_args_json, "command");
    return true;
}

const char *tool_policy_error_message(ToolErrorKind kind) {
    switch (kind) {
        case TOOL_ERROR_DENIED: return "tool denied by bridge policy";
        case TOOL_ERROR_MALFORMED_ARGS: return "malformed tool arguments";
        case TOOL_ERROR_TIMEOUT: return "tool execution timeout";
        default: return "tool execution failed";
    }
}

void tool_policy_build_error_summary(const char *tool_name,
                                     ToolErrorKind kind,
                                     const char *detail,
                                     char *dest,
                                     size_t max_len) {
    if (!dest || max_len == 0) return;
    snprintf(dest, max_len, "TOOL_ERROR %s %s%s%s\n",
             tool_name ? tool_name : "unknown",
             tool_policy_error_message(kind),
             detail && detail[0] ? " " : "",
             detail && detail[0] ? detail : "");
}
