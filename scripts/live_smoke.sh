#!/usr/bin/env bash
# Manual-safe live smoke for the real ds4-agent path (SB-10).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HOST="${STAR_BRIDGE_LIVE_HOST:-127.0.0.1}"
PORT="${PORT:-${STAR_BRIDGE_LIVE_PORT:-21780}}"
WORKSPACE="${STAR_BRIDGE_LIVE_WORKSPACE:-$ROOT}"
AGENT="${DS4_AGENT:-}"
MODEL="${DS4_MODEL_PATH:-}"
FORCE=0

usage() {
  cat <<USAGE
Usage: DS4_AGENT=/path/to/ds4-agent DS4_MODEL_PATH=/path/to/model.gguf scripts/live_smoke.sh [options]

Options:
  --agent PATH       ds4-agent binary path
  --model PATH       ds4 model path
  --workspace PATH   workspace sent to bridge (default: repo root)
  --port PORT        HTTP port (default: 21780)
  --force            stop existing ds4-agent processes before running
USAGE
}

while [ $# -gt 0 ]; do
  case "$1" in
    --agent) AGENT="${2:?missing --agent value}"; shift 2 ;;
    --model) MODEL="${2:?missing --model value}"; shift 2 ;;
    --workspace) WORKSPACE="${2:?missing --workspace value}"; shift 2 ;;
    --port) PORT="${2:?missing --port value}"; shift 2 ;;
    --force) FORCE=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "FAIL: unknown option: $1"; usage; exit 1 ;;
  esac
done

OUT="$ROOT/tests/.out"
LOG="$OUT/.live_smoke_bridge.log"
CFG="$OUT/live_smoke_config_$$.json"
SSE_FILE="$OUT/.live_smoke_cancel_sse.txt"
DEBUG_LOG="$ROOT/.codex-bridge-debug.log"
PID=""

pass() { echo "PASS: $1"; }
fail() {
  echo "FAIL: $1"
  echo "--- bridge log ---"
  tail -120 "$LOG" 2>/dev/null || true
  exit 1
}
skip() { echo "SKIP: $1"; exit 0; }

cleanup() {
  kill "${PID:-}" >/dev/null 2>&1 || true
  wait "${PID:-}" >/dev/null 2>&1 || true
  kill "$(lsof -ti:"$PORT" 2>/dev/null)" >/dev/null 2>&1 || true
  rm -f "$CFG" "$SSE_FILE"
}
trap cleanup EXIT

[ -n "$AGENT" ] || skip "set DS4_AGENT or pass --agent"
[ -x "$AGENT" ] || skip "ds4-agent not executable: $AGENT"
[ -n "$MODEL" ] || skip "set DS4_MODEL_PATH or pass --model"
[ -r "$MODEL" ] || skip "model not readable: $MODEL"

if pgrep -f '[/]ds4-agent' >/dev/null 2>&1; then
  if [ "$FORCE" != "1" ]; then
    skip "ds4-agent already running; rerun with --force when safe"
  fi
  bash "$ROOT/scripts/cleanup_stale_bridges.sh" >/dev/null 2>&1 || true
fi

cd "$ROOT"
make venv >/dev/null
make >/dev/null
mkdir -p "$OUT"
rm -f "$LOG" "$SSE_FILE" "$DEBUG_LOG"

cat >"$CFG" <<JSON
{
  "port": $PORT,
  "model_path": "$MODEL",
  "use_framed_protocol": true,
  "auto_load_resume_session": true,
  "auto_save_kv_cache": true,
  "kv_cache_policy": "per_session",
  "context_tokens": ${STAR_BRIDGE_LIVE_CONTEXT_TOKENS:-8192}
}
JSON

"$ROOT/bin/star_bridge" "$AGENT" "$WORKSPACE" \
  --config "$CFG" \
  -p "$PORT" \
  --model-load-timeout-ms "${STAR_BRIDGE_LIVE_MODEL_LOAD_MS:-600000}" \
  --turn-response-timeout-ms "${STAR_BRIDGE_LIVE_TURN_MS:-600000}" \
  >"$LOG" 2>&1 &
PID=$!

for i in $(seq 1 "${STAR_BRIDGE_LIVE_READY_SECONDS:-600}"); do
  kill -0 "$PID" 2>/dev/null || fail "bridge exited during startup"
  if curl -fsS "http://$HOST:$PORT/v1/models" >/dev/null 2>&1; then
    pass "models endpoint ready after ${i}s"
    break
  fi
  sleep 1
done
curl -fsS "http://$HOST:$PORT/v1/models" >/dev/null 2>&1 || fail "models endpoint not ready"

health="$(curl -fsS "http://$HOST:$PORT/health" --max-time 10 2>/dev/null || true)"
echo "$health" | grep -q '"native_status":"ok"' || fail "/health did not report native_status:ok ($health)"
pass "/health native_status ok"

