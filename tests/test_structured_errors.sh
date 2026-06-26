#!/usr/bin/env bash
# Test structured errors: verify that malformed requests and server errors
# produce valid JSON error responses with proper status codes
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.structured_errors.log"

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

echo "=== Test 1: Malformed JSON body ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{invalid json' \
  --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('status')=='failed', 'wrong status'" 2>/dev/null; then
  echo "PASS: malformed JSON returns failed status"
else
  echo "FAIL: expected failed status"
  echo "$RESP"
  exit 1
fi

echo "=== Test 2: Missing input field ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"stream":false}' \
  --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'error' in d, 'missing error'; assert 'message' in d['error'], 'missing message'" 2>/dev/null; then
  echo "PASS: missing input returns error message"
else
  echo "FAIL: expected error message"
  echo "$RESP"
  exit 1
fi

echo "=== Test 3: Non-existent route ==="
RESP="$(curl -s -X GET "http://127.0.0.1:$PORT/v1/nonexistent" --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'error' in d, 'missing error'" 2>/dev/null; then
  echo "PASS: non-existent route returns error"
else
  echo "FAIL: expected error"
  echo "$RESP"
  exit 1
fi

echo "=== Test 4: Request body too large ==="
LARGE_BODY="$(python3 -c 'import json; print(json.dumps({"input": "A"*2000000, "stream": False}))' 2>/dev/null)"
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data-binary <(echo "$LARGE_BODY") \
  --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'error' in d, 'missing error'" 2>/dev/null; then
  echo "PASS: large body returns error"
else
  echo "FAIL: expected error for large body"
  echo "$RESP" | head -c 500
  exit 1
fi

echo "All structured error tests passed."
