#!/usr/bin/env bash
#
# Rivet power-on checks, run once at boot before the webapp starts.
#
# The job is to put the robot into the state we already know it needs — AP
# pinned, WiFi power save off, address fixed, audio silent — and to say plainly
# what it found, so a rig that comes up wrong is diagnosable from
# `journalctl -u trossen-rivet-preflight` instead of from a keyboard.
#
# FAILURE POLICY, which is deliberate and worth reading before changing:
#   * A fix we can apply ourselves — the BSSID pin, `wifi.powersave`, the static
#     address — is retried, and if it still will not take, this script FAILS.
#     The webapp unit requires this one, so the robot stops before collecting
#     data nobody can reach. These are configuration, and configuration that
#     refuses to apply means the machine is not the machine we think it is.
#   * Anything we cannot fix from here — the AP is off, the floor's network is
#     down, the arms' switch is unplugged — is reported and does NOT block. A
#     Rivet with no WiFi still drives and still records locally; refusing to
#     start would turn a network problem into a dead robot.
#
# Everything here is idempotent: it is a boot script, and it will be re-run by
# hand at the worst possible moment by someone debugging.
set -uo pipefail

CONF="${TROSSEN_RIVET_CONF:-/etc/trossen/rivet.conf}"

# Defaults for every key, so an old config file missing a new key still boots.
WIFI_IFACE="wlan0"; WIFI_CONN=""; WIFI_BSSID=""
STATIC_IP=""; STATIC_GW=""; STATIC_DNS=""
MUTE_AUDIO=1; FIX_RETRIES=3; IFACE_WAIT_S=45; MIN_FREE_GB=20
ARM_SUBNET_IFACE="eth0"; REPO_DIR="/home/trossen/trossen_sdk"

# shellcheck source=/dev/null
[ -r "$CONF" ] && . "$CONF"

fail_count=0   # things we could not fix and consider fatal
warn_count=0

log()  { printf '[preflight] %s\n' "$*"; }
ok()   { printf '[preflight]   OK    %s\n' "$*"; }
warn() { printf '[preflight]   WARN  %s\n' "$*"; warn_count=$((warn_count + 1)); }
bad()  { printf '[preflight]   FAIL  %s\n' "$*"; fail_count=$((fail_count + 1)); }

if [ "$(id -u)" -ne 0 ]; then
  echo "[preflight] must run as root (it edits NetworkManager profiles)" >&2
  exit 1
fi

# --- 1. the wireless interface has to exist before anything else means anything
log "waiting up to ${IFACE_WAIT_S}s for $WIFI_IFACE"
waited=0
while [ ! -d "/sys/class/net/$WIFI_IFACE" ] && [ "$waited" -lt "$IFACE_WAIT_S" ]; do
  sleep 1
  waited=$((waited + 1))
done

if [ ! -d "/sys/class/net/$WIFI_IFACE" ]; then
  # Not fixable from here and not necessarily fatal: a Rivet driven over the
  # wired link is a normal way to work.
  warn "$WIFI_IFACE never appeared after ${IFACE_WAIT_S}s — skipping all WiFi checks"
