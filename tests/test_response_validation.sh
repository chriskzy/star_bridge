#!/usr/bin/env bash
# Test response validation: verify that responses from the server conform
# to the expected schema (valid JSON, proper fields, correct types)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.response_validation.log"

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

echo "=== Test: Response validation ==="

# Send a valid request and validate the response structure
BODY='{"input":"validate me","stream":false}'
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY" \
  --max-time 5 2>/dev/null || true)"

# Parse response and validate fields
PYTHON_CHECK=$(python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
    errors = []
    if d.get('object') != 'response':
        errors.append('object should be response')
    if 'id' not in d:
        errors.append('missing id field')
    if not d.get('id', '').startswith('resp'):
        errors.append('id should start with resp')
    if 'status' not in d:
        errors.append('missing status')
    if 'output' not in d:
        errors.append('missing output')
    if not isinstance(d.get('output'), list):
        errors.append('output should be a list')
    if 'model' not in d:
        errors.append('missing model')
    if errors:
        print('FAIL: ' + ', '.join(errors))
    else:
        print('PASS')
except Exception as e:
    print('FAIL: ' + str(e))
" <<< "$RESP" 2>/dev/null || echo "FAIL: python check failed")

if echo "$PYTHON_CHECK" | grep -q 'PASS'; then
  echo "PASS: response structure validated"
else
  echo "FAIL: response structure invalid"
  echo "$PYTHON_CHECK"
  echo "$RESP" | head -c 500
  exit 1
fi

# Verify models endpoint also returns valid JSON
echo "=== Test: Models endpoint response ==="
MODELS="$(curl -fsS "http://127.0.0.1:$PORT/v1/models" --max-time 2 2>/dev/null || true)"
PYTHON_MODELS=$(python3 -c "
import sys, json
d = json.load(sys.stdin)
if 'data' in d and isinstance(d['data'], list) and len(d['data']) > 0:
    print('PASS')
else:
    print('FAIL: unexpected models structure')
" <<< "$MODELS" 2>/dev/null || echo "FAIL: models check failed")

if echo "$PYTHON_MODELS" | grep -q 'PASS'; then
  echo "PASS: models endpoint returns valid response"
else
  echo "FAIL: models endpoint invalid"
  echo "$PYTHON_MODELS"
  exit 1
fi

echo "All response validation tests passed."
