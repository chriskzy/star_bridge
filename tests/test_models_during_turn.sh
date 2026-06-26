#!/usr/bin/env bash
# T1.3 — GET /v1/models during active turn returns canned model list
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make >/dev/null

PORT="${PORT:-$((28000 + RANDOM % 5000))}"
CFG="$ROOT/tests/.out/midturn_models_cfg_$$.json"
LOG="$ROOT/tests/.out/.midturn_models_bridge.log"
mkdir -p "$ROOT/tests/.out"

cat >"$CFG" <<EOF
{"use_framed_protocol": true}
EOF

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; cat "$LOG" 2>/dev/null || true; rm -f "$CFG"; exit 1; }

./bin/star_bridge tests/fake_agent.py "$ROOT" -p "$PORT" --config "$CFG" >"$LOG" 2>&1 &
BPID=$!
trap 'kill "$BPID" >/dev/null 2>&1 || true; wait "$BPID" >/dev/null 2>&1 || true; rm -f "$CFG"' EXIT

for _ in {1..50}; do
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

# Start a slow turn in background (fake_agent sleeps 1s on "delay")
curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"please delay this response\n"}' >/dev/null 2>&1 &
TURN_PID=$!
trap 'kill "$BPID" "$TURN_PID" >/dev/null 2>&1 || true; wait "$TURN_PID" >/dev/null 2>&1 || true; rm -f "$CFG"' EXIT

sleep 0.2

# Query /v1/models while the turn is running
MODELS=$(curl -fsS "http://127.0.0.1:$PORT/v1/models" 2>/dev/null || true)

# A concurrent POST /v1/responses during the active turn must be rejected with 409.
CONC_CODE=$(curl -sS -o /dev/null -w '%{http_code}' \
  -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"concurrent turn"}' 2>/dev/null || true)

wait "$TURN_PID" 2>/dev/null || true

if [ "$CONC_CODE" = "409" ]; then
  pass "concurrent POST during turn rejected with 409"
else
  fail "concurrent POST expected 409, got: $CONC_CODE"
fi

if echo "$MODELS" | grep -q '"object":"list"'; then
  pass "models endpoint responded during turn"
else
  fail "models endpoint did not respond during turn (got: $MODELS)"
fi

if echo "$MODELS" | grep -q '"id"'; then
  pass "model list contains id field"
else
  fail "model list missing id field"
fi

echo "models during turn test passed"
