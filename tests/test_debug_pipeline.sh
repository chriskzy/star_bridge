#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
SERVER_LOG="$ROOT/tests/.out/.debug-pipeline-server.log"
mkdir -p "$ROOT/tests/.out"
DEBUG_LOG="$ROOT/.codex-bridge-debug.log"

cd "$ROOT"

make >/dev/null 2>&1 || make >/dev/null
rm -f "$SERVER_LOG" "$DEBUG_LOG"

FAKE_AGENT="$ROOT/tests/fake_agent.sh"
chmod +x "$FAKE_AGENT"

./bin/star_bridge "$FAKE_AGENT" . --framed --no-config -p "$PORT" >"$SERVER_LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: server exited before readiness"
    cat "$SERVER_LOG"
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"pipeline debug smoke","stream":false,"reasoning_effort":"low"}')"

grep -q 'Fake agent received' <<<"$RESP"
sleep 0.3

require_log() {
  local pattern="$1"
  local label="$2"
  if ! grep -qE "$pattern" "$DEBUG_LOG"; then
    echo "FAIL: debug log missing $label"
    echo "pattern=$pattern"
    echo "--- debug log ---"
    test -f "$DEBUG_LOG" && cat "$DEBUG_LOG"
    echo "--- server log ---"
    cat "$SERVER_LOG"
    exit 1
  fi
}

require_log 'agent_start command=.*fake_agent\.sh.*framed=true' 'agent start'
require_log 'codex_to_bridge request=1 method=POST path=/v1/responses' 'Codex ingress'
require_log 'bridge_normalized request=1 input_bytes=[1-9][0-9]*' 'normalized input'
require_log 'bridge_normalized request=1 .*pipeline debug smoke.*context_tokens' 'normalized input delimiter'
require_log 'bridge_to_native request=1 protocol=framed payload_bytes=[1-9][0-9]*' 'native request send'
require_log 'native_to_bridge request=1 status=completed response_bytes=[1-9][0-9]*' 'native response'
require_log 'bridge_to_codex request=1 stream=false content_bytes=[1-9][0-9]*' 'Codex egress'

echo "Debug pipeline tests passed."
