#!/usr/bin/env bash
#
# Provision a Rivet from a fresh checkout to boot-to-ready, in one run.
#
#   sudo ./setup-rivet.sh --hostname rivet-02 --static-ip 192.168.5.34/24
#   sudo ./setup-rivet.sh --hostname rivet-02 --static-ip 192.168.5.34/24 \
#        --ts-authkey tskey-auth-… --ts-tag tag:shared
#   sudo ./setup-rivet.sh --dry-run --hostname rivet-02 --static-ip 192.168.5.34/24
#
# Run it ON the robot, from the repo checkout, over a wired link or the console
# if you can — step 4 re-activates WiFi and will drop an SSH session on that
# interface. Under tmux it survives; `robots shell <host>` gives you one.
#
# What it does, in order, each step skippable and each idempotent:
#   1  hostname            so the rig can be found by name, not by lease
#   2  WiFi discovery      profile, 5 GHz BSSID, interface — read, not guessed
#   3  /etc/trossen/rivet.conf
#   4  network fixes       power save off, AP pinned, static address, boot race
#   5  browser             for the robot's own screen
#   6  autologin           or the screen sits at a login prompt forever
#   7  units + autostart   via install-rivet.sh
#   8  tailscale           optional, needs an auth key
#   9  verify              start the preflight and report
#
# Everything it cannot decide — which AP, which address, whether to enrol in a
# tailnet — is a flag. It guesses nothing that would be wrong on the next rig.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="/etc/trossen/rivet.conf"

HOSTNAME_WANT=""
STATIC_IP=""
BSSID_WANT="auto"
CONN_WANT=""
TS_AUTHKEY=""
TS_TAG="tag:shared"
RUN_USER_WANT="trossen"
DRY=0
DO_AUTOLOGIN=1
DO_BROWSER=1
DO_NETFIX=1

usage() { sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
  case "$1" in
    --hostname)    HOSTNAME_WANT="${2:?}"; shift 2 ;;
    --static-ip)   STATIC_IP="${2:?}"; shift 2 ;;
    --bssid)       BSSID_WANT="${2:?}"; shift 2 ;;
    --conn)        CONN_WANT="${2:?}"; shift 2 ;;
    --user)        RUN_USER_WANT="${2:?}"; shift 2 ;;
    --ts-authkey)  TS_AUTHKEY="${2:?}"; shift 2 ;;
    --ts-tag)      TS_TAG="${2:?}"; shift 2 ;;
    --no-autologin) DO_AUTOLOGIN=0; shift ;;
    --no-browser)  DO_BROWSER=0; shift ;;
    --no-netfix)   DO_NETFIX=0; shift ;;
    --dry-run)     DRY=1; shift ;;
    -h|--help)     usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

step() { printf '\n=== %s\n' "$*"; }
say()  { printf '    %s\n' "$*"; }
run()  { if [ "$DRY" = "1" ]; then printf '    would: %s\n' "$*"; else "$@"; fi; }

if [ "$DRY" != "1" ] && [ "$(id -u)" -ne 0 ]; then
  echo "must run as root; use --dry-run to preview" >&2
  exit 1
fi

[ -n "$HOSTNAME_WANT" ] || { echo "--hostname is required (e.g. rivet-02)" >&2; exit 2; }
case "$HOSTNAME_WANT" in
  workbench-[0-9][0-9]|rivet-[0-9][0-9]|stationary-[0-9][0-9]|cockpit-[0-9][0-9]|payload-[0-9][0-9]) ;;
  *) echo "--hostname must be <type>-<nn>, e.g. rivet-02: the ssh config keys on that" >&2
     echo "pattern, and a name outside it skips the rig block entirely." >&2
     exit 2 ;;
esac

# --- 1. name -----------------------------------------------------------------
step "1. hostname"
CURRENT_HOST="$(hostname)"
if [ "$CURRENT_HOST" = "$HOSTNAME_WANT" ]; then
  say "already $HOSTNAME_WANT"
