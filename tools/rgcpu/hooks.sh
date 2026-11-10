#!/bin/bash
# Rebuild config/SCUS_974.90/native-hooks.txt: boot the game with the native
# CPU in several setups (defaults; the native pipeline; every option that
# installs hooks on), collect the hooked main-ELF slots each finds
# (RG_NATIVE_CPU_HOOKS) and write the union. Then `setup.py recompile` and
# `build`: tools/rgcpu translates calls to a listed function as boundaries.
#
#   tools/rgcpu/hooks.sh [BIN=build/game/src/roguegalaxy]
cd "$(dirname "$0")/../.." || exit 1
BIN=${1:-build/game/src/roguegalaxy}
OUT=build/run/hooks; mkdir -p "$OUT"; rm -f "$OUT"/*.txt
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-$(basename "$(ls "$XDG_RUNTIME_DIR"/wayland-? 2>/dev/null | head -1)")}
boot() { # NAME ENV...
    local name=$1; shift
    (
        export RG_NATIVE_CPU=1 RG_NATIVE_CPU_HOOKS="$OUT/$name.txt" RG_MUTE=1 DQ8_GFX_HIDDEN_WINDOW=1
        export RG_SETTINGS_FILE="$OUT/$name.ini" RG_MC0_DIR="$OUT/mc0" RG_STALL_WATCH=0
        for e in "$@"; do export "$e"; done
        "$BIN" disc/SCUS_974.90 > "$OUT/$name.log" 2>&1 &
        pid=$!
        for _ in $(seq 1 60); do sleep 1; grep -q "\[rg\] native CPU:" "$OUT/$name.log" && break; kill -0 $pid 2>/dev/null || break; done
        kill -INT $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
        echo "$name: $(grep -m1 '\[rg\] native CPU' "$OUT/$name.log")"
        grep "is hooked but not in native-hooks" "$OUT/$name.log" | sed 's/^/  /'
    )
}
boot defaults
boot pipeline RG_NATIVE_RENDERER=1 RG_NATIVE_PIPELINE=1
boot options RG_NATIVE_RENDERER=1 RG_TEXT_LANG=FR RG_DIRECTOR_CUT_BALANCE=1 RG_DIRECTOR_CUT_ENEMIES=1 \
    RG_DIRECTOR_CUT_PARTY_AI=1 RG_MOUSE_CAMERA=1 RG_DEV_TOOLS=1
F=config/SCUS_974.90/native-hooks.txt
{ grep '^#' "$F"; cat "$OUT"/*.txt <(grep -v '^#' "$F") | tr 'A-F' 'a-f' | sort -u; } > "$OUT/merged"
added=$(comm -13 <(grep -v '^#' "$F" | sort -u) <(grep -v '^#' "$OUT/merged"))
mv "$OUT/merged" "$F"
echo "native-hooks.txt: $(grep -vc '^#' "$F") functions; added: ${added:-none}"
