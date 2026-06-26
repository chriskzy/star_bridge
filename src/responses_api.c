#include "responses_api.h"
#include "json_utils.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void resp_init(ResponseObject *r, const char *model) {
    memset(r, 0, sizeof(*r));
    snprintf(r->object, sizeof(r->object), "response");
    snprintf(r->model, sizeof(r->model), "%s", model ? model : "star-bridge-ds4");
    r->created_at = (int64_t)time(NULL);
    r->status = RESP_STATUS_COMPLETED;
    r->output_count = 0;
    r->prompt_tokens = 0;
    r->completion_tokens = 0;
    r->total_tokens = 0;
    r->input_text_tokens = 0;
    r->input_cached_tokens = 0;
    r->input_audio_tokens = 0;
    r->output_text_tokens = 0;
    r->output_reasoning_tokens = 0;
    snprintf(r->finish_reason, sizeof(r->finish_reason), "stop");
}

void resp_set_id(ResponseObject *r, const char *id) {
    if (r && id) snprintf(r->id, sizeof(r->id), "%s", id);
}

void resp_add_text(ResponseObject *r, const char *text) {
    if (!r || !text) return;
    if (r->output_count >= MAX_OUTPUT_ITEMS) return;
    OutputItem *item = &r->output[r->output_count++];
    item->type = OUTPUT_TYPE_TEXT;
    snprintf(item->data.text, sizeof(item->data.text), "%s", text);
}

void resp_add_tool_call(ResponseObject *r, const char *call_id, const char *name, const char *arguments) {
    if (!r) return;
    if (r->output_count >= MAX_OUTPUT_ITEMS) return;
    OutputItem *item = &r->output[r->output_count++];
    item->type = OUTPUT_TYPE_TOOL_CALL;
    snprintf(item->data.tool_call.info.call_id, sizeof(item->data.tool_call.info.call_id), "%s", call_id ? call_id : "");
    snprintf(item->data.tool_call.info.type, sizeof(item->data.tool_call.info.type), "function");
    snprintf(item->data.tool_call.info.function.name, sizeof(item->data.tool_call.info.function.name), "%s", name ? name : "");
    snprintf(item->data.tool_call.info.function.arguments, sizeof(item->data.tool_call.info.function.arguments), "%s", arguments ? arguments : "{}");
}

void resp_set_error(ResponseObject *r, const char *msg) {
    if (r && msg) {
        snprintf(r->error_msg, sizeof(r->error_msg), "%s", msg);
        r->status = RESP_STATUS_FAILED;
    }
}

void resp_set_finish_reason(ResponseObject *r, const char *reason) {
    if (r && reason) snprintf(r->finish_reason, sizeof(r->finish_reason), "%s", reason);
}

/* JSON escaping helper using shared json_utils */
static void json_escape_str(const char *src, char *dest, size_t max_len) {
    json_escape(src ? src : "", dest, max_len);
}

static size_t json_escape_into(const char *src, char *buf, size_t max_len, size_t offset) {
    char escaped[MAX_TEXT_OUTPUT];
    json_escape_str(src, escaped, sizeof(escaped));
    size_t len = strlen(escaped);
    if (offset + len + 1 > max_len) len = max_len - offset - 1;
    memcpy(buf + offset, escaped, len);
    buf[offset + len] = '\0';
    return offset + len;
}

