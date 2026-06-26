#!/usr/bin/env bash
# Test: Bridge fails closed if type, role, protocol_version, or max_frame_bytes
# is missing or incompatible in native hello response.
#
# Tests:
# 1. Missing type field -> fatal error
# 2. Missing role field -> fatal error
# 3. Protocol version mismatch -> fatal error
# 4. Max_frame_bytes too small -> fatal error
# 5. All fields valid -> success (handshake completes)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the fail-closed test binary
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/test_uds_fail_closed" "$ROOT/tests/test_uds_fail_closed.c" 2>/dev/null || true
chmod +x "$ROOT/tests/test_uds_fail_closed" 2>/dev/null || true

# Run the test binary
"$ROOT/tests/test_uds_fail_closed" 2>&1 || {
  echo "FAIL: fail-closed test binary failed"
  exit 1
}

echo ""
echo "All fail-closed handshake tests passed."
