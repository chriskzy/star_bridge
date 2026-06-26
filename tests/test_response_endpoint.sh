#!/usr/bin/env bash
# Test response endpoint: verify GET /v1/responses/ and related endpoints
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.response_endpoint.log"

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

echo "=== Test 1: GET /v1/models ==="
MODELS="$(curl -fsS "http://127.0.0.1:$PORT/v1/models" --max-time 2 2>/dev/null || true)"
if echo "$MODELS" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'data' in d, 'missing data'; assert isinstance(d['data'], list), 'not list'" 2>/dev/null; then
  echo "PASS: /v1/models returns valid response"
else
  echo "FAIL: /v1/models invalid"
  echo "$MODELS" | head -c 500
  exit 1
fi

echo "=== Test 2: POST /v1/responses (non-streaming) ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello endpoint","stream":false}' \
  --max-time 5 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('object')=='response', 'not response'" 2>/dev/null; then
  echo "PASS: POST /v1/responses returns response"
else
  echo "FAIL: POST /v1/responses invalid"
  echo "$RESP" | head -c 500
  exit 1
fi

echo "=== Test 3: POST /v1/responses (streaming) ==="
STREAM="$(curl -s -N -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello stream","stream":true}' \
  --max-time 5 2>/dev/null || true)"
if echo "$STREAM" | grep -q 'event: response.created'; then
  echo "PASS: streaming returns SSE events"
else
  echo "FAIL: streaming no SSE events"
  echo "$STREAM"
  exit 1
fi

echo "=== Test 4: DELETE /v1/responses/ (cancel) ==="
CANCEL="$(curl -s -X DELETE "http://127.0.0.1:$PORT/v1/responses/resp-stream" --max-time 2 2>/dev/null || true)"
if echo "$CANCEL" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'status' in d, 'missing status'" 2>/dev/null; then
  echo "PASS: DELETE returns cancel status"
else
  echo "FAIL: DELETE invalid"
  echo "$CANCEL"
  exit 1
fi

echo "All response endpoint tests passed."
