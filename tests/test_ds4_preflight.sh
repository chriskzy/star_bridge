#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BAD_HOME="/tmp/ds4-preflight-bad-$$"
BAD_AGENT="$BAD_HOME/ds4-agent"

mkdir -p "$BAD_HOME"
cat >"$BAD_AGENT" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
chmod +x "$BAD_AGENT"

cd "$ROOT"
make venv >/dev/null
make >/dev/null

set +e
OUT="$(./bin/star_bridge "$BAD_AGENT" . --no-config -p 29999 2>&1)"
RC=$?
set -e

[[ "$RC" -ne 0 ]] || { echo "FAIL: preflight should reject agent without metal assets"; exit 1; }
echo "$OUT" | grep -q "Metal assets missing" || {
  echo "FAIL: expected Metal assets missing error"
  echo "$OUT"
  exit 1
}

rm -rf "$BAD_HOME"
echo "ds4 preflight test passed."