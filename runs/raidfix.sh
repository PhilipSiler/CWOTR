#!/bin/sh
# raidfix.sh <name> <lesson share> [rounds=40]: the rule-free Shadow (runs/anc7/sh6) loses 92% to the raider slow2/fp31
# (shadow_aw6 with rules: 57%). raid1.sh cured the rule-using Shadow in 8 minutes against a mix of raider + fp_aw7 +
# fp_aw4; this is the same for the rule-free one: alone against those three (all with their rules), rule-free itself.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; LES=$2; R=${3:-40}; d=runs/rx_$n; mkdir $d || exit 1
for x in fp sh pfp psh; do cp ${FROM:-runs/anc7/sh6/latest}.$x $d/latest.$x; done; echo 4200 > $d/rounds
E="WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0 WOTR_W=12:0,18:0 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1"
LSH="WOTR_NUDGE_SH=0 WOTR_NUDGE_FP=0.3 WOTR_LEAN_ALL_SH=1"
L=""; [ "$LES" != "0" ] && L="WOTR_AWLES_SH=$LES,$(cat runs/keep/self_all_sh_files.txt)"
env $E $LSH WOTR_FREEZE_FP=${POOL:-runs/slow2/fp31/latest,runs/keep/fp_aw7,runs/keep/fp_aw4} $L $EXTRA ./bin/wotr_x train $d $R 2000 ${SEED:-9} 960 > $d/train.log 2>&1
ev() { env WOTR_W=12:0,18:0 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 WOTR_DEEP_SH=3,4,1,6 $LSH ./bin/wotr_x eval $1 $d/latest 400 9 2>&1 | grep 'games | Shadow wins' | sed 's/.*games | //; s/SH corruption/corr/; s/SH military/conq/; s/FP military/raids/; s/FP ring/ring/; s/draws.*turns/turns/; s/battles.*//' | cut -c1-95; }
echo "$n lessons $LES rounds $R [$EXTRA] | raider vs it: $(ev runs/slow2/fp31/latest) | fp_aw7 vs it: $(ev runs/keep/fp_aw7)" | tee -a runs/raidfix.txt
