#!/bin/sh
# slowfade.sh <name> [WOTR_REW=FPMV,FPRV,FPML,FPRL,SPMV,SPRV,SPML,SPRL]: the slow version of rewfade.sh (Philip, 2026-11-20:
# fade the hand-written bonuses slowly from our best human-like players, with the outcome modifiers in place). The thirds
# of rewfade.sh hold their shape down to move rules 0.1 and break on the last step to 0, so the steps here get finer
# toward the bottom: STEPS (default 0.30 0.25 0.20 0.15 0.10 0.075 0.05 0.025 0 0), STAGE rounds each (default 20).
# Leans are a third of the move rules. The scoring formula stays at FLOORV (0.30). FROM_FP / FROM_SH = starting players.
# Read runs/<name>/summary.txt: the last training line of each step, then the new pair with all guidance off.
# NEVER edit this file while a run is using it (sh reads it as it goes).
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; shift; d=runs/$n; R=${STAGE:-20}; ST=${STEPS:-"0.30 0.25 0.20 0.15 0.10 0.075 0.05 0.025 0 0"}
FFP=${FROM_FP:-runs/keep/fp_aw7}; FSH=${FROM_SH:-runs/raid2/latest}
mkdir $d || exit 1
for x in fp pfp; do cp $FFP.$x $d/latest.$x; done; for x in sh psh; do cp $FSH.$x $d/latest.$x; done; echo 4200 > $d/rounds
k=1
for nu in $ST; do
  le=$(awk -v n=$nu 'BEGIN{printf "%.3f", n / 3}')
  L=""; [ "$nu" = "0" ] && L="WOTR_LANE_FP=0"
  env WOTR_W=12:0,18:0 WOTR_NUDGE=$nu WOTR_HUMAN_SH=$le WOTR_HUMAN_FP=$le $L WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=${FLOORV:-0.3} "$@" ./bin/wotr_x train $d $R 2000 9 960 > $d/train$k.log 2>&1
  echo "step $k (move rules $nu): $(grep 'self-play:' $d/train$k.log | tail -1 | sed 's/.*games | //; s/draws.*turns/turns/' | cut -c1-150)" >> $d/summary.txt
  for x in fp pfp sh psh; do cp $d/latest.$x $d/step$k.$x; done
  k=$((k + 1))
done
P="WOTR_W=12:0,18:0 WOTR_NUDGE=0 WOTR_LANE_FP=0 WOTR_HUMAN_SH=0 WOTR_HUMAN_FP=0"
echo "new pair, guidance off, played WITH its values: $(env $P "$@" ./bin/wotr_x eval $d/latest $d/latest 400 9 2>&1 | grep 'games | Shadow wins' | sed 's/.*games | //; s/draws.*turns/turns/' | cut -c1-150)" >> $d/summary.txt
for pr in "$d/latest $d/latest" "$d/latest runs/raid2/latest" "runs/keep/fp_aw7 $d/latest"; do env $P ./bin/wotr_x eval $pr 400 9 2>&1 | grep 'games | Shadow wins' | sed 's/: 396 games//; s/draws.*turns/turns/' | cut -c1-200; done >> $d/summary.txt
touch $d/done
