CC ?= clang
CFLAGS ?= -Wall -Wextra -O2 -std=gnu11 -pthread -I./include
DEBUG_CFLAGS ?= -Wall -Wextra -g -O0 -std=gnu11 -pthread -I./include -DDEBUG
TARGET := bin/star_bridge
COMPAT_TARGET := bin/codex_bridge
DEBUG_TARGET := bin/star_bridge_debug
SRC := src/main.c src/bridge_core.c src/capability_router.c src/codex_request_parser.c src/codex_response_formatter.c src/codex_stream_events.c src/codex_tool_detector.c src/codex_tool_normalizer.c src/config_manager.c src/debug_trace.c src/file_monitor_expanded.c src/json_builder.c src/json_log.c src/json_utils.c src/native_frame.c src/native_connection.c src/native_response.c src/responses_api.c src/responses_stream_state.c src/ring_buffer.c src/server.c src/tool_history.c src/tool_policy.c src/tool_runner.c src/turn_context.c src/uds_transport.c
VENDOR_CJSON := vendor/cjson/cJSON.c

.PHONY: all clean test debug venv test-runner test-config-validation test-usage-normalization test-codex-shim-regression test-native-response-parser sanitize

.DEFAULT_GOAL := all

PYTHON314 ?= python3.14
VENV_PY := .venv/bin/python3

venv:
	bash scripts/setup_python_venv.sh

# Source files excluding main.c (for test binaries that have their own main())
TEST_SRC := $(filter-out src/main.c, $(SRC))

# --------------------------------------------------------------------------
# Portable cJSON discovery
# --------------------------------------------------------------------------
# Strategy:
#   1. If CJSON_CFLAGS and CJSON_LIBS are set, use them (user override).
#   2. If pkg-config is available and reports cjson, use it.
#   3. Fall back to vendored cJSON in vendor/cjson/.
#
# Example override:
#   make CJSON_CFLAGS="-I/opt/homebrew/include" CJSON_LIBS="-L/opt/homebrew/lib -lcjson"

ifneq ($(CJSON_CFLAGS)$(CJSON_LIBS),)
  # User-provided override — use system cJSON, skip vendored
  CJSON_INC := $(CJSON_CFLAGS)
  CJSON_LINK := $(CJSON_LIBS)
  SRC_FINAL := $(SRC)
else ifeq ($(shell pkg-config --exists cjson 2>/dev/null && echo yes),yes)
  CJSON_INC := $(shell pkg-config --cflags cjson)
  CJSON_LINK := $(shell pkg-config --libs cjson)
  SRC_FINAL := $(SRC)
else
  # Vendored fallback — compile cJSON.c along with the rest
  CJSON_INC := -I./vendor/cjson
  CJSON_LINK :=
  SRC_FINAL := $(SRC) $(VENDOR_CJSON)
endif

CFLAGS += $(CJSON_INC)
DEBUG_CFLAGS += $(CJSON_INC)
LDFLAGS := $(CJSON_LINK) -lz

all: venv $(TARGET)

$(TARGET): $(SRC_FINAL)
	mkdir -p bin
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC_FINAL) $(LDFLAGS)
	ln -sf star_bridge $(COMPAT_TARGET)

debug: $(DEBUG_TARGET)

$(DEBUG_TARGET): $(SRC_FINAL)
	mkdir -p bin
	$(CC) $(DEBUG_CFLAGS) -o $(DEBUG_TARGET) $(SRC_FINAL) $(LDFLAGS)

