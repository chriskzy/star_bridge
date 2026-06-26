#!/usr/bin/env bash
# Integration: ds4 wrapper must pass --chdir and surface agent failures.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((21000 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.ds4_wrapper_integration.log"
mkdir -p "$ROOT/tests/.out"
FAKE_HOME="/tmp/ds4-bridge-test-home-$$"
FAKE_DS4="$FAKE_HOME/ds4-agent"
FAKE_MODEL="$FAKE_HOME/model.gguf"
FIXTURE="$ROOT/tests/fixtures/fake_persistent_ds4.py"

cd "$ROOT"
mkdir -p "$FAKE_HOME/metal"
touch "$FAKE_HOME/metal/flash_attn.metal"
touch "$FAKE_MODEL"
cp "$FIXTURE" "$FAKE_DS4"
chmod +x "$FAKE_DS4"

make venv >/dev/null
make >/dev/null

rm -f "$LOG" "$ROOT/.codex-bridge-debug.log"

DS4_MODEL_PATH="$FAKE_MODEL" ./bin/star_bridge "$FAKE_DS4" "$ROOT" -p "$PORT" --no-config >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  kill -0 "$PID" >/dev/null 2>&1 || { echo "FAIL: bridge died"; cat "$LOG"; exit 1; }
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"integration ping","stream":false}' --max-time 10)"

echo "$RESP" | grep -q 'fixture-ok' || {
  echo "FAIL: wrapper did not return fake ds4 output"
  echo "$RESP"
  cat "$LOG"
  exit 1
}
echo "$RESP" | grep -q 'integration ping' || {
  echo "FAIL: wrapper response lost original prompt"
  echo "$RESP"
  cat "$LOG"
  exit 1
}

grep -q '\[No output from ds4-agent\]' <<<"$RESP" && {
  echo "FAIL: placeholder output leaked"
  exit 1
}

echo "ds4 wrapper integration test passed."
