#!/bin/sh
# fade2.sh <name> [WOTR_REW=...]: as slowfade.sh, but each step sets BOTH the move rules and the scoring formula's
# share: STEPS = "rules:formula rules:formula ..." (2026-11-21). The formula carries about twice the weight of the
# network in these players and knows nothing of the outcome modifiers; the end state wanted is no formula and no rules.
# STAGE rounds a step (default 20). FROM_FP / FROM_SH = starting players (default: the last healthy step of sf0, rules 0.10).
# LEAN=<w>: hold the lean toward the player's OWN move suggester at w throughout (it is learned, not hand-written:
# the suggester is refitted every round on the player's own choices); without it the lean is a third of the move rules.
# SEED=<n>: the training seed (default 9), for measuring how much two identical runs differ.
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
  env WOTR_W=12:0,18:0 WOTR_NUDGE=$nu WOTR_HUMAN_SH=$le WOTR_HUMAN_FP=$le $L WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=$fl "$@" ./bin/wotr_x train $d $R 2000 ${SEED:-9} 960 > $d/train$k.log 2>&1
  # the step's AVERAGE over its rounds (single rounds swing by ten points: alternate rounds meet different earlier versions)
  echo "step $k (move rules $nu, formula $fl): $(grep 'self-play:' $d/train$k.log | sed 's/.*Shadow wins \([0-9.]*\)% | FP ring \([0-9.]*\)% FP military \([0-9.]*\)% | SH corruption \([0-9.]*\)% SH military \([0-9.]*\)%.*turns \([0-9.]*\) fsp moves \([0-9.]*\).*/\1 \2 \3 \4 \5 \6 \7/' | awk '{for(i=1;i<=7;i++)t[i]+=$i; n++} END{printf "Shadow wins %.1f%% | FP ring %.1f%% FP military %.1f%% | SH corruption %.1f%% SH military %.1f%% | turns %.1f fsp moves %.1f", t[1]/n,t[2]/n,t[3]/n,t[4]/n,t[5]/n,t[6]/n,t[7]/n}') | $(grep -o 'mean |prior| [0-9.]*, mean |network correction| [0-9.]*' $d/train$k.log | tail -1)" >> $d/summary.txt
  for x in fp pfp sh psh; do cp $d/latest.$x $d/step$k.$x; done
  # a clean reading of the step: the new pair against itself and the new Shadow against fp_aw7, this step's rules and lean,
  # no exploration, no earlier versions (the training games above mix in earlier versions, which moves their shares a lot)
  for pr in "$d/latest $d/latest" "runs/keep/fp_aw7 $d/latest"; do
    echo "    reading, $pr: $(env WOTR_W=12:0,18:0 WOTR_NUDGE=$nu WOTR_HUMAN_SH=$le WOTR_HUMAN_FP=$le $L "$@" ./bin/wotr_x eval $pr 300 9 2>&1 | grep 'games | Shadow wins' | sed 's/.*games | //; s/draws.*turns/turns/' | cut -c1-140)" >> $d/summary.txt
  done
  k=$((k + 1))
done
P="WOTR_W=12:0,18:0 WOTR_NUDGE=$nu WOTR_HUMAN_SH=${LEAN:-0} WOTR_HUMAN_FP=${LEAN:-0}"
for pr in "$d/latest $d/latest" "$d/latest runs/raid2/latest" "runs/keep/fp_aw7 $d/latest"; do env $P "$@" ./bin/wotr_x eval $pr 400 9 2>&1 | grep 'games | Shadow wins' | sed 's/: 396 games//; s/draws.*turns/turns/' | cut -c1-200; done >> $d/summary.txt
touch $d/done
