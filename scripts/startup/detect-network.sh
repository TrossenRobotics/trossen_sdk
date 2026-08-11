#!/usr/bin/env bash
#
# Report everything the startup config needs to know about this machine's
# network, and print a rivet.conf block with the answers filled in.
#
#   ./detect-network.sh                          # read what is there now
#   ./detect-network.sh --rescan                 # also re-scan for APs
#   ./detect-network.sh --static-ip 192.168.5.33 # propose this address instead
#
# READ-ONLY. It changes nothing, needs no root (a couple of lines say more with
# it), and is safe to run on a robot mid-session — with one caveat: --rescan
# asks the radio to sweep every channel, which briefly interrupts traffic on
# some drivers, so it is opt-in rather than the default.
#
# Run it on the RIG, not on the laptop. Every value it prints is a property of
# the machine it runs on, and the whole point is that the two differ.
set -uo pipefail

RESCAN=0
WANT_IP=""

while [ $# -gt 0 ]; do
  case "$1" in
    --rescan)    RESCAN=1; shift ;;
    --static-ip) WANT_IP="${2:?--static-ip needs a value}"; shift 2 ;;
    -h|--help)   sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

hr()  { printf '\n== %s %s\n' "$1" "$(printf '%.0s-' $(seq 1 $((60 - ${#1}))))"; }
say() { printf '  %s\n' "$*"; }

have() { command -v "$1" >/dev/null 2>&1; }

# Frequencies, so a BSSID can be identified as the 5 GHz one without counting
# megahertz by eye. 6 GHz included because a modern AP may put you there and it
# looks like neither of the two bands anyone talks about.
band_of() {
  local mhz="${1%%.*}"
  case "$mhz" in
    2[34][0-9][0-9]) echo "2.4GHz" ;;
    5[0-9][0-9][0-9]) echo "5GHz" ;;
    6[0-9][0-9][0-9]|7[0-1][0-9][0-9]) echo "6GHz" ;;
    *) echo "${mhz}MHz" ;;
  esac
}

hr "HOST"
say "hostname : $(hostname)"
say "kernel   : $(uname -r)"
say "uptime   : $(uptime -p 2>/dev/null || true)"

hr "NAMES AND REACHABILITY-BY-NAME"
# A rig is found by name or not at all: an IP here is a DHCP lease that will
# later point at some other device, which does not fail cleanly — it answers
# ping and refuses port 22, and reads exactly like "the rig is broken".
HOSTNAME_NOW="$(hostname)"
say "hostname : $HOSTNAME_NOW"
if printf '%s' "$HOSTNAME_NOW" | grep -qE '^(workbench|rivet|stationary|cockpit|payload)-[0-9]{2}$'; then
  say "           matches the <type>-<nn> scheme"
else
  say "           DOES NOT match <type>-<nn> (e.g. rivet-01)."
  say "           The ssh config keys on that pattern, so a name outside it"
  say "           skips the rig block entirely: no mDNS suffix, no key, no"
  say "           prompt-free settings. Rename with:"
  say "             sudo hostnamectl set-hostname rivet-01"
fi

if systemctl is-active --quiet avahi-daemon 2>/dev/null; then
  say "mDNS     : avahi running — answers to $HOSTNAME_NOW.local on this LAN"
else
  say "mDNS     : avahi NOT running — the .local name will not resolve"
fi

if have tailscale; then
  ts_state="$(tailscale status --json 2>/dev/null |
              python3 -c 'import json,sys; d=json.load(sys.stdin); s=d.get("Self",{}); print(s.get("DNSName","").rstrip("."), d.get("BackendState",""))' 2>/dev/null)"
  if [ -n "${ts_state% *}" ] && [ "${ts_state% *}" != "" ]; then
    say "tailscale: ${ts_state% *} (${ts_state##* })"
    say "           reachable from any network, no LAN or mDNS needed"
  else
    say "tailscale: installed but not logged in — sudo tailscale up --ssh --hostname=<name>"
  fi
else
  say "tailscale: NOT installed. This rig is only reachable on its own LAN,"
  say "           and only while its address holds. Install (Ubuntu/Jetson):"
  say "             curl -fsSL https://tailscale.com/install.sh | sh"
fi

hr "INTERFACES"
if have nmcli; then
  nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null |
    while IFS=: read -r dev type state conn; do
      addr="$(ip -4 -o addr show dev "$dev" 2>/dev/null | awk '{print $4}' | paste -sd, -)"
      say "$(printf '%-12s %-10s %-14s %-20s %s' "$dev" "$type" "$state" "${conn:-—}" "${addr:-no address}")"
    done
