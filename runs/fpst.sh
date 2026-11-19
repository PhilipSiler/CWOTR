#!/bin/sh
# fpst.sh <name> <start FP player> <frozen Shadow list> [extra env...]: one ordinary stretch of the Free Peoples alone
# (honest rewards) against frozen Shadows mixed; then each of our Shadows with look-ahead against the result.
# The folder runs/<name> must not exist yet.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; start=$2; opp=$3; shift 3; d=runs/$n
mkdir $d || exit 1
for x in fp sh pfp psh; do cp $start.$x $d/latest.$x; done; echo 4200 > $d/rounds
env WOTR_FREEZE_SH=$opp WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 "$@" ./bin/wotr_t train $d ${ROUNDS:-120} 2000 9 960 > $d/train.log 2>&1
[ -n "$NOEV" ] && { touch $d/done; exit 0; }      # NOEV=1: skip the two big checks below (the caller measures)
for sh in runs/keep/shadow_sd33 runs/keep/shadow_sd30; do
  echo "== $d/latest vs $sh (Shadow look-ahead)"
  WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 ./bin/wotr_t eval $d/latest $sh 1800 9 2>&1 | grep -E "^FP=|average turns to conclusion" | cut -c1-330
done > $d/ev.out 2>&1
touch $d/done
