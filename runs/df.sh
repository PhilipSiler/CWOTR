#!/bin/sh
# df.sh <name> <start FP player> <frozen Shadow list>: one stretch of the DEFENDER sparring partner (Free Peoples alone, a
# corruption loss counted 0.7 for them, so denying conquest comes first), then our Shadows against it. The folder
# runs/<name> must not exist yet.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; d=runs/$n
mkdir $d || exit 1
for x in fp sh pfp psh; do cp $2.$x $d/latest.$x; done; echo 4200 > $d/rounds
env WOTR_FREEZE_SH=$3 WOTR_CORRWIN=0.3 WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 ./bin/wotr_t train $d 120 2000 9 960 > $d/train.log 2>&1
for sh in runs/keep/shadow_sd9 runs/keep/shadow_sd8 runs/keep/shadow_1116; do
  echo "== $d/latest vs $sh (Shadow look-ahead)"
  WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 ./bin/wotr_t eval $d/latest $sh 1800 9 2>&1 | grep -E "^FP=|average turns to conclusion" | cut -c1-330
done > $d/ev.out 2>&1
touch $d/done
