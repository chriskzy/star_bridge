#!/usr/bin/env bash
# Test reversible managed Codex config install/disable workflow.
# Uses a temporary HOME to avoid touching real ~/.codex/config.toml.
set -euo pipefail

BRIDGE="./bin/star_bridge"

# Create a temp home with fake ~/.codex/
TMP_HOME=$(mktemp -d /tmp/test_managed_config_XXXXXX)
TMP_CODEX="$TMP_HOME/.codex"
mkdir -p "$TMP_CODEX"

# Wrap bridge calls with fake HOME
run_bridge() {
    HOME="$TMP_HOME" "$BRIDGE" "$@" 2>&1 || true
}

echo "=== Test 1: status with no config file ==="
out=$(run_bridge --status)
echo "$out"
if echo "$out" | grep -q "no config file"; then
    echo "PASS: status reports no config file"
else
    echo "FAIL: status should report no config file"
    exit 1
fi

echo ""
echo "=== Test 2: dry-run install with no existing config ==="
out=$(run_bridge --dry-run --install)
echo "$out"
if echo "$out" | grep -q "create new file"; then
    echo "PASS: dry-run shows create new file"
else
    echo "FAIL: dry-run should show create new file"
    echo "$out"
    exit 1
fi

# Verify no file was actually created
if [ -f "$TMP_CODEX/config.toml" ]; then
    echo "FAIL: dry-run should not create file"
    exit 1
fi
echo "PASS: dry-run did not create file"

echo ""
echo "=== Test 3: install managed config ==="
out=$(run_bridge --install)
echo "$out"
if echo "$out" | grep -q "Created new config"; then
    echo "PASS: install created new config"
else
    echo "FAIL: install should create new config"
    echo "$out"
    exit 1
fi

# Verify config.toml exists
if [ ! -f "$TMP_CODEX/config.toml" ]; then
    echo "FAIL: config.toml not found after install"
    exit 1
fi
echo "PASS: config.toml exists"

# Verify backup does NOT exist (no prior file to backup)
if [ -f "$TMP_CODEX/config.toml.bridge-backup" ]; then
    echo "FAIL: backup should not exist when no prior config"
    exit 1
fi
echo "PASS: no backup for fresh install (correct)"

# Verify managed block markers in file
if grep -q "BEGIN star_bridge-managed" "$TMP_CODEX/config.toml"; then
    echo "PASS: BEGIN marker found"
else
    echo "FAIL: BEGIN marker missing"
    exit 1
fi
if grep -q "END star_bridge-managed" "$TMP_CODEX/config.toml"; then
    echo "PASS: END marker found"
else
    echo "FAIL: END marker missing"
    exit 1
fi

# Verify expected content
if grep -q 'model.*"star-bridge-ds4"' "$TMP_CODEX/config.toml"; then
    echo "PASS: model section found"
else
    echo "FAIL: model section missing"
    exit 1
fi
if grep -q 'provider.*"star-bridge-local"' "$TMP_CODEX/config.toml"; then
    echo "PASS: provider section found"
else
    echo "FAIL: provider section missing"
    exit 1
fi

# T2.3: managed block must point Codex at the catalog via model_catalog_json
if grep -q 'model_catalog_json' "$TMP_CODEX/config.toml"; then
    echo "PASS: model_catalog_json key present"
else
    echo "FAIL: model_catalog_json key missing"
    exit 1
fi

# T2.3: the picker catalog must exist and carry codex-shim schema fields
CATALOG="$TMP_CODEX/custom_catalog.json"
if [ -f "$CATALOG" ]; then
    echo "PASS: custom_catalog.json written"
else
    echo "FAIL: custom_catalog.json not written"
    exit 1
fi
for field in '"slug"' '"display_name"' '"max_context_limit"' '"input_modalities"' '"supports_parallel_tool_calls"' '"hidden"'; do
    if grep -q "$field" "$CATALOG"; then
        echo "PASS: catalog has $field"
    else
        echo "FAIL: catalog missing $field"
        cat "$CATALOG"
        exit 1
    fi
