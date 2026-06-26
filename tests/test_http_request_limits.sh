#!/bin/bash
# RA-005: HTTP request limits - max_request_size enforcement
# Tests:
# 1. Normal request under limit works
# 2. Request with Content-Length exceeding max_request_size returns 413
# 3. Chunked request exceeding decoded body limit returns 413
# 4. Chunked request within limit works
# 5. Malformed Content-Length (negative) is rejected
# 6. Malformed Content-Length (overflow) is rejected
set -uo pipefail  # no -e: we handle errors explicitly

PORT=${PORT:-$((19050 + RANDOM % 10000))}
BRIDGE_PID=0
FAKE_AGENT_PID=0
AUTH="Bearer test-token-001"

cleanup() {
    kill $BRIDGE_PID 2>/dev/null || true
    kill $FAKE_AGENT_PID 2>/dev/null || true
    (wait $BRIDGE_PID 2>/dev/null; wait $FAKE_AGENT_PID 2>/dev/null) || true
    rm -f /tmp/bridge_test_config.json /tmp/bridge_test_dir/bridge_test.sock 2>/dev/null || true
}
trap cleanup EXIT

# Start fake UDS agent
echo "Starting fake UDS agent..."
rm -f /tmp/bridge_test_dir/bridge_test.sock 2>/dev/null || true
mkdir -p /tmp/bridge_test_dir
cat > /tmp/bridge_test_config.json <<'EOF'
{
  "workspace_root": "/tmp",
  "model_id": "star-bridge-ds4",
  "max_request_size": 4096,
  "auth_token": "test-token-001"
}
EOF

cc -Wall -Wextra -O2 -std=gnu11 -o tests/fake_uds_agent tests/fake_uds_agent.c 2>/dev/null || true
tests/fake_uds_agent /tmp/bridge_test_dir/bridge_test.sock &
FAKE_AGENT_PID=$!
sleep 1

./bin/star_bridge /bin/sh . \
    --native-transport uds \
    --native-socket-path /tmp/bridge_test_dir/bridge_test.sock \
    --uds-owner-mode connect_existing \
    --config /tmp/bridge_test_config.json \
    -p $PORT &
BRIDGE_PID=$!
sleep 2

echo "=== RA-005: HTTP request limits ==="