else
  ok "$WIFI_IFACE present after ${waited}s"

  # --- 2. fixes we own. Each is applied then verified; verification failing is
  # what counts, because nmcli exits 0 for plenty of things that did not happen.
  if [ -z "$WIFI_CONN" ]; then
    warn "WIFI_CONN unset in $CONF — cannot pin the AP, disable power save, or fix the address"
  else
    apply_fix() {
      # $1 human name, $2 verify command, $3.. the fix command
      local name="$1" verify="$2"; shift 2
      local try=1
      while [ "$try" -le "$FIX_RETRIES" ]; do
        if eval "$verify" >/dev/null 2>&1; then
          ok "$name"
          return 0
        fi
        log "  applying $name (attempt $try/$FIX_RETRIES)"
        "$@" >/dev/null 2>&1
        try=$((try + 1))
        sleep 1
      done
      eval "$verify" >/dev/null 2>&1 && { ok "$name"; return 0; }
      bad "$name — would not apply after $FIX_RETRIES attempts"
      return 1
    }

    # Power save is the one that produces the worst symptom: the link stays
    # associated and pingable while throughput collapses in bursts, which reads
    # as "the app is slow" rather than as a network fault.
    # `wifi.powersave` on the modify side is nmcli's alias; the property is
    # reported under its real name, with a dot. 2 = disabled (1 = default,
    # 3 = enabled), and the numeric form is what `-t` prints.
    apply_fix "wifi.powersave disabled" \
      "nmcli -t -f 802-11-wireless.powersave connection show '$WIFI_CONN' | grep -q ':2\$'" \
      nmcli connection modify "$WIFI_CONN" wifi.powersave 2

    # Belt and braces: the profile setting applies on the next activation, this
    # takes effect now.
    iw dev "$WIFI_IFACE" set power_save off >/dev/null 2>&1 || true

    if [ -n "$WIFI_BSSID" ]; then
      apply_fix "AP pinned to $WIFI_BSSID" \
        "nmcli -t -f 802-11-wireless.bssid connection show '$WIFI_CONN' | grep -qi '$WIFI_BSSID'" \
        nmcli connection modify "$WIFI_CONN" 802-11-wireless.bssid "$WIFI_BSSID"
    else
      log "  no WIFI_BSSID set — leaving the AP choice to the driver"
    fi

    if [ -n "$STATIC_IP" ]; then
      # Both halves, because either alone is a lie: the right address on
      # ipv4.method=auto is ignored in favour of the lease, and `manual` with
      # the wrong address is a robot at an address nobody is looking for.
      apply_fix "static address $STATIC_IP" \
        "nmcli -t -f ipv4.method,ipv4.addresses connection show '$WIFI_CONN' | grep -q '^ipv4.method:manual\$' && nmcli -t -f ipv4.addresses connection show '$WIFI_CONN' | grep -q '$STATIC_IP'" \
        nmcli connection modify "$WIFI_CONN" \
          ipv4.method manual ipv4.addresses "$STATIC_IP" \
          ipv4.gateway "$STATIC_GW" ipv4.dns "$STATIC_DNS"
    else
      log "  no STATIC_IP set — leaving the profile on DHCP"
    fi

    # Bring the profile up if it is not already. Not a "fix" in the sense above:
    # this fails whenever the AP is simply absent, which is not our fault and
    # not fatal.
    if nmcli -t -f NAME connection show --active | grep -qx "$WIFI_CONN"; then
      ok "profile '$WIFI_CONN' is active"
    else
      log "  activating '$WIFI_CONN'"
      if nmcli connection up "$WIFI_CONN" >/dev/null 2>&1; then
        ok "profile '$WIFI_CONN' activated"
      else
        warn "could not activate '$WIFI_CONN' — AP out of range or down. Not blocking."
      fi
    fi
  fi

  # --- 3. observations. None of these block; they are what you read after the
  # fact when someone says "it was fine yesterday".
  link="$(iw dev "$WIFI_IFACE" link 2>/dev/null)"
  if printf '%s' "$link" | grep -qi '^Connected to'; then
    bssid_now="$(printf '%s' "$link" | awk '/^Connected to/ {print $3}')"
    signal="$(printf '%s' "$link" | awk -F': ' '/signal:/ {print $2}')"
    ok "associated to ${bssid_now:-?} (${signal:-signal unknown})"
    if [ -n "$WIFI_BSSID" ] && [ "${bssid_now,,}" != "${WIFI_BSSID,,}" ]; then
      # The pin is in the profile but the radio is on a different AP: it will
      # move on the next activation, so this is worth saying and not worth
      # blocking on.
      warn "associated to $bssid_now but pinned to $WIFI_BSSID — will move on next reconnect"
    fi
  else
    warn "$WIFI_IFACE is not associated to any AP"
  fi

  addr="$(ip -4 -o addr show dev "$WIFI_IFACE" 2>/dev/null | awk '{print $4}' | head -1)"
  if [ -n "$addr" ]; then
    ok "address $addr"
    [ -n "$STATIC_IP" ] && [ "$addr" != "$STATIC_IP" ] && \
      warn "expected $STATIC_IP — the profile is right but the lease is not, so something else assigned this"
  else
    warn "no IPv4 address on $WIFI_IFACE"
  fi

  if [ -n "$STATIC_GW" ]; then
    if ping -c1 -W2 "$STATIC_GW" >/dev/null 2>&1; then
      ok "gateway $STATIC_GW answers"
    else
      warn "gateway $STATIC_GW did not answer — the robot works, you just cannot reach it"
    fi
  fi
fi

# --- 4. the arms' wired link. Carrier only: whether an arm answers is the
# webapp's hardware test, and arms never answer ICMP anyway.
if [ -d "/sys/class/net/$ARM_SUBNET_IFACE" ]; then
  if [ "$(cat "/sys/class/net/$ARM_SUBNET_IFACE/carrier" 2>/dev/null)" = "1" ]; then
    ok "$ARM_SUBNET_IFACE has carrier"
  else
    warn "$ARM_SUBNET_IFACE has NO carrier — the arm/base switch is unplugged or off"
  fi
else
  warn "$ARM_SUBNET_IFACE does not exist — check ARM_SUBNET_IFACE in $CONF"
fi

# --- 5. the rest of the boot-time facts worth having in the log.
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

# --- 6. silence. A Jetson with a monitor plugged in has an HDMI audio sink and
# nothing worth saying through it.
if [ "$MUTE_AUDIO" = "1" ]; then
  muted=0
  # ALSA, for every card the machine has.
  if command -v amixer >/dev/null 2>&1; then
    while read -r card; do
      for control in Master PCM Speaker Headphone; do
        amixer -c "$card" sset "$control" 0% mute >/dev/null 2>&1 && muted=1
      done
    done < <(aplay -l 2>/dev/null | awk -F'[ :]' '/^card/ {print $2}' | sort -u)
  fi
  # PulseAudio/PipeWire, if a session is up. Runs as the desktop user because
  # the sink belongs to their session, not to root.
  if command -v runuser >/dev/null 2>&1 && id "${RUN_USER:-trossen}" >/dev/null 2>&1; then
    # Single-quoted on purpose: this body runs in the target user's shell and
    # must expand THERE, against their session's sinks.
    # shellcheck disable=SC2016
    runuser -u "${RUN_USER:-trossen}" -- bash -lc '
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
log "done: $fail_count fatal, $warn_count warnings"
if [ "$fail_count" -gt 0 ]; then
  log "a fix that should have applied did not — refusing to start the webapp."
  log "override once with: systemctl start trossen-webapp"
  exit 1
fi
exit 0
