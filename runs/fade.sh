#!/bin/sh
# fade.sh <name>: Shadow alone against a frozen Free Peoples (fp_aw7, which keeps its own rules), with everything that
# did not come from self-play faded out in six stages of 48 rounds and then two more stages with all of it gone:
#   the hand-written formula and the written move rules (they scale with it): 0.30 -> 0 in a straight line over 288 rounds
#   the lean toward the human/AlphaWOTR-taught move-suggester:               0.10 0.08 0.06 0.04 0.02 0, then 0
#   the AlphaWOTR lessons kept in each round's fit (share):                   0.5 0.4 0.3 0.2 0.1 0, then 0
# Honest results throughout. After each stage the player is read alone (1,800 games) at that stage's settings.
# FROM (runs/keep/shadow_aw6). The folder must not exist yet.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; d=runs/$n; F=${FROM:-runs/keep/shadow_aw6}
mkdir $d || exit 1
for x in fp sh pfp psh; do cp $F.$x $d/latest.$x; done
echo 672 > $d/rounds        # the formula's share is 1 - rounds/960: 0.30 now, 0 after 288 more rounds
st=1
for s in "0.10 0.5" "0.08 0.4" "0.06 0.3" "0.04 0.2" "0.02 0.1" "0 0" "0 0" "0 0"; do
  set -- $s; lean=$1; les=$2
  L=""; [ "$les" != "0" ] && L="WOTR_AWLES_SH=$les,$(cat runs/keep/aw_shadow_files.txt)"
  env WOTR_FREEZE_FP=runs/keep/fp_aw7 WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0 WOTR_W=12:0,18:0 \
      WOTR_HUMAN_SH=$lean WOTR_HUMAN_FP=0.1 $L ./bin/wotr_t train $d 48 2000 9 960 > $d/train$st.log 2>&1
  for x in fp sh pfp psh; do cp $d/latest.$x $d/stage$st.$x; done
  echo "stage $st (lean $lean, lessons $les; $(grep 'strategy formula still' $d/train$st.log | tail -1 | sed 's/^ *//')): own training games, last 20 rounds: $(grep 'self-play:' $d/train$st.log | tail -20 | awk '{for(i=1;i<=NF;i++){if($i=="wins")w+=$(i+1); if($i=="military"&&$(i-1)=="SH")m+=$(i+1)}} END{printf "Shadow %.1f%%, conquest %.1f%%", w/NR, m/NR}')"
  echo "   read alone: $(env WOTR_W=12:0,18:0 WOTR_HUMAN_SH=$lean WOTR_HUMAN_FP=0.1 ./bin/wotr_t eval runs/keep/fp_aw7 $d/latest 1800 9 2>&1 | grep '^FP=' | sed 's/ | draws.*turns/ | turns/' | LC_ALL=C cut -c60-175)"
  st=$((st + 1))
done
touch $d/done
