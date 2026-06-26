#!/usr/bin/env python3
import json
import os
import struct
import sys
import hashlib

agent_argv = " ".join(sys.argv[1:])
agent_env = os.environ.get("DWARFSTAR_MODE", "missing")
request_count = 0
multi_tool_stage = 0


def read_exact(n):
    data = b""
    while len(data) < n:
        chunk = sys.stdin.buffer.read(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def read_frame():
    header = read_exact(4)
    if not header:
        return None
    (length,) = struct.unpack("<I", header)
    if length == 0:
        return {}
    payload = read_exact(length)
    if payload is None:
        return None
    return json.loads(payload.decode("utf-8"))


def write_frame(obj):
    payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    sys.stdout.buffer.write(struct.pack("<I", len(payload)))
    sys.stdout.buffer.write(payload)
    sys.stdout.buffer.flush()


while True:
    frame = read_frame()
    if frame is None:
        break

    frame_type = frame.get("type", "unknown")
    if frame_type == "hello":
        write_frame({
            "type": "hello",
            "role": "native_agent",
            "protocol_version": 1,
            "agent_name": "fake-agent",
            "agent_version": "0.1.0",
            "supported_transports": ["uds"],
            "supported_events": ["response", "error", "status", "log"],
            "max_frame_bytes": 16777216,
            "capabilities": {"model_loading": True}
        })
        write_frame({
            "type": "ready",
            "status": "ready",
            "model_loaded": True,
            "session_state": "idle"
        })
    elif frame_type == "health":
        write_frame({
            "type": "health",
            "id": frame.get("id", ""),
            "status": "ok"
        })
    elif frame_type == "ping":
        write_frame({
            "type": "pong",
            "id": frame.get("id", ""),
            "status": "ok"
        })
    elif frame_type == "request":
        input_text = frame.get("input", "")
        req_id = frame.get("id", "req-1")
        # Send turn-level ack first
        write_frame({
            "type": "ack",
            "id": req_id,
            "status": "accepted"
        })
        if "tool-history-old-first" in input_text:
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_history_old_first",
                    "name": "google_search",
                    "arguments": "{\"query\":\"OLD_FIRST_" + ("x" * 600) + "\"}"
                }
            })
            continue
        if "tool-history-old" in input_text:
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_history_old_late",
                    "name": "google_search",
                    "arguments": "{\"query\":\"OLD_LATE_" + ("x" * 600) + "\"}"
                }
            })
            continue
        if "tool-history-new" in input_text:
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_history_new",
                    "name": "google_search",
                    "arguments": "{\"query\":\"NEW_MARKER_" + ("y" * 120) + "\"}"
                }
            })
            continue
        if "tool-intent-malformed" in input_text:
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_intent_bad",
                    "name": "google_search"
                }
            })
            continue
        if "tool-intent-denied" in input_text:
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_intent_denied",
                    "name": "shell_command",
                    "arguments": {"command": "rm -rf /"}
                }
            })
            continue
        if "tool-intent-multi" in input_text:
            multi_tool_stage = 1
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_intent_1",
                    "name": "google_search",
                    "arguments": {"query": "codex bridge phase4 one"}
                }
            })
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_intent_2",
                    "name": "browse_url",
                    "arguments": {"url": "https://example.test"}
                }
            })
            continue
        if "tool-intent" in frame.get("input", ""):
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_intent_1",
                    "name": "google_search",
                    "arguments": {"query": "codex bridge phase4"}
                }
            })
            continue
        if "tool-timeout" in input_text:
            import time
            time.sleep(1.0)
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "completed",
                "text": "late tool timeout response"
            })
            continue
        if "replay check" in input_text:
            tool_history = frame.get("tool_history", [])
            history_str = json.dumps(tool_history) if tool_history else "[]"
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "completed",
                "text": f"Replay snapshot: tool_history:{history_str}"
            })
            continue
        if input_text and "delay" in input_text:
            import time
            time.sleep(1.0)
        if frame.get("reset_session") is True:
            request_count = 0
        request_count += 1
        if os.environ.get("FAKE_AGENT_ECHO_INPUT_STATS") == "1":
            digest = hashlib.sha256(input_text.encode("utf-8")).hexdigest()
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "completed",
                "text": f"input_len={len(input_text.encode('utf-8'))} input_sha256={digest}"
            })
            continue
        delta_flood = int(os.environ.get("FAKE_AGENT_DELTA_FLOOD", "0"))
        if delta_flood > 0:
            # Emit many text_delta frames (each counts as one turn event) then a
            # completed response. Exercises the event-cap partial-completion path.
            for n in range(delta_flood):
                write_frame({
                    "type": "text_delta",
                    "id": req_id,
                    "text": f"d{n} ",
                })
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "completed",
                "text": "FLOOD_DONE",
            })
            continue
        big_bytes = int(os.environ.get("FAKE_AGENT_BIG_OUTPUT_BYTES", "0"))
        if big_bytes > 0:
            write_frame({
                "type": "response",
                "id": req_id,
                "status": "completed",
                "text": "x" * big_bytes
            })
            continue
        text = input_text[:120]
        write_frame({
            "type": "response",
            "id": req_id,
            "status": "completed",
            "text": (
                f"Fake agent received: {text or 'empty'} "
                f"previous_response_id={frame.get('previous_response_id', 'none')} "
                f"auto_load_resume_session={str(frame.get('auto_load_resume_session', 'missing')).lower()} "
                f"context_tokens={frame.get('context_tokens', 'missing')} "
                f"argv={agent_argv} env=DWARFSTAR_MODE={agent_env}"
                f" request_count={request_count}"
                f" reset_session={str(frame.get('reset_session', False)).lower()}"
            )
            })
    elif frame_type == "tool_result":
        tool_name = frame.get("tool", {}).get("name", "unknown")
        tool_result = frame.get("tool", {}).get("result", "")
        if isinstance(tool_result, dict) and tool_result.get("status") == "error":
            write_frame({
                "type": "response",
                "id": frame.get("id", ""),
                "status": "completed",
                "text": f"Tool error received: {tool_name} {tool_result.get('message', '')}"
            })
            continue
        if multi_tool_stage == 1:
            multi_tool_stage = 2
            write_frame({
                "type": "response",
                "id": frame.get("id", ""),
                "status": "in_progress",
                "tool_intent": {
                    "id": "tool_intent_2",
                    "name": "browse_url",
                    "arguments": {"url": "https://example.test"}
                }
            })
            continue
        write_frame({
            "type": "response",
            "id": frame.get("id", ""),
            "status": "completed",
            "text": (
                f"Tool result received: {tool_name} "
                f"{tool_result}"
            )
        })
    elif frame_type == "shutdown":
        write_frame({"type": "response", "status": "ok", "message": "shutting down"})
        break
    elif frame_type == "error":
        # Ignore error frames silently (don't respond, they're informational)
        pass
    elif frame_type == "create_state":
        # Accept create_state and respond with ack containing state_id
        key = frame.get("key", "")
        state_id = f"state_{hash(key) & 0xffffffff}" if key else "state_empty"
        write_frame({"type": "ack", "status": "accepted", "state_id": state_id})
    elif frame_type == "save_state":
        # Accept save_state with ack
        write_frame({"type": "ack", "status": "accepted"})
    elif frame_type == "load_state":
        # Accept load_state with ack
        write_frame({"type": "ack", "status": "accepted"})
    elif frame_type == "switch_state":
        # Accept switch_state with ack
        write_frame({"type": "ack", "status": "accepted"})
    else:
        write_frame({"type": "error", "message": f"unknown frame type: {frame_type}"})
