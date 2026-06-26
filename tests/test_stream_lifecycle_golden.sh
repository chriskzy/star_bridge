#!/usr/bin/env bash
# T0.5: Golden stream-lifecycle test — end-to-end SSE test pinning full event order against a fixture.
# Uses fake agent (/bin/cat) to produce deterministic output. Compares actual SSE event sequence
# against golden fixture in test_fixtures/stream_lifecycle_golden.json. Also tests error variant
# against test_fixtures/stream_lifecycle_error.json.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19050 + RANDOM % 10000))}"
LOG="$ROOT/tests/.out/.stream_lifecycle_golden.log"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

echo "=== T0.5: Golden stream-lifecycle test ==="

# --- Test 1: Normal streaming lifecycle ---
echo "--- Test 1: Normal streaming lifecycle ---"
./bin/star_bridge /bin/cat . --native-transport stdio --no-config -p "$PORT" >"$LOG" 2>&1 &
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

BODY='{"input":"Hello world","stream":true}'
RESP="$(curl -s -N -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$BODY" \
  --max-time 5 2>/dev/null || true)"

# Parse events and data into JSON array
EVENTS_JSON="$(echo "$RESP" | python3 -c "
import sys, json

lines = sys.stdin.read().splitlines()
events = []
i = 0
while i < len(lines):
    if lines[i].startswith('event: '):
        name = lines[i][len('event: '):]
        if i + 1 < len(lines) and lines[i + 1].startswith('data: '):
            data = lines[i + 1][len('data: '):]
            parsed = json.loads(data)
            events.append({'name': name, 'data': parsed})
        i += 2
    else:
        i += 1
print(json.dumps(events, indent=2))
" 2>/dev/null || echo '[]')"

# Load golden fixture
GOLDEN="$(cat "$ROOT/test_fixtures/stream_lifecycle_golden.json")"

# Compare event names in order
NAMES="$(echo "$EVENTS_JSON" | python3 -c "
import sys, json
events = json.loads(sys.stdin.read())
print(' '.join(e['name'] for e in events))
")"
GOLDEN_NAMES="$(echo "$GOLDEN" | python3 -c "
import sys, json
events = json.loads(sys.stdin.read())
print(' '.join(e['name'] for e in events))
")"

if [ "$NAMES" != "$GOLDEN_NAMES" ]; then
  echo "FAIL: event order mismatch"
  echo "  Expected: $GOLDEN_NAMES"
  echo "  Got:      $NAMES"
  exit 1
fi
echo "PASS: event order matches golden fixture"

# Validate sequence number monotonicity
SEQ_OK="$(echo "$EVENTS_JSON" | python3 -c "
import sys, json
events = json.loads(sys.stdin.read())
seqs = [e['data']['sequence_number'] for e in events if 'sequence_number' in e.get('data', {})]
for i in range(1, len(seqs)):
    if seqs[i] != seqs[i-1] + 1:
        print(f'FAIL: non-monotonic sequence: {seqs[i-1]} -> {seqs[i]}')
        sys.exit(1)
print('OK')
" 2>/dev/null || echo 'FAIL')"
if [ "$SEQ_OK" != "OK" ]; then
  echo "FAIL: sequence numbers not monotonic"
  exit 1
fi
echo "PASS: sequence numbers monotonic"

# --- Test 2: Error variant ---
# A validation failure (empty input) returns a NON-streamed HTTP 400 with a
# {"object":"response","status":"failed","error":{...}} body. We pin that contract
# strictly here. The streamed SSE error path (response.created -> response.failed)
# does not exist yet — it is added by T1.3 (mid-turn cancel) / T1.4 (event cap).
# When that lands, extend this test with the streamed sequence and the SSE fixture.
echo "--- Test 2: Error variant (non-streamed 400 contract) ---"
ERROR_BODY='{"input":"","stream":true}'
ERROR_OUT="$(curl -s -o "$ROOT/tests/.out/.stream_error_body" \
  -w '%{http_code}' \
  -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data "$ERROR_BODY" \
  --max-time 5 2>/dev/null || true)"
ERROR_HTTP="$ERROR_OUT"
ERROR_RESP="$(cat "$ROOT/tests/.out/.stream_error_body" 2>/dev/null || echo '')"

# Expected HTTP status comes from the fixture (single source of truth).
EXPECT_HTTP="$(python3 -c "
import json
f = json.load(open('$ROOT/test_fixtures/stream_lifecycle_error.json'))
print(f['http_status'])
")"
if [ "$ERROR_HTTP" != "$EXPECT_HTTP" ]; then
  echo "FAIL: error HTTP status mismatch"
  echo "  Expected: $EXPECT_HTTP"
  echo "  Got:      $ERROR_HTTP"
  exit 1
fi
echo "PASS: error returns HTTP $EXPECT_HTTP"

# Body must declare a failed response with an error object (matches fixture body shape).
echo "$ERROR_RESP" | python3 -c "
import sys, json
body = json.load(sys.stdin)
assert body.get('object') == 'response', f\"object != response: {body.get('object')}\"
assert body.get('status') == 'failed', f\"status != failed: {body.get('status')}\"
assert 'error' in body, 'no error object in failed response'
print('PASS: error body declares status=failed with an error object')
" || { echo 'FAIL: error body does not match failed-response contract'; exit 1; }

echo "All golden stream-lifecycle tests passed."
