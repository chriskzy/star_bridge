#include "codex_request_parser.h"
#include "codex_tool_detector.h"
#include "config_manager.h"
#include "json_utils.h"
#include "cJSON.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Error helper                                                       */
/* ------------------------------------------------------------------ */

static void set_error(HarnessError *err, const char *code, const char *message) {
    if (!err) return;
    snprintf(err->code, sizeof(err->code), "%s", code ? code : "invalid_request");
    snprintf(err->message, sizeof(err->message), "%s", message ? message : "invalid request");
}

/* ------------------------------------------------------------------ */
/*  Text append helper                                                 */
/* ------------------------------------------------------------------ */

static bool append_owned(char **dest, size_t *len, size_t *cap, const char *text, HarnessError *err) {
    if (!dest || !len || !cap || !text) return true;
    size_t add = strlen(text);
    size_t limit = (size_t)(global_config.max_request_input_bytes > 0
        ? global_config.max_request_input_bytes
        : 8 * 1024 * 1024);
    if (*len + add > limit) {
        set_error(err, "request_input_too_large", "request input too large: exceeds max_request_input_bytes");
        return false;
    }
    if (*len + add + 1 > *cap) {
        size_t next = *cap ? *cap : 1024;
        while (next < *len + add + 1) {
            if (next > limit / 2) {
                next = limit + 1;
                break;
            }
            next *= 2;
        }
        char *grown = realloc(*dest, next);
        if (!grown) {
            set_error(err, "out_of_memory", "out of memory while parsing request input");
            return false;
        }
        *dest = grown;
        *cap = next;
    }
    memcpy(*dest + *len, text, add);
    *len += add;
    (*dest)[*len] = '\0';
    return true;
}

static char *dup_json_string(cJSON *obj, const char *key) {
    const char *value = json_get_string_ptr(obj, key);
    if (!value) return NULL;
    return strdup(value);
}

static bool item_append(char **dest, const char *text) {
    if (!dest || !text) return true;
    size_t cur = *dest ? strlen(*dest) : 0;
    size_t add = strlen(text);
    char *grown = realloc(*dest, cur + add + 1);
    if (!grown) return false;
    memcpy(grown + cur, text, add);
    grown[cur + add] = '\0';
    *dest = grown;
    return true;
}

static void input_item_free(InputItem *item) {
    if (!item) return;
    free(item->arguments);
    free(item->output);
    free(item->text);
    free(item->image_data);
    item->arguments = NULL;
    item->output = NULL;
    item->text = NULL;
    item->image_data = NULL;
}

void harness_request_free(HarnessRequest *req) {
    if (!req) return;
    free(req->normalized_input);
    free(req->instructions);
    req->normalized_input = NULL;
    req->instructions = NULL;
    for (int i = 0; i < req->input_item_count; i++) {
        input_item_free(&req->input_items[i]);
    }
    free(req->input_items);
    req->input_items = NULL;
    req->input_item_count = 0;
    req->input_item_capacity = 0;
}

/* ------------------------------------------------------------------ */
/*  Parse a content block within a message                             */
/* ------------------------------------------------------------------ */

static void parse_content_block(cJSON *block, InputItem *item) {
    if (!block || !item) return;
    const char *block_type = json_get_string_ptr(block, "type");
    if (!block_type) return;

    if (json_string_matches(block_type, "text", "input_text", NULL)) {
        const char *text_val = json_get_string_ptr(block, "text");
        if (text_val) {
            size_t cur = item->text ? strlen(item->text) : 0;
            if (cur > 0 && item->text[cur - 1] != '\n') item_append(&item->text, "\n");
            item_append(&item->text, text_val);
        }
    } else if (json_string_matches(block_type, "tool_call", "function_call", NULL)) {
        const char *name_val = json_get_string_ptr(block, "name");
        const char *args_val = json_get_string_ptr(block, "arguments");
        const char *call_id_val = json_get_string_ptr(block, "call_id");
        if (name_val)
            snprintf(item->name, sizeof(item->name), "%s", name_val);
        if (call_id_val)
            snprintf(item->call_id, sizeof(item->call_id), "%s", call_id_val);
        cJSON *args_item = cJSON_GetObjectItem(block, "arguments");
        if (args_item) {
            if (cJSON_IsString(args_item))
                item->arguments = args_val ? strdup(args_val) : NULL;
            else if (cJSON_IsObject(args_item)) {
                char *raw = cJSON_PrintUnformatted(args_item);
                if (raw) {
                    item->arguments = strdup(raw);
                    cJSON_free(raw);
                }
            }
        }
        item->type = INPUT_ITEM_TYPE_TOOL_CALL;
    } else if (json_string_matches(block_type, "reasoning_summary", "reasoning", NULL)) {
        const char *text_val = json_get_string_ptr(block, "text");
        if (text_val) {
            item_append(&item->text, text_val);
        }
        item->type = INPUT_ITEM_TYPE_REASONING;
    }
}

