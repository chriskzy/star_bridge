#include "codex_tool_normalizer.h"
#include "json_utils.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Text append helper (local copy for independent compilation)        */
/* ------------------------------------------------------------------ */

static size_t append_text(char *dest, size_t max_len, size_t off, const char *text) {
    if (!dest || !text || off >= max_len) return off;
    size_t len = strlen(text);
    if (len > max_len - off - 1) len = max_len - off - 1;
    memcpy(dest + off, text, len);
    off += len;
    dest[off] = '\0';
    return off;
}

/* ------------------------------------------------------------------ */
/*  Tool name normalization map                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *type;      /* tool type field from Responses API */
    const char *name;      /* tool name from Responses API */
    const char *normalized;/* bridge-owned stable name */
} ToolNameMap;

static const ToolNameMap s_tool_name_map[] = {
    /* Web search */
    { "function",   "web_search",       "google_search" },
    { "web_search", "web_search",       "google_search" },
    { "function",   "google_search",    "google_search" },

    /* Computer use */
    { "computer",   "computer",         "computer_call" },
    { "function",   "computer_call",    "computer_call" },
    { "computer",   "computer_call",    "computer_call" },

    /* Apply patch */
    { "function",   "apply_patch",      "apply_patch" },
    { "function",   "patch",            "apply_patch" },
    { "function",   "edit_file",        "apply_patch" },

    /* Shell / local shell */
    { "function",   "shell",            "execute_shell" },
    { "function",   "local_shell",      "execute_shell" },
    { "function",   "run_shell",        "execute_shell" },
    { "function",   "execute_shell",    "execute_shell" },
    { "function",   "bash",             "execute_shell" },
    { "function",   "sh",               "execute_shell" },

    /* Browse URL */
    { "function",   "browse_url",       "browse_url" },
    { "function",   "browse",           "browse_url" },

    /* Text editor */
    { "function",   "text_editor",      "text_editor" },

    /* MCP-like function tools - keep their original name */
    { "function",   "list_tools",       "list_tools" },
    { "function",   "mcp_list_tools",   "list_tools" },
    { "function",   "mcp_call",         "mcp_call" },

    /* End marker */
    { NULL, NULL, NULL }
};

