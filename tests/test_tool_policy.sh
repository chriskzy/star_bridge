#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

mkdir -p bin
${CC:-clang} ${CFLAGS:--Wall -Wextra -O2 -std=gnu11 -pthread -I./include} \
  -o bin/tool_policy_test tests/tool_policy_test.c src/tool_policy.c

./bin/tool_policy_test
echo "Tool policy tests passed."
