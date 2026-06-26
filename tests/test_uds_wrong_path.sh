#!/usr/bin/env bash
# UDS test for wrong/missing socket path returning structured startup error.
# The bridge should fail with a fatal error message, not hang or crash.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

echo "=== Test 1: Missing socket path (parent dir does not exist) ==="
LOG1="$ROOT/tests/.out/.wrong-path-test1.log"
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "/nonexistent/path/that/does/not/exist.sock" \
  >"$LOG1" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running (should have failed)"
  exit 1
fi
grep -q 'Fatal: socket.*validation failed' "$LOG1" || {
  cat "$LOG1"
  echo "FAIL: no structured error for missing socket path"
  exit 1
}
echo "PASS"

echo "=== Test 2: Empty socket path ==="
LOG2="$ROOT/tests/.out/.wrong-path-test2.log"
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "" \
  >"$LOG2" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running with empty path"
  exit 1
fi
# Empty path skips path validation but fails in uds_connect
grep -q 'Fatal:.*failed\|bind: Address already in use\|Failed to start agent process' "$LOG2" || {
  cat "$LOG2"
  echo "FAIL: no structured error for empty socket path"
  exit 1
}
echo "PASS"

echo "=== Test 3: Socket path with root-owned parent (/tmp) ==="
LOG3="$ROOT/tests/.out/.wrong-path-test3.log"
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "/tmp/test_uds_wrong_path.sock" \
  >"$LOG3" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running with /tmp path"
  exit 1
fi
grep -q 'Fatal: socket parent directory validation failed: native_socket_parent_dir_root_owned' "$LOG3" || {
  cat "$LOG3"
  echo "FAIL: no structured error for root-owned parent"
  exit 1
}
echo "PASS"

echo ""
echo "All wrong/missing socket path tests passed."
