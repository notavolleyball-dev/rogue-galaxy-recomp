#!/bin/bash
# bisect.sh OUT ROOTNAME CANDIDATES [SEEDS=4]: which native callee makes ROOT
# mismatch in the harness. OUT is a tools/rgcpu/check_trees.sh output (its
# rg_harness and lib.so); CANDIDATES a file of function names (ROOT's native
# callees, tree.py). Each step runs ROOT with half the candidates on the
# oracle (RG_ORACLE). Run from the repository root.
OUT=$1; ROOT=$2; C=$3; SEEDS=${4:-4}
echo "$ROOT" > "$OUT/b_root.txt"
fails() { RG_ORACLE=$1 RG_ELF=disc/SCUS_974.90 timeout 120 "$OUT/rg_harness" "$OUT/lib.so" "$OUT/b_root.txt" "$SEEDS" 2>&1 | grep -q "^MISMATCH"; }
grep -v "^$ROOT\$" "$C" > "$OUT/b_cand.txt"
: > "$OUT/b_none.txt"
fails "$OUT/b_none.txt" || { echo "root passes with everything native"; exit 0; }
fails "$OUT/b_cand.txt" && { echo "root itself (fails with every callee oracle)"; exit 0; }
cp "$OUT/b_cand.txt" "$OUT/b_set.txt"
while [ "$(wc -l < "$OUT/b_set.txt")" -gt 1 ]; do
  n=$(( $(wc -l < "$OUT/b_set.txt") / 2 ))
  head -n $n "$OUT/b_set.txt" > "$OUT/b_half.txt"
  tail -n +$((n+1)) "$OUT/b_set.txt" > "$OUT/b_rest.txt"
  # oracle for the first half: if it passes, the culprit is in it
  if fails "$OUT/b_half.txt"; then cp "$OUT/b_rest.txt" "$OUT/b_set.txt"; else cp "$OUT/b_half.txt" "$OUT/b_set.txt"; fi
done
echo "culprit: $(cat "$OUT/b_set.txt")"