/* Serialize a ResponseObject to JSON */
size_t resp_to_json(const ResponseObject *r, char *buf, size_t max_len) {
    if (!r || !buf || max_len == 0) return 0;
    size_t off = 0;

    /* Open object */
    off += snprintf(buf + off, max_len - off, "{\n");

    off += snprintf(buf + off, max_len - off, "\"id\":\"%s\",\n", r->id[0] ? r->id : "resp-unknown");
    off += snprintf(buf + off, max_len - off, "\"object\":\"%s\",\n", r->object);
    off += snprintf(buf + off, max_len - off, "\"created_at\":%lld,\n", (long long)r->created_at);

    const char *status_str = "completed";
    switch (r->status) {
        case RESP_STATUS_COMPLETED:   status_str = "completed"; break;
        case RESP_STATUS_FAILED:      status_str = "failed"; break;
        case RESP_STATUS_IN_PROGRESS: status_str = "in_progress"; break;
        case RESP_STATUS_INCOMPLETE:  status_str = "incomplete"; break;
    }
    off += snprintf(buf + off, max_len - off, "\"status\":\"%s\",\n", status_str);
    off += snprintf(buf + off, max_len - off, "\"model\":\"%s\",\n", r->model);

    /* Output array */
    off += snprintf(buf + off, max_len - off, "\"output\":[\n");
    for (int i = 0; i < r->output_count; i++) {
        const OutputItem *item = &r->output[i];
        if (i > 0) off += snprintf(buf + off, max_len - off, ",\n");
        off += snprintf(buf + off, max_len - off, "{\"type\":");
        switch (item->type) {
            case OUTPUT_TYPE_TEXT: {
                off += snprintf(buf + off, max_len - off, "\"text\",\"text\":\"");
                off = json_escape_into(item->data.text, buf, max_len, off);
                off += snprintf(buf + off, max_len - off, "\",\"annotations\":[]");
                break;
            }
            case OUTPUT_TYPE_TOOL_CALL: {
                off += snprintf(buf + off, max_len - off, "\"tool_call\",\"tool_call\":{\"id\":\"%s\",\"type\":\"%s\",\"function\":{\"name\":\"%s\",\"arguments\":\"",
                    item->data.tool_call.info.call_id,
                    item->data.tool_call.info.type,
                    item->data.tool_call.info.function.name);
                off = json_escape_into(item->data.tool_call.info.function.arguments, buf, max_len, off);
                off += snprintf(buf + off, max_len - off, "\"}}}");
                break;
            }
            default:
                off += snprintf(buf + off, max_len - off, "\"error\",\"error\":\"unknown_output_type\"");
                break;
        }
        off += snprintf(buf + off, max_len - off, "}");
    }
    off += snprintf(buf + off, max_len - off, "],\n");

    /* Error */
    if (r->error_msg[0]) {
        off += snprintf(buf + off, max_len - off, "\"error\":{\"message\":\"");
        off = json_escape_into(r->error_msg, buf, max_len, off);
        off += snprintf(buf + off, max_len - off, "\"},\n");
    } else {
        off += snprintf(buf + off, max_len - off, "\"error\":null,\n");
    }

    /* Finish reason */
    off += snprintf(buf + off, max_len - off, "\"finish_reason\":\"%s\"", r->finish_reason);

    /* Incomplete details — set when output was truncated due to buffer limits */
    if (r->incomplete_details[0]) {
        char escaped_inc[256];
        json_escape_str(r->incomplete_details, escaped_inc, sizeof(escaped_inc));
        off += snprintf(buf + off, max_len - off, ",\n\"incomplete_details\":\"%s\"", escaped_inc);
    }

    /* Usage - detailed if available, fall back to minimal */
    if (r->total_tokens > 0 || r->prompt_tokens > 0 || r->completion_tokens > 0) {
        char usage_buf[2048];
        size_t usage_len = resp_usage_to_json(r, usage_buf, sizeof(usage_buf));
        off += snprintf(buf + off, max_len - off, ",\n");
        size_t copy = max_len - off > usage_len ? usage_len : max_len - off;
        memcpy(buf + off, usage_buf, copy);
        off += copy;
    }

    off += snprintf(buf + off, max_len - off, "\n}\n");
    return off;
}

