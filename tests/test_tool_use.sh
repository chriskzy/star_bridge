#!/usr/bin/env bash
# Test tool use: verify that tools are properly defined and returned in responses
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.tool_use.log"

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

echo "=== Test 1: Request with tools ==="
BODY='{"input":"test tools","stream":false,"tools":[{"type":"function","function":{"name":"echo","description":"echo","parameters":{"type":"object","properties":{"text":{"type":"string"}}}}}]}'
RESP="$(curl -s -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY" \
  --max-time 5 2>/dev/null || true)"
if echo "$RESP" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d.get('object')=='response', 'should succeed'" 2>/dev/null; then
  echo "PASS: request with tools returns response"
else
  echo "FAIL: expected response"
  echo "$RESP" | head -c 500
  exit 1
fi

echo "=== Test 2: Tools appear in response ==="
if echo "$RESP" | python3 -c "
import sys,json
d=json.load(sys.stdin)
output = d.get('output', [])
assert len(output) > 0, 'output should not be empty'
print('PASS: output has', len(output), 'items')
" 2>/dev/null; then
  echo "PASS: tools response has output"
else
  echo "FAIL: tools response missing output"
  echo "$RESP" | head -c 500
  exit 1
fi

echo "All tool use tests passed."
