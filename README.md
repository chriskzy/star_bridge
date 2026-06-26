# Star Bridge

Local-first bridge for running a ds4 native agent from the Codex desktop app.

Star Bridge is a small native gateway that exposes the OpenAI Responses API shape Codex expects, routes requests into a local ds4 runtime, and keeps Codex compatibility, native-agent transport, logging, and optional tool handling in one inspectable C codebase. The primary binary is `bin/star_bridge` (a `bin/codex_bridge` compatibility symlink is kept for one alpha).

> Status: alpha. The bridge builds and has a growing regression suite, but the real ds4-agent workflow still needs wider validation before a stable public release.

## What It Does

- Serves local Codex-compatible `/v1/responses` and `/v1/models` endpoints.
- Adapts Codex requests into a native-agent frame protocol.
- Supports stdio framed transport and Unix domain socket transport.
- Auto-detects `ds4-agent` and uses the Python wrapper for ds4 non-interactive mode.
- Streams assistant text, structured errors, and completion events back to Codex.
- Supports session state frames and workspace-root injection.
- Provides policy-gated tool intent handling; browser/search execution stays with the native agent.
- Includes request validation, auth token checks, logging, debug traces, and regression tests.

## Repository Layout

```text
.
├── include/                 Public headers for bridge modules
├── src/                     C implementation
├── tests/                   Shell, C, and Python regression tests
├── tools/                   Optional Node.js tool driver
├── docs/                    Codex Desktop setup notes
├── config.example.json      Example local runtime config
├── Makefile                 Build and test targets
├── Dockerfile
├── docker-compose.yml
└── README.md
```

## Requirements

- macOS or Linux with a C11 compiler, `make`, and `zlib`.
- Python 3.14 preferred for the wrapper virtualenv; the setup script falls back to available Python where possible.
- DwarfStar/ds4 `ds4-agent` installed locally for real-agent use.
- Node.js is not required — browser/search tooling is not supported by the bridge; the agent handles browsing natively.
- Codex desktop app configured with a local custom provider.

The build vendors cJSON in `vendor/cjson/` when no system cJSON is available. cJSON is
© 2009–2017 Dave Gamble and cJSON contributors, MIT-licensed; see
[`vendor/cjson/LICENSE`](vendor/cjson/LICENSE).

## Build

```sh
make
```

Clean generated binaries and test logs:

```sh
make clean
```

Run the curated regression suite:

```sh
make ci
```

Run the full suite:

```sh
make test
```

## Run

Start the bridge with a native agent and workspace:

```sh
./bin/star_bridge /path/to/ds4-agent /path/to/workspace -p 9033 --turn-response-timeout-ms 120000
```

Then point Codex at:

```text
http://127.0.0.1:9033/v1
```

Use model:

```text
star-bridge-ds4
```

The bridge auto-detects a `ds4-agent` binary, launches the wrapper in `agent/ds4_wrapper.py`, and runs ds4 in persistent non-interactive mode behind the bridge protocol. For a custom framed agent, pass its script or binary directly:

```sh
./bin/star_bridge /path/to/native-agent-wrapper /path/to/workspace -p 9033 --framed
```

### CLI reference

Every flag the binary accepts (`./bin/star_bridge --help`) is documented here:

```text
-p <port>                         HTTP server port
--framed                          Enable framed stdio transport
--native-transport <mode>         Transport mode: auto|uds|stdio_framed|stdio
--native-socket-path <path>       Unix domain socket path
--uds-connect-timeout-ms <ms>     UDS connect timeout
--uds-owner-mode <mode>           UDS socket owner/permission mode
--hello-timeout-ms <ms>           Native hello/handshake timeout
--model-load-timeout-ms <ms>      Native model-load timeout
--turn-response-timeout-ms <ms>   Per-turn native response timeout
--model-reasoning-effort <val>    Default reasoning effort: low|medium|high
--max-output-buffer <bytes>       Max output buffer size (default 262144)
--max-output-chars <chars>        Max non-streamed output chars (0 = unlimited)
--max-turn-events <n>             Max native frames per turn before partial completion (default 65536)
--no-config                       Skip loading config.json
--generate-config [output_dir]    Write Codex config artifacts and exit
--install                         Install managed Codex config block
--disable                         Remove managed config block and restore displaced keys
--status                          Show managed config install status
--dry-run                         With --install/--disable, show changes without writing
--doctor [agent]                  Run one-shot diagnostics and exit (nonzero on first failure)
--version                         Print version and exit
--help                            Print help and exit
```

## Codex Configuration

You can generate local Codex config artifacts:

```sh
./bin/star_bridge /path/to/ds4-agent /path/to/workspace -p 9033 --generate-config ./codex-config
```

Or install a managed block into `~/.codex/config.toml`:

```sh
./bin/star_bridge /path/to/ds4-agent /path/to/workspace -p 9033 --install
```

