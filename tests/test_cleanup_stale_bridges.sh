#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((24000 + RANDOM % 10000))}"

cd "$ROOT"
make venv >/dev/null
make >/dev/null

./bin/star_bridge ./tests/fake_native_agent.py . --no-config -p "$PORT" >/dev/null 2>&1 &
PID=$!

for _ in {1..30}; do
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

bash scripts/cleanup_stale_bridges.sh >/dev/null

if kill -0 "$PID" >/dev/null 2>&1; then
  echo "FAIL: cleanup did not stop bridge pid $PID"
  exit 1
fi

echo "cleanup stale bridges test passed."