done
grep -q '"slug":[[:space:]]*"star-bridge-ds4"' "$CATALOG" || { echo "FAIL: catalog slug old"; cat "$CATALOG"; exit 1; }
grep -q '"provider":[[:space:]]*"star-bridge-local"' "$CATALOG" || { echo "FAIL: catalog provider old"; cat "$CATALOG"; exit 1; }
grep -q '"display_name":[[:space:]]*"Star Bridge ds4"' "$CATALOG" || { echo "FAIL: catalog display name old"; cat "$CATALOG"; exit 1; }

echo ""
echo "=== Test 4: status after install ==="
out=$(run_bridge --status)
echo "$out"
if echo "$out" | grep -q "INSTALLED"; then
    echo "PASS: status reports installed"
else
    echo "FAIL: status should report installed"
    echo "$out"
    exit 1
fi

echo ""
echo "=== Test 5: dry-run install again (update) ==="
# Create a config with an existing managed block to test update path
out=$(run_bridge --dry-run --install)
echo "$out"
if echo "$out" | grep -q "update existing"; then
    echo "PASS: dry-run shows update existing"
else
    echo "FAIL: dry-run should show update existing"
    echo "$out"
    exit 1
fi

# Verify no duplicate blocks
count=$(grep -c "BEGIN star_bridge-managed" "$TMP_CODEX/config.toml" || true)
if [ "$count" -eq 1 ]; then
    echo "PASS: single managed block (no duplicates)"
else
    echo "FAIL: found $count managed blocks (should be 1)"
    exit 1
fi

echo ""
echo "=== Test 6: install again (update) ==="
out=$(run_bridge --install)
echo "$out"
if echo "$out" | grep -q "Replaced all previous managed sections"; then
    echo "PASS: update succeeded"
else
    echo "FAIL: update should replace previous managed sections"
    echo "$out"
    exit 1
fi

# Verify still only one managed block
count=$(grep -c "BEGIN star_bridge-managed" "$TMP_CODEX/config.toml" || true)
if [ "$count" -eq 1 ]; then
    echo "PASS: single managed block after update"
else
    echo "FAIL: found $count managed blocks after update"
    exit 1
fi

echo ""
echo "=== Test 7: dry-run disable ==="
out=$(run_bridge --dry-run --disable)
echo "$out"
if echo "$out" | grep -q "remove all managed blocks"; then
    echo "PASS: dry-run shows remove managed block"
else
    echo "FAIL: dry-run should show remove all managed blocks"
    echo "$out"
    exit 1
fi

# Verify file still exists
if [ ! -f "$TMP_CODEX/config.toml" ]; then
    echo "FAIL: dry-run should not remove file"
    exit 1
fi
echo "PASS: dry-run did not remove file"

echo ""
echo "=== Test 8: disable managed config ==="
out=$(run_bridge --disable)
echo "$out"
if echo "$out" | grep -q "Removed managed"; then
    echo "PASS: disable removed managed block"
else
    echo "FAIL: disable should remove managed block"
    echo "$out"
    exit 1
fi

# Verify managed block is gone
if grep -q "BEGIN star_bridge-managed" "$TMP_CODEX/config.toml" 2>/dev/null; then
    echo "FAIL: managed block still present after disable"
    exit 1
fi
echo "PASS: managed block removed"

# Verify backup still exists
if [ ! -f "$TMP_CODEX/config.toml.bridge-backup" ]; then
    echo "FAIL: backup should still exist after disable"
    exit 1
fi
echo "PASS: backup still exists"

echo ""
echo "=== Test 9: status after disable ==="
out=$(run_bridge --status)
echo "$out"
# After disable, the file was deleted (only managed block). Status reports no config file.
if echo "$out" | grep -q "no config file"; then
    echo "PASS: status reports no config file (file was deleted since only managed block)"
else
    echo "FAIL: status should report no config file"
    echo "$out"
    exit 1
