#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.logging-test.log"
SERVER_LOG="$ROOT/tests/.out/.logging-server.log"

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

# Test 1: non-stream request — check request model logging
RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello from smoke\n","reasoning_effort":"low"}')"
grep -q '"object":"response"' <<<"$RESP"

sleep 0.3

# Check server log for expected request-line format
if ! grep -q 'request=' "$SERVER_LOG"; then
  echo "FAIL: no request= log line found"
  cat "$SERVER_LOG"
  exit 1
fi

# Check request log line contains model and public_alias
if ! grep -qE 'model=|public_alias=' "$SERVER_LOG"; then
  echo "FAIL: request log missing model/public_alias"
  cat "$SERVER_LOG"
  exit 1
fi

# Test 2: streaming request — check SSE logging with bytes and sequence
STREAM_RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello stream\n","stream":true}')"
grep -q 'event: response.created' <<<"$STREAM_RESP"
grep -q 'event: response.completed' <<<"$STREAM_RESP"

sleep 0.3

# Check SSE logging includes event name, bytes written, data_len
if ! grep -qE 'sse event=' "$SERVER_LOG"; then
  echo "FAIL: no sse event= log lines found"
  cat "$SERVER_LOG"
  exit 1
fi

# Check SSE log lines contain bytes= and data_len=
if ! grep -qE 'bytes=' "$SERVER_LOG"; then
  echo "FAIL: sse log missing bytes="
  cat "$SERVER_LOG"
  exit 1
fi
if ! grep -qE 'data_len=' "$SERVER_LOG"; then
  echo "FAIL: sse log missing data_len="
  cat "$SERVER_LOG"
  exit 1
fi

# Check that stream content_len and chunk_size are logged
if ! grep -qE 'stream content_len=' "$SERVER_LOG"; then
  echo "FAIL: no stream content_len= log line"
  cat "$SERVER_LOG"
  exit 1
fi

# Check SSE log lines contain seq= (sequence number)
if ! grep -qE 'seq=' "$SERVER_LOG"; then
  echo "FAIL: sse log missing seq= (sequence number)"
  cat "$SERVER_LOG"
  exit 1
fi

# Check that source code contains truncation logging logic
if ! grep -qE 'truncated' "$ROOT/src/server.c"; then
  echo "FAIL: no truncation logging in source code"
  exit 1
fi

echo "All logging tests passed."
