#!/usr/bin/env bash
#
# Point the touchscreen at the display it is physically stuck to.
#
#   kiosk-touch.sh            # auto: map every touch device to the connected output
#   TOUCH_DEVICE=… TOUCH_OUTPUT=… kiosk-touch.sh
#
# X maps a touch device across the whole desktop by default, so on a rotated or
# multi-head setup touches land somewhere other than where the finger is — 90°
# out on the Rivet, whose panel is portrait via `rotate right`. `xinput
# map-to-output` derives the coordinate transformation from that CRTC's own
# transform, so it fixes rotation and multi-head with the same call.
#
# None of this survives a logout, which is why it is an autostart entry rather
# than something you run once.
#
# Overrides come from /etc/trossen/rivet.conf:
#   TOUCH_DEVICE  exact xinput name; default = every device that looks like touch
#   TOUCH_OUTPUT  xrandr output name; default = the single connected output
#   TOUCH_MATRIX  nine numbers, applied INSTEAD of map-to-output for a panel
#                 whose transform has to be stated by hand
set -uo pipefail

CONF="${TROSSEN_RIVET_CONF:-/etc/trossen/rivet.conf}"
TOUCH_DEVICE=""; TOUCH_OUTPUT=""; TOUCH_MATRIX=""
# shellcheck source=/dev/null
[ -r "$CONF" ] && . "$CONF"

log() { printf '[touch] %s\n' "$*"; }

command -v xinput >/dev/null 2>&1 || { log "xinput not installed — nothing to do"; exit 0; }

# An autostart entry can beat X into being ready on a slow boot.
for _ in $(seq 1 15); do
  xrandr --query >/dev/null 2>&1 && break
  sleep 1
done

# Select by NUMERIC ID, never by name. A touch panel appears twice under one
# name — once as a slave pointer, once as a slave keyboard — and xinput refuses
# an ambiguous name outright ("There are multiple devices matching ...", exit
# non-zero, nothing applied). Addressing the pointer half by id is the only
# form that works, and it is the half that carries coordinates.
#
# Lines look like:
#   ⎜   ↳ TSTP MTouch      id=7  [slave  pointer  (2)]
FILTER="${TOUCH_DEVICE:-touch|finger}"
mapfile -t DEVICES < <(
  xinput list 2>/dev/null |
    grep -i 'slave  *pointer' |
    grep -iE "$FILTER" |
    sed -n 's/.*↳[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*id=\([0-9][0-9]*\).*/\2 \1/p'
)

if [ "${#DEVICES[@]}" -eq 0 ]; then
  log "no touch pointer found matching '$FILTER' — nothing to do"
  exit 0
fi

# Output: the configured one, or the only connected one. With two connected and
# none named we stop: guessing which screen the panel is would be a coin flip,
# and getting it wrong is indistinguishable from doing nothing.
if [ -z "$TOUCH_OUTPUT" ]; then
  mapfile -t CONNECTED < <(xrandr --query 2>/dev/null | awk '/ connected/ {print $1}')
  if [ "${#CONNECTED[@]}" -eq 1 ]; then
    TOUCH_OUTPUT="${CONNECTED[0]}"
  elif [ "${#CONNECTED[@]}" -gt 1 ]; then
    log "several outputs connected (${CONNECTED[*]}) and TOUCH_OUTPUT is unset."
    log "Set it in $CONF — which panel the touchscreen belongs to is not"
    log "something this script can work out."
    exit 0
  else
    log "no connected output — is X running?"
    exit 0
  fi
fi

for entry in "${DEVICES[@]}"; do
  id="${entry%% *}"
  name="${entry#* }"
  [ -n "$id" ] || continue
  if [ -n "$TOUCH_MATRIX" ]; then
    # shellcheck disable=SC2086
    if xinput set-prop "$id" 'Coordinate Transformation Matrix' $TOUCH_MATRIX 2>&1; then
      log "OK   '$name' (id=$id) matrix set to $TOUCH_MATRIX"
    else
      log "FAIL '$name' (id=$id) would not take the matrix"
    fi
  elif xinput map-to-output "$id" "$TOUCH_OUTPUT" 2>&1; then
    # map-to-output derives the matrix from that output's own CRTC transform,
    # so a rotated panel is handled without stating any numbers.
    log "OK   '$name' (id=$id) mapped to $TOUCH_OUTPUT"
    log "     matrix now: $(xinput list-props "$id" 2>/dev/null |
                            sed -n 's/.*Coordinate Transformation Matrix[^:]*:[[:space:]]*//p')"
  else
    log "FAIL '$name' (id=$id) could not be mapped to $TOUCH_OUTPUT"
  fi
done

log "done ($(xrandr --query 2>/dev/null | awk -v o="$TOUCH_OUTPUT" '$1==o {print $3, $4}'))"
