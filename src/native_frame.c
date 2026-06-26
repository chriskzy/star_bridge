#include "native_frame.h"
#include "json_utils.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------
 *  Frame read/write
 * ------------------------------------------------------------------- */

/* Read exactly n bytes from fd (blocking until all received or error).
 * Returns bytes read, or -1 on error (EOF returns 0). */
static ssize_t read_all(int fd, char *buf, size_t count) {
    size_t off = 0;
    while (off < count) {
        ssize_t n = read(fd, buf + off, count - off);
        if (n > 0) {
            off += (size_t)n;
        } else if (n == 0) {
            return (ssize_t)off == 0 ? 0 : (ssize_t)off;
        } else if (errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return (ssize_t)off;
}

/* Read up to count bytes from fd with deadline.
 * Uses poll() with the given deadline_ms before each read.
 * Returns bytes read (may be less than count on timeout/EOF), -1 on error.
 * deadline_ms: -1 means blocking (no timeout, uses read_all internally),
 *              0 means immediate poll (non-blocking),
 *              >0 means timeout in milliseconds.
 */
static ssize_t read_all_deadline(int fd, char *buf, size_t count, int deadline_ms, bool *timed_out) {
    if (deadline_ms < 0) {
        /* Blocking mode — use read_all */
        return read_all(fd, buf, count);
    }

    size_t off = 0;
    int remaining = deadline_ms;

    while (off < count) {
        if (deadline_ms > 0 && remaining <= 0) {
            *timed_out = true;
            return (ssize_t)off;
        }

        /* Poll for data with remaining timeout */
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pfd, 1, remaining);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ready == 0) {
            *timed_out = true;
            return (ssize_t)off;
        }
        if (!(pfd.revents & POLLIN)) {
            /* POLLHUP, POLLERR, or POLLNVAL */
            return -1;
        }

        ssize_t n = read(fd, buf + off, count - off);
        if (n > 0) {
            off += (size_t)n;
            /* Recalculate remaining — we don't track precise elapsed time
             * between poll and read, but for simplicity just decrement
             * by a small nominal amount per iteration.
             * For deadline_ms == 0 (non-blocking), stop after one read attempt. */
            if (deadline_ms == 0) {
                break;
            }
            /* Reduce remaining by a nominal poll + read cycle estimate (10ms) */
            remaining -= 10;
            if (remaining < 0) remaining = 0;
        } else if (n == 0) {
            /* EOF */
            return (ssize_t)off;
        } else if (errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }

    return (ssize_t)off;
}

/* Read a single length-prefixed JSON frame from fd.
 * Returns NULL on EOF/error. Caller must free the buffer.
 * If error_out is non-NULL, *error_out is set to a static error string on failure
 * (e.g. "payload_too_large", "read_error", "truncated_header", "truncated_payload",
 *  "timeout", "out_of_memory"), or NULL on success.
 * deadline_ms: -1 means blocking (no timeout), 0 means non-blocking (poll with 0),
 *              positive means timeout in milliseconds for the entire frame read. */
char *frame_read(int fd, size_t *out_len, int deadline_ms, const char **error_out) {
    uint32_t len = 0;
    bool timed_out = false;

    /* Read the 4-byte length prefix */
    ssize_t n = read_all_deadline(fd, (char *)&len, 4, deadline_ms, &timed_out);
    if (n <= 0) {
        if (n == 0) {
            /* EOF — nothing at all or partial header */
            if (timed_out) {
                if (out_len) *out_len = 0;
                if (error_out) *error_out = "timeout";
                return NULL;
            }
            /* EOF with 0 bytes = connection closed */
            if (out_len) *out_len = 0;
            if (error_out) *error_out = "read_error";
            return NULL;
        }
        /* n < 0 : error from read/poll */
        if (out_len) *out_len = 0;
        if (error_out) *error_out = timed_out ? "timeout" : "read_error";
        return NULL;
    }

    if ((size_t)n < 4) {
        /* Partial header: got some bytes but not a complete 4-byte length */
        if (out_len) *out_len = 0;
        if (error_out) *error_out = timed_out ? "timeout" : "truncated_header";
        return NULL;
    }

    /* Validate length */
    if (len > FRAME_MAX_PAYLOAD) {
        if (out_len) *out_len = 0;
        if (error_out) *error_out = "payload_too_large";
        return NULL;
    }

    if (len == 0) {
        /* Empty frame – still return an allocated empty string */
        if (out_len) *out_len = 0;
        if (error_out) *error_out = NULL;
        return strdup("");
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        if (out_len) *out_len = 0;
        if (error_out) *error_out = "out_of_memory";
        return NULL;
    }

    /* Read the payload. deadline_ms may have been consumed partly by header.
     * For simplicity, if deadline_ms > 0, reduce by a nominal 10ms for header;
     * otherwise pass through unchanged. */
    int payload_deadline = deadline_ms;
    if (deadline_ms > 0) {
        payload_deadline = deadline_ms - 10;
        if (payload_deadline < 0) payload_deadline = 0;
    }

    timed_out = false;
    n = read_all_deadline(fd, buf, (size_t)len, payload_deadline, &timed_out);
    if (n <= 0) {
        free(buf);
        if (n == 0 && timed_out) {
            /* Timeout before any payload data */
            if (out_len) *out_len = 0;
            if (error_out) *error_out = "timeout";
            return NULL;
        }
        if (out_len) *out_len = 0;
        if (error_out) *error_out = timed_out ? "timeout" : "read_error";
        return NULL;
    }

    if ((size_t)n < (size_t)len) {
        /* Partial payload: EOF or timeout before full payload */
        free(buf);
        if (timed_out) {
            if (out_len) *out_len = 0;
            if (error_out) *error_out = "timeout";
            return NULL;
        }
        /* EOF with partial payload */
        if (out_len) *out_len = 0;
        if (error_out) *error_out = "truncated_payload";
        return NULL;
    }

    buf[(size_t)len] = '\0';
    if (out_len) *out_len = (size_t)len;
    if (error_out) *error_out = NULL;
    return buf;
}

/* Write a JSON frame to fd. Returns true on success. */
bool frame_write(int fd, const char *json, size_t len) {
    if (!json || fd < 0) return false;

    uint32_t net_len = (uint32_t)len;
    if (net_len > FRAME_MAX_PAYLOAD) return false;

    /* Write length prefix */
    ssize_t n = write(fd, &net_len, 4);
    if (n != 4) return false;

    /* Write payload */
    size_t off = 0;
    while (off < len) {
        n = write(fd, json + off, len - off);
        if (n > 0) off += (size_t)n;
        else if (errno != EINTR) return false;
    }
    return true;
}

/* Write a typed frame: {"type":"...", ...} */
bool frame_write_typed(int fd, const char *type, const char *body_json, size_t body_len) {
    /* Build combined JSON: {"type":"TYPE", ...body...} */
    /* If body_json starts with '{', we insert type before the first '}' */
    /* Simplest: wrap body as {"type":"TYPE", "payload":BODY} */
    /* Or merge: we can just prepend type field */
    /* For now, we build a simple wrapper */
    char escaped_prefix_type[256] = {0};
    json_escape(type ? type : "", escaped_prefix_type, sizeof(escaped_prefix_type));

    char prefix[256];
    size_t prefix_len = (size_t)snprintf(prefix, sizeof(prefix),
        "{\"type\":\"%s\",\"payload\":", escaped_prefix_type);

    size_t total_len = prefix_len + body_len + 1;
    char *combined = malloc(total_len);
    if (!combined) return false;

    memcpy(combined, prefix, prefix_len);
    memcpy(combined + prefix_len, body_json, body_len);
    combined[total_len - 1] = '}';

    bool ok = frame_write(fd, combined, total_len);
    free(combined);
    return ok;
}

/* -------------------------------------------------------------------
 *  Envelope builders (simple JSON construction)
 * ------------------------------------------------------------------- */

/* Build a request envelope */
char *frame_build_request(const char *request_id, const char *input,
                          const char *system_instructions, const char *tools_json,
                          const char *reasoning_effort,
                          const char *previous_response_id,
                          bool auto_load_resume_session,
                          int context_tokens,
                          bool reset_session,
                          const char *tool_history_json) {
    /* Build a JSON request object */
    size_t input_len = input ? strlen(input) : 0;
    size_t history_len = tool_history_json ? strlen(tool_history_json) : 0;
    size_t alloc = input_len * 2 + history_len + 8192;
    if (alloc < 8192) alloc = 8192;
    char *buf = malloc(alloc);
    if (!buf) return NULL;

    char escaped_id[256] = {0};
    json_escape(request_id ? request_id : "", escaped_id, sizeof(escaped_id));

    size_t off = 0;
    off += (size_t)snprintf(buf + off, alloc - off,
        "{\"type\":\"request\",\"id\":\"%s\",\"input\":\"",
        escaped_id);

    /* Escape input */
    for (const char *p = input ? input : ""; *p && off + 6 < alloc; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"': case '\\': buf[off++] = '\\'; buf[off++] = (char)c; break;
            case '\n': buf[off++] = '\\'; buf[off++] = 'n'; break;
            case '\r': break;
            default: if (c < 32) break; else buf[off++] = (char)c;
        }
    }
    buf[off] = '\0';

    /* Add system instructions if provided */
    if (system_instructions && system_instructions[0]) {
        off += (size_t)snprintf(buf + off, alloc - off,
            "\",\"system\":\"");
        /* Escape system instructions */
        for (const char *p = system_instructions; *p && off + 6 < alloc; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
                case '"': case '\\': buf[off++] = '\\'; buf[off++] = (char)c; break;
                case '\n': buf[off++] = '\\'; buf[off++] = 'n'; break;
                case '\r': break;
                default: if (c < 32) break; else buf[off++] = (char)c;
            }
        }
        buf[off] = '\0';
        off += (size_t)snprintf(buf + off, alloc - off, "\"");
    } else {
        off += (size_t)snprintf(buf + off, alloc - off, "\"");
    }

    /* Add reasoning effort */
    if (reasoning_effort && reasoning_effort[0]) {
        char escaped_reasoning[256] = {0};
        json_escape(reasoning_effort, escaped_reasoning, sizeof(escaped_reasoning));
        off += (size_t)snprintf(buf + off, alloc - off,
            ",\"reasoning_effort\":\"%s\"", escaped_reasoning);
    }

    if (previous_response_id && previous_response_id[0]) {
        char escaped_prev[256] = {0};
        json_escape(previous_response_id, escaped_prev, sizeof(escaped_prev));
        off += (size_t)snprintf(buf + off, alloc - off,
            ",\"previous_response_id\":\"%s\"", escaped_prev);
    }

    off += (size_t)snprintf(buf + off, alloc - off,
        ",\"auto_load_resume_session\":%s,\"context_tokens\":%d,\"reset_session\":%s",
        auto_load_resume_session ? "true" : "false",
        context_tokens,
        reset_session ? "true" : "false");

    /* Add tools if provided */
    if (tools_json && tools_json[0]) {
        off += (size_t)snprintf(buf + off, alloc - off,
            ",\"tools\":%s", tools_json);
    }

    off += (size_t)snprintf(buf + off, alloc - off,
        ",\"tool_permission_mode\":\"auto_allow_safe\",\"do_not_prompt_for_read_only_tools\":true");

    off += (size_t)snprintf(buf + off, alloc - off,
        ",\"accepted_events\":[\"compaction.started\",\"compaction.summary\",\"compaction.completed\"]");

    /* Add tool history if provided */
    if (tool_history_json && tool_history_json[0] && strcmp(tool_history_json, "[]") != 0) {
        off += (size_t)snprintf(buf + off, alloc - off,
            ",\"tool_history\":%s", tool_history_json);
    }

    off += (size_t)snprintf(buf + off, alloc - off, "}");
    return buf;
}

