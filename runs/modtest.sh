#!/bin/sh
# modtest.sh <name> [settings for the training, e.g. WOTR_REW=0.3,1,1,1,1,1,1,1]: one modifier test in under ten minutes
# (Philip, 2026-11-21). The test is the place where the rule-free Free Peoples go wrong: from the saved healthy
# generation-1 player (runs/anc2/fp1: 57% Shadow wins against shadow_aw6, Ring 40%, raids 2%, 15 Fellowship moves) they
# train ROUNDS (40) more rounds alone against the fixed Shadows (shadow_aw6, raid2 and anc2's generation-1 Shadow, all
# with their written rules), rule-free themselves, lessons at LES (0.2). With plain rewards that is where they slide:
# anc2 generation 2 = Shadow wins 63.1, Ring 31.1, raids 5.7, 13.4 moves (generation 3: Ring 21.0, raids 20.9, 11.0).
# Two plain readings afterwards, Shadow looking ahead, N (600) games each -> one line in runs/modtest.txt.
# SIDE=SH tests the Shadow the same way (from runs/anc2/sh1 against fp_aw7, fp_aw4 and anc2's generation-1 Free Peoples).
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; shift; d=runs/mt_$n; mkdir $d || exit 1
R=${ROUNDS:-40}; LES=${LES:-0.2}
E="WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0 WOTR_W=12:0,18:0 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1"
LFP="WOTR_NUDGE_FP=0 WOTR_LANE_FP=0 WOTR_NUDGE_SH=0.3 WOTR_LEAN_ALL_FP=1"
LSH="WOTR_NUDGE_SH=0 WOTR_NUDGE_FP=0.3 WOTR_LEAN_ALL_SH=1"
ev() { env WOTR_W=12:0,18:0 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 WOTR_DEEP_SH=3,4,1,6 $1 ./bin/wotr_x eval $2 $3 ${N:-600} 9 2>&1 | grep 'games | Shadow wins' | sed 's/.*games | //; s/SH corruption/corr/; s/SH military/conq/; s/FP military/raids/; s/FP ring/ring/; s/draws.*turns/turns/; s/battles.*//' | cut -c1-110; }
if [ "${SIDE:-FP}" = FP ]; then
  for x in fp sh pfp psh; do cp runs/anc2/fp1/latest.$x $d/latest.$x; done; echo 4200 > $d/rounds
  env $E $LFP WOTR_FREEZE_SH=runs/keep/shadow_aw6,runs/raid2/latest,runs/anc2/sh1/latest WOTR_AWLES_FP=$LES,$(cat runs/keep/self_all_fp_files.txt) "$@" ./bin/wotr_x train $d $R 2000 9 960 > $d/train.log 2>&1
  echo "$n [$*] FP | vs shadow_aw6: $(ev "$LFP" $d/latest runs/keep/shadow_aw6) | vs raid2: $(ev "$LFP" $d/latest runs/raid2/latest)" >> runs/modtest.txt
else
  for x in fp sh pfp psh; do cp runs/anc2/sh1/latest.$x $d/latest.$x; done; echo 4200 > $d/rounds
  env $E $LSH WOTR_FREEZE_FP=runs/keep/fp_aw7,runs/keep/fp_aw4,runs/anc2/fp1/latest WOTR_AWLES_SH=$LES,$(cat runs/keep/self_all_sh_files.txt) "$@" ./bin/wotr_x train $d $R 2000 9 960 > $d/train.log 2>&1
  echo "$n [$*] SH | fp_aw7 vs it: $(ev "$LSH" runs/keep/fp_aw7 $d/latest) | fp_aw4 vs it: $(ev "$LSH" runs/keep/fp_aw4 $d/latest)" >> runs/modtest.txt
fi
tail -1 runs/modtest.txt
