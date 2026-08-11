#!/usr/bin/env bash
#
# Turn a Raspberry Pi into a display that boots straight into the webapp.
#
#   ./install-pi-kiosk.sh --url http://192.168.5.33:8000/
#   ./install-pi-kiosk.sh --url … --install-missing   # also apt-get the packages
#   ./install-pi-kiosk.sh --uninstall
#
# This is webapp/PI_KIOSK.md as a script. That document stays the explanation —
# why cage, why Pi OS Lite, what each flag is for; this only does the steps, so
# a second Pi does not depend on someone reading carefully.
#
# NOT a systemd unit, unlike the Rivet: cage needs a seat, and a seat comes from
# a real console login. Console autologin plus a block in ~/.bash_profile is the
# arrangement PI_KIOSK.md verified end to end, so it is the one reproduced here.
set -euo pipefail

URL=""
INSTALL_MISSING=0
UNINSTALL=0
MARK_BEGIN="# >>> trossen kiosk >>>"
MARK_END="# <<< trossen kiosk <<<"
PROFILE="$HOME/.bash_profile"

while [ $# -gt 0 ]; do
  case "$1" in
    --url)             URL="${2:?--url needs a value}"; shift 2 ;;
    --install-missing) INSTALL_MISSING=1; shift ;;
    --uninstall)       UNINSTALL=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ "$(id -u)" -eq 0 ]; then
  echo "Run as the kiosk USER, not root — it edits that user's ~/.bash_profile." >&2
  echo "It will call sudo for the two system-level steps." >&2
  exit 1
fi

# The block is delimited by markers so re-running replaces it instead of
# stacking a second copy — which would launch two browsers onto one screen.
strip_block() {
  [ -f "$PROFILE" ] || return 0
  sed -i "/^${MARK_BEGIN}$/,/^${MARK_END}$/d" "$PROFILE"
}

if [ "$UNINSTALL" = "1" ]; then
  strip_block
  echo "Removed the kiosk block from $PROFILE."
  echo "Autologin is left as it is: sudo raspi-config nonint do_boot_behaviour B1  # console, manual login"
  exit 0
fi

[ -n "$URL" ] || { echo "--url is required, e.g. --url http://192.168.5.33:8000/" >&2; exit 2; }

# --- packages ----------------------------------------------------------------
# Font included on purpose: the UI asks for JetBrains Mono and ships no webfont,
# so without it every screen renders in a fallback and looks broken.
PKGS=(cage chromium fonts-jetbrains-mono fonts-dejavu-core)
missing=()
for p in "${PKGS[@]}"; do
  dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done
if [ "${#missing[@]}" -gt 0 ]; then
  if [ "$INSTALL_MISSING" = "1" ]; then
    echo "Installing: ${missing[*]}"
    sudo apt-get update
    sudo apt-get install -y "${missing[@]}"
  else
    echo "MISSING PACKAGES: ${missing[*]}"
    echo "Install them, or re-run with --install-missing. Continuing."
  fi
fi

# --- the console session -----------------------------------------------------
echo "Enabling console autologin (B2)"
sudo raspi-config nonint do_boot_behaviour B2 || \
  echo "raspi-config failed — set console autologin by hand, or the kiosk never starts."

# cage needs these to drive the display and read input directly, with no
# desktop underneath it.
sudo usermod -aG video,render,input,tty "$USER" || true

# --- the kiosk block ---------------------------------------------------------
CHROMIUM="$(command -v chromium || command -v chromium-browser || echo /usr/bin/chromium)"

strip_block
cat >> "$PROFILE" <<BLOCK
$MARK_BEGIN
# Managed by scripts/startup/install-pi-kiosk.sh. Edit the URL here, or re-run
# that script. Everything between the markers is replaced on re-install.
#
# Only on the physical console: over SSH there is no seat, cage fails, and
# without this guard every ssh login would try to start a browser.
if [ "\$(tty)" = "/dev/tty1" ]; then
  # WLR_RENDERER=pixman forces software rendering. Fine for these pages — they
  # composite images and text — and necessary on a Pi with no usable GPU path.
  export WLR_RENDERER=pixman
  exec cage -s -- "$CHROMIUM" --kiosk --app="$URL" \\
    --user-data-dir="\$HOME/.config/trossen-kiosk" \\
    --noerrdialogs --disable-session-crashed-bubble --disable-infobars \\
    --no-first-run --check-for-update-interval=31536000
fi
$MARK_END
BLOCK

cat <<NEXT

Installed for user $USER.

  URL      $URL
  browser  $CHROMIUM
  profile  $PROFILE  (block between the trossen kiosk markers)

Reboot to test: sudo reboot

If the screen stays black, the two usual causes are in PI_KIOSK.md's
troubleshooting table — a syntax error in $PROFILE (check with
\`bash -n $PROFILE\` BEFORE rebooting, a broken one kills the tty1 login), and
cage being run over SSH where it has no seat.

Ctrl-Alt-F2 gets you a second console if the kiosk misbehaves.
NEXT
