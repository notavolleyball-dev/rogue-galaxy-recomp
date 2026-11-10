#!/bin/bash
# Run the walk as NAME; once unpaced, CPU ms per frame for the main threads over 10 s
# (Main: the presenter). RG_BIN picks the binary, as walk.sh.
NAME=$1
cd "$(dirname "$0")/../../.." || exit 1
(XDG_RUNTIME_DIR=/run/user/$(id -u) RG_WALK_SHOTS=0 RG_DRAW_STAGE=1 RG_NATIVE_CPU=1 RG_DIR=$PWD RG_NATIVE_PIPELINE=1 bash tools/rgcpu/bench/walk.sh $NAME gpu 110 3000 >/dev/null 2>&1 &)
for i in $(seq 1 100); do sleep 1; f=$(grep -oE '\[gfx\] [0-9]+' build/run/$NAME.log | tail -1 | grep -oE '[0-9]+'); [ "${f:-0}" -gt 200 ] && break; done; sleep 2
pid=$(pgrep -x roguegalaxy)
f0=$(grep -c '\[gfx\] [0-9.]* fps' build/run/$NAME.log)
declare -A t0
tidOf() { if [ "$1" = Main ]; then echo $pid; else ps -T -p $pid -o tid,comm | awk -v n=$1 '$2==n{print $1}'; fi; }
for th in GameThread Draw VIF1 Main; do tid=$(tidOf $th); t0[$th]=$(awk '{print $14+$15}' /proc/$pid/task/$tid/stat); tids[$th]=$tid; done
sleep 10
f1=$(grep -c '\[gfx\] [0-9.]* fps' build/run/$NAME.log)
frames=$(( (f1-f0)*60 ))
for th in GameThread Draw VIF1 Main; do tid=$(tidOf $th); t1=$(awk '{print $14+$15}' /proc/$pid/task/$tid/stat); awk -v a=${t0[$th]} -v b=$t1 -v f=$frames -v n=$th 'BEGIN{printf "%s %.3f ms/frame CPU\n", n, (b-a)*10/f}'; done
echo "frames $frames in 10 s"
