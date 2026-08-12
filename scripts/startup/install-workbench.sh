#!/usr/bin/env bash
#
# Make a Workbench come up ready without a keyboard: checks, then the webapp,
# then its own screen showing the status display.
#
#   sudo ./install-workbench.sh              # install / re-install
#   sudo ./install-workbench.sh --uninstall
#   ./install-workbench.sh --dry-run         # print what it would do
#
# The Workbench twin of install-rivet.sh. Separate rather than a flag on that
# one, because the two rigs differ in the part that matters most — the failure
# policy. A Rivet's preflight can fail the boot, because it applies WiFi and
# addressing fixes it owns and configuration that refuses to apply means the
# machine is not the one we think it is. A Workbench is wired end to end and
# owns no such fixes, so its preflight is purely observational and the webapp
# only Wants= it. Folding both into one script would have meant one flag
# silently changing whether a bad check stops the robot.
#
# Three pieces, deliberately separate so a failure names itself:
#   trossen-workbench-preflight.service  root, oneshot — link/arm checks, mute
#   trossen-webapp.service               the webapp, wants the preflight
#   ~/.config/autostart/…                the browser, needs a graphical session
#
# The browser is NOT a system unit. It needs the user's display and session bus,
# and a system service that pokes at those is a long argument with logind that
# an autostart entry simply does not have.
#
# Shares kiosk-browser.sh, kiosk-touch.sh and wait-for-url.sh with the Rivet:
# those are about screens and browsers, not about which rig this is.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="/etc/trossen/workbench.conf"
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
  echo "Removing the Workbench startup units. $CONF is left alone."
  run systemctl disable --now trossen-webapp.service || true
  run systemctl disable --now trossen-workbench-preflight.service || true
  run rm -f /etc/systemd/system/trossen-webapp.service \
            /etc/systemd/system/trossen-workbench-preflight.service
  run systemctl daemon-reload
  echo "Autostart entries (if any) are per-user:"
  echo "  rm ~/.config/autostart/trossen-kiosk.desktop ~/.config/autostart/trossen-touch.desktop"
  echo "  (and trossen-second-screen.desktop, this entry's name before the rename)"
  exit 0
fi

# --- config ------------------------------------------------------------------
CONF_CREATED=0
if [ ! -f "$CONF" ]; then
  echo "Installing $CONF from the example — FILL IT IN before rebooting."
  run install -d -m 0755 /etc/trossen
  run install -m 0644 "$HERE/workbench.conf.example" "$CONF"
  CONF_CREATED=1
else
  echo "$CONF exists; leaving it alone (this robot's values live there)."
fi

# Read it, so the units can be written with this robot's paths baked in.
REPO_DIR="/home/trossen/trossen_sdk"; RUN_USER="trossen"
WEBAPP_ARGS="--zed --no-realsense"; SCREEN_URL="http://localhost:8000/"
ARM_IFACES=""; ARM_ADDRS=""
# shellcheck source=/dev/null
[ -r "$CONF" ] && . "$CONF"

# The one mistake that does not announce itself. --rivet on a Workbench sets
# TROSSEN_ENABLE_RIVET=ON, CMake then clones the private trossen_base over
# HTTPS, and git blocks forever on a credential prompt no unit can answer. The
# unit sits in `activating` at near-zero CPU, indistinguishable from a slow
# compile, for as long as anyone is willing to wait.
case " $WEBAPP_ARGS " in
  *" --rivet "*)
    echo "ERROR: WEBAPP_ARGS in $CONF contains --rivet, which is for a Rivet." >&2
    echo "       On a Workbench it makes the webapp unit hang forever waiting" >&2
    echo "       on git credentials for the private trossen_base clone." >&2
    echo "       Remove it, or use install-rivet.sh if this really is a Rivet." >&2
    exit 2
    ;;
esac

if [ ! -x "$REPO_DIR/webapp/run-native.sh" ]; then
  echo "WARNING: $REPO_DIR/webapp/run-native.sh not found or not executable." >&2
  echo "         Set REPO_DIR in $CONF. Installing the units anyway." >&2
fi

