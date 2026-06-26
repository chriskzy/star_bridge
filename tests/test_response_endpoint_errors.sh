#!/usr/bin/env bash
# Test response endpoint error handling: verify error responses for various bad requests
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.response_endpoint_errors.log"

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

echo "=== Test 1: GET /v1/responses (no GET support) ==="
RESP="$(curl -s -X GET "http://127.0.0.1:$PORT/v1/responses" --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'error' in d, 'missing error'" 2>/dev/null; then
  echo "PASS: GET /v1/responses returns error"
else
  echo "FAIL: expected error"
  echo "$RESP"
  exit 1
fi

echo "=== Test 2: POST with unsupported content type ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: text/plain' \
  --data 'plain text' \
  --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'error' in d, 'missing error'" 2>/dev/null; then
  echo "PASS: unsupported content type returns error"
else
  echo "FAIL: expected error"
  echo "$RESP"
  exit 1
fi

echo "=== Test 3: POST with empty body ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '' \
  --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'error' in d, 'missing error'" 2>/dev/null; then
  echo "PASS: empty body returns error"
else
  echo "FAIL: expected error"
  echo "$RESP"
  exit 1
fi

echo "=== Test 4: DELETE unknown response id ==="
RESP="$(curl -s -X DELETE "http://127.0.0.1:$PORT/v1/responses/unknown-id" --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'status' in d, 'missing status'" 2>/dev/null; then
  echo "PASS: DELETE unknown id returns status"
else
  echo "FAIL: expected status"
  echo "$RESP"
  exit 1
fi

echo "All response endpoint error tests passed."
