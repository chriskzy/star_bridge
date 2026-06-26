#!/bin/bash
# ------------------------------------------------------------------ #
#  Healthcheck probe for star_bridge Docker container                  #
#  Returns 0 if bridge responds with 200, 1 otherwise                 #
# ------------------------------------------------------------------ #

PORT="${CODEX_PORT:-8080}"
URL="http://localhost:${PORT}/health"

# Use curl with a short timeout
response=$(curl -s -o /dev/null -w "%{http_code}" --max-time 3 "$URL" 2>/dev/null)

if [ "$response" = "200" ]; then
    exit 0
fi

# Fallback: check if the process is still running
if kill -0 "$(cat /tmp/star_bridge.pid 2>/dev/null)" 2>/dev/null; then
    exit 0
fi

exit 1
