#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-19052}"
LOG="$ROOT/tests/.out/.phase1-server.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

./bin/star_bridge /bin/cat . --native-transport stdio -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

COMPLEX_RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{
    "model": "star-bridge-ds4",
    "previous_response_id": "resp_prev_123",
    "instructions": "system instruction smoke",
    "metadata": {"repo": "star_bridge", "case": "phase1"},
    "input": [
      {
        "role": "user",
        "content": [
          {"type": "input_text", "text": "array input smoke"}
        ]
      }
    ]
  }')"

grep -q '"object":"response"' <<<"$COMPLEX_RESP"
grep -q '"type":"text"' <<<"$COMPLEX_RESP"
grep -q 'system instruction smoke' <<<"$COMPLEX_RESP"
grep -q 'previous_response_id: resp_prev_123' <<<"$COMPLEX_RESP"
grep -q 'metadata:.*repo.*star_bridge' <<<"$COMPLEX_RESP"
grep -q 'array input smoke' <<<"$COMPLEX_RESP"

LARGE_TEXT="$(python3 - <<'PY'
print("large-start " + ("x" * 12000) + " large-tail-marker")
PY
)"
LARGE_PAYLOAD="$(python3 - <<'PY' "$LARGE_TEXT"
import json, sys
print(json.dumps({"input": [{"role": "user", "content": [{"type": "input_text", "text": sys.argv[1]}]}]}))
PY
)"
LARGE_RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$LARGE_PAYLOAD")"

grep -q '"object":"response"' <<<"$LARGE_RESP"
grep -q 'large-start' <<<"$LARGE_RESP"
grep -q 'large-tail-marker' <<<"$LARGE_RESP"

TOOL_RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"google_search query codex bridge phase1"}')"

grep -q '"object":"response"' <<<"$TOOL_RESP"
grep -q '"type":"text"' <<<"$TOOL_RESP"
grep -q 'google_search' <<<"$TOOL_RESP"

echo "Phase 1 input fixture tests passed."
