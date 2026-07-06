#!/usr/bin/env python3
"""
Wrapper: bridge framed protocol <-> persistent ds4-agent stdin mode.
"""

import sys
import struct
import json
import re
import subprocess
import os
import pty
import select
import time
import queue
import threading
import socket  # for UDS support in bridge<->wrapper framed comms
import hashlib

_ANSI_ESCAPE_RE = re.compile(r"\x1b\[[0-9;]*m")
DS4_AGENT_PATH = os.environ.get("DS4_AGENT_PATH")
if not DS4_AGENT_PATH:
    sys.stderr.write("ERROR: DS4_AGENT_PATH environment variable must be set to the ds4-agent binary path\n")
    sys.exit(1)
DS4_AGENT = DS4_AGENT_PATH

MODEL_PATH = os.environ.get("DS4_MODEL_PATH")
if not MODEL_PATH:
    sys.stderr.write("ERROR: DS4_MODEL_PATH environment variable must be set to the ds4 model file path\n")
    sys.exit(1)
_CONTEXT_TOKENS_ENV = os.environ.get("DS4_CONTEXT_TOKENS")
CONTEXT_TOKENS = int(_CONTEXT_TOKENS_ENV or "150000")
TRACE_DIR = "/tmp/ds4_traces"
WAITING_MARKER = "+DWARFSTAR_WAITING"
DS4_STARTUP_TIMEOUT = int(os.environ.get("DS4_STARTUP_TIMEOUT", "300"))
DS4_TURN_TIMEOUT = int(os.environ.get("DS4_TURN_TIMEOUT", "600"))

# Captured from bridge hello; used to prefix prompts so ds4 uses the right cwd for tools.
BRIDGE_WORKSPACE_ROOT = "."

# UDS support for end-to-end between bridge and wrapper (for real ds4 "agent to bridge" UDS).
# When DS4_BRIDGE_UDS_SOCKET set (by bridge in launch_and_connect uds mode), wrapper
# binds as server, accepts bridge, uses the socket for framed protocol instead of stdio.
# This finishes UDS without changing the pty wrapper for the real ds4 binary inside.
BRIDGE_UDS_SOCKET = os.environ.get("DS4_BRIDGE_UDS_SOCKET")
_bridge_comm = None
_uds_server = None

# Session tracking for per-Codex-conversation session mapping (T2.2)
_session_map = {}         # conversation_key → {"sha": str, "project_root": str, "model_id": str}
_active_session_key = ""  # current active session key

# Reasoning effort tracking (T2.1)
_current_reasoning_effort = None  # current effort level for the active session


def ds4_runtime_home():
    """Bridge-owned HOME for ds4 so global ~/.ds4 sessions never leak in."""
    override = os.environ.get("STAR_BRIDGE_DS4_HOME")
    if override:
        return override
    parent_home = os.environ.get("HOME") or "/tmp"
    ws = os.path.abspath(BRIDGE_WORKSPACE_ROOT or ".")
    digest = hashlib.sha256(ws.encode("utf-8")).hexdigest()[:16]
    return os.path.join(parent_home, ".star_bridge", "ds4-home", digest)

def _setup_bridge_comm():
    """Setup UDS or stdio for bridge framed comms. Call early in main before first read_frame.
    For UDS (launch_and_connect): wrapper binds/listens/accepts (acts as server), bridge connects.
    """
    global _bridge_comm, _uds_server
    if BRIDGE_UDS_SOCKET:
        try:
            if os.path.exists(BRIDGE_UDS_SOCKET):
                os.unlink(BRIDGE_UDS_SOCKET)
        except OSError:
            pass
        _uds_server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        _uds_server.bind(BRIDGE_UDS_SOCKET)
        _uds_server.listen(1)
        sys.stderr.write(f"DEBUG ds4 wrapper binding UDS for bridge at {BRIDGE_UDS_SOCKET}\n")
        sys.stderr.flush()
        _bridge_comm, _ = _uds_server.accept()
        sys.stderr.write(f"DEBUG ds4 wrapper accepted UDS bridge connection\n")
        sys.stderr.flush()
    else:
        _bridge_comm = None

def _recv_all(sock, n):
    data = b""
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data

def _send_all(sock, data):
    sock.sendall(data)

# Tool transcript filter: the agent's internal "🛠 list/find/..." lines (from rtk/caveman skills etc.)
# pollute the visible assistant output in Codex UI. Make optional via env (default: hide).
DS4_SHOW_TOOL_LINES = os.environ.get("DS4_SHOW_TOOL_LINES", "0").lower() in ("1", "true", "yes", "on")
_TOOL_LINE_RE = re.compile(
    r'^\s*(🛠️|🛠|\[tool|tool\s*call|executing|running tool)|'
    r'list\s+path=|^\s*\$\s*(ls|find|cat|grep|read|write|cd|pwd)|'
    r'^\s*(find|list|read|write)\s+.*path=',
    re.IGNORECASE
)

def _is_tool_transcript_line(text):
    if not text:
        return False
    t = text.strip()
    if not t:
        return False
    if _TOOL_LINE_RE.search(t):
        return True
    if t.startswith("🛠"):
        return True
    # Common rtk-style visible command echoes that leak into final answer
    if "🛠" in t or ("path=" in t and any(k in t.lower() for k in ("list", "find", "read", "tool"))):
        return True
    return False

DS4_MAX_LISTING_LINES = int(os.environ.get("DS4_MAX_LISTING_LINES", "100"))

def _looks_like_listing_line(line):
    l = (line or "").strip()
    if not l:
        return False
    low = l.lower()
    return (l.startswith(("drwx", "-rw", "total ", "ls:", "find:")) or
            any(x in l for x in ("/", ".sh", ".py", ".c", ".md", ".log", ".sock", ".json")) and len(l.split()) >= 2)

def _compact_dir_listings(lines):
    """Compact long directory/file listings to avoid Codex output limits (~2099 lines UI?) and improve readability.
    Keeps sample, reports count. Also emits compaction event for bridge to surface notice.
    Used on final collected parts (live deltas may still show burst but final is compact).
    """
    if not lines or len(lines) <= DS4_MAX_LISTING_LINES:
        return lines
    compacted = []
    i = 0
    n = len(lines)
    while i < n:
        j = i
        while j < n and _looks_like_listing_line(lines[j]):
            j += 1
        burst = j - i
        if burst > 15:  # significant listing burst
            first = lines[i:i+5]
            last = lines[j-3:j] if burst > 8 else []
            sample = first + (["..."] if last else []) + last
            summary = f"[compacted listing: {burst} entries; e.g. {'; '.join(s for s in sample if s)} ]"
            compacted.append(summary)
            # emit so bridge can turn into compaction notice (like native)
            try:
                write_frame({
                    "type": "event",
                    "event": "compaction.summary",
                    "message": f"Compacted large directory listing ({burst} lines) for output limits/readability."
                })
            except Exception:
                pass
            i = j
        else:
            compacted.extend(lines[i:j])
            i = j
    return compacted


