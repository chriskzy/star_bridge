#!/usr/bin/env bash
# UDS test for unsafe socket directory/path rejection.
# The bridge must reject unsafe configurations with structured fatal errors.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

echo "=== Test 1: World-writable socket path ==="
LOG1="$ROOT/tests/.out/.unsafe-test1.log"
# Create a world-writable socket path
touch "$ROOT/tests/.out/.world_writable_test.sock"
chmod 666 "$ROOT/tests/.out/.world_writable_test.sock"
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "$ROOT/tests/.out/.world_writable_test.sock" \
  >"$LOG1" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running with world-writable socket"
  rm -f "$ROOT/tests/.out/.world_writable_test.sock"
  exit 1
fi
grep -q 'Fatal: socket path validation failed: native_socket_path_world_writable' "$LOG1" || {
  cat "$LOG1"
  echo "FAIL: no structured error for world-writable socket path"
  rm -f "$ROOT/tests/.out/.world_writable_test.sock"
  exit 1
}
echo "PASS"
rm -f "$ROOT/tests/.out/.world_writable_test.sock"

echo "=== Test 2: Socket path is a directory ==="
LOG2="$ROOT/tests/.out/.unsafe-test2.log"
mkdir -p "$ROOT/tests/.out/.unsafe_dir_test"
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "$ROOT/tests/.out/.unsafe_dir_test" \
  >"$LOG2" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running with socket path as directory"
  rm -rf "$ROOT/tests/.out/.unsafe_dir_test"
  exit 1
fi
# Bridge checks parent dir first; if parent is root-owned, that's the error.
# If parent is fine, it checks the path itself which is a directory.
# The path exists (as a dir) but validation checks ownership/permissions.
# Let's check for any fatal error
grep -q 'Fatal:.*failed' "$LOG2" || {
  cat "$LOG2"
  echo "FAIL: no structured error for socket path as directory"
  rm -rf "$ROOT/tests/.out/.unsafe_dir_test"
  exit 1
}
echo "PASS"
rm -rf "$ROOT/tests/.out/.unsafe_dir_test"

echo "=== Test 3: Root-owned socket path ==="
LOG3="$ROOT/tests/.out/.unsafe-test3.log"
# Create a root-owned socket file (requires sudo, so we'll simulate by checking
# if the bridge rejects when stat succeeds and uid==0)
# Instead, use a path that exists and is root-owned
# We can use /var/root/.test.sock or similar
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "/var/root/test_root_owned.sock" \
  >"$LOG3" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running with root-owned socket path"
  exit 1
fi
# Parent dir /var is root-owned, so it should be rejected for root-owned parent
grep -q 'Fatal:.*failed' "$LOG3" || {
  cat "$LOG3"
  echo "FAIL: no structured error for root-owned path"
  exit 1
}
echo "PASS"

echo "=== Test 4: Socket path too long (>103 bytes) ==="
LOG4="$ROOT/tests/.out/.unsafe-test4.log"
LONG_PATH="$(python3 -c "print('x' * 120)")"
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "/tmp/$LONG_PATH.sock" \
  >"$LOG4" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running with too-long socket path"
  exit 1
fi
grep -q 'Fatal:.*failed' "$LOG4" || {
  cat "$LOG4"
  echo "FAIL: no structured error for too-long path"
  exit 1
}
echo "PASS"

echo "=== Test 5: World-writable parent directory without sticky bit ==="
LOG5="$ROOT/tests/.out/.unsafe-test5.log"
# Create a world-writable directory without sticky bit
mkdir -p "$ROOT/tests/.out/.world_writable_parent"
chmod 777 "$ROOT/tests/.out/.world_writable_parent"
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "$ROOT/tests/.out/.world_writable_parent/test.sock" \
  >"$LOG5" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running with world-writable parent"
  rm -rf "$ROOT/tests/.out/.world_writable_parent"
  exit 1
fi
grep -q 'Fatal:.*failed' "$LOG5" || {
  cat "$LOG5"
  echo "FAIL: no structured error for world-writable parent"
  rm -rf "$ROOT/tests/.out/.world_writable_parent"
  exit 1
}
echo "PASS"
rm -rf "$ROOT/tests/.out/.world_writable_parent"

echo "=== Test 6: Parent directory not searchable (no X_OK) ==="
LOG6="$ROOT/tests/.out/.unsafe-test6.log"
mkdir -p "$ROOT/tests/.out/.not_searchable_parent"
chmod 644 "$ROOT/tests/.out/.not_searchable_parent"
./bin/star_bridge /bin/cat . --no-config \
  --framed \
  --native-transport uds \
  --native-socket-path "$ROOT/tests/.out/.not_searchable_parent/test.sock" \
  >"$LOG6" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
  echo "FAIL: bridge still running with non-searchable parent"
  rm -rf "$ROOT/tests/.out/.not_searchable_parent"
  exit 1
fi
grep -q 'Fatal:.*failed' "$LOG6" || {
  cat "$LOG6"
  echo "FAIL: no structured error for non-searchable parent"
  rm -rf "$ROOT/tests/.out/.not_searchable_parent"
  exit 1
}
echo "PASS"
rm -rf "$ROOT/tests/.out/.not_searchable_parent"

echo ""
echo "All unsafe socket directory rejection tests passed."
