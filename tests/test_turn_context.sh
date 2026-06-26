#!/usr/bin/env bash
# Test TurnContext lifecycle: turn_begin, turn_await_ack, turn_process_events, turn_cleanup
# Uses fake framed native agent to simulate a complete turn cycle.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19150 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.turn_context_test.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

cleanup() {
    kill "$BRIDGE_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== TurnContext Lifecycle Test ==="

# Start bridge with framed fake native agent
./bin/star_bridge ./tests/fake_native_agent.py . \
  --framed \
  --no-config \
  -p "$PORT" \
  >"$LOG" 2>&1 &
BRIDGE_PID=$!

# Wait for bridge ready
for _ in {1..30}; do
  if kill -0 "$BRIDGE_PID" 2>/dev/null && curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    echo "Bridge ready"
    break
  fi
  sleep 0.3
done

echo "=== Test 1: Turn begin (POST request) ==="
BODY='{"input":"Hello turn test","stream":false}'
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY" --max-time 10 2>/dev/null || true)"
if echo "$RESP" | grep -q '"text"'; then
  echo "PASS: Turn begin + ack + process events returned valid response"
else
  echo "FAIL: Turn did not produce text response"
  echo "  response: $RESP"
  exit 1
fi

echo "=== Test 2: Turn cleanup (verify server healthy after turn) ==="
HEALTH="$(curl -fsS "http://127.0.0.1:$PORT/health" --max-time 5 2>/dev/null || true)"
if echo "$HEALTH" | grep -q '"status":"ok"'; then
  echo "PASS: Server healthy after turn cleanup"
else
  echo "FAIL: Server not healthy after turn"
  echo "  health: $HEALTH"
  exit 1
fi

echo "=== Test 3: Second turn (follow-up request after cancellation recovery) ==="
BODY2='{"input":"Second turn test","stream":false}'
RESP2="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY2" --max-time 10 2>/dev/null || true)"
if echo "$RESP2" | grep -q '"text"'; then
  echo "PASS: Second turn succeeded after previous turn"
else
  echo "FAIL: Second turn failed"
  echo "  response: $RESP2"
  exit 1
fi

echo "=== Test 4: Streaming turn (verify SSE events) ==="
BODY3='{"input":"Stream test","stream":true}'
SSE="$(curl -s -N -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY3" --max-time 10 2>/dev/null || true)"
if echo "$SSE" | grep -q 'event: response.created'; then
  echo "PASS: Streaming turn produced SSE events"
else
  echo "FAIL: Streaming turn did not produce SSE"
  echo "  SSE: $SSE"
  exit 1
fi

echo "All TurnContext lifecycle tests passed."
