#!/bin/bash
# cyc.sh NAME BIN: the unpaced walk; 20 s of hardware counters on the game
# thread once it is unpaced; cycles and instructions per frame.
S=$(dirname "$0"); NAME=$1; BIN=$2
(RG_DIR=$PWD RG_BIN=$BIN RG_NATIVE_PIPELINE=1 $S/walk.sh $NAME gpu 130 3000 >/dev/null 2>&1 &)
sleep 80
pid=$(pgrep -x "$(basename $BIN | cut -c1-15)"); tid=$(ps -L -o tid=,comm= -p $pid | awk '$2=="GameThread"{print $1}'); vtid=$(ps -L -o tid=,comm= -p $pid | awk '$2=="VIF1"{print $1}')
L=$(grep -c "\[gfx\] [0-9.]* fps" build/run/$NAME.log)
perf stat -x, -e cycles,instructions -t $tid -o build/run/$NAME.stat sleep 40 &
perf stat -x, -e cycles,instructions -t $vtid -o build/run/$NAME.vstat sleep 40
wait
L2=$(grep -c "\[gfx\] [0-9.]* fps" build/run/$NAME.log)
frames=$(( (L2 - L) * 60 ))
sleep 15
for f in stat vstat; do
awk -F, -v n=$NAME -v fr=$frames -v w=$f '/cycles/{c=$1} /instructions/{i=$1} END{printf "%-8s %-5s frames %5d  %6.2f Mcycles/frame  %6.2f Minstr/frame  IPC %.2f\n", n, (w=="stat"?"game":"vif1"), fr, c/fr/1e6, i/fr/1e6, i/c}' build/run/$NAME.$f
done
