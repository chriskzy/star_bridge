#include "responses_stream_state.h"
#include "codex_stream_events.h"
#include "json_utils.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

/* NOTE: SSE responses are sent uncompressed. A previous implementation gzip-
 * compressed individual `data:` payloads and inlined the raw bytes — that is
 * invalid: gzip output contains NUL bytes (so the `%s` formatting truncated it)
 * and embedding binary in an SSE `data:` field breaks the text/event-stream
 * framing, while also mismatching the single Content-Encoding: gzip header.
 * Event payloads are small text; compression is removed rather than reworked. */

/* ------------------------------------------------------------------ */
/*  Helper: send a single SSE event                                   */
/* ------------------------------------------------------------------ */
static bool send_sse_event(int fd, const char *event, const char *data, int seq) {
    char buf[131072 + 128];
    int n = snprintf(buf, sizeof(buf),
                     "event: %s\ndata: %s\n\n", event ? event : "",
                     data ? data : "");
    if (n <= 0) return false;
    if ((size_t)n > sizeof(buf)) n = (int)sizeof(buf) - 1;
    ssize_t written = write(fd, buf, (size_t)n);
    fprintf(stderr, "sse event=%s bytes=%zd data_len=%zu seq=%d\n",
            event ? event : "",
            written > 0 ? written : 0,
            data ? strlen(data) : 0,
            seq);
    return written == n;
}

