#!/bin/bash
# The native pipeline's pass criteria (docs/NATIVE-PIPELINE.md, steps 5b-7c)
# from a run's log: build/run/NAME.log. Prints one line per check.
#   tools/native-check.sh NAME
set -u
LOG="build/run/${1:?usage: native-check.sh NAME}.log"
[ -f "$LOG" ] || { echo "no $LOG"; exit 1; }

sites() { grep -a "^  $1 " "$LOG" | sed 's/: [0-9].*//' | sort | uniq -c | sort -rn; }
# The sum of a site's per-window counts over the run.
total() { grep -a "^  .*$1: [0-9]" "$LOG" | sed "s/.*$1: \([0-9]*\).*/\1/" | awk '{s+=$1} END {print s+0}'; }
# The sum of a "label N" count in the producer's lines.
count() { grep -ao "$1 [0-9]*" "$LOG" | awk '{s+=$NF} END {print s+0}'; }
verdict() { if [ "$2" = "0" ]; then echo "PASS  $1"; else echo "FAIL  $1 ($2)"; fi; }

echo "== $LOG"
echo "-- G2 / G4 sites seen (windows):"
sites G2
sites G4
verdict "7a: no GS::writeRegister" "$(total 'GS::writeRegister')"
verdict "7a: no GS::drawRegisters" "$(total 'GS::drawRegisters (the register file)')"
verdict "7b: no depthprobe::read" "$(total 'depthprobe::read (depth by GS address)')"
verdict "7c: no hostreads::begin" "$(total 'hostreads::begin (a target by GS address)')"
verdict "5b/5c: no SubmitNativeEffect" "$(total 'renderer SubmitNativeEffect (GS buffers, registers)')"
echo "INFO  5b: green-to-alpha ops made: $(count 'made green to alpha')"
echo "INFO  5c: objects routed: $(count 'object layers: routed'), tails made: $(count 'tails made'), put back: $(count 'put back for packets')"
echo "INFO  7b: $(grep -ao 'depth reads (all so far) [0-9]*: answered [0-9]*, no depth target there [0-9]*' "$LOG" | tail -1)"
grep -ao "depth reads: asked.*" "$LOG" | tail -1 | sed 's/^/INFO  7b renderer: /'
grep -ao "colour reads: served.*" "$LOG" | tail -1 | sed 's/^/INFO  7c renderer: /'
echo "-- errors:"
grep -a "depth reads off\|colour read .* failed\|a colour read was not served\|frame lost\|object layers: a tail\|\[native-pipeline\] refused:" "$LOG" | sort | uniq -c | head
