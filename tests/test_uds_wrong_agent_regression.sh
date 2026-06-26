#!/usr/bin/env bash
# Regression test: wrong stdio/framed command cannot be mistaken for healthy UDS native agent.
#
# Tests (via C binary):
# 1. uds_connect to non-existent socket returns structured error
# 2. uds_connect to a socket with no listening agent returns ECONNREFUSED
# 3. Connecting to a file that is not a socket fails
# 4. uds_connect with empty path fails
# 5. Connect to valid listening socket works
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the regression test binary
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/test_uds_wrong_agent_regression" "$ROOT/tests/test_uds_wrong_agent_regression.c" 2>/dev/null || true
chmod +x "$ROOT/tests/test_uds_wrong_agent_regression" 2>/dev/null || true

# Run the test binary
"$ROOT/tests/test_uds_wrong_agent_regression" 2>&1 || {
  echo "FAIL: wrong-agent regression test binary failed"
  exit 1
}

echo ""
echo "All wrong-agent regression tests passed."