else
  say "nmcli not installed — falling back to ip(8)"
  ip -4 -o addr show | awk '{printf "  %-12s %s\n", $2, $4}'
fi

# --- the wireless link -------------------------------------------------------
WIFI_DEV=""
if have nmcli; then
  WIFI_DEV="$(nmcli -t -f DEVICE,TYPE device status 2>/dev/null | awk -F: '$2=="wifi"{print $1; exit}')"
fi
if [ -z "$WIFI_DEV" ]; then
  # Fallback for a box with no NetworkManager: the kernel's predictable names
  # start wl* for wireless.
  for candidate in /sys/class/net/wl*; do
    [ -e "$candidate" ] && { WIFI_DEV="$(basename "$candidate")"; break; }
  done
fi

ACTIVE_CONN=""; CUR_BSSID=""; CUR_SSID=""; CUR_BAND=""; PROFILE_DNS=""

hr "WIFI ASSOCIATION"
if [ -z "$WIFI_DEV" ]; then
  say "no wireless interface found"
else
  say "device   : $WIFI_DEV"
  if have iw; then
    link="$(iw dev "$WIFI_DEV" link 2>/dev/null)"
    if printf '%s' "$link" | grep -qi '^Connected to'; then
      CUR_BSSID="$(printf '%s' "$link" | awk '/^Connected to/{print $3}')"
      CUR_SSID="$(printf '%s' "$link" | awk -F': ' '/SSID:/{print $2; exit}')"
      freq="$(printf '%s' "$link" | awk -F': ' '/freq:/{print $2; exit}')"
      CUR_BAND="$(band_of "${freq:-0}")"
      say "SSID     : ${CUR_SSID:-?}"
      say "BSSID    : ${CUR_BSSID:-?}   <-- this is the AP you are on"
      say "band     : $CUR_BAND (${freq:-?} MHz)"
      say "signal   : $(printf '%s' "$link" | awk -F': ' '/signal:/{print $2; exit}')"
      if [ "$CUR_BAND" != "5GHz" ]; then
        say ""
        say "NOTE: this is not a 5 GHz association. If the plan is to pin the 5 GHz"
        say "      AP, take the BSSID from the scan below, not from this line."
      fi
    else
      say "not associated to any AP"
    fi
  else
    say "iw not installed — install it, or read the BSSID from the scan below"
  fi

  # `iw` is not installed everywhere, and a rig missing it must still report a
  # BSSID — that is the single value this script exists to produce. nmcli's scan
  # list marks the associated AP with `*`, which is the same fact by another
  # route. Note the \: escaping: nmcli escapes the colons INSIDE a BSSID in
  # terse mode, so the field has to be unescaped rather than split naively.
  if [ -z "$CUR_BSSID" ] && have nmcli; then
    while IFS=: read -r inuse rest; do
      [ "$inuse" = "*" ] || continue
      CUR_BSSID="$(printf '%s' "$rest" | sed 's/\\:/:/g' | cut -d: -f1-6)"
      break
    done < <(nmcli -t -f IN-USE,BSSID device wifi list 2>/dev/null)
    [ -n "$CUR_BSSID" ] && say "BSSID    : $CUR_BSSID   <-- from nmcli (iw absent)"
  fi

  if have nmcli; then
    ACTIVE_CONN="$(nmcli -t -f NAME,DEVICE connection show --active 2>/dev/null |
                   awk -F: -v d="$WIFI_DEV" '$2==d{print $1; exit}')"
    if [ -z "$CUR_SSID" ] && [ -n "$ACTIVE_CONN" ]; then
      CUR_SSID="$(nmcli -t -f 802-11-wireless.ssid connection show "$ACTIVE_CONN" 2>/dev/null | cut -d: -f2-)"
      [ -n "$CUR_SSID" ] && say "SSID     : $CUR_SSID   <-- from the active profile"
    fi
    if [ -z "$CUR_BAND" ] && [ -n "$CUR_BSSID" ]; then
      freq_now="$(nmcli -t -f IN-USE,FREQ device wifi list 2>/dev/null |
                  awk -F: '$1=="*"{print $2; exit}')"
      CUR_BAND="$(band_of "${freq_now%% *}")"
      say "band     : $CUR_BAND"
      [ "$CUR_BAND" != "5GHz" ] && say "NOTE: not a 5 GHz association — take the BSSID from the scan below"
    fi
    say "profile  : ${ACTIVE_CONN:-none active}"
    if [ -n "$ACTIVE_CONN" ]; then
      ps_val="$(nmcli -t -f 802-11-wireless.powersave connection show "$ACTIVE_CONN" 2>/dev/null | cut -d: -f2)"
      case "$ps_val" in
        2) say "powersave: 2 (disabled) — what we want" ;;
        3) say "powersave: 3 (ENABLED) — this is the one that makes throughput collapse in bursts" ;;
        *) say "powersave: ${ps_val:-unset} (0=default 1=ignore 2=disable 3=enable)" ;;
      esac
      pinned="$(nmcli -t -f 802-11-wireless.bssid connection show "$ACTIVE_CONN" 2>/dev/null | cut -d: -f2-)"
      say "pinned   : ${pinned:-none — free to roam}"

      # More than one profile for one SSID is a trap: the startup config names
      # ONE of them, and NetworkManager picks at boot by priority then by which
      # was used last. Fixes applied to the profile that does not win are
      # invisible — everything looks configured and nothing took effect.
      dupes="$(nmcli -t -f NAME,TYPE connection show 2>/dev/null |
               awk -F: '$2=="802-11-wireless"' | cut -d: -f1 |
               while IFS= read -r c; do
                 [ "$c" = "$ACTIVE_CONN" ] && continue
                 s="$(nmcli -t -f 802-11-wireless.ssid connection show "$c" 2>/dev/null | cut -d: -f2-)"
                 [ "$s" = "$CUR_SSID" ] && printf '%s\n' "$c"
               done)"
      if [ -n "$dupes" ]; then
        say ""
        say "WARNING: other profiles exist for SSID \"$CUR_SSID\":"
        printf '           %s\n' "$dupes"
        say "  Only \"$ACTIVE_CONN\" is in use now. Whichever wins at boot is the one"
        say "  that matters, so either delete the others —"
        say "      nmcli connection delete <name>"
        say "  or make sure WIFI_CONN names the one that actually comes up."
      fi
    fi
  fi
