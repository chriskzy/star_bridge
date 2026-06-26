#include "codex_stream_events.h"
#include "json_utils.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
static void json_escape_event_text(const char *src, char *dest, size_t max_len) {
    json_escape(src ? src : "", dest, max_len);
}

/* ------------------------------------------------------------------ */
/*  Public stream event functions                                     */
/* ------------------------------------------------------------------ */

bool codex_stream_created(const char *id, const char *model, HarnessStreamEvent *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.created");
    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.created\",\"response\":{\"id\":\"%s\",\"object\":\"response\",\"created_at\":%lld,\"status\":\"in_progress\",\"error\":null,\"incomplete_details\":null,\"instructions\":null,\"max_output_tokens\":null,\"model\":\"%s\",\"output\":[],\"parallel_tool_calls\":true,\"previous_response_id\":null,\"reasoning\":null,\"store\":false,\"temperature\":1,\"text\":{\"format\":{\"type\":\"text\"}},\"tool_choice\":\"auto\",\"tools\":[],\"top_p\":1,\"truncation\":\"disabled\",\"usage\":null,\"user\":null,\"metadata\":{}},\"sequence_number\":0}",
             id && id[0] ? id : "resp-stream",
             (long long)time(NULL),
             model && model[0] ? model : "star-bridge-ds4");
    return true;
}

bool codex_stream_delta(const char *id, const char *model, const char *delta_text, HarnessStreamEvent *out) {
    return codex_stream_output_text_delta(id, model, delta_text, 0, 0, 1, out);
}

bool codex_stream_completed(const char *id, const char *model, const char *final_text, int sequence_number, int prompt_tokens, int completion_tokens, const char *usage_json, const char *incomplete_details, HarnessStreamEvent *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.completed");
    char escaped[131072];
    json_escape_event_text(final_text ? final_text : "", escaped, sizeof(escaped));

    /* Build usage JSON if tokens are provided */
    char usage_buf[512] = {0};
    if (prompt_tokens > 0 || completion_tokens > 0) {
        int cached = 0, reasoning = 0;
        if (usage_json && usage_json[0]) {
            /* Simple parse for cache_hit and reasoning_tokens */
            const char *p = usage_json;
            /* cache_hit */
            const char *ch = strstr(p, "\"cache_hit\"");
            if (ch) {
                const char *val = ch + strlen("\"cache_hit\"");
                while (*val && (*val == ':' || *val == ' ' || *val == '\t' || *val == '\n' || *val == '\r')) val++;
                if (*val >= '0' && *val <= '9') cached = atoi(val);
            }
            /* cache_create */
            const char *cc = strstr(p, "\"cache_create\"");
            if (cc) {
                const char *val = cc + strlen("\"cache_create\"");
                while (*val && (*val == ':' || *val == ' ' || *val == '\t' || *val == '\n' || *val == '\r')) val++;
                if (*val >= '0' && *val <= '9') cached += atoi(val);
            }
            /* reasoning_tokens */
            const char *rt = strstr(p, "\"reasoning_tokens\"");
            if (rt) {
                const char *val = rt + strlen("\"reasoning_tokens\"");
                while (*val && (*val == ':' || *val == ' ' || *val == '\t' || *val == '\n' || *val == '\r')) val++;
                if (*val >= '0' && *val <= '9') reasoning = atoi(val);
            }
        }
        int total = prompt_tokens + completion_tokens;
        snprintf(usage_buf, sizeof(usage_buf),
                 "\"usage\":{\"input_tokens\":%d,\"output_tokens\":%d,\"total_tokens\":%d,"
                 "\"input_tokens_details\":{\"text_tokens\":%d,\"cached_tokens\":%d},"
                 "\"output_tokens_details\":{\"text_tokens\":%d,\"reasoning_tokens\":%d}}",
                 prompt_tokens, completion_tokens, total,
                 prompt_tokens, cached,
                 completion_tokens, reasoning);
    }

    /* Use incomplete_details if provided, otherwise null. Allocate dynamically
       to avoid truncation when escaped text is longer than small stack buffer. */
    char *incomplete_buf = NULL;
    if (incomplete_details && incomplete_details[0]) {
        size_t esc_capacity = strlen(incomplete_details) * 2 + 32;
        char *escaped_incomplete = malloc(esc_capacity);
        if (!escaped_incomplete) return false;
        json_escape_event_text(incomplete_details, escaped_incomplete, esc_capacity);
        size_t needed = strlen(escaped_incomplete) + 32;
        incomplete_buf = malloc(needed);
        if (!incomplete_buf) {
            free(escaped_incomplete);
            return false;
        }
        snprintf(incomplete_buf, needed, "\"incomplete_details\":\"%s\"", escaped_incomplete);
        free(escaped_incomplete);
    } else {
        incomplete_buf = strdup("\"incomplete_details\":null");
        if (!incomplete_buf) return false;
    }

    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.completed\",\"response\":{\"id\":\"%s\",\"object\":\"response\",\"created_at\":%lld,\"status\":\"completed\",\"error\":null,%s,\"instructions\":null,\"max_output_tokens\":null,\"model\":\"%s\",\"output\":[{\"id\":\"msg_0\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"%s\",\"annotations\":[]}]}],\"parallel_tool_calls\":true,\"previous_response_id\":null,\"reasoning\":null,\"store\":false,\"temperature\":1,\"text\":{\"format\":{\"type\":\"text\"}},\"tool_choice\":\"auto\",\"tools\":[],\"top_p\":1,\"truncation\":\"disabled\"%s%s%s,\"user\":null,\"metadata\":{}},\"sequence_number\":%d}",
             id && id[0] ? id : "resp-stream",
             (long long)time(NULL),
             incomplete_buf,
             model && model[0] ? model : "star-bridge-ds4",
             escaped,
             usage_buf[0] ? "," : "", usage_buf[0] ? usage_buf : "", usage_buf[0] ? "" : "",
             sequence_number);
    free(incomplete_buf);
    return true;
}

