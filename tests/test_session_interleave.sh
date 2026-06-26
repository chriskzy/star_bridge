#!/usr/bin/env bash
# T2.2 — per-Codex-conversation session mapping.
# Interleave two conversation keys A,B,A through the bridge HTTP API and assert
# the bridge derives the key from previous_response_id and issues a session
# switch each time the key changes between turns.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make >/dev/null

PORT="${PORT:-$((36000 + RANDOM % 5000))}"
CFG="$ROOT/tests/.out/session_interleave_cfg_$$.json"
LOG="$ROOT/tests/.out/.session_interleave_bridge.log"
DEBUG_LOG="$ROOT/.codex-bridge-debug.log"
mkdir -p "$ROOT/tests/.out"

cat >"$CFG" <<EOF
{"use_framed_protocol": true, "auto_load_project_session": true, "auto_save_kv_cache": true, "kv_cache_policy": "per_session"}
EOF

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; cat "$LOG" 2>/dev/null || true; rm -f "$CFG"; exit 1; }

rm -f "$DEBUG_LOG"
./bin/star_bridge tests/fake_agent.py "$ROOT" -p "$PORT" --config "$CFG" >"$LOG" 2>&1 &
BPID=$!
trap 'kill "$BPID" >/dev/null 2>&1 || true; wait "$BPID" >/dev/null 2>&1 || true; rm -f "$CFG"' EXIT

for _ in {1..50}; do
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

turn() {
  curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
    -H 'Content-Type: application/json' \
    --data "{\"input\":\"turn for $1\",\"previous_response_id\":\"$1\"}" >/dev/null 2>&1 || true
}

# A, B, A
turn conv-A
turn conv-B
turn conv-A

sleep 0.3

[ -f "$DEBUG_LOG" ] || fail "debug log not written"

# A -> B switch
if grep -q 'session_switch from=conv-A to=conv-B' "$DEBUG_LOG"; then
  pass "switch A->B on key change"
else
  echo "--- debug log ---"; grep session_ "$DEBUG_LOG" || true
  fail "expected session_switch from=conv-A to=conv-B"
fi

# B -> A switch (third turn resumes conversation A)
if grep -q 'session_switch from=conv-B to=conv-A' "$DEBUG_LOG"; then
  pass "switch B->A: third turn resumes conversation A"
else
  echo "--- debug log ---"; grep session_ "$DEBUG_LOG" || true
  fail "expected session_switch from=conv-B to=conv-A"
fi

echo "session interleave test passed"
