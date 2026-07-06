#!/bin/bash
#
# Test: Fake UDS handshake scenarios
# Tests full handshake lifecycle over stdio_framed.
#
# Tests:
# 1. Success handshake (hello + ready = bridge ready)
# 2. Protocol mismatch (wrong protocol version)
# 3. No ready frame (timeout)
# 4. Model loading timeout (model_loaded=false)
# 5. Native busy (ack with status=busy)
# 6. Request id mismatch (response with wrong id)
# 7. Clean shutdown (shutdown -> shutdown_ack)
#

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
make clean && make 2>/dev/null
mkdir -p "$ROOT/tests/.out"

echo "=== Handshake tests ==="

if [ ! -x tests/test_uds_handshake ]; then
    echo "Building test_uds_handshake..."
    cc -Wall -Wextra -O2 -std=gnu11 -pthread -I./include -I./vendor/cjson \
        -o tests/test_uds_handshake tests/test_uds_handshake.c \
        src/bridge_core.c src/capability_router.c src/codex_request_parser.c \
        src/codex_response_formatter.c src/codex_stream_events.c src/codex_tool_detector.c \
        src/codex_tool_normalizer.c src/config_manager.c src/debug_trace.c \
        src/file_monitor_expanded.c src/json_builder.c src/json_log.c src/json_utils.c \
        src/native_frame.c src/native_connection.c src/native_response.c src/responses_api.c \
        src/responses_stream_state.c src/ring_buffer.c src/server.c src/tool_history.c \
        src/tool_policy.c src/tool_runner.c src/turn_context.c src/uds_transport.c \
        vendor/cjson/cJSON.c 2>/dev/null -lz
fi

if [ ! -x tests/test_uds_handshake ]; then
    echo "ERROR: could not build test binary"
    exit 1
fi

./tests/test_uds_handshake
status=$?
if [ $status -eq 0 ]; then
    echo "All handshake tests passed."
    exit 0
else
    echo "Some handshake tests FAILED."
    exit 1
fi
