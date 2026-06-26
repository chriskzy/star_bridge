#!/usr/bin/env bash
# Release rename regression: star_bridge is primary binary, codex_bridge remains compat.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [[ ! -x ./bin/star_bridge ]]; then
    echo "FAIL: bin/star_bridge missing or not executable"
    exit 1
fi

if [[ ! -e ./bin/codex_bridge ]]; then
    echo "FAIL: bin/codex_bridge compatibility entry missing"
    exit 1
fi

VERSION_OUT="$(./bin/star_bridge --version)"
case "$VERSION_OUT" in
    "star_bridge version "*) ;;
    *)
        echo "FAIL: unexpected star_bridge --version output: $VERSION_OUT"
        exit 1
        ;;
esac

COMPAT_OUT="$(./bin/codex_bridge --version)"
if [[ "$COMPAT_OUT" != "$VERSION_OUT" ]]; then
    echo "FAIL: compat binary version differs: $COMPAT_OUT != $VERSION_OUT"
    exit 1
fi

echo "star_bridge binary test passed"