test: venv $(TARGET) tests/test_codex_shim_regression bin/test_capability_routing tests/test_proxy_bypass.sh
	mkdir -p tests/.out
	bash tests/test_star_bridge_binary.sh
	bash tests/test_codex_adapter.sh
	bash tests/test_generate_config.sh
	bash tests/smoke.sh
	bash tests/test_fake_agent.sh
	bash tests/test_fake_uds_agent.sh
	bash tests/test_uds_connect_existing.sh
	bash tests/test_uds_launch_and_connect.sh
	bash tests/test_uds_wrong_path.sh
	bash tests/test_uds_stale_cleanup.sh
	bash tests/test_uds_unsafe_dir.sh
	bash tests/test_uds_frame_too_large.sh
	bash tests/test_uds_partial_frame.sh
	bash tests/test_uds_reconnect.sh
	bash tests/test_uds_wrong_agent_regression.sh
	bash tests/test_uds_hello_frame.sh
	bash tests/test_uds_native_hello.sh
	bash tests/test_uds_fail_closed.sh
	bash tests/test_uds_handshake.sh
	bash tests/test_uds_transport_runtime.sh
	bash tests/test_phase2_resume.sh
	bash tests/test_phase2_spawn_config.sh
	bash tests/test_phase2_lifecycle.sh
	bash tests/test_phase3_harness_config.sh
	bash tests/test_phase4_tool_intent.sh
	bash tests/test_phase4_tool_edges.sh
	bash tests/test_phase4_tool_replay_trunc.sh
	bash tests/test_tool_policy.sh
	bash tests/test_tool_runner.sh
	bash tests/test_tool_config.sh
	bash tests/test_logging.sh
	bash tests/test_chunked_request.sh
	bash tests/test_request_diagnostics.sh
	bash tests/test_debug_pipeline.sh
	bash tests/test_bridge_native_connection.sh
	bash tests/test_auto_framed_response.sh
	$(VENV_PY) tests/test_ds4_wrapper.py
	bash tests/test_python_venv.sh
	bash tests/test_ds4_wrapper_integration.sh
	bash tests/test_ds4_persistent_wrapper.sh
	bash tests/test_ds4_preflight.sh
	bash tests/test_port_in_use.sh
	bash tests/test_cleanup_stale_bridges.sh
	bash tests/test_client_disconnect.sh
	bash tests/test_streaming_text.sh
	bash tests/test_streaming_tool_call.sh
	bash tests/test_golden_fixtures.sh
	bash tests/test_sse_events.sh
	bash tests/test_structured_errors.sh
	bash tests/test_backpressure.sh
	bash tests/test_request_validation.sh
	bash tests/test_response_validation.sh
	bash tests/test_auth.sh
	bash tests/test_tool_use.sh
	bash tests/test_streaming_tool_calls.sh
	bash tests/test_response_endpoint.sh
	bash tests/test_response_endpoint_errors.sh
	bash tests/test_http_request_limits.sh
	bash tests/test_large_input.sh
	bash tests/test_event_cap.sh
	bash tests/test_stream_lifecycle_golden.sh
	bash tests/test_sse_no_gzip.sh
	bash tests/test_turn_context.sh
	bash tests/test_output_truncation.sh
	bash tests/test_models_during_turn.sh
	bash tests/test_cancel_mid_turn.sh
	bash tests/test_doctor.sh
	bash tests/test_error_after_ack.sh
	bash tests/test_phase2_fault_injection.sh
	bash tests/test_second_harness_openai_sdk.sh
	bash tests/test_readme_flags.sh
	bash tests/test_analytics.sh
	bash tests/test_session_interleave.sh
	./tests/test_codex_shim_regression
	$(MAKE) bin/test_capability_routing
	./bin/test_capability_routing
	bash tests/test_proxy_bypass.sh
	bash tests/test_managed_config.sh

# Build standalone test binary for config validation
tests/config_validation_test: tests/config_validation_test.c $(TEST_SRC) $(VENDOR_CJSON)
	mkdir -p bin
	$(CC) $(CFLAGS) -o tests/config_validation_test tests/config_validation_test.c $(TEST_SRC) $(VENDOR_CJSON) $(LDFLAGS)

test-config-validation: tests/config_validation_test
	./tests/config_validation_test

# Build standalone test binary for usage normalization
tests/usage_normalization_test: tests/usage_normalization_test.c $(TEST_SRC) $(VENDOR_CJSON)
	mkdir -p bin
	$(CC) $(CFLAGS) -o tests/usage_normalization_test tests/usage_normalization_test.c $(TEST_SRC) $(VENDOR_CJSON) $(LDFLAGS)

test-usage-normalization: tests/usage_normalization_test
	./tests/usage_normalization_test

# Build standalone test binary for native response parser
tests/test_native_response_parser: tests/test_native_response_parser.c src/native_response.c src/json_utils.c $(VENDOR_CJSON)
	mkdir -p bin
	$(CC) $(CFLAGS) -o tests/test_native_response_parser tests/test_native_response_parser.c src/native_response.c src/json_utils.c $(VENDOR_CJSON) $(LDFLAGS)

test-native-response-parser: tests/test_native_response_parser
	./tests/test_native_response_parser

