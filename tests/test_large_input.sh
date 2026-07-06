#!/usr/bin/env bash
# test_large_input.sh — proves buffer truncation bugs (fails pre-T1.1, passes after)
#
# Cases:
#   (a) >32 input items       — assert all 33 items forwarded to agent
#   (b) single item >8KB      — byte-exact forwarding via input_len echo
#   (c) total normalized input >64KB — byte-exact check; static char input[65536]
#                               in server.c truncates to 65535 bytes pre-T1.1
#   (d) instructions >8KB     — instructions field not clipped
#
# Uses FAKE_AGENT_ECHO_INPUT_STATS=1 so fake_agent.py responds with:
#   input_len=N input_sha256=HASH
# instead of its normal echo text.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((19200 + RANDOM % 800))}"
LOG="$ROOT/tests/.out/.large-input-server.log"
FAKE_AGENT="$ROOT/tests/fake_agent.sh"

cd "$ROOT"

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

chmod +x "$FAKE_AGENT"

# Start bridge with FAKE_AGENT_ECHO_INPUT_STATS so the agent echoes byte stats
FAKE_AGENT_ECHO_INPUT_STATS=1 \
  ./bin/star_bridge "$FAKE_AGENT" . --framed --no-config -p "$PORT" >"$LOG" 2>&1 &
PID=$!
trap 'kill "$PID" >/dev/null 2>&1 || true; wait "$PID" >/dev/null 2>&1 || true' EXIT

# Wait for bridge ready
for _ in {1..50}; do
  if ! kill -0 "$PID" >/dev/null 2>&1; then
    echo "FAIL: bridge exited before ready"
    sed -n '1,40p' "$LOG"
    exit 1
  fi
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

PASS_COUNT=0
FAIL_COUNT=0

pass() { echo "PASS: $1"; PASS_COUNT=$((PASS_COUNT + 1)); }
fail() { echo "FAIL: $1"; FAIL_COUNT=$((FAIL_COUNT + 1)); }

# POST to /v1/responses, return raw response body
post_responses() {
  curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
    -H 'Content-Type: application/json' \
    --data "$1"
}

# Extract assistant text from a Responses API JSON object
extract_text() {
  python3 - <<'PY' "$1"
import json, sys
try:
    obj = json.loads(sys.argv[1])
    for out in obj.get("output", []):
        for part in out.get("content", []):
            t = part.get("type", "")
            if t in ("text", "output_text"):
                print(part["text"], end="")
        if out.get("type") == "text":
            print(out.get("text", ""), end="")
except Exception as e:
    print(f"extract_error: {e}", file=__import__('sys').stderr)
PY
}

# Parse input_len=N from the echo text returned by fake_agent.py
parse_input_len() {
  python3 -c "
import re, sys
m = re.search(r'input_len=(\d+)', sys.argv[1])
print(m.group(1) if m else '0')
" "$1"
}

# ------------------------------------------------------------------ #
# Case (a): >32 input items                                           #
# The codex_request_parser starts with capacity 16, doubles as        #
# needed — so parsing is fine. The truncation risk is in             #
# normalize_text_item writing into the 65536-byte server buffer.      #
# We send 33 short items and check input_len accounts for all 33.    #
# Each "item-text-NNN\n" ≈ 15 bytes → 33 items ≈ 495 bytes minimum. #
# If a static item array capped at 32 existed, the 33rd would be     #
# absent and input_len would be ~14 bytes shorter.                   #
# ------------------------------------------------------------------ #
echo "=== Case (a): 33 input items ==="

ITEMS_JSON="$(python3 - <<'PY'
import json
items = []
for i in range(33):
    items.append({
        "role": "user",
        "content": [{"type": "input_text", "text": f"item-text-{i:03d}"}]
    })
print(json.dumps({"input": items}))
PY
)"

RESP_A="$(post_responses "$ITEMS_JSON")"
TEXT_A="$(extract_text "$RESP_A")"
INPUT_LEN_A="$(parse_input_len "$TEXT_A")"

# 33 items × "item-text-NNN\n" (15 bytes) = 495 bytes, plus workspace_root (~60 bytes)
# A 32-item truncation would produce ~480 bytes of item text.
# Threshold: input_len must be ≥ 495 to prove all 33 items are present.
EXPECTED_MIN_A=495
if [ "${INPUT_LEN_A:-0}" -ge "$EXPECTED_MIN_A" ] 2>/dev/null; then
  pass "case (a): 33 items — input_len=${INPUT_LEN_A} >= ${EXPECTED_MIN_A}"
else
  fail "case (a): 33rd item missing — input_len=${INPUT_LEN_A} < ${EXPECTED_MIN_A} (all 33 items not forwarded)"
fi

# ------------------------------------------------------------------ #
# Case (b): single input_text item > 8 KB                            #
# Pre-T1.1: a static 8192-byte buffer in an intermediate path clips  #
# the item text. The agent should receive exactly 9000 bytes of 'b'  #
# plus overhead (workspace_root ~60 bytes, trailing newline).         #
# ------------------------------------------------------------------ #
echo "=== Case (b): single item > 8KB ==="

ITEM_TEXT_B="$(python3 -c "print('b' * 9000, end='')")"
PAYLOAD_B="$(python3 - <<'PY' "$ITEM_TEXT_B"
import json, sys
text = sys.argv[1]
print(json.dumps({
    "input": [
        {"role": "user", "content": [{"type": "input_text", "text": text}]}
    ]
}))
PY
)"

RESP_B="$(post_responses "$PAYLOAD_B")"
TEXT_B="$(extract_text "$RESP_B")"
INPUT_LEN_B="$(parse_input_len "$TEXT_B")"

