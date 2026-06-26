#!/usr/bin/env bash
# UDS unit test for successful connect_existing connection.
# Starts fake_uds_agent, connects via C test binary, exchanges frames.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOCKET="$(pwd)/.uds_connect_test_$$.sock"
AGENT_LOG="$ROOT/tests/.out/.fake-uds-agent.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the fake UDS agent and test binary
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/fake_uds_agent" "$ROOT/tests/fake_uds_agent.c" 2>/dev/null || true
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/test_uds_connect_existing" "$ROOT/tests/test_uds_connect_existing.c" 2>/dev/null || true
chmod +x "$ROOT/tests/fake_uds_agent" "$ROOT/tests/test_uds_connect_existing"

# Start the fake UDS agent
"$ROOT/tests/fake_uds_agent" "$SOCKET" >"$AGENT_LOG" 2>&1 &
AGENT_PID=$!
trap 'kill "$AGENT_PID" >/dev/null 2>&1 || true; rm -f "$SOCKET"' EXIT
trap 'kill "$AGENT_PID" >/dev/null 2>&1 || true; rm -f "$SOCKET"' INT TERM

# Wait for socket to be created
for _ in {1..20}; do
  if [ -S "$SOCKET" ]; then
    break
  fi
  sleep 0.2
done

if ! [ -S "$SOCKET" ]; then
  echo "FAIL: fake UDS agent did not create socket"
  exit 1
fi

echo "Fake UDS agent listening on $SOCKET"

# Run the connect_existing test binary
"$ROOT/tests/test_uds_connect_existing" "$SOCKET" 2>&1

echo ""
echo "UDS connect_existing test passed."
