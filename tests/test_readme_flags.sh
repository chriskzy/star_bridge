#!/usr/bin/env bash
# test_readme_flags.sh — D2 doc guard: every CLI flag in `--help` must appear in
# README.md, and every `--flag` token in README's CLI reference must be a real
# flag in `--help`. Keeps the docs and the binary from drifting apart.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
make >/dev/null

HELP="$("$ROOT/bin/star_bridge" --help 2>&1)"
README="$ROOT/README.md"
fail=0

# Direction 1: every help flag is documented in README.
HELP_FLAGS=$(printf '%s\n' "$HELP" | grep -oE -- '--[a-z][a-z-]+' | sort -u)
for f in $HELP_FLAGS; do
  if grep -q -- "$f" "$README"; then
    :
  else
    echo "FAIL: flag $f from --help is not documented in README.md"; fail=1
  fi
done

# Direction 2: every long flag in the README CLI reference block exists in --help
# (no stale docs). Scoped to the "### CLI reference" fenced block so unrelated
# tooling flags elsewhere in the README (e.g. `docker run --rm`) are not treated
# as bridge flags.
CLI_REF=$(awk '/^### CLI reference/{f=1} f; /^## /{if(f && !/^### CLI reference/) exit}' "$README")
README_FLAGS=$(printf '%s\n' "$CLI_REF" | grep -oE -- '--[a-z][a-z-]+' | sort -u)
for f in $README_FLAGS; do
  if printf '%s\n' "$HELP" | grep -q -- "$f"; then
    :
  else
    echo "FAIL: README mentions $f which is not a real --help flag"; fail=1
  fi
done

if [ "$fail" -ne 0 ]; then echo "readme flags test FAILED"; exit 1; fi
echo "readme flags test passed (all --help flags documented and vice versa)"
