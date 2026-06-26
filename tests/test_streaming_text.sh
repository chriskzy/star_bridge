#!/usr/bin/env bash
# Test streamed text response: request with stream=true and verify text deltas
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.streaming_text.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Use /bin/cat as agent
./bin/star_bridge /bin/cat . --native-transport stdio --no-config -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: server exited"
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

echo "=== Test: Streamed text response ==="

BODY='{"input":"Hello streaming world","stream":true}'
RESP="$(curl -s -N -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY" \
  --max-time 5 2>/dev/null || true)"

# Check for key SSE events
if echo "$RESP" | grep -q 'event: response.created'; then
  echo "PASS: response.created event found"
else
  echo "FAIL: missing response.created event"
  echo "$RESP"
  exit 1
fi

if echo "$RESP" | grep -q 'event: response.output_text.delta'; then
  echo "PASS: response.output_text.delta event found"
else
  echo "FAIL: missing response.output_text.delta event"
  echo "$RESP"
  exit 1
fi

if echo "$RESP" | grep -q 'event: response.completed'; then
  echo "PASS: response.completed event found"
else
  echo "FAIL: missing response.completed event"
  echo "$RESP"
  exit 1
fi

echo "All streaming text tests passed."
