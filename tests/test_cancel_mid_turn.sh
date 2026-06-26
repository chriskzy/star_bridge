#!/usr/bin/env bash
# T1.3 — DELETE /v1/responses/<id> cancels an in-progress turn.
# Primary contract (done-signal #4): a DELETE during a slow STREAMING turn ends the
# live SSE stream with response.failed within ~2s, and the bridge stays healthy.
#
# Note: the agent may keep computing in the background after a cancel (agent-limited,
# accepted). Because of that, the streaming cancel runs first on a fresh agent so its
# stream is not polluted by a previous cancelled turn's stale frames.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make >/dev/null

PORT="${PORT:-$((28500 + RANDOM % 5000))}"
CFG="$ROOT/tests/.out/cancel_midturn_cfg_$$.json"
LOG="$ROOT/tests/.out/.cancel_midturn_bridge.log"
SSE_FILE="$ROOT/tests/.out/.cancel_midturn_sse.txt"
mkdir -p "$ROOT/tests/.out"

cat >"$CFG" <<EOF
{"use_framed_protocol": true}
EOF

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; cat "$LOG" 2>/dev/null || true; rm -f "$CFG" "$SSE_FILE"; exit 1; }

./bin/star_bridge tests/fake_agent.py "$ROOT" -p "$PORT" --config "$CFG" >"$LOG" 2>&1 &
BPID=$!
trap 'kill "$BPID" >/dev/null 2>&1 || true; wait "$BPID" >/dev/null 2>&1 || true; rm -f "$CFG" "$SSE_FILE"' EXIT

for _ in {1..50}; do
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

# ------------------------------------------------------------------ #
# Streaming cancel: DELETE during a slow streaming turn must end the   #
# live SSE stream with response.failed within ~2s.                    #
# ------------------------------------------------------------------ #
: >"$SSE_FILE"
START=$(date +%s)
curl -sS -N -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"please delay this stream\n","stream":true}' \
  -o "$SSE_FILE" &
SPID=$!
sleep 0.3
CANCEL=$(curl -sS -X DELETE "http://127.0.0.1:$PORT/v1/responses/stream-id" 2>/dev/null || true)
wait "$SPID" 2>/dev/null || true
ELAPSED=$(( $(date +%s) - START ))

if echo "$CANCEL" | grep -q '"cancelled"'; then
  pass "DELETE returns cancelled status"
else
  fail "DELETE did not return cancelled status (got: $CANCEL)"
fi
if grep -q 'response.failed' "$SSE_FILE"; then
  pass "streaming cancel emitted response.failed on the live stream"
else
  fail "streaming cancel did not emit response.failed (got: $(head -c 400 "$SSE_FILE"))"
fi
if [ "$ELAPSED" -le 2 ]; then
  pass "streaming cancel terminated within ${ELAPSED}s (<= 2s)"
else
  fail "streaming cancel took ${ELAPSED}s (> 2s)"
fi

# Bridge must remain healthy after a cancel (no listen-socket corruption).
ALIVE=$(curl -fsS "http://127.0.0.1:$PORT/v1/models" 2>/dev/null || true)
if echo "$ALIVE" | grep -q '"object":"list"'; then
  pass "bridge alive after cancel"
else
  fail "bridge did not respond after cancel (bridge may have crashed)"
fi

echo "cancel mid-turn test passed"
