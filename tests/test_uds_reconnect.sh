#!/usr/bin/env bash
# UDS reconnect test after fake native agent restart.
#
# Tests:
# 1. Initial connection works (frame exchange)
# 2. Reconnect after agent restart (close, restart agent, reconnect)
# 3. Multiple reconnects work
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the reconnect test binary
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/test_uds_reconnect" "$ROOT/tests/test_uds_reconnect.c" 2>/dev/null || true
chmod +x "$ROOT/tests/test_uds_reconnect" 2>/dev/null || true

# Run the test binary
"$ROOT/tests/test_uds_reconnect" 2>&1 || {
  echo "FAIL: reconnect test binary failed"
  exit 1
}

echo ""
echo "All reconnect tests passed."
