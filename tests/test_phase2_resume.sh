#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((20000 + $$ % 20000))}"
LOG="$ROOT/tests/.out/.phase2-server.log"

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

cat > config.json <<'JSON'
{
  "port": __PORT__,
  "theme": "monokai",
  "use_framed_protocol": true,
  "auto_load_resume_session": true,
  "context_tokens": 150000
}
JSON
perl -0pi -e "s/__PORT__/$PORT/g" config.json

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

RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"previous_response_id":"resp_codex_resume_42","input":"resume smoke"}')"

grep -q '"object":"response"' <<<"$RESP"
grep -q 'Fake agent received' <<<"$RESP"
grep -q 'previous_response_id=resp_codex_resume_42' <<<"$RESP"
grep -q 'auto_load_resume_session=true' <<<"$RESP"
grep -q 'context_tokens=150000' <<<"$RESP"
if grep -qE '<style>|bridge-container|tree-row|event-node|config-strip' <<<"$RESP"; then
  echo "FAIL: bridge UI leaked into resume response"
  exit 1
fi

cat > config.json <<'JSON'
{
  "port": __PORT__,
  "theme": "monokai",
  "use_framed_protocol": true,
  "auto_load_resume_session": false,
  "context_tokens": 150000
}
JSON
perl -0pi -e "s/__PORT__/$PORT/g" config.json

kill "$PID" >/dev/null 2>&1 || true
wait "$PID" >/dev/null 2>&1 || true
./bin/star_bridge "$FAKE_AGENT" . --framed -p "$PORT" >"$LOG" 2>&1 &
PID=$!

for _ in {1..50}; do
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

OFF_RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"previous_response_id":"resp_codex_resume_43","input":"resume off smoke"}')"

grep -q 'auto_load_resume_session=false' <<<"$OFF_RESP"
if grep -qE '<style>|bridge-container|tree-row|event-node|config-strip' <<<"$OFF_RESP"; then
  echo "FAIL: bridge UI leaked into resume-off response"
  exit 1
fi

echo "Phase 2 resume config tests passed."