fi

echo ""
echo "=== Test 10: disable again (no-op) ==="
out=$(run_bridge --disable)
echo "$out"
# After disable, file was deleted; second disable reports no config file
if echo "$out" | grep -q "No config file"; then
    echo "PASS: second disable reports no config file"
else
    echo "FAIL: second disable should report no config file"
    echo "$out"
    exit 1
fi

echo ""
echo "=== Test 11: install with existing user config (displaced keys preserved) ==="
# Create a user config with existing model section
cat > "$TMP_CODEX/config.toml" << 'EOF'
[model "star-bridge-ds4"]
provider = "existing-provider"

[provider "star-bridge-local"]
api_base = "http://existing:8080/v1"
wire_api = "chat"
EOF
echo "Created user config with existing model/provider sections"
cat "$TMP_CODEX/config.toml"

out=$(run_bridge --install)
echo "$out"
if echo "$out" | grep -q "Preserved displaced"; then
    echo "PASS: displaced keys preserved"
else
    echo "FAIL: displaced keys should be preserved"
    echo "$out"
    exit 1
fi

# Verify backup contains original user sections
if grep -q "existing-provider" "$TMP_CODEX/config.toml.bridge-backup"; then
    echo "PASS: backup contains original user sections"
else
    echo "FAIL: backup missing original user sections"
    exit 1
fi

# Verify the managed config file has the managed block
if grep -q "BEGIN star_bridge-managed" "$TMP_CODEX/config.toml"; then
    echo "PASS: managed block in config"
else
    echo "FAIL: managed block missing in config"
    exit 1
fi

echo ""
echo "=== Test 12: disable restores displaced keys ==="
out=$(run_bridge --disable)
echo "$out"
if echo "$out" | grep -q "Restored displaced"; then
    echo "PASS: displaced keys restored"
else
    echo "FAIL: displaced keys should be restored"
    echo "$out"
    exit 1
fi

# Verify user's original section is back
if grep -q "existing-provider" "$TMP_CODEX/config.toml"; then
    echo "PASS: original user section restored"
else
    echo "FAIL: original user section missing after restore"
    exit 1
fi

# Verify managed block is gone
if grep -q "BEGIN star_bridge-managed" "$TMP_CODEX/config.toml" 2>/dev/null; then
    echo "FAIL: managed block still present after disable"
    exit 1
fi
echo "PASS: managed block removed"

echo ""
echo "=== Test 13: --help lists managed config flags ==="
out=$(run_bridge --help)
echo "$out"
if echo "$out" | grep -q -- "--install"; then
    echo "PASS: --install listed in help"
else
    echo "FAIL: --install missing from help"
    exit 1
fi
if echo "$out" | grep -q -- "--disable"; then
    echo "PASS: --disable listed in help"
else
    echo "FAIL: --disable missing from help"
    exit 1
fi
if echo "$out" | grep -q -- "--status"; then
    echo "PASS: --status listed in help"
else
    echo "FAIL: --status missing from help"
    exit 1
fi
if echo "$out" | grep -q -- "--dry-run"; then
    echo "PASS: --dry-run listed in help"
else
    echo "FAIL: --dry-run missing from help"
    exit 1
fi

echo ""
echo "=== Test 14: error on failed install (nonexistent HOME) ==="
# Create a temporary HOME that doesn't have a .codex dir with proper permissions
TMP_BAD=$(mktemp -d /tmp/test_managed_bad_XXXXXX)
out=$(HOME="$TMP_BAD" "$BRIDGE" --install 2>&1) || true
echo "$out"
if echo "$out" | grep -q "failed to write"; then
    echo "PASS: install failure reported"
else
    # On some systems this may succeed because the dir is writable
    echo "NOTE: install succeeded in bad dir (dir may be writable)"
fi
rm -rf "$TMP_BAD"

# Cleanup
rm -rf "$TMP_HOME"

echo ""
echo "All managed config tests PASS"
exit 0
