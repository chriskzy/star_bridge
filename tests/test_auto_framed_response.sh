#!/usr/bin/env bash
# Regression: native_transport=auto with no socket path must select stdio_framed
# and return assistant text from framed response "output" fields.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.auto_framed_response.log"
mkdir -p "$ROOT/tests/.out"

cd "$ROOT"

make >/dev/null

rm -f "$LOG" "$ROOT/.codex-bridge-debug.log"

./bin/star_bridge ./tests/fake_native_agent.py . --no-config -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: server exited"
    cat "$LOG"
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

echo "=== Test: auto stdio_framed returns assistant text ==="
RESP="$(curl -sS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"auto framed works","stream":false}' \
  --max-time 5)"

if echo "$RESP" | grep -q 'Echo: auto framed works'; then
  echo "PASS: assistant text returned"
else
  echo "FAIL: assistant text missing"
  echo "$RESP"
  exit 1
fi

grep -q 'transport_selected transport=stdio_framed' "$ROOT/.codex-bridge-debug.log" || {
  echo "FAIL: auto transport did not select stdio_framed"
  cat "$ROOT/.codex-bridge-debug.log"
  exit 1
}

grep -q 'agent_start .*framed=true' "$ROOT/.codex-bridge-debug.log" || {
  echo "FAIL: auto stdio_framed did not enable framed mode"
  cat "$ROOT/.codex-bridge-debug.log"
  exit 1
}

grep -q 'bridge_to_codex request=1 stream=false content_bytes=[1-9]' "$ROOT/.codex-bridge-debug.log" || {
  echo "FAIL: bridge returned empty Codex content"
  cat "$ROOT/.codex-bridge-debug.log"
  exit 1
}

echo "All auto framed response tests passed."
