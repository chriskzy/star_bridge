#!/usr/bin/env bash
# Fix5 — SSE must be sent uncompressed even when the client offers gzip.
# A previous build gzip-compressed individual `data:` payloads and advertised
# Content-Encoding: gzip, which is invalid for text/event-stream. Assert neither
# happens: no Content-Encoding: gzip header, and the stream is readable text.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19500 + RANDOM % 9000))}"
LOG="$ROOT/tests/.out/.sse_no_gzip.log"
FAKE_AGENT="$ROOT/tests/fake_agent.sh"
mkdir -p "$ROOT/tests/.out"
cd "$ROOT"
make >/dev/null
chmod +x "$FAKE_AGENT"

# Make the agent emit > 256 bytes so the old gzip path would have triggered.
./bin/star_bridge "$FAKE_AGENT" . --framed --no-config -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT
for _ in {1..50}; do curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break; sleep 0.1; done

BIG="$(printf 'x%.0s' $(seq 1 400))"
HDRS="$ROOT/tests/.out/.sse_no_gzip_hdrs.txt"
BODY="$(curl -sS -D "$HDRS" -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  -H 'Accept-Encoding: gzip' \
  --data "{\"input\":\"$BIG\",\"stream\":true}" --max-time 5)"

fail=0
if grep -iq '^Content-Encoding:.*gzip' "$HDRS"; then
  echo "FAIL: SSE response advertised Content-Encoding: gzip"; fail=1
else
  echo "PASS: no Content-Encoding: gzip on SSE response"
fi
# Body must be readable text SSE with a proper terminal event (not binary gzip).
if grep -q 'response.completed' <<<"$BODY"; then
  echo "PASS: SSE body is readable text (response.completed present)"
else
  echo "FAIL: SSE body not readable / missing response.completed"; fail=1
fi

[ "$fail" -eq 0 ] || { echo "sse no-gzip test FAILED"; exit 1; }
echo "sse no-gzip test passed."