To make the local model appear in the Codex Desktop picker (catalog + the
macOS-only ASAR app patch), see [docs/model_picker_setup.md](docs/model_picker_setup.md).

Manual profile example:

```toml
model = "star-bridge-ds4"
model_provider = "star-bridge-local"

[model_providers.star-bridge-local]
name = "Star Bridge Local"
base_url = "http://127.0.0.1:9033/v1"
wire_api = "responses"
```

## Behavior notes

Non-obvious but intentional behavior (see [ARCHITECTURE.md](ARCHITECTURE.md) for internals):

- **Reasoning effort restarts the agent.** Changing reasoning effort mid-session saves the ds4
  session, relaunches ds4 with the new think flag, and reloads the session. Expect a pause of
  model-load duration. This is documented behavior, not a hang.
- **Per-conversation sessions.** Each Codex conversation (keyed off `previous_response_id`) maps to
  its own ds4 session; switching threads in Codex saves the old session and switches to the target,
  so context does not bleed between threads.
- **No silent truncation.** Oversized requests are rejected loudly (HTTP 413 or a 400
  `request_input_too_large`), never clipped silently. When output is clipped by `max_output_buffer` /
  `max_output_chars`, or a turn hits `max_turn_events`, the response carries `incomplete_details` so
  Codex shows the truncation instead of hiding it.
- **Streaming keepalive.** Long silent turns send SSE heartbeats so Codex does not "Reconnect".
- **Mid-turn cancel is limited by the agent.** `DELETE /v1/responses/<id>` during a turn ends the
  stream with `response.failed` within ~2s and sends a best-effort cancel to ds4, but ds4 may finish
  its current compute in the background. `GET /v1/models` is answered during a turn; a concurrent
  `POST /v1/responses` returns `409`.

## Configuration

Copy `config.example.json` to `config.json` for local overrides. `config.json` is ignored by git.

Important settings:

```json
{
  "port": 9033,
  "codex_model": "star-bridge-ds4",
  "use_framed_protocol": true,
  "auto_load_resume_session": true,
  "context_tokens": 150000,
  "trace": true,
  "debug_log": true,
  "debug_log_path": ".codex-bridge-debug.log",
  "hide_tool_transcripts": true
}
```

Config priority is CLI flags, environment variables, `config.json`, then built-in defaults.

## Output Buffer Configuration

The bridge supports configurable output buffer limits to prevent display truncation in Codex Desktop:

### `max_output_buffer` (bytes, default: 262144)
Controls the size of internal output buffers used for agent responses. Set higher if agent outputs exceed 256 KB. Affects buffers in:
- `handle_response_block` (non-streaming text responses)
- `handle_response_stream` (streaming text responses)
- `handle_compact` (compact response mode)
- `handle_response` (response endpoint)
- JSON serialization buffer

### `max_output_chars` (chars, default: 0 = unlimited)
When set to a positive value, non-streaming responses (`handle_response_block`) are truncated to that many characters before being sent to Codex. This allows limiting output size for the Codex Desktop summary display. Truncation is logged to stderr and the debug trace.

### CLI flags
- `--max-output-buffer <bytes>` — override output buffer size
- `--max-output-chars <chars>` — set output character limit

### Config file fields
```json
{
  "max_output_buffer": 262144,
  "max_output_chars": 0
}
```

## Optional Tooling

Browser and web search tooling is not supported by the bridge. The agent handles browsing natively via its own tool capabilities. If you need a browser workflow, use the agent's native browsing rather than bridge-owned Playwright or search API.

## Debugging

Useful artifacts when reproducing issues:

- Bridge stdout/stderr.
- `.codex-bridge-debug.log` from the working directory.
- ds4 trace logs, if enabled through ds4 options or wrapper environment.
- Exact `./bin/star_bridge ...` command line.

Common issues:

- Wrong workspace path: pass an explicit full workspace path as the second argument.
- Native agent never sends hello/ready: use the ds4 wrapper or a framed-compatible agent.
- Long model/tool turn times: raise `--turn-response-timeout-ms`.
- Stale local processes or sockets: run `scripts/cleanup_stale_bridges.sh`.

## Security Notes

This is a local developer bridge with access to agent, shell, browser, and workspace operations. Treat it like a trusted local dev tool, not a remote service.

- Bind to `127.0.0.1` for normal use.
- Do not expose the bridge port to untrusted networks.
- Use `auth_token` before any non-loopback experiment.
- Keep API keys in environment variables or ignored local config.
- Review tool policies before enabling shell/browser workflows.
- Avoid committing local paths, model paths, trace logs, or debug logs.

The bridge sets loopback proxy bypass variables for child processes when they are not already present:

```text
NO_PROXY=127.0.0.1,localhost,::1
no_proxy=127.0.0.1,localhost,::1
```

## License

MIT. See [LICENSE](LICENSE).