# --- Plan-only vs. complete-summary detection ---
# Some ds4 turns stop after a planning sentence and return +DWARFSTAR_WAITING before
# producing the requested synthesis. The continuation loop keeps the same logical
# Codex response open, reuses the same ack id for additional text deltas, and emits
# the terminal response frame only after a complete answer or a bounded fallback.

PLAN_PHRASES = (
    "let me explore",
    "let me first explore",
    "let me explore it thoroughly",
    "let me explore the project more deeply",
    "now let me read key files",
    "let me start by exploring",
    "i've already done a broad listing",
    "now let me read",
    "the user wants me to review",
)

SYNTHESIS_MARKERS = (
    "intent of the project",
    "intent of the folder",
    "current state (",  # structured section form e.g. "Current state (structure, key files..."
    "effort:",
    "benefit:",
    "effort (low/medium/high)",
    "benefit (low/medium/high)",
    "review summary",
    "potential improvements",
    # Also accept common score renderings the completion_directive requests.
    "effort: low", "effort: medium", "effort: high",
    "benefit: low", "benefit: medium", "benefit: high",
)


def _is_plan_only_output(text: str) -> bool:
    """Heuristic: does the collected (visible, filtered) output look like only a plan
    announcement was emitted before the native hit WAITING, without the required
    structured synthesis (sections + Effort/Benefit scores) that the completion_directive
    and user review prompt demand?
    Used after each WAITING (in the post-stream loop) to decide whether to internally
    continue the *same* logical Codex request on the persistent session.
    """
    if not text:
        return True
    t = _strip_ansi(text).lower()
    stripped = t.strip()
    if not stripped:
        return True

    has_plan_language = any(p in t for p in PLAN_PHRASES)
    has_synthesis = any(m in t for m in SYNTHESIS_MARKERS)

    # Strong plan-only signal: plan phrasing present with no synthesis yet.
    if has_plan_language and not has_synthesis:
        return True

    # Short "review the X folder /path, its intent... Let me explore..." announcement
    # (as seen in the prompt paraphrase + first visible agent sentences) with very little substance.
    if ("review the" in t or "review a" in t or "the user wants me to review" in t) and \
       ("folder" in t or "directory" in t or "repository" in t or "project" in t):
        lines = [ln for ln in stripped.splitlines() if ln.strip()]
        if len(lines) <= 6 and not has_synthesis:
            return True

    # If we have synthesis markers, treat as complete (even if some plan text remains from
    # earlier deltas in the accumulation). This is the positive stop signal.
    if has_synthesis:
        return False

    return False


def _looks_like_complete_summary(text: str) -> bool:
    """Positive heuristic that the output now contains the required review synthesis.
    Used to drive aggressive continuation for review-style tasks until we have scores/sections
    (or hit cap), rather than only relying on the absence of plan language.
    """
    if not text:
        return False
    t = _strip_ansi(text).lower()
    if any(m in t for m in SYNTHESIS_MARKERS):
        return True
    # Long non-plan output is a reasonable proxy for "it did the work and synthesized".
    if not _is_plan_only_output(text) and len(text) > 400:
        return True
    return False


def ds4_agent_home():
    """Directory ds4-agent must run from (Metal assets live here)."""
    return os.path.dirname(os.path.abspath(DS4_AGENT))


def _preflight_ds4_or_raise():
    """Fail fast with actionable message if ds4 binary/model/metal assets are missing.
    This improves 'no output' RCA (user sees exact missing piece instead of generic)."""
    problems = []
    if not DS4_AGENT or not os.path.isfile(DS4_AGENT) or not os.access(DS4_AGENT, os.X_OK):
        problems.append(f"DS4_AGENT not executable or missing: {DS4_AGENT}")
    if MODEL_PATH and (not os.path.isfile(MODEL_PATH) or not os.access(MODEL_PATH, os.R_OK)):
        problems.append(f"DS4_MODEL_PATH not readable: {MODEL_PATH}")
    home = ds4_agent_home()
    metal = os.path.join(home, "metal", "flash_attn.metal")
    if not os.path.isfile(metal) or not os.access(metal, os.R_OK):
        problems.append(f"metal/flash_attn.metal missing under ds4 home (needed for --chdir): {metal}")
    if problems:
        msg = "ds4 preflight failed: " + "; ".join(problems) + \
              " | Check DS4_AGENT_PATH/DS4_MODEL_PATH env, that ds4-agent dir contains metal/, and permissions."
        sys.stderr.write("DEBUG " + msg + "\n")
        sys.stderr.flush()
        raise RuntimeError(msg)


# Reasoning effort -> ds4-agent thinking LAUNCH flag (T2.1).
# ds4-agent exposes thinking only as a launch flag (verified: `ds4-agent --help sampling`):
#   --nothink     Disable thinking and ask for direct replies.
#   --think       Use normal thinking mode.  (ds4 default)
#   --think-max   Use Think Max when context is large enough.
# There is NO `--reasoning-effort` flag. Changing effort therefore requires an agent
# restart (see apply_effort_change). Single source of truth for the mapping:
THINK_FLAGS = {
    "low": "--nothink",
    "medium": "--think",
    "high": "--think-max",
}


def think_flag_for_effort(reasoning_effort):
    """Return the ds4 launch flag for an effort level, or None to use ds4's default.
    Unknown values map to None (we never pass an unrecognized flag to ds4)."""
    if not reasoning_effort:
        return None
    return THINK_FLAGS.get(reasoning_effort)


_SAVED_ID_RE = re.compile(r"\b([A-Za-z0-9][A-Za-z0-9._-]{3,})\b")


def _id_score(tok):
    """Higher = more id-like. Real kvcache ids carry a digit AND a separator (e.g.
    'kv-9f3a2b'); plain path/dictionary words ('kvcache') and short fragments ('ds4')
    score low. Returns 0 for non-ids."""
    has_digit = any(c.isdigit() for c in tok)
    if not has_digit:
        return 0
    has_sep = ("-" in tok) or ("_" in tok)
    return (2 if has_sep else 1) * 100 + len(tok)


