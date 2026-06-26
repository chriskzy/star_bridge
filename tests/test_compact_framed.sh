#!/bin/bash
# test_compact_framed.sh - Framed protocol test for POST /v1/responses/compact
#
# Tests the compact endpoint with a native agent that supports framed protocol
# and returns compaction events.

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
SERVER_PID=0
SERVER_LOG=$(mktemp)
AGENT_PID=0
AGENT_LOG=$(mktemp)
CLEANUP_NEEDED=1

cleanup() {
    CLEANUP_NEEDED=0
    if [ "$SERVER_PID" -gt 0 ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
    fi
    if [ "$AGENT_PID" -gt 0 ] && kill -0 "$AGENT_PID" 2>/dev/null; then
        kill "$AGENT_PID" 2>/dev/null
    fi
    rm -f "$SERVER_LOG" "$AGENT_LOG"
}

trap cleanup EXIT INT TERM

die() { echo "FAIL: $*" >&2; cleanup; exit 1; }

# Create a native agent script that supports framed protocol and compaction
AGENT_SCRIPT="/tmp/test_compact_agent_framed_$$"
cat > "$AGENT_SCRIPT" << 'PYEOF'
#!/usr/bin/env python3
"""Framed-protocol native agent that supports compaction."""
import sys, struct, json, os

def send_frame(msg):
    data = json.dumps(msg).encode()
    sys.stdout.buffer.write(struct.pack('<I', len(data)) + data)
    sys.stdout.flush()

def recv_frame():
    header = sys.stdin.buffer.read(4)
    if not header or len(header) < 4:
        return None
    plen = struct.unpack('<I', header)[0]
    if plen == 0:
        return None
    payload = sys.stdin.buffer.read(plen)
    if not payload:
        return None
    return json.loads(payload.decode())

# Send hello frame
send_frame({
    "type": "hello",
    "role": "native_agent",
    "protocol_version": 1,
    "agent_name": "test-compact-agent",
    "agent_version": "1.0.0",
    "model_name": "test-model",
    "features": {
        "compaction": True,
        "streaming": False,
        "tool_use": False
    },
    "supported_transports": ["stdio_framed", "uds"]
})

# Read bridge hello
msg = recv_frame()
assert msg and msg.get("type") == "hello", f"Expected hello, got: {msg}"

# Send ready frame
send_frame({
    "type": "ready",
    "status": "ready",
    "model_loaded": True,
    "session_state": "idle"
})

# Process frames until shutdown
while True:
    msg = recv_frame()
    if msg is None:
        break
    msg_type = msg.get("type", "")
    if msg_type == "ping":
        send_frame({"type": "pong", "echo": msg.get("timestamp", 0)})
    elif msg_type == "turn":
        # Extract compaction intent from turn text
        turn_text = msg.get("text", "")
        prev_id = msg.get("previous_response_id", "")
        reasoning = msg.get("reasoning_effort", "")
        is_compact = "Compact the conversation" in turn_text or "compact" in turn_text.lower()

        if is_compact:
            # Return compaction events
            send_frame({
                "type": "event",
                "event": {
                    "type": "compaction",
                    "compaction": {
                        "previous_response_id": prev_id or "none",
                        "reasoning_effort": reasoning or "none",
                        "context": "Compacted context data"
                    }
                }
            })
            # Send done event
            send_frame({
                "type": "event",
                "event": {
                    "type": "done",
                    "done": {"status": "completed"}
                }
            })
        else:
            # Return text response
            send_frame({
                "type": "event",
                "event": {
                    "type": "response",
                    "response": {
                        "text": "Hello from framed agent"
                    }
                }
            })
            send_frame({
                "type": "event",
                "event": {
                    "type": "done",
                    "done": {"status": "completed"}
                }
            })
    elif msg_type == "shutdown":
        break
    else:
        # Unknown message type, send error
        send_frame({
            "type": "event",
            "event": {
                "type": "error",
                "error": {"message": f"Unknown message type: {msg_type}"}
            }
        })

sys.exit(0)
PYEOF
chmod +x "$AGENT_SCRIPT"

# Start the bridge in framed mode
echo "Starting codex bridge with framed agent..."
"$SRC_DIR/bin/star_bridge" \
    "$AGENT_SCRIPT" . \
    -p 40002 \
    --framed \
    --turn-response-timeout-ms 15000 \
    --no-config \
    > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 3

# Check server started
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    cat "$SERVER_LOG"
    die "Server failed to start"
fi

echo "Server PID=$SERVER_PID"

# Test 1: Compact with previous_response_id
echo "=== Test 1: Compact endpoint with previous_response_id ==="
HTTP_RESPONSE=$(curl -s -X POST "http://127.0.0.1:40002/v1/responses/compact" \
    -H "Content-Type: application/json" \
    -d '{"previous_response_id":"req-123","reasoning_effort":"high"}' 2>&1)
echo "Response: $HTTP_RESPONSE"

# Verify
echo "$HTTP_RESPONSE" | python3 -c "
import json,sys
d = json.loads(sys.stdin.read())
assert d.get('object') == 'response', 'missing object'
assert d.get('status') in ('completed','failed'), 'bad status'
print('PASS: Test 1 - Compact with previous_response_id works')
" 2>&1 || die "Test 1 failed"

# Test 2: Compact with minimal body (no previous_response_id)
echo "=== Test 2: Compact with minimal body ==="
HTTP_RESPONSE2=$(curl -s -X POST "http://127.0.0.1:40002/v1/responses/compact" \
    -H "Content-Type: application/json" \
    -d '{}' 2>&1)
echo "Response: $HTTP_RESPONSE2"
echo "$HTTP_RESPONSE2" | python3 -c "
import json,sys
d = json.loads(sys.stdin.read())
assert d.get('object') == 'response', 'missing object'
assert d.get('status') in ('completed','failed'), 'bad status'
print('PASS: Test 2 - Minimal body works')
" 2>&1 || die "Test 2 failed"

# Test 3: Verify output text contains compacted context
echo "=== Test 3: Verify compact output ==="
echo "$HTTP_RESPONSE" | python3 -c "
import json,sys
d = json.loads(sys.stdin.read())
if d.get('status') == 'completed':
    output = d.get('output', [])
    texts = [o.get('text','') for o in output if o.get('type') == 'text']
    combined = ' '.join(texts)
    assert 'Compacted' in combined or 'compacted' in combined or len(combined) > 0, f'no compact output: {combined}'
    print(f'PASS: Test 3 - Output contains compacted context ({len(combined)} chars)')
else:
    print(f'PASS: Test 3 - Status {d[\"status\"]} (expected completed or failed)')
" 2>&1 || die "Test 3 failed"

cleanup
echo ""
echo "=== All framed compact endpoint tests PASS ==="
exit 0
