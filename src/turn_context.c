#include "turn_context.h"
#include "config_manager.h"
#include "json_utils.h"
#include "native_response.h"
#include "server.h"
#include "debug_trace.h"
#include "native_frame.h"
#include "tool_policy.h"
#include "tool_runner.h"
#include "tool_history.h"
#include "responses_stream_state.h"
#include "codex_response_formatter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>

/* -------------------------------------------------------------------
 *  TurnContext lifecycle implementation
 * ------------------------------------------------------------------- */

void turn_context_init(TurnContext *ctx, BridgeEngine *eng, unsigned long request_number,
                       const char *input_text, const char *previous_response_id,
                       const char *reasoning_effort, bool reset_session,
                       char *out_buf, size_t out_max,
                       int live_client_fd,
                       ResponsesStreamState *stream_state,
                       int server_fd) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->eng = eng;
    ctx->request_number = request_number;
    ctx->input_text = input_text;
    ctx->previous_response_id = previous_response_id;
    ctx->reasoning_effort = reasoning_effort;
    ctx->reset_session = reset_session;
    ctx->out_buf = out_buf;
    ctx->out_max = out_max;
    ctx->live_fd = (live_client_fd >= 0 ? live_client_fd : -1);
    ctx->server_fd = (server_fd >= 0 ? server_fd : -1);
    ctx->delta_seq = 0;
    ctx->stream_state = stream_state;
    ctx->event_limit_exceeded = false;
    ctx->tool_calls = 0;
    clock_gettime(CLOCK_MONOTONIC, &ctx->start_ts);
}

/* Emit a single structured analytics line per turn outcome. Consumed by
 * scripts/analytics.py to report latency, throughput (tk/s), tool usage, and
 * steering (reasoning_effort) effectiveness. completion_tokens may be 0 when the
 * native agent does not report usage; the analyzer falls back to output bytes. */
static void emit_turn_metrics(TurnContext *ctx, const char *status,
                              int prompt_tokens, int completion_tokens) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long duration_ms = (now.tv_sec - ctx->start_ts.tv_sec) * 1000L +
                       (now.tv_nsec - ctx->start_ts.tv_nsec) / 1000000L;
    if (duration_ms < 0) duration_ms = 0;
    size_t output_bytes = ctx->out_buf ? strlen(ctx->out_buf) : 0;
    const char *effort = (ctx->reasoning_effort && ctx->reasoning_effort[0])
                             ? ctx->reasoning_effort : "default";
    debug_trace_append("turn_metrics request=%lu status=%s effort=%s duration_ms=%ld "
                       "output_bytes=%zu prompt_tokens=%d completion_tokens=%d tool_calls=%d",
                       ctx->request_number, status, effort, duration_ms,
                       output_bytes, prompt_tokens, completion_tokens, ctx->tool_calls);
}

/* -------------------------------------------------------------------
 *  Phase 1: Session management + build request frame + send to native
 * ------------------------------------------------------------------- */