else
  say "$CURRENT_HOST -> $HOSTNAME_WANT"
  run hostnamectl set-hostname "$HOSTNAME_WANT"
  # /etc/hosts keeps the old name on the loopback line; sudo warns about an
  # unresolvable host on every call until it is fixed.
  run sed -i "s/\b$CURRENT_HOST\b/$HOSTNAME_WANT/g" /etc/hosts
fi

# --- 2. what the radio is actually doing -------------------------------------
step "2. WiFi discovery"
WIFI_IFACE="$(nmcli -t -f DEVICE,TYPE device status 2>/dev/null | awk -F: '$2=="wifi"{print $1; exit}')"
say "interface : ${WIFI_IFACE:-NONE FOUND}"

WIFI_CONN="$CONN_WANT"
if [ -z "$WIFI_CONN" ]; then
  WIFI_CONN="$(nmcli -t -f NAME,DEVICE connection show --active 2>/dev/null |
               awk -F: -v d="$WIFI_IFACE" '$2==d{print $1; exit}')"
fi
say "profile   : ${WIFI_CONN:-NONE ACTIVE — connect to the network first}"

WIFI_BSSID=""
if [ "$BSSID_WANT" = "auto" ]; then
  # The 5 GHz BSSID for the SSID we are on, strongest first. Not "the AP we are
  # associated to": that may be the 2.4 GHz radio of the same AP, and pinning it
  # would lock the rig to the slower band forever.
  SSID_NOW="$(nmcli -t -f 802-11-wireless.ssid connection show "$WIFI_CONN" 2>/dev/null | cut -d: -f2-)"
  # 5 GHz means 5000-5900 MHz and nothing else. Without the upper bound a 6 GHz
  # radio (5955-7115) satisfies ">= 5000" and gets pinned instead — which is how
  # a dry run on the laptop chose …B5:93, the 6E radio of the same AP, over the
  # 5 GHz …B5:92 that was actually wanted. 6 GHz also carries less far, so
  # pinning it on a robot that moves is a worse trade than it looks.
  WIFI_BSSID="$(nmcli -t -f BSSID,FREQ,SIGNAL,SSID device wifi list 2>/dev/null |
                sed 's/\\:/-/g' |
                awk -F: -v s="$SSID_NOW" '$4==s && $2+0 >= 5000 && $2+0 < 5900 {print $3, $1}' |
                sort -rn | head -1 | awk '{print $2}' | tr '-' ':')"
  say "5GHz AP   : ${WIFI_BSSID:-none found for \"$SSID_NOW\" — leaving unpinned}"
else
  WIFI_BSSID="$BSSID_WANT"
  say "5GHz AP   : $WIFI_BSSID (given)"
fi

GW="$(ip -4 route show default 2>/dev/null | awk '{print $3; exit}')"
DNS="$(nmcli -t -f ipv4.dns connection show "$WIFI_CONN" 2>/dev/null | cut -d: -f2- | tr ',;' ' ')"
[ -n "$DNS" ] || DNS="$GW"
[ -n "$STATIC_IP" ] || STATIC_IP="$(ip -4 -o addr show dev "$WIFI_IFACE" 2>/dev/null | awk '{print $4; exit}')"
say "gateway   : ${GW:-unknown}"
say "dns       : ${DNS:-unknown}"
say "address   : ${STATIC_IP:-unknown}"

ARM_IFACE="$(nmcli -t -f DEVICE,TYPE,STATE device status 2>/dev/null |
             awk -F: '$2=="ethernet" && $3=="connected"{print $1; exit}')"
if [ -n "$ARM_IFACE" ]; then
  say "arm link  : $ARM_IFACE"
else
  say "arm link  : NONE CONNECTED. The arms and base live on a wired subnet; with"
  say "            nothing plugged in, ARM_SUBNET_IFACE is left empty and the"
  say "            preflight cannot check it. Plug the switch in and re-run, or"
  say "            set ARM_SUBNET_IFACE in $CONF by hand."
fi

