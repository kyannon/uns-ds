#!/bin/bash
# Unnatural Selection (1993) — dual-screen port for ROCKNIX (RG DS / RG DS Plus)
# Place this script in your ports folder (e.g. /storage/roms/ports/) next to the
# "unnaturalselection" folder.

. /etc/profile 2>/dev/null

GAMEDIR="$(cd "$(dirname "$0")" && pwd)/unnaturalselection"
cd "$GAMEDIR" || exit 1
LOG="$GAMEDIR/log.txt"
exec >"$LOG" 2>&1
echo "== Unnatural Selection DS: $(date)"
echo "device: ${QUIRK_DEVICE:-unknown}  ui: ${UI_SERVICE:-unknown}  top output: ${WLR_CON:-?}"

if [ ! -f "$GAMEDIR/gamedata/uns.exe" ] && [ ! -f "$GAMEDIR/gamedata/UNS.EXE" ]; then
  echo "game files missing: copy the DOS game into $GAMEDIR/gamedata"
fi

chmod +x "$GAMEDIR/usds" 2>/dev/null
export USDS_DIR="$GAMEDIR"
export SDL_VIDEO_WAYLAND_WMCLASS=usds
export SDL_VIDEO_X11_WMCLASS=usds

# --- sway: put each window on its own panel ---------------------------------
# ROCKNIX already moves windows titled "...Bottom..." to the second panel and
# powers it on; this helper makes sure of it (and of which panel is which).
place_windows() {
  command -v swaymsg >/dev/null 2>&1 || return
  command -v jq >/dev/null 2>&1 || return
  local outs top bottom n=0
  outs=$(swaymsg -t get_outputs -r 2>/dev/null | jq -r '.[].name')
  top=${USDS_TOP_OUTPUT:-$WLR_CON}
  [ -z "$top" ] && top=$(echo "$outs" | head -n1)
  bottom=${USDS_BOTTOM_OUTPUT:-$(echo "$outs" | grep -v -x "$top" | head -n1)}
  [ "$USDS_SWAP_OUTPUTS" = "1" ] && { local t=$top; top=$bottom; bottom=$t; }
  echo "sway outputs: $(echo $outs)  -> top=$top bottom=$bottom"
  [ -n "$bottom" ] && swaymsg "output $bottom power on" >/dev/null 2>&1
  # wait for both windows to exist
  while [ $n -lt 40 ]; do
    if swaymsg -t get_tree -r 2>/dev/null | grep -q 'Unnatural Selection - Bottom'; then break; fi
    sleep 0.25; n=$((n+1))
  done
  sleep 0.3
  swaymsg '[title="^Unnatural Selection$"] fullscreen disable, move window to output '"$top"', focus, fullscreen enable' >/dev/null 2>&1
  if [ -n "$bottom" ]; then
    swaymsg '[title="^Unnatural Selection - Bottom$"] fullscreen disable, move window to output '"$bottom"', fullscreen enable' >/dev/null 2>&1
    # the bottom panel's touch goes to whatever is on it
    swaymsg 'input type:touch map_to_output '"$bottom" >/dev/null 2>&1
  fi
  swaymsg '[title="^Unnatural Selection$"] focus' >/dev/null 2>&1
}

if echo "${UI_SERVICE}" | grep -q sway || [ -n "$SWAYSOCK" ] || pgrep -x sway >/dev/null 2>&1; then
  place_windows &
fi

# Stop anything that turns pad presses into key presses for other ports
pkill -f gptokeyb 2>/dev/null

./usds --set window_mode=dual
status=$?
echo "== exit $status"

# restore ES on the top panel
if command -v swaymsg >/dev/null 2>&1; then
  swaymsg '[app_id="emulationstation"] focus' >/dev/null 2>&1
fi
exit $status
