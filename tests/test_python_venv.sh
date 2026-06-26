#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make venv >/dev/null

PY="$ROOT/.venv/bin/python3"
[[ -x "$PY" ]] || { echo "FAIL: .venv/bin/python3 missing"; exit 1; }

VER="$("$PY" --version 2>&1)"
echo "$VER" | grep -q 'Python 3\.14\.' || {
  echo "FAIL: expected Python 3.14.x, got: $VER"
  exit 1
}

# Bridge must launch ds4 wrapper via project venv, not system python.
FAKE_HOME="/tmp/ds4-venv-bridge-home-$$"
FAKE_DS4="$FAKE_HOME/ds4-agent"
PORT="${PORT:-$((25000 + RANDOM % 10000))}"
FIXTURE="$ROOT/tests/fixtures/fake_persistent_ds4.py"

mkdir -p "$FAKE_HOME/metal"
touch "$FAKE_HOME/metal/flash_attn.metal"
cp "$FIXTURE" "$FAKE_DS4"
chmod +x "$FAKE_DS4"

make >/dev/null
rm -f "$ROOT/.codex-bridge-debug.log"

LOG="$ROOT/tests/.out/.python_venv_bridge.log"
mkdir -p "$ROOT/tests/.out"
./bin/star_bridge "$FAKE_DS4" "$ROOT" -p "$PORT" --no-config >"$LOG" 2>&1 &
BPID=$!
trap 'kill "$BPID" >/dev/null 2>&1 || true; wait "$BPID" >/dev/null 2>&1 || true; rm -rf "$FAKE_HOME"' EXIT

for _ in {1..50}; do
  curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break
  sleep 0.1
done

grep -q '\.venv/bin/python3' "$LOG" || {
  echo "FAIL: bridge stderr should mention .venv/bin/python3"
  cat "$LOG"
  exit 1
}

grep -q 'python=.*\.venv/bin/python3' "$ROOT/.codex-bridge-debug.log" || {
  echo "FAIL: debug log should record venv python path"
  cat "$ROOT/.codex-bridge-debug.log" 2>/dev/null || true
  exit 1
}

# Without venv, ds4 bridge path must fail fast with setup hint.
VENV_BACKUP="$ROOT/.venv.testbak-$$"
mv "$ROOT/.venv" "$VENV_BACKUP"
trap 'mv "$VENV_BACKUP" "$ROOT/.venv" 2>/dev/null || true; rm -rf "$FAKE_HOME"' EXIT

set +e
NO_VENV_OUT="$(./bin/star_bridge "$FAKE_DS4" "$ROOT" -p "$((PORT + 1))" --no-config 2>&1)"
NO_VENV_RC=$?
set -e

mv "$VENV_BACKUP" "$ROOT/.venv"

[[ "$NO_VENV_RC" -ne 0 ]] || { echo "FAIL: bridge should fail without venv"; exit 1; }
echo "$NO_VENV_OUT" | grep -q 'make venv' || {
  echo "FAIL: expected make venv hint"
  echo "$NO_VENV_OUT"
  exit 1
}

echo "python venv test passed ($VER)"