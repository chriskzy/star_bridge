#!/usr/bin/env bash
# test_release_copy.sh — D3: scripts/make_release_copy.sh must produce a
# sanitize-clean, idempotent release tree that excludes archive/specs/bin.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p "$ROOT/tests/.out"
A="$ROOT/tests/.out/relcopy_a"
B="$ROOT/tests/.out/relcopy_b"
trap 'rm -rf "$A" "$B"' EXIT

fail=0

# Two runs must both succeed (sanitize-clean) and yield identical trees.
bash scripts/make_release_copy.sh "$A" >/dev/null
bash scripts/make_release_copy.sh "$B" >/dev/null

if diff -rq "$A" "$B" >/dev/null 2>&1; then
  echo "PASS: release copy is idempotent"
else
  echo "FAIL: release copy not idempotent"; diff -rq "$A" "$B" || true; fail=1
fi

# Excluded directories must not ship.
for excluded in archive specs bin; do
  if [ -e "$A/$excluded" ]; then
    echo "FAIL: '$excluded/' leaked into release copy"; fail=1
  else
    echo "PASS: '$excluded/' excluded"
  fi
done

# Core allowlist members must be present.
for needed in src include agent Makefile README.md LICENSE; do
  if [ -e "$A/$needed" ]; then
    echo "PASS: '$needed' present"
  else
    echo "FAIL: '$needed' missing from release copy"; fail=1
  fi
done

if [ "$fail" -ne 0 ]; then echo "release copy test FAILED"; exit 1; fi
echo "release copy test passed"