bool turn_begin(TurnContext *ctx) {
    BridgeEngine *eng = ctx->eng;
    ctx->active = true;

    /* Reset cancellation state from any previous turn */
    eng->cancelled = false;

    /* Generate unique request id per Codex request */
    char request_id[64];
    snprintf(request_id, sizeof(request_id), "req-%lu", ctx->request_number);
    snprintf(eng->current_request_id, sizeof(eng->current_request_id), "%s", request_id);
    snprintf(ctx->request_id, sizeof(ctx->request_id), "%s", request_id);

    /* Session management */
    const char *effective_session_id = NULL;
    bool can_load = eng->cfg->auto_load_project_session;
    bool can_create = can_load;
    if (strcmp(eng->cfg->kv_cache_policy, "manual") == 0) {
        can_load = false;
        can_create = false;
    }
    if (ctx->reset_session) {
        char time_buf[64];
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        snprintf(time_buf, sizeof(time_buf), "reset_%lld_%ld", (long long)ts.tv_sec, (long)ts.tv_nsec);
        set_session_id(time_buf);
        compute_session_key(eng->workspace, time_buf);
        const char *sk = get_session_key();
        if (sk && sk[0]) {
            debug_trace_append("session_create_reset key=%s", sk);
            char meta_buf[4096];
            build_session_metadata_json(eng, "", meta_buf, sizeof(meta_buf));
            bool created = engine_create_session_state(eng, sk,
                                        eng->workspace,
                                        eng->cfg->codex_model,
                                        eng->cfg->context_tokens,
                                        meta_buf);
            if (created) {
                snprintf(eng->last_session_id, sizeof(eng->last_session_id), "%s", time_buf);
            } else {
                debug_trace_append("session_create_reset failed key=%s", sk);
                engine_send_error(eng, "session_create_failure", "failed to create fresh session state");
            }
        }
    } else if (ctx->previous_response_id && ctx->previous_response_id[0]) {
        effective_session_id = ctx->previous_response_id;
    } else if (eng->last_session_id[0]) {
        effective_session_id = eng->last_session_id;
    } else if (can_load) {
        set_session_id("default");
        compute_session_key(eng->workspace, "default");
        const char *sk = get_session_key();
        if (sk && sk[0]) {
            load_session_index();
            const char *state_id = session_index_lookup(sk);
            if (state_id) {
                const char *stored_ws = session_index_get_workspace(sk);
                if (stored_ws && stored_ws[0] && strcmp(stored_ws, eng->workspace) != 0) {
                    debug_trace_append("session_default_restore WORKSPACE_MISMATCH stored=%s current=%s key=%s",
                                       stored_ws, eng->workspace, sk);
                    engine_send_error(eng, "session_incompatible_state", "workspace mismatch: cannot restore default session");
                    state_id = NULL;
                }
                if (state_id) {
                    const char *stored_mid = session_index_get_model_id(sk);
                    if (stored_mid && stored_mid[0] && strcmp(stored_mid, eng->cfg->codex_model) != 0) {
                        debug_trace_append("session_default_restore MODEL_MISMATCH stored=%s current=%s key=%s",
                                           stored_mid, eng->cfg->codex_model, sk);
                        engine_send_error(eng, "session_incompatible_state", "model mismatch: cannot restore default session");
                        state_id = NULL;
                    }
                }
            }
            if (state_id) {
                debug_trace_append("session_default_restore key=%s state_id=%s", sk, state_id);
                bool loaded = engine_load_session_state(eng, sk, state_id);
                if (!loaded) {
                    debug_trace_append("session_default_restore load_failed key=%s state_id=%s", sk, state_id);
                    engine_send_error(eng, "session_corrupt_state", "failed to load default session: native agent returned error");
                } else {
                    snprintf(eng->last_session_id, sizeof(eng->last_session_id), "%s", "default");
                }
            } else if (can_create) {
                debug_trace_append("session_create_no_default key=%s", sk);
                char meta_buf[4096];
                build_session_metadata_json(eng, "", meta_buf, sizeof(meta_buf));
                bool created = engine_create_session_state(eng, sk,
                                            eng->workspace,
                                            eng->cfg->codex_model,
                                            eng->cfg->context_tokens,
                                            meta_buf);
                if (created) {
                    snprintf(eng->last_session_id, sizeof(eng->last_session_id), "%s", sk);
                } else {
                    debug_trace_append("session_create_no_default failed key=%s", sk);
                    engine_send_error(eng, "session_create_failure", "failed to create default session state");
                }
            }
        }
    }
    if (effective_session_id) {
        set_session_id(effective_session_id);
        compute_session_key(eng->workspace, effective_session_id);
        const char *sk = get_session_key();
        if (sk && sk[0]) {
            load_session_index();
            const char *state_id = session_index_lookup(sk);
            if (eng->last_session_id[0] && strcmp(eng->last_session_id, effective_session_id) != 0) {
                char old_sk[512];
                session_key_for(eng->workspace, eng->last_session_id, old_sk, sizeof(old_sk));
                const char *old_state_id = session_index_lookup(old_sk);
                debug_trace_append("session_switch from=%s to=%s old_state=%s new_state=%s",
                                   eng->last_session_id, effective_session_id,
                                   old_state_id ? old_state_id : "none",
                                   state_id ? state_id : "none");
                if (old_state_id && eng->cfg->auto_save_kv_cache) {
                    bool saved = engine_save_session_state(eng, old_sk, old_state_id);
                    if (!saved) {
                        debug_trace_append("session_switch save_failed key=%s", old_sk);
                        engine_send_error(eng, "session_save_failure", "failed to save previous session state");
                    }
                }
                if (state_id && can_load) {
                    bool switched = engine_switch_session_state(eng, old_sk, sk, state_id);
                    if (!switched) {
                        debug_trace_append("session_switch switch_failed to_key=%s", sk);
                        engine_send_error(eng, "session_switch_failure", "native agent failed to switch session state");
                    }
                } else {
                    debug_trace_append("session_switch no_new_state key=%s", sk);
                    if (can_load && !state_id) {
                        engine_send_error(eng, "session_state_missing", "target session state not found in index");
                    }
                }
            } else if (!eng->last_session_id[0]) {
                if (state_id && can_load) {
                    const char *stored_ws = session_index_get_workspace(sk);
                    if (stored_ws && stored_ws[0] && strcmp(stored_ws, eng->workspace) != 0) {
                        debug_trace_append("session_load WORKSPACE_MISMATCH stored=%s current=%s key=%s",
                                           stored_ws, eng->workspace, sk);
                        engine_send_error(eng, "session_incompatible_state", "workspace mismatch: cannot load saved state");
                        state_id = NULL;
                    }
                    if (state_id) {
                        const char *stored_mid = session_index_get_model_id(sk);
                        if (stored_mid && stored_mid[0] && strcmp(stored_mid, eng->cfg->codex_model) != 0) {
                            debug_trace_append("session_load MODEL_MISMATCH stored=%s current=%s key=%s",
                                               stored_mid, eng->cfg->codex_model, sk);
                            engine_send_error(eng, "session_incompatible_state", "model mismatch: cannot load saved state");
                            state_id = NULL;
                        }
                    }
                    if (state_id) {
                        debug_trace_append("session_load first_load key=%s state_id=%s", sk, state_id);
                        bool loaded = engine_load_session_state(eng, sk, state_id);
                        if (!loaded) {
                            debug_trace_append("session_load load_failed key=%s state_id=%s", sk, state_id);
                            engine_send_error(eng, "session_corrupt_state", "failed to load KV cache: native agent returned error");
                        }
                    }
                } else {
                    debug_trace_append("session_load no_saved_state key=%s", sk);
                    if (can_load && !state_id) {
                        engine_send_error(eng, "session_state_missing", "no saved state found for this session");
                    }
                }
            }
            snprintf(eng->last_session_id, sizeof(eng->last_session_id), "%s", effective_session_id);
        }
    }

    /* Build request frame with tool history (omit when hide_tool_transcripts is set) */
    char *tool_history_json = tool_history_build_json();
    const char *history_arg = (global_config.hide_tool_transcripts || !tool_history_json || !tool_history_json[0] || strcmp(tool_history_json, "[]") == 0)
                              ? NULL : tool_history_json;
    char *req = frame_build_request(request_id, ctx->input_text, "", "",
                                    ctx->reasoning_effort,
                                    ctx->previous_response_id,
                                    eng->cfg->auto_load_resume_session,
                                    eng->cfg->context_tokens,
                                    ctx->reset_session,
                                    history_arg);
    if (!req) {
        free(tool_history_json);
        return false;
    }
    trace_store_text(ctx->trace_initialized ? NULL : NULL, 0, NULL); /* no-op guard */
    trace_store_text(g_trace_session.native_request, sizeof(g_trace_session.native_request), req);
    char req_sample[512];
    debug_trace_compact_text(req, req_sample, sizeof(req_sample));
    debug_trace_append("bridge_to_native request=%lu protocol=framed payload_bytes=%zu sample=\"%s\"",
                       ctx->request_number, strlen(req), req_sample);
    engine_write_frame(eng, req, strlen(req));
    free(req);
    free(tool_history_json);
    return true;
}