# --- 3. config ---------------------------------------------------------------
step "3. $CONF"
if [ "$DRY" = "1" ]; then
  say "would write:"
  say "  WIFI_IFACE=$WIFI_IFACE  WIFI_CONN=$WIFI_CONN  WIFI_BSSID=$WIFI_BSSID"
  say "  STATIC_IP=$STATIC_IP  STATIC_GW=$GW  ARM_SUBNET_IFACE=$ARM_IFACE"
else
  install -d -m 0755 /etc/trossen
  [ -f "$CONF" ] && cp -a "$CONF" "$CONF.bak.$$" && say "kept a copy at $CONF.bak.$$"
  # Start from the shipped example so the comments come along, then set values.
  install -m 0644 "$HERE/rivet.conf.example" "$CONF"
  set_conf() {
    if grep -qE "^$1=" "$CONF"; then
      sed -i "s|^$1=.*|$1=\"$2\"|" "$CONF"
    else
      printf '%s="%s"\n' "$1" "$2" >> "$CONF"
    fi
  }
  set_conf WIFI_IFACE "$WIFI_IFACE"
  set_conf WIFI_CONN "$WIFI_CONN"
  set_conf WIFI_BSSID "$WIFI_BSSID"
  set_conf STATIC_IP "$STATIC_IP"
  set_conf STATIC_GW "$GW"
  set_conf STATIC_DNS "$DNS"
  set_conf ARM_SUBNET_IFACE "$ARM_IFACE"
  set_conf RUN_USER "$RUN_USER_WANT"
  set_conf REPO_DIR "$(cd "$HERE/../.." && pwd)"
  say "written"
fi

# --- 4. the network fixes ----------------------------------------------------
step "4. network fixes"
if [ "$DO_NETFIX" = "0" ]; then
  say "skipped (--no-netfix)"
elif [ -z "$WIFI_CONN" ]; then
  say "no active profile — skipping. Connect to the network, then re-run."
else
  say "power save off, AP pinned, static address, and this profile wins at boot"
  run nmcli connection modify "$WIFI_CONN" wifi.powersave 2
  [ -n "$WIFI_BSSID" ] && run nmcli connection modify "$WIFI_CONN" 802-11-wireless.bssid "$WIFI_BSSID"
  [ -n "$STATIC_IP" ] && run nmcli connection modify "$WIFI_CONN" \
    ipv4.method manual ipv4.addresses "$STATIC_IP" ipv4.gateway "$GW" ipv4.dns "$DNS"
  run nmcli connection modify "$WIFI_CONN" connection.autoconnect yes
  run nmcli connection modify "$WIFI_CONN" connection.autoconnect-priority 100

  # Every OTHER wireless profile is a way for the rig to come up somewhere you
  # are not looking. Duplicates for this same SSID are worse: the fixes above
  # are applied to one profile by name, so a duplicate that wins at boot makes
  # them invisible.
  nmcli -t -f NAME,TYPE connection show 2>/dev/null |
    awk -F: '$2=="802-11-wireless"' | cut -d: -f1 |
    while IFS= read -r other; do
      [ "$other" = "$WIFI_CONN" ] && continue
      say "  demoting '$other' (autoconnect off)"
      run nmcli connection modify "$other" connection.autoconnect no
    done

  run nmcli connection up "$WIFI_CONN"
  run iw dev "$WIFI_IFACE" set power_save off
fi

# --- 5. browser --------------------------------------------------------------
step "5. browser for the robot's screen"
export PATH="$PATH:/snap/bin"
FOUND_BROWSER=""
for b in brave brave-browser chromium chromium-browser google-chrome firefox; do
  command -v "$b" >/dev/null 2>&1 && { FOUND_BROWSER="$b"; break; }
done
if [ -n "$FOUND_BROWSER" ]; then
  say "already have $FOUND_BROWSER ($(command -v "$FOUND_BROWSER"))"
elif [ "$DO_BROWSER" = "0" ]; then
  say "none found, skipped (--no-browser). The screen will stay blank."