/* Serialize a single output item (for streaming or partial output) */
size_t resp_item_to_json(const OutputItem *item, char *buf, size_t max_len) {
    if (!item || !buf || max_len == 0) return 0;
    size_t off = 0;
    off += snprintf(buf + off, max_len - off, "{\"type\":");
    switch (item->type) {
        case OUTPUT_TYPE_TEXT:
            off += snprintf(buf + off, max_len - off, "\"text\",\"text\":\"");
            off = json_escape_into(item->data.text, buf, max_len, off);
            off += snprintf(buf + off, max_len - off, "\",\"annotations\":[]}");
            break;
        case OUTPUT_TYPE_TOOL_CALL:
            off += snprintf(buf + off, max_len - off, "\"tool_call\",\"tool_call\":{\"id\":\"%s\",\"type\":\"function\",\"function\":{\"name\":\"%s\",\"arguments\":\"",
                item->data.tool_call.info.call_id,
                item->data.tool_call.info.function.name);
            off = json_escape_into(item->data.tool_call.info.function.arguments, buf, max_len, off);
            off += snprintf(buf + off, max_len - off, "\"}}}");
            break;
        default:
            off += snprintf(buf + off, max_len - off, "\"error\",\"error\":\"unknown\"}");
            break;
    }
    return off;
}

/* Serialize a streaming delta event (SSE data frame) */
size_t resp_stream_delta(const ResponseObject *r, const char *delta_text, char *buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    size_t off = 0;
    off += snprintf(buf + off, max_len - off, "{\"id\":\"%s\",\"object\":\"response\",\"type\":\"response.output_item.delta\",\"status\":\"in_progress\",\"model\":\"%s\",\"delta\":\"",
        r && r->id[0] ? r->id : "resp-stream",
        r && r->model[0] ? r->model : "star-bridge-ds4");
    off = json_escape_into(delta_text, buf, max_len, off);
    off += snprintf(buf + off, max_len - off, "\"}\n");
    return off;
}

/* Serialize error response */
size_t resp_error_to_json(const char *msg, char *buf, size_t max_len) {
    if (!buf || max_len == 0) return 0;
    char escaped[MAX_ERROR_MSG];
    json_escape_str(msg, escaped, sizeof(escaped));
    return (size_t)snprintf(buf, max_len,
        "{\"id\":\"resp-error\",\"object\":\"response\",\"status\":\"failed\",\"error\":{\"message\":\"%s\"}}\n",
        escaped);
}

/* ---- Usage normalization ---- */

/**
 * resp_normalize_usage - Parse native agent usage fields into ResponseObject.
 *
 * Maps:
 *   prompt_tokens   → prompt_tokens, input_text_tokens
 *   completion_tokens → completion_tokens, output_text_tokens
 *   total_tokens     = prompt_tokens + completion_tokens
 *   usage_json fields:
 *     "cache_hit"    → input_cached_tokens
 *     "cache_create" → input_cached_tokens (additive)
 *     "reasoning_tokens" → output_reasoning_tokens
 */
void resp_normalize_usage(ResponseObject *r, int prompt_tokens, int completion_tokens, const char *usage_json) {
    if (!r) return;

    /* Base token counts */
    r->prompt_tokens = prompt_tokens;
    r->completion_tokens = completion_tokens;
    r->total_tokens = prompt_tokens + completion_tokens;

    /* Default details: all prompt goes to input_text, all completion goes to output_text */
    r->input_text_tokens = prompt_tokens;
    r->input_cached_tokens = 0;
    r->input_audio_tokens = 0;
    r->output_text_tokens = completion_tokens;
    r->output_reasoning_tokens = 0;

    if (!usage_json || usage_json[0] == '\0') return;

    /* Parse usage_json for cache_hit, cache_create, reasoning_tokens */
    /* We use a simple manual parse since cJSON might not be available in this translation unit.
     * But we can include cJSON via json_utils. Actually json_utils.h wraps cJSON. */
    /* Use cJSON to parse the usage object */
    /* Include cJSON header - we have it vendored */
    cJSON *usage = cJSON_Parse(usage_json);
    if (!usage) return;

    /* Extract cache_hit → input_cached_tokens */
    cJSON *cache_hit = cJSON_GetObjectItem(usage, "cache_hit");
    if (cache_hit && cJSON_IsNumber(cache_hit)) {
        r->input_cached_tokens += (int)cJSON_GetNumberValue(cache_hit);
    }

    /* Extract cache_create → input_cached_tokens */
    cJSON *cache_create = cJSON_GetObjectItem(usage, "cache_create");
    if (cache_create && cJSON_IsNumber(cache_create)) {
        r->input_cached_tokens += (int)cJSON_GetNumberValue(cache_create);
    }

    /* Extract reasoning_tokens → output_reasoning_tokens */
    cJSON *reasoning = cJSON_GetObjectItem(usage, "reasoning_tokens");
    if (reasoning && cJSON_IsNumber(reasoning)) {
        r->output_reasoning_tokens = (int)cJSON_GetNumberValue(reasoning);
        /* Cap reasoning_tokens at output_text_tokens (which is completion_tokens initially) */
        if (r->output_reasoning_tokens > r->output_text_tokens) {
            r->output_reasoning_tokens = r->output_text_tokens;
        }
        /* Subtract reasoning tokens from output_text_tokens so details sum to completion_tokens */
        r->output_text_tokens = r->completion_tokens - r->output_reasoning_tokens;
    }

    cJSON_Delete(usage);
}