/* -------------------------------------------------------------------
 *  Phase 2: Wait for native ack frame
 * ------------------------------------------------------------------- */
bool turn_await_ack(TurnContext *ctx) {
    BridgeEngine *eng = ctx->eng;
    /* Retry loop: skip pong/ping/health frames that might be stolen from heartbeat */
    int ack_retries = 5;
    while (ack_retries-- > 0) {
        size_t ack_len = 0;
        char *ack = engine_read_frame_timeout(eng, &ack_len, eng->cfg->response_timeout_ms);
        if (!ack) {
            snprintf(ctx->out_buf, ctx->out_max, "native agent ack timeout");
            trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), ctx->out_buf);
            debug_trace_append("native_to_bridge request=%lu status=ack_timeout timeout_ms=%d",
                               ctx->request_number, eng->cfg->response_timeout_ms);
            engine_send_error(eng, "handshake_timeout", "ack frame timeout");
            return false;
        }

        /* Parse ack frame with native_response parser */
        NativeEvent ev;
        native_parse_frame(ack, ack_len, &ev);
        bool is_ack = ev.type == NATIVE_EVENT_ACK;
        bool id_match = strstr(ack, ctx->request_id) != NULL;
        bool accepted = strcmp(ev.data.response.status, "accepted") == 0;
        bool busy = strcmp(ev.data.response.status, "busy") == 0;

        /* Skip lifecycle frames that belong to heartbeat */
        if (native_event_is_lifecycle(&ev) || strstr(ack, "\"type\":\"event\"") != NULL) {
            debug_trace_append("native_to_bridge request=%lu status=skip_non_ack type=%.10s",
                               ctx->request_number, ev.type_str);
            free(ack);
            continue;
        }

        if (!is_ack || !id_match) {
            snprintf(ctx->out_buf, ctx->out_max, "native agent ack invalid or rejected");
            trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), ctx->out_buf);
            debug_trace_append("native_to_bridge request=%lu status=ack_invalid is_ack=%d id_match=%d",
                               ctx->request_number, is_ack, id_match);
            free(ack);
            return false;
        }
        if (busy) {
            int retry_ms = 0;
            char retry_buf[32] = {0};
            if (extract_json_string_field(ack, "retry_after_ms", retry_buf, sizeof(retry_buf))) {
                retry_ms = atoi(retry_buf);
            }
            snprintf(ctx->out_buf, ctx->out_max, "native agent busy (retry_after_ms=%d)", retry_ms);
            trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), ctx->out_buf);
            debug_trace_append("native_to_bridge request=%lu status=busy retry_after_ms=%d",
                               ctx->request_number, retry_ms);
            engine_send_error(eng, "native_busy", "agent busy");
            free(ack);
            return false;
        }
        if (!accepted) {
            snprintf(ctx->out_buf, ctx->out_max, "native agent ack rejected (unknown status)");
            trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), ctx->out_buf);
            debug_trace_append("native_to_bridge request=%lu status=ack_rejected",
                               ctx->request_number);
            free(ack);
            return false;
        }
        debug_trace_append("native_to_bridge request=%lu status=ack_accepted",
                           ctx->request_number);
        free(ack);
        return true;
    }
    snprintf(ctx->out_buf, ctx->out_max, "native agent ack retry exhausted");
    trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), ctx->out_buf);
    debug_trace_append("native_to_bridge request=%lu status=ack_retry_exhausted",
                       ctx->request_number);
    engine_send_error(eng, "handshake_timeout", "ack retry exhausted");
    return false;
}

