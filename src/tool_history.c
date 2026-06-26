#include "tool_history.h"
#include "config_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------
 *  Tool history implementation
 * ------------------------------------------------------------------- */

/* Structured ring buffer */
static ToolHistoryEntry g_history_entries[MAX_HISTORY_ENTRIES];
static int g_history_count = 0;
static int g_history_max = 20;
static int g_history_drop_count = 0;

/* Raw string buffer (legacy injection format) */
static char g_tool_history[2048];
static size_t g_tool_history_len = 0;

/* Status string helpers */
const char *tool_status_to_string(ToolStatus s) {
    switch (s) {
        case TOOL_STATUS_OK:      return "ok";
        case TOOL_STATUS_ERROR:   return "error";
        case TOOL_STATUS_DENIED:  return "denied";
        case TOOL_STATUS_TIMEOUT: return "timeout";
        default:                  return "unknown";
    }
}

ToolStatus tool_status_from_string(const char *s) {
    if (!s) return TOOL_STATUS_UNKNOWN;
    if (strcmp(s, "ok") == 0 || strcmp(s, "OK") == 0) return TOOL_STATUS_OK;
    if (strcmp(s, "error") == 0 || strcmp(s, "ERROR") == 0) return TOOL_STATUS_ERROR;
    if (strcmp(s, "denied") == 0 || strcmp(s, "DENIED") == 0) return TOOL_STATUS_DENIED;
    if (strcmp(s, "timeout") == 0 || strcmp(s, "TIMEOUT") == 0) return TOOL_STATUS_TIMEOUT;
    return TOOL_STATUS_UNKNOWN;
}

void tool_history_append(const char *line) {
    if (!line || !line[0]) return;
    size_t len = strlen(line);
    if (len + 1 >= sizeof(g_tool_history)) {
        snprintf(g_tool_history, sizeof(g_tool_history), "%s", line);
        g_tool_history_len = strlen(g_tool_history);
        return;
    }
    if (g_tool_history_len + len + 1 >= sizeof(g_tool_history)) {
        size_t drop = (g_tool_history_len + len + 1) - sizeof(g_tool_history);
        if (drop >= g_tool_history_len) {
            g_tool_history_len = 0;
            g_tool_history[0] = '\0';
        } else {
            memmove(g_tool_history, g_tool_history + drop, g_tool_history_len - drop);
            g_tool_history_len -= drop;
            g_tool_history[g_tool_history_len] = '\0';
        }
    }
    memcpy(g_tool_history + g_tool_history_len, line, len);
    g_tool_history_len += len;
    g_tool_history[g_tool_history_len] = '\0';

    /* Also add to structured ring buffer */
    g_history_max = global_config.max_history_entries > 0 ? global_config.max_history_entries : 20;
    if (g_history_max > MAX_HISTORY_ENTRIES) g_history_max = MAX_HISTORY_ENTRIES;

    /* Parse line format: TOOL_RESULT <tool> <args> or TOOL_ERROR <tool> <msg> */
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", line);
    char *tool = NULL;
    char *detail = NULL;
    char *tok = tmp;
    /* Skip first token (TOOL_RESULT or TOOL_ERROR) */
    char *space = strchr(tok, ' ');
    if (space) {
        *space = '\0';
        tool = space + 1;
        char *space2 = strchr(tool, ' ');
        if (space2) {
            *space2 = '\0';
            detail = space2 + 1;
        }
    }
    if (!tool) return;

    /* Evict oldest if at max */
    if (g_history_count >= g_history_max) {
        int evict = g_history_count - g_history_max + 1;
        memmove(g_history_entries, g_history_entries + evict,
                sizeof(ToolHistoryEntry) * (g_history_count - evict));
        g_history_count -= evict;
        g_history_drop_count += evict;
    }

    ToolHistoryEntry *e = &g_history_entries[g_history_count];
    snprintf(e->tool_name, sizeof(e->tool_name), "%s", tool);
    if (detail) {
        snprintf(e->args, sizeof(e->args), "%s", detail);
    } else {
        e->args[0] = '\0';
    }
    e->status = strstr(line, "TOOL_ERROR") ? TOOL_STATUS_ERROR : TOOL_STATUS_OK;
    g_history_count++;
}

/* Build JSON array from tool history ring buffer.
 * Caller must free the returned buffer. */
char *tool_history_build_json(void) {
    /* Estimate: each entry ~512+ chars, up to MAX_HISTORY_ENTRIES entries */
    size_t max_entries = g_history_max > 0 ? g_history_max : 20;
    if (max_entries > MAX_HISTORY_ENTRIES) max_entries = MAX_HISTORY_ENTRIES;
    size_t alloc = max_entries * 1024 + 512;
    char *buf = malloc(alloc);
    if (!buf) return NULL;
    size_t off = 0;
    off += (size_t)snprintf(buf + off, alloc - off, "[");
    int start_index = 0;
    if (g_history_count > 12) {
        start_index = g_history_count - 12;
    }
    int dropped_count = g_history_drop_count + start_index;
    if (dropped_count > 0 && g_history_count > 0) {
        off += (size_t)snprintf(buf + off, alloc - off,
                "{\"truncated\":true,\"dropped\":%d},", dropped_count);
    }
    for (int i = start_index; i < g_history_count && off + 512 < alloc; i++) {
        ToolHistoryEntry *e = &g_history_entries[i];
        char args_esc[160];
        size_t ai = 0;
        for (const char *s = e->args; *s && ai + 5 < sizeof(args_esc); s++) {
            if (*s == '"') { args_esc[ai++] = '\\'; args_esc[ai++] = '"'; }
            else if (*s == '\\') { args_esc[ai++] = '\\'; args_esc[ai++] = '\\'; }
            else if (*s == '\n') { args_esc[ai++] = '\\'; args_esc[ai++] = 'n'; }
            else { args_esc[ai++] = *s; }
        }
        args_esc[ai] = '\0';
        char name_esc[128];
        ai = 0;
        for (const char *s = e->tool_name; *s && ai + 5 < sizeof(name_esc); s++) {
            if (*s == '"') { name_esc[ai++] = '\\'; name_esc[ai++] = '"'; }
            else if (*s == '\\') { name_esc[ai++] = '\\'; name_esc[ai++] = '\\'; }
            else { name_esc[ai++] = *s; }
        }
        name_esc[ai] = '\0';
        if (i > start_index) {
            off += (size_t)snprintf(buf + off, alloc - off, ",");
        }
        off += (size_t)snprintf(buf + off, alloc - off,
            "{\"tool\":\"%s\",\"args\":\"%s\",\"status\":\"%s\"}",
            name_esc, args_esc, tool_status_to_string(e->status));
    }
    off += (size_t)snprintf(buf + off, alloc - off, "]");
    return buf;
}

int tool_history_count(void) {
    return g_history_count;
}

void tool_history_reset(void) {
    g_history_count = 0;
    g_history_drop_count = 0;
    g_tool_history_len = 0;
    g_tool_history[0] = '\0';
}
