#!/usr/bin/env bash
# Lightweight smoke test: starts a fake native agent (Python) and posts a /v1/responses request
set -euo pipefail
cd "$(dirname "$0")/.."
# Start bridge in background if not running - assume binary is codex_unified_bridge
# For smoke we run a minimal fake native agent that echoes framed requests
PY_SCRIPT=$(cat <<'PY'
import sys,struct
# simple framed protocol: 4-byte big-endian length + JSON
while True:
    hdr = sys.stdin.buffer.read(4)
    if not hdr or len(hdr)<4:
        break
    l = struct.unpack('>I', hdr)[0]
    data = sys.stdin.buffer.read(l).decode('utf-8')
    # reply with a simple JSON frame
    resp = '{"status":"ok","text":"Hello from fake native agent"}'
    out = resp.encode('utf-8')
    sys.stdout.buffer.write(struct.pack('>I', len(out)))
    sys.stdout.buffer.write(out)
    sys.stdout.buffer.flush()
PY
)
python3 -c "$PY_SCRIPT" &
FAKE_PID=$!
sleep 0.2
# Use nc to simulate bridge client POST if nc available, else print instructions
if command -v nc >/dev/null 2>&1; then
    echo "Posting test request to local bridge (you must have started the bridge pointing at fake agent)..."
    printf 'POST /v1/responses HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s' $(printf '%s' '{"input":"hello"}' | wc -c) '{"input":"hello"}' | nc -q 1 127.0.0.1 9050 || true
else
    echo "nc not found. Start bridge and use: curl -v -X POST http://127.0.0.1:9050/v1/responses -d '{\"input\":\"hello\"}'"
fi
kill $FAKE_PID || true
