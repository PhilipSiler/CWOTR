#!/bin/sh
# slow.sh <name> <generations>: the long, slow fade (Philip, 2026-11-19: "slowly fade the formula out over time; slow
# learning rate, lots of exploration"). Both sides learn, each alone against a pool of fixed opponents (pool.sh's loop):
# one generation = the Free Peoples train ROUNDS rounds against the Shadow pool, then Shadow trains ROUNDS rounds against
# the Free Peoples pool, and both results join the pools. The pools are the two anchors that keep their hand-written rules
# (so nobody forgets how to meet them) plus the newest four generations.
# What fades, in a straight line from generation 1 to generation FADE and then stays at nothing:
#   the hand-written formula's share and the written move rules that scale with it   0.30 -> 0
#   the lean toward the human/AlphaWOTR-taught move-suggester                        0.10 -> 0
#   the AlphaWOTR lessons kept in every round's fit (share)                          0.5  -> 0
# Honest win/loss results throughout. Settings (mine, from his words): LR 1e-4 (a third of the usual), TEMP 0.12 (twice it).
# After every generation both new players are read alone, 900 games against each anchor -> runs/<name>/progress.txt.
# Nothing is ever overwritten; the folder must not exist yet.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; G=$2; d=runs/$n; mkdir $d || exit 1
R=${ROUNDS:-100}; FADE=${FADE:-30}; LR=${LR:-0.0001}; TEMP=${TEMP:-0.12}
ASH="runs/keep/shadow_aw6 runs/keep/shadow_hunter1"; AFP="runs/keep/fp_aw4 runs/keep/fp_aw7"
SHP=""; FPP=""; csh=runs/keep/shadow_aw6; cfp=runs/keep/fp_aw4
H=$((R * FADE * 10 / 3))            # the formula's share is 1 - rounds/H: 0.30 at rounds = 0.7 H, nothing FADE generations later
pool() { a=$1; shift; echo $a $(echo $* | tr ' ' '\n' | tail -4) | tr ' ' ','; }
ev() { env WOTR_W=12:0,18:0 WOTR_HUMAN_SH=$lean WOTR_HUMAN_FP=$lean ./bin/wotr_t eval $1 $2 ${N:-900} 9 2>&1 | grep '^FP=' | sed 's/.*| Shadow wins \([0-9.]*\)% | FP ring \([0-9.]*\)% FP military \([0-9.]*\)% | SH corruption \([0-9.]*\)% SH military \([0-9.]*\)%.*turns \([0-9.]*\) fsp.*/Shadow \1 (corruption \4, conquest \5; Ring \2; \6 turns)/'; }
g=1
while [ $g -le $G ]; do
  start=$((H * 7 / 10 + (g - 1) * R))
  lean=$(awk -v g=$g -v f=$FADE 'BEGIN{x=1-(g-1)/f; if(x<0)x=0; printf "%.3f", 0.1*x}'); les=$(awk -v g=$g -v f=$FADE 'BEGIN{x=1-(g-1)/f; if(x<0)x=0; printf "%.3f", 0.5*x}')
  E="WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=$TEMP WOTR_LR=$LR WOTR_FLOOR=0 WOTR_W=12:0,18:0 WOTR_HUMAN_SH=$lean WOTR_HUMAN_FP=$lean"
  f=$d/fp$g; mkdir $f; for x in fp sh pfp psh; do cp $cfp.$x $f/latest.$x; done; echo $start > $f/rounds
  L=""; [ "$les" != "0.000" ] && L="WOTR_AWLES_FP=$les,$(cat runs/keep/aw_fp_files.txt)"
  env $E WOTR_FREEZE_SH=$(pool "$ASH" $SHP | tr ' ' ',') $L ./bin/wotr_t train $f $R 2000 9 $H > $f/train.log 2>&1
  cfp=$f/latest; FPP="$FPP $cfp"
  s=$d/sh$g; mkdir $s; for x in fp sh pfp psh; do cp $csh.$x $s/latest.$x; done; echo $start > $s/rounds
  L=""; [ "$les" != "0.000" ] && L="WOTR_AWLES_SH=$les,$(cat runs/keep/aw_shadow_files.txt)"
  env $E WOTR_FREEZE_FP=$(pool "$AFP" $FPP | tr ' ' ',') $L ./bin/wotr_t train $s $R 2000 9 $H > $s/train.log 2>&1
  csh=$s/latest; SHP="$SHP $csh"
  {
    echo "generation $g $(date +%H:%M) (formula $(grep 'strategy formula still' $s/train.log | tail -1 | sed 's/.*still at //; s/ of its.*//'), lean $lean, lessons $les; $(grep '^it ' $s/train.log | tail -1 | cut -d' ' -f3-4) a round)"
    echo "  new Shadow vs fp_aw4:           $(ev runs/keep/fp_aw4 $csh)"
    echo "  new Shadow vs fp_aw7:           $(ev runs/keep/fp_aw7 $csh)"
    echo "  shadow_aw6 vs new Free Peoples: $(ev $cfp runs/keep/shadow_aw6)"
    echo "  hunter vs new Free Peoples:     $(ev $cfp runs/keep/shadow_hunter1)"
    echo "  new Shadow vs new Free Peoples: $(ev $cfp $csh)"
  } >> $d/progress.txt 2>&1
  g=$((g + 1))
done
touch $d/done
