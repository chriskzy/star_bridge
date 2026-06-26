#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

mkdir -p bin
${CC:-clang} ${CFLAGS:--Wall -Wextra -O2 -std=gnu11 -pthread -I./include} \
  -I./vendor/cjson \
  -o bin/tool_config_test tests/tool_config_test.c src/config_manager.c src/json_utils.c vendor/cjson/cJSON.c

./bin/tool_config_test
echo "Tool config tests passed."
