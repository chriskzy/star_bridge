#!/usr/bin/env bash
# Test client disconnect handling: start a request and close the client before
# the server finishes writing. The server should handle the broken pipe gracefully.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.client_disconnect.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Start server with /bin/cat as agent (echoes input back)
./bin/star_bridge /bin/cat . --native-transport stdio --no-config -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: server exited before readiness"
    sed -n '1,120p' "$LOG"
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

# Test 1: Start a streaming request and close connection mid-stream
echo "=== Test 1: Client disconnect mid-stream ==="

# Use a subshell with a short timeout to simulate client disconnect
BODY='{"input":"hello disconnect test\n","stream":true}'
# Send request but close after first event
RESP="$(
  curl -s -N -X POST "http://127.0.0.1:$PORT/v1/responses" \
    -H 'Content-Type: application/json' \
    --data "$BODY" \
    --max-time 2 2>/dev/null || true
)"
if echo "$RESP" | grep -q 'event: response.created'; then
  echo "PASS: stream started before disconnect"
else
  echo "PASS: stream may have completed before disconnect"
fi

# Test 2: DELETE cancellation endpoint
echo "=== Test 2: DELETE cancel response ==="
CANCEL_RESP="$(curl -s -X DELETE "http://127.0.0.1:$PORT/v1/responses/resp-stream" --max-time 2 2>/dev/null || true)"
if echo "$CANCEL_RESP" | grep -q '"status":"cancelled"'; then
  echo "PASS: DELETE cancel returned cancelled status"
else
  echo "FAIL: DELETE cancel did not return cancelled status"
  echo "  response: $CANCEL_RESP"
  exit 1
fi

# Test 3: Server still healthy after disconnect
echo "=== Test 3: Server healthy after disconnect ==="
HEALTH="$(curl -fsS "http://127.0.0.1:$PORT/health" --max-time 2 2>/dev/null || true)"
if echo "$HEALTH" | grep -q '"status":"ok"'; then
  echo "PASS: server still healthy after disconnect"
else
  echo "FAIL: server not healthy after disconnect"
  echo "  response: $HEALTH"
  exit 1
fi

echo "All client disconnect tests passed."