bool codex_stream_output_item_added(const char *id, int output_index, int sequence_number, HarnessStreamEvent *out) {
    if (!out) return false;
    (void)id;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.output_item.added");
    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.output_item.added\",\"output_index\":%d,\"item\":{\"id\":\"msg_%d\",\"type\":\"message\",\"status\":\"in_progress\",\"role\":\"assistant\",\"content\":[]},\"sequence_number\":%d}",
             output_index,
             output_index,
             sequence_number);
    return true;
}

bool codex_stream_content_part_added(const char *id, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out) {
    if (!out) return false;
    (void)id;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.content_part.added");
    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.content_part.added\",\"item_id\":\"msg_%d\",\"output_index\":%d,\"content_index\":%d,\"part\":{\"type\":\"output_text\",\"text\":\"\",\"annotations\":[]},\"sequence_number\":%d}",
             output_index,
             output_index,
             content_index,
             sequence_number);
    return true;
}

bool codex_stream_output_text_delta(const char *id, const char *model, const char *delta_text, int item_index, int content_index, int sequence_number, HarnessStreamEvent *out) {
    if (!out) return false;
    (void)id;
    (void)model;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.output_text.delta");
    char escaped[131072];
    json_escape_event_text(delta_text ? delta_text : "", escaped, sizeof(escaped));
    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.output_text.delta\",\"item_id\":\"msg_%d\",\"output_index\":%d,\"content_index\":%d,\"delta\":\"%s\",\"sequence_number\":%d}",
             item_index,
             item_index,
             content_index,
             escaped,
             sequence_number);
    return true;
}

bool codex_stream_output_text_done(const char *id, const char *text, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out) {
    if (!out) return false;
    (void)id;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.output_text.done");
    char escaped[131072];
    json_escape_event_text(text ? text : "", escaped, sizeof(escaped));
    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.output_text.done\",\"item_id\":\"msg_%d\",\"output_index\":%d,\"content_index\":%d,\"text\":\"%s\",\"logprobs\":[],\"sequence_number\":%d}",
             output_index,
             output_index,
             content_index,
             escaped,
             sequence_number);
    return true;
}

bool codex_stream_content_part_done(const char *id, const char *text, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out) {
    if (!out) return false;
    (void)id;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.content_part.done");
    char escaped[131072];
    json_escape_event_text(text ? text : "", escaped, sizeof(escaped));
    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.content_part.done\",\"item_id\":\"msg_%d\",\"output_index\":%d,\"content_index\":%d,\"part\":{\"type\":\"output_text\",\"text\":\"%s\",\"annotations\":[]},\"sequence_number\":%d}",
             output_index,
             output_index,
             content_index,
             escaped,
             sequence_number);
    return true;
}

bool codex_stream_output_item_done(const char *id, const char *text, int output_index, int sequence_number, HarnessStreamEvent *out) {
    if (!out) return false;
    (void)id;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.output_item.done");
    char escaped[131072];
    json_escape_event_text(text ? text : "", escaped, sizeof(escaped));
    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.output_item.done\",\"output_index\":%d,\"item\":{\"id\":\"msg_%d\",\"type\":\"message\",\"status\":\"completed\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"%s\",\"annotations\":[]}]},\"sequence_number\":%d}",
             output_index,
             output_index,
             escaped,
             sequence_number);
    return true;
}

bool codex_stream_error(const char *id, const char *error_message, int sequence_number, HarnessStreamEvent *out) {
    if (!out) return false;
    (void)id;
    memset(out, 0, sizeof(*out));
    snprintf(out->event, sizeof(out->event), "response.error");
    char escaped[131072];
    json_escape_event_text(error_message ? error_message : "", escaped, sizeof(escaped));
    snprintf(out->data, sizeof(out->data),
             "{\"type\":\"response.error\",\"error\":{\"message\":\"%s\"},\"sequence_number\":%d}",
             escaped, sequence_number);
    return true;
}
