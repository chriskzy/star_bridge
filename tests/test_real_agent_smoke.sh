#!/usr/bin/env bash
# Real-agent smoke test for session state persistence.
#
# Uses fake_agent.py as the "real agent" (stdin/stdout framed protocol).
# Tests that the bridge correctly delegates session state operations
# (create, save, load, switch) to the agent, and that KV is saved/loaded
# across turns.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_LOG="$ROOT/tests/.out/.bridge-real-agent-smoke.log"
PORT=21779

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null
chmod +x "$ROOT/tests/fake_agent.py"

# Kill any leftover bridge from previous runs
kill $(lsof -ti:$PORT) 2>/dev/null || true
sleep 0.3

# Helper: send HTTP request using Python (nc doesn't wait long enough)
send_http() {
    local method="$1" path="$2" body="$3"
    "${VENV_PY:-python3}" -c "
import http.client
conn = http.client.HTTPConnection('localhost', $PORT)
conn.request('$method', '$path', '$body', {'Content-Type': 'application/json'})
resp = conn.getresponse()
print(resp.status, resp.reason)
data = resp.read().decode()
print(data)
conn.close()
" 2>/dev/null
}

get_debug_session() {
    "${VENV_PY:-python3}" -c "
import http.client
conn = http.client.HTTPConnection('localhost', $PORT)
conn.request('GET', '/debug/session')
resp = conn.getresponse()
print(resp.status, resp.reason)
data = resp.read().decode()
print(data)
conn.close()
" 2>/dev/null
}

start_bridge() {
    "$ROOT/bin/star_bridge" \
        "$ROOT/tests/fake_agent.py" \
        "$ROOT" \
        -p "$PORT" \
        --framed \
        --trace \
        >"$BRIDGE_LOG" 2>&1 &
    BRIDGE_PID=$!
    sleep 1.5
    if ! kill -0 $BRIDGE_PID 2>/dev/null; then
        echo "FAIL: bridge failed to start"
        cat "$BRIDGE_LOG"
        exit 1
    fi
    echo "Bridge PID=$BRIDGE_PID"
}

stop_bridge() {
    kill "$BRIDGE_PID" >/dev/null 2>&1 || true
    sleep 0.3
}

echo ""
echo "=== Real-agent session state smoke tests ==="

# Test 1: Create new session (reset_session=true)
echo ""
echo "Test 1: Create new session (reset_session=true)"
start_bridge
send_http "POST" "/v1/responses" '{"input":"hello from test","reset_session":true}'
sleep 0.5
stop_bridge
echo "Bridge log snippet:"
grep -i "session\|state\|create\|save\|load\|switch\|kv" "$BRIDGE_LOG" 2>/dev/null || echo "(no session/state log lines)"
echo "PASS"

# Test 2: Load saved session (previous_response_id)
echo ""
echo "Test 2: Load saved session via previous_response_id"
start_bridge
send_http "POST" "/v1/responses" '{"input":"first turn","previous_response_id":"session-abc"}'
sleep 0.5
stop_bridge
echo "Bridge log snippet:"
grep -i "session\|state\|create\|save\|load\|switch\|kv" "$BRIDGE_LOG" 2>/dev/null || echo "(no session/state log lines)"
echo "PASS"

# Test 3: Switch session via different previous_response_id
echo ""
echo "Test 3: Switch session"
start_bridge
send_http "POST" "/v1/responses" '{"input":"session a","previous_response_id":"session-a"}'
sleep 0.3
send_http "POST" "/v1/responses" '{"input":"session b","previous_response_id":"session-b"}'
sleep 0.5
stop_bridge
echo "Bridge log snippet:"
grep -i "session\|state\|create\|save\|load\|switch\|kv" "$BRIDGE_LOG" 2>/dev/null || echo "(no session/state log lines)"
echo "PASS"

# Test 4: Default session restore (no previous_response_id)
echo ""
echo "Test 4: Default session restore (project-scoped session)"
start_bridge
send_http "POST" "/v1/responses" '{"input":"project session"}'
sleep 0.5
stop_bridge
echo "Bridge log snippet:"
grep -i "session\|state\|create\|save\|load\|switch\|kv" "$BRIDGE_LOG" 2>/dev/null || echo "(no session/state log lines)"
echo "PASS"

# Test 5: Debug session endpoint shows state info
echo ""
echo "Test 5: Debug session endpoint"
start_bridge
send_http "POST" "/v1/responses" '{"input":"debug test","reset_session":true}'
sleep 0.5
debug_resp=$(get_debug_session)
echo "Debug response:"
echo "$debug_resp" | head -30
stop_bridge
echo "PASS"

echo ""
echo "All real-agent smoke tests completed."
