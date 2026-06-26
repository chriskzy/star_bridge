#!/usr/bin/env bash
# test_event_cap.sh — T1.4: exceeding max_turn_events completes the response with
# partial output + incomplete_details, instead of failing the whole turn.
#
# Drives the fake agent in FAKE_AGENT_DELTA_FLOOD mode (emits N text_delta frames,
# each counting as one turn event) with a low --max-turn-events cap. The bridge must
# stop at the cap and return HTTP 200 response.completed carrying the partial delta
# text and incomplete_details="max_turn_events" — never an error and never the final
# FLOOD_DONE marker (which arrives only after the cap).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19000 + RANDOM % 800))}"
LOG="$ROOT/tests/.out/.event-cap-server.log"
FAKE_AGENT="$ROOT/tests/fake_agent.sh"
CAP=10
FLOOD=50

cd "$ROOT"
mkdir -p "$ROOT/tests/.out"
make >/dev/null
chmod +x "$FAKE_AGENT"

FAKE_AGENT_DELTA_FLOOD="$FLOOD" \
  ./bin/star_bridge "$FAKE_AGENT" . --framed --no-config -p "$PORT" --max-turn-events "$CAP" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: bridge exited before ready"; sed -n '1,40p' "$LOG"; exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then break; fi
  sleep 0.1
done

RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"flood me"}')"

fail=0

if grep -q '"status":"completed"' <<<"$RESP"; then
  echo "PASS: response completed (not failed) despite exceeding event cap"
else
  echo "FAIL: expected status=completed; got: $(head -c 300 <<<"$RESP")"; fail=1
fi

if grep -q '"incomplete_details":"max_turn_events"' <<<"$RESP"; then
  echo "PASS: incomplete_details=max_turn_events surfaced"
else
  echo "FAIL: missing incomplete_details=max_turn_events; got: $(head -c 300 <<<"$RESP")"; fail=1
fi

# Partial deltas must be present; the post-cap FLOOD_DONE marker must NOT be.
if grep -q 'd0 ' <<<"$RESP"; then
  echo "PASS: partial delta output present"
else
  echo "FAIL: partial delta output missing"; fail=1
fi
if grep -q 'FLOOD_DONE' <<<"$RESP"; then
  echo "FAIL: final completion marker leaked — cap did not stop the turn"; fail=1
else
  echo "PASS: post-cap completion marker correctly absent"
fi

if [ "$fail" -ne 0 ]; then echo "Event-cap test FAILED"; exit 1; fi
echo "Event-cap test passed."
