#!/usr/bin/env bash
# Test auth: verify that auth token validation works for non-loopback requests
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.auth_test.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

# Create a config file with auth token enabled
cat > /tmp/auth_config.json << 'EOF'
{
  "auth_token": "test-secret-token-123"
}
EOF

./bin/star_bridge /bin/cat . --native-transport stdio --config /tmp/auth_config.json -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: server exited"
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

# Helper: send request and extract HTTP status code + body
do_request() {
  curl -s -o /tmp/auth_resp_body.txt -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/responses" \
    -H 'Content-Type: application/json' \
    -H "$1" \
    --data "$2" \
    --max-time 2 2>/dev/null || true
}

echo "=== Test 1: Request without auth token ==="
HTTP_CODE=$(curl -s -o /tmp/auth_resp_body.txt -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"hello","stream":false}' \
  --max-time 2 2>/dev/null || true)
if [ "$HTTP_CODE" = "401" ]; then
  echo "PASS: request without auth returns 401"
else
  echo "FAIL: expected 401, got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 2: Request with valid Bearer auth ==="
HTTP_CODE=$(do_request 'Authorization: Bearer test-secret-token-123' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "200" ]; then
  echo "PASS: valid Bearer auth returns 200"
else
  echo "FAIL: expected 200, got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 3: Request with valid Token auth ==="
HTTP_CODE=$(do_request 'Authorization: Token test-secret-token-123' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "200" ]; then
  echo "PASS: valid Token auth returns 200"
else
  echo "FAIL: expected 200, got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 4: Request with wrong auth token ==="
HTTP_CODE=$(do_request 'Authorization: Bearer wrong-token' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "401" ]; then
  echo "PASS: wrong token returns 401"
else
  echo "FAIL: expected 401, got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 5: Prefix bypass attempt (shorter prefix of real token) ==="
# "test-secret-token-1" is a prefix of the real token "test-secret-token-123"
# It must be rejected because length differs
HTTP_CODE=$(do_request 'Authorization: Bearer test-secret-token-1' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "401" ]; then
  echo "PASS: prefix bypass attempt returns 401"
else
  echo "FAIL: expected 401 (prefix bypass), got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 6: Suffix bypass attempt (longer than real token) ==="
HTTP_CODE=$(do_request 'Authorization: Bearer test-secret-token-1234' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "401" ]; then
  echo "PASS: suffix bypass attempt returns 401"
else
  echo "FAIL: expected 401 (suffix bypass), got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 7: Case-insensitive header (lowercase 'authorization') ==="
HTTP_CODE=$(do_request 'authorization: Bearer test-secret-token-123' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "200" ]; then
  echo "PASS: lowercase authorization header returns 200"
else
  echo "FAIL: expected 200, got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 8: Case-insensitive header (uppercase 'AUTHORIZATION') ==="
HTTP_CODE=$(do_request 'AUTHORIZATION: Bearer test-secret-token-123' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "200" ]; then
  echo "PASS: uppercase AUTHORIZATION header returns 200"
else
  echo "FAIL: expected 200, got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 9: Empty token after Bearer prefix ==="
HTTP_CODE=$(do_request 'Authorization: Bearer ' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "401" ]; then
  echo "PASS: empty token returns 401"
else
  echo "FAIL: expected 401, got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "=== Test 10: Missing Bearer/Token prefix ==="
HTTP_CODE=$(do_request 'Authorization: test-secret-token-123' '{"input":"hello","stream":false}')
if [ "$HTTP_CODE" = "401" ]; then
  echo "PASS: missing Bearer/Token prefix returns 401"
else
  echo "FAIL: expected 401, got $HTTP_CODE"
  cat /tmp/auth_resp_body.txt
  exit 1
fi

echo "All auth tests passed."
