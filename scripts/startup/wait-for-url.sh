#!/usr/bin/env bash
#
# Block until an HTTP endpoint answers, or give up.
#
#   wait-for-url.sh <url> [timeout_seconds]
#
# Every kiosk here needs this and none of them should each invent it: a browser
# started before the webapp is listening shows a connection error and then sits
# on it forever, because nothing tells a kiosk browser to try again.
set -uo pipefail

URL="${1:?usage: wait-for-url.sh <url> [timeout_seconds]}"
TIMEOUT="${2:-180}"

# ANY HTTP response counts as up, including 404 — deliberately not `curl -f`.
# A backend running without `frontend/dist` serves the API and 404s every page
# ("no frontend/dist — API only", see webapp/RIG_BRINGUP.md), and a rig in that
# state is exactly when you want the screen to come up and SAY so. Waiting for a
# 200 there means a black display and no clue why.
answering() {
  local code
  code="$(curl -sS -o /dev/null -w '%{http_code}' --max-time 3 "$1" 2>/dev/null)"
  [ -n "$code" ] && [ "$code" != "000" ]
}

waited=0
until answering "$URL"; do
  if [ "$waited" -ge "$TIMEOUT" ]; then
    echo "wait-for-url: $URL did not answer within ${TIMEOUT}s" >&2
    exit 1
  fi
  sleep 2
  waited=$((waited + 2))
done
echo "wait-for-url: $URL answered after ${waited}s"
