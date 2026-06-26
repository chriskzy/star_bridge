#!/usr/bin/env bash
# Test: Bridge sends hello frame to native agent on startup.
#
# Verifies that engine_send_hello produces a JSON hello frame with:
#   type: "hello", role: "bridge", bridge_version, protocol_version: 1,
#   harness_id: "codex.responses", workspace_root, accepted_events, max_frame_bytes
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the hello frame test binary
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/test_uds_hello_frame" "$ROOT/tests/test_uds_hello_frame.c" 2>/dev/null || true
chmod +x "$ROOT/tests/test_uds_hello_frame" 2>/dev/null || true

# Run the test binary
"$ROOT/tests/test_uds_hello_frame" 2>&1 || {
  echo "FAIL: hello frame test binary failed"
  exit 1
}

echo ""
echo "All hello frame tests passed."