/* ------------------------------------------------------------------ */
/*  Parse a single input item from the input array                     */
/* ------------------------------------------------------------------ */

static bool parse_input_item(cJSON *item_obj, InputItem *out_item) {
    if (!item_obj || !out_item) return false;
    memset(out_item, 0, sizeof(*out_item));
    out_item->type = INPUT_ITEM_TYPE_TEXT; /* default */

    const char *item_type = json_get_string_ptr(item_obj, "type");

    /* If no type field, detect by presence of role/content fields */
    if (!item_type) {
        cJSON *role_item = cJSON_GetObjectItem(item_obj, "role");
        cJSON *content_item = cJSON_GetObjectItem(item_obj, "content");
        if (role_item || content_item) {
            item_type = "message";
        } else {
            item_type = "text";
        }
    }

    if (strcmp(item_type, "message") == 0) {
        out_item->type = INPUT_ITEM_TYPE_MESSAGE;
        const char *role_val = json_get_string_ptr(item_obj, "role");
        if (role_val)
            snprintf(out_item->role, sizeof(out_item->role), "%s", role_val);
        /* Parse content array within the message */
        cJSON *content_arr = cJSON_GetObjectItem(item_obj, "content");
        if (content_arr && cJSON_IsArray(content_arr)) {
            int count = cJSON_GetArraySize(content_arr);
            for (int i = 0; i < count; i++) {
                cJSON *block = cJSON_GetArrayItem(content_arr, i);
                parse_content_block(block, out_item);
            }
        }
        return true;
    }

    if (json_string_matches(item_type, "input_text", "text", NULL)) {
        out_item->type = INPUT_ITEM_TYPE_TEXT;
        const char *text_val = json_get_string_ptr(item_obj, "text");
        if (text_val)
            out_item->text = strdup(text_val);
        return true;
    }

    if (json_string_matches(item_type, "input_image", "image", NULL)) {
        out_item->type = INPUT_ITEM_TYPE_IMAGE;
        const char *url_val = json_get_string_ptr(item_obj, "image_url");
        if (url_val)
            snprintf(out_item->image_url, sizeof(out_item->image_url), "%s", url_val);
        const char *data_val = json_get_string_ptr(item_obj, "data");
        if (data_val) out_item->image_data = strdup(data_val);
        return true;
    }

    if (json_string_matches(item_type, "tool_call", "function_call", NULL)) {
        out_item->type = INPUT_ITEM_TYPE_TOOL_CALL;
        json_get_string(item_obj, "name", out_item->name, sizeof(out_item->name));
        json_get_string(item_obj, "call_id", out_item->call_id, sizeof(out_item->call_id));
        cJSON *args_item = cJSON_GetObjectItem(item_obj, "arguments");
        if (args_item) {
            if (cJSON_IsString(args_item))
                out_item->arguments = dup_json_string(item_obj, "arguments");
            else if (cJSON_IsObject(args_item)) {
                char *raw = cJSON_PrintUnformatted(args_item);
                if (raw) {
                    out_item->arguments = strdup(raw);
                    cJSON_free(raw);
                }
            }
        }
        return true;
    }

    if (json_string_matches(item_type, "tool_call_output", "function_call_output", NULL)) {
        out_item->type = INPUT_ITEM_TYPE_TOOL_OUTPUT;
        json_get_string(item_obj, "call_id", out_item->call_id, sizeof(out_item->call_id));
        out_item->output = dup_json_string(item_obj, "output");
        return true;
    }

    if (strcmp(item_type, "computer_call") == 0) {
        out_item->type = INPUT_ITEM_TYPE_COMPUTER;
        cJSON *action_item = cJSON_GetObjectItem(item_obj, "action");
        if (action_item) {
            char *raw = cJSON_PrintUnformatted(action_item);
            if (raw) {
                size_t need = strlen(raw) + 18;
                out_item->text = malloc(need);
                if (out_item->text) snprintf(out_item->text, need, "[computer_call: %s]", raw);
                cJSON_free(raw);
            }
        }
        return true;
    }

    if (json_string_matches(item_type, "reasoning", "reasoning_summary", NULL)) {
        out_item->type = INPUT_ITEM_TYPE_REASONING;
        const char *text_val = json_get_string_ptr(item_obj, "text");
        if (text_val)
            out_item->text = strdup(text_val);
        return true;
    }

    /* Unknown item type — store as raw JSON */
    {
        char *raw = cJSON_PrintUnformatted(item_obj);
        if (raw) {
            out_item->text = strdup(raw);
            cJSON_free(raw);
        }
        return true;
    }
}

