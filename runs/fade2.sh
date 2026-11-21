#!/bin/sh
# fade2.sh <name> [WOTR_REW=...]: as slowfade.sh, but each step sets BOTH the move rules and the scoring formula's
# share: STEPS = "rules:formula rules:formula ..." (2026-11-21). The formula carries about twice the weight of the
# network in these players and knows nothing of the outcome modifiers; the end state wanted is no formula and no rules.
# STAGE rounds a step (default 20). FROM_FP / FROM_SH = starting players (default: the last healthy step of sf0, rules 0.10).
# LEAN=<w>: hold the lean toward the player's OWN move suggester at w throughout (it is learned, not hand-written:
# the suggester is refitted every round on the player's own choices); without it the lean is a third of the move rules.
# Read runs/<name>/summary.txt; players saved per step as step<k>. NEVER edit this file while a run is using it.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; shift; d=runs/$n; R=${STAGE:-20}
ST=${STEPS:-"0.10:0.25 0.10:0.20 0.10:0.15 0.10:0.10 0.10:0.05 0.10:0"}
FFP=${FROM_FP:-runs/sf0/step5}; FSH=${FROM_SH:-runs/sf0/step5}
mkdir $d || exit 1
for x in fp pfp; do cp $FFP.$x $d/latest.$x; done; for x in sh psh; do cp $FSH.$x $d/latest.$x; done; echo 4200 > $d/rounds
k=1
for st in $ST; do
  nu=${st%%:*}; fl=${st##*:}; le=${LEAN:-$(awk -v n=$nu 'BEGIN{printf "%.3f", n / 3}')}
  L=""; [ "$nu" = "0" ] && L="WOTR_LANE_FP=0"
  env WOTR_W=12:0,18:0 WOTR_NUDGE=$nu WOTR_HUMAN_SH=$le WOTR_HUMAN_FP=$le $L WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=$fl "$@" ./bin/wotr_x train $d $R 2000 9 960 > $d/train$k.log 2>&1
  echo "step $k (move rules $nu, formula $fl): $(grep 'self-play:' $d/train$k.log | tail -1 | sed 's/.*games | //; s/draws.*turns/turns/' | cut -c1-150) | $(grep -o 'mean |prior| [0-9.]*, mean |network correction| [0-9.]*' $d/train$k.log | tail -1)" >> $d/summary.txt
  for x in fp pfp sh psh; do cp $d/latest.$x $d/step$k.$x; done
  k=$((k + 1))
done
P="WOTR_W=12:0,18:0 WOTR_NUDGE=$nu WOTR_HUMAN_SH=${LEAN:-0} WOTR_HUMAN_FP=${LEAN:-0}"
for pr in "$d/latest $d/latest" "$d/latest runs/raid2/latest" "runs/keep/fp_aw7 $d/latest"; do env $P "$@" ./bin/wotr_x eval $pr 400 9 2>&1 | grep 'games | Shadow wins' | sed 's/: 396 games//; s/draws.*turns/turns/' | cut -c1-200; done >> $d/summary.txt
touch $d/done
