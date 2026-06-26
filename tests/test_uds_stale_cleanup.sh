#!/usr/bin/env bash
# UDS test for stale socket cleanup only in bridge-owned mode.
#
# Tests:
# 1. Stale socket is cleaned in launch_and_connect mode (via config.json)
# 2. Stale socket is NOT cleaned in connect_existing mode (default)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

echo "=== Test 1: Stale socket cleaned in launch_and_connect mode ==="

# Create a stale socket
SOCKET1="$(pwd)/.stale_cleanup_test_1_$$.sock"
python3 -c "
import socket, os
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind('$SOCKET1')
s.close()
" 2>/dev/null
if [ -S "$SOCKET1" ]; then
  echo "Created stale socket: $SOCKET1"
else
  echo "FAIL: could not create stale socket"
  exit 1
fi

# Create config.json with launch_and_connect mode
cat > "$ROOT/tests/.out/.stale_config.json" <<EOF
{
  "uds_owner_mode": "launch_and_connect",
  "uds_connect_timeout_ms": 5000
}
EOF

# Start bridge (no --no-config, so it loads .stale_config.json as config.json)
# We need to run from a subdir where config.json exists
mkdir -p "$ROOT/tests/.out/.stale_test_dir"
cp "$ROOT/tests/.out/.stale_config.json" "$ROOT/tests/.out/.stale_test_dir/config.json"
cd "$ROOT/tests/.out/.stale_test_dir"

"$ROOT/bin/star_bridge" /bin/cat . --framed \
  -p 0 \
  --native-transport uds \
  --native-socket-path "$SOCKET1" \
  >"$ROOT/tests/.out/.stale-cleanup-test1.log" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
fi

cd "$ROOT"

# Check if socket was cleaned
if [ -S "$SOCKET1" ]; then
  echo "FAIL: stale socket was NOT cleaned in launch_and_connect mode"
  rm -f "$SOCKET1"
  rm -rf "$ROOT/tests/.out/.stale_test_dir"
  exit 1
fi
echo "PASS (stale socket cleaned)"
rm -rf "$ROOT/tests/.out/.stale_test_dir"
rm -f "$SOCKET1"

echo "=== Test 2: Stale socket NOT cleaned in connect_existing mode (default) ==="

SOCKET2="$(pwd)/.stale_cleanup_test_2_$$.sock"
python3 -c "
import socket, os
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind('$SOCKET2')
s.close()
" 2>/dev/null
if [ -S "$SOCKET2" ]; then
  echo "Created stale socket: $SOCKET2"
else
  echo "FAIL: could not create stale socket"
  exit 1
fi

# Create config.json with connect_existing mode (explicit)
cat > "$ROOT/tests/.out/.stale_config2.json" <<EOF
{
  "uds_owner_mode": "connect_existing",
  "uds_connect_timeout_ms": 5000
}
EOF

mkdir -p "$ROOT/tests/.out/.stale_test_dir2"
cp "$ROOT/tests/.out/.stale_config2.json" "$ROOT/tests/.out/.stale_test_dir2/config.json"
cd "$ROOT/tests/.out/.stale_test_dir2"

"$ROOT/bin/star_bridge" /bin/cat . --framed \
  -p 0 \
  --native-transport uds \
  --native-socket-path "$SOCKET2" \
  >"$ROOT/tests/.out/.stale-cleanup-test2.log" 2>&1 &
PID=$!
sleep 2
if kill -0 $PID 2>/dev/null; then
  kill $PID 2>/dev/null
fi

cd "$ROOT"

# Check if socket was NOT cleaned
if [ ! -S "$SOCKET2" ]; then
  echo "FAIL: stale socket was cleaned in connect_existing mode (should not)"
  rm -f "$SOCKET2"
  rm -rf "$ROOT/tests/.out/.stale_test_dir2"
  exit 1
fi
echo "PASS (stale socket NOT cleaned in connect_existing)"
rm -f "$SOCKET2"
rm -rf "$ROOT/tests/.out/.stale_test_dir2"

echo ""
echo "All stale socket cleanup tests passed."
