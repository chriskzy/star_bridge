# Star Bridge Harness Contract

Version: 0.1.0-alpha
Status: supported subset, source-backed by `make ci`

This document defines the HTTP surface a non-Codex harness may use. It is intentionally
smaller than the full OpenAI Responses API. Anything not listed here is out of contract.

## Base URL

Default:

```text
http://127.0.0.1:8080/v1
```

The port may come from `-p`, `PORT`, or `config.json`. Generated provider artifacts use
the effective port. The bridge binds to loopback by default.

## Authentication

If `auth_token` is configured, every non-`OPTIONS` request must include:

```text
Authorization: Bearer <token>
```

or:

```text
Authorization: Token <token>
```

Missing auth, an empty token, a wrong token, or a token with only a matching prefix is
rejected with HTTP 401. Token comparison is constant-time for equal-length candidates.

## Endpoints

| Method | Path | Contract |
| --- | --- | --- |
| `GET` | `/v1/models` | Returns a JSON model list containing `star-bridge-ds4`. `/models` is accepted as a compatibility alias. |
| `POST` | `/v1/responses` | Runs one turn. Non-streaming returns one response JSON body. Streaming returns Server-Sent Events. `/responses` is accepted as a compatibility alias. |
| `DELETE` | `/v1/responses/{id}` | Best-effort mid-turn cancel. Returns `{"status":"cancelled"}`. A streaming turn should terminate with `response.failed`. |
| `GET` | `/health` | Returns HTTP 200 and `{"status":"ok","native_status":"ok"}` when the native agent answers heartbeat; otherwise HTTP 503. |
| `GET` | `/debug/session` | Trace-only diagnostic endpoint. Returns HTTP 404 when trace is disabled. |
| `POST` | `/v1/responses/compact` | Bridge compact helper. Supported for Codex compatibility, not required for a basic harness. |

Unknown routes return HTTP 404 with a JSON error object.

## Response Request

Supported fields for `POST /v1/responses`:

| Field | Type | Meaning |
| --- | --- | --- |
| `input` | string or OpenAI-style input array | Required turn input. Empty input is rejected. |
| `stream` | boolean | When true, return SSE events. Default false. |
| `model` | string | Optional. The bridge serves the configured alias and normalizes to the native ds4 agent. |
| `previous_response_id` | string | Conversation/session key. Turns with the same value share native state. |
| `reset_session` | boolean | Starts a fresh native state for the turn/session. |
| `reasoning.effort` | string | `low`, `medium`, or `high`; maps to ds4 launch thinking flags and may restart the native process. |
| `max_output_chars` | number | Output character cap. Truncation is reported via `incomplete_details`. |
| `max_turn_events` | number | Streaming event cap. Truncation is reported via `incomplete_details`. |
| `context_tokens` | number | Per-turn context-token override passed to the ds4 wrapper when supported. |

Unsupported fields are ignored unless they conflict with validation.

## Non-Streaming Response

A successful non-streaming turn returns HTTP 200 and a JSON object with:

```json
{
  "id": "resp-...",
  "object": "response",
  "created_at": 0,
  "status": "completed",
  "model": "star-bridge-ds4",
  "output": []
}
```

Failures that occur before or during the turn return a JSON response object with
`"status":"failed"` and an `"error"` object. Validation failures may use HTTP 400.
Native-agent availability failures may use HTTP 503.

## Streaming Response

Streaming uses `Content-Type: text/event-stream` and readable SSE frames:

```text
event: response.created
data: {...}

event: response.output_text.delta
data: {...}

event: response.completed
data: {...}
```

The normal lifecycle is:

1. `response.created`
2. zero or more `response.output_text.delta`
3. `response.completed`

Failure lifecycle after a stream has started:

1. `response.created`
2. optional text/status events
3. `response.failed`

Event `data` objects include monotonic `sequence_number` where applicable. Harnesses
must treat `response.completed` and `response.failed` as terminal.

## Sessions

`previous_response_id` is the public session key. A harness can simulate multiple
conversations by alternating keys:

```json
{"input":"turn A1","previous_response_id":"conversation-A"}
{"input":"turn B1","previous_response_id":"conversation-B"}
{"input":"turn A2","previous_response_id":"conversation-A"}
```

The bridge loads/saves native state around each turn when session persistence is enabled.
If native state cannot be switched safely, the turn fails loudly instead of silently
mixing sessions.

## Concurrency And Cancel

Only one active turn is accepted at a time. While a turn is active:

- `GET /v1/models` remains available.
- `DELETE /v1/responses/{id}` requests cancellation.
- A concurrent `POST /v1/responses` returns HTTP 409.

Cancel is best-effort against the native agent. The HTTP DELETE should return quickly;
the active stream should emit `response.failed`.

## Limits And Truncation

The bridge enforces request-size, frame-size, output, and event limits. When a response is
truncated rather than naturally completed, the response includes `incomplete_details`.
Harnesses should surface that field to users because it means the native answer is
partial.

Examples:

```json
{"status":"completed","incomplete_details":{"reason":"max_output_chars"}}
```

```json
{"status":"completed","incomplete_details":{"reason":"max_turn_events"}}
```

## Error Taxonomy

Harnesses should branch on HTTP status first, then response `status`, then `error`.

| Condition | HTTP | Body shape |
| --- | --- | --- |
| Missing/invalid auth | 401 | `{"error":"missing auth"}` or `{"error":"invalid auth"}` |
| Host not allowlisted | 403 | `{"error":"Host not in allowlist"}` |
| Concurrent turn | 409 | JSON error/failed response |
| Malformed JSON or invalid request | 400 | response object with `status:"failed"` and `error` |
| Unknown route | 404 | `{"error":"not found"}` |
| Native unavailable/protocol failure | 503 | response object or JSON error |
| Stream cancelled or mid-turn native error | 200 SSE | terminal `response.failed` event |

Native/bridge error codes seen in the implementation include:

- `protocol_mismatch`
- `handshake_timeout`
- `frame_decode_error`
- `payload_too_large`
- `native_unhealthy`
- `native_agent_protocol_failure`
- `model_loading`
- `session_incompatible_state`
- `output_truncated`

Harnesses must not assume this list is exhaustive until the fault-injection suite is
complete. Unknown error codes should be shown as fatal turn failures with the original
message preserved.

## Compatibility Requirements For Harness Authors

A minimal compatible harness must:

1. Call `GET /v1/models` before first turn and show connection failure clearly.
2. Send `POST /v1/responses` with `input`.
3. Parse non-streaming response objects with `status`.
4. Parse SSE by event name and stop on `response.completed` or `response.failed`.
5. Preserve and send `previous_response_id` if it wants native session continuity.
6. Surface `incomplete_details` and `error.message`.
7. Treat HTTP 409 as "turn already running", not as a retry storm trigger.