def _parse_saved_session_id(text):
    """Extract a saved-session id from ds4 `/save` output (best-effort).
    Picks the most id-shaped token, preferring lines that mention 'save'. Returns None
    if nothing id-shaped is found.
    The exact `/save` line format is undocumented — VERIFY against a live ds4 `/save`."""
    if not text:
        return None
    best = (0, None)  # (score, token), with a save-line bonus
    for line in text.splitlines():
        bonus = 1000 if "save" in line.lower() else 0
        for tok in _SAVED_ID_RE.findall(line):
            score = _id_score(tok)
            if score == 0:
                continue
            total = bonus + score
            if total > best[0]:
                best = (total, tok)
    return best[1]


def build_ds4_persistent_args(model=MODEL_PATH, ctx=CONTEXT_TOKENS, reasoning_effort=None):
    """Argv for long-lived ds4-agent (stdin prompts, no -p).
    Maps reasoning_effort (low/medium/high) to the ds4 thinking launch flag.
    """
    home = ds4_agent_home()
    os.makedirs(TRACE_DIR, exist_ok=True)
    trace_file = os.path.join(TRACE_DIR, f"ds4_wrapper_{int(time.time())}.log")
    args = [
        DS4_AGENT,
        "--non-interactive",
        "--chdir", home,
        "-m", model,
        "--ctx", str(ctx),
        "--trace", trace_file,
    ]
    flag = think_flag_for_effort(reasoning_effort)
    if flag:
        args.append(flag)
    elif reasoning_effort:
        sys.stderr.write(
            f"DEBUG unknown reasoning_effort={reasoning_effort!r}; using ds4 default thinking mode\n"
        )
    return args


def resolve_context_tokens(request_context_tokens=None):
    """Resolve ds4 context tokens for process start/restart."""
    if _CONTEXT_TOKENS_ENV:
        return CONTEXT_TOKENS
    if request_context_tokens:
        try:
            return int(request_context_tokens)
        except (TypeError, ValueError):
            pass
    return CONTEXT_TOKENS


def build_ds4_args(prompt, model=MODEL_PATH, ctx=CONTEXT_TOKENS):
    """One-shot argv fallback (tests / emergency)."""
    args = build_ds4_persistent_args(model=model, ctx=ctx)
    args.extend(["-p", prompt])
    return args


def _strip_ansi(text):
    return _ANSI_ESCAPE_RE.sub("", text)


def format_ds4_response(stdout, stderr):
    """Return assistant text; surface stderr when ds4 produced no stdout."""
    stdout = _strip_ansi(stdout)
    lines = stdout.split("\n")
    clean_lines = [l for l in lines if not l.startswith("+DWARFSTAR")]
    clean_output = "\n".join(clean_lines).strip()
    if clean_output:
        return clean_output
    err = (stderr or "").strip()
    if err:
        return f"ds4-agent error: {err}"
    return "ds4-agent error: no output (check agent binary, model path, --chdir=ds4-home for metal/, preflight errors above, and that +DWARFSTAR_WAITING was emitted). See .codex-bridge-debug.log and /tmp/ds4_traces/ for the exact argv and last lines."


def collect_ds4_response_lines(lines):
    """Collect assistant text between WAITING markers."""
    collecting = False
    parts = []
    for raw in lines:
        line = raw.rstrip("\n")
        if line == WAITING_MARKER:
            if collecting:
                break
            collecting = True
            continue
        if line.startswith("+DWARFSTAR"):
            continue
        if collecting and line:
            parts.append(line)
    return format_ds4_response("\n".join(parts), "")


def handle_request(frame):
    """Process a request frame, extracting reasoning_effort and context_tokens.
    Returns (input_text, previous_response_id, reset_session, reasoning_effort, context_tokens).
    Does NOT restart the agent — that is done in the main loop when effort changes.
    """
    input_text = frame.get("input", "")
    previous_response_id = frame.get("previous_response_id", "")
    reset_session = frame.get("reset_session", False)
    reasoning_effort = frame.get("reasoning_effort", None)
    context_tokens = frame.get("context_tokens", None)
    return (input_text, previous_response_id, reset_session, reasoning_effort, context_tokens)


