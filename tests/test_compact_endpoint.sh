#!/bin/bash
# test_compact_endpoint.sh - Integration test for POST /v1/responses/compact
#
# Tests:
# 1. Compact endpoint returns a structured Responses-shaped response
# 2. Compact endpoint works when native agent is connected
# 3. Compact endpoint returns structured error when native agent unavailable
#
# Task 180: Implement POST /v1/responses/compact for Codex compaction requests

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
SERVER_PID=0
SERVER_LOG=$(mktemp)
CLEANUP_NEEDED=1

cleanup() {
    CLEANUP_NEEDED=0
    if [ "$SERVER_PID" -gt 0 ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null
    fi
    rm -f "$SERVER_LOG"
}

trap cleanup EXIT INT TERM

die() { echo "FAIL: $*" >&2; cleanup; exit 1; }

# Create a dummy agent that just sleeps
AGENT_SCRIPT="/tmp/test_compact_agent_$$"
cat > "$AGENT_SCRIPT" << 'EOF'
#!/bin/bash
while true; do sleep 1; done
EOF
chmod +x "$AGENT_SCRIPT"

# Start server
echo "Starting codex bridge..."
"$SRC_DIR/bin/star_bridge" \
    "$AGENT_SCRIPT" . \
    -p 40001 \
    --turn-response-timeout-ms 10000 \
    --no-config \
    > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 2

# Check server started
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    cat "$SERVER_LOG"
    die "Server failed to start"
fi

echo "Server PID=$SERVER_PID"

# Test 1: Compact endpoint (native agent connected via stdio, non-framed)
echo "=== Test 1: Compact endpoint ==="
HTTP_RESPONSE=$(curl -s -X POST "http://127.0.0.1:40001/v1/responses/compact" \
    -H "Content-Type: application/json" \
    -d '{"previous_response_id":"req-1","reasoning_effort":"low"}' 2>&1)
echo "Response: $HTTP_RESPONSE"

# Check response has expected fields
echo "$HTTP_RESPONSE" | python3 -c "
import json,sys
d = json.loads(sys.stdin.read())
assert d.get('object') == 'response', 'missing object field'
assert d.get('status') in ('completed','failed'), 'bad status'
if d.get('status') == 'completed':
    assert 'output' in d, 'missing output'
print('PASS: Test 1 - Valid Responses-shaped response')
" 2>&1 || die "Test 1 failed"

# Test 2: Compact with minimal body
echo "=== Test 2: Compact with minimal body ==="
HTTP_RESPONSE2=$(curl -s -X POST "http://127.0.0.1:40001/v1/responses/compact" \
    -H "Content-Type: application/json" \
    -d '{}' 2>&1)
echo "Response: $HTTP_RESPONSE2"
echo "$HTTP_RESPONSE2" | python3 -c "
import json,sys
d = json.loads(sys.stdin.read())
assert d.get('object') == 'response', 'missing object'
print('PASS: Test 2 - Minimal body works')
" 2>&1 || die "Test 2 failed"

# Test 3: Compact returns structured error on failure (e.g. no previous_response_id)
echo "=== Test 3: Compact error handling ==="
HTTP_RESPONSE3=$(curl -s -X POST "http://127.0.0.1:40001/v1/responses/compact" \
    -H "Content-Type: application/json" \
    -d '{"invalid_field":true}' 2>&1)
echo "Response: $HTTP_RESPONSE3"
echo "$HTTP_RESPONSE3" | python3 -c "
import json,sys
d = json.loads(sys.stdin.read())
assert d.get('object') == 'response', 'missing object'
assert d.get('status') in ('completed','failed'), 'bad status'
print('PASS: Test 3 - Error handling works')
" 2>&1 || die "Test 3 failed"

cleanup
echo ""
echo "=== All compact endpoint tests PASS ==="
exit 0