/* Build a response envelope */
char *frame_build_response(const char *request_id, const char *status,
                           const char *text_chunk, const char *tool_intent_json) {
    size_t alloc = 4096;
    char *buf = malloc(alloc);
    if (!buf) return NULL;

    char escaped_id[256] = {0};
    json_escape(request_id ? request_id : "", escaped_id, sizeof(escaped_id));
    char escaped_status[256] = {0};
    json_escape(status ? status : "completed", escaped_status, sizeof(escaped_status));

    size_t off = 0;
    off += (size_t)snprintf(buf + off, alloc - off,
        "{\"type\":\"response\",\"id\":\"%s\",\"status\":\"%s\"",
        escaped_id, escaped_status);

    if (text_chunk && text_chunk[0]) {
        off += (size_t)snprintf(buf + off, alloc - off,
            ",\"text\":\"");
        for (const char *p = text_chunk; *p && off + 6 < alloc; p++) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
                case '"': case '\\': buf[off++] = '\\'; buf[off++] = (char)c; break;
                case '\n': buf[off++] = '\\'; buf[off++] = 'n'; break;
                case '\r': break;
                default: if (c < 32) break; else buf[off++] = (char)c;
            }
        }
        buf[off] = '\0';
        off += (size_t)snprintf(buf + off, alloc - off, "\"");
    }

    if (tool_intent_json && tool_intent_json[0]) {
        off += (size_t)snprintf(buf + off, alloc - off,
            ",\"tool_intent\":%s", tool_intent_json);
    }

    off += (size_t)snprintf(buf + off, alloc - off, "}");
    return buf;
}

