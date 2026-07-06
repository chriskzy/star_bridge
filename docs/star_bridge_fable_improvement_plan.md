# Star Bridge Fable Improvement Plan

Created: 2026-07-05. Owner doc for all Star Bridge improvement work from this date.
Scope: local Gitea repo (`/Users/chriskz/dev/Terminus/Star_AE`) only. The GitHub sister
repo (https://github.com/chriskzy/star_bridge) is read-only for now; changes migrate
there later by manual copy (see §10).

## 0. Verdict

State: solid against fake agents, unproven against the real agent, and the test gate
has silently gone red.

- North star: **"a straightforward bridge that passes comms bi-directionally between
  the native ds4 agent and Codex or another harness accessing the HTTP exposed by
  the bridge."**
- Overall matrix score today: **6.2 / 10** (weighted, §4.4).
- The single biggest drag is A11 (real ds4 end-to-end): **3.8 / 10**. Every prior plan
  ended with "one live pass remains" and that pass never happened. `RELEASE_VERIFICATION.md`
  sections 5–6 are unchecked and `v0.1.0-alpha` was never tagged (`git tag` is empty
  despite a 2026-06-26 CHANGELOG entry).
- `make ci` is **red on the current checkout** (2026-07-05): `tests/test_generate_config.sh`
  asserts `api_base == http://127.0.0.1:8080/v1` but runs `--generate-config` without
  `--no-config`, so the developer-local `config.json` (`port: 9033`) leaks in. This is a
  test-hermeticity defect, not a product defect — and it is exactly the failure mode
  that killed previous plans' "green" claims.

Priority is not more feature surface. Priority is: restore a trustworthy green, then
prove the north star live, then deepen.

## 1. North star, decomposed

The north star has four load-bearing clauses. Every feature is scored against them.

| Clause | Meaning | Measurable proxy |
| --- | --- | --- |
| N1 straightforward | Zero-surprise setup and operation; small inspectable codebase | Quickstart works verbatim; `wc -l src` trend; no dead modules |
| N2 passes comms bi-directionally | Requests in, deltas/events/errors out, with no silent loss either way | Golden stream transcripts; no-silent-truncation invariants; fault-injection suite |
| N3 native ds4 agent | The real agent, not the fake | Repeatable live smoke green (first turn, resume, switch, effort restart, cancel) |
| N4 Codex **or another harness** over HTTP | The HTTP surface is a documented contract, not "whatever Codex tolerates" | Written Responses-API-subset contract; second-harness (plain OpenAI SDK / curl) test suite green |

N3 and N4 are the weak clauses today. N3 has never been proven end-to-end. N4 has no
written contract and no non-Codex consumer test.

## 2. Lessons from previous plans

Sources: `archive/review_star_ae.md`, `archive/tasklist_improvements.md`,
`archive/star_bridge_release_plan.md`, `archive/user_journeys.md`,
`archive/release_tasklist.md`, `RELEASE_VERIFICATION.md`, git log.

### What worked — keep doing

1. **Red-test-first P0 fixes landed and stuck.** All four 2026-06 P0s are verified in
   source today: constant-time auth compare (`src/server.c:37`, `:1950`), deadline-aware
   frame reads (`src/native_frame.c:41`), UDS wiring, structured native errors. Evidence
   tables with file:line references made these verifiable months later.
2. **User-journey framing (UJ-01..20).** It converted vague "improve the bridge" into
   20 checkable behaviors. This plan reuses that inventory.
3. **A release bar with named gates** (G1–G7, M0–M7) plus the sanitize gate and
   `scripts/make_release_copy.sh`. The hygiene work (untracked binaries, personal paths)
   actually happened and held.
4. **Archiving doc sprawl.** 15+ planning docs moved to `archive/`; the public surface
   (README, ARCHITECTURE, CHANGELOG, RELEASE_VERIFICATION) is coherent.

### What failed — design this plan around it

1. **"Green" claims decayed silently.** The 2026-06-26 plan said `make ci` green; today
   it is red because a test depends on developer-local `config.json`. Nothing re-ran the
   gate after the claim. Fix: hermetic tests + a standing rule that every score in this
   doc carries a dated command output, and re-scoring reruns the commands (§8).
2. **Live validation was always "the last step" and never happened.** Three consecutive
   docs deferred the real-ds4 pass. The last recorded live attempt (2026-06-26) failed:
   `/health` 503, stale session context leaked into a fresh prompt, DELETE blocked
   mid-turn. Code fixes for all three landed afterwards (mid-turn control plane, session
   keying) but only against fakes. Fix: live proof moves to Phase 1, before any deepening
   work, and gets a scripted, repeatable, manual-safe harness instead of ad-hoc runs.
3. **Scores without a protocol.** The brooks-lint 58/100 audit was cited once and never
   re-measured, so "improvement" was unfalsifiable. Fix: §8 defines the exact re-scoring
   protocol any agent can run.
4. **Task-count success instead of outcome success.** `release_tasklist.md` (39K) burned
   down tasks while the two outcomes that mattered (tag shipped, live smoke green) stayed
   open. Fix: phases here gate on outcomes, each with a single yes/no evidence artifact.
5. **Root litter regenerates.** Cleaned in June; today the repo root again holds a
   tracked 9.7K junk file literally named `<test>` and a 59K `.codex-bridge-debug.log`.
   Fix: extend the sanitize gate to fail `make ci` on tracked junk, not just release copies.

## 3. Scoring methodology

Two rubrics. Scores are 0–10 per dimension, weighted, totalled to /10. Every dimension
score MUST cite evidence (a command run today, or file:line). No evidence, no score.

### 3.1 Feature rubric (user-visible journeys)

| Dimension | Weight | 2 means | 5 means | 8 means | 10 means |
| --- | --- | --- | --- | --- | --- |
| Functionality | 25 | Mostly missing | Works on happy path | Works incl. edge cases | Best-in-class, nothing comparable missing |
| Reliability & error handling | 25 | Fails silently | Fails loudly, sometimes hangs | Structured errors, always terminates | Fault-injection proven, self-recovering |
| User experience / usability | 15 | Manual surgery needed | Docs-driven, several steps | One command, good messages | Zero-config, self-explaining |
| Speed | 10 | Blocks or times out | Noticeable stalls | Fast enough not to notice | Measured, budgeted, regression-gated |
| Appearance / polish | 10 | Confusing raw output | Usable but rough | Clean output, clean docs | Indistinguishable from a first-party tool |
| Test evidence | 15 | Untested | Some tests, gaps known | Regression-pinned in `make ci` | CI + live proof + fault injection |

### 3.2 Component rubric (internal modules / dependencies — no UX/appearance)

| Dimension | Weight |
| --- | --- |
| Interface contract & depth (simple interface hiding real complexity; deletion test) | 25 |
| Reliability & error propagation | 25 |
| Code quality (size, duplication, single-responsibility) | 20 |
| Test evidence | 20 |
| Performance | 10 |

### 3.3 Rules

- A feature's score is capped by its weakest **critical** dependency: a feature cannot
  score above (weakest critical component + 2).
- "Live-proven" is a separate flag, not a score bonus. A feature relying only on
  fake-agent evidence caps Reliability at 7.
- Matrix total = Σ(dimension × weight) / 100, one decimal.

## 4. Feature inventory, dependencies, and scores

Evidence base for this pass (2026-07-05): `make ci` run (red at
`test_generate_config.sh`), source greps cited inline, `git tag` (empty), archive docs,
110 test files, `wc -l` = 13,191 lines across `src/` + wrapper.

### 4.1 Features (journeys)

| ID | Feature | Journeys | Critical dependencies | Func | Rel | UX | Spd | Pol | Test | **Total** |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | Build & install from source | UJ-01,02 | Makefile, vendored cJSON, Python venv | 9 | 8 | 8 | 9 | 7 | 8 | **8.3** |
| A2 | Connect Codex (config gen / managed install / picker) | UJ-05..08 | C7 config_manager, ASAR patch doc | 7 | 7 | 5 | 9 | 6 | 7 | **6.8** |
| A3 | Chat turn round-trip (stream + non-stream) | UJ-09,10 | C1,C2,C3,C4,C6 | 8 | 8 | 7 | 6 | 7 | 9 | **7.7** |
| A4 | Cancel & mid-turn control plane | UJ-11 | C1,C6 | 7 | 7 | 6 | 8 | 7 | 8 | **7.1** |
| A5 | Reasoning-effort steering | UJ-12 | C8 wrapper (restart mechanism) | 6 | 6 | 4 | 3 | 6 | 7 | **5.6** |
| A6 | Per-conversation sessions (save/switch/restore) | UJ-13 | C6,C8 | 8 | 6 | 7 | 7 | 7 | 7 | **7.0** |
| A7 | Tool intent & policy gating | UJ-14 | C2, capability_router, tool_policy | 6 | 7 | 6 | 7 | 6 | 7 | **6.5** |
| A8 | Compact context | UJ-15 | C6,C8 | 6 | 6 | 6 | 7 | 6 | 7 | **6.3** |
| A9 | Diagnostics & analytics (doctor, /health, /debug/session, turn_metrics, analytics.py) | UJ-16 | main.c doctor, debug_trace, scripts/analytics.py | 8 | 7 | 7 | 8 | 7 | 7 | **7.4** |
| A10 | Security baseline (auth, host allowlist, loopback, redaction) | UJ-17 | C1 | 7 | 8 | 7 | 8 | 7 | 8 | **7.5** |
| A11 | **Real ds4 end-to-end** | UJ-04 | C4,C6,C8, ds4-agent binary | 4 | 3 | 4 | 5 | 5 | 3 | **3.8** |
| A12 | Release consumer & contributor workflow | UJ-19,20 + Docker | Makefile ci, ci.yml, make_release_copy.sh | 6 | 6 | 6 | 7 | 5 | 5 | **5.9** |

Score notes (facts, then judgment):

- **A2 = 6.8.** Picker requires a manual macOS ASAR patch (`docs/model_picker_setup.md`);
  UX 5. Generated names are correct and test-backed, but the generate-config test is the
  one currently red (hermeticity, not product). Manual picker proof never recorded.
- **A3 = 7.7.** Strongest feature. SSE lifecycle golden-tested, heartbeats, chunked
  encoding, 409-on-concurrent, `incomplete_details` on truncation. Reliability capped
  at 7-range: fake-agent evidence only (rule 3.3).
- **A5 = 5.6.** Effort change = full ds4 relaunch + model reload. Documented, honest —
  and slow (Speed 3). Live restart proof still missing. This is agent-architecture
  constrained; a 10 here means masking the pause well, not eliminating it.
- **A6 = 7.0.** Design is right (keyed off `previous_response_id`, workspace+session
  index, `session_incompatible_state` rejections). Reliability 6 because the one live
  run showed stale-context leak into a fresh prompt and the fix is fake-proven only.
- **A11 = 3.8.** The north star clause N3. Last live evidence (2026-06-26): `/v1/models`
  ok, `/health` 503, first turn answered with stale audit context, DELETE blocked.
  Fixes landed since, zero live re-runs since. Test 3: no repeatable live harness exists
  (`tests/test_real_agent_fixture.sh` was fixed for hygiene but not re-run).
- **A12 = 5.9.** Release copy script + verification checklist + issue template exist
  (good), but: no tag, checklist unchecked, `make ci` red today, tracked `<test>` junk
  file in root, `.codex-bridge-debug.log` littering the working tree.

### 4.2 Components

| ID | Component | Files | Contract | Rel | Code | Test | Perf | **Total** |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C1 | HTTP/SSE server | server.c (2,065 ln) | 7 | 7 | 5 | 8 | 7 | **6.8** |
| C2 | Request parse & tool normalize | codex_request_parser.c, codex_tool_normalizer.c, codex_tool_detector.c | 8 | 8 | 7 | 8 | 8 | **7.8** |
| C3 | Response / stream formatting | codex_response_formatter.c, codex_stream_events.c, responses_stream_state.c, responses_api.c | 8 | 8 | 7 | 8 | 8 | **7.8** |
| C4 | Native transport & frame protocol | native_frame.c, native_connection.c, uds_transport.c | 8 | 8 | 8 | 8 | 8 | **8.0** |
| C5 | Engine / process lifecycle | bridge_core.c (1,179 ln) | 7 | 7 | 6 | 7 | 7 | **6.8** |
| C6 | Turn lifecycle & sessions | turn_context.c | 8 | 7 | 7 | 8 | 7 | **7.5** |
| C7 | Config & managed Codex config | config_manager.c (1,637 ln) | 8 | 7 | 6 | 8 | 8 | **7.4** |
| C8 | ds4 wrapper | agent/ds4_wrapper.py (1,263 ln) | 7 | 6 | 6 | 7 | 6 | **6.5** |
| C9 | Test harness & fakes | tests/ (110 files), fake_agent.py, fake_uds_agent.c | 7 | 6 | 6 | 7 | 6 | **6.5** |
| C10 | Build & vendored deps | Makefile, vendor/cjson, zlib | 8 | 8 | 7 | 7 | 8 | **7.6** |
| C11 | Dead modules | plugin_registry.c, plugin_example.c, structural_overlay.c, file_monitor_expanded.c, ring_buffer.c | 2 | — | — | — | — | **2.0** |

Component notes:

- **C1 code quality 5.** server.c is 2,065 lines mixing HTTP plumbing (chunked parse,
  header scan, socket I/O) with routing and response orchestration. The 2026-06 extract
  of `handle_response()` helped; the file is still the highest-friction seam. Deletion
  test on the HTTP-plumbing half: extracting it would concentrate complexity → real seam.
- **C4 = 8.0, best module.** Deadline-aware reads, structured truncation errors
  (`truncated_header`/`truncated_payload`), bounded UDS connect, FD_CLOEXEC. This is
  what "deep" looks like here: small interface (`frame_read`/`frame_write` + deadline),
  hard problems hidden.
- **C8 reliability 6.** The wrapper leans on text heuristics against ds4 stdout:
  `_parse_saved_session_id` token-scoring (`agent/ds4_wrapper.py:310`),
  `_is_plan_only_output` (`:195`), `_looks_like_complete_summary` (`:233`),
  `_is_tool_transcript_line` (`:99`). Any ds4 output-format drift breaks session
  save/switch or transcript filtering silently. Highest-risk single component for N3.
- **C9 reliability 6.** Proven today: `test_generate_config.sh` is not hermetic
  (reads developer `config.json`). Audit all 110 tests for the same class of leak
  (`--no-config` / env / port assumptions).
- **C11 = 2.** `plugin_registry.c`, `plugin_example.c`, `structural_overlay.c`,
  `file_monitor_expanded.c`, `ring_buffer.c` are in the Makefile `SRC` and compile into
  every binary; `plugin_registry_`/`overlay_` symbols are referenced nowhere in main.c,
  server.c, bridge_core.c, turn_context.c, or tool_runner.c. Deletion test: deleting
  them moves nothing — pure dead mass against N1. Cut them (keep in git history).

### 4.3 External dependencies / inputs

| Input | Feeds | Risk | Note |
| --- | --- | --- | --- |
| ds4-agent binary (+ model file) | A11, A5, A6, A8 | High | Not versioned by us; wrapper heuristics (C8) couple us to its stdout format. Pin a known-good ds4 version in docs + doctor check. |
| Codex Desktop (Responses API dialect, ASAR internals) | A2, A3 | Medium | App updates can break picker patch silently. Contract tests (WS4) reduce blast radius. |
| Python 3 venv | C8, tests | Low | Setup script handles fallbacks. |
| vendored cJSON, zlib | all C | Low | MIT, vendored, stable. |
| Gitea remote / GitHub sister repo | A12 | Low | Manual-copy publish flow already scripted. |

### 4.4 Overall north-star score

Weighted by north-star relevance: A11 ×25, A3 ×15, A2 ×10, A6 ×10, A12 ×7.5, A1 ×5,
A4 ×5, A5 ×5, A7 ×5, A9 ×5, A10 ×5, A8 ×2.5.

**Total: 6.2 / 10.** Remove the A11 drag (3.8 → 8) and the same weights give ≈ 7.3;
everything else is incremental. The real agent is the plan.

## 5. What 10/10 means (best-in-class definition)

Competitors for "local model behind an OpenAI-compatible endpoint": Ollama, LM Studio,
llama.cpp `llama-server`, LiteLLM proxy. None of them bridge a *stateful native agent*
(sessions, tool intents, effort steering) — that is Star Bridge's differentiation.
Best-in-class therefore means:

1. **Setup**: one command from clone to Codex picker entry (Ollama-grade onboarding).
2. **Contract**: a written, versioned spec of the supported Responses API subset that a
   harness author can implement against without reading C. No competitor documents
   agent-session semantics over this API; we would be first.
3. **Reliability**: every failure mode produces a structured, documented error within a
   bounded time; fault-injection suite proves it. No hangs, ever.
4. **Transparency**: turn_metrics + analytics give per-effort latency/throughput out of
   the box (already ahead of competitors here).
5. **Proof**: live smoke green on every release, in CI-recorded artifacts.

## 6. Improvement plan (phased, outcome-gated)

Each phase gates on ONE binary outcome with a named evidence artifact. No phase starts
until the previous gate's artifact exists. Task IDs `SB-xx` are stable for cross-agent
reference.

### Phase 0 — Restore trustworthy green (target: days)

Gate: `make ci` green on a fresh clone with no `config.json`, AND with a hostile
`config.json` present. Evidence: `docs/evidence/P0_ci_green.txt` (command + output + date).

- **SB-01** Fix `tests/test_generate_config.sh`: pass `--no-config` (or set an explicit
  isolated config), assert against the flag-driven port. Root cause, not the assert.
- **SB-02** Hermeticity audit of all 110 tests: grep for missing `--no-config`, absolute
  ports, `$HOME`, personal paths. Fix every leak. Add a meta-test that runs the suite
  with a poisoned `config.json` in cwd.
- **SB-03** Extend sanitize gate into `make ci`: fail on tracked non-source junk. Remove
  the tracked `<test>` file from the index; add debug-log patterns to `.gitignore`
  check.
- **SB-04** Delete dead modules (C11) from `SRC` and `src/` (plugin_registry,
  plugin_example, structural_overlay, file_monitor_expanded, ring_buffer if unused after
  check). `make ci` green after. Raises N1; shrinks binary and cognitive surface.
- **SB-05** Verify `ci.yml` actually runs `make ci` on push in Gitea/GitHub CI, and that
  a red gate is visible. A gate nobody watches is not a gate — this is the direct fix
  for shortfall #1.

### Phase 1 — Prove the north star live (target: 1–2 weeks)

Gate: scripted real-ds4 smoke green, twice on different days. Evidence:
`docs/evidence/P1_live_smoke_{date}.log` ×2.

- **SB-10** Build `scripts/live_smoke.sh`: manual-safe (refuses to disturb an active ds4
  unless `--force`), runs: fresh first turn (prompt-specific answer, no stale context),
  `/health` 200 with `native_status:ok`, continued turn, save/switch A-B-A across two
  simulated conversations, effort change restart + session restore, `DELETE` mid-turn
  (`response.failed` ≤2s), concurrent POST → 409, context-token override honored.
  Each check prints PASS/FAIL; log is the artifact.
- **SB-11** Fix whatever SB-10 finds. Historical live failures to specifically re-test:
  health 503 after models-ok; stale session leak into fresh prompt; DELETE blocking.
- **SB-12** Pin the ds4 version tested against in `--doctor` output and README
  ("validated against ds4 <version/commit>"). Doctor warns on mismatch.
- **SB-13** Tag `v0.1.0-alpha` once `RELEASE_VERIFICATION.md` is fully checked,
  including the manual Codex picker proof (record it this time — screenshot or terse
  log note in the checklist).

### Phase 2 — Error contract & second harness (N2 + N4) (target: 2–3 weeks)

Gate: fault-injection suite green + second-harness suite green. Evidence:
`docs/evidence/P2_faults.log`, `docs/evidence/P2_second_harness.log`.

- **SB-20** Write `docs/HARNESS_CONTRACT.md`: the exact Responses API subset served —
  endpoints, SSE event order (golden transcript), error taxonomy (every `code` the
  bridge can emit, when, and what the harness should do), limits/`incomplete_details`
  semantics, session semantics via `previous_response_id`, auth. This is the N4
  deliverable and the doc a Codex-alternative author reads.
- **SB-21** Fault-injection suite (extends fake agents): agent killed mid-turn; agent
  emits garbage frame; agent stalls past deadline; agent closes socket after ack; client
  disconnects mid-stream; oversized frame. Invariant under all: a terminal SSE event or
  HTTP error within bounded time, correct `turn_metrics` line, bridge accepts the next
  request. No hang, no silent success.
- **SB-22** Second-harness proof: a test client using the plain `openai` Python SDK
  (Responses API) against the bridge — non-stream, stream, error, cancel. Proves "or
  another harness" with zero Codex-specific tolerance.
- **SB-23** Wrapper hardening (C8): where ds4 offers structured output, replace stdout
  heuristics; where it doesn't, fence each heuristic with a regression test using
  captured real ds4 transcripts (fixtures), so format drift breaks tests, not sessions.

### Phase 3 — Deepen and de-friction (N1) (target: 3–4 weeks)

Gate: `make ci` green throughout; server.c below ~1,200 lines with behavior pinned by
existing golden tests. Evidence: `docs/evidence/P3_refactor.txt` (before/after `wc -l`,
test run).

- **SB-30** Extract HTTP plumbing from server.c into `http_io.c` (request read, chunked
  decode, header parse, `send_http`/`send_sse`). Interface: read request / write
  response+events. Behavior identical; golden stream tests are the safety net. This is
  the one real deepening seam found; do NOT split further without new friction evidence.
- **SB-31** config_manager.c: separate runtime-config load/validate from managed-Codex
  install/catalog authoring (two concerns, 1,637 lines). Only if SB-30 goes cleanly;
  otherwise defer — refactor appetite is capped at two extractions this phase.
- **SB-32** UX: `--install` prints a numbered "what happens next" (restart Codex, pick
  model, first-turn model-load wait). Effort-restart pause: emit an SSE notice
  ("reloading model at <effort>…") so Codex shows progress instead of silence (A5's
  path from 5.6 toward 8: mask the pause honestly).
- **SB-33** Picker setup: fold the ASAR patch steps into `--doctor`-style preflight +
  a single `scripts/setup_picker.sh` with dry-run. Manual patch stays documented as
  fallback.

### Phase 4 — Security posture & release cadence (target: ongoing)

Gate: threat-model doc + clean redaction audit. Evidence: `docs/SECURITY.md`,
`docs/evidence/P4_redaction.log`.

- **SB-40** `docs/SECURITY.md`: trust boundary (localhost dev tool), threat model
  (who can hit the port, what the agent can do), auth-token guidance; generate a random
  `auth_token` on `--install` by default.
- **SB-41** Redaction audit: `/debug/session` and debug traces vs tokens, prompts,
  personal paths — scripted grep against a live capture, kept as a test.
- **SB-42** Release cadence: every tag reruns §7's full matrix; RELEASE_VERIFICATION
  gets a "verified on <date> by <agent/human>" line per section. Sister-repo GitHub sync
  happens only after a tag, by `make_release_copy.sh`, never by direct push (per repo
  policy — do not touch GitHub repo until instructed).

### Score targets by phase

| Feature | Now | After P0 | P1 | P2 | P3 | P4 (ceiling) |
| --- | --- | --- | --- | --- | --- | --- |
| A11 real ds4 | 3.8 | 3.8 | **7.5** | 8.5 | 8.5 | 9.5 |
| A3 turn round-trip | 7.7 | 7.7 | 8.2 | **9.0** | 9.2 | 9.5 |
| A12 release/contrib | 5.9 | **7.5** | 8.0 | 8.2 | 8.5 | 9.0 |
| A2 Codex connect | 6.8 | 7.0 | 7.2 | 7.2 | **8.5** | 9.0 |
| A5 effort steering | 5.6 | 5.6 | 6.5 | 6.5 | **8.0** | 8.5 |
| A6 sessions | 7.0 | 7.0 | **8.0** | 8.5 | 8.5 | 9.5 |
| C1 server | 6.8 | 6.8 | 6.8 | 7.2 | **8.5** | 9.0 |
| C8 wrapper | 6.5 | 6.5 | 7.0 | **8.0** | 8.0 | 9.0 |
| C9 tests | 6.5 | **8.0** | 8.5 | 9.0 | 9.0 | 9.5 |
| **Overall (weighted)** | **6.2** | 6.6 | **7.7** | 8.2 | 8.6 | ~9.2 |

Ceilings below 10 are honest: A5 is bounded by ds4's restart architecture; A2 by
Codex's closed picker. A 10 overall requires upstream changes we don't control.

## 7. Verification methodology (for implementing agents)

Run before claiming any SB task done:

1. `make` — clean build, zero new warnings.
2. `make ci` — green, from a state with a poisoned `config.json` in cwd (after SB-02).
3. The task's own acceptance evidence (each SB task names its artifact/behavior).
4. For anything touching turn/stream paths: `tests/test_golden_fixtures.sh` and the
   stream-lifecycle golden test must pass unmodified — if you had to edit a golden
   fixture, say so explicitly in the commit message and why.
5. For anything touching wrapper/session paths: `tests/test_ds4_wrapper.py` +
   `tests/test_ds4_wrapper_integration.sh`, and after Phase 1 exists,
   `scripts/live_smoke.sh` (or record why live wasn't run).
6. Write the evidence file under `docs/evidence/` named per the phase convention.
   A claim without an evidence file does not count as done.
7. Update this doc's score tables ONLY via the re-scoring protocol (§8), never ad hoc.

Working rules (carried from what worked before): red test first for every behavior fix;
smallest robust change; no refactor from a red baseline; commit messages reference SB-xx.

## 8. Re-scoring protocol (for measuring agents)

Any agent can reproduce the §4 scores. Protocol:

1. **Collect evidence, fresh.** Run: `make ci` (record red/green + failing test),
   `git tag`, `git status --short` (litter check), `wc -l src/*.c agent/ds4_wrapper.py`,
   dead-symbol grep (Makefile `SRC` vs references), and read the latest files under
   `docs/evidence/`. Do NOT reuse this document's evidence — it is dated 2026-07-05.
2. **Score dimensions against the §3.1/§3.2 anchor tables.** Use the anchors, not vibes:
   e.g. Reliability 8 requires "structured errors, always terminates" with test evidence;
   cap at 7 without live proof (rule 3.3).
3. **Apply the caps**: weakest-critical-dependency cap; fake-only reliability cap.
4. **Compute**: total = Σ(dim × weight)/100, one decimal; overall = §4.4 weights.
5. **Record**: append a dated row to the table below, with a one-line evidence pointer
   per changed score. Never overwrite prior rows — the trend is the point.
6. **Disagreement rule**: if your score differs from the previous row by ≥2 on any
   dimension without a code change explaining it, the previous row was mis-scored;
   annotate which and why.

### Score history

| Date | Overall | A11 | A3 | A12 | make ci | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| 2026-07-05 | 6.2 | 3.8 | 7.7 | 5.9 | RED (test_generate_config hermeticity) | Baseline, this doc |
| 2026-07-06 | 6.6 | 3.8 | 7.7 | 7.5 | GREEN | P0 restored: `docs/evidence/P0_ci_green_2026-07-06.txt`; live ds4 proof still missing, so A11 unchanged |
| 2026-07-06 | 7.1 | 7.0 | 7.7 | 7.5 | GREEN | First scripted real-ds4 smoke green: `docs/evidence/P1_live_smoke_2026-07-06.log`; Phase 1 gate still needs second different-day pass |
| 2026-07-06 | 7.5 | 7.5 | 8.2 | 7.5 | GREEN | P1 two-day live gate met: `docs/evidence/P1_live_smoke_2026-07-05.log` and `docs/evidence/P1_live_smoke_2026-07-06.log`; A12 held at P0 score until release tag/checklist |
| 2026-07-06 | 8.0 | 8.5 | 9.0 | 8.2 | GREEN | P2 gate met: `docs/HARNESS_CONTRACT.md`, `docs/evidence/P2_faults.log`, `docs/evidence/P2_second_harness.log`, and `docs/evidence/P2_wrapper_fixtures.log`; conservative overall keeps A2/A5/A7 below later-phase targets |

## 9. Risks

- **ds4 output-format drift** breaks wrapper heuristics silently → SB-23 fixtures are
  the mitigation; until then treat any session-restore weirdness as suspect #1.
- **Codex Desktop update** breaks ASAR picker patch → contract tests (SB-20/22) keep the
  API-level path provable even if the picker path breaks.
- **Live smoke needs a free machine/model** — the historical blocker. Mitigation:
  SB-10 is manual-safe and cheap to rerun; schedule it, don't wait for "a good moment".
- **Refactor appetite** — Phase 3 is capped at two extractions. History shows this
  codebase's failure mode is planning, not code rot.

## 10. Out of scope

- GitHub sister repo (https://github.com/chriskzy/star_bridge): no pushes, no edits.
  Migration = manual copy via `scripts/make_release_copy.sh` after a local tag, on
  explicit instruction only.
- Bridge-owned browser/search tooling (decided cut; ds4 browses natively).
- Windows/WSL, hosted routing, multi-agent orchestration, plugin API (deleted in SB-04;
  revisit only when a second native agent actually exists — "one adapter = hypothetical
  seam").