class _PtyLineReader:
    """PTY reader: real ds4-agent line-buffers stdout on pipes, not TTYs."""

    def __init__(self, master_fd):
        self._fd = master_fd
        self._q = queue.Queue()
        self._thread = threading.Thread(target=self._run, daemon=False)
        self._thread.start()

    def _run(self):
        buf = ""
        while True:
            try:
                ready, _, _ = select.select([self._fd], [], [], 0.5)
            except (OSError, ValueError):
                break
            if not ready:
                continue
            try:
                chunk = os.read(self._fd, 4096)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk.decode("utf-8", "replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                self._q.put(line + "\n")

    def readline(self, timeout=1.0):
        try:
            return self._q.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        if self._thread:
            self._thread.join(timeout=1.0)
        try:
            os.close(self._fd)
        except OSError:
            pass


class Ds4PersistentSession:
    """Keep one ds4-agent process alive; feed prompts on stdin."""

    def __init__(self, model=MODEL_PATH, ctx=CONTEXT_TOKENS, reasoning_effort=None, runtime_home=None):
        self.model = model
        self.ctx = ctx
        self.reasoning_effort = reasoning_effort
        self.runtime_home = runtime_home
        self.proc = None
        self._stdout_reader = None

    def start(self, timeout=DS4_STARTUP_TIMEOUT):
        _preflight_ds4_or_raise()
        args = build_ds4_persistent_args(
            model=self.model, ctx=self.ctx, reasoning_effort=self.reasoning_effort
        )
        runtime_home = self.runtime_home or ds4_runtime_home()
        self.runtime_home = runtime_home
        os.makedirs(runtime_home, exist_ok=True)
        master_fd, slave_fd = pty.openpty()
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=slave_fd,
            stderr=slave_fd,
            close_fds=True,
            env={**os.environ, "PYTHONUNBUFFERED": "1", "HOME": runtime_home},
        )
        os.close(slave_fd)
        self._stdout_reader = _PtyLineReader(master_fd)
        self._wait_until_waiting(timeout)
        sys.stderr.write(f"DEBUG ds4 persistent session ready home={runtime_home}\n")
        sys.stderr.flush()

    def run_prompt(self, prompt, timeout=DS4_TURN_TIMEOUT):
        if not self.proc or not self.proc.stdin:
            return "ds4-agent error: session not started", "session not started"
        one_line = " ".join(prompt.split())
        payload = (one_line + "\n").encode("utf-8")
        self.proc.stdin.write(payload)
        self.proc.stdin.flush()
        return self._read_turn_output(timeout)

    def stream_prompt_with_deltas(self, prompt, req_id, timeout=DS4_TURN_TIMEOUT):
        """Streaming version for Codex Desktop: sends text_delta frames as output appears,
        and periodic 'thinking' events during long silences between lines.
        Returns the full collected text at the end (for the final 'response' frame).
        """
        if not self.proc or not self.proc.stdin:
            return "ds4-agent error: session not started", "session not started"
        one_line = " ".join(prompt.split())
        payload = (one_line + "\n").encode("utf-8")
        self.proc.stdin.write(payload)
        self.proc.stdin.flush()

        # Send immediate activity to start the stream lifecycle on client side
        write_frame({
            "type": "event",
            "event": "thinking",
            "message": "ds4-agent received prompt, starting..."
        })
        last_activity = time.time()  # reset after initial ping

        deadline = time.time() + timeout
        parts = []
        last_activity = time.time()
        THINKING_PING_SECS = 2.0  # shorter than bridge's default 5s response_timeout_ms to prevent timeout while waiting for pings or output

        # For live streaming: limit individual deltas for long directory listing bursts (the agent's
        # natural "exploration" output like the massive file list in the 17:40 run). Show a sample live,
        # then a single notice delta, so the UI stays usable and the stream doesn't flood with 100s of
        # path lines. All lines still go to parts for final _compact_dir_listings (summarised in the
        # completed output). This makes the visible response "similar" to the agent's natural exploration
        # + "Let me..." thinking, while keeping the stream open for continuation deltas that deliver the
        # summary at the end.
        listing_burst_count = 0
        MAX_LIVE_LISTING_DELTAS = 12

        while time.time() < deadline:
            if self.proc.poll() is not None:
                return format_ds4_response("", "ds4-agent exited mid-turn"), "exited"
            line = self._stdout_reader.readline(timeout=1.0)
            if line:
                text = line.rstrip("\n")
                if text == WAITING_MARKER or text.startswith("+DWARFSTAR"):
                    break
                if text:
                    clean_text = _strip_ansi(text)
                    if clean_text:
                        is_tool = _is_tool_transcript_line(clean_text)
                        if is_tool and not DS4_SHOW_TOOL_LINES:
                            last_activity = time.time()
                        else:
                            is_listing = _looks_like_listing_line(clean_text)
                            if is_listing:
                                listing_burst_count += 1
                                if listing_burst_count <= MAX_LIVE_LISTING_DELTAS:
                                    parts.append(clean_text)
                                    write_frame({
                                        "type": "text_delta",
                                        "id": req_id,
                                        "text": clean_text + "\n"
                                    })
                                elif listing_burst_count == MAX_LIVE_LISTING_DELTAS + 1:
                                    # Send one notice instead of flooding the live stream with every path.
                                    notice = "[... listing many more files (see compacted summary in final response) ...]"
                                    parts.append(notice)
                                    write_frame({
                                        "type": "text_delta",
                                        "id": req_id,
                                        "text": notice + "\n"
                                    })
                                # else: still collect for final compaction/summary, but no more live deltas for this burst
                            else:
                                listing_burst_count = 0
                                parts.append(clean_text)
                                write_frame({
                                    "type": "text_delta",
                                    "id": req_id,
                                    "text": clean_text + "\n"
                                })
                            last_activity = time.time()
                    else:
                        last_activity = time.time()
                        listing_burst_count = 0
            else:
                if time.time() - last_activity > THINKING_PING_SECS:
                    write_frame({
                        "type": "event",
                        "event": "thinking",
                        "message": "ds4-agent is reasoning..."
                    })
                    last_activity = time.time()

        parts = _compact_dir_listings(parts)
        return format_ds4_response("\n".join(parts), ""), ""

    def save_session(self, timeout=DS4_TURN_TIMEOUT):
        """Issue ds4 `/save` and return the saved session id (best-effort parse).

        ds4 saves the session in ~/.ds4/kvcache and prints a confirmation line. The
        exact line format is not contract-documented, so we parse heuristically: the
        last id-shaped token in the command output. Returns None if no id is found
        (caller falls back to a fresh session). VERIFY against a live ds4 `/save`.
        """
        if not self.proc or not self.proc.stdin:
            return None
        self.proc.stdin.write(b"/save\n")
        self.proc.stdin.flush()
        text, _ = self._read_turn_output(timeout)
        return _parse_saved_session_id(text)

    def switch_session(self, sha, timeout=DS4_TURN_TIMEOUT):
        """Issue ds4 `/switch <id>` to restore a saved session. Returns True on success."""
        if not self.proc or not self.proc.stdin or not sha:
            return False
        self.proc.stdin.write(f"/switch {sha}\n".encode("utf-8"))
        self.proc.stdin.flush()
        self._read_turn_output(timeout)
        return True

    def close(self):
        if self._stdout_reader:
            self._stdout_reader.close()
            self._stdout_reader = None
        if self.proc and self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except Exception:
                pass
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def _wait_until_waiting(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    "ds4-agent exited during startup "
                    "(another ds4-agent may already be running — run scripts/cleanup_stale_bridges.sh)"
                )
            line = self._stdout_reader.readline(timeout=1.0)
            if not line:
                continue
            if WAITING_MARKER in line:
                return
        raise TimeoutError("ds4-agent did not emit +DWARFSTAR_WAITING during startup")

    def _read_turn_output(self, timeout):
        deadline = time.time() + timeout
        parts = []
        while time.time() < deadline:
            if self.proc.poll() is not None:
                return format_ds4_response("", "ds4-agent exited mid-turn"), "exited"
            line = self._stdout_reader.readline(timeout=1.0)
            if not line:
                continue
            text = line.rstrip("\n")
            if text == WAITING_MARKER or text.startswith("+DWARFSTAR"):
                break
            clean = _strip_ansi(text)
            if clean and (DS4_SHOW_TOOL_LINES or not _is_tool_transcript_line(clean)):
                parts.append(clean)
            # else: drop tool transcript lines from non-stream final too (unless explicitly shown)
        parts = _compact_dir_listings(parts)
        return format_ds4_response("\n".join(parts), ""), ""


