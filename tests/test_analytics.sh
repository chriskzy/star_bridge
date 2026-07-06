#!/usr/bin/env bash
# Analytics — scripts/analytics.py parses turn_metrics lines and reports
# performance + steering. (1) End-to-end: drive a real turn through the bridge
# and confirm a turn_metrics line is emitted with the steering level. (2) Parser:
# feed a synthetic multi-effort log and assert the aggregates via --json.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p tests/.out
make bin/star_bridge >/dev/null
chmod +x tests/fake_agent.sh

PORT="${PORT:-$((26000 + RANDOM % 9000))}"
DBG="$ROOT/tests/.out/.analytics-debug.log"
CFG="$ROOT/tests/.out/.analytics-cfg.json"
LOG="$ROOT/tests/.out/.analytics-bridge.log"
rm -f "$DBG"
cat >"$CFG" <<EOF
{"use_framed_protocol": true, "trace": true, "debug_log": true, "debug_log_path": "$DBG"}
EOF

./bin/star_bridge tests/fake_agent.sh . -p "$PORT" --config "$CFG" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true; rm -f "$CFG"' EXIT
for _ in {1..50}; do curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1 && break; sleep 0.1; done

curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" -H 'Content-Type: application/json' \
  --data '{"input":"analytics turn","reasoning_effort":"high"}' >/dev/null 2>&1
sleep 0.3

fail=0
if grep -q 'turn_metrics request=.* status=completed effort=high' "$DBG"; then
  echo "PASS: turn_metrics line emitted with steering level (effort=high)"
else
  echo "FAIL: no turn_metrics line with effort=high"; grep turn_metrics "$DBG" || true; fail=1
fi

# Parser correctness against a known synthetic log.
SYN="$ROOT/tests/.out/.analytics-synth.log"
cat >"$SYN" <<'EOF'
turn_metrics request=1 status=completed effort=low duration_ms=1000 output_bytes=400 prompt_tokens=100 completion_tokens=200 tool_calls=0
turn_metrics request=2 status=completed effort=high duration_ms=4000 output_bytes=2000 prompt_tokens=200 completion_tokens=2000 tool_calls=2
turn_metrics request=3 status=timeout effort=high duration_ms=60000 output_bytes=0 prompt_tokens=0 completion_tokens=0 tool_calls=0
EOF
JSON="$(python3 scripts/analytics.py "$SYN" --json)"

check() { # key expected
  local got
  got="$(python3 -c "import sys,json; print(json.load(sys.stdin).get('$1'))" <<<"$JSON")"
  if [ "$got" = "$2" ]; then echo "PASS: $1 == $2"; else echo "FAIL: $1 expected $2 got $got"; fail=1; fi
}
check turns_total 3
check completed 2
check total_tool_calls 2
check steering_active True
check tokens_are_estimated False
# low: 200 tok / 1s = 200 tk/s ; high completed: 2000 tok / 4s = 500 tk/s ; overall avg = 350
check avg_tokens_per_s 350.0

[ "$fail" -eq 0 ] || { echo "analytics test FAILED"; exit 1; }
echo "analytics test passed."