# systemd gives a unit a minimal PATH — no ~/.local/bin — and that is where the
# astral installer puts `uv`. So the service failed with "uv is not installed"
# on a rig where the operator runs run-native.sh by hand every day. Resolve it
# through the user's own login shell and bake the directory into the unit.
UV_DIR=""
if id "$RUN_USER" >/dev/null 2>&1; then
  UV_PATH="$(runuser -u "$RUN_USER" -- bash -lc 'command -v uv' 2>/dev/null)" || UV_PATH=""
  [ -n "$UV_PATH" ] && UV_DIR="$(dirname "$UV_PATH")"
fi
if [ -n "$UV_DIR" ]; then
  echo "Found uv at $UV_PATH — adding $UV_DIR to the service PATH"
else
  echo "WARNING: could not find 'uv' for $RUN_USER. run-native.sh needs it." >&2
  echo "         Install with: curl -LsSf https://astral.sh/uv/install.sh | sh" >&2
fi
SERVICE_PATH="${UV_DIR:+$UV_DIR:}/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

# --- scripts -----------------------------------------------------------------
echo "Installing scripts to $LIB"
run install -d -m 0755 "$LIB"
for f in workbench-preflight.sh wait-for-url.sh kiosk-browser.sh kiosk-touch.sh; do
  run install -m 0755 "$HERE/$f" "$LIB/$f"
done

# --- units -------------------------------------------------------------------
echo "Installing systemd units"
if [ "$DRY" = "1" ]; then
  echo "  would: write /etc/systemd/system/trossen-workbench-preflight.service"
  echo "  would: write /etc/systemd/system/trossen-webapp.service"
else
  cat > /etc/systemd/system/trossen-workbench-preflight.service <<UNIT
[Unit]
Description=Trossen Workbench power-on checks
# NetworkManager brings the wired profiles up; the checks are meaningless until
# it has.
After=NetworkManager.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$LIB/workbench-preflight.sh
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
UNIT

  cat > /etc/systemd/system/trossen-webapp.service <<UNIT
[Unit]
Description=Trossen SDK webapp (native)
# Wants, NOT Requires — the difference from the Rivet, and the whole reason this
# installer is separate. The Workbench preflight only observes; it applies no fix
# that could refuse. A Workbench whose arms are powered off must still bring up
# its UI, because that is how the operator finds out the arms are off.
Wants=trossen-workbench-preflight.service
After=trossen-workbench-preflight.service
# Bound the retry loop. Restart=on-failure against a fault that is not going to
# clear on its own is an infinite loop that buries the first, real error.
StartLimitIntervalSec=300
StartLimitBurst=5

[Service]
Type=simple
User=$RUN_USER
WorkingDirectory=$REPO_DIR/webapp
# A unit does not read the user's shell profile, so anything installed under
# ~/.local/bin — uv, notably — is invisible without this. HOME/USER/SHELL come
# from the account database because User= is set, so they need no line here.
Environment=PATH=$SERVICE_PATH
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
run systemctl enable trossen-workbench-preflight.service
run systemctl enable trossen-webapp.service

# --- the screen --------------------------------------------------------------
# Installed for the account that will be logged in, which is RUN_USER, not root.
# `|| USER_HOME=""` is load-bearing: getent exits non-zero when the user does
# not exist, and under `set -e` that assignment ends the script — silently, with
# the units already installed and no autostart entry, which looks like success.
USER_HOME="$(getent passwd "$RUN_USER" 2>/dev/null | cut -d: -f6)" || USER_HOME=""
if [ -z "$USER_HOME" ]; then
  echo "SKIPPING the kiosk autostart entry: user '$RUN_USER' does not exist" >&2
  echo "  Set RUN_USER in $CONF to the account that logs in on the robot's screen." >&2
  AUTOSTART=""
else
  AUTOSTART="$USER_HOME/.config/autostart"
fi

