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

PROFILE_DIR="$HOME/.config/$PROFILE"

# Is this browser a snap? It matters more than it looks. A snap runs with $HOME
# remapped to ~/snap/<name>/<rev>, so --user-data-dir pointing at ~/.config/...
# is silently ignored — the browser uses its own default profile instead, in a
# directory we were not looking at. That is not worth fighting: for a kiosk the
# default profile is fine, since nothing else uses this browser. So we stop
# passing the flag, and go looking for locks where they actually are.
IS_SNAP=0
case "$(readlink -f "$(command -v "$BROWSER")" 2>/dev/null)" in
  /snap/*) IS_SNAP=1 ;;
esac
[ -x "/snap/bin/$BROWSER" ] && [ "$(command -v "$BROWSER")" = "/snap/bin/$BROWSER" ] && IS_SNAP=1
if [ "$IS_SNAP" = "1" ]; then
  echo "kiosk: $BROWSER is a snap — using its default profile (--user-data-dir is"
  echo "kiosk: ignored under confinement, so isolating state that way is a fiction)"
fi

# Chromium-family browsers lock a profile with a SingletonLock symlink whose
# target embeds the HOSTNAME and pid. Rename the machine — which every rig gets
# on setup, trossen-agx202-2 -> rivet-01 — and the lock now names "another
# computer", so the browser refuses to start, prints that it has locked the
# profile to avoid corruption, and EXITS ZERO. A kiosk then restarts it forever
# on a five-second timer and the screen stays empty.
#
# Safe to clear here because this profile belongs to this script alone: nothing
# else is ever pointed at it. The pgrep guard is for the one case that is not
# stale — a browser genuinely running on this profile right now.
clear_stale_lock() {
  # Anything actually running means the lock is real, not stale.
  if pgrep -x "$BROWSER" >/dev/null 2>&1 || pgrep -f -- "--user-data-dir=$PROFILE_DIR" >/dev/null 2>&1; then
    return 0
  fi
  # Both places a profile can be: ours when the flag works, and the snap's own
  # when it does not. Bounded depth so this cannot wander the whole home dir.
  local root
  for root in "$PROFILE_DIR" "$HOME/snap/$BROWSER"; do
    [ -d "$root" ] || continue
    while IFS= read -r lock; do
      [ -n "$lock" ] || continue
      rm -f "$lock" "${lock%Lock}Socket" "${lock%Lock}Cookie" &&
        echo "kiosk: cleared stale profile lock at $lock"
    done < <(find "$root" -maxdepth 6 -name SingletonLock 2>/dev/null)
  done
}

fast_failures=0
while true; do
  clear_stale_lock
  started_at=$SECONDS
  if [ "$BROWSER" = "firefox" ]; then
    "$BROWSER" --kiosk "$URL"
  else
    # --user-data-dir keeps this window's state (the third screen's camera
    # choice, the Monitor page's viewer mode) out of any browsing the machine is
    # also used for, and gives the flags a profile they actually apply to.
    # --noerrdialogs / --disable-session-crashed-bubble stop a "restore pages?"
    # prompt from covering the feed after an unclean shutdown, which on an
    # unattended display would stay there indefinitely.
    # A snap ignores --user-data-dir, so passing it only obscures which profile
    # is really in use when something goes wrong.
    profile_flag=()
    [ "$IS_SNAP" = "0" ] && profile_flag=(--user-data-dir="$PROFILE_DIR")
    "$BROWSER" \
      --kiosk \
      --app="$URL" \
      "${profile_flag[@]}" \
      --noerrdialogs \
      --disable-session-crashed-bubble \
      --disable-infobars \
      --no-first-run \
      --check-for-update-interval=31536000 \
      --autoplay-policy=no-user-gesture-required
  fi
  status=$?
  ran_for=$((SECONDS - started_at))
  # A browser that dies immediately, over and over, is not a browser being
  # closed — it is one that cannot start. Keep retrying (a kiosk with nobody in
  # front of it must heal itself), but say so loudly enough that the reason is
  # findable in the journal instead of buried under identical restart lines.
  if [ "$ran_for" -lt 15 ]; then
    fast_failures=$((fast_failures + 1))
  else
    fast_failures=0
  fi
  if [ "$fast_failures" -ge 3 ]; then
    echo "kiosk: $BROWSER has exited within 15s, $fast_failures times running." >&2
    echo "kiosk: it is failing to start, not being closed. Look ABOVE this line" >&2
    echo "kiosk: for the browser's own error; a stale profile lock and a missing" >&2
    echo "kiosk: display are the usual two. Profile: $PROFILE_DIR" >&2
    fast_failures=0
  fi
  echo "kiosk: browser exited ($status) after ${ran_for}s, restarting in 5s" >&2
  sleep 5
done
