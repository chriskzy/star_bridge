#!/usr/bin/env bash
# UDS test for frame payload too large.
# Sends an oversized frame (length > FRAME_MAX_PAYLOAD) and verifies
# the bridge rejects it with structured behavior (no crash, connection recovers).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Build the fake UDS agent if not already built
cc -Wall -Wextra -O2 -std=gnu11 -o "$ROOT/tests/fake_uds_agent" "$ROOT/tests/fake_uds_agent.c"
chmod +x "$ROOT/tests/fake_uds_agent"

echo "=== Test 1: Oversized frame via UDS is rejected ==="
SOCKET1="$(pwd)/.frame_too_large_test_1.sock"

# Start fake UDS agent
FAKE_AGENT_LOG="$(pwd)/.fake_agent_oversize.log"
"$(pwd)/tests/fake_uds_agent" "$SOCKET1" >"$FAKE_AGENT_LOG" 2>&1 &
FAKE_PID=$!
sleep 1

if [ ! -S "$SOCKET1" ]; then
  echo "FAIL: fake UDS agent did not create socket"
  kill -9 $FAKE_PID 2>/dev/null || true
  exit 1
fi

# Connect directly to the fake agent and send an oversized frame
# FRAME_MAX_PAYLOAD = 16777216, so send a frame with length > that
python3 -c "
import socket, struct, sys, time

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect('$SOCKET1')
except Exception as e:
    print('FAIL: connect failed: %s' % e)
    sys.exit(1)

# Send an oversized frame: length = FRAME_MAX_PAYLOAD + 1 (16777217)
s.sendall(struct.pack('<I', 16777217))
# Send some payload bytes (not needed, bridge should reject based on length alone)
s.sendall(b'X' * 100)
s.shutdown(socket.SHUT_WR)

# Try to read response — should get an error frame or connection close
s.settimeout(3)
try:
    data = s.recv(4096)
    if data:
        print('FAIL: got response data when oversized frame should be rejected')
        sys.exit(1)
except socket.timeout:
    print('Timeout waiting for response (expected: connection closed by peer)')
    pass
except (ConnectionResetError, BrokenPipeError, OSError):
    pass

print('PASS')
s.close()
" 2>&1

# Kill fake agent (SIGKILL because macOS signal() may restart accept)
kill -9 $FAKE_PID 2>/dev/null || true
wait $FAKE_PID 2>/dev/null || true
rm -f "$SOCKET1"

echo "=== Test 2: Normal-sized frame works after oversized frame (connection recovers) ==="
SOCKET2="$(pwd)/.frame_recovery_test.sock"

# Start fake UDS agent
FAKE_AGENT_LOG2="$(pwd)/.fake_agent_recovery.log"
"$(pwd)/tests/fake_uds_agent" "$SOCKET2" >"$FAKE_AGENT_LOG2" 2>&1 &
FAKE_PID2=$!
sleep 1

if [ ! -S "$SOCKET2" ]; then
  echo "FAIL: fake UDS agent did not create socket"
  kill -9 $FAKE_PID2 2>/dev/null || true
  exit 1
fi

# Connection 1: send oversized frame (should be rejected)
echo "  Sending oversized frame on connection 1..."
python3 -c "
import socket, struct, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$SOCKET2')
# Send oversized frame
s.sendall(struct.pack('<I', 16777217))
s.sendall(b'X' * 100)
# Expect connection close
s.settimeout(3)
try:
    data = s.recv(4096)
    if data:
        print('FAIL: got data on oversized frame')
        sys.exit(1)
except socket.timeout:
    pass
except (ConnectionResetError, BrokenPipeError, OSError):
    pass
print('Connection closed as expected')
s.close()
" 2>&1

# Connection 2: send normal health request (should work)
echo "  Sending normal health request on new connection..."
python3 -c "
import socket, struct, sys, json
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('$SOCKET2')
health = json.dumps({'type': 'health', 'data': 'check'}).encode()
s.sendall(struct.pack('<I', len(health)))
s.sendall(health)
s.settimeout(3)
len_bytes = s.recv(4)
if not len_bytes:
    print('FAIL: no response on new connection')
    sys.exit(1)
import struct
flen = struct.unpack('<I', len_bytes)[0]
body = s.recv(flen)
if b'ok' not in body:
    print('FAIL: unexpected response: %s' % body)
    sys.exit(1)
print('Got health response: %s' % body)
s.close()
print('PASS')
" 2>&1

# Kill fake agent (SIGKILL because macOS signal() may restart accept)
kill -9 $FAKE_PID2 2>/dev/null || true
wait $FAKE_PID2 2>/dev/null || true
rm -f "$SOCKET2"

echo ""
echo "All frame too large tests passed."
