#!/usr/bin/env bash
# UDS integration test for bridge-launched fake agent in launch_and_connect mode.
# The test binary forks a child that creates a UDS socket and acts as a fake agent,
# then the parent waits for the socket, connects, and exchanges frames.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOCKET="$(pwd)/.launch_connect_test_$$.sock"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the launch_and_connect test binary
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/test_uds_launch_and_connect" "$ROOT/tests/test_uds_launch_and_connect.c" 2>/dev/null || true
chmod +x "$ROOT/tests/test_uds_launch_and_connect"

# Run the test binary
"$ROOT/tests/test_uds_launch_and_connect" "$SOCKET" 2>&1

echo ""
echo "UDS launch_and_connect integration test passed."
