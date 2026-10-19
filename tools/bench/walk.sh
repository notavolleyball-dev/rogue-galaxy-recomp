#!/bin/bash
# The Salgin walk on the pad clock, for speed and regression checks.
#
#   usage: tools/bench/walk.sh NAME [SCALE=3] [FPS=60|30]
#
# Loads the user's save (File 01) and walks the input script
# build/run/input_pad3.txt; the frame at pad read 2660 is logged to
# build/run/NAME.txt(.vram) and the log goes to build/run/NAME.log.
#   Speed:       SCALE=3 FPS=60; read the 6 [gfx] intervals before "frame
#                logged" (fpscmp.py), on an otherwise idle machine.
#   Correctness: SCALE=1 FPS=30, then cmpvram.py NAME REF against a reference
#                made with the same save (the 60 fps mode's steps follow the
#                frame time, so only the 30 fps walk is deterministic).
#   PROFILE=1    also samples every thread from 30 s in for 25 s into
#                build/run/NAME.prof (tools/profile/report.py); with LONG=1
#                the walk goes on 40 s more and the window is 62 s to 92 s,
#                all of it in the streets.
# Refuses to start while a game is running; only loads the save, never saves.
cd "$(dirname "$0")/../.." || exit 1
NAME=$1; SCALE=${2:-3}; FPS=${3:-60}
if [ -z "$NAME" ]; then echo "usage: walk.sh NAME [SCALE] [FPS]"; exit 2; fi
if tasklist | grep -qi "roguegalaxy.exe"; then echo "a game is running; not starting"; exit 2; fi
mkdir -p build/run
INPUT=build/run/input_pad3.txt
if [ "$LONG" = "1" ]; then
    # The same walk, then 40 s more of walking about the streets (for a
    # profile window that is all street, pad reads 3480..5470).
    INPUT=build/run/input_walk_long.txt
    if [ ! -f "$INPUT" ]; then
        # Down and up over the same stretch of street, so the walk never
        # leaves the area (a loop round the block reached a map change).
        extra=""
        t=3480
        while [ $t -lt 5500 ]; do extra="$extra,$t:LDOWN:120,$((t+170)):LUP:120"; t=$((t+340)); done
        echo "$(cat build/run/input_pad3.txt)$extra" > "$INPUT"
    fi
fi
export RG_INPUT="$(cat $INPUT)" RG_INPUT_CLOCK=pad RG_MUTE=1
export RG_GS_SCALE=$SCALE RG_SETTINGS_FILE="build/run/bench-settings.ini" DQ8_GFX_STATS_EVERY=60
export RG_GS_FRAMELOG="build/run/$NAME.txt@2660"
if [ "$FPS" = "60" ]; then export RG_FPS=60; else unset RG_FPS; fi
unset RG_GS_FRAMESKIP RG_TIME_TRACE RG_PROFILE RG_POS_TRACE
AFTER=8
if [ "$PROFILE" = "1" ]; then
    export RG_PROFILE="build/run/$NAME.prof" RG_PROFILE_START=30 RG_PROFILE_SECONDS=25 RG_PROFILE_STACKS=1
    AFTER=22
    if [ "$LONG" = "1" ]; then export RG_PROFILE_START=62 RG_PROFILE_SECONDS=30; fi
fi
if [ "$LONG" = "1" ]; then AFTER=52; fi
start=$(date +%s)
# A forward-slash disc path marks this instance; RogueGalaxy.bat passes a
# backslash one, so the user's game is never the one stopped below.
build/game/src/roguegalaxy.exe disc/SCUS_974.90 > "build/run/$NAME.log" 2>&1 &
for i in $(seq 1 120); do
    sleep 3
    grep -q "frame logged\|\[crash\]\|fatal" "build/run/$NAME.log" && break
done
sleep $AFTER
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='roguegalaxy.exe'\" | Where-Object { \$_.CommandLine -like '*disc/SCUS*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }"
echo "--- $NAME: scale $SCALE fps-mode $FPS, $(( $(date +%s) - start )) s"
grep -n "frame logged\|\[crash\]\|fatal\|VK_ERROR" "build/run/$NAME.log" | head -5
grep -m1 "internal scale" "build/run/$NAME.log"
if [ "$LONG" = "1" ]; then python tools/bench/fpscmp.py "build/run/$NAME.log" -n 6 -w 12:36; else python tools/bench/fpscmp.py "build/run/$NAME.log" -n 6; fi
