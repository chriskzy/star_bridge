#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.smoke-server.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

./bin/star_bridge /bin/cat . --native-transport stdio --no-config -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: smoke server exited before readiness"
    sed -n '1,120p' "$LOG"
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

MODELS="$(curl -fsS "http://127.0.0.1:$PORT/v1/models")"
# Default release model id should be the Star Bridge alias.
grep -q 'star-bridge-ds4' <<<"$MODELS"
# Browser/search tooling is cut from core; the bridge must advertise no such tools.
grep -q '"tools":\[\]' <<<"$MODELS"
if grep -qE 'google_search|browse_url|Playwright' <<<"$MODELS"; then
  echo "FAIL: bridge still advertises cut browser/search tools in /v1/models"
  exit 1
fi

# Test basic prompt – expect valid Responses API shape
RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello from smoke\n","reasoning_effort":"low"}')"
grep -q '"object":"response"' <<<"$RESP"
grep -q '"type":"text"' <<<"$RESP"
grep -q 'hello from smoke' <<<"$RESP"
if grep -qE '<style>|bridge-container|tree-row|event-node' <<<"$RESP"; then
  echo "FAIL: bridge UI leaked into assistant response"
  exit 1
fi

# Test tool definitions present – should not auto-execute tools (Phase 4)
TOOL_RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"list files","tools":[{"type":"function","function":{"name":"google_search","parameters":{"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}}}]}')"
grep -q '"object":"response"' <<<"$TOOL_RESP"
# Should NOT have a tool_call output item (tools present but not invoked)
if grep -q '"type":"tool_call"' <<<"$TOOL_RESP"; then
  echo "FAIL: tools present but should not auto-execute"
  exit 1
fi

# Test streaming request
STREAM_RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello stream\n","stream":true}')"
grep -q 'event: response.created' <<<"$STREAM_RESP"
grep -q 'event: response.completed' <<<"$STREAM_RESP"
if grep -qE '<style>|bridge-container|tree-row|event-node' <<<"$STREAM_RESP"; then
  echo "FAIL: bridge UI leaked into streaming response"
  exit 1
fi
STREAM_RESP="$STREAM_RESP" python3 - <<'PY'
import json
import os
import sys

payload = os.environ["STREAM_RESP"]
events = []
for block in payload.strip().split("\n\n"):
    data = None
    event = None
    for line in block.splitlines():
        if line.startswith("event: "):
            event = line[len("event: "):]
        if line.startswith("data: "):
            data = line[len("data: "):]
    if data is None:
        continue
    try:
        obj = json.loads(data)
    except json.JSONDecodeError as exc:
        print(f"FAIL: invalid stream JSON for {event}: {exc}", file=sys.stderr)
        sys.exit(1)
    if event != obj.get("type"):
        print(f"FAIL: SSE event {event} did not match payload type {obj.get('type')}", file=sys.stderr)
        sys.exit(1)
    events.append(obj)

expected = [
    "response.created",
    "response.output_text.delta",
    "response.completed",
]
seen_types = [event["type"] for event in events]
for event_type in expected:
    if event_type not in seen_types:
        print(f"FAIL: missing stream event {event_type}", file=sys.stderr)
        sys.exit(1)

sequence_numbers = [event["sequence_number"] for event in events]
if sequence_numbers != sorted(sequence_numbers):
    print(f"FAIL: stream sequence numbers not monotonic: {sequence_numbers}", file=sys.stderr)
    sys.exit(1)
PY

echo "All smoke tests passed."
