#!/usr/bin/env bash
# Failure-mode fixture: wrong command (interactive shell) test.
#
# Tests that starting the bridge with an interactive shell (/bin/zsh)
# as the native agent command produces a "protocol not ready" error
# because the shell does not speak the framed protocol.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_LOG="$ROOT/tests/.out/.bridge-failure-fixture.log"
mkdir -p "$ROOT/tests/.out"
PORT=21779

cd "$ROOT"

# Kill any leftover processes
kill $(lsof -ti:$PORT) 2>/dev/null || true
sleep 0.3

echo ""
echo "=== Failure-mode fixture: wrong command ==="

# Start bridge with /bin/zsh as the native agent (wrong command)
echo "Starting bridge with /bin/zsh as native agent..."
"$ROOT/bin/star_bridge" "/bin/zsh" "$ROOT" -p "$PORT" --framed >"$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!
echo "Bridge PID=$BRIDGE_PID"

# Wait briefly for bridge to fail
sleep 2

# Check if bridge is still running
if kill -0 $BRIDGE_PID 2>/dev/null; then
    echo "Bridge still running after 2s — checking status..."
    # Try a request
    REQUEST_RESULT=$(python3 -c "
import http.client
try:
    conn = http.client.HTTPConnection('localhost', $PORT, timeout=2)
    conn.request('POST', '/v1/responses', '{\"input\":\"test\"}', {'Content-Type': 'application/json'})
    r = conn.getresponse()
    print(r.status, r.reason)
    print(r.read().decode()[:300])
    conn.close()
except Exception as e:
    print('connection error:', e)
" 2>/dev/null || echo "connection failed")
    echo "Request result: $REQUEST_RESULT"
fi

# Wait for bridge to exit (max 15 seconds)
echo "Waiting for bridge to exit..."
for i in $(seq 1 30); do
    if ! kill -0 $BRIDGE_PID 2>/dev/null; then
        echo "Bridge exited after ${i}s"
        break
    fi
    sleep 0.5
done

# Capture exit code
WAIT_EXIT=0
wait $BRIDGE_PID 2>/dev/null || WAIT_EXIT=$?
echo "Bridge exit code: $WAIT_EXIT"

echo ""
echo "--- Bridge log ---"
cat "$BRIDGE_LOG"

echo ""
echo "--- Test result ---"

# Verify bridge exited with error
if [ $WAIT_EXIT -eq 0 ]; then
    echo "FAIL: Bridge exited with 0 (expected non-zero)"
    exit 1
fi

# Verify log contains protocol-not-ready error
if grep -q -i "protocol_not_ready\|handshake_timeout\|handshake failed\|native hello" "$BRIDGE_LOG"; then
    echo "PASS: Bridge reported protocol/handshake error"
elif grep -q -i "failed\|error\|fatal" "$BRIDGE_LOG"; then
    echo "PASS: Bridge reported error"
else
    echo "FAIL: Bridge log does not contain expected error"
    exit 1
fi

echo ""
echo "Failure-mode fixture test completed."
exit 0
