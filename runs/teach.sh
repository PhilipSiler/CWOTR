#!/bin/sh
# teach.sh <name> <rounds> [extra env...]: Shadow alone learns against a frozen Free Peoples (fp_aw7), plain recipe (honest
# results, leans 0.1, AlphaWOTR lessons kept in). Extra env switches the teacher on, e.g.
#   WOTR_ROLL_LEAN=1 WOTR_ROLL_TEACH=100,8,0.3,0.5   (100 rollouts choose and teach 30% of Shadow's sampled action decisions)
# Then the pupil is measured ALONE (one step ahead, no look-ahead, no rollouts): 1,800 games against fp_aw7 -> runs/<name>/ev.out.
# FROM (runs/keep/shadow_aw6) is the Shadow to start from. The folder must not exist yet.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; R=$2; shift 2; d=runs/$n; F=${FROM:-runs/keep/shadow_aw6}
mkdir $d || exit 1
for x in fp sh pfp psh; do cp $F.$x $d/latest.$x; done; echo 4200 > $d/rounds
env WOTR_FREEZE_FP=runs/keep/fp_aw7 WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 \
    WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 WOTR_AWLES_SH=0.5,$(cat runs/keep/aw_shadow_files.txt) "$@" \
    ./bin/wotr_t train $d $R 2000 9 960 > $d/train.log 2>&1
./bin/wotr_t share $d/latest $d/s25 0.25
WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 ./bin/wotr_t eval runs/keep/fp_aw7 $d/s25 ${N:-1800} 9 2>&1 | grep -E "^FP=|average turns|Gondor falls" | cut -c1-260 > $d/ev.out
touch $d/done
