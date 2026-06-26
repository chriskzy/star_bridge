#!/usr/bin/env bash
# Test request validation: verify that schema validation catches invalid
# request fields and returns appropriate errors
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== Test: Request validation ==="

PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.request_validation.log"

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

echo "=== Test 1: Empty body ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{}' \
  --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('status')=='failed', 'wrong status'" 2>/dev/null; then
  echo "PASS: empty body returns failed status"
else
  echo "FAIL: expected failed status"
  echo "$RESP"
  exit 1
fi

echo "=== Test 2: input is not a string ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":123,"stream":false}' \
  --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'error' in d, 'missing error'; assert 'message' in d['error'], 'missing message'" 2>/dev/null; then
  echo "PASS: non-string input returns error message"
else
  echo "FAIL: expected error message"
  echo "$RESP"
  exit 1
fi

echo "=== Test 3: stream is not a boolean ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello","stream":"yes"}' \
  --max-time 2 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert 'error' in d, 'missing error'; assert 'message' in d['error'], 'missing message'" 2>/dev/null; then
  echo "PASS: non-boolean stream returns error message"
else
  echo "FAIL: expected error message"
  echo "$RESP"
  exit 1
fi

echo "=== Test 4: valid request passes ==="
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"valid request","stream":false}' \
  --max-time 2 2>/dev/null || true)"
TYPE=$(echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('object',''))" 2>/dev/null || echo "")
if [ "$TYPE" = "response" ]; then
  echo "PASS: valid request returns response object"
else
  echo "FAIL: expected response object, got: $TYPE"
  echo "$RESP" | head -c 500
  exit 1
fi

echo "All request validation tests passed."
