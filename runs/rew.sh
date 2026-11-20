#!/bin/sh
# rew.sh <name> <rounds> [WOTR_OUT_FP=... WOTR_OUT_SH=...]: does a choice of outcome values stop the collapse (Philip, 2026-11-20)?
# Both sides learn together from our last good players (Free Peoples fp_aw7, Shadow raid2) with ALL written guidance off
# (no formula, no move rules, no leans, no lessons) - the setting in which slow2 fell apart: the Free Peoples stop
# carrying the Ring. Corrected rules (pull requests 1 and 2). What to read: runs/<name>/summary.txt = the training games'
# line every 20 rounds (Ring share, Fellowship moves a game), then the new pair read against each other and the anchors.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; R=$2; shift 2; d=runs/$n
mkdir $d || exit 1
for x in fp pfp; do cp runs/keep/fp_aw7.$x $d/latest.$x; done; for x in sh psh; do cp runs/raid2/latest.$x $d/latest.$x; done; echo 4200 > $d/rounds
P="WOTR_W=12:0,18:0 WOTR_NUDGE=0 WOTR_LANE_FP=0 WOTR_HUMAN_SH=0 WOTR_HUMAN_FP=0"
env $P WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.12 WOTR_FLOOR=0 "$@" ./bin/wotr_x train $d $R 2000 9 960 > $d/train.log 2>&1
{ grep -E "^outcome values" $d/train.log; grep "self-play:" $d/train.log | awk 'NR==1 || NR%20==0' | cut -c1-215
  for pr in "$d/latest $d/latest" "$d/latest runs/raid2/latest" "runs/keep/fp_aw7 $d/latest"; do env $P ./bin/wotr_x eval $pr 400 9 2>&1 | grep -E "^FP=" | cut -c1-260; done; } > $d/summary.txt
touch $d/done
