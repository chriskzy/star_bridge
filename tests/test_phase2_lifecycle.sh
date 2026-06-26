#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((28000 + $$ % 20000))}"
LOG="$ROOT/tests/.out/.phase2-lifecycle-server.log"

cd "$ROOT"

cleanup() {
  kill "${PID:-}" >/dev/null 2>&1 || true
  wait "${PID:-}" >/dev/null 2>&1 || true
  rm -f config.json
}
trap cleanup EXIT

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

cat > config.json <<JSON
{
  "port": $PORT,
  "use_framed_protocol": true,
  "response_timeout_ms": 250
}
JSON

FAKE_AGENT="$ROOT/tests/fake_agent.sh"
chmod +x "$FAKE_AGENT"

./bin/star_bridge "$FAKE_AGENT" . --framed -p "$PORT" >"$LOG" 2>&1 &
PID=$!

for _ in {1..50}; do
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

HEALTH="$(curl -fsS "http://127.0.0.1:$PORT/health")"
grep -q '"native_status":"ok"' <<<"$HEALTH"

FIRST="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"first lifecycle"}')"
grep -q 'request_count=1' <<<"$FIRST"
grep -q 'reset_session=false' <<<"$FIRST"

SECOND="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"second lifecycle"}')"
grep -q 'request_count=2' <<<"$SECOND"

RESET="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"reset lifecycle","reset_session":true}')"
grep -q 'request_count=1' <<<"$RESET"
grep -q 'reset_session=true' <<<"$RESET"

TIMEOUT="$(curl -sS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  -H 'Accept: application/json' \
  --data '{"input":"delay lifecycle"}')"
grep -q 'native agent response timeout' <<<"$TIMEOUT"

echo "Phase 2 lifecycle tests passed."
