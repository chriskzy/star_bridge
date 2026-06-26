#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="$ROOT/.venv"
PY314="${PYTHON314:-python3.14}"

if ! command -v "$PY314" >/dev/null 2>&1; then
  echo "Error: $PY314 not found. Install Python 3.14 (e.g. brew install python@3.14)" >&2
  exit 1
fi

if [[ ! -d "$VENV" ]]; then
  echo "Creating venv at $VENV with $PY314"
  "$PY314" -m venv "$VENV"
else
  CURRENT="$("$VENV/bin/python3" --version 2>&1 || true)"
  if ! grep -q 'Python 3\.14\.' <<<"$CURRENT"; then
    echo "Recreating venv (was: $CURRENT)"
    rm -rf "$VENV"
    "$PY314" -m venv "$VENV"
  fi
fi

VER="$("$VENV/bin/python3" --version 2>&1)"
echo "venv ready: $VER at $VENV"