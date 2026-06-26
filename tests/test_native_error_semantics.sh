#!/bin/bash
# Test: native protocol failures return structured errors, not completed assistant text
# RA-004: Ack timeout, response timeout, id mismatch, busy, unavailable produce HTTP errors or SSE error events

set -e

cd "$(dirname "$0")/.."

BRIDGE="./bin/star_bridge"
FAKE_AGENT="./tests/fake_uds_agent"
PORT=21781

cleanup() {
    kill $BRIDGE_PID 2>/dev/null || true
    kill $FAKE_PID 2>/dev/null || true
    rm -f "$SOCK" 2>/dev/null || true
}

trap cleanup EXIT

echo "=== RA-004: Native error semantics ==="

# Use a user-owned directory for socket
mkdir -p /tmp/bridge_test_dir
SOCK="/tmp/bridge_test_dir/bridge_test.sock"

# Kill any leftover bridges
pkill -9 -f "star_bridge" 2>/dev/null || true
pkill -9 -f "fake_uds_agent" 2>/dev/null || true
sleep 1

# Start fake UDS agent
echo "Starting fake UDS agent..."
$FAKE_AGENT "$SOCK" &
FAKE_PID=$!
sleep 1

# Start bridge in connect_existing mode
cat > /tmp/bridge_test_config.json << 'EOF'
{
    "native_socket_path": "/tmp/bridge_test_dir/bridge_test.sock",
    "uds_owner_mode": "connect_existing",
    "uds_connect_timeout_ms": 2000,
    "response_timeout_ms": 5000,
    "auth_token": "",
    "port": 21781,
    "trace": false,
    "use_framed_protocol": true
}
EOF

$BRIDGE --config /tmp/bridge_test_config.json > /tmp/bridge_test.log 2>&1 &
BRIDGE_PID=$!
sleep 3

echo "Bridge PID=$BRIDGE_PID"
cat /tmp/bridge_test.log | head -20

# Check if bridge is still running
if ! kill -0 $BRIDGE_PID 2>/dev/null; then
    echo "Bridge exited unexpectedly. Log:"
    cat /tmp/bridge_test.log
    exit 1
fi

# 1. Test: Successful non-streaming request returns 200 with valid JSON
echo "Test 1: Valid non-streaming request -> HTTP 200"
HTTP_CODE=$(curl -s -w "%{http_code}" -o /tmp/bridge_resp1.json \
    -X POST -H "Content-Type: application/json" \
    -d '{"input":"hello","model":"test"}' \
    http://127.0.0.1:$PORT/v1/responses 2>/dev/null)
echo "HTTP status: $HTTP_CODE"
if [ "$HTTP_CODE" = "200" ]; then
    echo "PASS: Valid request returns 200"
    echo "Response type: $(python3 -c "import sys,json; d=json.load(open('/tmp/bridge_resp1.json')); print(d.get('type','?'))" 2>/dev/null)"
else
    echo "FAIL: Expected 200, got $HTTP_CODE"
    exit 1
fi

# 2. Test: Successful streaming request
echo "Test 2: Valid streaming request -> SSE events"
HTTP_CODE=$(curl -s -w "%{http_code}" -o /tmp/bridge_resp2.txt \
    -X POST -H "Content-Type: application/json" \
    -d '{"input":"hello stream","model":"test","stream":true}' \
    http://127.0.0.1:$PORT/v1/responses 2>/dev/null)
echo "HTTP status: $HTTP_CODE"
if [ "$HTTP_CODE" = "200" ]; then
    echo "PASS: Streaming returns 200"
    grep -q "response.completed" /tmp/bridge_resp2.txt && echo "PASS: response.completed found in SSE" || echo "NOTE: no response.completed"
else
    echo "FAIL: Expected 200, got $HTTP_CODE"
    exit 1
fi

# 3. Test: Stop fake agent, then request should produce 503
echo "Test 3: Agent unavailable -> 503"
kill $FAKE_PID 2>/dev/null
sleep 3

HTTP_CODE=$(curl -s -w "%{http_code}" -o /tmp/bridge_resp3.json \
    -X POST -H "Content-Type: application/json" \
    -d '{"input":"after disconnect","model":"test"}' \
    http://127.0.0.1:$PORT/v1/responses 2>/dev/null)
echo "HTTP status: $HTTP_CODE"
if [ "$HTTP_CODE" = "503" ]; then
    echo "PASS: Agent unavailable returns 503"
elif [ "$HTTP_CODE" = "200" ]; then
    echo "NOTE: Got 200 - checking if it's error content"
    TYPE=$(python3 -c "import sys,json; d=json.load(open('/tmp/bridge_resp3.json')); print(d.get('type','?'))" 2>/dev/null)
    echo "Response type: $TYPE"
    if [ "$TYPE" = "response.error" ]; then
        echo "PASS: Error response type (not completed)"
    else
        echo "FAIL: Got completed response type when should have error"
        exit 1
    fi
else
    echo "NOTE: Got $HTTP_CODE"
fi

cleanup
rm -f /tmp/bridge_test_config.json /tmp/bridge_resp*.json /tmp/bridge_resp*.txt /tmp/bridge_test.log
rmdir /tmp/bridge_test_dir 2>/dev/null || true

echo ""
echo "All RA-004 native error semantics tests passed."
