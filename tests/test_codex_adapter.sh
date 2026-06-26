#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

mkdir -p bin
CC_BIN="${CC:-clang}"

# Portable cJSON discovery: use vendored cJSON.c for tests
CFLAGS_VALUE="${CFLAGS:--Wall -Wextra -O2 -std=gnu11 -pthread -I./include -I./vendor/cjson}"

$CC_BIN $CFLAGS_VALUE \
  -o bin/codex_adapter_test tests/codex_adapter_test.c src/codex_request_parser.c src/codex_response_formatter.c src/codex_stream_events.c src/codex_tool_detector.c src/codex_tool_normalizer.c src/responses_api.c src/json_utils.c src/config_manager.c vendor/cjson/cJSON.c

./bin/codex_adapter_test
echo "Codex adapter tests passed."
