# Native Model Picker Setup

Goal: make the local DwarfStar/ds4 model appear in Codex Desktop's model
("intelligence") picker, side-by-side with the OpenAI models, so you can pick
local or cloud per thread.

There are two pieces:

1. **Catalog + managed config** — automated by `--install`.
2. **ASAR app patch** — manual, macOS-only, because Codex Desktop hides
   non-allowlisted model slugs behind a Statsig flag.

---

## 1. Catalog + managed config (automated)

```sh
./bin/star_bridge /path/to/ds4-agent /path/to/workspace -p 9033 --install
```

This writes:

- `~/.codex/config.toml` — a marked managed block containing
  `model_catalog_json = "~/.codex/custom_catalog.json"` plus the
  `star-bridge-local` provider pointing at `http://127.0.0.1:<port>/v1`.
- `~/.codex/custom_catalog.json` — the picker catalog. One entry, codex-shim
  schema:

  ```json
  {
    "models": [
      {
        "slug": "star-bridge-ds4",
        "display_name": "Star Bridge ds4",
        "provider": "star-bridge-local",
        "max_context_limit": 150000,
        "input_modalities": ["text"],
        "supports_parallel_tool_calls": true,
        "hidden": false
      }
    ]
  }
  ```

Verify with `--status`, or run `./bin/star_bridge --doctor` which checks the
managed block and catalog file are present.

Roll back any time with `--disable`.

---

## 2. ASAR app patch (manual, macOS-only)

Codex Desktop ships a Statsig allowlist that hides model slugs it does not
recognize, so the catalog alone is not enough — the local model stays hidden in
the picker until the app's bundled JavaScript is patched.

Star Bridge starts with this documented manual patch flow. A future
`star_bridge patch-app` / `restore-app` command pair can wrap the same steps if
the manual path proves too error-prone. The needle below is version-sensitive;
re-locate it after Codex Desktop updates.

### Prerequisites

- `npm install -g @electron/asar` (provides the `asar` CLI).
- Codex Desktop installed at `/Applications/Codex.app` (adjust path as needed).
- `codesign` (Xcode command line tools).

### Steps

1. **Quit Codex Desktop.**

2. **Back up the app archive.**

   ```sh
   APP="/Applications/Codex.app/Contents/Resources"
   cp "$APP/app.asar" "$APP/app.asar.star-bridge-backup"
   ```

3. **Extract.**

   ```sh
   asar extract "$APP/app.asar" /tmp/codex-app
   ```

4. **Disable the hidden-model enforcement.** Find the Statsig gate that hides
   non-approved slugs. It is typically a `useHiddenModels` (or similarly named)
   check whose value comes from a Statsig flag. Flip the enforcement so all
   catalog entries render:

   ```sh
   grep -rn "useHiddenModels" /tmp/codex-app/
   ```

   In the matched file, force the gate's result to `false` (do not hide). The
   exact expression depends on the Codex build; the intent is: the picker should
   render catalog entries regardless of the Statsig allowlist.

5. **(Optional) Keep OpenAI recent threads loading.** A second guard can break
   loading of OpenAI cloud threads once the first patch is applied. Locate the
   thread-loading path and ensure it does not early-return for local providers.
   Skip this step unless you observe OpenAI threads failing to load after step 4.

6. **Repack.**

   ```sh
   asar pack /tmp/codex-app "$APP/app.asar"
   ```

7. **Re-sign** (required or macOS Gatekeeper refuses to launch the modified app):

   ```sh
   codesign --force --deep --sign - /Applications/Codex.app
   ```

8. **Relaunch Codex Desktop.** The picker should now show
   `Star Bridge ds4` alongside the OpenAI models.

### Restore

```sh
APP="/Applications/Codex.app/Contents/Resources"
mv "$APP/app.asar.star-bridge-backup" "$APP/app.asar"
codesign --force --deep --sign - /Applications/Codex.app
```

### Verify (manual checklist)

- [ ] Picker shows `Star Bridge ds4` (slug `star-bridge-ds4`).
- [ ] OpenAI models are still listed and selectable.
- [ ] Selecting the local model routes turns to the bridge
      (`http://127.0.0.1:<port>/v1`); OpenAI threads still reach the cloud.

---

## Notes

- The catalog/managed-config half is reversible and safe. The ASAR patch edits a
  signed third-party app bundle — keep the backup, and re-apply after every
  Codex Desktop update (the needle moves between builds).
- Generated catalogs use the `star-bridge-ds4` slug and `star-bridge-local`
  provider by default.
