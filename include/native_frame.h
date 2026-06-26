#ifndef NATIVE_FRAME_H
#define NATIVE_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -------------------------------------------------------------------
 *  Framed native-agent protocol
 *
 *  Each frame is:
 *    [4-byte uint32 LE length][JSON payload]
 *
 *  Length includes only the JSON payload bytes (0 means no payload).
 *  Max frame size: 16 MB (16777216 bytes)
 * ------------------------------------------------------------------- */

#define FRAME_MAX_PAYLOAD 16777216u
#define FRAME_HEADER_SIZE 4

/* Frame types (embedded in JSON as "type" field) */
#define FRAME_TYPE_REQUEST   "request"
#define FRAME_TYPE_RESPONSE  "response"
#define FRAME_TYPE_EVENT     "event"
#define FRAME_TYPE_HEALTH    "health"
#define FRAME_TYPE_SHUTDOWN  "shutdown"
#define FRAME_TYPE_ERROR     "error"
#define FRAME_TYPE_TOOL_INTENT  "tool_intent"
#define FRAME_TYPE_TOOL_RESULT  "tool_result"

/* -------------------------------------------------------------------
 *  Frame read/write helpers
 * ------------------------------------------------------------------- */

/* Read a single frame from fd. Returns NULL on EOF/error.
 * Caller must free the returned buffer.
 * If error_out is non-NULL, *error_out is set to a static error string on failure
 * (e.g. "payload_too_large", "read_error", "truncated_header", "truncated_payload",
 *  "timeout", "out_of_memory"), or NULL on success.
 * deadline_ms: -1 means blocking (no timeout), 0 means non-blocking (poll with 0),
 *              positive means timeout in milliseconds for the entire frame read. */
char *frame_read(int fd, size_t *out_len, int deadline_ms, const char **error_out);

/* Write a JSON frame to fd. Returns true on success. */
bool frame_write(int fd, const char *json, size_t len);

/* Write a framed message with given type and body JSON.
 * Combines: {"type":"...", ...body... } into one frame. */
bool frame_write_typed(int fd, const char *type, const char *body_json, size_t body_len);

/* -------------------------------------------------------------------
 *  Request/Response envelope helpers
 * ------------------------------------------------------------------- */

/* Build a request envelope JSON string (caller must free result).
 * Returns NULL on allocation failure. */
char *frame_build_request(const char *request_id, const char *input,
                          const char *system_instructions, const char *tools_json,
                          const char *reasoning_effort,
                          const char *previous_response_id,
                          bool auto_load_resume_session,
                          int context_tokens,
                          bool reset_session,
                          const char *tool_history_json);

/* Build a response envelope from native agent output */
char *frame_build_response(const char *request_id, const char *status,
                           const char *text_chunk, const char *tool_intent_json);

/* Build a tool-result envelope to send back into native agent context */
char *frame_build_tool_result(const char *request_id, const char *tool_name,
                              const char *tool_result_json);
char *frame_build_tool_error(const char *request_id, const char *tool_name,
                             const char *message);

/* Parse a frame to extract the "type" field (caller must free). */
char *frame_get_type(const char *frame, size_t len);

#endif /* NATIVE_FRAME_H */
