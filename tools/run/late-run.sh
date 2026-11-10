#!/bin/bash
# The GameFAQs late save (build/run/mc0-late, a copy per run): File 01 loads
# at Zerard's transporter with its menu open (pad read ~2100), then INPUTS
# (RG_INPUT entries, pad reads). Harvest on into build/harvest, 30 fps, 1x,
# hidden and muted; screenshots every SHOT frames into build/run/shots/NAME.
#   late-run.sh NAME SECONDS "INPUTS" [SHOT=120] [VAR=value ...]
cd "$(dirname "$0")/../.." || exit 1
NAME=$1; SECS=$2; EXTRA=$3; SHOT=${4:-120}; shift 4 2>/dev/null || shift $#
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-1
LOAD="909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5"
export RG_INPUT="$LOAD${EXTRA:+,$EXTRA}"
export RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=30 RG_GS_SCALE=1
rm -rf build/run/mc0-run; cp -r build/run/mc0-late build/run/mc0-run
export RG_MC0_DIR=build/run/mc0-run RG_SETTINGS_FILE=build/run/native-on.ini
export RG_VU1_HARVEST=build/harvest RG_VU1_NATIVE=check RG_VU1_NATIVE_CHECK_EVERY=16 RG_NATIVE_RENDERER=1
export DQ8_GFX_SCREENSHOT_DIR=build/run/shots/$NAME DQ8_GFX_SCREENSHOT_EVERY=$SHOT
for assignment in "$@"; do export "$assignment"; done
rm -rf build/run/shots/$NAME; mkdir -p build/run/shots/$NAME
build/game/src/roguegalaxy disc/SCUS_974.90 > "build/run/$NAME.log" 2>&1 &
pid=$!
sleep "$SECS"
kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
grep -E "\[harvest\] new|\[crash\]|fatal" "build/run/$NAME.log" | head -20
grep '\[vu1native\]' "build/run/$NAME.log" | tail -1