# Build standalone test binary for codex-shim regression tests
tests/test_codex_shim_regression: tests/test_codex_shim_regression.c $(TEST_SRC) $(VENDOR_CJSON)
	mkdir -p bin
	$(CC) $(CFLAGS) -o tests/test_codex_shim_regression tests/test_codex_shim_regression.c $(TEST_SRC) $(VENDOR_CJSON) $(LDFLAGS)

test-codex-shim-regression: tests/test_codex_shim_regression
	./tests/test_codex_shim_regression

# CI target — build and run a curated set of fast regression tests
ci: sanitize venv $(TARGET) tests/config_validation_test tests/test_codex_shim_regression bin/test_capability_routing tests/test_native_response_parser
	mkdir -p tests/.out
	bash tests/test_star_bridge_binary.sh
	bash tests/test_auth.sh
	bash tests/test_generate_config.sh
	bash tests/smoke.sh
	bash tests/test_fake_agent.sh
	bash tests/test_fake_uds_agent.sh
	bash tests/test_uds_hello_frame.sh
	bash tests/test_uds_native_hello.sh
	bash tests/test_uds_launch_and_connect.sh
	bash tests/test_uds_partial_frame.sh
	bash tests/test_uds_frame_too_large.sh
	bash tests/test_uds_handshake.sh
	bash tests/test_phase2_resume.sh
	bash tests/test_phase2_spawn_config.sh
	bash tests/test_phase2_lifecycle.sh
	bash tests/test_phase3_harness_config.sh
	bash tests/test_phase4_tool_intent.sh
	bash tests/test_tool_policy.sh
	bash tests/test_tool_runner.sh
	bash tests/test_logging.sh
	bash tests/test_chunked_request.sh
	bash tests/test_request_diagnostics.sh
	bash tests/test_debug_pipeline.sh
	bash tests/test_bridge_native_connection.sh
	bash tests/test_auto_framed_response.sh
	bash tests/test_python_venv.sh
	bash tests/test_ds4_preflight.sh
	bash tests/test_port_in_use.sh
	bash tests/test_cleanup_stale_bridges.sh
	bash tests/test_client_disconnect.sh
	bash tests/test_streaming_text.sh
	bash tests/test_sse_events.sh
	bash tests/test_structured_errors.sh
	bash tests/test_backpressure.sh
	bash tests/test_request_validation.sh
	bash tests/test_response_validation.sh
	bash tests/test_tool_use.sh
	bash tests/test_response_endpoint.sh
	bash tests/test_response_endpoint_errors.sh
	bash tests/test_http_request_limits.sh
	bash tests/test_large_input.sh
	bash tests/test_event_cap.sh
	bash tests/test_stream_lifecycle_golden.sh
	bash tests/test_sse_no_gzip.sh
	bash tests/test_turn_context.sh
	bash tests/test_output_truncation.sh
	bash tests/test_models_during_turn.sh
	bash tests/test_cancel_mid_turn.sh
	bash tests/test_doctor.sh
	bash tests/test_error_after_ack.sh
	bash tests/test_readme_flags.sh
	bash tests/test_analytics.sh
	bash tests/test_session_interleave.sh
	./tests/test_codex_shim_regression
	$(MAKE) bin/test_capability_routing
	./bin/test_capability_routing
	bash tests/test_release_copy.sh
	bash tests/test_hermeticity_poisoned_config.sh
	./tests/test_native_response_parser
	./tests/config_validation_test
	bash tests/test_managed_config.sh

test-runner: $(TARGET)
	cc -Wall -Wextra -O2 -std=gnu11 -pthread -I./include -I./vendor/cjson -I./vendor/unity -o tests/test_runner tests/test_runner.c vendor/unity/unity.c $(filter-out src/main.c, $(SRC)) $(VENDOR_CJSON) $(LDFLAGS)
	./tests/test_runner

bin/test_capability_routing: tests/test_capability_routing.c src/capability_router.c src/codex_tool_normalizer.c src/json_utils.c $(VENDOR_CJSON)
	mkdir -p bin
	$(CC) $(CFLAGS) -o bin/test_capability_routing tests/test_capability_routing.c src/capability_router.c src/codex_tool_normalizer.c src/json_utils.c $(VENDOR_CJSON) $(LDFLAGS)

clean:
	rm -rf bin tests/.out
	rm -f .*.log .*_test_dir* .*_test.sock .*_test_*.*

sanitize:
	bash scripts/sanitize_gate.sh
