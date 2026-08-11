#!/usr/bin/env bash
#
# Turn an ordinary Linux desktop (Ubuntu/GNOME) into a display that boots
# straight into one page — the third screen, normally.
#
#   ./install-desktop-kiosk.sh --url 'http://192.168.5.33:8000/third_screen?camera=camera_main'
#   ./install-desktop-kiosk.sh --url … --autologin      # also enable GDM autologin
#   ./install-desktop-kiosk.sh --uninstall
#
# Unlike the Pi, this machine HAS a desktop session, so there is no cage and no
# console block: GNOME logs in and runs an autostart entry, which is the least
# surprising thing on a box someone may also use normally.
#
# Pin the camera in the URL. `?camera=<stream_id>` is what makes the display
# come back to the same feed after a reboot, and the on-screen picker
# deliberately does not overwrite it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
URL=""
AUTOLOGIN=0
UNINSTALL=0
LIB="$HOME/.local/lib/trossen"
ENTRY="$HOME/.config/autostart/trossen-third-screen.desktop"

while [ $# -gt 0 ]; do
  case "$1" in
    --url)       URL="${2:?--url needs a value}"; shift 2 ;;
    --autologin) AUTOLOGIN=1; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [ "$(id -u)" -eq 0 ]; then
  echo "Run as the display USER, not root — the autostart entry is per-user." >&2
  exit 1
fi

if [ "$UNINSTALL" = "1" ]; then
  rm -f "$ENTRY"
  echo "Removed $ENTRY. Autologin, if it was enabled, is left as it is."
  exit 0
fi

[ -n "$URL" ] || { echo "--url is required" >&2; exit 2; }

install -d -m 0755 "$LIB"
install -m 0755 "$HERE/kiosk-browser.sh" "$HERE/wait-for-url.sh" "$LIB/"

install -d -m 0755 "$(dirname "$ENTRY")"
cat > "$ENTRY" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Trossen third screen
Comment=Camera display, fullscreen, once the webapp answers
Exec=$LIB/kiosk-browser.sh $URL trossen-third-screen
X-GNOME-Autostart-enabled=true
DESKTOP

echo "Installed autostart entry: $ENTRY"
echo "  URL $URL"

if [ "$AUTOLOGIN" = "1" ]; then
  CUSTOM=/etc/gdm3/custom.conf
  [ -f "$CUSTOM" ] || CUSTOM=/etc/gdm/custom.conf
  if [ -f "$CUSTOM" ]; then
    echo "Enabling GDM autologin for $USER in $CUSTOM (sudo)"
    sudo cp -n "$CUSTOM" "$CUSTOM.trossen.bak" || true
    # Idempotent: rewrite the two keys if present, add them under [daemon] if not.
    sudo python3 - "$CUSTOM" "$USER" <<'PY'
import re, sys
path, user = sys.argv[1], sys.argv[2]
text = open(path).read()
keys = {"AutomaticLoginEnable": "true", "AutomaticLogin": user}
for key, value in keys.items():
    line = f"{key}={value}"
    if re.search(rf"^\s*#?\s*{key}\s*=.*$", text, re.M):
        text = re.sub(rf"^\s*#?\s*{key}\s*=.*$", line, text, count=1, flags=re.M)
    elif "[daemon]" in text:
        text = text.replace("[daemon]", f"[daemon]\n{line}", 1)
    else:
        text += f"\n[daemon]\n{line}\n"
open(path, "w").write(text)
print(f"  set {', '.join(f'{k}={v}' for k, v in keys.items())}")
PY
  else
    echo "No GDM custom.conf found — enable autologin through Settings > Users." >&2
  fi
else
  echo
  echo "Autologin NOT enabled. Without it this machine stops at a login prompt and"
  echo "the display stays dark until someone types a password. Re-run with"
  echo "--autologin, or set it in Settings > Users > Automatic Login."
fi

echo
echo "Reboot to test. The browser waits for the webapp before opening, so a"
echo "display that comes up before the robot does is not a problem."