class BridgeFrameRouter:
    """Background reader: answer pings immediately, queue other frames."""

    def __init__(self):
        self.queue = queue.Queue()
        self._stop = threading.Event()
        self._thread = None

    def start(self):
        self._thread = threading.Thread(target=self._run, daemon=False)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3.0)

    def _run(self):
        while not self._stop.is_set():
            frame = read_frame()
            if frame is None:
                self._stop.set()
                self.queue.put(None)
                break
            ft = frame.get("type", "")
            if ft == "ping":
                write_frame({"type": "pong", "id": frame.get("id", ""), "status": "ok"})
            elif ft == "health":
                write_frame({"type": "health", "id": frame.get("id", ""), "status": "ok"})
            elif ft == "error":
                sys.stderr.write(
                    f"DEBUG ignore error frame: {frame.get('error_type','')}: {frame.get('detail','')}\n"
                )
                sys.stderr.flush()
            else:
                self.queue.put(frame)


def read_frame():
    """Read a length-prefixed JSON frame from stdin (default) or UDS _bridge_comm (for UDS end-to-end)."""
    if _bridge_comm:
        header = _recv_all(_bridge_comm, 4)
        if not header or len(header) < 4:
            return None
        nlen = struct.unpack("<I", header)[0]
        payload = _recv_all(_bridge_comm, nlen)
        if not payload or len(payload) < nlen:
            return None
        decoded = json.loads(payload.decode("utf-8"))
        ft = decoded.get("type", "?")
        fid = decoded.get("id", "")
        sys.stderr.write(f"DEBUG recv: type={ft} id={fid}\n")
        sys.stderr.flush()
        return decoded
    else:
        # original stdio
        header = sys.stdin.buffer.read(4)
        if not header or len(header) < 4:
            return None
        nlen = struct.unpack("<I", header)[0]
        payload = sys.stdin.buffer.read(nlen)
        if not payload or len(payload) < nlen:
            return None
        decoded = json.loads(payload.decode("utf-8"))
        ft = decoded.get("type", "?")
        fid = decoded.get("id", "")
        sys.stderr.write(f"DEBUG recv: type={ft} id={fid}\n")
        sys.stderr.flush()
        return decoded


def write_frame(obj):
    """Write a JSON frame with length prefix to stdout (default) or UDS _bridge_comm."""
    data = json.dumps(obj, separators=(",", ":"))
    buf = data.encode("utf-8")
    nlen = len(buf)
    if _bridge_comm:
        header = struct.pack("<I", nlen)
        _send_all(_bridge_comm, header + buf)
    else:
        sys.stdout.buffer.write(struct.pack("<I", nlen))
        sys.stdout.buffer.write(buf)
        sys.stdout.flush()