/* ------------------------------------------------------------------ */
/*  Normalize request text using structured input items                */
/* ------------------------------------------------------------------ */

static bool normalize_text_item(InputItem *item, char **dest, size_t *len, size_t *cap, HarnessError *err) {
    size_t off = *len;
    switch (item->type) {
        case INPUT_ITEM_TYPE_TEXT:
            if (item->text && item->text[0]) {
                if (off > 0 && (*dest)[off - 1] != '\n')
                    if (!append_owned(dest, len, cap, "\n", err)) return false;
                if (!append_owned(dest, len, cap, item->text, err)) return false;
            }
            break;
        case INPUT_ITEM_TYPE_MESSAGE:
            if (off > 0 && (*dest)[off - 1] != '\n')
                if (!append_owned(dest, len, cap, "\n", err)) return false;
            if (item->role[0])
                { if (!append_owned(dest, len, cap, item->role, err)) return false; }
            else
                { if (!append_owned(dest, len, cap, "user", err)) return false; }
            if (!append_owned(dest, len, cap, ": ", err)) return false;
            if (!append_owned(dest, len, cap, item->text ? item->text : "", err)) return false;
            break;
        case INPUT_ITEM_TYPE_TOOL_CALL:
            if (off > 0 && (*dest)[off - 1] != '\n')
                if (!append_owned(dest, len, cap, "\n", err)) return false;
            if (!append_owned(dest, len, cap, "tool_call: ", err)) return false;
            if (item->name[0]) {
                if (!append_owned(dest, len, cap, item->name, err)) return false;
                if (!append_owned(dest, len, cap, "(", err)) return false;
                if (item->arguments && item->arguments[0])
                    if (!append_owned(dest, len, cap, item->arguments, err)) return false;
                if (!append_owned(dest, len, cap, ")", err)) return false;
            }
            if (item->call_id[0]) {
                if (!append_owned(dest, len, cap, " [call_id: ", err)) return false;
                if (!append_owned(dest, len, cap, item->call_id, err)) return false;
                if (!append_owned(dest, len, cap, "]", err)) return false;
            }
            break;
        case INPUT_ITEM_TYPE_TOOL_OUTPUT:
            if (off > 0 && (*dest)[off - 1] != '\n')
                if (!append_owned(dest, len, cap, "\n", err)) return false;
            if (!append_owned(dest, len, cap, "tool_output: ", err)) return false;
            if (item->output && item->output[0])
                if (!append_owned(dest, len, cap, item->output, err)) return false;
            if (item->call_id[0]) {
                if (!append_owned(dest, len, cap, " [call_id: ", err)) return false;
                if (!append_owned(dest, len, cap, item->call_id, err)) return false;
                if (!append_owned(dest, len, cap, "]", err)) return false;
            }
            break;
        case INPUT_ITEM_TYPE_IMAGE:
            if (off > 0 && (*dest)[off - 1] != '\n')
                if (!append_owned(dest, len, cap, "\n", err)) return false;
            if (!append_owned(dest, len, cap, "[image: ", err)) return false;
            if (item->image_url[0]) {
                if (!append_owned(dest, len, cap, item->image_url, err)) return false;
            } else if (item->image_data && item->image_data[0]) {
                if (!append_owned(dest, len, cap, "base64_data ", err)) return false;
                if (!append_owned(dest, len, cap, item->image_data, err)) return false;
                if (!append_owned(dest, len, cap, "...", err)) return false;
            } else {
                if (!append_owned(dest, len, cap, "unknown", err)) return false;
            }
            if (!append_owned(dest, len, cap, "]", err)) return false;
            break;
        case INPUT_ITEM_TYPE_COMPUTER:
            if (off > 0 && (*dest)[off - 1] != '\n')
                if (!append_owned(dest, len, cap, "\n", err)) return false;
            if (!append_owned(dest, len, cap, "computer_call: ", err)) return false;
            if (!append_owned(dest, len, cap, item->text ? item->text : "", err)) return false;
            break;
        case INPUT_ITEM_TYPE_REASONING:
            if (off > 0 && (*dest)[off - 1] != '\n')
                if (!append_owned(dest, len, cap, "\n", err)) return false;
            if (!append_owned(dest, len, cap, "reasoning: ", err)) return false;
            if (!append_owned(dest, len, cap, item->text ? item->text : "", err)) return false;
            break;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Parse input array into structured items and normalize text         */
/* ------------------------------------------------------------------ */

static bool ensure_input_item_capacity(HarnessRequest *out, int needed, HarnessError *err) {
    if (needed <= out->input_item_capacity) return true;
    int next = out->input_item_capacity ? out->input_item_capacity * 2 : 16;
    while (next < needed) next *= 2;
    InputItem *grown = realloc(out->input_items, (size_t)next * sizeof(InputItem));
    if (!grown) {
        set_error(err, "out_of_memory", "out of memory while parsing request items");
        return false;
    }
    memset(grown + out->input_item_capacity, 0, (size_t)(next - out->input_item_capacity) * sizeof(InputItem));
    out->input_items = grown;
    out->input_item_capacity = next;
    return true;
}

static bool parse_input_array(cJSON *input_arr, HarnessRequest *out, size_t *len, size_t *cap, HarnessError *err) {
    if (!cJSON_IsArray(input_arr)) return true;

    int count = cJSON_GetArraySize(input_arr);
    int parsed = 0;

    /* First pass: parse each item into structured storage */
    for (int i = 0; i < count; i++) {
        cJSON *item_obj = cJSON_GetArrayItem(input_arr, i);
        if (!item_obj) continue;
        if (!ensure_input_item_capacity(out, parsed + 1, err)) return false;
        InputItem *stored = &out->input_items[parsed];
        if (parse_input_item(item_obj, stored)) {
            /* Normalize text representation */
            if (!normalize_text_item(stored, &out->normalized_input, len, cap, err)) {
                /* Free any allocations inside the partially-populated item so
                   harness_request_free doesn't leak them later. */
                input_item_free(stored);
                return false;
            }
            parsed++;
        }
    }
    out->input_item_count = parsed;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Normalize request text using parsed JSON                            */
/* ------------------------------------------------------------------ */

static bool normalize_request_text(cJSON *body, HarnessRequest *out, HarnessError *err) {
    size_t len = 0;
    size_t cap = 0;
    if (!append_owned(&out->normalized_input, &len, &cap, "", err)) return false;

    /* Instructions first */
    if (out->instructions && out->instructions[0]) {
        if (!append_owned(&out->normalized_input, &len, &cap, out->instructions, err)) return false;
        if (!append_owned(&out->normalized_input, &len, &cap, "\n", err)) return false;
    }

    /* Previous response id */
    if (out->previous_response_id[0]) {
        if (!append_owned(&out->normalized_input, &len, &cap, "previous_response_id: ", err)) return false;
        if (!append_owned(&out->normalized_input, &len, &cap, out->previous_response_id, err)) return false;
        if (!append_owned(&out->normalized_input, &len, &cap, "\n", err)) return false;
    }

    /* Metadata */
    if (out->metadata_json[0]) {
        if (!append_owned(&out->normalized_input, &len, &cap, "metadata: ", err)) return false;
        if (!append_owned(&out->normalized_input, &len, &cap, out->metadata_json, err)) return false;
        if (!append_owned(&out->normalized_input, &len, &cap, "\n", err)) return false;
    }

    cJSON *input_item = cJSON_GetObjectItem(body, "input");
    if (!input_item) return out->normalized_input && out->normalized_input[0] != '\0';

    if (cJSON_IsString(input_item)) {
        if (!append_owned(&out->normalized_input, &len, &cap, input_item->valuestring, err)) return false;
        return out->normalized_input && out->normalized_input[0] != '\0';
    }

    if (cJSON_IsArray(input_item)) {
        /* Parse structured input items and normalize text */
        if (!parse_input_array(input_item, out, &len, &cap, err)) return false;
        return out->normalized_input && out->normalized_input[0] != '\0';
    }

    return out->normalized_input && out->normalized_input[0] != '\0';
}

/* ------------------------------------------------------------------ */
/*  Public functions                                                   */
/* ------------------------------------------------------------------ */

bool codex_parse_request(const char *body, HarnessRequest *out, HarnessError *err) {
    if (!body || !out) {
        set_error(err, "invalid_request", "missing request body");
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (err) memset(err, 0, sizeof(*err));
    snprintf(out->harness, sizeof(out->harness), "codex.responses");
    snprintf(out->model, sizeof(out->model), "star-bridge-ds4");

    /* Parse once with cJSON */
    cJSON *obj = cJSON_Parse(body);
    if (!obj || !cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        set_error(err, "invalid_json", "Request body must be a JSON object");
        return false;
    }

    /* Extract string fields */
    json_get_string(obj, "model", out->model, sizeof(out->model));
    out->instructions = dup_json_string(obj, "instructions");
    json_get_string(obj, "previous_response_id", out->previous_response_id, sizeof(out->previous_response_id));
    json_get_string(obj, "reasoning_effort", out->reasoning_effort, sizeof(out->reasoning_effort));

    /* Fallback: nested reasoning effort */
    if (!out->reasoning_effort[0]) {
        cJSON *reasoning_obj = cJSON_GetObjectItem(obj, "reasoning");
        if (reasoning_obj && cJSON_IsObject(reasoning_obj)) {
            json_get_string(reasoning_obj, "effort", out->reasoning_effort, sizeof(out->reasoning_effort));
        }
    }

    /* If still no reasoning_effort, fall back to config default (model_reasoning_effort).
     * Map values: "extra high" -> "high" since DeepSeek max is high. */
    if (!out->reasoning_effort[0]) {
        const char *default_effort = global_config.model_reasoning_effort;
        if (default_effort && default_effort[0]) {
            if (strcmp(default_effort, "extra high") == 0) {
                snprintf(out->reasoning_effort, sizeof(out->reasoning_effort), "high");
            } else {
                snprintf(out->reasoning_effort, sizeof(out->reasoning_effort), "%s", default_effort);
            }
        }
    }

    /* Extract boolean fields */
    cJSON *stream_item = cJSON_GetObjectItem(obj, "stream");
    if (stream_item && cJSON_IsBool(stream_item)) {
        out->stream = cJSON_IsTrue(stream_item);
    }

    cJSON *reset_item = cJSON_GetObjectItem(obj, "reset_session");
    if (reset_item && cJSON_IsBool(reset_item)) {
        out->reset_session = cJSON_IsTrue(reset_item);
    }

    /* Extract metadata object as raw JSON string */
    cJSON *metadata_obj = cJSON_GetObjectItem(obj, "metadata");
    if (metadata_obj && cJSON_IsObject(metadata_obj)) {
        char *raw = cJSON_PrintUnformatted(metadata_obj);
        if (raw) {
            snprintf(out->metadata_json, sizeof(out->metadata_json), "%s", raw);
            cJSON_free(raw);
        }
    }

    /* Detect tools and explicit tool calls */
    {
        char tool_name[128] = {0};
        out->has_tools = cJSON_GetObjectItem(obj, "tools") != NULL;

        if (!out->has_tools) {
            json_get_string(obj, "name", tool_name, sizeof(tool_name));
            if (tool_name[0]) {
                if (strcmp(tool_name, "google_search") == 0 || strcmp(tool_name, "browse_url") == 0) {
                    out->explicit_tool_call = true;
                    snprintf(out->tool_name, sizeof(out->tool_name), "%s", tool_name);
                    cJSON *args_obj = cJSON_GetObjectItem(obj, "arguments");
                    if (args_obj && cJSON_IsObject(args_obj)) {
                        char *raw = cJSON_PrintUnformatted(args_obj);
                        if (raw) {
                            snprintf(out->tool_argument, sizeof(out->tool_argument), "%s", raw);
                            cJSON_free(raw);
                        }
                    }
                }
            }
        }
    }

    /* Parse tools array into tool_defs_json */
    {
        cJSON *tools_arr = cJSON_GetObjectItem(obj, "tools");
        if (tools_arr && cJSON_IsArray(tools_arr)) {
            char *raw = cJSON_PrintUnformatted(tools_arr);
            if (raw) {
                snprintf(out->tool_defs_json, sizeof(out->tool_defs_json), "%s", raw);
                cJSON_free(raw);
                out->tool_count = cJSON_GetArraySize(tools_arr);
            }
        }
    }

    /* Parse tool_choice */
    {
        cJSON *tc_item = cJSON_GetObjectItem(obj, "tool_choice");
        if (tc_item) {
            char *raw = cJSON_PrintUnformatted(tc_item);
            if (raw) {
                snprintf(out->tool_choice_json, sizeof(out->tool_choice_json), "%s", raw);
                cJSON_free(raw);
            }
        }
    }

    if (out->explicit_tool_call && !cJSON_GetObjectItem(obj, "input")) {
        size_t len = 0;
        size_t cap = 0;
        if (!append_owned(&out->normalized_input, &len, &cap, "", err)) {
            cJSON_Delete(obj);
            harness_request_free(out);
            return false;
        }
    } else if (!normalize_request_text(obj, out, err)) {
        cJSON_Delete(obj);
        harness_request_free(out);
        return false;
    }
    cJSON_Delete(obj);
    return true;
}

bool codex_validate_request_schema(const char *body, char *error, size_t error_max) {
    if (!body || !*body) {
        snprintf(error, error_max, "empty request body");
        return false;
    }

    /* Parse with cJSON */
    cJSON *obj = cJSON_Parse(body);
    if (!obj || !cJSON_IsObject(obj)) {
        snprintf(error, error_max, "body must be a JSON object");
        cJSON_Delete(obj);
        return false;
    }

    /* Required: input */
    cJSON *input_item = cJSON_GetObjectItem(obj, "input");
    if (!input_item) {
        snprintf(error, error_max, "missing required field 'input'");
        cJSON_Delete(obj);
        return false;
    }

    /* Validate input type: string or array */
    if (!cJSON_IsString(input_item) && !cJSON_IsArray(input_item)) {
        snprintf(error, error_max, "field 'input' must be a string or array");
        cJSON_Delete(obj);
        return false;
    }

    /* stream must be boolean if present */
    cJSON *stream_item = cJSON_GetObjectItem(obj, "stream");
    if (stream_item && !cJSON_IsBool(stream_item)) {
        snprintf(error, error_max, "field 'stream' must be a boolean");
        cJSON_Delete(obj);
        return false;
    }

    /* reset_session must be boolean if present */
    cJSON *reset_item = cJSON_GetObjectItem(obj, "reset_session");
    if (reset_item && !cJSON_IsBool(reset_item)) {
        snprintf(error, error_max, "field 'reset_session' must be a boolean");
        cJSON_Delete(obj);
        return false;
    }

    cJSON_Delete(obj);
    return true;
}