/* Build a tool-result envelope */
char *frame_build_tool_result(const char *request_id, const char *tool_name,
                              const char *tool_result_json) {
    size_t alloc = 4096;
    char *buf = malloc(alloc);
    if (!buf) return NULL;

    char escaped_tool_id[256] = {0};
    json_escape(request_id ? request_id : "", escaped_tool_id, sizeof(escaped_tool_id));
    char escaped_tool_name[256] = {0};
    json_escape(tool_name ? tool_name : "", escaped_tool_name, sizeof(escaped_tool_name));

    size_t off = 0;
    off += (size_t)snprintf(buf + off, alloc - off,
        "{\"type\":\"tool_result\",\"id\":\"%s\",\"tool\":{\"name\":\"%s\",\"result\":",
        escaped_tool_id, escaped_tool_name);

    if (tool_result_json && tool_result_json[0]) {
        off += (size_t)snprintf(buf + off, alloc - off, "%s", tool_result_json);
    } else {
        off += (size_t)snprintf(buf + off, alloc - off, "\"{}\"");
    }
    off += (size_t)snprintf(buf + off, alloc - off, "}}");
    return buf;
}

/* Build a tool-error envelope */
char *frame_build_tool_error(const char *request_id, const char *tool_name,
                             const char *message) {
    size_t alloc = 4096;
    char *buf = malloc(alloc);
    if (!buf) return NULL;

    char escaped_err_id[256] = {0};
    json_escape(request_id ? request_id : "", escaped_err_id, sizeof(escaped_err_id));
    char escaped_err_name[256] = {0};
    json_escape(tool_name ? tool_name : "", escaped_err_name, sizeof(escaped_err_name));

    size_t off = 0;
    off += (size_t)snprintf(buf + off, alloc - off,
        "{\"type\":\"tool_result\",\"id\":\"%s\",\"tool\":{\"name\":\"%s\",\"result\":{\"status\":\"error\",\"message\":\"",
        escaped_err_id, escaped_err_name);

    for (const char *p = message ? message : ""; *p && off + 6 < alloc; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"': case '\\': buf[off++] = '\\'; buf[off++] = (char)c; break;
            case '\n': buf[off++] = '\\'; buf[off++] = 'n'; break;
            case '\r': break;
            default: if (c < 32) break; else buf[off++] = (char)c;
        }
    }
    off += (size_t)snprintf(buf + off, alloc - off, "\"}}}");
    return buf;
}

/* Parse a frame to extract the "type" field using cJSON via json_utils */
char *frame_get_type(const char *frame, size_t len) {
    if (!frame || len == 0) return NULL;

    void *obj = json_parse(frame);
    if (!obj) return NULL;
    char buf[64] = {0};
    bool found = json_get_string(obj, "type", buf, sizeof(buf));
    json_free(obj);
    if (!found) return NULL;
    char *type = malloc(sizeof(buf));
    if (!type) return NULL;
    snprintf(type, sizeof(buf), "%s", buf);
    return type;
}
