#!/bin/sh
# pool.sh <name> <generations>: the generation loop with a pool of fixed opponents (2026-11-19). Each generation:
#   1. the Free Peoples train alone (ROUNDS, default 40) against ALL Shadows in the pool (the newest six), dealt out among the games;
#   2. the result joins the Free Peoples pool; Shadow trains alone against ALL Free Peoples in the pool; the result joins the Shadow pool;
#   3. every new player is measured alone (one step ahead, no look-ahead, no rollouts; N games, default 900) against each pool member.
# Plain recipe: honest results, leans 0.1, AlphaWOTR lessons kept in. Nothing is ever overwritten; the folder must not exist yet.
# Pools start as: Shadow = shadow_aw6 (conquest-minded) + runs/tc2/s25 (hunter); Free Peoples = fp_aw7.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; G=$2; d=runs/$n; mkdir $d || exit 1
SHP="runs/keep/shadow_aw6 runs/tc2/s25"; FPP="runs/keep/fp_aw7"; csh=runs/tc2/s25; cfp=runs/keep/fp_aw7
last6() { echo $* | tr ' ' '\n' | tail -6 | paste -sd, -; }
E="WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1"
ev() { env WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 ./bin/wotr_t eval $1 $2 ${N:-900} 9 2>&1 | grep '^FP=' | sed 's/ | draws.*turns/ | turns/' | cut -c1-175; }
g=1
while [ $g -le $G ]; do
  f=$d/fp$g; mkdir $f; for x in fp sh pfp psh; do cp $cfp.$x $f/latest.$x; done; echo 4200 > $f/rounds
  env $E WOTR_FREEZE_SH=$(last6 $SHP) WOTR_AWLES_FP=0.5,$(cat runs/keep/aw_fp_files.txt) ./bin/wotr_t train $f ${ROUNDS:-40} 2000 9 960 > $f/train.log 2>&1
  ./bin/wotr_t share $f/latest $f/s25 0.25 > /dev/null; cfp=$f/s25
  echo "== generation $g, new Free Peoples against each Shadow in the pool"; for s in $SHP; do ev $cfp $s; done
  FPP="$FPP $cfp"
  s=$d/sh$g; mkdir $s; for x in fp sh pfp psh; do cp $csh.$x $s/latest.$x; done; echo 4200 > $s/rounds
  env $E WOTR_FREEZE_FP=$(last6 $FPP) WOTR_AWLES_SH=0.5,$(cat runs/keep/aw_shadow_files.txt) ./bin/wotr_t train $s ${ROUNDS:-40} 2000 9 960 > $s/train.log 2>&1
  ./bin/wotr_t share $s/latest $s/s25 0.25 > /dev/null; csh=$s/s25
  echo "== generation $g, new Shadow against each Free Peoples in the pool"; for p in $FPP; do ev $p $csh; done
  SHP="$SHP $csh"
  g=$((g + 1))
done
touch $d/done
