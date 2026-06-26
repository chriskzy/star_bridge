#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((24000 + $$ % 20000))}"
LOG="$ROOT/tests/.out/.phase2-spawn-server.log"

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
  "model_path": "/tmp/dwarfstar-model-q4.bin",
  "kv_cache_dir": "/tmp/dwarfstar-kv-cache",
  "agent_env": "DWARFSTAR_MODE=test",
  "extra_native_args": "--native-agent --no-banner"
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

RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"spawn config smoke"}')"

grep -q '"object":"response"' <<<"$RESP"
grep -q 'argv=.*--model /tmp/dwarfstar-model-q4.bin' <<<"$RESP"
grep -q 'argv=.*--kv-cache-dir /tmp/dwarfstar-kv-cache' <<<"$RESP"
grep -q 'argv=.*--native-agent --no-banner' <<<"$RESP"
grep -q 'env=DWARFSTAR_MODE=test' <<<"$RESP"

echo "Phase 2 spawn config tests passed."
