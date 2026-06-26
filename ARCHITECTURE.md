# Architecture

Star Bridge is a small C gateway that makes a local ds4 agent look like an OpenAI
Responses API endpoint to the Codex desktop app. It owns the HTTP/SSE surface, a
framed agent protocol, session bookkeeping, and turn lifecycle — it does **not** own
a browser or any model routing.

```
Codex Desktop ──HTTP /v1/responses, /v1/models──▶  star_bridge (C)
                                                      │  framed JSON over stdio/UDS
                                                      ▼
                                            agent/ds4_wrapper.py  ──▶  ds4-agent
```

## Module map (`src/`)

| Area | Files |
| --- | --- |
| Server / HTTP / SSE | `server.c` (accept loop, routing, auth, host allowlist, request decode, block + stream writers) |
| Turn lifecycle | `turn_context.c` (begin → await ack → process events → cleanup; session switch; mid-turn control plane) |
| Engine / transport | `bridge_core.c`, `native_connection.c`, `native_frame.c`, `native_response.c`, `uds_transport.c` |
| Request parse / normalize | `codex_request_parser.c`, `codex_tool_normalizer.c`, `codex_tool_detector.c` |
| Response / stream format | `codex_response_formatter.c`, `codex_stream_events.c`, `responses_stream_state.c`, `responses_api.c` |
| Capability gating | `capability_router.c`, `tool_policy.c`, `tool_runner.c`, `tool_history.c` |
| Config / managed Codex config | `config_manager.c` (config load/validate, `--generate-config`, `--install`/`--disable`, picker catalog) |
| Diagnostics | `main.c` (`--doctor`), `debug_trace.c`, `json_log.c` |
| Vendored | `vendor/cjson/` (MIT) |

## Frame protocol (bridge ⇄ wrapper)

Newline-delimited JSON frames. The bridge sends a **request** frame carrying the
normalized input, `previous_response_id`, `reasoning_effort`, `context_tokens`, and
session ops. The wrapper replies with an **ack**, then a stream of events:

- `text_delta` — incremental assistant text (each counts as one turn event).
- `response` (`status: in_progress|completed`) — final or staged output.
- `compaction` — agent is compacting context (surfaced as a notice).
- `error` — structured native failure (returned immediately after ack).
- `cancelled` — cancellation acknowledgement.
- session-state frames — save / switch / create / load (see Sessions).

## Turn lifecycle

`turn_begin → turn_await_ack → turn_process_events → turn_cleanup`
(`src/turn_context.c`). `turn_process_events` polls the agent connection on a
heartbeat interval and, when a turn is active, **also polls the listening socket**
(0ms `poll()` before `accept()`) so the mid-turn control plane can answer requests
without blocking the turn.

## Mid-turn control plane

During an active turn the bridge still answers:

- `GET /v1/models` — canned model list.
- `DELETE /v1/responses/<id>` — sets cancel, forwards a best-effort cancel frame to
  the agent, and ends the live SSE stream with `response.failed` (reason `cancelled`)
  within ~one 100ms poll cycle. ds4 may keep computing in the background (agent-limited).
- `POST /v1/responses` (concurrent) — rejected with `409`.

The listening fd is published to the turn code via `g_server_fd`; it stays **blocking**
and is polled for readiness, so accepted client sockets are never left non-blocking.

## Sessions (per-conversation mapping)

`session_id` is derived per turn from the request's `previous_response_id`
(`turn_context.c`), keyed through `compute_session_key(workspace, session_id)` into a
workspace+session index. When the key changes between turns the bridge saves the prior
session state and switches to the target (`engine_switch_session_state`), emitting
`session_switch from=<old> to=<new>`. Workspace- and model-mismatch are rejected with
`session_incompatible_state`.

## Limits and truncation (no silent loss)

- **Input**: heap-backed, growable (`HarnessRequest.normalized_input/instructions`,
  `input_items`). Over-ceiling requests are **loudly** rejected — the HTTP body cap
  `max_request_size` (413) or the parser cap `max_request_input_bytes` (400,
  `request_input_too_large`). Never a silent clip.
- **Output**: `max_output_buffer` / `max_output_chars` clip non-streamed output; the clip
  is logged and surfaced via `incomplete_details`.
- **Turn events**: at `max_turn_events` the turn **completes with partial output** and
  `incomplete_details=max_turn_events` instead of failing.

## Reasoning effort = agent restart

Changing reasoning effort restarts ds4: the wrapper saves the session, relaunches ds4
with the new think flag (`--nothink`/`--think`/`--think-max`), and switches back. Expect
a pause of model-load duration. This is documented behavior, not a bug.
