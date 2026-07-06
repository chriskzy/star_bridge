#!/usr/bin/env python3
"""Fake native agent for deterministic tests of the framed protocol.

This script speaks the bridge framed protocol:
- Receives hello from bridge, responds with hello + ready
- Receives request frames, responds with ack + response
- Receives session state frames, responds with ack

Usage:
  ./tests/fake_native_agent.py

Reads framed JSON from stdin, writes framed JSON to stdout.
"""

import sys
import os
import json
import struct
import time

def read_frame():
    """Read a length-prefixed JSON frame from stdin (little-endian)."""
    header = sys.stdin.buffer.read(4)
    if not header or len(header) < 4:
        return None
    nlen = struct.unpack("<I", header)[0]
    payload = sys.stdin.buffer.read(nlen)
    if not payload or len(payload) < nlen:
        return None
    return json.loads(payload.decode("utf-8"))

def write_frame(obj):
    """Write a JSON frame with little-endian length prefix to stdout."""
    data = json.dumps(obj, separators=(",", ":"))
    buf = data.encode("utf-8")
    sys.stdout.buffer.write(struct.pack("<I", len(buf)))
    sys.stdout.buffer.write(buf)
    sys.stdout.flush()

def write_raw_frame(payload):
    """Write raw frame bytes for protocol-fault tests."""
    sys.stdout.buffer.write(struct.pack("<I", len(payload)))
    sys.stdout.buffer.write(payload)
    sys.stdout.flush()

def write_oversized_header():
    """Advertise a frame larger than the bridge max without sending payload."""
    sys.stdout.buffer.write(struct.pack("<I", 16777217))
    sys.stdout.flush()

def main():
    # Wait for hello from bridge
    frame = read_frame()
    if not frame or frame.get("type") != "hello":
        write_frame({"type": "error", "message": "expected hello frame"})
        return

    # Respond with hello
    write_frame({
        "type": "hello",
        "role": "native_agent",
        "protocol_version": 1,
        "agent_name": "fake-agent",
        "agent_version": "1.0.0-test",
        "supported_transports": ["stdio_framed"],
        "supported_events": ["response", "error", "status", "log"],
        "max_frame_bytes": 16777216,
    })

    # Respond with ready
    write_frame({
        "type": "ready",
        "status": "ready",
        "model_loaded": True,
        "model_name": "fake-model",
        "model_size": 12345,
    })

    # Main loop: read frames and respond
    while True:
        frame = read_frame()
        if frame is None:
            break

        ft = frame.get("type", "")
        req_id = frame.get("id", "")

        if ft == "shutdown":
            write_frame({"type": "response", "status": "ok", "message": "shutting down"})
            break

        elif ft == "request":
            input_text = frame.get("input", "")
            # Send ack
            ack_id = req_id if req_id else "req-1"
            write_frame({"type": "ack", "id": ack_id, "status": "accepted"})
            fault = os.environ.get("FAKE_NATIVE_FAULT", "")
            if os.environ.get("FAKE_NATIVE_ERROR_AFTER_ACK") == "1":
                fault = "error_after_ack"
            if fault == "error_after_ack":
                # AUD3: ack, then a native error. The bridge must fail the turn
                # immediately with a structured error, not wait for a timeout.
                write_frame({
                    "type": "error",
                    "id": ack_id,
                    "code": "native_blew_up",
                    "message": "simulated native failure after ack",
                })
                continue
            if fault == "killed_mid_turn":
                os._exit(42)
            if fault == "garbage_frame_after_ack":
                write_raw_frame(b"{not-json")
                continue
            if fault == "stall_after_ack":
                time.sleep(10)
                continue
            if fault == "close_after_ack":
                return
            if fault == "oversized_frame_after_ack":
                write_oversized_header()
                continue
            if fault == "slow_text_delta":
                for i in range(100):
                    write_frame({
                        "type": "text_delta",
                        "id": ack_id,
                        "text": f"slow-{i} ",
                    })
                    time.sleep(0.05)
                write_frame({
                    "type": "response",
                    "id": ack_id,
                    "status": "completed",
                    "output": "slow stream completed",
                })
                continue
            # Send response
            write_frame({
                "type": "response",
                "id": ack_id,
                "status": "completed",
                "output": f"Echo: {input_text}",
            })

        elif ft in ("create_state", "load_state", "save_state", "switch_state"):
            # Session state operations
            state_id = frame.get("key", "state-1")
            write_frame({"type": "ack", "status": "accepted", "state_id": state_id})

        elif ft == "ping":
            ping_id = frame.get("id", "")
            write_frame({"type": "pong", "id": ping_id, "status": "ok"})

        elif ft == "health":
            health_id = frame.get("id", "")
            write_frame({"type": "health", "id": health_id, "status": "ok"})

        elif ft == "error":
            # Ignore bridge error frames
            pass

        else:
            write_frame({"type": "error", "message": f"unknown frame type: {ft}"})

if __name__ == "__main__":
    main()
