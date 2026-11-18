#!/bin/sh
# avg.sh <trial> ...: each trial's checkpoints 3200, 3220 and 3240 against the reference (1,800 games each way each), averaged
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
for n in "$@"; do
  fa=0; fs=0; ca=0
  L=${LAST:-3240}; for c in $((L-40)) $((L-20)) $L; do
    a=$(runs/ev.sh runs/$n/ckpt$c runs/r7/ckpt3120 1800 | sed -E 's/.*FP wins ([0-9.]+)%.*/\1/')
    l=$(runs/ev.sh runs/r7/ckpt3120 runs/$n/ckpt$c 1800)
    b=$(echo "$l" | sed -E 's/.*FP wins ([0-9.]+)%.*/\1/'); k=$(echo "$l" | sed -E 's/.*Shadow-conquest ([0-9.]+)%.*/\1/')
    fa=$(echo "$fa + $a" | bc -l); fs=$(echo "$fs + $b" | bc -l); ca=$(echo "$ca + $k" | bc -l)
  done
  printf "%s: as Free Peoples %.1f%% | reference Free Peoples against its Shadow %.1f%% (its conquest %.1f%%)\n" $n $(echo "$fa/3" | bc -l) $(echo "$fs/3" | bc -l) $(echo "$ca/3" | bc -l)
done