fi

# --- what else is on the air -------------------------------------------------
hr "ACCESS POINTS${CUR_SSID:+ FOR \"$CUR_SSID\"}"
if have nmcli && [ -n "$WIFI_DEV" ]; then
  [ "$RESCAN" = "1" ] && { say "re-scanning…"; nmcli device wifi rescan >/dev/null 2>&1; sleep 3; }
  printf '  %-4s %-18s %-8s %-5s %-7s %s\n' "USE" "BSSID" "BAND" "CHAN" "SIGNAL" "SSID"
  nmcli -t -f IN-USE,BSSID,FREQ,CHAN,SIGNAL,SSID device wifi list 2>/dev/null |
    sed 's/\\:/-/g' |
    while IFS=: read -r inuse bssid freq chan signal ssid; do
      # Only this SSID, when we know it; everything otherwise.
      [ -n "$CUR_SSID" ] && [ "$ssid" != "$CUR_SSID" ] && continue
      printf '  %-4s %-18s %-8s %-5s %-7s %s\n' \
        "${inuse:-}" "$(printf '%s' "$bssid" | tr '-' ':')" \
        "$(band_of "${freq%% *}")" "$chan" "$signal" "$ssid"
    done
  say ""
  say "Pick the 5GHz row with the best SIGNAL and put its BSSID in WIFI_BSSID."
  say "Re-run with --rescan if the list looks stale or short."
else
  say "nmcli unavailable — cannot list APs"
fi

# --- addressing --------------------------------------------------------------
hr "ADDRESSING"
GW="$(ip -4 route show default 2>/dev/null | awk '{print $3; exit}')"
say "default route : ${GW:-none} $(ip -4 route show default 2>/dev/null | awk '{print "via dev " $5}')"
if have resolvectl; then
  dns_list="$(resolvectl dns 2>/dev/null | awk -F': ' '/Link/ && NF>1 {print $2}' | tr '\n' ' ')"
else
  dns_list="$(awk '/^nameserver/{printf "%s ", $2}' /etc/resolv.conf 2>/dev/null)"
fi
say "DNS servers   : ${dns_list:-none configured}"

