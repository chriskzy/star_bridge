#!/usr/bin/env bash
# Test the bridge using the fake native-agent binary
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-19051}"
LOG="$ROOT/tests/.out/.smoke-server.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Start bridge with fake agent
FAKE_AGENT="$ROOT/tests/fake_agent.sh"
chmod +x "$FAKE_AGENT"

./bin/star_bridge "$FAKE_AGENT" . --framed --no-config -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

echo "=== Test 1: Models endpoint ==="
MODELS="$(curl -fsS "http://127.0.0.1:$PORT/v1/models")"
echo "$MODELS" | grep -q 'star-bridge-ds4' || { echo "FAIL: models"; exit 1; }
echo "PASS"

echo "=== Test 2: Basic prompt ==="
RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello from test\n"}')"
echo "$RESP" | grep -q '"object":"response"' || { echo "FAIL: object"; exit 1; }
echo "$RESP" | grep -q '"type":"text"' || { echo "FAIL: text output"; exit 1; }
echo "$RESP" | grep -q 'Fake agent received' || { echo "FAIL: fake agent text"; exit 1; }
if echo "$RESP" | grep -qE '<style>|bridge-container|tree-row|event-node'; then
  echo "FAIL: bridge UI leaked into fake-agent response"
  exit 1
fi
echo "PASS"

echo "=== Test 3: Streaming ==="
STREAM="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"stream test\n","stream":true}')"
echo "$STREAM" | grep -q 'event: response.created' || { echo "FAIL: stream created"; exit 1; }
echo "$STREAM" | grep -q 'event: response.completed' || { echo "FAIL: stream completed"; exit 1; }
echo "$STREAM" | grep -q 'Fake agent received' || { echo "FAIL: fake agent stream text"; exit 1; }
if echo "$STREAM" | grep -qE '<style>|bridge-container|tree-row|event-node'; then
  echo "FAIL: bridge UI leaked into fake-agent stream"
  exit 1
fi
echo "PASS"

echo "=== Test 4: Tools defined but not auto-executed ==="
TOOL_RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"list files","tools":[{"type":"function","function":{"name":"google_search"}}]}')"
echo "$TOOL_RESP" | grep -q '"object":"response"' || { echo "FAIL: object"; exit 1; }
if echo "$TOOL_RESP" | grep -q '"type":"tool_call"'; then
  echo "FAIL: tools should not auto-execute"
  exit 1
fi
echo "PASS"

echo "=== Test 5: Structured error ==="
ERROR_RESP="$(curl -sS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data 'invalid-json')"
echo "$ERROR_RESP" | grep -q '"object":"response"' || { echo "FAIL: error object"; exit 1; }
echo "$ERROR_RESP" | grep -q '"status":"failed"' || { echo "FAIL: status failed"; exit 1; }
echo "PASS"

echo ""
echo "All fake-agent tests passed."
