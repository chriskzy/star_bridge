#!/usr/bin/env bash
# SB-22 — second harness proof using plain OpenAI Python SDK.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((23600 + RANDOM % 1000))}"
LOG="$ROOT/tests/.out/.second_harness_openai_sdk.log"
MODEL="star-bridge-ds4"
mkdir -p "$ROOT/tests/.out"
cd "$ROOT"

make venv >/dev/null
if ! .venv/bin/python3 -c 'import openai' >/dev/null 2>&1; then
  echo "Installing OpenAI SDK into local venv for SB-22 proof..."
  .venv/bin/python3 -m pip install openai >/dev/null
fi
make >/dev/null

FAKE_NATIVE_FAULT=slow_text_delta \
  ./bin/star_bridge ./tests/fake_native_agent.py . --no-config -p "$PORT" \
    --turn-response-timeout-ms 10000 >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..80}; do
  kill -0 "$PID" 2>/dev/null || { sed -n '1,160p' "$LOG"; exit 1; }
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1

.venv/bin/python3 tests/second_harness_openai_sdk.py "http://127.0.0.1:$PORT/v1" "$MODEL"
