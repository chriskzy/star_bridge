#!/usr/bin/env bash
# T2.5 — doctor subcommand: each broken precondition names the right failing check
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

make >/dev/null

BIN="$ROOT/bin/star_bridge"
FREE_PORT=$((34000 + RANDOM % 5000))

pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; exit 1; }

# --- Case 1: broken agent path → names 'agent' ---
set +e
OUT=$("$BIN" --doctor /no/such/agent/binary --no-config -p "$FREE_PORT" 2>&1)
RC=$?
set -e
if [ "$RC" -ne 0 ] && echo "$OUT" | grep -q "DOCTOR FAIL: agent"; then
  pass "broken agent path -> agent failure"
else
  echo "$OUT"; fail "expected agent failure (rc=$RC)"
fi

# --- Case 2: occupied port → names 'port' ---
# Hold a port with a background listener
HOLD_PORT=$((39000 + RANDOM % 5000))
python3 -c "
import socket, time, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $HOLD_PORT))
s.listen(1)
sys.stdout.write('ready\n'); sys.stdout.flush()
time.sleep(10)
" &
HOLD_PID=$!
disown "$HOLD_PID" 2>/dev/null || true
trap 'kill "$HOLD_PID" >/dev/null 2>&1 || true' EXIT
sleep 0.5

set +e
OUT=$("$BIN" --doctor /bin/cat --no-config -p "$HOLD_PORT" 2>&1)
RC=$?
set -e
if [ "$RC" -ne 0 ] && echo "$OUT" | grep -q "DOCTOR FAIL: port"; then
  pass "occupied port -> port failure"
else
  echo "$OUT"; fail "expected port failure (rc=$RC)"
fi
kill "$HOLD_PID" >/dev/null 2>&1 || true

# --- Case 3: missing managed block → names 'codex-config' ---
# Point HOME at a temp dir with no managed config
FAKE_HOME="$(mktemp -d)"
mkdir -p "$FAKE_HOME/.codex"
trap 'kill "$HOLD_PID" >/dev/null 2>&1 || true; rm -rf "$FAKE_HOME"' EXIT

set +e
OUT=$(HOME="$FAKE_HOME" "$BIN" --doctor /bin/cat --no-config -p "$FREE_PORT" 2>&1)
RC=$?
set -e
if [ "$RC" -ne 0 ] && echo "$OUT" | grep -q "DOCTOR FAIL: codex-config"; then
  pass "missing managed block -> codex-config failure"
else
  echo "$OUT"; fail "expected codex-config failure (rc=$RC)"
fi

echo "doctor test passed"
