#!/bin/bash
# Reference captures for docs/NATIVE-ONLY.md: one build, one renderer mode,
# over fixed routes, writing a screenshot every SHOT pad reads.
#
#   tools/run/reference.sh LABEL MODE [ROUTE...]
#
#   LABEL   names the capture set: build/reference/ROUTE/LABEL/
#           (e.g. tag-gs, tag-native, m1). An existing set is replaced.
#   MODE    gs      the emulated GS path (only builds that still have it)
#           native  the native pipeline
#   ROUTE   any of the routes below; all of them when none is given.
#
#   EXE=PATH        another build (default build/game/src/roguegalaxy); for
#                   the tag: build it in a worktree and pass its executable.
#   SHOT=N          a screenshot every N pad reads (default per route).
#   NATIVE_CPU=0    the game's code through the oracle translation instead of
#                   tools/rgcpu's (default 1, as tools/run/play.sh).
#   VAR=value ...   after the route names, passed to the game.
#
# Every run is deterministic: the 30 fps mode on the pad clock, 1x, hidden
# and muted, a fresh copy of the route's memory card, the default settings.
# Two runs of one build give the same frames; tools/framediff.py compares two
# sets. Captures come from the disc: they stay in build/, never committed.
#
# Routes (memory cards as tools/bench/walk.sh and tools/run/late-run.sh
# keep them in build/run/; a card that is missing is copied from disc/mc0
# the way those scripts do, or the route is skipped and says so):
#   walk     File 01, the Salgin walk (tools/vu1/oracle/native-run.sh)
#   town     File 03 at Salgin's transporter, walked about the square
#   battle   File 03 by the waterway, a battle forced on the first step
#   gate     File 03 beside the boss gate, standing
#   late     the late save at Zerard's transporter, menu closed, a short walk
#   title    no save: the boot, the opening movie and the title screen
# Still to add (an input script recorded by the user, then a block below):
# an event with an item message and a battle dissolve, the item menu.
cd "$(dirname "$0")/../.." || exit 1
LABEL=$1; MODE=$2
if [ -z "$LABEL" ] || [ -z "$MODE" ]; then sed -n '2,36p' "$0"; exit 2; fi
shift 2
ROUTES=(); PASS=()
for a in "$@"; do case $a in *=*) PASS+=("$a") ;; *) ROUTES+=("$a") ;; esac; done
[ ${#ROUTES[@]} -eq 0 ] && ROUTES=(walk town battle gate late title)
GAME=${EXE:-build/game/src/roguegalaxy}
if [ ! -x "$GAME" ]; then echo "no build at $GAME"; exit 2; fi
if pgrep -x roguegalaxy > /dev/null; then echo "a game is running; not starting"; exit 2; fi

LOAD01="909:CROSS:5,999:CROSS:5,1059:DOWN:5,1118:CROSS:5,1238:CROSS:5,1358:CROSS:5,1478:CROSS:5,1569:CROSS:5,1629:CROSS:5,2019:CROSS:5,2069:CROSS:5"
LOAD03="$LOAD01,2223:CIRCLE:5,2277:CIRCLE:5"

# card MASTER SLOT: the master copy of a route's card, made from disc/mc0's
# slot when missing. Prints nothing and fails when it cannot be had.
card() {
    local master=$1 slot=$2
    if [ ! -d "$master/$slot" ]; then
        [ -d "disc/mc0/$slot" ] || return 1
        mkdir -p "$master" && cp -r "disc/mc0/$slot" "$master/"
    fi
}

route() {
    local name=$1 secs input master slot shot=120 extra=()
    case $name in
        walk)
            master=build/run/mc0-bench; slot=BASCUS-97490LV5RG_00; secs=110
            input="$LOAD01,2223:CIRCLE:5,2277:CIRCLE:5,2900:LUP:240,3200:LLEFT:120,3400:LDOWN:200" ;;
        town)
            master=build/run/mc0-town; slot=BASCUS-97490LV5RG_02; secs=100
            input=$LOAD03; local t=2400
            for d in LUP LUP LRIGHT LUP LLEFT LUP LUP LDOWN LRIGHT LUP; do input="$input,$t:$d:240"; t=$((t+260)); done ;;
        battle)
            master=build/run/mc0-battle; slot=BASCUS-97490LV5RG_02; secs=95
            input=$LOAD03; local t=2400
            while [ $t -lt 6100 ]; do input="$input,$t:LUP:300,$((t+330)):LDOWN:300"; t=$((t+660)); done
            t=2420; while [ $t -lt 6100 ]; do input="$input,$t:CROSS:4"; t=$((t+30)); done
            extra=(RG_FORCE_ENCOUNTER=1) ;;
        gate)
            master=build/run/mc0-gate; slot=BASCUS-97490LV5RG_02; secs=85; input=$LOAD03 ;;
        late)
            master=build/run/mc0-late; slot=; secs=90
            input="$LOAD01,2200:CIRCLE:5,2260:CIRCLE:5"; local t=2600
            for d in LUP LUP LRIGHT LUP LLEFT LDOWN; do input="$input,$t:$d:110"; t=$((t+135)); done ;;
        title)
            master=; slot=; secs=75; input=; shot=60 ;;
        *) echo "unknown route $name"; return 1 ;;
    esac
    shot=${SHOT:-$shot}

    local out=build/reference/$name/$LABEL run=build/reference/.run
    rm -rf "$out" "$run"; mkdir -p "$out" "$run/mc0"
    if [ -n "$master" ]; then
        if [ -n "$slot" ] && ! card "$master" "$slot"; then echo "$name: no card ($master/$slot, disc/mc0/$slot); skipped"; return 0; fi
        if [ ! -d "$master" ]; then echo "$name: no card ($master); skipped"; return 0; fi
        cp -r "$master"/. "$run/mc0/"
    fi

    (
        export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
        export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-? 2>/dev/null | head -1)")}
        unset RG_VU1_REPLAY RG_VU1_HARVEST RG_GS_FRAMELOG RG_PROFILE RG_NATIVE_PIPELINE
        export RG_INPUT="$input" RG_INPUT_CLOCK=pad RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1 RG_FPS=30 RG_GS_SCALE=1
        export RG_MC0_DIR="$run/mc0" RG_SETTINGS_FILE="$run/settings.ini" DQ8_GFX_STATS_EVERY=60
        export DQ8_GFX_SCREENSHOT_DIR="$out" DQ8_GFX_SCREENSHOT_EVERY=$shot
        [ "${NATIVE_CPU:-1}" = "1" ] && export RG_NATIVE_CPU=1
        case $MODE in
            gs) export RG_NATIVE_RENDERER=0 ;;
            native) export RG_NATIVE_RENDERER=1 RG_NATIVE_PIPELINE=1 ;;
        esac
        for a in "${extra[@]}" "${PASS[@]}"; do export "$a"; done
        {
            echo "route $name"; echo "label $LABEL"; echo "mode $MODE"; echo "exe $GAME"
            echo "commit $(git -C "$(dirname "$GAME")" rev-parse --short HEAD 2>/dev/null || echo ?)"
            echo "date $(date -u +%FT%TZ)"; echo "seconds $secs"; echo "shot $shot"
            env | grep -E '^(RG_|DQ8_GFX_)' | sort
        } > "$out/manifest.txt"
        cat disc/*.DAT disc/*.HD6 > /dev/null 2>&1
        "$GAME" disc/SCUS_974.90 > "$out/game.log" 2>&1 &
        pid=$!
        sleep "$secs"
        kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
    )
    local n; n=$(ls "$out"/frame_*.ppm 2>/dev/null | wc -l)
    echo "$name: $n frames in $out$(grep -qE '\[crash\]|fatal' "$out/game.log" && echo '  (crash or fatal in game.log)')"
}

case $MODE in gs|native) ;; *) echo "MODE is gs or native"; exit 2 ;; esac
for r in "${ROUTES[@]}"; do route "$r"; done
rm -rf build/reference/.run
