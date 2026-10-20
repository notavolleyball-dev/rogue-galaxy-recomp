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
#                References on the bench card: build/run/ref1x30c (1x) and
#                ref3x30c (3x), 2026-10-19 afternoon, File 01 of 2026-10-18
#                21:23 (the b ones differ only in 915 bytes of texture
#                area the old block patch left stale; the display buffer
#                is what must match).
#   PROFILE=1    also samples every thread from 30 s in for 25 s into
#                build/run/NAME.prof (tools/profile/report.py); with LONG=1
#                the walk goes on 40 s more and the window is 62 s to 92 s,
#                all of it in the streets.
#   BATTLE=1     the battle bench: the player's File 03 (build/run/mc0-battle,
#                three characters on a field), build/run/input_battle1.txt
#                loads it and walks up and down; a battle runs from about
#                80 s to 125 s, the frame is logged at pad read 5400 (in
#                it), the profile window is 85 s to 120 s, and fpscmp
#                prints the 25 intervals before the marker and the 50 after.
#                The battle's length and timing vary run to run (the 60 fps
#                walk is not deterministic): read the [gfx] draws per
#                second, 60-70k in the battle against 25-50k on the field,
#                or tools/bench/battlefps.py NAME..., the frame rate over
#                the intervals with 55k draws or more (the comparable number).
#   GATE=1       the boss gate bench: the player's File 03 saved beside it
#                (build/run/mc0-gate), loaded and left standing; prints
#                tools/bench/gatefps.py (fps, VIF1 ms a frame).
#   MC0=DIR      another memory card directory (default build/run/mc0-bench).
#   AFFINITY=HEX the instance's processor mask, set two seconds after start.
#   INPUT=FILE   another input script (default build/run/input_pad3.txt);
#                PROFILE_START=, PROFILE_SECONDS= and AFTER_OVERRIDE= (the
#                seconds kept running after the logged frame) go with it.
# Refuses to start while a game is running; loads File 01 from a copy of
# the card (build/run/mc0-bench), so the player's card is never touched.
cd "$(dirname "$0")/../.." || exit 1
NAME=$1; SCALE=${2:-3}; FPS=${3:-60}
if [ -z "$NAME" ]; then echo "usage: walk.sh NAME [SCALE] [FPS]"; exit 2; fi
if tasklist | grep -qi "roguegalaxy.exe"; then echo "a game is running; not starting"; exit 2; fi
mkdir -p build/run
INPUT=${INPUT:-build/run/input_pad3.txt}
if [ "$BATTLE" = "1" ]; then
    INPUT=build/run/input_battle1.txt
    MC0=${MC0:-build/run/mc0-battle}
    FRAME=${FRAME:-5400}
    AFTER_OVERRIDE=${AFTER_OVERRIDE:-70}
    PROFILE_START=${PROFILE_START:-85}
    PROFILE_SECONDS=${PROFILE_SECONDS:-35}
    if [ ! -d build/run/mc0-battle/BASCUS-97490LV5RG_02 ]; then
        mkdir -p build/run/mc0-battle && cp -r disc/mc0/BASCUS-97490LV5RG_02 build/run/mc0-battle/
    fi
fi
if [ "$GATE" = "1" ]; then
    # The boss gate: the user's File 03 as saved beside it (2026-10-19
    # 21:16, copied to build/run/mc0-gate; delete the copy to refresh it),
    # loaded and left standing, where the VIF1 thread is the limit.
    INPUT=build/run/input_gate0.txt
    MC0=${MC0:-build/run/mc0-gate}
    FRAME=${FRAME:-3200}
    AFTER_OVERRIDE=${AFTER_OVERRIDE:-25}
    if [ ! -f "$INPUT" ]; then
        echo "909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5,2223:CIRCLE:5,2277:CIRCLE:5" > "$INPUT"
    fi
    if [ ! -d build/run/mc0-gate/BASCUS-97490LV5RG_02 ]; then
        mkdir -p build/run/mc0-gate && cp -r disc/mc0/BASCUS-97490LV5RG_02 build/run/mc0-gate/
    fi
fi
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
# The scripted load menu picks a save by position: a second save of the
# player's would change which one loads. The runs use a card holding only
# File 01 (copied once from disc/mc0; delete the copy to refresh it).
if [ ! -d build/run/mc0-bench/BASCUS-97490LV5RG_00 ]; then
    mkdir -p build/run/mc0-bench && cp -r disc/mc0/BASCUS-97490LV5RG_00 build/run/mc0-bench/
