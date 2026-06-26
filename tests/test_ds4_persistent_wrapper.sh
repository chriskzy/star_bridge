#!/usr/bin/env bash
# Two-turn fixture: persistent ds4 stdin mode should serve both requests.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((23000 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.ds4_persistent_wrapper.log"
mkdir -p "$ROOT/tests/.out"
FAKE_HOME="/tmp/ds4-persistent-home-$$"
FAKE_DS4="$FAKE_HOME/ds4-agent"
FAKE_MODEL="$FAKE_HOME/model.gguf"
FIXTURE="$ROOT/tests/fixtures/fake_persistent_ds4.py"

mkdir -p "$FAKE_HOME/metal"
touch "$FAKE_HOME/metal/flash_attn.metal"
touch "$FAKE_MODEL"
cp "$FIXTURE" "$FAKE_DS4"
chmod +x "$FAKE_DS4"

cd "$ROOT"
make venv >/dev/null
make >/dev/null

rm -f "$LOG" "$ROOT/.codex-bridge-debug.log"

DS4_MODEL_PATH="$FAKE_MODEL" ./bin/star_bridge "$FAKE_DS4" "$ROOT" -p "$PORT" --no-config \
  --turn-response-timeout-ms 30000 >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true; rm -rf "$FAKE_HOME"' EXIT

# TDD verification for UDS end-to-end ds4 (bridge <-> wrapper over UDS)
for _ in {1..30}; do
  if grep -q "UDS transport to bridge" "$LOG" 2>/dev/null || grep -q "binding UDS for bridge" "$LOG" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
grep -q -E "(UDS transport to bridge|binding UDS for bridge)" "$LOG" || { echo "FAIL: ds4 did not use UDS end-to-end"; cat "$LOG"; exit 1; }

for _ in {1..50}; do
  kill -0 "$PID" >/dev/null 2>&1 || { echo "FAIL: bridge died"; cat "$LOG"; exit 1; }
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

R1="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"turn one","stream":false}' --max-time 15)"
R2="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"turn two","stream":false}' --max-time 15)"

# The prompt now includes long directives (ws, completion requirement for synthesis); fixture echoes input so match parts
echo "$R1" | grep -q 'fixture-ok' || { echo "FAIL: turn one (no fixture-ok)"; echo "$R1"; exit 1; }
echo "$R1" | grep -q 'turn one' || { echo "FAIL: turn one"; echo "$R1"; exit 1; }
echo "$R2" | grep -q 'fixture-ok' || { echo "FAIL: turn two (no fixture-ok)"; echo "$R2"; exit 1; }
echo "$R2" | grep -q 'turn two' || { echo "FAIL: turn two"; echo "$R2"; exit 1; }

echo "ds4 persistent wrapper test (with UDS) passed."
