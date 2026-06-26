#!/usr/bin/env bash
# Test that --generate-config produces valid JSON artifacts.
# Uses vendored cJSON path.
set -euo pipefail

cjson_inc="-I./vendor/cjson"
cjson_lib="./vendor/cjson/cJSON.c"

OUTDIR=$(mktemp -d /tmp/star_bridge_config_gen_XXXXXX)
trap "rm -rf $OUTDIR" EXIT

cd "$(dirname "$0")/.." || exit 1

echo "=== Test: --generate-config writes catalog and provider ==="
./bin/star_bridge --generate-config "$OUTDIR" 2>&1

if [ ! -f "$OUTDIR/custom_model_catalog.json" ]; then
    echo "FAIL: custom_model_catalog.json not found"
    exit 1
fi

if [ ! -f "$OUTDIR/star_bridge_provider.json" ]; then
    echo "FAIL: star_bridge_provider.json not found"
    exit 1
fi

echo "=== Validate JSON syntax ==="
python3 -c "import json; json.load(open('$OUTDIR/custom_model_catalog.json'))" || {
    echo "FAIL: custom_model_catalog.json is not valid JSON"
    exit 1
}
python3 -c "import json; json.load(open('$OUTDIR/star_bridge_provider.json'))" || {
    echo "FAIL: star_bridge_provider.json is not valid JSON"
    exit 1
}

echo "=== Verify model catalog structure ==="
python3 -c "
import json
cat = json.load(open('$OUTDIR/custom_model_catalog.json'))
assert 'models' in cat, 'missing models'
assert len(cat['models']) == 1, 'expected 1 model'
m = cat['models'][0]
assert m['id'] == 'star-bridge-ds4', 'wrong model id'
assert m['provider'] == 'custom', 'wrong provider'
assert m['capabilities']['function_calling'] == True, 'missing function_calling'
assert m['capabilities']['context'] == 150000, 'wrong capability context'
assert m['config']['wire_api'] == 'responses', 'wrong wire_api'
assert m['config']['model_alias'] == 'star-bridge-ds4', 'wrong model_alias'
print('Model catalog OK')
" 2>&1

echo "=== Verify provider config structure ==="
python3 -c "
import json
prov = json.load(open('$OUTDIR/star_bridge_provider.json'))
assert prov['provider_id'] == 'star-bridge-local', 'wrong provider_id'
assert prov['api_base'] == 'http://127.0.0.1:8080/v1', 'wrong api_base'
assert prov['wire_api'] == 'responses', 'wrong wire_api'
assert prov['model_alias'] == 'star-bridge-ds4', 'wrong model_alias'
print('Provider config OK')
" 2>&1

echo "=== Test: --generate-config without arg uses current dir ==="
TMPDIR=$(mktemp -d /tmp/star_bridge_config_gen_XXXXXX)
trap "rm -rf $TMPDIR" EXIT
cd "$TMPDIR"
"$OLDPWD/bin/star_bridge" --generate-config 2>&1
if [ ! -f custom_model_catalog.json ]; then
    echo "FAIL: no file in current dir"
    exit 1
fi
rm custom_model_catalog.json star_bridge_provider.json
cd "$OLDPWD"

echo "=== TDD RED case: --generate-config on fresh non-existing dir must succeed (mkdir inside) ==="
FRESHDIR="/tmp/star_bridge_fresh_gen_$$_$(date +%s)"
rm -rf "$FRESHDIR"
./bin/star_bridge --generate-config "$FRESHDIR" 2>&1 || {
    echo "RED: generate failed on non-existing dir (no mkdir -p)"
    ls -ld "$FRESHDIR" 2>/dev/null || true
    exit 1
}
if [ ! -f "$FRESHDIR/custom_model_catalog.json" ] || [ ! -f "$FRESHDIR/star_bridge_provider.json" ]; then
    echo "RED: files not created in fresh dir"
    exit 1
fi
echo "temp files created, will cleanup"
rm -rf "$FRESHDIR"

echo "=== Test: --generate-config with invalid path ==="
./bin/star_bridge --generate-config /nonexistent/dir 2>&1 && {
    echo "FAIL: should have failed on invalid path"
    exit 1
} || true

echo "ALL PASS"
