# Changelog

## [0.1.0-alpha] — 2026-06-26

First tagged alpha of **Star Bridge**.

### Added
- Renamed project to Star Bridge: primary binary `bin/star_bridge` (with a `bin/codex_bridge`
  compatibility symlink for one alpha), default model/alias `star-bridge-ds4`, provider `star-bridge-local`.
- Reasoning-effort changes restart the ds4 agent (save → relaunch with new think flag → switch) — documented behavior.
- `--doctor` one-shot diagnostics (config, agent, venv, socket, port, managed block, catalog).
- Native model picker support: codex-shim-shaped catalog + managed `model_catalog_json`; manual ASAR patch
  documented in `docs/model_picker_setup.md`.
- Mid-turn control plane: `/v1/models` and DELETE-cancel answered during an active turn; cancel ends the
  live stream with `response.failed` ≤2s; concurrent POST returns 409.
- `--max-turn-events` flag; event-cap now completes with partial output + `incomplete_details=max_turn_events`.
- GitHub bug-report issue template; vendored cJSON MIT license file + README attribution.

### Changed
- Dynamic, heap-backed input pipeline removes silent truncation; over-ceiling requests are loudly rejected.
- `/v1/models` no longer advertises bridge-owned browser/search tools (cut from core).
- Production ds4 wrapper promoted to `agent/ds4_wrapper.py`; personal-path defaults removed.
