#!/usr/bin/env python3
"""Star Bridge analytics — parse `turn_metrics` lines from the debug trace and
report performance and steering (reasoning_effort) effectiveness.

Each turn emits one line:
  turn_metrics request=N status=S effort=E duration_ms=D output_bytes=B \
               prompt_tokens=P completion_tokens=C tool_calls=K

Metrics:
  - time to complete   : mean/median duration_ms of completed turns
  - throughput (tk/s)  : completion_tokens / seconds; when the agent reports no
                         token usage, falls back to an output-bytes estimate
                         (~4 bytes/token) and labels the number "(est)"
  - running / outcomes : counts by status (completed/error/timeout/...)
  - tool calls         : total and mean per turn
  - steering           : per reasoning_effort breakdown (latency, tk/s, tools,
                         completion rate) so you can see what is actually working

Usage:
  scripts/analytics.py [LOGFILE ...] [--json]
Defaults to .codex-bridge-debug.log in the current directory.
"""
import sys
import re
import json
import statistics
from collections import defaultdict

BYTES_PER_TOKEN = 4  # rough heuristic when the agent reports no usage

LINE_RE = re.compile(r"turn_metrics\s+(.*)$")
KV_RE = re.compile(r"(\w+)=(\S+)")


def parse(paths):
    turns = []
    for path in paths:
        try:
            with open(path, "r", errors="replace") as f:
                for line in f:
                    m = LINE_RE.search(line)
                    if not m:
                        continue
                    kv = dict(KV_RE.findall(m.group(1)))
                    if "request" not in kv:
                        continue
                    turns.append({
                        "request": kv.get("request", "?"),
                        "status": kv.get("status", "unknown"),
                        "effort": kv.get("effort", "default"),
                        "duration_ms": int(kv.get("duration_ms", 0) or 0),
                        "output_bytes": int(kv.get("output_bytes", 0) or 0),
                        "prompt_tokens": int(kv.get("prompt_tokens", 0) or 0),
                        "completion_tokens": int(kv.get("completion_tokens", 0) or 0),
                        "tool_calls": int(kv.get("tool_calls", 0) or 0),
                    })
        except FileNotFoundError:
            print(f"warning: {path} not found", file=sys.stderr)
    return turns


def tokens_for(t):
    """(tokens, estimated?) for throughput."""
    if t["completion_tokens"] > 0:
        return t["completion_tokens"], False
    return (t["output_bytes"] / BYTES_PER_TOKEN), True


def tk_per_s(t):
    if t["duration_ms"] <= 0:
        return None
    toks, _ = tokens_for(t)
    return toks / (t["duration_ms"] / 1000.0)


def agg(turns):
    completed = [t for t in turns if t["status"] == "completed"]
    durations = [t["duration_ms"] for t in completed if t["duration_ms"] > 0]
    tks = [v for v in (tk_per_s(t) for t in completed) if v is not None]
    any_real_tokens = any(t["completion_tokens"] > 0 for t in turns)
    return {
        "completed": completed,
        "durations": durations,
        "tks": tks,
        "any_real_tokens": any_real_tokens,
    }


def fmt_mean(xs):
    return f"{statistics.mean(xs):.1f}" if xs else "n/a"


def fmt_median(xs):
    return f"{statistics.median(xs):.1f}" if xs else "n/a"


def build_report(turns):
    out = {}
    status_counts = defaultdict(int)
    for t in turns:
        status_counts[t["status"]] += 1
    a = agg(turns)
    total_tools = sum(t["tool_calls"] for t in turns)

    out["turns_total"] = len(turns)
    out["status_counts"] = dict(status_counts)
    out["completed"] = len(a["completed"])
    out["avg_duration_ms"] = round(statistics.mean(a["durations"]), 1) if a["durations"] else None
    out["median_duration_ms"] = round(statistics.median(a["durations"]), 1) if a["durations"] else None
    out["avg_tokens_per_s"] = round(statistics.mean(a["tks"]), 2) if a["tks"] else None
    out["tokens_are_estimated"] = not a["any_real_tokens"]
    out["total_tool_calls"] = total_tools
    out["avg_tool_calls_per_turn"] = round(total_tools / len(turns), 2) if turns else None

    # Steering breakdown by reasoning_effort.
    by_effort = defaultdict(list)
    for t in turns:
        by_effort[t["effort"]].append(t)
    steering = {}
    for effort, ts in sorted(by_effort.items()):
        comp = [t for t in ts if t["status"] == "completed"]
        durs = [t["duration_ms"] for t in comp if t["duration_ms"] > 0]
        ek = [v for v in (tk_per_s(t) for t in comp) if v is not None]
        steering[effort] = {
            "turns": len(ts),
            "completed": len(comp),
            "completion_rate": round(len(comp) / len(ts), 2) if ts else None,
            "avg_duration_ms": round(statistics.mean(durs), 1) if durs else None,
            "avg_tokens_per_s": round(statistics.mean(ek), 2) if ek else None,
            "avg_tool_calls": round(sum(t["tool_calls"] for t in ts) / len(ts), 2) if ts else None,
        }
    out["steering"] = steering
    out["steering_active"] = len(by_effort) > 1
    return out


def na(x):
    return "n/a" if x is None else x


def print_report(r):
    print("=" * 60)
    print("Star Bridge — turn analytics")
    print("=" * 60)
    if r["turns_total"] == 0:
        print("No turn_metrics lines found. Run the bridge with trace/debug_log")
        print("enabled (config: trace=true, debug_log=true) and drive some turns.")
        return
    print(f"Turns:            {r['turns_total']}  (completed: {r['completed']})")
    print(f"Outcomes:         " + ", ".join(f"{k}={v}" for k, v in r["status_counts"].items()))
    print(f"Time to complete: avg {na(r['avg_duration_ms'])} ms, median {na(r['median_duration_ms'])} ms")
    est = " (est from bytes — agent reported no token usage)" if r["tokens_are_estimated"] else ""
    print(f"Throughput:       {na(r['avg_tokens_per_s'])} tk/s{est}")
    print(f"Tool calls:       {r['total_tool_calls']} total, {na(r['avg_tool_calls_per_turn'])}/turn")
    print()
    print("Steering (reasoning_effort / thinking level):")
    if not r["steering_active"]:
        only = next(iter(r["steering"]), "n/a")
        print(f"  Only one effort level seen ('{only}') — no steering variation to")
        print("  compare. Drive turns at low/medium/high to evaluate steering.")
    for effort, s in r["steering"].items():
        print(f"  {effort:8s}  turns={s['turns']:<4d} done={na(s['completion_rate'])}  "
              f"lat={na(s['avg_duration_ms'])}ms  {na(s['avg_tokens_per_s'])}tk/s  "
              f"tools/turn={na(s['avg_tool_calls'])}")
    if r["steering_active"]:
        # "What is working": best effort by throughput among those with data.
        ranked = [(e, s["avg_tokens_per_s"]) for e, s in r["steering"].items()
                  if s["avg_tokens_per_s"] is not None]
        if ranked:
            best = max(ranked, key=lambda x: x[1])
            print(f"\n  Highest throughput: '{best[0]}' at {best[1]} tk/s.")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    as_json = "--json" in sys.argv[1:]
    paths = args or [".codex-bridge-debug.log"]
    turns = parse(paths)
    report = build_report(turns)
    if as_json:
        print(json.dumps(report, indent=2))
    else:
        print_report(report)


if __name__ == "__main__":
    main()