fi
# MC0=DIR: another card, e.g. build/run/mc0-battle holding only the
# player's File 03 (two characters, a field where walking meets a battle).
export RG_MC0_DIR=${MC0:-build/run/mc0-bench}
export RG_GS_SCALE=$SCALE RG_SETTINGS_FILE="build/run/bench-settings.ini" DQ8_GFX_STATS_EVERY=60
# FRAME=N logs the frame at another pad read (the battle script: 5400).
export RG_GS_FRAMELOG="build/run/$NAME.txt@${FRAME:-2660}"
if [ "$FPS" = "60" ]; then export RG_FPS=60; else unset RG_FPS; fi
unset RG_GS_FRAMESKIP RG_TIME_TRACE RG_PROFILE RG_POS_TRACE
# TIME_TRACE=1: the 60 fps mode's timing lines ([timing] frame time and
# scale every 300 updates).
if [ "$TIME_TRACE" = "1" ]; then export RG_TIME_TRACE=1; fi
AFTER=8
if [ "$PROFILE" = "1" ]; then
    # PROFILE_STACKS=0 samples leaf pcs only, some 50 times as often (for
    # instruction-level reads of one hot function).
    export RG_PROFILE="build/run/$NAME.prof" RG_PROFILE_STACKS=${PROFILE_STACKS:-1}
    export RG_PROFILE_START=${PROFILE_START:-30} RG_PROFILE_SECONDS=${PROFILE_SECONDS:-25}
    AFTER=22
    if [ "$LONG" = "1" ]; then export RG_PROFILE_START=${PROFILE_START:-62} RG_PROFILE_SECONDS=${PROFILE_SECONDS:-30}; fi
fi
if [ "$LONG" = "1" ]; then AFTER=52; fi
# INPUT=, PROFILE_START=, PROFILE_SECONDS= and AFTER= override the above
# for other scripts (the street view: build/run/input_street.txt).
AFTER=${AFTER_OVERRIDE:-$AFTER}
# The walk's inputs count pad reads, and the pad is read during the load
# screen too: a load that takes longer on the clock (the disc's data out of
# the file cache) starts the walk at another point of the area's arrival and
# the frame differs. Reading the data first keeps the load time the same
# (4 s when cold, nothing when cached; 2026-10-19: three runs out of four
# diverged until this).
cat disc/*.DAT disc/*.HD6 > /dev/null 2>&1
start=$(date +%s)
# A forward-slash disc path marks this instance; RogueGalaxy.bat passes a
# backslash one, so the user's game is never the one stopped below.
build/game/src/roguegalaxy.exe disc/SCUS_974.90 > "build/run/$NAME.log" 2>&1 &
# AFFINITY=HEX: the instance's processor mask (5555: one logical processor
# per core of an 8-core SMT machine), to see whether two busy threads
# sharing a core is what varies between runs.
if [ -n "$AFFINITY" ]; then
    sleep 2
    powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='roguegalaxy.exe'\" | Where-Object { \$_.CommandLine -like '*disc/SCUS*' } | ForEach-Object { (Get-Process -Id \$_.ProcessId).ProcessorAffinity = 0x$AFFINITY }"
fi
for i in $(seq 1 120); do
    sleep 3
    grep -q "frame logged\|\[crash\]\|fatal" "build/run/$NAME.log" && break
done
sleep $AFTER
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='roguegalaxy.exe'\" | Where-Object { \$_.CommandLine -like '*disc/SCUS*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }"
echo "--- $NAME: scale $SCALE fps-mode $FPS, $(( $(date +%s) - start )) s"
grep -n "frame logged\|\[crash\]\|fatal\|VK_ERROR" "build/run/$NAME.log" | head -5
grep -m1 "internal scale" "build/run/$NAME.log"
if [ "$LONG" = "1" ]; then python tools/bench/fpscmp.py "build/run/$NAME.log" -n 6 -w 12:36
elif [ "$BATTLE" = "1" ]; then python tools/bench/fpscmp.py "build/run/$NAME.log" -n 25 -w 0:50
elif [ "$GATE" = "1" ]; then python tools/bench/gatefps.py "$NAME"
else python tools/bench/fpscmp.py "build/run/$NAME.log" -n 6; fi
