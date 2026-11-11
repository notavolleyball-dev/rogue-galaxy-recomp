#!/bin/bash
# Bug-mark test: the late save hidden under RenderDoc, one mark at pad read $1 (2400).
cd "$(dirname "$0")/../.." || exit 1
pgrep -x roguegalaxy > /dev/null && { echo "a game is running"; exit 2; }
rm -rf build/run/mc0-run; cp -r build/run/mc0-late build/run/mc0-run
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-1 DISPLAY=${DISPLAY:-:1}
LOAD="909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5"
EXTRA=$(for r in $(seq 2400 120 4200); do printf ",%d:CROSS:5" $r; done)
export RG_INPUT="$LOAD,2200:CIRCLE:5,2260:CIRCLE:5$EXTRA" RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=30 RG_SCALE=2
export RG_MC0_DIR=build/run/mc0-run RG_SETTINGS_FILE=build/run/native-on.ini RG_NATIVE_CPU=1
export VK_ADD_IMPLICIT_LAYER_PATH=$HOME/.local/share/renderdoc-wayland ENABLE_VULKAN_RENDERDOC_CAPTURE=1 RG_BUG_MARK_AT=${1:-2400} RG_LOG_FILE=build/run/marktest.log
build/game/src/roguegalaxy disc/SCUS_974.90 > build/run/marktest.log 2>&1 &
pid=$!
WANT=$(echo "${1:-2400}" | tr ',' '\n' | wc -l)
for i in $(seq 1 200); do sleep 1; [ "$(grep -ac '\[bugmark\] done' build/run/marktest.log)" -ge "$WANT" ] && break; kill -0 $pid 2>/dev/null || break; done
sleep 1; kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
echo "waited ${i}s"
grep -a 'bugmark\|SDL GPU backend\|\[crash\]\|fatal\|\[rg\] GS' build/run/marktest.log | head -12
