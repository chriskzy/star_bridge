#!/usr/bin/env bash
# Test: Bridge reads and validates native hello response.
#
# Tests:
# 1. Valid native hello response with all required fields
# 2. Missing type field -> rejection
# 3. Protocol version mismatch -> rejection
# 4. Missing agent_name -> rejection
# 5. Missing supported_transports -> rejection
# 6. Missing role field -> rejection
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the native hello test binary
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/test_uds_native_hello" "$ROOT/tests/test_uds_native_hello.c" 2>/dev/null || true
chmod +x "$ROOT/tests/test_uds_native_hello" 2>/dev/null || true

# Run the test binary
"$ROOT/tests/test_uds_native_hello" 2>&1 || {
  echo "FAIL: native hello test binary failed"
  exit 1
}

echo ""
echo "All native hello response tests passed."
