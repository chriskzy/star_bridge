#!/usr/bin/env bash
# Test fake-agent handling of session state operations:
#   create_state, save_state, load_state, switch_state, reset
#   missing state (load_state with non-existent key)
#   incompatible state (switch_state with wrong params)
#
# Connects directly to fake_uds_agent over Unix-domain socket.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOCKET="$(pwd)/.test_session_state_$$.sock"
AGENT_LOG="$ROOT/tests/.out/.fake-session-agent.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the fake UDS agent
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/fake_uds_agent" "$ROOT/tests/fake_uds_agent.c" 2>/dev/null || true
chmod +x "$ROOT/tests/fake_uds_agent"

# Start the fake UDS agent
"$ROOT/tests/fake_uds_agent" "$SOCKET" >"$AGENT_LOG" 2>&1 &
AGENT_PID=$!
trap 'kill "$AGENT_PID" >/dev/null 2>&1 || true; rm -f "$SOCKET"' EXIT
trap 'kill "$AGENT_PID" >/dev/null 2>&1 || true; rm -f "$SOCKET"' INT TERM

# Wait for socket
for _ in {1..20}; do
  if [ -S "$SOCKET" ]; then break; fi
  sleep 0.2
done

if ! [ -S "$SOCKET" ]; then
  echo "FAIL: fake UDS agent did not create socket"
  exit 1
fi

echo "Fake UDS agent listening on $SOCKET"

# Helper: connect, send a JSON frame, read response (short timeout)
uds_send() {
  local json_payload="$1"
  python3 -c '
import json, struct, socket, sys
payload = json.loads(sys.argv[2])
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect("'"$SOCKET"'")
body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
s.sendall(struct.pack("<I", len(body)) + body)
# Read exactly 4-byte length header
hlen = b""
while len(hlen) < 4:
    try:
        chunk = s.recv(4 - len(hlen))
        if not chunk:
            break
        hlen += chunk
    except socket.timeout:
        break
if len(hlen) < 4:
    print("ERROR: no response data")
    s.close()
    sys.exit(1)
rlen = struct.unpack("<I", hlen)[0]
# Read exactly rlen bytes
resp = b""
while len(resp) < rlen:
    try:
        chunk = s.recv(rlen - len(resp))
        if not chunk:
            break
        resp += chunk
    except socket.timeout:
        break
print(resp.decode("utf-8"))
s.close()
' -- "$json_payload"
}

echo ""
echo "=== Test 1: Create state ==="
RESP=$(uds_send '{"type":"create_state","key":"session-key-1"}')
echo "Response: $RESP"
echo "$RESP" | grep -q '"type":"ack"' || { echo "FAIL: expected ack"; exit 1; }
echo "$RESP" | grep -q '"state_id"' || { echo "FAIL: expected state_id"; exit 1; }
echo "PASS"

echo ""
echo "=== Test 2: Save state ==="
RESP=$(uds_send '{"type":"save_state","key":"session-key-1","state_id":"state_abc"}')
echo "Response: $RESP"
echo "$RESP" | grep -q '"type":"ack"' || { echo "FAIL: expected ack"; exit 1; }
echo "PASS"

echo ""
echo "=== Test 3: Load state ==="
RESP=$(uds_send '{"type":"load_state","key":"session-key-1"}')
echo "Response: $RESP"
echo "$RESP" | grep -q '"type":"ack"' || { echo "FAIL: expected ack"; exit 1; }
echo "PASS"

echo ""
echo "=== Test 4: Switch state ==="
RESP=$(uds_send '{"type":"switch_state","key":"session-key-2","state_id":"state_def"}')
echo "Response: $RESP"
echo "$RESP" | grep -q '"type":"ack"' || { echo "FAIL: expected ack"; exit 1; }
echo "PASS"

echo ""
echo "=== Test 5: Reset (create_state with same key generates new state_id) ==="
RESP1=$(uds_send '{"type":"create_state","key":"reset-key"}')
echo "First create: $RESP1"
STATE1=$(echo "$RESP1" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d.get('state_id',''))")
RESP2=$(uds_send '{"type":"create_state","key":"reset-key"}')
echo "Second create: $RESP2"
STATE2=$(echo "$RESP2" | python3 -c "import sys,json; d=json.loads(sys.stdin.read()); print(d.get('state_id',''))")
if [ "$STATE1" != "$STATE2" ]; then
    echo "FAIL: reset should produce same state_id for same key (deterministic hash)"
    echo "state1=$STATE1 state2=$STATE2"
    exit 1
fi
echo "PASS"

echo ""
echo "=== Test 6: Missing state (load_state with non-existent key) ==="
RESP=$(uds_send '{"type":"load_state","key":"nonexistent-key"}')
echo "Response: $RESP"
echo "$RESP" | grep -q '"type":"ack"' || { echo "FAIL: expected ack even for missing state"; exit 1; }
echo "PASS"

echo ""
echo "=== Test 7: Incompatible state (switch_state with missing fields) ==="
# Send switch_state without key/state_id - agent should still accept
RESP=$(uds_send '{"type":"switch_state"}')
echo "Response: $RESP"
echo "$RESP" | grep -q '"type":"ack"' || { echo "FAIL: expected ack"; exit 1; }
echo "PASS"

echo ""
echo "=== Test 8: Multiple sequential connections ==="
echo "Connection 1:"
uds_send '{"type":"create_state","key":"multi-key"}' | grep -q '"state_id"' || { echo "FAIL: connection 1"; exit 1; }
echo "Connection 2:"
uds_send '{"type":"save_state","key":"multi-key","state_id":"state_multi"}' | grep -q '"type":"ack"' || { echo "FAIL: connection 2"; exit 1; }
echo "Connection 3:"
uds_send '{"type":"load_state","key":"multi-key"}' | grep -q '"type":"ack"' || { echo "FAIL: connection 3"; exit 1; }
echo "Connection 4:"
uds_send '{"type":"switch_state","key":"multi-key-2","state_id":"state_multi_2"}' | grep -q '"type":"ack"' || { echo "FAIL: connection 4"; exit 1; }
echo "PASS"

echo ""
echo "All session state tests passed."
cat "$AGENT_LOG" 2>/dev/null

