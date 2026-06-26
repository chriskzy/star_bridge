#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((22000 + RANDOM % 10000))}"

cd "$ROOT"
make >/dev/null

./bin/star_bridge ./tests/fake_native_agent.py . --no-config -p "$PORT" >/dev/null 2>&1 &
PID1=$!
trap 'kill "$PID1" >/dev/null 2>&1 || true; wait "$PID1" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

set +e
OUT="$(./bin/star_bridge ./tests/fake_native_agent.py . --no-config -p "$PORT" 2>&1)"
RC=$?
set -e

[[ "$RC" -ne 0 ]] || { echo "FAIL: second bind should fail"; exit 1; }
echo "$OUT" | grep -q "already in use" || {
  echo "FAIL: missing friendly port-in-use message"
  echo "$OUT"
  exit 1
}

echo "port-in-use test passed."