#!/usr/bin/env bash
# AUD3 — native error-after-ack regression.
# The fake native agent acks a request, then sends {"type":"error",...}. The bridge
# must return a structured failure IMMEDIATELY (not a completed turn, and well before
# any response timeout).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19060 + RANDOM % 9000))}"
LOG="$ROOT/tests/.out/.error_after_ack.log"
mkdir -p "$ROOT/tests/.out"
cd "$ROOT"
make >/dev/null

# Generous response timeout so a "wait for timeout" bug would blow the --max-time budget.
FAKE_NATIVE_ERROR_AFTER_ACK=1 \
  ./bin/star_bridge ./tests/fake_native_agent.py . --no-config -p "$PORT" \
  --turn-response-timeout-ms 30000 >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  kill -0 "$PID" 2>/dev/null || { echo "FAIL: server exited"; cat "$LOG"; exit 1; }
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

START=$(date +%s)
# --max-time 5 is far below the 30s response timeout; an immediate failure must beat it.
RESP="$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"boom","stream":false}' --max-time 5 || echo "curl_timeout")"
ELAPSED=$(( $(date +%s) - START ))

fail=0
if [ "$RESP" = "curl_timeout" ]; then
  echo "FAIL: bridge did not respond within 5s (waited for turn timeout instead of failing on error)"; fail=1
else
  echo "PASS: bridge responded immediately (HTTP $RESP, ${ELAPSED}s) — did not wait for timeout"
fi

BODY="$(curl -sS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"boom","stream":false}' --max-time 5 || true)"
if echo "$BODY" | grep -qiE 'error|failed'; then
  echo "PASS: response is a structured error, not a completed turn"
else
  echo "FAIL: expected structured error body, got: $(head -c 200 <<<"$BODY")"; fail=1
fi
if echo "$BODY" | grep -q '"status":"completed"'; then
  echo "FAIL: bridge returned a completed turn despite native error-after-ack"; fail=1
fi

[ "$fail" -eq 0 ] || { echo "error-after-ack test FAILED"; exit 1; }
echo "error-after-ack test passed."
