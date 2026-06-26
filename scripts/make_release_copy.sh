#!/usr/bin/env bash
# make_release_copy.sh — assemble a clean Star Bridge release tree for the manual
# folder-copy publish flow. Copies ONLY an explicit allowlist into TARGET, so
# archive/, specs/, bin/, logs, caches, venvs, and private config never ship.
#
# Usage:   scripts/make_release_copy.sh <target-dir>
# Example: scripts/make_release_copy.sh /tmp/star_bridge_release
#
# The script is idempotent: it wipes and rebuilds TARGET each run, so running it
# twice yields byte-identical trees. After copying it runs the same sanitize
# checks the CI gate enforces (no Mach-O, __pycache__, .dSYM, chriskz, /Users/)
# against the COPY, and fails loudly if anything leaked.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ $# -lt 1 ]; then
  echo "usage: $0 <target-dir>" >&2
  exit 2
fi
TARGET="$1"

# Directories shipped wholesale (test doubles/fixtures included; build output is not).
ALLOW_DIRS=(
  src include agent tests scripts docs vendor .github
)
# Top-level files shipped if present. ARCHITECTURE.md / CONTRIBUTING.md are created
# by D2 and copied when they exist.
ALLOW_FILES=(
  Makefile README.md ARCHITECTURE.md CHANGELOG.md LICENSE CONTRIBUTING.md
  config.example.json Dockerfile docker-compose.yml .gitignore .dockerignore
)

echo "=== Building release copy in $TARGET ==="
rm -rf "$TARGET"
mkdir -p "$TARGET"

for d in "${ALLOW_DIRS[@]}"; do
  if [ -d "$ROOT/$d" ]; then
    mkdir -p "$TARGET/$d"
    # Copy contents, excluding build/cache/log noise.
    rsync -a \
      --exclude '__pycache__' \
      --exclude '*.pyc' \
      --exclude '.out' \
      --exclude '*.dSYM' \
      --exclude '*.log' \
      --exclude '*.o' \
      --exclude '.venv' \
      "$ROOT/$d/" "$TARGET/$d/"
    echo "  + $d/"
  fi
done

for f in "${ALLOW_FILES[@]}"; do
  if [ -f "$ROOT/$f" ]; then
    cp "$ROOT/$f" "$TARGET/$f"
    echo "  + $f"
  fi
done

echo "=== Stripping compiled test binaries from the copy ==="
# tests/ ships sources only; remove any committed/compiled binaries that slipped in.
find "$TARGET/tests" -type f -perm -u+x 2>/dev/null | while read -r exe; do
  case "$exe" in
    *.sh|*.py) : ;;  # keep scripts
    *) if file -b "$exe" 2>/dev/null | grep -q 'Mach-O\|ELF'; then rm -f "$exe"; echo "  - $(basename "$exe")"; fi ;;
  esac
done

echo "=== Sanitize gate on the copy ==="
fail=0
if find "$TARGET" -type f -exec file -b {} \; 2>/dev/null | grep -q 'Mach-O'; then
  echo "FAIL: Mach-O binary in copy"; fail=1
fi
if find "$TARGET" -name '__pycache__' -o -name '*.dSYM' | grep -q .; then
  echo "FAIL: cache/dSYM in copy"; fail=1
fi
# The sanitize-checking code itself contains these strings as search patterns
# (this script and the CI gate in .github/workflows). Exclude them — they are not
# leaked personal paths. This mirrors the CI gate's own exclusions.
SANITIZE_EXCLUDE='(/\.git/|/\.github/|scripts/make_release_copy\.sh$)'
CHRIS=$(grep -rIl 'chriskz' "$TARGET" 2>/dev/null | grep -vE "$SANITIZE_EXCLUDE" || true)
if [ -n "$CHRIS" ]; then
  echo "FAIL: 'chriskz' in copy: $CHRIS"; fail=1
fi
USERS=$(grep -rIl '/Users/' "$TARGET" 2>/dev/null | grep -vE "$SANITIZE_EXCLUDE" || true)
if [ -n "$USERS" ]; then
  echo "FAIL: '/Users/' in copy: $USERS"; fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "=== Release copy FAILED sanitize ==="
  exit 1
fi

echo "=== Release copy ready and sanitize-clean: $TARGET ==="
