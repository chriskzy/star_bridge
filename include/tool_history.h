#ifndef TOOL_HISTORY_H
#define TOOL_HISTORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Tool status enum — replaces raw char[32] status strings.
 */
typedef enum {
    TOOL_STATUS_OK      = 0,
    TOOL_STATUS_ERROR   = 1,
    TOOL_STATUS_DENIED  = 2,
    TOOL_STATUS_TIMEOUT = 3,
    TOOL_STATUS_UNKNOWN = 4,
} ToolStatus;

/**
 * Structured tool history entry.
 * Each entry captures one tool invocation during a turn.
 */
typedef struct {
    char tool_name[64];
    char args[256];
    ToolStatus status;
} ToolHistoryEntry;

/**
 * Maximum number of history entries stored in the ring buffer.
 */
#define MAX_HISTORY_ENTRIES 64

/**
 * Convert a ToolStatus enum to a human-readable string.
 */
const char *tool_status_to_string(ToolStatus s);

/**
 * Convert a string to a ToolStatus enum (case-insensitive).
 */
ToolStatus tool_status_from_string(const char *s);

/**
 * Append a tool history line to both the structured ring buffer
 * and the raw string buffer.  The line is parsed to extract
 * tool name, arguments, and status.
 */
void tool_history_append(const char *line);

/**
 * Build a JSON array string from the structured tool history ring buffer.
 * Caller must free the returned buffer.
 */
char *tool_history_build_json(void);

/**
 * Return the current count of history entries.
 */
int tool_history_count(void);

/**
 * Reset the tool history (clear all entries).
 */
void tool_history_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* TOOL_HISTORY_H */
