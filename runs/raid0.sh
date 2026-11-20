#!/bin/sh
# raid0.sh <name> <rounds> [extra env...]: does plain training teach the Shadow to beat the raiding Free Peoples of run slow2
# (runs/slow2/fp31/latest: leaves the Ring, takes Orthanc + Moria)? Shadow alone learns against it, frozen; the recipe of
# teach.sh (honest results, leans 0.1, AlphaWOTR lessons kept in, nudges 0.3). Then the Shadow is read alone against the
# raider and against fp_aw7 (what did it cost?), N games each -> runs/<name>/ev.out. FROM = the Shadow to start from.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; R=$2; shift 2; d=runs/$n; F=${FROM:-runs/keep/shadow_aw6}; RAID=${RAID:-runs/slow2/fp31/latest}
mkdir $d || exit 1
for x in fp sh pfp psh; do cp $F.$x $d/latest.$x; done; echo 4200 > $d/rounds
env WOTR_FREEZE_FP=$RAID WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 \
    WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 WOTR_AWLES_SH=0.5,$(cat runs/keep/aw_shadow_files.txt) "$@" \
    ./bin/wotr_x train $d $R 2000 9 960 > $d/train.log 2>&1
./bin/wotr_x share $d/latest $d/s25 0.25
for fpp in $RAID runs/keep/fp_aw7; do
  env WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 "$@" ./bin/wotr_x eval $fpp $d/s25 ${N:-400} 9 2>&1 | grep -E "^FP=|Free Peoples captured|average turns|Hunt before" | cut -c1-260
done > $d/ev.out
touch $d/done
