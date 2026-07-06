#!/usr/bin/env bash
# test_hermeticity_poisoned_config.sh — SB-02 meta-test.
# Verifies that `make ci` passes with a poisoned developer-local config.json in cwd.
# A failure here means one or more CI tests is silently loading repo-root config.json
# instead of using its intended --no-config / --config / temp-dir isolation.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

if [ "${STAR_BRIDGE_POISONED_CI:-0}" = "1" ]; then
  echo "SKIP: already running under poisoned config meta-test"
  exit 0
fi

ORIG_CONFIG=""
if [ -f config.json ]; then
  ORIG_CONFIG="$(mktemp /tmp/bridge_orig_config_XXXXXX.json)"
  cp config.json "$ORIG_CONFIG"
fi

cat > config.json <<'EOF'
{
  "server_port": 9033,
  "model_path": "/poisoned/path/model.gguf",
  "native_socket_path": "/poisoned/path/socket.sock"
}
EOF

echo "=== Running make ci with poisoned config.json ==="
if STAR_BRIDGE_POISONED_CI=1 make ci; then
  echo "PASS: make ci green with poisoned config.json"
else
  echo "FAIL: make ci failed with poisoned config.json"
  FAIL=1
fi

if [ -n "$ORIG_CONFIG" ]; then
  mv "$ORIG_CONFIG" config.json
else
  rm -f config.json
fi

exit ${FAIL:-0}
