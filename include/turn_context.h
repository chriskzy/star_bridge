#ifndef TURN_CONTEXT_H
#define TURN_CONTEXT_H

#include "bridge_core.h"
#include "responses_stream_state.h"
#include "tool_runner.h"

/* TurnContext encapsulates a single Codex request turn sent to the native agent.
 * Turn lifecycle (framed protocol):
 *   turn_context_init()  ->  turn_begin()  ->  turn_await_ack()  ->  turn_process_events()  ->  turn_cleanup()
 * Every early-return path in turn_begin / turn_await_ack / turn_process_events
 * must clear active/cancel state via turn_cleanup() before returning false. */
typedef struct TurnContext {
    BridgeEngine *eng;
    unsigned long request_number;
    char request_id[64];
    const char *input_text;
    const char *previous_response_id;
    const char *reasoning_effort;
    bool reset_session;
    char *out_buf;
    size_t out_max;
    bool active;                     /* turn active flag for cleanup */
    char event_prefix[8192];         /* accumulated compaction notices */
    size_t event_prefix_len;
    bool trace_initialized;          /* true after trace fields are set */
    int live_fd;                     /* client fd for live SSE emission during streaming turn (-1 = not live) */
    int server_fd;                   /* server listen fd for mid-turn control plane (-1 = disabled) */
    int delta_seq;                   /* sequence for SSE deltas */
    ResponsesStreamState *stream_state;  /* if non-NULL, use for live Responses event emission */

    /* Last tool-execution details, used for structured summary synthesis */
    char last_tool_name[64];
    char last_tool_args[2048];
    ToolRunResult last_tool_run;

    /* Flag set by turn_process_events when the event limit (max_turn_events) is reached.
     * The caller should emit response.completed with incomplete_details instead of error. */
    bool event_limit_exceeded;
} TurnContext;

/* Initialize a TurnContext from the caller's parameters.
 * live_client_fd: pass the client socket fd when doing live streaming emission for this turn
 *                 (so text_delta can be forwarded to SSE immediately). Pass -1 for normal case.
 * server_fd: pass the server listen socket fd so mid-turn control plane can poll for new
 *            connections during a long turn. Pass -1 to disable mid-turn control plane.
 */
void turn_context_init(TurnContext *ctx, BridgeEngine *eng, unsigned long request_number,
                       const char *input_text, const char *previous_response_id,
                       const char *reasoning_effort, bool reset_session,
                       char *out_buf, size_t out_max,
                       int live_client_fd,
                       ResponsesStreamState *stream_state,
                       int server_fd);

/* Phase 1: Session management + build request frame + send to native.
 * Returns false on failure (caller must then call turn_cleanup). */
bool turn_begin(TurnContext *ctx);

/* Phase 2: Wait for native ack frame.
 * Returns false on failure (caller must then call turn_cleanup). */
bool turn_await_ack(TurnContext *ctx);

/* Phase 3: Process events/tools loop until turn completion.
 * Returns false on failure (caller must then call turn_cleanup).
 * Returns true when the turn produced output text in ctx->out_buf. */
bool turn_process_events(TurnContext *ctx);

/* Phase 4: Cleanup — clear turn-active flag and cancellation state. */
void turn_cleanup(TurnContext *ctx);

/* Raw-protocol helper: write input, wait for output, read into out_buf. */
bool turn_raw_send_and_wait(TurnContext *ctx);

#endif /* TURN_CONTEXT_H */
