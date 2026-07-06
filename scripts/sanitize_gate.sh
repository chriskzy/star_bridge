#!/usr/bin/env bash
# Sanitize gate: fail if tracked non-source junk exists.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "=== Sanitize gate ==="

MACHO=$(
  while IFS= read -r -d '' path; do
    if file -b -- "$path" 2>/dev/null | grep -q "Mach-O"; then
      printf '%s\n' "$path"
    fi
  done < <(git ls-files -z)
) || true
if [ -n "$MACHO" ]; then echo "FAIL: tracked Mach-O files: $MACHO"; exit 1; fi

PYCACHE=$(git ls-files | grep -E '__pycache__' || true)
if [ -n "$PYCACHE" ]; then echo "FAIL: tracked __pycache__: $PYCACHE"; exit 1; fi

DSYM=$(git ls-files | grep -E '\.dSYM' || true)
if [ -n "$DSYM" ]; then echo "FAIL: tracked .dSYM: $DSYM"; exit 1; fi

PUBLISH=$(git ls-files | grep -vE '\.github/|^archive/|^docs/evidence/|^docs/star_bridge_fable_improvement_plan\.md$|RELEASE_VERIFICATION\.md$|scripts/make_release_copy\.sh$|scripts/sanitize_gate\.sh$')
CHRIS=$(printf '%s\n' "$PUBLISH" | xargs git grep -I --fixed-strings 'chriskz' -- 2>/dev/null || true)
if [ -n "$CHRIS" ]; then echo "FAIL: tracked files contain 'chriskz': $CHRIS"; exit 1; fi

USERS=$(printf '%s\n' "$PUBLISH" | xargs git grep -I --fixed-strings '/Users/' -- 2>/dev/null || true)
if [ -n "$USERS" ]; then echo "FAIL: tracked files contain '/Users/': $USERS"; exit 1; fi

JUNK=$(git ls-files | grep -F -e '<test>' -e '.codex-bridge-debug.log' || true)
if [ -n "$JUNK" ]; then echo "FAIL: tracked junk: $JUNK"; exit 1; fi

echo "Sanitize gate passed."
