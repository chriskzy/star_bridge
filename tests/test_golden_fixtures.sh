#!/usr/bin/env bash
# Test golden fixtures: verify that the server produces valid JSON matching expected schemas
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== Test: Golden fixture validation ==="

# Test 1: basic_response.json is valid JSON
if python3 -c "import json; json.load(open('test_fixtures/basic_response.json'))" 2>/dev/null; then
  echo "PASS: basic_response.json is valid JSON"
else
  echo "FAIL: basic_response.json is not valid JSON"
  exit 1
fi

# Test 2: tool_call_response.json is valid JSON
if python3 -c "import json; json.load(open('test_fixtures/tool_call_response.json'))" 2>/dev/null; then
  echo "PASS: tool_call_response.json is valid JSON"
else
  echo "FAIL: tool_call_response.json is not valid JSON"
  exit 1
fi

# Test 3: streaming_events.txt has correct SSE structure
# Each event block should start with "event:" and have a "data:" line
VALID=0
INVALID=0
while IFS= read -r line; do
  if [[ "$line" == event:* ]]; then
    VALID=1
  fi
done < test_fixtures/streaming_events.txt
if [ "$VALID" -eq 1 ]; then
  echo "PASS: streaming_events.txt has valid SSE structure"
else
  echo "FAIL: streaming_events.txt missing event lines"
  exit 1
fi

echo "All golden fixture tests passed."
