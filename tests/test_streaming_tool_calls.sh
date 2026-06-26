#!/usr/bin/env bash
# Test streaming tool calls: verify tool-related events during streaming
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.streaming_tool_calls.log"

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

echo "=== Test: Streaming tool calls ==="

BODY='{
  "input":"test streaming tool",
  "stream":true,
  "tools":[{
    "type":"function",
    "function":{
      "name":"echo",
      "description":"echo",
      "parameters":{"type":"object","properties":{"text":{"type":"string"}}}
    }
  }]
}'

RESP="$(curl -s -N -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY" \
  --max-time 5 2>/dev/null || true)"

# Extract event names in order
EVENTS="$(echo "$RESP" | grep 'event: ' | sed 's/event: //')"

echo "Events in order:"
echo "$EVENTS"

# Validate sequence starts with response.created
FIRST=$(echo "$EVENTS" | head -1)
if [ "$FIRST" = "response.created" ]; then
  echo "PASS: first event is response.created"
else
  echo "FAIL: first event should be response.created, got: $FIRST"
  exit 1
fi

# Validate sequence ends with response.completed
LAST=$(echo "$EVENTS" | tail -1)
if [ "$LAST" = "response.completed" ]; then
  echo "PASS: last event is response.completed"
else
  echo "FAIL: last event should be response.completed, got: $LAST"
  exit 1
fi

# Validate each data field is valid JSON
ERRORS=0
while IFS= read -r line; do
  if [[ "$line" == data:* ]]; then
    DATA="${line#data: }"
    if ! echo "$DATA" | python3 -c "import sys, json; json.load(sys.stdin)" 2>/dev/null; then
      echo "FAIL: invalid JSON in data field"
      ERRORS=$((ERRORS + 1))
    fi
  fi
done <<< "$RESP"

if [ "$ERRORS" -eq 0 ]; then
  echo "PASS: all data fields are valid JSON"
else
  echo "FAIL: $ERRORS data fields have invalid JSON"
  exit 1
fi

echo "All streaming tool call tests passed."