if [ -n "$AUTOSTART" ]; then
echo "Installing the kiosk autostart entry for $RUN_USER"
# Renamed from trossen-second-screen.desktop, so remove that one explicitly.
# Both are autostart entries: a rig provisioned before the rename would
# otherwise keep it and launch a SECOND kiosk browser, and two fullscreen
# windows fighting over one display is not a state anyone diagnoses quickly.
if [ "$DRY" = "1" ]; then
  echo "  would: rm -f $AUTOSTART/trossen-second-screen.desktop (renamed)"
  echo "  would: write $AUTOSTART/trossen-kiosk.desktop -> $SCREEN_URL"
else
  install -d -m 0755 -o "$RUN_USER" -g "$RUN_USER" "$AUTOSTART"
  rm -f "$AUTOSTART/trossen-second-screen.desktop"
  cat > "$AUTOSTART/trossen-kiosk.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Trossen kiosk
Comment=The webapp, fullscreen on this robot's screen, once it answers
Exec=$LIB/kiosk-browser.sh $SCREEN_URL trossen-kiosk
X-GNOME-Autostart-enabled=true
DESKTOP
  chown "$RUN_USER:$RUN_USER" "$AUTOSTART/trossen-kiosk.desktop"
fi

# Separate entry from the browser, and ordered before it, because the two fail
# independently: a touchscreen 90° out is still worth fixing on a display whose
# browser did not start, and vice versa.
echo "Installing the touchscreen mapping autostart entry"
if [ "$DRY" = "1" ]; then
  echo "  would: write $AUTOSTART/trossen-touch.desktop"
else
  cat > "$AUTOSTART/trossen-touch.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Trossen touchscreen mapping
Comment=Bind the touch panel to its display, which X does not do by itself
Exec=$LIB/kiosk-touch.sh
X-GNOME-Autostart-enabled=true
DESKTOP
  chown "$RUN_USER:$RUN_USER" "$AUTOSTART/trossen-touch.desktop"
fi
fi

# kiosk-touch.sh reads the Rivet's conf path by default. Point it at this one so
# a Workbench's TOUCH_* settings are the ones that apply.
if [ "$DRY" != "1" ] && [ -n "$AUTOSTART" ]; then
  sed -i "s|^Exec=$LIB/kiosk-touch.sh\$|Exec=/usr/bin/env TROSSEN_RIVET_CONF=$CONF $LIB/kiosk-touch.sh|" \
    "$AUTOSTART/trossen-touch.desktop"
fi

echo
echo "Installed. What is left:"
echo

if [ "$CONF_CREATED" = "1" ] || [ -z "$ARM_ADDRS" ]; then
  echo "* FILL IN $CONF — ARM_IFACES and ARM_ADDRS are what this rig cannot be"
  echo "  guessed for. \`ip -4 -o addr show\` names the interfaces."
else
  echo "* $CONF looks complete (ARM_IFACES=\"$ARM_IFACES\")."
fi

# GDM's config lives in one of two places depending on the distro's packaging.
GDM_CONF=/etc/gdm3/custom.conf
[ -f "$GDM_CONF" ] || GDM_CONF=/etc/gdm/custom.conf
if [ -f "$GDM_CONF" ] && grep -qE '^\s*AutomaticLoginEnable\s*=\s*[Tt]rue' "$GDM_CONF" 2>/dev/null; then
  echo "* Autologin is already enabled in $GDM_CONF for $(grep -E '^\s*AutomaticLogin\s*=' "$GDM_CONF" | head -1 | cut -d= -f2- | tr -d ' ')."
else
  echo "* ENABLE AUTOLOGIN for $RUN_USER, or the screen stays on a login prompt and"
  echo "  the kiosk entry never runs:"
  echo "    [daemon]"
  echo "    AutomaticLoginEnable=true"
  echo "    AutomaticLogin=$RUN_USER"
fi

cat <<NEXT

Test the checks WITHOUT rebooting — nothing depends on the result:

  sudo systemctl start trossen-workbench-preflight
  journalctl -u trossen-workbench-preflight -b --no-pager | tail -25

Then the webapp (first start compiles the SDK extension — minutes on an Orin):

  sudo systemctl start trossen-webapp
  journalctl -u trossen-webapp -f

Back out at any point with:
  sudo ./install-workbench.sh --uninstall
NEXT
