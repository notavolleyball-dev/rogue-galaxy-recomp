#!/bin/bash
# RG_DRAW_ORIGINS on the field after File 01 loads (copy of the card), 30 fps
# mode, 1x, hidden and muted, VU1 programs on the VU1: build/run/NAME.origins
# (and .seq for flip SEQ), SECONDS of play, counted from flip FROM.
#   tools/bench/origins-run.sh NAME [SECONDS=90] [FROM=0] [SEQ=0] [extra env...]
cd "$(dirname "$0")/../.." || exit 1
NAME=$1; SECS=${2:-90}; FROM=${3:-0}; SEQ=${4:-0}; shift $(( $# < 4 ? $# : 4 ))
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-1
export RG_INPUT="${RG_INPUT:-909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5,2223:CIRCLE:5,2277:CIRCLE:5,2900:LUP:240,3200:LLEFT:120,3400:LDOWN:200}"
export RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=30 RG_GS_SCALE=1
export RG_MC0_DIR=build/run/mc0-bench RG_SETTINGS_FILE=build/run/bench-settings.ini DQ8_GFX_STATS_EVERY=60
export RG_VU1_NATIVE=0 RG_DRAW_ORIGINS="build/run/$NAME.origins" RG_DRAW_ORIGINS_FROM=$FROM RG_DRAW_ORIGINS_SEQ=$SEQ
for kv in "$@"; do export "$kv"; done
unset RG_VU1_REPLAY RG_GS_FRAMELOG
cat disc/*.DAT disc/*.HD6 > /dev/null 2>&1
build/game/src/roguegalaxy disc/SCUS_974.90 > "build/run/$NAME.log" 2>&1 &
pid=$!
sleep "$SECS"
kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
grep -E "\[origins\]|\[crash\]|fatal|not in the function" "build/run/$NAME.log" | head -5
