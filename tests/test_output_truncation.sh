#!/usr/bin/env bash
# T1.5 — output > max_output_buffer → incomplete_details in response + stderr log
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make >/dev/null

PORT="${PORT:-$((27000 + RANDOM % 5000))}"
CFG="$ROOT/tests/.out/truncation_test_config_$$.json"
LOG="$ROOT/tests/.out/.truncation_bridge.log"
mkdir -p "$ROOT/tests/.out"

# 1 KB buffer; we'll send 2 KB of output
cat >"$CFG" <<EOF
{"max_output_buffer": 1024, "use_framed_protocol": true}
EOF

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; cat "$LOG" 2>/dev/null || true; rm -f "$CFG"; exit 1; }

FAKE_AGENT="$ROOT/tests/fake_agent.py"

FAKE_AGENT_BIG_OUTPUT_BYTES=2048 \
  ./bin/star_bridge "$FAKE_AGENT" "$ROOT" -p "$PORT" --config "$CFG" >"$LOG" 2>&1 &
BPID=$!
trap 'kill "$BPID" >/dev/null 2>&1 || true; wait "$BPID" >/dev/null 2>&1 || true; rm -f "$CFG"' EXIT

for _ in {1..50}; do
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

RESP=$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"trigger big output\n"}' 2>/dev/null || true)

# The response events stream — look for incomplete_details in the completed event
if echo "$RESP" | grep -q '"incomplete_details"'; then
  pass "incomplete_details present in response stream"
else
  fail "incomplete_details missing from response stream"
fi

# stderr log must contain the truncation message
if grep -q "truncated assistant text" "$LOG"; then
  pass "truncation logged to stderr"
else
  fail "truncation not logged to stderr"
fi

echo "output truncation test passed"
