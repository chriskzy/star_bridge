#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.chunked-test.log"
SERVER_LOG="$ROOT/tests/.out/.chunked-server.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

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

# Test 1: send a chunked POST request using Python (raw TCP, no Content-Length)
# The body is '{"input":"hello\n"}'
BODY="{\"input\":\"hello\\n\"}"
HEX_LEN=$(printf '%x' $(echo -n "$BODY" | wc -c))

RESP=$(python3 -c "
import socket
body = b'{\"input\":\"hello\\\\n\"}'
hex_len = b'%x' % len(body)
req = b'POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n' + hex_len + b'\r\n' + body + b'\r\n0\r\n\r\n'
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $PORT))
s.sendall(req)
resp = b''
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    resp += chunk
s.close()
print(resp.decode('utf-8', errors='replace'))
" 2>/dev/null || true)

# Check that we got a 200 OK response
if ! echo "$RESP" | grep -q '200 OK'; then
  echo "FAIL: chunked request did not get 200 OK"
  echo "RESP=$RESP"
  cat "$SERVER_LOG"
  exit 1
fi

# Check response body is valid JSON with object type response
BODY_LINE=$(echo "$RESP" | grep -E '"object"|"type"' | head -1)
if ! echo "$BODY_LINE" | grep -q '"object":"response"'; then
  echo "FAIL: chunked response not valid responses API shape"
  echo "RESP=$RESP"
  cat "$SERVER_LOG"
  exit 1
fi

# Test 2: malformed chunked request (invalid hex) should get 400
MALFORMED_RESP=$(python3 -c "
import socket
req = b'POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n{\"input\":\"hello\"}\r\n0\r\n\r\n'
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(('127.0.0.1', $PORT))
s.sendall(req)
resp = b''
while True:
    chunk = s.recv(4096)
    if not chunk:
        break
    resp += chunk
s.close()
print(resp.decode('utf-8', errors='replace'))
" 2>/dev/null || true)

if ! echo "$MALFORMED_RESP" | grep -q '400 Bad Request'; then
  echo "FAIL: malformed chunked request should get 400"
  echo "MALFORMED_RESP=$MALFORMED_RESP"
  cat "$SERVER_LOG"
  exit 1
fi

echo "Phase 4 chunked request tests passed."
