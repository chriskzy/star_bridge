# Contributing to Star Bridge

Thanks for helping. Star Bridge is a small, inspectable C codebase; changes should keep
it that way — the smallest robust change, no clever abstractions before tests prove the
need.

## Build & test

```sh
make           # build bin/star_bridge (+ bin/codex_bridge compat symlink)
make ci        # curated regression gate (the bar for every PR)
make test      # full local suite
make clean     # remove binaries and tests/.out/
```

Primary platform is macOS arm64; Linux is supported. CI runs both
(`.github/workflows/ci.yml`). You need a C11 compiler, `make`, `zlib`, and Python 3 for
the wrapper venv.

## Before opening a PR

- `make ci` is green locally.
- New behavior has a test. Shell tests live in `tests/` and write artifacts only to
  `tests/.out/` (gitignored). Use the fake agents (`tests/fake_agent.py`,
  `tests/fake_native_agent.py`) rather than a real ds4 where possible.
- No personal paths or secrets. The CI **sanitize gate** fails on tracked Mach-O
  binaries, `__pycache__`, `*.dSYM`, and any home-directory paths or usernames in tracked
  files. Don't commit compiled test binaries or local logs.
- CLI changes update both `--help` and the README CLI reference — `tests/test_readme_flags.sh`
  enforces they match.
- The streaming event order is pinned by `tests/test_stream_lifecycle_golden.sh`; if you
  touch the SSE path, keep it green.

## Style

- Match the surrounding code: comment density, naming, idiom.
- Keep `src/server.c` input/output buffer semantics straight — input buffers are heap and
  must not silently truncate; output clips must surface `incomplete_details`.
- Document non-obvious protocol or lifecycle behavior in `ARCHITECTURE.md`.

## Reporting bugs

Use the bug-report issue template; include the bridge command line, a
`.codex-bridge-debug.log` excerpt, and Star Bridge / Codex / ds4 / OS versions.
