#!/usr/bin/env bash
# SB-21 — fault injection suite.
# Public seam: HTTP /v1/responses against the framed fake native agent.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT_BASE="${PORT_BASE:-23100}"
OUT="$ROOT/tests/.out"
mkdir -p "$OUT"
cd "$ROOT"

make >/dev/null

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; exit 1; }

run_fault() {
  local fault="$1"
  local port="$2"
  local log="$OUT/.phase2_fault_${fault}.log"
  local body="$OUT/.phase2_fault_${fault}.json"
  rm -f "$log" "$body" "$ROOT/.codex-bridge-debug.log"

  FAKE_NATIVE_FAULT="$fault" \
    ./bin/star_bridge ./tests/fake_native_agent.py . --no-config -p "$port" \
      --turn-response-timeout-ms 2000 >"$log" 2>&1 &
  local pid=$!
  trap 'kill "$pid" >/dev/null 2>&1 || true; wait "$pid" >/dev/null 2>&1 || true' RETURN

  for _ in {1..60}; do
    kill -0 "$pid" 2>/dev/null || { sed -n '1,160p' "$log"; fail "$fault: bridge exited before readiness"; }
    curl -fsS "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1 && break
    sleep 0.1
  done
  curl -fsS "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1 || fail "$fault: models endpoint not ready"

  local start code elapsed
  start="$(date +%s)"
  code="$(curl -sS -o "$body" -w '%{http_code}' -X POST "http://127.0.0.1:$port/v1/responses" \
    -H 'Content-Type: application/json' \
    --data "{\"input\":\"fault $fault\",\"stream\":false}" \
    --max-time 6 2>/dev/null || echo timeout)"
  elapsed=$(( $(date +%s) - start ))
  [ "$code" != "timeout" ] || { sed -n '1,200p' "$log"; fail "$fault: request hung"; }
  [ "$elapsed" -le 6 ] || fail "$fault: response exceeded bounded time (${elapsed}s)"
  grep -Eq '"status":"failed"|"error"|native agent|native_agent' "$body" || {
    echo "body:"; cat "$body"; sed -n '1,200p' "$log"; fail "$fault: no structured error body";
  }
  if grep -q '"status":"completed"' "$body"; then
    cat "$body"; fail "$fault: reported completed despite fault"
  fi
  grep -q 'turn_metrics .*status=' "$ROOT/.codex-bridge-debug.log" || {
    cat "$ROOT/.codex-bridge-debug.log" 2>/dev/null || true
    fail "$fault: missing turn_metrics"
  }
  curl -fsS "http://127.0.0.1:$port/v1/models" >/dev/null 2>&1 || fail "$fault: bridge not responsive after fault"
  local next_code
  next_code="$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$port/v1/responses" \
    -H 'Content-Type: application/json' \
    --data "{\"input\":\"after $fault\",\"stream\":false}" \
    --max-time 3 2>/dev/null || echo timeout)"
  [ "$next_code" != "timeout" ] || fail "$fault: next request hung"
  pass "$fault bounded failure, metrics, responsive bridge, next request returned HTTP $next_code"

  kill "$pid" >/dev/null 2>&1 || true
  wait "$pid" >/dev/null 2>&1 || true
  trap - RETURN
}

faults=(
  error_after_ack
  killed_mid_turn
  garbage_frame_after_ack
  stall_after_ack
  close_after_ack
  oversized_frame_after_ack
)

i=0
for fault in "${faults[@]}"; do
  run_fault "$fault" "$((PORT_BASE + i))"
  i=$((i + 1))
done

echo "Phase 2 fault injection tests passed."
