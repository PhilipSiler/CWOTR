#!/bin/sh
# sd.sh <name> [extra env...]: one Shadow stretch from keep4200 against frozen Free Peoples MIXED: the defender
# (runs/keep/defender1), the sprinter (fp_1116) and the reference (r7); then Shadow with look-ahead at share 0.25
# against each of the three, 1,800 games. The folder runs/<name> must not exist yet (nothing is ever deleted here).
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; shift; d=runs/$n
mkdir $d || exit 1
for x in fp sh pfp psh; do cp ${START:-runs/n1/keep4200}.$x $d/latest.$x; done; echo 4200 > $d/rounds
env WOTR_FREEZE_FP=${OPP:-runs/keep/defender1,runs/keep/fp_1116,runs/r7/ckpt3120} WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 "$@" ./bin/wotr_t train $d ${ROUNDS:-120} 2000 9 960 > $d/train.log 2>&1
./bin/wotr_t share $d/latest $d/s25 0.25
for fp in runs/keep/defender1 runs/keep/fp_1116 runs/r7/ckpt3120; do
  echo "== $d/s25 vs $fp"
  WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 ./bin/wotr_t eval $fp $d/s25 1800 9 2>&1 | grep -E "^FP=|average turns to conclusion" | cut -c1-330
done > $d/ev.out 2>&1
touch $d/done
