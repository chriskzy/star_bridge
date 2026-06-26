#ifndef JSON_LOG_H
#define JSON_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

/* Log levels */
typedef enum {
    LOG_LEVEL_INFO  = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_ERROR = 2,
    LOG_LEVEL_DEBUG = 3,
} LogLevel;

/* Structured JSON logging.
 * Emits a JSON line to stderr with level, event, timestamp, and optional fields.
 * Example output:
 *   {"ts":"2025-06-01T12:00:00Z","level":"info","event":"bridge_start","request_id":null,"duration_ms":null,"msg":"bridge initialized"}
 *
 * Caller provides event name, level, and printf-style format + args for the message.
 * request_id and duration_ms are auto-filled if available from global config.
 */
void json_log(LogLevel level, const char *event, const char *fmt, ...);

/* Convenience wrappers */
#define json_log_info(event, fmt, ...)    json_log(LOG_LEVEL_INFO, event, fmt, ##__VA_ARGS__)
#define json_log_warn(event, fmt, ...)    json_log(LOG_LEVEL_WARN, event, fmt, ##__VA_ARGS__)
#define json_log_error(event, fmt, ...)   json_log(LOG_LEVEL_ERROR, event, fmt, ##__VA_ARGS__)
#define json_log_debug(event, fmt, ...)   json_log(LOG_LEVEL_DEBUG, event, fmt, ##__VA_ARGS__)

/* Convert debug_trace_append to use JSON logging internally.
 * Keeps the same file-based output but writes JSON lines instead of key=val format.
 */
void debug_trace_set_json_mode(bool enabled);
bool debug_trace_json_mode(void);

#endif /* JSON_LOG_H */