/* -------------------------------------------------------------------
 *  Mid-turn control plane: handle new HTTP connections on the server listen fd
 *  during a long turn. This allows cancellation, model queries, and concurrent
 *  request rejection without waiting for the turn to finish.
 * ------------------------------------------------------------------- */
static void handle_midturn_control_plane(TurnContext *ctx) {
    int sfd = ctx->server_fd;
    if (sfd < 0) return;

    /* Non-blocking accept loop: drain all pending connections */
    struct sockaddr_storage addr;
    socklen_t addrlen = sizeof(addr);

    while (1) {
        /* Poll for a pending connection with a 0ms timeout so we never block the
         * turn on accept. The listen fd stays blocking; only ready connections are
         * accepted. */
        struct pollfd lpf = { .fd = sfd, .events = POLLIN, .revents = 0 };
        int pr = poll(&lpf, 1, 0);
        if (pr <= 0 || !(lpf.revents & POLLIN)) break;
        int cfd = accept(sfd, (struct sockaddr *)&addr, &addrlen);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            break;
        }

        /* Read HTTP request line */
        char buf[4096];
        int n = read(cfd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            close(cfd);
            continue;
        }
        buf[n] = '\0';

        /* Parse first line: METHOD PATH HTTP/1.1 */
        char method[16], path[512];
        method[0] = path[0] = '\0';
        int parsed = sscanf(buf, "%15s %511s", method, path);
        if (parsed < 2) {
            close(cfd);
            continue;
        }

        const char *content_type = "application/json";
        int status_code = 200;
        char canned_response[4096];
        const char *response_body = NULL;

        if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/models") == 0) {
            const char *mid = (ctx->eng->cfg && ctx->eng->cfg->codex_model[0])
                                ? ctx->eng->cfg->codex_model : "star-bridge-ds4";
            snprintf(canned_response, sizeof(canned_response),
                "{\"object\":\"list\",\"data\":[{\"id\":\"%s\",\"object\":\"model\","
                "\"created\":1700000000,\"owned_by\":\"Star Bridge Local\",\"tools\":[]}]}", mid);
            response_body = canned_response;
        } else if (strcmp(method, "DELETE") == 0 && strncmp(path, "/v1/responses/", 14) == 0) {
            /* Cancel request — forward a best-effort cancel frame to the agent and
             * flag the engine. engine_cancel sets eng->cancelled AND writes the cancel
             * frame (idempotent), so call it before the flag is otherwise set. The turn
             * loop checks eng->cancelled after this handler and ends the live stream with
             * response.failed (reason "cancelled") within one poll cycle. */
            BridgeEngine *eng = ctx->eng;
            engine_cancel(eng);
            engine_set_turn_active(eng, false);
            snprintf(canned_response, sizeof(canned_response),
                "{\"object\":\"response\",\"status\":\"cancelled\"}");
            response_body = canned_response;
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/responses") == 0) {
            /* Concurrent request — reject with 409 */
            status_code = 409;
            snprintf(canned_response, sizeof(canned_response),
                "{\"object\":\"error\",\"type\":\"conflict\",\"code\":\"turn_active\","
                "\"message\":\"A turn is already in progress for this session.\"}");
            response_body = canned_response;
        } else {
            status_code = 404;
            snprintf(canned_response, sizeof(canned_response),
                "{\"object\":\"error\",\"type\":\"not_found\",\"message\":\"Not found\"}");
            response_body = canned_response;
        }

        /* Send HTTP response and close */
        char http_resp[8192];
        const char *status_text =
            (status_code == 200) ? "OK" :
            (status_code == 409) ? "Conflict" : "Not Found";
        int rlen = snprintf(http_resp, sizeof(http_resp),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            status_code, status_text, content_type,
            strlen(response_body), response_body);
        write(cfd, http_resp, rlen);
        close(cfd);
    }
}

