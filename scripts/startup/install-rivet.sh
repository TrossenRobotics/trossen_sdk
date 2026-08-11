#!/usr/bin/env bash
#
# Make a Rivet come up ready without a keyboard: checks, then the webapp, then
# its own screen showing the status display.
#
#   sudo ./install-rivet.sh              # install / re-install
#   sudo ./install-rivet.sh --uninstall
#   ./install-rivet.sh --dry-run         # print what it would do
#
# Three pieces, deliberately separate so a failure names itself:
#   trossen-rivet-preflight.service  root, oneshot — network fixes, checks, mute
#   trossen-webapp.service           the webapp, requires the preflight
#   ~/.config/autostart/…            the browser, needs a graphical session
#
# The browser is NOT a system unit. It needs the user's display and session bus,
# and a system service that pokes at those is a long argument with logind that
# an autostart entry simply does not have.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="/etc/trossen/rivet.conf"
LIB="/usr/local/lib/trossen"
DRY=0
UNINSTALL=0

for arg in "$@"; do
  case "$arg" in
    --dry-run)   DRY=1 ;;
    --uninstall) UNINSTALL=1 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

run() {
  if [ "$DRY" = "1" ]; then printf '  would: %s\n' "$*"; else "$@"; fi
}

if [ "$DRY" != "1" ] && [ "$(id -u)" -ne 0 ]; then
  echo "must run as root (installs systemd units); use --dry-run to preview" >&2
  exit 1
fi

if [ "$UNINSTALL" = "1" ]; then
  echo "Removing the Rivet startup units. $CONF is left alone."
  run systemctl disable --now trossen-webapp.service || true
  run systemctl disable --now trossen-rivet-preflight.service || true
  run rm -f /etc/systemd/system/trossen-webapp.service \
            /etc/systemd/system/trossen-rivet-preflight.service
  run systemctl daemon-reload
  echo "Autostart entry (if any) is per-user: rm ~/.config/autostart/trossen-second-screen.desktop"
  exit 0
fi

# --- config ------------------------------------------------------------------
if [ ! -f "$CONF" ]; then
  echo "Installing $CONF from the example — FILL IT IN before rebooting."
  run install -d -m 0755 /etc/trossen
  run install -m 0644 "$HERE/rivet.conf.example" "$CONF"
else
  echo "$CONF exists; leaving it alone (this robot's values live there)."
fi

# Read it, so the units can be written with this robot's paths baked in.
REPO_DIR="/home/trossen/trossen_sdk"; RUN_USER="trossen"
WEBAPP_ARGS="--rivet --zed --no-realsense"; SCREEN_URL="http://localhost:8000/second_screen"
# shellcheck source=/dev/null
[ -r "$CONF" ] && . "$CONF"

if [ ! -x "$REPO_DIR/webapp/run-native.sh" ]; then
  echo "WARNING: $REPO_DIR/webapp/run-native.sh not found or not executable." >&2
  echo "         Set REPO_DIR in $CONF. Installing the units anyway." >&2
fi

# --- scripts -----------------------------------------------------------------
echo "Installing scripts to $LIB"
run install -d -m 0755 "$LIB"
for f in rivet-preflight.sh wait-for-url.sh kiosk-browser.sh; do
  run install -m 0755 "$HERE/$f" "$LIB/$f"
done

# --- units -------------------------------------------------------------------
echo "Installing systemd units"
if [ "$DRY" = "1" ]; then
  echo "  would: write /etc/systemd/system/trossen-rivet-preflight.service"
  echo "  would: write /etc/systemd/system/trossen-webapp.service"
else
  cat > /etc/systemd/system/trossen-rivet-preflight.service <<UNIT
[Unit]
Description=Trossen Rivet power-on checks
# NetworkManager must be up to be configured, but network-online.target is
# deliberately only Wanted: on a rig with no AP in range it never arrives, and
# waiting for it would hold the robot down for a network it does not need.
After=NetworkManager.service
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$LIB/rivet-preflight.sh
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
UNIT

  cat > /etc/systemd/system/trossen-webapp.service <<UNIT
[Unit]
Description=Trossen SDK webapp (native)
# Requires, not Wants: a preflight that failed means a fix we own would not
# apply, and the chosen policy is to stop rather than record data nobody can
# reach. Start this by hand to override for one boot.
Requires=trossen-rivet-preflight.service
After=trossen-rivet-preflight.service

[Service]
Type=simple
User=$RUN_USER
WorkingDirectory=$REPO_DIR/webapp
ExecStart=$REPO_DIR/webapp/run-native.sh $WEBAPP_ARGS
# The first start compiles the SDK extension, which is slow on an Orin and must
# not be mistaken for a hang.
TimeoutStartSec=1800
Restart=on-failure
RestartSec=10
# A recording in flight owns the arms. Give it room to stop them cleanly rather
# than leaving them energised on a SIGKILL.
KillSignal=SIGINT
TimeoutStopSec=60

[Install]
WantedBy=multi-user.target
UNIT
fi

run systemctl daemon-reload
run systemctl enable trossen-rivet-preflight.service
run systemctl enable trossen-webapp.service

# --- the screen --------------------------------------------------------------
# Installed for the account that will be logged in, which is RUN_USER, not root.
USER_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
AUTOSTART="$USER_HOME/.config/autostart"
echo "Installing the second-screen autostart entry for $RUN_USER"
if [ "$DRY" = "1" ]; then
  echo "  would: write $AUTOSTART/trossen-second-screen.desktop -> $SCREEN_URL"
else
  install -d -m 0755 -o "$RUN_USER" -g "$RUN_USER" "$AUTOSTART"
  cat > "$AUTOSTART/trossen-second-screen.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Trossen second screen
Comment=Robot status display, fullscreen, once the webapp answers
Exec=$LIB/kiosk-browser.sh $SCREEN_URL trossen-second-screen
X-GNOME-Autostart-enabled=true
DESKTOP
  chown "$RUN_USER:$RUN_USER" "$AUTOSTART/trossen-second-screen.desktop"
fi

cat <<NEXT

Installed. Two things are NOT done automatically, on purpose:

1. FILL IN $CONF — WIFI_CONN, WIFI_BSSID, STATIC_IP. Without them the preflight
   reports what it cannot do and carries on; it does not guess.

2. ENABLE AUTOLOGIN for $RUN_USER, or the screen stays on a login prompt and the
   autostart entry never runs. On a Jetson with GDM:

     sudo nano /etc/gdm3/custom.conf     # or /etc/gdm/custom.conf
       [daemon]
       AutomaticLoginEnable=true
       AutomaticLogin=$RUN_USER

   Left manual because it weakens physical security on a machine that may not
   be yours to make that call about.

Then:  sudo reboot
Check: systemctl status trossen-rivet-preflight trossen-webapp
       journalctl -u trossen-rivet-preflight -b
NEXT
