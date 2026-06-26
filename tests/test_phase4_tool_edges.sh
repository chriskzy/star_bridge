#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

FAKE_AGENT="$ROOT/tests/fake_agent.sh"
chmod +x "$FAKE_AGENT"

start_bridge() {
  local port="$1"
  local log="$2"
  ./bin/star_bridge "$FAKE_AGENT" . --framed -p "$port" >"$log" 2>&1 &
  PID=$!
  for _ in {1..50}; do
    if curl -fsS "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}

stop_bridge() {
  kill "${PID:-}" >/dev/null 2>&1 || true
  wait "${PID:-}" >/dev/null 2>&1 || true
  PID=""
}

run_case() {
  local port="$1"
  local log="$2"
  local body="$3"
  local outfile
  outfile="$(mktemp)"
  curl --max-time 5 -fsS -X POST "http://127.0.0.1:$port/v1/responses" \
    -H 'Content-Type: application/json' \
    --data "$body" >"$outfile"
  echo "$outfile"
}

cleanup_all() {
  stop_bridge
  rm -f config.json
}
trap cleanup_all EXIT

PID=""

PORT1="${PORT1:-${PORT:-28010}}"
LOG1="$ROOT/tests/.out/.phase4-edge-multi.log"
start_bridge "$PORT1" "$LOG1"
MULTI_FILE="$(run_case "$PORT1" "$LOG1" '{"input":"tool-intent-multi"}')"
grep -q 'Tool error received: google_search' "$MULTI_FILE"
stop_bridge

PORT2="${PORT2:-${PORT:-28011}}"
LOG2="$ROOT/tests/.out/.phase4-edge-malformed.log"
start_bridge "$PORT2" "$LOG2"
MALFORMED_FILE="$(run_case "$PORT2" "$LOG2" '{"input":"tool-intent-malformed"}')"
grep -q 'malformed tool arguments' "$MALFORMED_FILE"
stop_bridge

PORT3="${PORT3:-${PORT:-28012}}"
LOG3="$ROOT/tests/.out/.phase4-edge-denied.log"
start_bridge "$PORT3" "$LOG3"
DENIED_FILE="$(run_case "$PORT3" "$LOG3" '{"input":"tool-intent-denied"}')"
grep -q 'Tool error received: shell_command' "$DENIED_FILE"
stop_bridge

PORT4="${PORT4:-${PORT:-28013}}"
LOG4="$ROOT/tests/.out/.phase4-edge-timeout.log"
start_bridge "$PORT4" "$LOG4"
TIMEOUT_FILE="$(mktemp)"
curl --max-time 5 -sS -X POST "http://127.0.0.1:$PORT4/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"tool-timeout"}' >"$TIMEOUT_FILE" || true
grep -q 'late tool timeout response' "$TIMEOUT_FILE"
stop_bridge

echo "Phase 4 tool edge tests passed."
