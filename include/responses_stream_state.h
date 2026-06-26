#ifndef RESPONSES_STREAM_STATE_H
#define RESPONSES_STREAM_STATE_H

#include "harness_adapter.h"

#include <stdbool.h>
#include <stddef.h>

/* Maximum number of output items (messages) in a single response */
#define RESPONSES_MAX_OUTPUT_ITEMS 16
/* Maximum number of content parts per output item */
#define RESPONSES_MAX_CONTENT_PARTS 16
/* Maximum number of reasoning summaries per response */
#define RESPONSES_MAX_REASONING 8
/* Maximum number of tool calls per response */
#define RESPONSES_MAX_TOOL_CALLS 16

/* Content part types */
typedef enum {
    CONTENT_TYPE_OUTPUT_TEXT = 0,
    CONTENT_TYPE_REASONING_SUMMARY = 1,
    CONTENT_TYPE_FUNCTION_CALL = 2,
} ContentPartType;

/* A single content part within an output item */
typedef struct {
    ContentPartType type;
    char text[131072];       /* accumulated text for output_text parts */
    char name[64];           /* function name for function_call parts */
    char arguments[131072];  /* accumulated function arguments */
    int seq_start;           /* sequence number when this part started */
} ContentPart;

/* An output item (message) within the response */
typedef struct {
    char id[64];             /* item id like "msg_0" */
    char status[32];         /* "in_progress" or "completed" */
    char role[16];           /* "assistant" */
    int content_count;
    ContentPart parts[RESPONSES_MAX_CONTENT_PARTS];
} OutputItem;

/* Usage tracking */
typedef struct {
    int input_tokens;
    int output_tokens;
    int reasoning_tokens;
    bool tracked;
} StreamUsage;

/* Terminal state */
typedef enum {
    STREAM_ACTIVE = 0,
    STREAM_COMPLETED = 1,
    STREAM_CANCELLED = 2,
    STREAM_ERROR = 3,
    STREAM_DISCONNECTED = 4,
} StreamTerminalState;

/* Main stream state */
typedef struct {
    char id[64];             /* stream/response id */
    char model[128];         /* model name */
    int sequence_number;     /* global SSE sequence counter */
    int fd;                  /* client fd for writing */
    bool headers_sent;

    /* Output items */
    int item_count;
    OutputItem items[RESPONSES_MAX_OUTPUT_ITEMS];

    /* Current item/part being accumulated */
    int cur_item;
    int cur_part;

    /* Gzip support flag (set from Accept-Encoding header) */
    bool gzip_supported;

    /* Usage */
    StreamUsage usage;

    /* Terminal */
    StreamTerminalState terminal_state;
    char error_message[256];
    char incomplete_details[128];  /* set if response completed with partial output */
} ResponsesStreamState;

/* Initialize a stream state for a new response */
void responses_stream_init(ResponsesStreamState *s, const char *id,
                           const char *model, int fd, bool gzip_supported);

/* Send SSE headers if not already sent */
bool responses_stream_send_headers(ResponsesStreamState *s);

/* Emit response.created event (always first) */
bool responses_stream_emit_created(ResponsesStreamState *s);

/* Emit output_item.added for a new output item */
bool responses_stream_emit_output_item_added(ResponsesStreamState *s);

/* Emit content_part.added for a new content part */
bool responses_stream_emit_content_part_added(ResponsesStreamState *s,
                                               ContentPartType type);

/* Emit output_text.delta for a text chunk */
bool responses_stream_emit_text_delta(ResponsesStreamState *s,
                                       const char *delta_text);

/* Emit reasoning_summary delta (reasoning summary events) */
bool responses_stream_emit_reasoning_delta(ResponsesStreamState *s,
                                            const char *delta_text);

/* Emit function_call_arguments.delta */
bool responses_stream_emit_function_call_delta(ResponsesStreamState *s,
                                                const char *delta_text);

/* Emit output_text.done when text part completes */
bool responses_stream_emit_text_done(ResponsesStreamState *s);

/* Emit content_part.done */
bool responses_stream_emit_content_part_done(ResponsesStreamState *s);

/* Emit output_item.done */
bool responses_stream_emit_output_item_done(ResponsesStreamState *s);

/* Emit response.completed with final state */
bool responses_stream_emit_completed(ResponsesStreamState *s);

/* Emit error event and set terminal state */
bool responses_stream_emit_error(ResponsesStreamState *s,
                                  const char *error_message);

/* Emit a terminal response.failed event (Responses API failure terminal) with a
 * machine reason such as "cancelled". Used for mid-turn DELETE cancellation. */
bool responses_stream_emit_failed(ResponsesStreamState *s,
                                  const char *reason);

/* Send SSE heartbeat keepalive */
bool responses_stream_send_heartbeat(ResponsesStreamState *s);

/* Check if stream is still active */
bool responses_stream_is_active(const ResponsesStreamState *s);

/* Send [DONE] marker at end of successful stream */
bool responses_stream_send_done_marker(ResponsesStreamState *s);

/* Start a new output item (message) */
bool responses_stream_start_item(ResponsesStreamState *s);

/* Start a new content part of given type */
bool responses_stream_start_part(ResponsesStreamState *s, ContentPartType type,
                                  const char *name);

/* Convenience: run full streaming lifecycle with text content.
 * Returns true if stream completed successfully. */
bool responses_stream_run_text(ResponsesStreamState *s,
                                const char *content, size_t content_len,
                                size_t chunk_size);

#endif /* RESPONSES_STREAM_STATE_H */
