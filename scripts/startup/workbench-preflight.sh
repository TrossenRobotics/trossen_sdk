#!/usr/bin/env bash
#
# Workbench power-on checks, run once at boot before the webapp starts.
#
# A Workbench is a WIRED rig: the Orin, the leader arms and the follower arms
# all sit on the same switch, and nothing in the data path is wireless. So this
# is deliberately NOT rivet-preflight.sh with the WiFi parts skipped — it is a
# shorter script that never mentions a radio, because a Workbench with no WiFi
# association is not degraded, it is normal.
#
# FAILURE POLICY, and it differs from the Rivet's on purpose:
#   NOTHING HERE IS FATAL.
#
#   The Rivet's preflight fails the boot when a fix it OWNS will not apply —
#   the AP pin, WiFi power save, the static address — because configuration
#   that refuses to apply means the machine is not the one we think it is.
#   A Workbench has no such fixes: its addressing is static on interfaces
#   NetworkManager already brought up, so there is nothing here to apply and
#   therefore nothing that can refuse. Everything below is an observation.
#
#   That is why the webapp unit only Wants= this one. A Workbench whose arms
#   are powered off should still bring up its UI — that is how the operator
#   finds out the arms are off.
#
# Everything here is idempotent: it is a boot script, and it will be re-run by
# hand at the worst possible moment by someone debugging.
set -uo pipefail

CONF="${TROSSEN_WORKBENCH_CONF:-/etc/trossen/workbench.conf}"

# Defaults for every key, so an old config file missing a new one still boots.
ARM_IFACES="eth0"
ARM_ADDRS=""
MUTE_AUDIO=1
MIN_FREE_GB=20
REPO_DIR="/home/trossen/trossen_sdk"
RUN_USER="trossen"
IFACE_WAIT_S=30

# shellcheck source=/dev/null
[ -r "$CONF" ] && . "$CONF"

warn_count=0

log()  { printf '[preflight] %s\n' "$*"; }
ok()   { printf '[preflight]   OK    %s\n' "$*"; }
warn() { printf '[preflight]   WARN  %s\n' "$*"; warn_count=$((warn_count + 1)); }

if [ "$(id -u)" -ne 0 ]; then
  echo "[preflight] must run as root (it mutes the system mixer)" >&2
  exit 1
fi

# --- 1. the wired links the arms are on --------------------------------------
# Carrier only. Whether an arm ANSWERS is checked separately below, because a
# live cable to a powered-off controller is a different problem from a cable
# that is not plugged in, and they need different things done about them.
for iface in $ARM_IFACES; do
  waited=0
  while [ ! -d "/sys/class/net/$iface" ] && [ "$waited" -lt "$IFACE_WAIT_S" ]; do
    sleep 1
    waited=$((waited + 1))
  done

  if [ ! -d "/sys/class/net/$iface" ]; then
    warn "$iface does not exist — check ARM_IFACES in $CONF"
    continue
  fi
  if [ "$(cat "/sys/class/net/$iface/carrier" 2>/dev/null)" = "1" ]; then
    speed="$(cat "/sys/class/net/$iface/speed" 2>/dev/null)"
    addr="$(ip -4 -o addr show "$iface" 2>/dev/null | awk '{print $4}' | paste -sd, -)"
    ok "$iface has carrier${speed:+ at ${speed}Mb/s}${addr:+, addressed $addr}"
  else
    warn "$iface has NO carrier — the arm switch is unplugged or powered off"
  fi
done

# --- 2. do the arms actually answer? -----------------------------------------
# ARP, not ping. The arm controllers do not answer ICMP at all, so a ping test
# reports every healthy arm as dead — which is worse than no test, because it
# trains people to ignore it. An ARP reply proves the controller is powered and
# on the wire, which is exactly the question this is asking.
if [ -n "$ARM_ADDRS" ]; then
  for addr in $ARM_ADDRS; do
    # Provokes the ARP resolution; the ping itself is expected to fail.
    ping -c1 -W1 "$addr" >/dev/null 2>&1 &
  done
  wait 2>/dev/null || true
  sleep 1

  answered=0
  total=0
  silent=""
  for addr in $ARM_ADDRS; do
    total=$((total + 1))
    if ip neigh show "$addr" 2>/dev/null | grep -qE 'lladdr'; then
      answered=$((answered + 1))
    else
      silent="${silent:+$silent }$addr"
    fi
  done

  if [ -z "$silent" ]; then
    ok "all $total arm controllers answered ARP"
  else
    warn "$answered/$total arm controllers answered; silent: $silent"
    warn "  (powered off, or on a different subnet than this rig is addressed for)"
  fi
else
  log "  no ARM_ADDRS set — skipping the arm liveness check"
fi

# --- 3. the rest of the boot-time facts worth having in the log --------------
free_gb="$(df -BG --output=avail "$REPO_DIR" 2>/dev/null | tail -1 | tr -dc '0-9')"
if [ -n "$free_gb" ]; then
  if [ "$free_gb" -ge "$MIN_FREE_GB" ]; then
    ok "${free_gb}G free on $REPO_DIR"
  else
    warn "only ${free_gb}G free on $REPO_DIR (want >= ${MIN_FREE_GB}G) — recordings will fill this"
  fi
else
  warn "could not read free space for $REPO_DIR"
fi

if systemctl is-active --quiet avahi-daemon; then
  ok "avahi-daemon running (.local name resolves)"
else
  warn "avahi-daemon not running — the rig will not answer to its .local name"
fi

# A clock that is wrong by a lot makes every recording's timestamps wrong and
# breaks TLS to anything, which presents as unrelated failures much later.
if timedatectl show -p NTPSynchronized --value 2>/dev/null | grep -q yes; then
  ok "clock synchronised"
else
  warn "clock not NTP-synchronised — recording timestamps will be whatever the RTC says"
fi

# --- 4. silence. A Jetson with a monitor plugged in has an HDMI audio sink and
# nothing worth saying through it.
if [ "$MUTE_AUDIO" = "1" ]; then
  muted=0
  if command -v amixer >/dev/null 2>&1; then
    while read -r card; do
      for control in Master PCM Speaker Headphone; do
        amixer -c "$card" sset "$control" 0% mute >/dev/null 2>&1 && muted=1
      done
    done < <(aplay -l 2>/dev/null | awk -F'[ :]' '/^card/ {print $2}' | sort -u)
  fi
  # PulseAudio/PipeWire, if a session is up. Runs as the desktop user because
  # the sink belongs to their session, not to root.
  if command -v runuser >/dev/null 2>&1 && id "$RUN_USER" >/dev/null 2>&1; then
    # Single-quoted on purpose: this body runs in the target user's shell and
    # must expand THERE, against their session's sinks.
    # shellcheck disable=SC2016
    runuser -u "$RUN_USER" -- bash -lc '
      command -v pactl >/dev/null 2>&1 || exit 0
      for sink in $(pactl list short sinks 2>/dev/null | cut -f1); do
        pactl set-sink-volume "$sink" 0% 2>/dev/null
        pactl set-sink-mute "$sink" 1 2>/dev/null
      done' >/dev/null 2>&1 && muted=1
  fi
  if [ "$muted" = "1" ]; then
    ok "audio muted"
  else
    warn "found no mixer to mute"
  fi
fi

# --- verdict -----------------------------------------------------------------
# Always zero. See the failure policy at the top: there is nothing here this
# script could have fixed, so there is nothing whose failure should stop a
# Workbench from bringing up its UI.
log "done: $warn_count warnings (nothing here blocks the webapp)"
exit 0
