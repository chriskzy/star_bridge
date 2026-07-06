#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOCKET="$(pwd)/.fake_uds_agent_test_$$.sock"
AGENT_LOG="$ROOT/tests/.out/.fake-uds-agent.log"
mkdir -p "$ROOT/tests/.out"

cd "$ROOT"

# Ensure the fake UDS agent binary is built
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/fake_uds_agent" "$ROOT/tests/fake_uds_agent.c"
chmod +x "$ROOT/tests/fake_uds_agent"

# Start the fake UDS agent
"$ROOT/tests/fake_uds_agent" "$SOCKET" >"$AGENT_LOG" 2>&1 &
AGENT_PID=$!
trap 'kill "$AGENT_PID" >/dev/null 2>&1 || true; rm -f "$SOCKET"' EXIT
trap 'kill "$AGENT_PID" >/dev/null 2>&1 || true; rm -f "$SOCKET"' INT TERM

for _ in {1..20}; do
  if [ -S "$SOCKET" ]; then
    break
  fi
  sleep 0.2
done

if ! [ -S "$SOCKET" ]; then
  echo "FAIL: fake UDS agent did not create socket"
  exit 1
fi

echo "Fake UDS agent listening on $SOCKET"

# Helper: connect, send a JSON frame, read response (skipping ack frames)
uds_send() {
  local json_payload="$1"
  python3 <<EOF
import json, struct, socket, sys
payload = json.loads("""$json_payload""")
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect("$SOCKET")
body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
s.sendall(struct.pack("<I", len(body)) + body)
data = b""
while True:
    try:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    except socket.timeout:
        break
off = 0
last_resp = None
while off + 4 <= len(data):
    rlen = struct.unpack("<I", data[off:off+4])[0]
    if off + 4 + rlen > len(data):
        break
    frame = data[off+4:off+4+rlen].decode("utf-8")
    off += 4 + rlen
    if '"ack"' in frame:
        continue
    last_resp = frame
if last_resp is None:
    print("ERROR: no non-ack response frame")
    sys.exit(1)
print(last_resp)
s.close()
EOF
}

echo "=== Test 1: Health frame ==="
HEALTH_RESP="$(uds_send '{"type":"health"}')"
echo "$HEALTH_RESP" | grep -q '"status":"ok"' || { echo "FAIL: health response"; exit 1; }
echo "$HEALTH_RESP" | grep -q '"agent":"fake-uds-agent"' || { echo "FAIL: agent field"; exit 1; }
echo "PASS"

echo "=== Test 2: Request frame ==="
REQ_RESP="$(uds_send '{"type":"request","input":"hello from test"}')"
echo "$REQ_RESP" | grep -q '"status":"completed"' || { echo "FAIL: completed status"; exit 1; }
echo "$REQ_RESP" | grep -q 'Fake UDS agent received' || { echo "FAIL: echo text"; exit 1; }
echo "PASS"

echo "=== Test 3: Unknown frame ==="
UNK_RESP="$(uds_send '{"type":"unknown"}')"
echo "$UNK_RESP" | grep -q '"status":"completed"' || { echo "FAIL: unknown frame response"; exit 1; }
echo "PASS"

echo "=== Test 4: Multiple connections ==="
echo "First connection:"
FIRST_RESP="$(uds_send '{"type":"health"}')"
echo "$FIRST_RESP" | grep -q '"status":"ok"' || { echo "FAIL: first connection"; exit 1; }
sleep 0.5
echo "Second connection:"
SEC_RESP="$(uds_send '{"type":"request","input":"connection two"}')"
echo "$SEC_RESP" | grep -q 'connection two' || { echo "FAIL: second connection"; exit 1; }
echo "PASS"

echo ""
echo "All fake UDS agent tests passed."
