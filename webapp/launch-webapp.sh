#!/usr/bin/env bash
#
# One-click launcher for the Trossen SDK webapp.
#
# Brings the docker-compose stack up (building it the first time), waits for the
# frontend to answer, then opens it in a dedicated browser window so it behaves
# like a native desktop app. Safe to run repeatedly: if the stack is already up
# it just (re)opens the window.
#
# Wired to ~/.local/share/applications/trossen-webapp.desktop by install-launcher.sh
# so it can be started from the GNOME app grid / dock. Can also be run directly.

set -euo pipefail

# --- locate the webapp dir (this script lives in it) -------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

URL="http://localhost:5173"
LOG="${TMPDIR:-/tmp}/trossen-webapp-launch.log"
COMPOSE=(docker compose)

# --- helpers -----------------------------------------------------------------
ICON="$HOME/.local/share/icons/trossen-webapp.png"   # installed by install-launcher.sh
notify() { command -v notify-send >/dev/null && notify-send -i "$ICON" "Trossen Webapp" "$1" || true; }

frontend_up() { curl -fsS -o /dev/null --max-time 2 "$URL" 2>/dev/null; }

open_window() {
  # Prefer a chrome-less "app" window so it feels like a native application.
  if command -v brave >/dev/null; then
    setsid brave --app="$URL" >/dev/null 2>&1 &
  elif command -v google-chrome >/dev/null; then
    setsid google-chrome --app="$URL" >/dev/null 2>&1 &
  elif command -v chromium >/dev/null; then
    setsid chromium --app="$URL" >/dev/null 2>&1 &
  else
    setsid xdg-open "$URL" >/dev/null 2>&1 &
  fi
}

fail() {
  notify "Failed to start — see $LOG"
  if command -v zenity >/dev/null; then
    zenity --error --width=480 \
      --title="Trossen Webapp" \
      --text="The webapp failed to start.\n\nLast lines of the log:\n\n$(tail -n 15 "$LOG" 2>/dev/null | sed 's/&/\&amp;/g; s/</\&lt;/g')" || true
  fi
  exit 1
}

# --- fast path: already running ----------------------------------------------
if frontend_up; then
  open_window
  exit 0
fi

# --- bring the stack up ------------------------------------------------------
# Run the (possibly slow, first-time) build+up under a pulsating zenity dialog
# so the operator gets feedback instead of a dead click. The build streams to
# $LOG; on failure we surface its tail.
: > "$LOG"
notify "Starting up…"

build_and_wait() {
  echo "[launch] docker compose up -d --build" >>"$LOG"
  if ! "${COMPOSE[@]}" up -d --build >>"$LOG" 2>&1; then
    echo "__COMPOSE_FAILED__" >>"$LOG"
    return 1
  fi
  # Containers are up; the frontend dev server still needs a moment to bind.
  echo "[launch] waiting for $URL" >>"$LOG"
  for _ in $(seq 1 120); do          # up to ~2 min for the dev server to answer
    frontend_up && return 0
    sleep 1
  done
  echo "__FRONTEND_TIMEOUT__" >>"$LOG"
  return 1
}

if command -v zenity >/dev/null; then
  # Drive a pulsating progress dialog; close it when the work finishes.
  ( build_and_wait; echo $? > "${LOG}.rc" ) &
  WORK_PID=$!
  ( while kill -0 "$WORK_PID" 2>/dev/null; do echo "# Building / starting containers…"; sleep 1; done ) \
    | zenity --progress --pulsate --auto-close --no-cancel \
        --width=420 --title="Trossen Webapp" \
        --text="Building / starting containers…" || true
  wait "$WORK_PID" 2>/dev/null || true
  [ "$(cat "${LOG}.rc" 2>/dev/null || echo 1)" = "0" ] || fail
else
  build_and_wait || fail
fi

notify "Ready"
open_window
