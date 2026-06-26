#ifndef RESPONSES_API_H
#define RESPONSES_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* Maximum lengths for response fields */
#define MAX_TEXT_OUTPUT 65536
#define MAX_OUTPUT_ITEMS 64
#define MAX_TOOL_CALLS 16
#define MAX_ERROR_MSG 512

typedef enum {
    RESP_STATUS_COMPLETED = 0,
    RESP_STATUS_FAILED,
    RESP_STATUS_IN_PROGRESS,
    RESP_STATUS_INCOMPLETE
} ResponseStatus;

typedef enum {
    OUTPUT_TYPE_TEXT = 0,
    OUTPUT_TYPE_TOOL_CALL,
    OUTPUT_TYPE_REFUSAL,
    OUTPUT_TYPE_ERROR
} OutputType;

typedef struct {
    char id[64];
    char name[128];
    char arguments[4096];
} ToolCallFunction;

typedef struct {
    char call_id[64];
    char type[32];          /* "function" */
    ToolCallFunction function;
} ToolCallInfo;

typedef struct {
    OutputType type;
    union {
        char text[MAX_TEXT_OUTPUT];
        struct {
            ToolCallInfo info;
        } tool_call;
    } data;
} OutputItem;

typedef struct {
    char id[64];            /* e.g. "resp_abc123" */
    char object[16];        /* "response" */
    int64_t created_at;
    ResponseStatus status;
    char model[128];
    OutputItem output[MAX_OUTPUT_ITEMS];
    int output_count;
    char error_msg[MAX_ERROR_MSG];
    /* usage is optional – we store minimal */
    int prompt_tokens;
    int completion_tokens;
    int total_tokens;        /* prompt_tokens + completion_tokens, 0 if unset */
    /* input_tokens_details: text, cached, audio, etc. */
    int input_text_tokens;
    int input_cached_tokens;
    int input_audio_tokens;
    /* output_tokens_details: text, reasoning */
    int output_text_tokens;
    int output_reasoning_tokens;
    /* finish_reason for stream end */
    char finish_reason[64];
    /* incomplete_details — set when response completed with partial output due to limits */
    char incomplete_details[128];
} ResponseObject;

/* Convenience builders */
void resp_init(ResponseObject *r, const char *model);
void resp_set_id(ResponseObject *r, const char *id);
void resp_add_text(ResponseObject *r, const char *text);
void resp_add_tool_call(ResponseObject *r, const char *call_id, const char *name, const char *arguments);
void resp_set_error(ResponseObject *r, const char *msg);
void resp_set_finish_reason(ResponseObject *r, const char *reason);

/* Serialization */
size_t resp_to_json(const ResponseObject *r, char *buf, size_t max_len);

/* Serialize a single output item for streaming */
size_t resp_item_to_json(const OutputItem *item, char *buf, size_t max_len);

/* Serialize a streaming delta event (partial text) */
size_t resp_stream_delta(const ResponseObject *r, const char *delta_text, char *buf, size_t max_len);

/* Serialize error response */
size_t resp_error_to_json(const char *msg, char *buf, size_t max_len);

/* Normalize usage from native agent response into ResponseObject.
 * Parses prompt_tokens/completion_tokens and usage_json fields from
 * NativeResponseEvent and maps them to input_tokens, output_tokens,
 * total_tokens, input_tokens_details, output_tokens_details.
 * If usage_json contains cache_hit/cache_create fields, they are
 * mapped to input_cached_tokens. If reasoning_tokens is present,
 * it maps to output_reasoning_tokens. */
void resp_normalize_usage(ResponseObject *r, int prompt_tokens, int completion_tokens, const char *usage_json);

/* Set usage directly from raw token counts (for non-stream responses). */
void resp_set_usage(ResponseObject *r, int input_tokens, int output_tokens,
                    int input_text, int input_cached, int input_audio,
                    int output_text, int output_reasoning);

/* Serialize usage JSON fragment (without surrounding object) into buf.
 * Returns bytes written. Writes e.g.:
 *   "usage":{"input_tokens":10,"output_tokens":5,"total_tokens":15,
 *            "input_tokens_details":{"text_tokens":10,"cached_tokens":0},
 *            "output_tokens_details":{"text_tokens":5,"reasoning_tokens":0}} */
size_t resp_usage_to_json(const ResponseObject *r, char *buf, size_t max_len);

#endif /* RESPONSES_API_H */
