#!/usr/bin/env bash
# Stop leftover bridge/ds4 processes from test runs or crashed wrappers.
set -euo pipefail

stop_pids() {
  local label="$1"
  local pattern="$2"
  local pids
  pids="$(pgrep -f "$pattern" 2>/dev/null || true)"
  if [[ -z "$pids" ]]; then
    return 0
  fi
  echo "Stopping $label PIDs: $pids"
  kill $pids 2>/dev/null || true
  sleep 0.5
  local still
  still="$(pgrep -f "$pattern" 2>/dev/null || true)"
  if [[ -n "$still" ]]; then
    echo "Force killing $label: $still"
    kill -9 $still 2>/dev/null || true
  fi
}

stop_pids "star_bridge" '[/](star_bridge|codex_bridge)|bin/(star_bridge|codex_bridge)'
stop_pids "ds4_wrapper" 'ds4_wrapper\.py'
# ds4-agent allows only one instance; stop orphans from crashed wrapper runs.
stop_pids "ds4-agent" 'ds4-agent'

echo "Stale bridge cleanup done."