/* -------------------------------------------------------------------
 *  Phase 3: Process events/tools loop until turn completion
 * ------------------------------------------------------------------- */
bool turn_process_events(TurnContext *ctx) {
    BridgeEngine *eng = ctx->eng;
    ctx->event_prefix_len = 0;
    ctx->event_prefix[0] = '\0';

    /* Event processing loop.
     * Legacy 32-step cap was for bridge-orchestrated tool roundtrips (TOOL_INTENT -> result write).
     * For framed streaming agents (ds4 wrapper), we receive many text_delta + periodic "thinking"
     * pings per logical turn. Use a high cap on total events, and only treat tool cycles as
     * "steps" that can run away. This prevents "tool loop exceeded" after one block of output
     * + pings during long internal tool use or thinking. */
    int event_count = 0;
    int max_events = (eng->cfg ? eng->cfg->max_turn_events : 65536);
    int tool_cycles = 0;
    const int MAX_TOOL_CYCLES = 64;
    const int HEARTBEAT_POLL_MS = 2000;
    const int CONTROL_PLANE_POLL_MS = 100;  /* poll interval when mid-turn control plane active */
    int remaining_timeout = eng->cfg->response_timeout_ms > 0 ? eng->cfg->response_timeout_ms : 60000;

    /* NOTE: we deliberately do NOT set the listen fd O_NONBLOCK here. On macOS/BSD
     * accepted client sockets inherit the listen socket's O_NONBLOCK flag, which would
     * make the main accept loop read empty and RST every later connection. Instead the
     * control plane polls the listen fd for readiness (0ms) before each accept, so the
     * fd stays blocking and accepted client sockets stay blocking. */

    while (event_count < max_events) {
        size_t rlen = 0;
        int poll_ms = (remaining_timeout < HEARTBEAT_POLL_MS) ? remaining_timeout : HEARTBEAT_POLL_MS;
        /* When mid-turn control plane is active, use a shorter poll interval so we
         * can poll the server listen fd for cancellation/model/concurrent requests. */
        if (ctx->server_fd >= 0 && poll_ms > CONTROL_PLANE_POLL_MS) {
            poll_ms = CONTROL_PLANE_POLL_MS;
        }
        /* Check for mid-turn control plane requests before each frame read */
        if (ctx->server_fd >= 0) {
            handle_midturn_control_plane(ctx);
            /* A mid-turn DELETE sets eng->cancelled. End the turn promptly (within one
             * 100ms poll cycle) so the caller fails the live stream with response.failed.
             * ds4 may keep computing in the background — agent-limited, accepted. */
            if (eng->cancelled) {
                snprintf(ctx->out_buf, ctx->out_max, "cancelled");
                debug_trace_append("native_to_bridge request=%lu status=cancelled_by_client",
                                   ctx->request_number);
                emit_turn_metrics(ctx, "cancelled", 0, 0);
                return false;
            }
        }
        char *resp = engine_read_frame_timeout(eng, &rlen, poll_ms);
        if (!resp) {
            /* Send heartbeat to client SSE to prevent Codex timeout during idle */
            if (ctx->live_fd >= 0) {
                send_sse_heartbeat(ctx->live_fd);
            }
            remaining_timeout -= poll_ms;
            if (remaining_timeout <= 0) {
                /* Check for control plane requests before declaring timeout */
                if (ctx->server_fd >= 0) {
                    handle_midturn_control_plane(ctx);
                }
                snprintf(ctx->out_buf, ctx->out_max, "native agent response timeout");
                trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), ctx->out_buf);
                debug_trace_append("native_to_bridge request=%lu status=timeout timeout_ms=%d",
                                    ctx->request_number, eng->cfg->response_timeout_ms);
                engine_send_error(eng, "handshake_timeout", "native agent response timeout");
                emit_turn_metrics(ctx, "timeout", 0, 0);
                return false;
            }
            continue;
        }
        /* Reset remaining timeout when a frame arrives */
        remaining_timeout = eng->cfg->response_timeout_ms > 0 ? eng->cfg->response_timeout_ms : 60000;
        trace_store_text(g_trace_session.native_response, sizeof(g_trace_session.native_response), resp);
        char resp_sample[512];
        debug_trace_compact_text(resp, resp_sample, sizeof(resp_sample));
        /* Parse frame with schema-aware parser */
        NativeEvent ev;
        native_parse_frame(resp, rlen, &ev);
        debug_trace_append("native_to_bridge request=%lu status=%s response_bytes=%zu sample=\"%s\"",
                            ctx->request_number,
                            ev.data.response.status[0] ? ev.data.response.status : "(none)",
                            rlen,
                            resp_sample);

        /* Live SSE activity for streaming turns: every frame we receive from the native
         * (especially text_delta and the periodic "thinking" events from the ds4 wrapper)
         * gives us a chance to send a heartbeat or delta to Codex. This prevents the
         * client from seeing a long silent SSE and deciding to "Reconnect".
         * Wrapper pings (2) + bridge forwarding to client is more optimal and efficient
         * than pure bridge synthetic (3), because the pings originate from the actual
         * agent reader (knows if the process is alive), are cheap, and when real output
         * arrives we send real content instead of fake "thinking" text.
         */
        if (ctx->live_fd >= 0) {
            send_sse_heartbeat(ctx->live_fd);
        }

        /* Validate that every native frame echoes the same request id,
         * except allowlisted lifecycle/health frames */
        bool has_id = ev.id[0] != '\0';
        bool id_match = strstr(resp, ctx->request_id) != NULL;
        if (has_id && !id_match) {
            if (!native_event_is_lifecycle(&ev)) {
                snprintf(ctx->out_buf, ctx->out_max, "native agent frame id mismatch");
                trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), ctx->out_buf);
                debug_trace_append("native_to_bridge request=%lu status=id_mismatch expected=%s",
                                   ctx->request_number, ctx->request_id);
                engine_send_error(eng, "request_id_mismatch", "frame id does not match request id");
                free(resp);
                return false;
            }
            debug_trace_append("native_to_bridge request=%lu status=allowlisted_frame type=%.10s",
                               ctx->request_number, ev.type_str);
            free(resp);
            continue;
        }

        /* Live text delta from native (e.g. ds4 wrapper sending partial output).
         * Append for final out_buf. The heartbeat was already sent above.
         * If we have live_fd (headers sent early), we could emit a more precise
         * delta event here using the Responses format, but the heartbeat + final
         * chunked emission from out_buf is sufficient and efficient to stop reconnects.
         */
        if (ev.type == NATIVE_EVENT_TEXT_DELTA) {
            size_t ti = strlen(ctx->out_buf);
            ti = append_text(ctx->out_buf, ctx->out_max, ti, ev.data.response.text);
            if (ctx->stream_state && ev.data.response.text[0]) {
                responses_stream_emit_text_delta(ctx->stream_state, ev.data.response.text);
            } else if (ctx->live_fd >= 0 && ev.data.response.text[0]) {
                /* Fallback basic emit if no full state */
                char evdata[8192];
                snprintf(evdata, sizeof(evdata), "{\"delta\":\"%s\"}", ev.data.response.text);
                send_sse(ctx->live_fd, "response.output_text.delta", evdata, ctx->delta_seq++);
            }
            free(resp);
            event_count++;
            continue;
        }

        if (ev.type == NATIVE_EVENT_UNKNOWN) {
            if (strstr(resp, "\"type\":\"event\"")) {
                free(resp);
                event_count++;
                continue;
            }
            /* Periodic "thinking" ping from ds4 wrapper during long silences.
             * We already sent a heartbeat above. Do not copy the JSON into the
             * final out_buf (would pollute the answer).
             */
            if (strstr(resp, "\"thinking\"") || strstr(resp, "thinking")) {
                free(resp);
                event_count++;
                continue;
            }
        }

        /* Compaction event */
        if (ev.type == NATIVE_EVENT_COMPACTION) {
            const char *msg = ev.data.compaction.message[0] ? ev.data.compaction.message : "Native agent is compacting context.";
            ctx->event_prefix_len = append_compaction_notice(ctx->event_prefix, sizeof(ctx->event_prefix),
                                                             ctx->event_prefix_len, msg);
            free(resp);
            event_count++;
            continue;
        }

        /* Cancellation acknowledgement */
        if (ev.type == NATIVE_EVENT_CANCELLED) {
            debug_trace_append("native_to_bridge request=%lu status=cancellation_acknowledged",
                               ctx->request_number);
            free(resp);
            return false;
        }

        if (ev.type == NATIVE_EVENT_ERROR) {
            const char *msg = ev.data.error.message[0] ? ev.data.error.message : "native agent error";
            snprintf(ctx->out_buf, ctx->out_max, "%s", msg);
            trace_store_text(g_trace_session.error, sizeof(g_trace_session.error), ctx->out_buf);
            debug_trace_append("native_to_bridge request=%lu status=error message=\"%s\"",
                               ctx->request_number, msg);
            free(resp);
            emit_turn_metrics(ctx, "error", 0, 0);
            return false;
        }

        /* Tool intent handling */
        if (ev.type == NATIVE_EVENT_TOOL_INTENT) {
            tool_cycles++;
            ctx->tool_calls++;
            if (tool_cycles > MAX_TOOL_CYCLES) {
                snprintf(ctx->out_buf, ctx->out_max, "native agent tool cycle limit exceeded");
                debug_trace_append("native_to_bridge request=%lu status=tool_cycles_exceeded", ctx->request_number);
                free(resp);
                return false;
            }
            char *tool_name = ev.data.tool_intent.tool_name;
            char *tool_args = ev.data.tool_intent.tool_args;

            /* Save tool details for structured summary */
            snprintf(ctx->last_tool_name, sizeof(ctx->last_tool_name), "%s", tool_name ? tool_name : "");
            snprintf(ctx->last_tool_args, sizeof(ctx->last_tool_args), "%s", tool_args && tool_args[0] ? tool_args : "{}");
            memset(&ctx->last_tool_run, 0, sizeof(ctx->last_tool_run));

            char *tool_frame = NULL;
            if (tool_policy_is_denied(tool_name)) {
                const char *message = tool_policy_error_message(TOOL_ERROR_DENIED);
                tool_frame = frame_build_tool_error(ctx->request_id, tool_name, message);
                char history_line[512];
                tool_policy_build_error_summary(tool_name, TOOL_ERROR_DENIED, NULL, history_line, sizeof(history_line));
                tool_history_append(history_line);
                ctx->last_tool_run.denied = true;
                ctx->last_tool_run.ok = false;
            } else if (!tool_policy_args_valid(tool_name, tool_args)) {
                const char *message = tool_policy_error_message(TOOL_ERROR_MALFORMED_ARGS);
                tool_frame = frame_build_tool_error(ctx->request_id, tool_name, message);
                char history_line[512];
                tool_policy_build_error_summary(tool_name, TOOL_ERROR_MALFORMED_ARGS, tool_args, history_line, sizeof(history_line));
                tool_history_append(history_line);
                ctx->last_tool_run.ok = false;
                ctx->last_tool_run.exit_code = 1;
            } else {
                ToolRunResult run = {0};
                bool tool_ok = tool_runner_run(tool_name, tool_args, NULL,
                                               eng->cfg->agent_env,
                                               eng->cfg->extra_native_args,
                                               eng->cfg->browser_state_dir,
                                               eng->cfg->preferred_browser,
                                               eng->cfg->shell_command_enabled,
                                               tool_timeout_ms(tool_name), &run);
                log_tool_diagnostics(&run);
                tool_frame = frame_build_tool_result(ctx->request_id, tool_name,
                                                     tool_ok ? run.result_json : "{\"status\":\"error\"}");
                char history_line[1024];
                snprintf(history_line, sizeof(history_line), "TOOL_RESULT %s %.*s\n", tool_name, 256, tool_args);
                tool_history_append(history_line);
                /* Save run result for structured summary */
                memcpy(&ctx->last_tool_run, &run, sizeof(run));
            }
            free(resp);
            if (!tool_frame) {
                snprintf(ctx->out_buf, ctx->out_max, "native agent tool result build failed");
                return false;
            }
            engine_write_frame(eng, tool_frame, strlen(tool_frame));
            char tool_sample[512];
            debug_trace_compact_text(tool_frame, tool_sample, sizeof(tool_sample));
            debug_trace_append("bridge_to_native request=%lu protocol=framed tool_result_bytes=%zu sample=\"%s\"",
                               ctx->request_number, strlen(tool_frame), tool_sample);
            free(tool_frame);
            event_count++;
            continue;
        }

        /* Response text handling - only extract from top-level "text" in NATIVE_EVENT_RESPONSE */
        if (ev.type == NATIVE_EVENT_RESPONSE) {
            size_t ti = 0;
            if (ctx->event_prefix_len > 0) {
                ti = append_text(ctx->out_buf, ctx->out_max, ti, ctx->event_prefix);
            }
            ti = append_text(ctx->out_buf, ctx->out_max, ti, ev.data.response.text);

            /* If streaming is active, emit the full response text as text_delta events
             * so that Codex receives incremental text even when the native agent sends a
             * single response frame instead of incremental text_delta frames. */
            if (ctx->stream_state && ev.data.response.text[0]) {
                /* Emit in chunks to keep events manageable */
                size_t text_len = strlen(ev.data.response.text);
                size_t chunk_size = 4096;
                for (size_t off = 0; off < text_len; off += chunk_size) {
                    size_t clen = text_len - off;
                    if (clen > chunk_size) clen = chunk_size;
                    char chunk[4096 + 1];
                    memcpy(chunk, ev.data.response.text + off, clen);
                    chunk[clen] = '\0';
                    if (!responses_stream_emit_text_delta(ctx->stream_state, chunk)) {
                        break;
                    }
                }
            } else if (ctx->live_fd >= 0 && ev.data.response.text[0]) {
                /* Fallback basic emit if no full state */
                char evdata[8192];
                snprintf(evdata, sizeof(evdata), "{\"delta\":\"%s\"}", ev.data.response.text);
                send_sse(ctx->live_fd, "response.output_text.delta", evdata, ctx->delta_seq++);
            }

            /* Null-terminate at current buffer length */
            ctx->out_buf[strlen(ctx->out_buf)] = '\0';
            free(resp);
            /* After successful turn completion, save session state */
            if (eng->last_session_id[0] && eng->cfg->auto_save_kv_cache) {
                set_session_id(eng->last_session_id);
                compute_session_key(eng->workspace, eng->last_session_id);
                const char *save_sk = get_session_key();
                if (save_sk && save_sk[0]) {
                    load_session_index();
                    const char *save_state = session_index_lookup(save_sk);
                    if (save_state) {
                        debug_trace_append("session_save_after_turn key=%s state_id=%s", save_sk, save_state);
                        engine_save_session_state(eng, save_sk, save_state);
                    } else {
                        debug_trace_append("session_save_after_turn no_state_to_save key=%s", save_sk);
                    }
                }
            }
            emit_turn_metrics(ctx, "completed", ev.data.response.prompt_tokens,
                              ev.data.response.completion_tokens);
            return true;
        } else {
            size_t ti = 0;
            if (ctx->event_prefix_len > 0) {
                ti = append_text(ctx->out_buf, ctx->out_max, ti, ctx->event_prefix);
            }
            size_t copy_len = rlen;
            if (copy_len > ctx->out_max - ti - 1) copy_len = ctx->out_max - ti - 1;
            memcpy(ctx->out_buf + ti, resp, copy_len);
            ctx->out_buf[ti + copy_len] = '\0';
            free(resp);
        }
        event_count++;
    }

    /* Event limit reached: return partial output with incomplete_details instead of error.
     * The caller (server.c) will emit response.completed with incomplete_details set to
     * "max_turn_events" so Codex shows the partial output rather than an error. */
    debug_trace_append("native_to_bridge request=%lu status=event_limit_exceeded events=%d partial=%.200s",
                       ctx->request_number, event_count, ctx->out_buf);
    ctx->event_limit_exceeded = true;
    emit_turn_metrics(ctx, "incomplete", 0, 0);
    return true;
}

/* -------------------------------------------------------------------
 *  Phase 4: Cleanup — clear turn-active flag and cancellation state
 * ------------------------------------------------------------------- */
void turn_cleanup(TurnContext *ctx) {
    if (!ctx || !ctx->active) return;
    BridgeEngine *eng = ctx->eng;
    engine_set_turn_active(eng, false);
    ctx->active = false;
    /* Reset cancellation flag so the next turn starts clean */
    eng->cancelled = false;
}
