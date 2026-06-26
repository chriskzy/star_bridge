#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
SERVER_LOG="$ROOT/tests/.out/.diag-server.log"
DEBUG_LOG="$ROOT/.codex-bridge-debug.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null
rm -f "$SERVER_LOG" "$DEBUG_LOG"

./bin/star_bridge /bin/cat . --native-transport stdio --no-config -p "$PORT" >"$SERVER_LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: server exited before readiness"
    sed -n '1,120p' "$SERVER_LOG"
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

# Send a valid request and check that diagnostics are logged
curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello\n"}' >/dev/null 2>&1

LOWER_BODY='{"input":"lowercase header"}'
LOWER_LEN=$(printf '%s' "$LOWER_BODY" | wc -c | tr -d ' ')
python3 -c "
import socket
body = b'{\"input\":\"lowercase header\"}'
req = (
    b'POST /v1/responses HTTP/1.1\r\n'
    b'host: 127.0.0.1\r\n'
    b'content-type: application/json\r\n'
    b'content-length: ' + str(len(body)).encode() + b'\r\n'
    b'\r\n' + body
)
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $PORT))
s.sendall(req)
while s.recv(4096):
    pass
s.close()
"

curl -sS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '[]' >/dev/null 2>&1 || true

sleep 0.3

# Check that request diagnostics include method/path
if ! grep -qE 'method=POST|path=/v1/responses' "$SERVER_LOG"; then
  echo "FAIL: diagnostics missing method/path"
  cat "$SERVER_LOG"
  exit 1
fi

# Check that content length is logged
if ! grep -qE 'content_length|cl=|len=' "$SERVER_LOG"; then
  echo "FAIL: diagnostics missing content length"
  cat "$SERVER_LOG"
  exit 1
fi

if ! grep -q "content_length=$LOWER_LEN" "$SERVER_LOG"; then
  echo "FAIL: lowercase content-length header not logged correctly"
  cat "$SERVER_LOG"
  exit 1
fi

# Check that transfer encoding is logged
if ! grep -qE 'transfer.encoding|te=' "$SERVER_LOG"; then
  echo "FAIL: diagnostics missing transfer encoding"
  cat "$SERVER_LOG"
  exit 1
fi

# Check that parse result (success/failure) is logged
if ! grep -qE 'parse.ok|parse.fail|parse_result|request=' "$SERVER_LOG"; then
  echo "FAIL: diagnostics missing parse result"
  cat "$SERVER_LOG"
  exit 1
fi

if ! curl -fsS "http://127.0.0.1:$PORT/debug/session" | grep -q '"trace":true'; then
  echo "FAIL: trace debug session not enabled by default"
  cat "$SERVER_LOG"
  exit 1
fi

if ! grep -q 'parse_fail=1' "$DEBUG_LOG"; then
  echo "FAIL: debug log missing parse failure"
  cat "$SERVER_LOG"
  test -f "$DEBUG_LOG" && cat "$DEBUG_LOG"
  exit 1
fi

if ! grep -q 'body_first_non_ws=0x5b' "$DEBUG_LOG"; then
  echo "FAIL: debug log missing parse-failure body probe"
  cat "$DEBUG_LOG"
  exit 1
fi

echo "All request diagnostics tests passed."
