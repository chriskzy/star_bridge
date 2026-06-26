#ifndef NATIVE_RESPONSE_H
#define NATIVE_RESPONSE_H

#include <stdbool.h>
#include <stddef.h>

/* -------------------------------------------------------------------
 *  Schema-aware native response/event parser
 *
 *  Parses native response frames into typed event structs using cJSON
 *  via json_utils. Properly decodes all JSON escape sequences and
 *  distinguishes top-level "text" fields from nested ones.
 * ------------------------------------------------------------------- */

/* Event types */
typedef enum {
    NATIVE_EVENT_UNKNOWN        = 0,
    NATIVE_EVENT_ACK            = 1,
    NATIVE_EVENT_RESPONSE       = 2,  /* {"type":"response","id":"...","text|output":"...","status":"..."} */
    NATIVE_EVENT_TEXT_DELTA     = 13, /* {"type":"text_delta","id":"...","text":"partial..."} live during turn */
    NATIVE_EVENT_TOOL_INTENT    = 3,  /* {"type":"tool_intent","id":"...","tool":{"name":"...","args":...}} */
    NATIVE_EVENT_ERROR          = 4,  /* {"type":"error","id":"...","status":"...","message":"..."} */
    NATIVE_EVENT_COMPACTION     = 5,  /* {"type":"compaction.started|compaction.summary|compaction.completed",...} */
    NATIVE_EVENT_HEALTH         = 6,
    NATIVE_EVENT_PING           = 7,
    NATIVE_EVENT_PONG           = 8,
    NATIVE_EVENT_READY          = 9,
    NATIVE_EVENT_SHUTDOWN       = 10,
    NATIVE_EVENT_CANCELLED      = 11,
    NATIVE_EVENT_SHUTDOWN_ACK   = 12,
} NativeEventType;

/* Response event struct */
typedef struct {
    char id[64];
    char status[64];
    char text[4096];       /* top-level "text" or "output" field, properly decoded */
    char tool_intent_json[4096];
    /* Usage fields parsed from native response (may be 0 if not reported) */
    int prompt_tokens;
    int completion_tokens;
    char usage_json[4096]; /* raw usage object from native agent (JSON string) */
} NativeResponseEvent;

/* Tool intent event struct */
typedef struct {
    char id[64];
    char tool_name[128];
    char tool_args[4096];
} NativeToolIntentEvent;

/* Error event struct */
typedef struct {
    char id[64];
    char status[64];
    char message[512];
} NativeErrorEvent;

/* Compaction event struct */
typedef struct {
    char type[64];        /* compaction.started, compaction.summary, compaction.completed */
    char message[512];
} NativeCompactionEvent;

/* Union of event data */
typedef union {
    NativeResponseEvent     response;
    NativeToolIntentEvent   tool_intent;
    NativeErrorEvent        error;
    NativeCompactionEvent   compaction;
} NativeEventData;

/* Top-level parsed event */
typedef struct {
    NativeEventType type;
    char            type_str[64];   /* original type string from frame */
    char            id[64];        /* common id field (may be empty) */
    NativeEventData data;
} NativeEvent;

/* Parse a native response frame into a typed event struct.
 * Returns true on success. frame must be a valid JSON object string.
 * Always zero-initializes event on entry. */
bool native_parse_frame(const char *frame, size_t len, NativeEvent *event);

/* Map a type string to a NativeEventType enum.
 * Returns NATIVE_EVENT_UNKNOWN if not recognized. */
NativeEventType native_event_type_from_string(const char *type_str);

/* Convenience: check if event type is a lifecycle frame (ping/pong/health/ready/shutdown) */
bool native_event_is_lifecycle(const NativeEvent *event);

#endif /* NATIVE_RESPONSE_H */