post_json() {
  local body="$1"
  local out="$2"
  local code
  code="$(curl -sS -X POST "http://$HOST:$PORT/v1/responses" \
    -H 'Content-Type: application/json' \
    --data "$body" \
    -o "$out" \
    -w '%{http_code}')"
  [ "$code" = "200" ] || fail "POST expected 200, got $code for body $body"
  grep -q '"object":"response"' "$out" || fail "POST did not return response object"
}

assert_contains() {
  local file="$1"
  local needle="$2"
  grep -Fq "$needle" "$file" || fail "response did not contain expected marker: $needle"
}

NONCE="STAR_BRIDGE_LIVE_$$_$(date +%s)"
RESP1="$OUT/live_smoke_first.json"
post_json "{\"input\":\"Reply exactly: $NONCE FIRST\",\"reset_session\":true}" "$RESP1"
assert_contains "$RESP1" "$NONCE"
pass "fresh first turn returned prompt-specific marker"

RESP2="$OUT/live_smoke_continue.json"
post_json "{\"input\":\"Reply exactly: $NONCE CONTINUE\",\"previous_response_id\":\"live-A\"}" "$RESP2"
assert_contains "$RESP2" "$NONCE"
pass "continued turn returned prompt-specific marker"

RESP_A1="$OUT/live_smoke_a1.json"
RESP_B1="$OUT/live_smoke_b1.json"
RESP_A2="$OUT/live_smoke_a2.json"
post_json "{\"input\":\"Reply exactly: $NONCE A1\",\"previous_response_id\":\"conversation-A\"}" "$RESP_A1"
post_json "{\"input\":\"Reply exactly: $NONCE B1\",\"previous_response_id\":\"conversation-B\"}" "$RESP_B1"
post_json "{\"input\":\"Reply exactly: $NONCE A2\",\"previous_response_id\":\"conversation-A\"}" "$RESP_A2"
assert_contains "$RESP_A1" "$NONCE"
assert_contains "$RESP_B1" "$NONCE"
assert_contains "$RESP_A2" "$NONCE"
grep -q 'session_switch from=conversation-A to=conversation-B' "$DEBUG_LOG" || fail "missing session switch A->B"
grep -q 'session_switch from=conversation-B to=conversation-A' "$DEBUG_LOG" || fail "missing session switch B->A"
pass "save/switch A-B-A recorded"

RESP_EFFORT="$OUT/live_smoke_effort.json"
post_json "{\"input\":\"Reply exactly: $NONCE EFFORT\",\"previous_response_id\":\"conversation-A\",\"reasoning\":{\"effort\":\"high\"}}" "$RESP_EFFORT"
assert_contains "$RESP_EFFORT" "$NONCE"
grep -q 'effort=high' "$DEBUG_LOG" || fail "missing high-effort turn_metrics/debug evidence"
pass "effort change turn completed with debug evidence"

if grep -q "context_tokens=${STAR_BRIDGE_LIVE_CONTEXT_TOKENS:-8192}" "$DEBUG_LOG" ||
   grep -q "ctx=${STAR_BRIDGE_LIVE_CONTEXT_TOKENS:-8192}" "$LOG"; then
  pass "context-token override honored"
else
  fail "context-token override missing from debug log"
fi

SLOW_BODY='{"input":"Stream slowly for live cancel smoke. Count to 100 before final.","stream":true}'
: >"$SSE_FILE"
START=$(date +%s)
curl -sS -N -X POST "http://$HOST:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$SLOW_BODY" \
  -o "$SSE_FILE" &
SPID=$!
sleep 0.5
CONC_CODE="$(curl -sS -X POST "http://$HOST:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"concurrent request should be rejected"}' \
  -o /dev/null \
  -w '%{http_code}' 2>/dev/null || true)"
[ "$CONC_CODE" = "409" ] || fail "concurrent POST expected 409, got $CONC_CODE"
pass "concurrent POST rejected with 409"

CANCEL="$(curl -sS -X DELETE "http://$HOST:$PORT/v1/responses/live-cancel" --max-time 2 2>/dev/null || true)"
wait "$SPID" 2>/dev/null || true
ELAPSED=$(( $(date +%s) - START ))
echo "$CANCEL" | grep -q '"cancelled"' || fail "DELETE did not return cancelled status: $CANCEL"
grep -q 'response.failed' "$SSE_FILE" || fail "cancel stream did not emit response.failed"
[ "$ELAPSED" -le 2 ] || fail "cancel took ${ELAPSED}s (>2s)"
pass "DELETE mid-turn emitted response.failed within ${ELAPSED}s"

curl -fsS "http://$HOST:$PORT/v1/models" >/dev/null 2>&1 || fail "bridge not healthy after cancel"
pass "bridge accepted health check after cancel"

echo "LIVE SMOKE PASS"