if have nmcli && [ -n "$ACTIVE_CONN" ]; then
  say ""
  say "profile '$ACTIVE_CONN' as configured (not as leased):"
  for prop in ipv4.method ipv4.addresses ipv4.gateway ipv4.dns; do
    val="$(nmcli -t -f "$prop" connection show "$ACTIVE_CONN" 2>/dev/null | cut -d: -f2-)"
    say "  $(printf '%-16s %s' "$prop" "${val:-—}")"
    # Keep the profile's own DNS for the suggestion below. It beats the runtime
    # resolver list, which on a machine running Tailscale is mostly MagicDNS
    # entries belonging to THIS host's tailnet — pasting those into a robot's
    # config gives it nameservers that mean nothing to it.
    [ "$prop" = "ipv4.dns" ] && PROFILE_DNS="$(printf '%s' "$val" | tr ',;' '  ')"
  done
  method="$(nmcli -t -f ipv4.method connection show "$ACTIVE_CONN" 2>/dev/null | cut -d: -f2)"
  if [ "$method" = "auto" ]; then
    say ""
    say "This profile is on DHCP. The address below is a LEASE — it can change,"
    say "and pointing anything at it is how a rig 'moves' overnight."
  fi
fi

CUR_IP="$(ip -4 -o addr show dev "${WIFI_DEV:-lo}" 2>/dev/null | awk '{print $4; exit}')"
say ""
say "current address on ${WIFI_DEV:-?} : ${CUR_IP:-none}"

# --- can it actually reach anything -----------------------------------------
hr "REACHABILITY"
check() { # name, command...
  local name="$1"; shift
  if "$@" >/dev/null 2>&1; then say "OK    $name"; else say "FAIL  $name"; fi
}

# ICMP is not a reliable single probe on a wireless link with power save on: the
# radio sleeps, one echo request is dropped, and a single ping with a 2s
# deadline reports the network as down while TCP — which retransmits — is fine.
# That misdiagnosis was observed on a Rivet, so this sends several and then
# falls back to TCP and to ARP rather than trusting one packet.
reachable() { # host [tcp_port...]
  local host="$1"; shift
  if ping -c3 -W3 -i 0.3 "$host" >/dev/null 2>&1; then
    say "OK    $host (icmp)"
    return 0
  fi
  local port
  for port in "$@"; do
    if timeout 3 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; then
      say "OK    $host (tcp/$port — ICMP did not answer, which is normal on a"
      say "      power-saving link or a device that simply does not do ping)"
      return 0
    fi
  done
  # Layer 2. A device that answers neither ICMP nor TCP but has an ARP entry is
  # present on the wire — the distinction between "not there" and "not talking".
  if ip neigh show "$host" 2>/dev/null | grep -qE 'REACHABLE|STALE|DELAY'; then
    say "WARN  $host is in the ARP table but answered nothing"
    return 0
  fi
  say "FAIL  $host — no ICMP, no TCP, no ARP entry"
  return 1
}

[ -n "$GW" ] && reachable "$GW" 80 443 53
# Separate from DNS on purpose: an address that answers while a name will not
# resolve is a DNS fault, and from inside the app the two are indistinguishable.
reachable 1.1.1.1 443 53
check "DNS resolves github.com"       getent hosts github.com
have curl && check "HTTPS to github.com" curl -fsS --max-time 8 -o /dev/null https://github.com

# --- the answer --------------------------------------------------------------
hr "SUGGESTED /etc/trossen/rivet.conf"
PROPOSED_IP="$WANT_IP"
if [ -z "$PROPOSED_IP" ]; then
  PROPOSED_IP="${CUR_IP:-}"
fi
# A bare address needs a prefix; assume /24 and say so rather than silently
# writing something that will not apply.
case "$PROPOSED_IP" in
  */*) ;;
  ?*)  PROPOSED_IP="$PROPOSED_IP/24"; say "# assumed /24 for $PROPOSED_IP — check the netmask" ;;
esac

cat <<CONF
  WIFI_IFACE="${WIFI_DEV:-wlan0}"
  WIFI_CONN="${ACTIVE_CONN:-FILL_ME}"
  WIFI_BSSID="${CUR_BSSID:-FILL_ME}"${CUR_BAND:+   # currently $CUR_BAND}
  STATIC_IP="${PROPOSED_IP:-FILL_ME}"
  STATIC_GW="${GW:-FILL_ME}"
  STATIC_DNS="$(printf '%s' "${PROFILE_DNS:-$dns_list}" | sed 's/  */ /g; s/ *$//')"
CONF

cat <<'NOTE'

  Check before pasting:
  * WIFI_BSSID must be the 5GHz AP from the scan above — the association line
    tells you where you are now, which is not necessarily where you want to be.
  * STATIC_IP must be OUTSIDE the router's DHCP pool, or the same address gets
    leased to something else and both drop off intermittently.
  * STATIC_DNS empty means the gateway is not handing out DNS. Put something
    real there (the gateway itself, or 1.1.1.1) or name resolution stops the
    moment the profile goes manual — the classic "it had internet on DHCP and
    lost it when we made it static".
NOTE
