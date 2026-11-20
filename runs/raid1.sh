#!/bin/sh
# raid1.sh <name> <rounds> [extra env...]: as raid0.sh, but the Shadow learns against a MIX of frozen Free Peoples (POOL,
# comma-separated; default the raider + fp_aw7 + fp_aw4), and is then read alone against each of READ, N games each.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; R=$2; shift 2; d=runs/$n; F=${FROM:-runs/keep/shadow_aw6}
POOL=${POOL:-runs/slow2/fp31/latest,runs/keep/fp_aw7,runs/keep/fp_aw4}; READ=${READ:-"runs/slow2/fp31/latest runs/keep/fp_aw7 runs/keep/fp_aw4 runs/slow2/fp15/latest"}
mkdir $d || exit 1
for x in fp sh pfp psh; do cp $F.$x $d/latest.$x; done; echo 4200 > $d/rounds
env WOTR_FREEZE_FP=$POOL WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 \
    WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 WOTR_AWLES_SH=0.5,$(cat runs/keep/aw_shadow_files.txt) "$@" \
    ./bin/wotr_x train $d $R 2000 9 960 > $d/train.log 2>&1
./bin/wotr_x share $d/latest $d/s25 0.25
for fpp in $READ; do
  env WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 "$@" ./bin/wotr_x eval $fpp $d/s25 ${N:-400} 9 2>&1 | grep -E "^FP=|Free Peoples captured|average turns|Hunt before" | cut -c1-260
done > $d/ev.out
touch $d/done
