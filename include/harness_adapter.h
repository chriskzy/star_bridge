#ifndef HARNESS_ADAPTER_H
#define HARNESS_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>

#define HARNESS_METADATA_MAX 8192
#define HARNESS_ID_MAX 256
#define INPUT_ITEM_NAME_MAX 64

/* Input item types in the Responses API */
typedef enum {
    INPUT_ITEM_TYPE_TEXT = 0,        /* input_text / simple text */
    INPUT_ITEM_TYPE_MESSAGE = 1,     /* message with content array */
    INPUT_ITEM_TYPE_TOOL_CALL = 2,   /* tool_call / function_call */
    INPUT_ITEM_TYPE_TOOL_OUTPUT = 3, /* tool_call_output / function_call_output */
    INPUT_ITEM_TYPE_IMAGE = 4,       /* input_image */
    INPUT_ITEM_TYPE_COMPUTER = 5,    /* computer_call */
    INPUT_ITEM_TYPE_REASONING = 6,   /* reasoning summary */
} InputItemType;

/* A single input item from the Responses API input array */
typedef struct {
    InputItemType type;
    char role[16];              /* "user", "assistant", "system" for messages */
    char name[INPUT_ITEM_NAME_MAX]; /* function name for tool calls */
    char call_id[128];          /* call id for tool calls */
    char *arguments;            /* JSON arguments for tool calls */
    char *output;               /* output for tool_call_output */
    char *text;                 /* text content for text/message items */
    char image_url[2048];       /* image URL */
    char *image_data;           /* base64 image data */
} InputItem;

typedef struct {
    char harness[32];
    char model[128];
    char *normalized_input;
    char *instructions;
    char metadata_json[HARNESS_METADATA_MAX];
    char previous_response_id[HARNESS_ID_MAX];
    char reasoning_effort[32];
    bool stream;
    bool reset_session;
    bool has_tools;
    bool explicit_tool_call;
    char tool_name[128];
    char tool_argument[2048];

    /* Structured input items */
    int input_item_count;
    int input_item_capacity;
    InputItem *input_items;

    /* Tool definitions from request */
    int tool_count;
    char tool_defs_json[65536];
    char tool_choice_json[2048];
} HarnessRequest;

void harness_request_free(HarnessRequest *req);

typedef struct {
    int status_code;
    char content_type[64];
    char body[150000];
    bool streaming;
} HarnessResponse;

typedef struct {
    char event[64];
    char data[131072];
} HarnessStreamEvent;

typedef struct {
    char code[64];
    char message[512];
} HarnessError;

typedef struct {
    char harness_id[32];
    char model[128];
    bool enabled;
    bool streaming_enabled;
} HarnessAdapterConfig;

typedef struct {
    const char *name;
    bool (*parse_request)(const char *body, HarnessRequest *out, HarnessError *err);
    size_t (*serialize_text_response)(const char *id, const char *model, const char *text, char *buf, size_t max_len);
    size_t (*serialize_error)(const char *message, char *buf, size_t max_len);
    bool (*stream_created)(const char *id, const char *model, HarnessStreamEvent *out);
    bool (*stream_delta)(const char *id, const char *model, const char *delta_text, HarnessStreamEvent *out);
    bool (*stream_completed)(const char *id, const char *model, const char *final_text, int sequence_number, int prompt_tokens, int completion_tokens, const char *usage_json, const char *incomplete_details, HarnessStreamEvent *out);
    /* Responses lifecycle events */
    bool (*stream_output_item_added)(const char *id, int output_index, int sequence_number, HarnessStreamEvent *out);
    bool (*stream_content_part_added)(const char *id, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out);
    bool (*stream_output_text_delta)(const char *id, const char *model, const char *delta_text, int item_index, int content_index, int sequence_number, HarnessStreamEvent *out);
    bool (*stream_output_text_done)(const char *id, const char *text, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out);
    bool (*stream_content_part_done)(const char *id, const char *text, int output_index, int content_index, int sequence_number, HarnessStreamEvent *out);
    bool (*stream_output_item_done)(const char *id, const char *text, int output_index, int sequence_number, HarnessStreamEvent *out);
    bool (*stream_error)(const char *id, const char *error_message, int sequence_number, HarnessStreamEvent *out);
} HarnessAdapterVTable;

#endif /* HARNESS_ADAPTER_H */
