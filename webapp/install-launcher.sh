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
ICONS_DIR="$HOME/.local/share/icons"
DESKTOP_DIR="$(xdg-user-dir DESKTOP 2>/dev/null || echo "$HOME/Desktop")"
DESKTOP_FILE="trossen-webapp.desktop"
# Source icon shipped in the repo. Prefer the vector (svg); fall back to the
# PNG. We copy it into a location we own and point the .desktop at that, so the
# icon renders on every machine independent of the repo's location.
ICON_SRC="$SCRIPT_DIR/frontend/public/icon.svg"
[ -f "$ICON_SRC" ] || ICON_SRC="$SCRIPT_DIR/frontend/public/icon-512.png"
ICON="$ICONS_DIR/trossen-webapp.${ICON_SRC##*.}"

if [ "${1:-}" = "--uninstall" ]; then
  rm -f "$APPS_DIR/$DESKTOP_FILE" "$DESKTOP_DIR/$DESKTOP_FILE" \
        "$ICONS_DIR/trossen-webapp.svg" "$ICONS_DIR/trossen-webapp.png"
  update-desktop-database "$APPS_DIR" 2>/dev/null || true
  echo "Removed launcher."
  exit 0
fi

chmod +x "$LAUNCHER"
mkdir -p "$APPS_DIR" "$ICONS_DIR"

# Copy the repo-bundled icon into our own location.
cp -f "$ICON_SRC" "$ICON"

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
Exec=/bin/bash -c "cd '$SCRIPT_DIR' && docker compose down && notify-send -i '$ICON' 'Trossen Webapp' 'Stopped'"
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