elif command -v snap >/dev/null 2>&1; then
  say "installing brave (snap)"
  run snap install brave
else
  say "installing chromium (apt)"
  run apt-get install -y chromium-browser
fi

# --- 6. autologin ------------------------------------------------------------
step "6. autologin for $RUN_USER_WANT"
GDM_CONF=/etc/gdm3/custom.conf
[ -f "$GDM_CONF" ] || GDM_CONF=/etc/gdm/custom.conf
if [ "$DO_AUTOLOGIN" = "0" ]; then
  say "skipped (--no-autologin) — the screen will stop at a login prompt"
elif [ ! -f "$GDM_CONF" ]; then
  say "no GDM config found; enable automatic login in Settings > Users"
elif grep -qE '^\s*AutomaticLoginEnable\s*=\s*[Tt]rue' "$GDM_CONF"; then
  say "already enabled in $GDM_CONF"
elif [ "$DRY" = "1" ]; then
  say "would enable AutomaticLogin=$RUN_USER_WANT in $GDM_CONF"
else
  cp -n "$GDM_CONF" "$GDM_CONF.trossen.bak" 2>/dev/null || true
  python3 - "$GDM_CONF" "$RUN_USER_WANT" <<'PY'
import re, sys
path, user = sys.argv[1], sys.argv[2]
text = open(path).read()
for key, value in (("AutomaticLoginEnable", "true"), ("AutomaticLogin", user)):
    line = f"{key}={value}"
    if re.search(rf"^\s*#?\s*{key}\s*=.*$", text, re.M):
        text = re.sub(rf"^\s*#?\s*{key}\s*=.*$", line, text, count=1, flags=re.M)
    elif "[daemon]" in text:
        text = text.replace("[daemon]", f"[daemon]\n{line}", 1)
    else:
        text += f"\n[daemon]\n{line}\n"
open(path, "w").write(text)
PY
  say "enabled (backup at $GDM_CONF.trossen.bak)"
fi

# --- 7. units ----------------------------------------------------------------
step "7. services and autostart entries"
if [ "$DRY" = "1" ]; then
  run "$HERE/install-rivet.sh" --dry-run
else
  "$HERE/install-rivet.sh" | sed 's/^/    /'
fi

# --- 8. tailscale ------------------------------------------------------------
step "8. tailscale"
if command -v tailscale >/dev/null 2>&1; then
  say "already installed"
else
  say "installing"
  run sh -c 'curl -fsSL https://tailscale.com/install.sh | sh'
fi
if [ -n "$TS_AUTHKEY" ]; then
  say "enrolling as $HOSTNAME_WANT with $TS_TAG"
  run tailscale up --ssh --hostname="$HOSTNAME_WANT" \
    --advertise-tags="$TS_TAG" --authkey="$TS_AUTHKEY"
else
  say "no --ts-authkey given. Enrol later with:"
  say "  sudo tailscale up --ssh --hostname=$HOSTNAME_WANT --advertise-tags=$TS_TAG"
  say "A tag with no matching ACL rule makes the rig unreachable over the"
  say "tailnet — keep a LAN session open the first time."
fi

# --- 9. verify ---------------------------------------------------------------
step "9. verify"
if [ "$DRY" = "1" ]; then
  say "would run: systemctl start trossen-rivet-preflight"
else
  systemctl start trossen-rivet-preflight 2>/dev/null
  journalctl -u trossen-rivet-preflight -n 30 --no-pager | sed 's/^/    /'
fi

cat <<NEXT

=== done

Left to do by hand:
  * reboot, which is the only real test of all of this
  * after it comes back:  ./scripts/startup/detect-network.sh
  * add it to the laptop's inventory:  robots add $HOSTNAME_WANT

The touchscreen and the browser only start with a GRAPHICAL LOGIN, so neither
appears until the session restarts — a reboot, not a service restart.

Back out of the service half with:
  sudo ./scripts/startup/install-rivet.sh --uninstall
NEXT
