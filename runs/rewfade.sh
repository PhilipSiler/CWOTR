#!/bin/sh
# rewfade.sh <name> [WOTR_REW=FPMV,FPRV,FPML,FPRL,SPMV,SPRV,SPML,SPRL]: the fairer test of outcome values (2026-11-20). rew.sh switched the
# written move rules off at once, and the starting pair lost its human shape before any training. Here play starts with
# the move rules and leans on (10 turns, 15 Fellowship moves, Ring 40%) and they are taken away in steps while both sides
# learn together: move rules 0.3 0.2 0.1 0 0, leans 0.10 0.07 0.03 0 0, STAGE rounds each (default 30). The scoring
# formula stays at 0.30. Question: with the values in place, is the shape still there when the rules are gone?
# Read runs/<name>/summary.txt: the last training line of each stage, then the new pair with all guidance off.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; shift; d=runs/$n; R=${STAGE:-30}
mkdir $d || exit 1
for x in fp pfp; do cp runs/keep/fp_aw7.$x $d/latest.$x; done; for x in sh psh; do cp runs/raid2/latest.$x $d/latest.$x; done; echo 4200 > $d/rounds
k=1
for st in "0.3 0.10" "0.2 0.07" "0.1 0.03" "0 0" "0 0"; do
  set -- $st "$@"; nu=$1; le=$2; shift 2
  L=""; [ "$nu" = "0" ] && L="WOTR_LANE_FP=0"
  env WOTR_W=12:0,18:0 WOTR_NUDGE=$nu WOTR_HUMAN_SH=$le WOTR_HUMAN_FP=$le $L WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 "$@" ./bin/wotr_x train $d $R 2000 9 960 > $d/train$k.log 2>&1
  echo "stage $k (move rules $nu, leans $le): $(grep 'self-play:' $d/train$k.log | tail -1 | sed 's/.*games | //; s/draws.*turns/turns/' | cut -c1-150)" >> $d/summary.txt
  k=$((k + 1))
done
P="WOTR_W=12:0,18:0 WOTR_NUDGE=0 WOTR_LANE_FP=0 WOTR_HUMAN_SH=0 WOTR_HUMAN_FP=0"
echo "new pair, guidance off, played WITH its values: $(env $P "$@" ./bin/wotr_x eval $d/latest $d/latest 400 9 2>&1 | grep 'games | Shadow wins' | sed 's/.*games | //; s/draws.*turns/turns/' | cut -c1-150)" >> $d/summary.txt
# the plain game (every win counts 1), which is what matters in the end
for pr in "$d/latest $d/latest" "$d/latest runs/raid2/latest" "runs/keep/fp_aw7 $d/latest"; do env $P ./bin/wotr_x eval $pr 400 9 2>&1 | grep 'games | Shadow wins' | sed 's/: 396 games//; s/draws.*turns/turns/' | cut -c1-200; done >> $d/summary.txt
touch $d/done