/**
 * resp_set_usage - Directly set usage fields from known values.
 */
void resp_set_usage(ResponseObject *r, int input_tokens, int output_tokens,
                    int input_text, int input_cached, int input_audio,
                    int output_text, int output_reasoning) {
    if (!r) return;
    r->prompt_tokens = input_tokens;
    r->completion_tokens = output_tokens;
    r->total_tokens = input_tokens + output_tokens;
    r->input_text_tokens = input_text;
    r->input_cached_tokens = input_cached;
    r->input_audio_tokens = input_audio;
    r->output_text_tokens = output_text;
    r->output_reasoning_tokens = output_reasoning;
}

/**
 * resp_usage_to_json - Serialize usage as a JSON fragment.
 *
 * Output format:
 *   "usage":{"input_tokens":N,"output_tokens":N,"total_tokens":N,
 *            "input_tokens_details":{"text_tokens":N,"cached_tokens":N,"audio_tokens":N},
 *            "output_tokens_details":{"text_tokens":N,"reasoning_tokens":N}}
 *
 * Fields that are 0 are omitted from details objects, except that
 * text_tokens is always included since it's the base.
 */
size_t resp_usage_to_json(const ResponseObject *r, char *buf, size_t max_len) {
    if (!r || !buf || max_len == 0) return 0;

    int pt = r->prompt_tokens;
    int ct = r->completion_tokens;
    int tt = r->total_tokens;
    int it = r->input_text_tokens;
    int ic = r->input_cached_tokens;
    int ia = r->input_audio_tokens;
    int ot = r->output_text_tokens;
    int ort = r->output_reasoning_tokens;

    /* Use default text if totals are unset */
    if (tt == 0 && pt == 0 && ct == 0) return 0;
    if (pt == 0) pt = tt > ct ? tt - ct : 0;
    if (ct == 0) ct = tt > pt ? tt - pt : 0;
    if (tt == 0) tt = pt + ct;

    size_t off = 0;

    /* Open usage object */
    off += snprintf(buf + off, max_len - off,
        "\"usage\":{\"input_tokens\":%d,\"output_tokens\":%d,\"total_tokens\":%d",
        pt, ct, tt);

    /* Input details */
    off += snprintf(buf + off, max_len - off, ",\"input_tokens_details\":{\"text_tokens\":%d", it);
    if (ic > 0) off += snprintf(buf + off, max_len - off, ",\"cached_tokens\":%d", ic);
    if (ia > 0) off += snprintf(buf + off, max_len - off, ",\"audio_tokens\":%d", ia);
    off += snprintf(buf + off, max_len - off, "}");

    /* Output details */
    off += snprintf(buf + off, max_len - off, ",\"output_tokens_details\":{\"text_tokens\":%d", ot);
    if (ort > 0) off += snprintf(buf + off, max_len - off, ",\"reasoning_tokens\":%d", ort);
    off += snprintf(buf + off, max_len - off, "}");

    off += snprintf(buf + off, max_len - off, "}");
    return off;
}