def run_ds4_agent(prompt, model=MODEL_PATH, ctx=CONTEXT_TOKENS):
    """One-shot fallback."""
    args = build_ds4_args(prompt, model=model, ctx=ctx)
    try:
        proc = subprocess.Popen(
            args,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        stdout, stderr = proc.communicate(timeout=DS4_TURN_TIMEOUT)
        return format_ds4_response(stdout, stderr), stderr.strip()
    except subprocess.TimeoutExpired:
        proc.kill()
        return "ds4-agent error: timeout after 600s", "Timeout: ds4-agent did not complete"
    except Exception as e:
        return f"ds4-agent error: {e}", f"Error: {e}"


def apply_effort_change(current_session, reasoning_effort, context_tokens,
                        session_factory=Ds4PersistentSession):
    """Restart ds4 with a new thinking flag, preserving context via /save + /switch (T2.1).

    Sequence (per release decision): status ping -> /save current session (capture id)
    -> terminate ds4 -> relaunch with the new --think* flag -> /switch <id> to restore.
    Returns the new, running session. Raises on restart failure so the caller can emit a
    structured `effort_change_failed` error. The previous process is already terminated
    when a restart is attempted, so a failed relaunch leaves no live session (documented).
    """
    write_frame({
        "type": "event",
        "event": "status",
        "message": f"restarting agent for reasoning effort change ({reasoning_effort})",
    })
    saved_sha = None
    if current_session is not None:
        try:
            saved_sha = current_session.save_session()
        except Exception as e:
            sys.stderr.write(f"DEBUG /save before effort restart failed: {e}\n")
        current_session.close()

    runtime_home = getattr(current_session, "runtime_home", None) if current_session is not None else None
    new_ctx = resolve_context_tokens(context_tokens)
    new_session = session_factory(model=MODEL_PATH, ctx=new_ctx,
                                  reasoning_effort=reasoning_effort,
                                  runtime_home=runtime_home)
    new_session.start()

    if saved_sha:
        try:
            new_session.switch_session(saved_sha)
        except Exception as e:
            sys.stderr.write(f"DEBUG /switch {saved_sha} after effort restart failed: {e}\n")
    return new_session, saved_sha


def main():
    global _session_map, _active_session_key, _current_reasoning_effort
    _setup_bridge_comm()  # must be before first read_frame; for UDS blocks on accept until bridge connects
    session = None
    router = None
    try:
        while True:
            frame = read_frame()
            if frame is None:
                break
            if frame.get("type") == "hello":
                # Bridge sends workspace_root (from its argv[2] when launched as
                # star_bridge ds4-agent /path/to/workspace ...). Capture it so we can
                # force the agent to operate in the *correct* workspace even if Codex
                # request context mentions another dir (e.g. "New project 2").
                global BRIDGE_WORKSPACE_ROOT
                BRIDGE_WORKSPACE_ROOT = frame.get("workspace_root") or frame.get("workspace") or "."
                write_frame({
                    "type": "hello",
                    "role": "native_agent",
                    "protocol_version": 1,
                    "agent_name": "ds4-framed-wrapper",
                    "agent_version": "1.1.0",
                    "supported_transports": ["stdio_framed", "uds"],
                    "capabilities": {"session_state": True},
                })
                session = Ds4PersistentSession()
                try:
                    session.start()
                except Exception as e:
                    sys.stderr.write(f"DEBUG ds4 startup failed: {e}\n")
                    sys.stderr.flush()
                    write_frame({"type": "error", "message": f"ds4 startup failed: {e}"})
                    return
                write_frame({
                    "type": "ready",
                    "status": "ready",
                    "model_loaded": True,
                })
                sys.stderr.write(
                    f"DEBUG ds4 wrapper ready: workspace={BRIDGE_WORKSPACE_ROOT} show_tool_lines={DS4_SHOW_TOOL_LINES}\n"
                )
                sys.stderr.flush()
                router = BridgeFrameRouter()
                router.start()
                break
            if frame.get("type") == "shutdown":
                write_frame({"type": "response", "status": "ok", "message": "shutting down"})
                return

        while True:
            frame = router.queue.get()
            if frame is None:
                break

            frame_type = frame.get("type", "")
            req_id = frame.get("id", "")

            if frame_type == "shutdown":
                write_frame({"type": "response", "status": "ok", "message": "shutting down"})
                break

            if frame_type == "request":
                # Use handle_request to extract reasoning_effort and context_tokens
                result = handle_request(frame)
                if result is None:
                    continue  # error already sent via write_frame
                input_text, previous_response_id, reset_session, reasoning_effort, context_tokens = result

                ack_id = req_id if req_id else "req-1"
                sys.stderr.write(f"DEBUG send ack: id={ack_id}\n")
                sys.stderr.flush()
                write_frame({"type": "ack", "id": ack_id, "status": "accepted"})

                # reset_session means fresh native context. Do not save/switch the
                # current ds4 state here; that is exactly how stale context leaks.
                if reset_session:
                    sys.stderr.write("DEBUG reset_session: restarting ds4 with fresh native context\n")
                    sys.stderr.flush()
                    if session is not None:
                        session.close()
                    fresh_ctx = resolve_context_tokens(context_tokens)
                    reset_home = os.path.join(
                        ds4_runtime_home(),
                        "reset",
                        f"{int(time.time() * 1000)}_{os.getpid()}",
                    )
                    session = Ds4PersistentSession(
                        model=MODEL_PATH,
                        ctx=fresh_ctx,
                        reasoning_effort=reasoning_effort,
                        runtime_home=reset_home,
                    )
                    session.start()
                    _current_reasoning_effort = reasoning_effort
                    sys.stderr.write(
                        f"DEBUG reset_session: fresh ds4 ready reasoning_effort={reasoning_effort} ctx={fresh_ctx}\n"
                    )
                    sys.stderr.flush()

                # Handle effort change: restart agent with the new thinking flag if effort changed
                elif reasoning_effort is not None and reasoning_effort != _current_reasoning_effort:
                    sys.stderr.write(
                        f"DEBUG effort change: {_current_reasoning_effort} -> {reasoning_effort}\n"
                    )
                    sys.stderr.flush()
                    try:
                        session, saved_sha = apply_effort_change(
                            session, reasoning_effort, context_tokens
                        )
                        _current_reasoning_effort = reasoning_effort
                        sys.stderr.write(
                            f"DEBUG ds4 restarted with reasoning_effort={reasoning_effort} "
                            f"(restored sha={saved_sha})\n"
                        )
                        sys.stderr.flush()
                    except Exception as e:
                        session = None
                        msg = f"Failed to restart ds4 for reasoning_effort={reasoning_effort}: {e}"
                        sys.stderr.write(f"DEBUG {msg}\n")
                        sys.stderr.flush()
                        write_frame({"type": "error", "message": msg, "error_type": "effort_change_failed"})
                        continue
                elif reasoning_effort is not None:
                    # Effort unchanged, no restart needed
                    sys.stderr.write(f"DEBUG effort unchanged: {reasoning_effort} (no restart)\n")
                    sys.stderr.flush()
                else:
                    _current_reasoning_effort = None

                if reset_session:
                    base = f"[Session: new]\n{input_text}"
                elif previous_response_id:
                    base = f"[Session: {previous_response_id}]\n{input_text}"
                else:
                    base = input_text

                # Strongly inject the bridge-provided workspace so the agent (ds4) performs
                # list/find/read/edit etc. against the *correct* project instead
                # of whatever dir Codex Desktop had open when it built the big request context
                # ("New project 2" etc.). The prefix goes at the very front.
                ws = BRIDGE_WORKSPACE_ROOT or "."
                bool_reply_exactly = "reply exactly" in (input_text or "").lower()
                if ws and ws != "." and not bool_reply_exactly:
                    ws_prefix = (
                        f"WORKSPACE_ROOT={ws}\n"
                        "You are operating inside this exact directory for the current task. "
                        "All file listing, searching, reading, writing, and exploration MUST target paths under it. "
                        "Ignore any paths or 'current project' mentions from history or prior context that point elsewhere. "
                        "If your tools take a path, prefer relative paths under this root or absolute under it.\n\n"
                    )
                    prompt = ws_prefix + base
                else:
                    prompt = base

                # Detect review-style tasks (from the original user input) so completion
                # forcing is applied only when the user asked for review/exploration.
                input_lower = (input_text or "").lower()
                review_task = (not bool_reply_exactly) and any(kw in input_lower for kw in (
                    "review", "explore", "analyze", "analyse", "audit",
                    "intent", "current state", "potential improvements",
                    "effort and benefit", "effort/benefit", "effort: ", "benefit: "
                ))

                # Append synthesis / completion guidelines so that after tool use, exploration,
                # or raw output (directory scans, file listings, etc.), the native agent continues
                # generating a full, structured final answer that directly fulfills the ORIGINAL
                # user request. This makes the bridged experience produce the "response / completion
                # summary" (e.g. project intent, current state from explored files, scored
                # improvements) that users expect from the Codex app, while the bridge remains a
                # transparent adapter and the native ds4 remains the source of the intelligence.
                # Appended at the end so it has high priority for the current generation/turn.
                #
                # Strengthened for dogfood: explicitly require the synthesis *before finishing the
                # response to this message* (to fight the observed "plan sentence then WAITING"
                # behavior on review tasks).
                completion_directive = (
                    "\n\n## Task Completion Requirement\n"
                    "After any tool calls, exploration, directory scanning, file listings, or printing of raw results, "
                    "you MUST continue your response (in the same turn / same generation, before you would normally finish or yield) "
                    "with a complete, self-contained final answer that fully addresses the user's ORIGINAL request. "
                    "Do not end the response after raw tool output, listings, or a high-level plan announcement.\n"
                    "For review, analysis, 'understand intent/state/improvements with effort and benefit scores', or similar tasks, "
                    "structure the conclusion with clear sections in natural prose and markdown suitable for the Codex app UI:\n"
                    "- Intent of the project/folder\n"
                    "- Current state (structure, key files like README.md / src/ / tests/ / docs/, maturity from what you actually explored)\n"
                    "- Potential improvements (list several; for each give a short description plus Effort: Low/Medium/High and Benefit: Low/Medium/High scores)\n"
                    "Stream any intermediate thinking or plan as normal, but the *final content you emit for this input* must contain the synthesized completion summary with the scored sections. "
                    "The visible answer after tools/exploration must be the structured review, not just the plan or raw scans."
                )
                if review_task:
                    prompt = prompt + completion_directive

                # Stream deltas + periodic thinking pings for long silences.
                # This is the key to prevent Codex "Reconnecting" during thinking.
                # Real lines -> immediate text_delta to bridge (live SSE deltas to Codex).
                # No output for a while -> "thinking" event (bridge turns into SSE keepalive/heartbeat).
                # (These pings + the withholding of the terminal "response" frame are what keep the
                # outer Codex Responses stream open during the dogfood internal continuation loop.)
                try:
                    output_text, stderr_text = session.stream_prompt_with_deltas(prompt, ack_id)
                except Exception as e:
                    output_text = f"ds4-agent error: {e}"
                    stderr_text = str(e)

                if stderr_text:
                    sys.stderr.write(f"DEBUG ds4 stderr: {stderr_text[:500]}\n")
                    sys.stderr.flush()

                # --- Dogfood continuation logic (post stream_prompt_with_deltas) ---
                # Withhold the terminal "response" frame until the native output has a real
                # synthesis, so Codex sees one continuous answer instead of a completed plan.
                full_output = (output_text or "").strip()
                MAX_CONTINUATIONS = 5
                continuation_count = 0

                # For review tasks we are aggressive: keep going (up to cap) until we see positive
                # evidence of the complete scored summary, rather than only while "looks like plan".
                # This forces the extra generations needed for exploration + the "MUST ... final answer"
                # even if a cont generation produces mostly filtered tool transcript or another short
                # visible sentence. We also guarantee at least one cont for review tasks.
                def _should_continue_for_review(fo: str, count: int) -> bool:
                    if count >= MAX_CONTINUATIONS:
                        return False
                    if _looks_like_complete_summary(fo):
                        return False
                    if review_task:
                        # Force at least 1-2 conts for review, and keep while not complete.
                        if count < 2 or not _looks_like_complete_summary(fo):
                            return True
                    # Fallback to the plan-only heuristic for non-review or after the min.
                    return (not bool_reply_exactly) and _is_plan_only_output(fo)

                while _should_continue_for_review(full_output, continuation_count):
                    continuation_count += 1
                    sys.stderr.write(
                        f"DEBUG ds4 wrapper: {'review_task' if review_task else 'plan-only'} output after WAITING "
                        f"(cont={continuation_count}); sending internal continuation prompt to capture full review.\n"
                    )
                    sys.stderr.flush()

                    # Short, directive-focused continuation. The prior prompt (ws_prefix +
                    # completion_directive) is already in the persistent session context.
                    # Re-emphasize doing the *work* (list/find/read under WORKSPACE_ROOT) then
                    # emitting the exact synthesis shape. Use a stronger "final nudge" variant
                    # on later continuations to discourage further planning/tool echoes and
                    # demand the visible structured answer now.
                    if continuation_count >= 4 or (review_task and continuation_count >= 3):
                        cont_prompt = (
                            "FINAL NUDGE - LAST GENERATION FOR THIS REQUEST. The visible response so far was "
                            "only planning or exploration setup, and it was curtailed before the actual answer. "
                            "Stop *all* further planning, meta-commentary, \"Let me...\", or new exploration announcements. "
                            "Based on everything explored so far for the ORIGINAL request, IMMEDIATELY output ONLY "
                            "the complete self-contained structured final answer. It *must* contain at minimum the sections:\n"
                            "- Intent of the project/folder\n"
                            "- Current state (structure, key files like README.md / src/ / tests/ / docs/, "
                            "maturity from what you actually explored)\n"
                            "- Potential improvements (several concrete ones; for each: short description + "
                            "Effort: Low/Medium/High and Benefit: Low/Medium/High scores)\n"
                            "No more plan sentences of any kind. The final visible content of *this* turn must be "
                            "exactly the scored review summary. Deliver the full structured answer with the scores now, "
                            "before your next WAITING."
                        )
                    else:
                        cont_prompt = (
                            "Continue the original task immediately. Do not emit another high-level plan. "
                            "Use list/find/read (and other tools as needed) against paths under the WORKSPACE_ROOT "
                            "established for this session. After exploration or any tool results/listings, "
                            "MUST produce (in this generation, before your next WAITING) the complete "
                            "self-contained structured final answer for the ORIGINAL user request. "
                            "For a review/analysis task include at minimum these sections in natural prose/markdown:\n"
                            "- Intent of the project/folder\n"
                            "- Current state (structure, key files like README.md / src/ / tests/ / docs/, "
                            "maturity from what you actually explored)\n"
                            "- Potential improvements (several; for each: short description + Effort: Low/Medium/High "
                            "and Benefit: Low/Medium/High scores)\n"
                            "Deliver the synthesized summary now; stream any intermediate thinking as normal. "
                            "The final output of this turn must contain the scored review, not just raw scans or plans."
                        )

                    try:
                        more_text, more_err = session.stream_prompt_with_deltas(cont_prompt, ack_id)
                    except Exception as e:
                        more_text = ""
                        more_err = str(e)

                    if more_err:
                        sys.stderr.write(f"DEBUG ds4 continuation stderr: {str(more_err)[:300]}\n")
                        sys.stderr.flush()

                    if more_text and more_text.strip():
                        # Live text_delta(s) + any pings for this continuation were already emitted
                        # by stream_prompt_with_deltas (same req_id/ack_id). This keeps the Codex
                        # Responses stream open and appends seamlessly. Just accumulate here for
                        # the terminal protocol "response" frame + detector re-eval.
                        addition = more_text.strip()
                        full_output = (full_output + "\n\n" + addition) if full_output else addition

                    # Loop will re-eval _should_continue... at top (using updated full_output).

                # Last chance for review tasks: ask once more for synthesis before falling
                # back to a compact wrapper note.
                if review_task and not _looks_like_complete_summary(full_output) and continuation_count < MAX_CONTINUATIONS:
                    continuation_count += 1
                    sys.stderr.write(
                        f"DEBUG ds4 wrapper: review_task still missing synthesis after {continuation_count-1} conts; "
                        f"forcing one last synthesis-only prompt + bridge fallback if needed.\n"
                    )
                    sys.stderr.flush()
                    force_prompt = (
                        "STOP. Output the structured review summary for the ORIGINAL request RIGHT NOW. "
                        "Use exactly these sections (natural prose/markdown, no plans, no more tools, no echoes):\n"
                        "- Intent of the project/folder\n"
                        "- Current state (structure, key files like README.md / src/ / tests/ / docs/, "
                        "maturity from prior exploration of WORKSPACE_ROOT)\n"
                        "- Potential improvements (several with Effort: Low/Medium/High and Benefit: Low/Medium/High)\n"
                        "This is the visible final answer. Emit it before WAITING."
                    )
                    try:
                        last_text, last_err = session.stream_prompt_with_deltas(force_prompt, ack_id)
                    except Exception as e:
                        last_text = ""
                        last_err = str(e)
                    if last_err:
                        sys.stderr.write(f"DEBUG ds4 final-force stderr: {str(last_err)[:200]}\n")
                        sys.stderr.flush()
                    if last_text and last_text.strip():
                        add = last_text.strip()
                        full_output = (full_output + "\n\n" + add) if full_output else add

                # If *still* no synthesis for a review task after the force, produce a minimal
                # bridge-side summary note (intelligently summarised path) and surface it via
                # an extra text_delta (keeps stream progressing) before the completed frame.
                # We don't fabricate full "native" details (we only have the filtered visible
                # parts + workspace root), but we give the user a usable structured stub with
                # reasonable scores so the review request doesn't end curtailed at the plan.
                if review_task and not _looks_like_complete_summary(full_output):
                    sys.stderr.write(
                        "DEBUG ds4 wrapper: review_task synthesis still absent after max conts + force; "
                        "emitting bridge-side summary note as final visible delta + including in output.\n"
                    )
                    sys.stderr.flush()
                    bridge_note = (
                        "\n\n## Bridge Fallback Summary\n"
                        "The native agent stopped before emitting the requested scored review sections. "
                        f"Workspace: {BRIDGE_WORKSPACE_ROOT}.\n"
                        "Current state: the bridge received partial native output and exhausted its bounded "
                        "continuation attempts without a complete synthesis.\n"
                        "Potential improvements:\n"
                        "1. Rerun with a narrower review prompt that names the exact files or areas to inspect - Effort: Low, Benefit: Medium\n"
                        "2. Increase native continuation reliability so review tasks finish with visible scored sections - Effort: Medium, Benefit: High\n"
                        "3. Add a dedicated local scan fallback for review prompts when the native agent stops early - Effort: High, Benefit: High\n"
                    )
                    # Emit as live delta so Codex stream (still open, pre-response-frame) receives it
                    # and the UI appends the summary after the plan text.
                    try:
                        write_frame({
                            "type": "text_delta",
                            "id": ack_id,
                            "text": bridge_note + "\n"
                        })
                    except Exception:
                        pass
                    full_output = (full_output + bridge_note) if full_output else bridge_note

                # Optional intelligent summary step for huge exploration content in the final
                # assembled output (preserves the spirit of _compact_dir_listings). In practice
                # the per-sub-turn compaction + the fact that successful continuations (or the
                # bridge fallback) reach the short synthesized review section means the final
                # payload stays reasonable. Lightweight length guard; we already emitted any
                # needed deltas above.
                if len(full_output) > 20000:
                    try:
                        write_frame({
                            "type": "event",
                            "event": "compaction.summary",
                            "message": f"Compacted very large accumulated review output ({len(full_output)} chars) for transport."
                        })
                    except Exception:
                        pass
                    head = full_output[:8000]
                    tail = full_output[-8000:]
                    full_output = head + "\n\n[... large exploration output compacted ...]\n\n" + tail

                # Final completed response (for protocol completeness and non-stream paths).
                # At this point we have either the native's synthesized review (best case, via
                # conts), or the forced nudge output, or the bridge-side summarised note (with
                # scores) for review tasks. The terminal "response" frame is sent *only now*,
                # after any extra deltas from the summarised path. This is what tells the bridge
                # (ResponsesStreamState) to emit the final done/completed + [DONE] on the Codex side.
                final_output = full_output if full_output else (output_text or "")
                write_frame({
                    "type": "response",
                    "id": ack_id,
                    "status": "completed",
                    "output": final_output,
                })

            elif frame_type == "create_state":
                key = frame.get("key", "")
                project_root = frame.get("project_root", "")
                model_id = frame.get("model_id", "")
                # Generate deterministic state_id from key (same as fake_agent)
                state_id = f"state_{hash(key) & 0xffffffff}" if key else "state_empty"
                # Record session mapping
                _session_map[key] = {
                    "sha": state_id,
                    "project_root": project_root,
                    "model_id": model_id,
                }
                _active_session_key = key
                sys.stderr.write(f"DEBUG session create_state key={key} state_id={state_id}\n")
                sys.stderr.flush()
                write_frame({"type": "ack", "status": "accepted", "state_id": state_id})

            elif frame_type == "load_state":
                key = frame.get("key", "")
                state_id = frame.get("state_id", "")
                # Look up or create session mapping
                if key in _session_map:
                    sha = _session_map[key]["sha"]
                    sys.stderr.write(f"DEBUG session load_state key={key} sha={sha}\n")
                else:
                    sha = f"state_{hash(key) & 0xffffffff}" if key else "state_empty"
                    _session_map[key] = {"sha": sha, "project_root": "", "model_id": ""}
                    sys.stderr.write(f"DEBUG session load_state key={key} created sha={sha}\n")
                _active_session_key = key
                sys.stderr.flush()
                write_frame({"type": "ack", "status": "accepted", "state_id": sha})

            elif frame_type == "save_state":
                key = frame.get("key", "")
                state_id = frame.get("state_id", "")
                # Record the session state for this key
                if key:
                    if key not in _session_map:
                        _session_map[key] = {"sha": state_id, "project_root": "", "model_id": ""}
                    else:
                        _session_map[key]["sha"] = state_id
                    sys.stderr.write(f"DEBUG session save_state key={key} state_id={state_id}\n")
                    sys.stderr.flush()
                write_frame({"type": "ack", "status": "accepted"})

            elif frame_type == "switch_state":
                from_key = frame.get("from_key", "")
                to_key = frame.get("to_key", "")
                to_state_id = frame.get("state_id", "")
                # Record the from_key session if we have one
                if from_key and _active_session_key and from_key == _active_session_key:
                    if from_key not in _session_map:
                        _session_map[from_key] = {"sha": "", "project_root": "", "model_id": ""}
                    sys.stderr.write(f"DEBUG session switch_state from={from_key} to={to_key} state_id={to_state_id}\n")
                # Activate the to_key session
                if to_key:
                    _active_session_key = to_key
                    if to_key not in _session_map:
                        sha = f"state_{hash(to_key) & 0xffffffff}" if to_key else "state_empty"
                        _session_map[to_key] = {"sha": sha, "project_root": "", "model_id": ""}
                sys.stderr.flush()
                write_frame({"type": "ack", "status": "accepted"})

            else:
                write_frame({"type": "error", "message": f"unknown frame type: {frame_type}"})
    finally:
        if router:
            router.stop()
        if session:
            session.close()
        try:
            sys.stdout.flush()
            sys.stderr.flush()
        except Exception:
            pass


if __name__ == "__main__":
    main()
