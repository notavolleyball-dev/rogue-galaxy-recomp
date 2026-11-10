#!/bin/bash
# One late-save run per Zerard destination (DOWN presses from Fountain
# Plaza in the Teleport list): warp to its first transporter, then walk
# in long legs with a Cross now and then, battles forced.
#   zerard-batch.sh DOWNS...
S=$(dirname "$0")
for downs in "$@"; do
    IN="2400:DOWN:5,2500:CROSS:5"
    for i in $(seq 1 "$downs"); do IN="$IN,$((2740+i*60)):DOWN:5"; done
    t=$((2740+downs*60+100))
    IN="$IN,$t:CROSS:5,$((t+100)):DOWN:5,$((t+200)):CROSS:5"
    t=$((t+600))
    dirs=(LUP LUP LRIGHT LUP LLEFT LDOWN LLEFT LUP LRIGHT LDOWN)
    for i in $(seq 0 49); do
        IN="$IN,$t:${dirs[$((i%10))]}:110"; t=$((t+115))
        t=$((t+20))
    done
    secs=$(( (t * 100 / 3700) + 40 ))
    "$S/late-run.sh" "zr$downs" "$secs" "$IN" 400 RG_FORCE_ENCOUNTER=1 | grep -v vu1native
    grep -c "\[encounter\] countdown" "$S/../../build/run/zr$downs.log"
done
