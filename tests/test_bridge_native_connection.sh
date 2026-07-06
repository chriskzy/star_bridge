#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((23000 + $$ % 20000))}"
LOG="$ROOT/tests/.out/.bridge-native-connection.log"

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
  "trace": true,
  "debug_log": true,
  "response_timeout_ms": 500
}
JSON

FAKE_AGENT="$ROOT/tests/fake_agent.sh"
chmod +x "$FAKE_AGENT"

./bin/star_bridge "$FAKE_AGENT" . --framed --no-config -p "$PORT" >"$LOG" 2>&1 &
PID=$!

for _ in {1..50}; do
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

HEALTH="$(curl -fsS "http://127.0.0.1:$PORT/health")"
grep -q '"native_status":"ok"' <<<"$HEALTH"

RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"bridge native connection smoke","stream":false}')"
grep -q '"object":"response"' <<<"$RESP"
grep -q 'Fake agent received: bridge native connection smoke' <<<"$RESP"

DEBUG="$(curl -fsS "http://127.0.0.1:$PORT/debug/session")"
grep -q '"trace":true' <<<"$DEBUG"
grep -q '"method":"POST"' <<<"$DEBUG"
grep -q '"path":"/v1/responses"' <<<"$DEBUG"
grep -q 'bridge native connection smoke' <<<"$DEBUG"

grep -q 'bridge_to_native request=1 protocol=framed' "$ROOT/.codex-bridge-debug.log"
grep -q 'native_to_bridge request=1 status=completed' "$ROOT/.codex-bridge-debug.log"

echo "Bridge/native framed connection test passed."