/* ------------------------------------------------------------------ */
/*  Helper: send SSE heartbeat                                        */
/* ------------------------------------------------------------------ */
static bool send_heartbeat(int fd) {
    const char *hb = ":heartbeat\n\n";
    ssize_t w = write(fd, hb, strlen(hb));
    return w == (ssize_t)strlen(hb);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void responses_stream_init(ResponsesStreamState *s, const char *id,
                           const char *model, int fd, bool gzip_supported) {
    memset(s, 0, sizeof(*s));
    if (id) snprintf(s->id, sizeof(s->id), "%s", id);
    if (model) snprintf(s->model, sizeof(s->model), "%s", model);
    s->fd = fd;
    s->gzip_supported = gzip_supported;
    s->sequence_number = 0;
    s->headers_sent = false;
    s->terminal_state = STREAM_ACTIVE;
    s->cur_item = -1;
    s->cur_part = -1;
    s->usage.input_tokens = 0;
    s->usage.output_tokens = 0;
    s->usage.reasoning_tokens = 0;
    s->usage.tracked = false;
}

bool responses_stream_send_headers(ResponsesStreamState *s) {
    if (s->headers_sent) return true;
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\nConnection: keep-alive\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n");
    if (n <= 0 || (size_t)n >= sizeof(hdr)) return false;
    ssize_t w = write(s->fd, hdr, (size_t)n);
    if (w != (ssize_t)n) return false;
    s->headers_sent = true;
    return true;
}

bool responses_stream_emit_created(ResponsesStreamState *s) {
    if (!responses_stream_send_headers(s)) return false;
    HarnessStreamEvent event;
    codex_stream_created(s->id, s->model, &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);
    send_heartbeat(s->fd);
    return true;
}

bool responses_stream_emit_output_item_added(ResponsesStreamState *s) {
    int idx = s->item_count;
    if (idx >= RESPONSES_MAX_OUTPUT_ITEMS) return false;
    HarnessStreamEvent event;
    codex_stream_output_item_added(s->id, idx, s->sequence_number, &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);
    send_heartbeat(s->fd);

    /* Track the item */
    snprintf(s->items[idx].id, sizeof(s->items[idx].id), "msg_%d", idx);
    snprintf(s->items[idx].status, sizeof(s->items[idx].status), "in_progress");
    snprintf(s->items[idx].role, sizeof(s->items[idx].role), "assistant");
    s->items[idx].content_count = 0;
    s->item_count++;
    s->cur_item = idx;
    s->cur_part = -1;
    return true;
}

bool responses_stream_emit_content_part_added(ResponsesStreamState *s,
                                               ContentPartType type) {
    if (s->cur_item < 0 || s->cur_item >= s->item_count) return false;
    OutputItem *item = &s->items[s->cur_item];
    int idx = item->content_count;
    if (idx >= RESPONSES_MAX_CONTENT_PARTS) return false;

    HarnessStreamEvent event;
    codex_stream_content_part_added(s->id, s->cur_item, idx,
                                     s->sequence_number, &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);
    send_heartbeat(s->fd);

    /* Track the part */
    ContentPart *part = &item->parts[idx];
    memset(part, 0, sizeof(*part));
    part->type = type;
    part->seq_start = seq;
    item->content_count++;
    s->cur_part = idx;
    return true;
}

bool responses_stream_emit_text_delta(ResponsesStreamState *s,
                                       const char *delta_text) {
    if (s->cur_item < 0 || s->cur_part < 0) return false;
    OutputItem *item = &s->items[s->cur_item];
    ContentPart *part = &item->parts[s->cur_part];
    if (part->type != CONTENT_TYPE_OUTPUT_TEXT) return false;

    HarnessStreamEvent event;
    codex_stream_output_text_delta(s->id, s->model, delta_text,
                                    s->cur_item, s->cur_part,
                                    s->sequence_number, &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);

    /* Accumulate text in part */
    size_t current = strlen(part->text);
    size_t delta_len = strlen(delta_text);
    if (current + delta_len < sizeof(part->text)) {
        memcpy(part->text + current, delta_text, delta_len + 1);
    }
    return true;
}

bool responses_stream_emit_reasoning_delta(ResponsesStreamState *s,
                                            const char *delta_text) {
    if (s->cur_item < 0 || s->cur_part < 0) return false;
    OutputItem *item = &s->items[s->cur_item];
    ContentPart *part = &item->parts[s->cur_part];
    if (part->type != CONTENT_TYPE_REASONING_SUMMARY) return false;

    /* Emit reasoning_summary.delta event */
    char escaped[131072];
    json_escape(delta_text ? delta_text : "", escaped, sizeof(escaped));
    char buf[131072 + 128];
    int seq = s->sequence_number++;
    snprintf(buf, sizeof(buf),
             "{\"type\":\"reasoning_summary.delta\",\"item_id\":\"%s\","
             "\"output_index\":%d,\"content_index\":%d,\"delta\":\"%s\","
             "\"sequence_number\":%d}",
             item->id, s->cur_item, s->cur_part, escaped, seq);
    send_sse_event(s->fd, "reasoning_summary.delta", buf, seq);

    /* Accumulate */
    size_t current = strlen(part->text);
    size_t delta_len = strlen(delta_text);
    if (current + delta_len < sizeof(part->text)) {
        memcpy(part->text + current, delta_text, delta_len + 1);
    }
    return true;
}

bool responses_stream_emit_function_call_delta(ResponsesStreamState *s,
                                                const char *delta_text) {
    if (s->cur_item < 0 || s->cur_part < 0) return false;
    OutputItem *item = &s->items[s->cur_item];
    ContentPart *part = &item->parts[s->cur_part];
    if (part->type != CONTENT_TYPE_FUNCTION_CALL) return false;

    /* Emit function_call_arguments.delta event */
    char escaped[131072];
    json_escape(delta_text ? delta_text : "", escaped, sizeof(escaped));
    char buf[131072 + 128];
    int seq = s->sequence_number++;
    snprintf(buf, sizeof(buf),
             "{\"type\":\"function_call_arguments.delta\",\"item_id\":\"%s\","
             "\"output_index\":%d,\"content_index\":%d,\"delta\":\"%s\","
             "\"sequence_number\":%d}",
             item->id, s->cur_item, s->cur_part, escaped, seq);
    send_sse_event(s->fd, "function_call_arguments.delta", buf, seq);

    /* Accumulate */
    size_t current = strlen(part->arguments);
    size_t delta_len = strlen(delta_text);
    if (current + delta_len < sizeof(part->arguments)) {
        memcpy(part->arguments + current, delta_text, delta_len + 1);
    }
    return true;
}

bool responses_stream_emit_text_done(ResponsesStreamState *s) {
    if (s->cur_item < 0 || s->cur_part < 0) return false;
    OutputItem *item = &s->items[s->cur_item];
    ContentPart *part = &item->parts[s->cur_part];
    if (part->type != CONTENT_TYPE_OUTPUT_TEXT) return false;

    HarnessStreamEvent event;
    codex_stream_output_text_done(s->id, part->text,
                                   s->cur_item, s->cur_part,
                                   s->sequence_number, &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);
    send_heartbeat(s->fd);
    return true;
}

bool responses_stream_emit_content_part_done(ResponsesStreamState *s) {
    if (s->cur_item < 0 || s->cur_part < 0) return false;
    OutputItem *item = &s->items[s->cur_item];
    ContentPart *part = &item->parts[s->cur_part];

    HarnessStreamEvent event;
    codex_stream_content_part_done(s->id, part->text,
                                    s->cur_item, s->cur_part,
                                    s->sequence_number, &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);
    send_heartbeat(s->fd);
    return true;
}

bool responses_stream_emit_output_item_done(ResponsesStreamState *s) {
    if (s->cur_item < 0 || s->cur_item >= s->item_count) return false;
    OutputItem *item = &s->items[s->cur_item];

    HarnessStreamEvent event;
    codex_stream_output_item_done(s->id, item->parts[0].text,
                                   s->cur_item, s->sequence_number, &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);
    send_heartbeat(s->fd);

    snprintf(item->status, sizeof(item->status), "completed");
    return true;
}

bool responses_stream_emit_completed(ResponsesStreamState *s) {
    HarnessStreamEvent event;
    /* Build final text from first output item's first part */
    const char *final_text = "";
    if (s->item_count > 0 && s->items[0].content_count > 0) {
        final_text = s->items[0].parts[0].text;
    }
    const char *incomplete = s->incomplete_details[0] ? s->incomplete_details : NULL;
    codex_stream_completed(s->id, s->model, final_text,
                            s->sequence_number,
                            s->usage.input_tokens, s->usage.output_tokens,
                            "",
                            incomplete,
                            &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);
    send_heartbeat(s->fd);

    /* Send [DONE] marker */
    const char *done = "[DONE]\n";
    write(s->fd, done, strlen(done));

    s->terminal_state = STREAM_COMPLETED;
    return true;
}

bool responses_stream_emit_error(ResponsesStreamState *s,
                                  const char *error_message) {
    HarnessStreamEvent event;
    codex_stream_error(s->id, error_message, s->sequence_number, &event);
    int seq = s->sequence_number++;
    send_sse_event(s->fd, event.event, event.data, seq);
    send_heartbeat(s->fd);

    snprintf(s->error_message, sizeof(s->error_message), "%s",
             error_message ? error_message : "");
    s->terminal_state = STREAM_ERROR;
    return true;
}

bool responses_stream_emit_failed(ResponsesStreamState *s,
                                  const char *reason) {
    const char *r = (reason && reason[0]) ? reason : "failed";
    char escaped[1024];
    json_escape(r, escaped, sizeof(escaped));
    char data[2048];
    int seq = s->sequence_number++;
    snprintf(data, sizeof(data),
             "{\"type\":\"response.failed\",\"response\":{\"id\":\"%s\",\"object\":\"response\","
             "\"status\":\"failed\"},\"error\":{\"code\":\"%s\",\"message\":\"%s\"},"
             "\"sequence_number\":%d}",
             s->id, escaped, escaped, seq);
    send_sse_event(s->fd, "response.failed", data, seq);
    send_heartbeat(s->fd);
    snprintf(s->error_message, sizeof(s->error_message), "%s", r);
    s->terminal_state = STREAM_ERROR;
    return true;
}

bool responses_stream_send_heartbeat(ResponsesStreamState *s) {
    return send_heartbeat(s->fd);
}

bool responses_stream_is_active(const ResponsesStreamState *s) {
    return s->terminal_state == STREAM_ACTIVE;
}

bool responses_stream_send_done_marker(ResponsesStreamState *s) {
    const char *done = "[DONE]\n";
    ssize_t w = write(s->fd, done, strlen(done));
    return w == (ssize_t)strlen(done);
}

bool responses_stream_start_item(ResponsesStreamState *s) {
    return responses_stream_emit_output_item_added(s);
}

bool responses_stream_start_part(ResponsesStreamState *s, ContentPartType type,
                                  const char *name) {
    if (!responses_stream_emit_content_part_added(s, type)) return false;
    if (type == CONTENT_TYPE_FUNCTION_CALL && name) {
        OutputItem *item = &s->items[s->cur_item];
        ContentPart *part = &item->parts[s->cur_part];
        snprintf(part->name, sizeof(part->name), "%s", name);
    }
    return true;
}

/* Convenience: run full streaming lifecycle with text content.
 * Emits response.created, output_item.added, content_part.added,
 * text deltas in chunks, then done/completed with [DONE]. */
bool responses_stream_run_text(ResponsesStreamState *s,
                                const char *content, size_t content_len,
                                size_t chunk_size) {
    if (!responses_stream_emit_created(s)) return false;
    if (!responses_stream_start_item(s)) return false;
    if (!responses_stream_start_part(s, CONTENT_TYPE_OUTPUT_TEXT, NULL))
        return false;

    if (content && content_len > 0) {
        size_t offset = 0;
        while (offset < content_len) {
            if (!responses_stream_is_active(s)) return false;
            size_t chunk_len = content_len - offset;
            if (chunk_len > chunk_size) chunk_len = chunk_size;
            char chunk[4096 + 1];
            memcpy(chunk, content + offset, chunk_len);
            chunk[chunk_len] = '\0';
            if (!responses_stream_emit_text_delta(s, chunk)) return false;
            offset += chunk_len;
        }
    }

    if (!responses_stream_is_active(s)) return false;
    if (!responses_stream_emit_text_done(s)) return false;
    if (!responses_stream_emit_content_part_done(s)) return false;
    if (!responses_stream_emit_output_item_done(s)) return false;
    if (!responses_stream_emit_completed(s)) return false;
    return true;
}
