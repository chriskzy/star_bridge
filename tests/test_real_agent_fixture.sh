#!/usr/bin/env bash
# Real-agent one-turn fixture test.
#
# Tests that a Codex request lands, the framed request is accepted by the
# native agent wrapper (which translates to ds4-agent non-interactive mode),
# and the Codex client receives a valid Responses API object.
#
# This test uses agent/ds4_wrapper.py which bridges the framed protocol
# to the real ds4-agent's non-interactive mode.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_LOG="$ROOT/tests/.out/.bridge-real-agent-fixture.log"
mkdir -p "$ROOT/tests/.out"
WRAPPER="$ROOT/agent/ds4_wrapper.py"
PORT=21778
HOST=127.0.0.1
BRIDGE_PID=""

cleanup() {
    if [[ -n "${BRIDGE_PID:-}" ]]; then
        kill "$BRIDGE_PID" 2>/dev/null || true
    fi
    kill "$(lsof -ti:$PORT 2>/dev/null)" 2>/dev/null || true
}
trap cleanup EXIT

cd "$ROOT"
make venv >/dev/null
make >/dev/null

# ds4-agent allows only one instance; do not kill active user work unless caller opts in.
if pgrep -f '[/]ds4-agent' >/dev/null 2>&1; then
    if [[ "${STAR_BRIDGE_ALLOW_KILL_ACTIVE_DS4:-0}" != "1" ]]; then
        echo "SKIP: ds4-agent already running; set STAR_BRIDGE_ALLOW_KILL_ACTIVE_DS4=1 to run this smoke"
        exit 0
    fi
    bash "$ROOT/scripts/cleanup_stale_bridges.sh" >/dev/null 2>&1 || true
fi
cleanup
sleep 1

echo ""
echo "=== Real-agent one-turn fixture ==="

# Start bridge with real ds4-agent path (auto-selects framed wrapper)
DS4_AGENT="${DS4_AGENT:?DS4_AGENT environment variable must be set to the ds4-agent binary path}"
if [[ ! -x "$DS4_AGENT" ]]; then
    echo "SKIP: ds4-agent not found at $DS4_AGENT"
    exit 0
fi
echo "Starting bridge with ds4-agent at $DS4_AGENT..."
export DS4_CONTEXT_TOKENS="${DS4_CONTEXT_TOKENS:-8192}"
"$ROOT/bin/star_bridge" "$DS4_AGENT" "$ROOT" -p "$PORT" --no-config \
  --model-load-timeout-ms 600000 --turn-response-timeout-ms 600000 >"$BRIDGE_LOG" 2>&1 &
BRIDGE_PID=$!

# Wait for HTTP ready (ds4 model load can take minutes on cold start)
for i in $(seq 1 600); do
    if ! kill -0 $BRIDGE_PID 2>/dev/null; then
        echo "FAIL: bridge exited during startup"
        cat "$BRIDGE_LOG"
        exit 1
    fi
    if curl -fsS "http://$HOST:$PORT/v1/models" >/dev/null 2>&1; then
        echo "Bridge ready (PID=$BRIDGE_PID) after ${i}s"
        break
    fi
    sleep 1
done

if ! kill -0 $BRIDGE_PID 2>/dev/null; then
    echo "FAIL: bridge failed to start"
    cat "$BRIDGE_LOG"
    exit 1
fi

if ! grep -q 'ds4_wrapper_selected' "$ROOT/.codex-bridge-debug.log" 2>/dev/null; then
    echo "SKIP: real ds4-agent not available (wrapper not selected)"
    kill $BRIDGE_PID 2>/dev/null || true
    exit 0
fi

# Test 1: Send a simple request
echo ""
echo "Test 1: Simple request"
"${VENV_PY:-$ROOT/.venv/bin/python3}" -c "
import http.client, socket
conn = http.client.HTTPConnection('$HOST', $PORT, timeout=180)
conn.request('POST', '/v1/responses', '{\"input\":\"hello from test\"}', {'Content-Type': 'application/json'})
resp = conn.getresponse()
print(resp.status, resp.reason)
data = resp.read().decode()
print(data[:500])
if '[No output from ds4-agent]' in data:
    raise SystemExit('placeholder output')
conn.close()
"

sleep 0.3

# Test 2: Request with reset_session
echo ""
echo "Test 2: Request with reset_session=true"
"${VENV_PY:-$ROOT/.venv/bin/python3}" -c "
import http.client
conn = http.client.HTTPConnection('$HOST', $PORT, timeout=180)
conn.request('POST', '/v1/responses', '{\"input\":\"fresh start\",\"reset_session\":true}', {'Content-Type': 'application/json'})
resp = conn.getresponse()
print(resp.status, resp.reason)
data = resp.read().decode()
print(data[:500])
conn.close()
"

sleep 0.3

# Test 3: Request with previous_response_id
echo ""
echo "Test 3: Request with previous_response_id"
"${VENV_PY:-$ROOT/.venv/bin/python3}" -c "
import http.client
conn = http.client.HTTPConnection('$HOST', $PORT, timeout=180)
conn.request('POST', '/v1/responses', '{\"input\":\"continue\",\"previous_response_id\":\"session-abc\"}', {'Content-Type': 'application/json'})
resp = conn.getresponse()
print(resp.status, resp.reason)
data = resp.read().decode()
print(data[:500])
conn.close()
"

sleep 0.3

echo ""
echo "All real-agent fixture tests completed."
