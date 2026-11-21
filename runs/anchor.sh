#!/bin/sh
# anchor.sh <name> <generations>: rule-free learners trained against FIXED opponents that keep their rules (2026-11-21).
# The fade recipe (fade2.sh, full2) gives a rule-free, formula-free pair in human shape, but trained only against each
# other the two go soft together: against the old keepers they are 15-17 points weaker and get weaker with more rounds.
# Here each side learns alone, hold.sh's way: one generation = the Free Peoples train ROUNDS rounds against a pool of
# Shadows, then the Shadow trains ROUNDS rounds against a pool of Free Peoples. The pools are the old keepers (anchors)
# plus the newest generations. The learner plays with NO move rules and NO formula, leaning on its own suggester at
# every decision, with its own recorded lessons kept in (share LES, default 0.2); EXTRA = more settings for the training
# (for example the outcome modifiers, EXTRA="WOTR_REW=0.3,1,1,1,1,1,1,1"; the readings stay plain); every opponent plays with the written
# rules at 0.3 (the per-side switches WOTR_NUDGE_FP / _SH). Plain rewards.
# After each generation, 600 games a line -> runs/<name>/progress.txt, each side on its own settings, Shadow looking
# ahead: new Free Peoples vs shadow_aw6 (mark: fp_aw7 50.5, norules3 65.3), fp_aw7 vs new Shadow (mark 50.5, norules3
# 33.8), and the new pair against itself with no rules. Nothing is overwritten; the folder must not exist yet.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; G=$2; d=runs/$n; mkdir $d || exit 1
R=${ROUNDS:-40}; LES=${LES:-0.2}
ASH=${ASH:-"runs/keep/shadow_aw6 runs/raid2/latest"}; AFP=${AFP:-"runs/keep/fp_aw7 runs/keep/fp_aw4"}      # raid2 guards its strongholds: against shadow_aw6 alone the Free Peoples learn to raid (anc1)
SHP=""; FPP=""; csh=${SH0:-runs/keep/norules3}; cfp=${FP0:-runs/keep/norules3}
E="WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0 WOTR_W=12:0,18:0 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1"
LFP="WOTR_NUDGE_FP=0 WOTR_LANE_FP=0 WOTR_NUDGE_SH=0.3 WOTR_LEAN_ALL_FP=1"      # the Free Peoples are the rule-free side
LSH="WOTR_NUDGE_SH=0 WOTR_NUDGE_FP=0.3 WOTR_LEAN_ALL_SH=1"                     # the Shadow is the rule-free side
pool() { k=$1; a=$2; shift 2; echo $a $(echo $* | tr ' ' '\n' | tail -$k) | tr ' ' ','; }
ev() { env WOTR_W=12:0,18:0 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 WOTR_DEEP_SH=3,4,1,6 $1 ./bin/wotr_x eval $2 $3 ${N:-600} 9 2>&1 | grep 'games | Shadow wins' | sed 's/.*games | //; s/draws.*turns/turns/; s/battles.*//' | cut -c1-130; }
g=1
while [ $g -le $G ]; do
  f=$d/fp$g; mkdir $f; for x in fp sh pfp psh; do cp $cfp.$x $f/latest.$x; done; echo 4200 > $f/rounds
  env $E $LFP $EXTRA WOTR_FREEZE_SH=$(pool 2 "$ASH" $SHP) WOTR_AWLES_FP=$LES,$(cat runs/keep/self_all_fp_files.txt) ./bin/wotr_x train $f $R 2000 9 960 > $f/train.log 2>&1
  cfp=$f/latest; FPP="$FPP $cfp"
  s=$d/sh$g; mkdir $s; for x in fp sh pfp psh; do cp $csh.$x $s/latest.$x; done; echo 4200 > $s/rounds
  env $E $LSH $EXTRA WOTR_FREEZE_FP=$(pool 3 "$AFP" $FPP) WOTR_AWLES_SH=$LES,$(cat runs/keep/self_all_sh_files.txt) ./bin/wotr_x train $s $R 2000 9 960 > $s/train.log 2>&1
  csh=$s/latest; SHP="$SHP $csh"
  {
    echo "generation $g $(date +%H:%M)"
    echo "  new Free Peoples (no rules) vs shadow_aw6 (rules): $(ev "$LFP" $cfp runs/keep/shadow_aw6)"
    echo "  fp_aw7 (rules) vs new Shadow (no rules):           $(ev "$LSH" runs/keep/fp_aw7 $csh)"
    echo "  new Free Peoples (no rules) vs raid2 (rules):      $(ev "$LFP" $cfp runs/raid2/latest)"
    echo "  new pair, no rules on either side:                 $(ev "WOTR_NUDGE=0 WOTR_LANE_FP=0 WOTR_LEAN_ALL=1" $cfp $csh)"
  } >> $d/progress.txt
  g=$((g + 1))
done
touch $d/done
