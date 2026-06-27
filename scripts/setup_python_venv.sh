#!/usr/bin/env bash
# Create the wrapper virtualenv. Python 3.14 is PREFERRED, but this script
# genuinely falls back to any available python3 (>= 3.8) so a fresh checkout,
# CI runner, or Docker build without 3.14 still works.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="$ROOT/.venv"

# Preference order: explicit override, 3.14, then progressively older, then plain python3.
CANDIDATES=("${PYTHON314:-}" python3.14 python3.13 python3.12 python3.11 python3.10 python3)
PY=""
for c in "${CANDIDATES[@]}"; do
  [[ -n "$c" ]] || continue
  if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done

if [[ -z "$PY" ]]; then
  echo "Error: no usable python3 found. Install Python 3 (3.14 preferred, e.g. 'brew install python@3.14')." >&2
  exit 1
fi

PYVER="$("$PY" --version 2>&1)"
if ! grep -q 'Python 3\.14\.' <<<"$PYVER"; then
  echo "Note: Python 3.14 not found; using $PY ($PYVER). 3.14 is preferred but not required." >&2
fi

# Create the venv if missing, or recreate it only if the existing interpreter is
# broken (NOT merely an older 3.x — we accept the fallback version).
if [[ ! -x "$VENV/bin/python3" ]]; then
  echo "Creating venv at $VENV with $PY ($PYVER)"
  rm -rf "$VENV"
  "$PY" -m venv "$VENV"
elif ! "$VENV/bin/python3" --version >/dev/null 2>&1; then
  echo "Recreating broken venv at $VENV with $PY ($PYVER)"
  rm -rf "$VENV"
  "$PY" -m venv "$VENV"
fi

VER="$("$VENV/bin/python3" --version 2>&1)"
echo "venv ready: $VER at $VENV"
