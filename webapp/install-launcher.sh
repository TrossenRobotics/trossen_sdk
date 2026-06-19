#!/usr/bin/env bash
#
# Installs a desktop launcher for the Trossen SDK webapp so it can be started
# from the GNOME app grid / dock like any other application.
#
#   ./install-launcher.sh            # install (app grid + Desktop)
#   ./install-launcher.sh --uninstall
#
# The launcher points at launch-webapp.sh in this same directory, so the repo
# can live anywhere — paths are resolved at install time.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAUNCHER="$SCRIPT_DIR/launch-webapp.sh"
APPS_DIR="$HOME/.local/share/applications"
DESKTOP_DIR="$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")"
DESKTOP_FILE="trossen-webapp.desktop"

# Pick an icon: the SDK's installed one if present, else the frontend PWA icon.
ICON="$HOME/.local/share/icons/trossen_ai.svg"
[ -f "$ICON" ] || ICON="$SCRIPT_DIR/frontend/public/icon-512.png"

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$APPS_DIR/$DESKTOP_FILE" "$DESKTOP_DIR/$DESKTOP_FILE"
  update-desktop-database "$APPS_DIR" 2>/dev/null || true
  echo "Removed launcher."
  exit 0
fi

chmod +x "$LAUNCHER"
mkdir -p "$APPS_DIR"

write_desktop() {
  cat >"$1" <<EOF
[Desktop Entry]
Version=1.0
Type=Application
Name=Trossen Webapp
Comment=Start the Trossen SDK data-collection webapp
Exec=$LAUNCHER
Icon=$ICON
Terminal=false
Categories=Utility;
Actions=Stop;

[Desktop Action Stop]
Name=Stop webapp
Exec=/bin/bash -c "cd '$SCRIPT_DIR' && docker compose down && notify-send -i trossen_ai 'Trossen Webapp' 'Stopped'"
EOF
  chmod +x "$1"
}

write_desktop "$APPS_DIR/$DESKTOP_FILE"
update-desktop-database "$APPS_DIR" 2>/dev/null || true

# Also drop a copy on the Desktop, and mark it trusted so GNOME runs it on
# double-click without the "Allow Launching" prompt (gio is best-effort).
if [ -d "$DESKTOP_DIR" ]; then
  write_desktop "$DESKTOP_DIR/$DESKTOP_FILE"
  gio set "$DESKTOP_DIR/$DESKTOP_FILE" metadata::trusted true 2>/dev/null || true
fi

echo "Installed launcher:"
echo "  app grid : $APPS_DIR/$DESKTOP_FILE"
[ -d "$DESKTOP_DIR" ] && echo "  desktop  : $DESKTOP_DIR/$DESKTOP_FILE"
echo "  launcher : $LAUNCHER"
echo "  icon     : $ICON"
echo
echo "Look for \"Trossen Webapp\" in your apps. Right-click the icon for \"Stop webapp\"."
