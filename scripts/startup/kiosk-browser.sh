#!/usr/bin/env bash
#
# Open one URL fullscreen, with nothing around it, and keep it open.
#
#   kiosk-browser.sh <url> [profile_name]
#
# Used by the Rivet's own screen and by the third-screen machine. The Pi is the
# exception: it has no desktop session, so it runs chromium under `cage`
# directly (see install-pi-kiosk.sh).
#
# Restart-on-exit is the point. A kiosk browser that is closed, crashes, or is
# killed by the OOM reaper must come back without anyone walking over to it.
set -uo pipefail

URL="${1:?usage: kiosk-browser.sh <url> [profile_name]}"
PROFILE="${2:-trossen-kiosk}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Wait for the app before opening anything: see wait-for-url.sh.
"$HERE/wait-for-url.sh" "$URL" "${KIOSK_WAIT_S:-300}" || \
  echo "kiosk: starting the browser anyway so the screen is not blank" >&2

# Screen blanking and idle locking are the two things that make a display look
# broken hours later. Best-effort — these are no-ops off a GNOME session.
if command -v gsettings >/dev/null 2>&1; then
  gsettings set org.gnome.desktop.session idle-delay 0 2>/dev/null || true
  gsettings set org.gnome.desktop.screensaver lock-enabled false 2>/dev/null || true
  gsettings set org.gnome.settings-daemon.plugins.power sleep-inactive-ac-type nothing 2>/dev/null || true
fi
command -v xset >/dev/null 2>&1 && { xset s off; xset -dpms; xset s noblank; } 2>/dev/null || true

# Snaps live in /snap/bin, which a systemd unit's PATH and some non-login
# shells do not carry. A Jetson with Brave installed as a snap therefore looked
# like a machine with no browser at all.
case ":$PATH:" in *":/snap/bin:"*) ;; *) PATH="$PATH:/snap/bin" ;; esac

# Whatever Chromium is called on this machine. Firefox last: its kiosk mode
# works but ignores several of the flags below.
BROWSER=""
for candidate in brave brave-browser chromium chromium-browser google-chrome google-chrome-stable firefox; do
  if command -v "$candidate" >/dev/null 2>&1; then BROWSER="$candidate"; break; fi
done
if [ -z "$BROWSER" ]; then
  echo "kiosk: no browser found. Tried brave, chromium, google-chrome, firefox" >&2
  echo "kiosk: on PATH=$PATH" >&2
  echo "kiosk: install one with 'sudo snap install brave' or" >&2
  echo "kiosk: 'sudo apt install -y chromium-browser'" >&2
  exit 1
fi
echo "kiosk: $BROWSER -> $URL"

while true; do
  if [ "$BROWSER" = "firefox" ]; then
    "$BROWSER" --kiosk "$URL"
  else
    # --user-data-dir keeps this window's state (the third screen's camera
    # choice, the Monitor page's viewer mode) out of any browsing the machine is
    # also used for, and gives the flags a profile they actually apply to.
    # --noerrdialogs / --disable-session-crashed-bubble stop a "restore pages?"
    # prompt from covering the feed after an unclean shutdown, which on an
    # unattended display would stay there indefinitely.
    "$BROWSER" \
      --kiosk \
      --app="$URL" \
      --user-data-dir="$HOME/.config/$PROFILE" \
      --noerrdialogs \
      --disable-session-crashed-bubble \
      --disable-infobars \
      --no-first-run \
      --check-for-update-interval=31536000 \
      --autoplay-policy=no-user-gesture-required
  fi
  echo "kiosk: browser exited ($?), restarting in 5s" >&2
  sleep 5
done