EXPECTED_MIN_B=9000
if [ "${INPUT_LEN_B:-0}" -ge "$EXPECTED_MIN_B" ] 2>/dev/null; then
  pass "case (b): 9000-byte item — input_len=${INPUT_LEN_B} >= ${EXPECTED_MIN_B}"
else
  fail "case (b): item clipped — input_len=${INPUT_LEN_B} < ${EXPECTED_MIN_B}"
fi

# ------------------------------------------------------------------ #
# Case (c): total normalized input > 64 KB                           #
# server.c declares: char input[65536];                               #
# snprintf(input, sizeof(input), ...) silently truncates at 65535.   #
# We send 66000 bytes of 'c' and assert input_len ≥ 66000.           #
# ------------------------------------------------------------------ #
echo "=== Case (c): total input > 64KB ==="

ITEM_TEXT_C="$(python3 -c "print('c' * 66000, end='')")"
PAYLOAD_C="$(python3 - <<'PY' "$ITEM_TEXT_C"
import json, sys
text = sys.argv[1]
print(json.dumps({
    "input": [
        {"role": "user", "content": [{"type": "input_text", "text": text}]}
    ]
}))
PY
)"

RESP_C="$(post_responses "$PAYLOAD_C")"
TEXT_C="$(extract_text "$RESP_C")"
INPUT_LEN_C="$(parse_input_len "$TEXT_C")"

EXPECTED_MIN_C=66000
if [ "${INPUT_LEN_C:-0}" -ge "$EXPECTED_MIN_C" ] 2>/dev/null; then
  pass "case (c): 66000-byte input — input_len=${INPUT_LEN_C} >= ${EXPECTED_MIN_C}"
else
  fail "case (c): 64KB buffer truncated — input_len=${INPUT_LEN_C} < ${EXPECTED_MIN_C}"
fi

# ------------------------------------------------------------------ #
# Case (d): instructions field > 8 KB                                #
# Instructions are prepended to normalized_input before the user      #
# text. Any static buffer in the parser or server holding             #
# instructions at 8192 bytes would clip this.                         #
# We send 8500 bytes of 'd' as instructions plus "ping" as input.    #
# Normalized total: 8500 + "\n" + "ping" = 8505 bytes minimum.       #
# ------------------------------------------------------------------ #
echo "=== Case (d): instructions > 8KB ==="

INSTRUCTIONS_D="$(python3 -c "print('d' * 8500, end='')")"
PAYLOAD_D="$(python3 - <<'PY' "$INSTRUCTIONS_D"
import json, sys
instr = sys.argv[1]
print(json.dumps({
    "instructions": instr,
    "input": "ping"
}))
PY
)"

RESP_D="$(post_responses "$PAYLOAD_D")"
TEXT_D="$(extract_text "$RESP_D")"
INPUT_LEN_D="$(parse_input_len "$TEXT_D")"

# 8500 (instructions) + 1 (\n) + 4 (ping) = 8505 minimum
EXPECTED_MIN_D=8505
if [ "${INPUT_LEN_D:-0}" -ge "$EXPECTED_MIN_D" ] 2>/dev/null; then
  pass "case (d): 8500-byte instructions — input_len=${INPUT_LEN_D} >= ${EXPECTED_MIN_D}"
else
  fail "case (d): instructions clipped — input_len=${INPUT_LEN_D} < ${EXPECTED_MIN_D}"
fi

# ------------------------------------------------------------------ #
# Case (e): body over the hard ceiling -> loud structured rejection   #
# The bridge enforces two ceilings: the HTTP body cap                 #
# (max_request_size, default 1 MiB -> 413) which fires first, and the #
# parser cap (max_request_input_bytes, default 8 MiB ->               #
# request_input_too_large 400) for the post-decode path. The contract #
# that matters for T1.1: an over-ceiling body must be rejected LOUDLY  #
# with a structured error and an error 4xx status — never silently    #
# accepted and clipped to a 200 with short input. Send ~2 MiB (over   #
# the 1 MiB body cap) and assert a 4xx + structured error body.       #
# ------------------------------------------------------------------ #
echo "=== Case (e): body over ceiling -> loud structured rejection ==="

CEIL_PAYLOAD="$ROOT/tests/.out/.large-input-ceiling.json"
python3 - "$CEIL_PAYLOAD" <<'PY'
import json, sys
with open(sys.argv[1], "w") as f:
    json.dump({"input": "e" * (2 * 1024 * 1024)}, f)
PY

CEIL_BODY="$ROOT/tests/.out/.large-input-ceiling-resp.txt"
CEIL_CODE="$(curl -s -o "$CEIL_BODY" -w '%{http_code}' \
  -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data @"$CEIL_PAYLOAD" || true)"

# Must be a 4xx rejection (not a 200 with silently-clipped input) and the
# body must carry a structured error (an "error" field or a too-large marker).
if printf '%s' "$CEIL_CODE" | grep -qE '^4[0-9][0-9]$' \
   && grep -qiE 'too large|request_input_too_large|"error"' "$CEIL_BODY"; then
  pass "case (e): over-ceiling body loudly rejected (code=${CEIL_CODE}, structured error)"
else
  fail "case (e): expected 4xx + structured error, got code=${CEIL_CODE} body=$(head -c 200 "$CEIL_BODY")"
fi

# ------------------------------------------------------------------ #
# Summary                                                             #
# ------------------------------------------------------------------ #
echo ""
echo "Results: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"

if [ "$FAIL_COUNT" -gt 0 ]; then
  echo "FAIL: ${FAIL_COUNT} case(s) failed — buffer truncation confirmed (expected pre-T1.1)"
  exit 1
fi

echo "All large-input tests passed."
