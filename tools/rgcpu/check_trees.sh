#!/bin/bash
# Translate the main ELF with tools/rgcpu and diff every native function --
# with its native callees -- against ps2_recomp's (rg_harness.cpp).
#
#   tools/rgcpu/check_trees.sh OUT [SEEDS=8] [ITERS=2000] [FILTER=.]
#
# Run from a checkout that has built the game. OUT/lib.so is reused when only
# FILTER changes (delete OUT to rebuild).
set -euo pipefail
ROOT=$(pwd); OUT=$(readlink -m "$1"); SEEDS=${2:-8}; ITERS=${3:-2000}; FILTER=${4:-.}
HERE=$(dirname "$(readlink -f "$0")")
GEN=$ROOT/build/generated/main
RT=$ROOT/thirdparty/PS2Recomp/ps2xRuntime
INC="-I$HERE -I$OUT/src -I$GEN -I$ROOT/src -I$RT/include -I$RT/src/lib/Kernel -I$ROOT/thirdparty/PS2Recomp/ps2xIOP/include"
FL="-std=c++20 -O2 -g0 -fno-strict-aliasing -fwrapv -msse4.1 -fPIC -include $ROOT/src/ee_fpu.h -w"
# The tables and the function ranges the harness dispatches with. The old
# run calls through a table of the oracle's functions; the new run through
# one where every translated function's slot holds its NAME_nc entry, as the
# game's does under RG_NATIVE_CPU (the native guard compares against it).
table() {
  {
    echo '#include "ps2_runtime.h"'
    echo '#include "ps2_recompiled_functions.h"'
    echo 'struct RgFunc { uint32_t start, end; PS2Runtime::RecompiledFunction fn; };'
    awk '{print "void " $1 "_nc(uint8_t*, R5900Context*, PS2Runtime*);"}' "$OUT/src/names.txt"
    echo 'extern "C" void rgFillTable(PS2Runtime::RecompiledFunction *t) {'
    awk '{print "    t[" $1 "] = &" $2 ";"}' "$OUT/src/table.txt"
    echo '}'
    echo 'extern "C" void rgFillTableNc(PS2Runtime::RecompiledFunction *t) {'
    awk 'NR==FNR {nc[$1]=1; next} {print "    t[" $1 "] = &" $2 (($2 in nc) ? "_nc" : "") ";"}' "$OUT/src/names.txt" "$OUT/src/table.txt"
    echo '}'
    echo 'extern "C" const RgFunc rgFuncs[] = {'
    sort "$OUT/src/funcs.txt" | awk '{print "    {0x" $1 "u, 0x" $2 "u, &" $3 "},"}'
    echo '};'
    echo "extern \"C\" const int rgFuncCount = $(wc -l < "$OUT/src/funcs.txt");"
    # Host stubs some oracle functions call directly: the harness's fake.
    echo 'void rgHarnessStub(R5900Context *ctx, uint32_t addr);'
    awk '{print "void " $2 "(uint8_t*, R5900Context* ctx, PS2Runtime*) { rgHarnessStub(ctx, 0x" $1 "u); }"}' "$OUT/src/stubs.txt"
    # How much a stub writes through a pointer into the caller's stack, by
    # name: the fake writes that much (rg_harness.cpp fakeCall).
    echo 'extern "C" const uint32_t rgStubOut[][2] = {'
    awk '{n = 8; if ($2 ~ /Matrix/) n = 64; else if ($2 ~ /Vec|Vector/) n = 16; print "    {0x" $1 "u, " n "u},"}' "$OUT/src/stubs.txt"
    echo '};'
    echo "extern \"C\" const int rgStubOutCount = $(wc -l < "$OUT/src/stubs.txt");"
  } > "$OUT/obj/table.cpp"
}
if [ -f "$OUT/lib.so" ] && [ -n "${NC_ONLY:-}" ]; then
  # Retranslate and rebuild only the native side (the oracle objects stay).
  python3 "$HERE/rgcpu.py" disc/SCUS_974.90 config/SCUS_974.90/functions_main.csv "$GEN" "$OUT/src"
  rm -f "$OUT"/obj/nc_* "$OUT"/obj/table.cpp*
  cp "$OUT"/src/nc_*.cpp "$OUT/obj/"
  table
  ls "$OUT"/obj/nc_*.cpp "$OUT"/obj/table.cpp | xargs -P "$(nproc)" -I{} sh -c "clang++ $FL $INC -c {} -o {}.o 2> {}.err || echo FAILED {}"
  clang++ -shared -o "$OUT/lib.so" "$OUT"/obj/*.o
fi
if [ ! -f "$OUT/lib.so" ]; then
  rm -rf "$OUT"; mkdir -p "$OUT/obj"
  python3 "$HERE/rgcpu.py" disc/SCUS_974.90 config/SCUS_974.90/functions_main.csv "$GEN" "$OUT/src"
  # Every oracle function that is not a host stub, 150 per translation unit.
  awk '{print "'"$GEN"'/" $3 ".cpp"}' "$OUT/src/funcs.txt" > "$OUT/oracle_files.txt"
  split -l 150 -d -a 3 "$OUT/oracle_files.txt" "$OUT/obj/oracle."
  for c in "$OUT"/obj/oracle.*; do sed 's|^|#include "|; s|$|"|' "$c" > "$c.cpp"; done
  cp "$OUT"/src/nc_*.cpp "$OUT/obj/"
  table
  ls "$OUT"/obj/*.cpp | xargs -P "$(nproc)" -I{} sh -c "clang++ $FL $INC -c {} -o {}.o 2> {}.err || echo FAILED {}"
  cat "$OUT"/obj/*.err | grep -E "error" | head -20 || true
  clang++ -shared -o "$OUT/lib.so" "$OUT"/obj/*.o
fi
clang++ -std=c++20 -O1 -msse4.1 $INC -include "$ROOT/src/ee_fpu.h" -w "$HERE/rg_harness.cpp" -o "$OUT/rg_harness" -rdynamic -ldl
[ -n "${NO_RUN:-}" ] && exit 0
grep -E "$FILTER" "$OUT/src/names.txt" > "$OUT/names.txt" || true
RG_ELF="$ROOT/disc/SCUS_974.90" "$OUT/rg_harness" "$OUT/lib.so" "$OUT/names.txt" "$SEEDS" bench "$ITERS" > "$OUT/result.txt" || true
tail -2 "$OUT/result.txt"
