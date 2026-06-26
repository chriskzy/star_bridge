#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((28014 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.phase4-replay-server.log"
CFG="$ROOT/tests/.out/.phase4-replay-config.json"

cd "$ROOT"

cleanup() {
  kill "${PID:-}" >/dev/null 2>&1 || true
  wait "${PID:-}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

cat >"$CFG" <<EOF
{"hide_tool_transcripts": false}
EOF

FAKE_AGENT="$ROOT/tests/fake_agent.sh"
chmod +x "$FAKE_AGENT"

./bin/star_bridge "$FAKE_AGENT" . --framed -p "$PORT" --config "$CFG" >"$LOG" 2>&1 &
PID=$!

for _ in {1..50}; do
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"tool-history-old-first"}' >/dev/null

for _ in {1..19}; do
  curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
    -H 'Content-Type: application/json' \
    --data '{"input":"tool-history-old"}' >/dev/null
done

curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"tool-history-new"}' >/dev/null

REPLAY="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"replay check"}')"

grep -q 'tool_history:' <<<"$REPLAY" || { echo "FAIL: replay response missing tool_history"; exit 1; }
grep -q 'NEW_MARKER_' <<<"$REPLAY" || { echo "FAIL: newest replay marker missing"; exit 1; }
if grep -q 'OLD_FIRST_' <<<"$REPLAY"; then
  echo "FAIL: oldest replay marker should be truncated"
  exit 1
fi

echo "Phase 4 replay truncation tests passed."
