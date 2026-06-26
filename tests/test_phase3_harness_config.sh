#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$((26000 + $$ % 20000))}"
LOG="$ROOT/tests/.out/.phase3-harness-server.log"

cd "$ROOT"

cleanup() {
  kill "${PID:-}" >/dev/null 2>&1 || true
  wait "${PID:-}" >/dev/null 2>&1 || true
  rm -f config.json
}
trap cleanup EXIT

make clean >/dev/null 2>&1 || true
mkdir -p "$ROOT/tests/.out"
make >/dev/null

cat > config.json <<JSON
{
  "port": $PORT,
  "codex_harness_id": "codex.responses.test",
  "codex_model_alias": "dwarfstar-native-test"
}
JSON

./bin/star_bridge /bin/cat . --native-transport stdio -p "$PORT" >"$LOG" 2>&1 &
PID=$!

for _ in {1..50}; do
  if curl -fsS "http://127.0.0.1:$PORT/v1/models" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
done

MODELS="$(curl -fsS "http://127.0.0.1:$PORT/v1/models")"
grep -q '"id":"star-bridge-ds4"' <<<"$MODELS"
grep -q 'dwarfstar-native-test' <<<"$MODELS"

RESP="$(curl -fsS -X POST "http://127.0.0.1:$PORT/v1/responses" \
  -H 'Content-Type: application/json' \
  --data '{"input":"phase3 harness config"}')"

grep -q '"model":"dwarfstar-native-test"' <<<"$RESP"
grep -q 'phase3 harness config' <<<"$RESP"

for _ in {1..20}; do
  if grep -q 'harness=codex.responses.test' "$LOG"; then
    break
  fi
  sleep 0.1
done
grep -q 'harness=codex.responses.test' "$LOG"
grep -q 'request=1' "$LOG"

echo "Phase 3 harness config tests passed."
