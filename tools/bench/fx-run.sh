#!/bin/bash
# A native effect's correctness run on Linux: the File 01 walk (origins-run.sh's
# input, which goes on to pad read 3600), 30 fps mode, 1x, hidden and muted, the
# native renderer on, the frame at pad read PAD logged to build/run/NAME.txt(.vram).
#   tools/bench/fx-run.sh NAME PAD [extra env...]
# Compare two runs' frames with tools/bench/cmpvram.py NAME REF and
# tools/bench/vram2png.py OUT.png build/run/NAME.txt.vram build/run/REF.txt.vram;
# a reference is the same run with the effect's switch at 0 (RG_NATIVE_DOF=0, ...).
cd "$(dirname "$0")/../.." || exit 1
NAME=$1; PAD=${2:-3000}; shift $(( $# < 2 ? $# : 2 ))
if [ -z "$NAME" ]; then echo "usage: fx-run.sh NAME PAD [env...]"; exit 2; fi
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi
mkdir -p build/run
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-wayland-1}
export RG_INPUT="${RG_INPUT:-909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5,2223:CIRCLE:5,2277:CIRCLE:5,2900:LUP:240,3200:LLEFT:120,3400:LDOWN:200}"
export RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=30 RG_GS_SCALE=${SCALE:-1}
export RG_MC0_DIR=build/run/mc0-bench RG_SETTINGS_FILE=build/run/bench-settings.ini DQ8_GFX_STATS_EVERY=60
export RG_NATIVE_RENDERER=${RG_NATIVE_RENDERER:-1} RG_GS_FRAMELOG="build/run/$NAME.txt@$PAD"
for kv in "$@"; do export "$kv"; done
unset RG_VU1_REPLAY RG_DRAW_ORIGINS
cat disc/*.DAT disc/*.HD6 > /dev/null 2>&1
rm -f "build/run/$NAME.txt" "build/run/$NAME.txt.vram"
build/game/src/roguegalaxy disc/SCUS_974.90 > "build/run/$NAME.log" 2>&1 &
pid=$!
# The frame is logged about 60 s in at pad 3000; give up after LIMIT seconds.
for _ in $(seq 1 "${LIMIT:-150}"); do
    sleep 1
    grep -q "frame logged" "build/run/$NAME.log" 2>/dev/null && break
    kill -0 $pid 2>/dev/null || break
done
sleep "${AFTER:-2}"
kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
grep -E "frame logged|\[nativefx\]|\[crash\]|fatal" "build/run/$NAME.log" | head -${LINES_SHOWN:-20}
