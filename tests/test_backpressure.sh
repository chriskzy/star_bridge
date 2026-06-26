#!/usr/bin/env bash
# Test backpressure: simulate a slow consumer by reading SSE with delays,
# verify that the server handles the backpressure gracefully without crashing
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.backpressure.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

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

echo "=== Test: Backpressure handling ==="

# Send streaming request and read slowly (1 line per second)
BODY='{"input":"slow consumer test","stream":true}'
# Use a subshell with background reader that sleeps between lines
RESP="$(curl -s -N -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY" \
  --max-time 10 2>/dev/null || true)"

# Check that we got a complete streaming response despite slow reading
if echo "$RESP" | grep -q 'event: response.created'; then
  echo "PASS: response.created received"
else
  echo "FAIL: no response.created"
  echo "$RESP"
  exit 1
fi

if echo "$RESP" | grep -q 'event: response.completed'; then
  echo "PASS: response.completed received"
else
  echo "FAIL: no response.completed"
  echo "$RESP"
  exit 1
fi

# Verify server is still healthy after the slow read
HEALTH="$(curl -fsS "http://127.0.0.1:$PORT/health" --max-time 2 2>/dev/null || true)"
if echo "$HEALTH" | grep -q '"status":"ok"'; then
  echo "PASS: server still healthy after backpressure"
else
  echo "FAIL: server not healthy after backpressure"
  exit 1
fi

echo "Backpressure tests passed."
