#ifndef SERVER_H
#define SERVER_H

#include "bridge_core.h"
#include "tool_runner.h"
#include "harness_adapter.h"

#define TRACE_MAX_TEXT 8192
typedef struct {
    unsigned long request_number;
    char method[32];
    char path[128];
    char http_headers[4096];
    char http_body[TRACE_MAX_TEXT];
    char normalized_input[TRACE_MAX_TEXT];
    char native_request[TRACE_MAX_TEXT];
    char native_response[TRACE_MAX_TEXT];
    char error[512];
} TraceSession;

extern TraceSession g_trace_session;

void start_codex_api_server(BridgeEngine *eng, int port);

/* Shared helper functions used by turn_context.c and server.c */

/* Text utilities */
size_t append_text(char *dest, size_t max_len, size_t off, const char *text);
bool extract_json_string_field(const char *json, const char *key, char *dest, size_t max_len);
bool extract_balanced_json_local(const char *start, char *dest, size_t max_len);
const char *decode_json_string_local(const char *p, char *dest, size_t max_len);
bool extract_tool_intent(const char *frame, char *tool_name, size_t tool_name_max,
                         char *tool_args_json, size_t tool_args_max);
bool extract_compaction_event(const char *frame, char *message, size_t message_max);
size_t append_compaction_notice(char *dest, size_t max_len, size_t off, const char *message);

/* Tool history — moved to tool_history.h */
#include "tool_history.h"

/* Diagnostics */
void trace_store_text(char *dest, size_t max_len, const char *src);
void trace_store_headers(const char *raw_request);
void log_tool_diagnostics(const ToolRunResult *run);
int tool_timeout_ms(const char *tool_name);

/* Session */
void build_session_metadata_json(const BridgeEngine *eng, const char *last_response_id,
                                 char *buf, size_t buf_max);

/* Waiting */
void wait_for_native_output(BridgeEngine *eng, int timeout_ms);

/* HTTP response helpers */
void send_http(int fd, const char *status, const char *content_type, const char *body);
void send_sse(int fd, const char *event, const char *data, int id);
void send_sse_heartbeat(int fd);
bool fd_writable(int fd, int timeout_ms);
void write_all(int fd, const char *buf, size_t len);

/* handle_response extracted phases */
/* Phase 1: Decode HTTP request — extract method, path, headers, body.
 * Returns decoded body (caller must free). On error sends HTTP response and returns NULL. */
char *handle_response_decode(int fd, const char *raw_request, size_t raw_len,
                             char *method_buf, size_t method_max,
                             char *path_buf, size_t path_max,
                             long *cl_val, char *te_buf, size_t te_max,
                             char *ce_buf, size_t ce_max,
                             size_t *body_len);

/* Phase 2: Parse request body — schema validate + parse into HarnessRequest.
 * Returns true on success. On failure sends HTTP error response and returns false. */
bool handle_response_parse(int fd, unsigned long request_number,
                           const char *body, HarnessRequest *req,
                           HarnessError *err);

/* Phase 3: Forward to native agent — build input, check connection, send turn.
 * Returns true on success. On failure sends error response and returns false.
 * out_buf is populated with agent output text for framed protocol. */
bool handle_response_native(int fd, BridgeEngine *eng, unsigned long request_number,
                            const HarnessRequest *req,
                            char *out_buf, size_t out_max);

/* Phase 4: SSE streaming writer — emit Responses lifecycle events + text deltas.
 * Reads content from out_buf (framed) or engine buffer (non-framed). */
void handle_response_stream(int fd, BridgeEngine *eng, unsigned long request_number,
                            const HarnessRequest *req,
                            const char *out_buf);

/* Phase 5: Blocking response writer — serialize + validate + send HTTP.
 * Reads content from out_buf (framed) or engine buffer (non-framed). */
void handle_response_block(int fd, unsigned long request_number,
                           BridgeEngine *eng,
                           const HarnessRequest *req,
                           const char *out_buf);

#endif