# 1. Normal request under limit
echo "Test 1: Request under 4KB limit -> 200"
HTTP_CODE=$(curl -s -w "%{http_code}" -o /tmp/limit_resp1.json \
    -X POST -H "Content-Type: application/json" -H "Authorization: $AUTH" \
    -d '{"input":"hello","model":"test"}' \
    http://127.0.0.1:$PORT/v1/responses 2>/dev/null)
if [ "$HTTP_CODE" = "200" ]; then
    echo "PASS: Small request returns 200"
else
    echo "FAIL: Expected 200, got $HTTP_CODE"
    exit 1
fi

# 2. Request with Content-Length exceeding limit
echo "Test 2: Content-Length exceeds max_request_size -> 413"
LARGE_BODY=$(python3 -c "import json; d={'input': 'x'*5000, 'model':'test'}; print(json.dumps(d))" 2>/dev/null)
HTTP_CODE=$(curl -s -w "%{http_code}" -o /tmp/limit_resp2.json \
    -X POST -H "Content-Type: application/json" -H "Authorization: $AUTH" \
    -d "$LARGE_BODY" \
    http://127.0.0.1:$PORT/v1/responses 2>/dev/null)
if [ "$HTTP_CODE" = "413" ]; then
    echo "PASS: Large request returns 413"
else
    echo "FAIL: Expected 413, got $HTTP_CODE"
    exit 1
fi

# 3. Chunked request exceeding decoded body limit
echo "Test 3: Chunked request exceeding limit -> 413"
CHUNKED_REQ=$(python3 -c "
body = '{\"input\":\"x\"' + ', \"model\":\"test\"' + '}'
while len(body) < 5000: body += ' '
body += ']}'
body = body[:5000]
chunk_data = f'{len(body):x}\r\n{body}\r\n0\r\n\r\n'
req = 'POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nContent-Type: application/json\r\nAuthorization: Bearer test-token-001\r\nConnection: close\r\n\r\n' + chunk_data
import sys; sys.stdout.buffer.write(req.encode())
" 2>/dev/null)
HTTP_RESP=$(printf "%s" "$CHUNKED_REQ" | nc -w 5 127.0.0.1 $PORT 2>/dev/null || true)
HTTP_CODE=$(echo "$HTTP_RESP" | head -1 | awk '{print $2}')
if [ "$HTTP_CODE" = "413" ]; then
    echo "PASS: Large chunked request returns 413"
else
    echo "FAIL: Expected 413, got $HTTP_CODE"
    echo "Response: $HTTP_RESP"
    exit 1
fi

# 4. Chunked request within limit works
echo "Test 4: Chunked request under limit -> 200"
SMALL_CHUNKED=$(python3 -c "
body = '{\"input\":\"hello\",\"model\":\"test\"}'
chunk_data = f'{len(body):x}\r\n{body}\r\n0\r\n\r\n'
req = 'POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nContent-Type: application/json\r\nAuthorization: Bearer test-token-001\r\nConnection: close\r\n\r\n' + chunk_data
import sys; sys.stdout.buffer.write(req.encode())
" 2>/dev/null)
HTTP_RESP=$(printf "%s" "$SMALL_CHUNKED" | nc -w 5 127.0.0.1 $PORT 2>/dev/null || true)
HTTP_CODE=$(echo "$HTTP_RESP" | head -1 | awk '{print $2}')
if [ "$HTTP_CODE" = "200" ]; then
    echo "PASS: Small chunked request returns 200"
else
    echo "FAIL: Expected 200, got $HTTP_CODE"
    echo "Response: $HTTP_RESP"
    exit 1
fi

# 5. Malformed Content-Length (negative)
echo "Test 5: Negative Content-Length -> rejected"
NEG_REQ=$(python3 -c "
body = '{\"input\":\"hello\",\"model\":\"test\"}'
req = 'POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: -50\r\nAuthorization: Bearer test-token-001\r\nConnection: close\r\n\r\n' + body
import sys; sys.stdout.buffer.write(req.encode())
" 2>/dev/null)
HTTP_RESP=$(printf "%s" "$NEG_REQ" | nc -w 5 127.0.0.1 $PORT 2>/dev/null || true)
HTTP_CODE=$(echo "$HTTP_RESP" | head -1 | awk '{print $2}')
if [ "$HTTP_CODE" != "200" ]; then
    echo "PASS: Negative Content-Length rejected (got $HTTP_CODE)"
else
    echo "FAIL: Expected non-200, got $HTTP_CODE"
    echo "Response: $HTTP_RESP"
    exit 1
fi

# 6. Malformed Content-Length (overflow)
echo "Test 6: Overflow Content-Length -> rejected"
OVERFLOW_REQ=$(python3 -c "
body = '{\"input\":\"hello\",\"model\":\"test\"}'
req = 'POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: 99999999999999999999\r\nAuthorization: Bearer test-token-001\r\nConnection: close\r\n\r\n' + body
import sys; sys.stdout.buffer.write(req.encode())
" 2>/dev/null)
HTTP_RESP=$(printf "%s" "$OVERFLOW_REQ" | nc -w 5 127.0.0.1 $PORT 2>/dev/null || true)
HTTP_CODE=$(echo "$HTTP_RESP" | head -1 | awk '{print $2}')
if [ "$HTTP_CODE" != "200" ]; then
    echo "PASS: Overflow Content-Length rejected (got $HTTP_CODE)"
else
    echo "FAIL: Expected non-200, got $HTTP_CODE"
    echo "Response: $HTTP_RESP"
    exit 1
fi

cleanup
echo ""
echo "All RA-005 HTTP request limits tests passed."