static const char *lookup_normalized_name(const char *type, const char *name) {
    if (!type || !name) return name;
    for (int i = 0; s_tool_name_map[i].type != NULL; i++) {
        if (strcmp(s_tool_name_map[i].type, type) == 0 &&
            strcmp(s_tool_name_map[i].name, name) == 0) {
            return s_tool_name_map[i].normalized;
        }
    }
    return name; /* No mapping — keep original */
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

const char *codex_normalize_tool_name(const char *type, const char *name) {
    return lookup_normalized_name(type, name);
}

int codex_normalize_tools(cJSON *body, NormalizedToolDef *tools, int max_tools) {
    if (!body || !tools || max_tools <= 0) return 0;

    cJSON *tools_arr = cJSON_GetObjectItem(body, "tools");
    if (!tools_arr || !cJSON_IsArray(tools_arr)) return 0;

    int count = cJSON_GetArraySize(tools_arr);
    if (count > max_tools) count = max_tools;
    int parsed = 0;

    for (int i = 0; i < count; i++) {
        cJSON *tool_obj = cJSON_GetArrayItem(tools_arr, i);
        if (!tool_obj) continue;

        NormalizedToolDef *def = &tools[parsed];
        memset(def, 0, sizeof(*def));

        /* Extract type */
        cJSON *type_item = cJSON_GetObjectItem(tool_obj, "type");
        const char *type_str = type_item && cJSON_IsString(type_item)
                                ? type_item->valuestring : "function";
        snprintf(def->type, sizeof(def->type), "%s", type_str);

        /* Extract name */
        cJSON *name_item = cJSON_GetObjectItem(tool_obj, "function");
        if (name_item && cJSON_IsObject(name_item)) {
            cJSON *fname_item = cJSON_GetObjectItem(name_item, "name");
            if (fname_item && cJSON_IsString(fname_item))
                snprintf(def->original_name, sizeof(def->original_name), "%s",
                         fname_item->valuestring);
        }
        /* Fallback: top-level name */
        if (!def->original_name[0]) {
            cJSON *top_name = cJSON_GetObjectItem(tool_obj, "name");
            if (top_name && cJSON_IsString(top_name))
                snprintf(def->original_name, sizeof(def->original_name), "%s",
                         top_name->valuestring);
        }

        /* Normalize name */
        const char *norm = codex_normalize_tool_name(def->type, def->original_name);
        snprintf(def->normalized_name, sizeof(def->normalized_name), "%s", norm);
        def->is_bridge_owned = (strcmp(norm, def->original_name) != 0) ||
                                json_string_matches(def->type, "computer", "web_search", NULL);

        /* Extract description */
        cJSON *desc_item = cJSON_GetObjectItem(tool_obj, "description");
        if (desc_item && cJSON_IsString(desc_item))
            snprintf(def->description, sizeof(def->description), "%s",
                     desc_item->valuestring);
        /* Fallback: description inside function object */
        if (!def->description[0] && name_item && cJSON_IsObject(name_item)) {
            cJSON *fdesc = cJSON_GetObjectItem(name_item, "description");
            if (fdesc && cJSON_IsString(fdesc))
                snprintf(def->description, sizeof(def->description), "%s",
                         fdesc->valuestring);
        }

        /* Extract input schema */
        cJSON *schema_item = cJSON_GetObjectItem(tool_obj, "input_schema");
        if (!schema_item && name_item && cJSON_IsObject(name_item)) {
            schema_item = cJSON_GetObjectItem(name_item, "parameters");
        }
        if (schema_item) {
            char *raw = cJSON_PrintUnformatted(schema_item);
            if (raw) {
                snprintf(def->input_schema_json, sizeof(def->input_schema_json),
                         "%s", raw);
                cJSON_free(raw);
            }
        }
        /* Fallback: if no schema, provide a default */
        if (!def->input_schema_json[0]) {
            snprintf(def->input_schema_json, sizeof(def->input_schema_json),
                     "{\"type\":\"object\",\"properties\":{}}");
        }

        parsed++;
    }

    return parsed;
}

ToolChoiceSpec codex_resolve_tool_choice(cJSON *body) {
    ToolChoiceSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.mode = TOOL_CHOICE_AUTO; /* default */

    cJSON *tc_item = cJSON_GetObjectItem(body, "tool_choice");
    if (!tc_item) return spec;

    if (cJSON_IsString(tc_item)) {
        const char *val = tc_item->valuestring;
        if (strcmp(val, "none") == 0) {
            spec.mode = TOOL_CHOICE_NONE;
        } else if (strcmp(val, "auto") == 0) {
            spec.mode = TOOL_CHOICE_AUTO;
        } else if (strcmp(val, "required") == 0 || strcmp(val, "any") == 0) {
            spec.mode = TOOL_CHOICE_REQUIRED;
        }
        return spec;
    }

    if (cJSON_IsObject(tc_item)) {
        /* Named tool choice: {"type": "function", "function": {"name": "..."}} */
        cJSON *func_obj = cJSON_GetObjectItem(tc_item, "function");
        if (func_obj && cJSON_IsObject(func_obj)) {
            cJSON *name_item = cJSON_GetObjectItem(func_obj, "name");
            if (name_item && cJSON_IsString(name_item)) {
                spec.mode = TOOL_CHOICE_NAMED;
                snprintf(spec.named_tool, sizeof(spec.named_tool), "%s",
                         name_item->valuestring);
                return spec;
            }
        }
        /* Fallback: direct name field */
        cJSON *name_item = cJSON_GetObjectItem(tc_item, "name");
        if (name_item && cJSON_IsString(name_item)) {
            spec.mode = TOOL_CHOICE_NAMED;
            snprintf(spec.named_tool, sizeof(spec.named_tool), "%s",
                     name_item->valuestring);
            return spec;
        }
        /* Fallback: type-based */
        cJSON *type_item = cJSON_GetObjectItem(tc_item, "type");
        if (type_item && cJSON_IsString(type_item)) {
            const char *tv = type_item->valuestring;
            if (strcmp(tv, "none") == 0) spec.mode = TOOL_CHOICE_NONE;
            else if (strcmp(tv, "auto") == 0) spec.mode = TOOL_CHOICE_AUTO;
            else if (strcmp(tv, "required") == 0) spec.mode = TOOL_CHOICE_REQUIRED;
        }
    }

    return spec;
}

const NormalizedToolDef *codex_find_normalized_tool(const NormalizedToolDef *tools,
                                                     int count, const char *name) {
    if (!tools || !name) return NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(tools[i].normalized_name, name) == 0 ||
            strcmp(tools[i].original_name, name) == 0)
            return &tools[i];
    }
    return NULL;
}

void codex_build_tool_prompt(const NormalizedToolDef *tools, int count,
                             char *dest, size_t max_len) {
    if (!dest || max_len == 0) return;
    dest[0] = '\0';
    size_t off = 0;

    for (int i = 0; i < count; i++) {
        const NormalizedToolDef *def = &tools[i];
        size_t needed = off + strlen("\n- ") + strlen(def->normalized_name) + 2;
        if (needed >= max_len) break;

        off = append_text(dest, max_len, off, "\n- ");
        off = append_text(dest, max_len, off, def->normalized_name);

        if (def->description[0]) {
            needed = off + strlen(": ") + strlen(def->description) + 1;
            if (needed < max_len) {
                off = append_text(dest, max_len, off, ": ");
                off = append_text(dest, max_len, off, def->description);
            }
        }
    }

    if (off > 0) {
        off = append_text(dest, max_len, off, "\n");
    }
}
