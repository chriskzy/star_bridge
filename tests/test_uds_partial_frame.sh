#!/usr/bin/env bash
# UDS test for partial frame reads/writes.
#
# Tests that fragmented frames are assembled correctly:
# 1. Fragmented write (body in 3 chunks)
# 2. Split write (length prefix separate from body)
# 3. Multiple fragmented frames in sequence
# 4. Empty frame
# 5. Byte-by-byte write (worst-case fragmentation)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the partial frame test binary
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/test_uds_partial_frame" "$ROOT/tests/test_uds_partial_frame.c" 2>/dev/null || true
chmod +x "$ROOT/tests/test_uds_partial_frame" 2>/dev/null || true

# Run the test binary
"$ROOT/tests/test_uds_partial_frame" 2>&1 || {
  echo "FAIL: partial frame test binary failed"
  exit 1
}

echo ""
echo "All partial frame read/write tests passed."
