#!/bin/sh
# league.sh: the overnight plan of 2026-11-16. Each side is trained alone against SEVERAL frozen opponents in rotation, including
# a sparring partner built to punish what the others let pass:
#   raider   Free Peoples for whom a Ring win counts half: goes after Shadow's strongholds
#   blitzer  Shadow for whom a corruption win counts half and a late win a little less: conquers fast
# Main players (the ones we would field) start from the reference r7/ckpt3120, the best against AlphaWOTR so far.
# No partial rewards anywhere. Results: runs/league.results (Free Peoples player first in every line).
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
L=runs/lg; REF=runs/r7/ckpt3120; mkdir -p $L
COMMON="WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3"
line() { ./bin/wotr eval $1 $2 3600 9 2>&1 | awk -v tag="$3" '/^FP=/{for(i=1;i<=NF;i++){if($i=="ring")a=$(i+1); if($i=="military"&&$(i-1)=="FP")b=$(i+1); if($i=="corruption")c=$(i+1); if($i=="military"&&$(i-1)=="SH")d=$(i+1)}} /average turns to conclusion/{t=$0; sub(/.*all games [0-9.]+; /,"",t)} END{print tag " | Ring " a " FP-conquest " b " corruption " c " Shadow-conquest " d " | turns: " t}' >> runs/league.results; }
# train <name> <SH|FP> <start player> <extra settings or -> <opponent> [<opponent> ...]: one 120-round stretch per opponent listed
train() {
  n=$1; side=$2; start=$3; extra=$4; shift 4
  d=$L/train_$n; rm -rf $d; mkdir $d
  for x in fp sh pfp psh; do cp $start.$x $d/latest.$x; done; echo 4000 > $d/rounds
  for opp in "$@"; do
    if [ $side = SH ]; then fz="WOTR_FREEZE_FP=$opp"; else fz="WOTR_FREEZE_SH=$opp"; fi
    [ "$extra" = - ] && extra=""
    env $fz $COMMON $extra ./bin/wotr train $d 120 2000 9 960 >> $L/train_$n.log 2>&1
    [ -e $L/STOP ] && break
  done
  for x in fp sh pfp psh; do cp $d/latest.$x $L/$n.$x; done
}
echo "=== $(date +%H:%M) sparring partners" >> runs/league.results
train raider FP $REF "WOTR_RINGWIN=0.5" $REF runs/a1/ckpt3840 $REF
line $L/raider $REF "raider FP vs reference Shadow"; line $L/raider runs/a1/ckpt3840 "raider FP vs a1 Shadow"
train blitzer SH runs/a1/ckpt3840 "WOTR_CORRWIN=0.5 WOTR_SOON=0.03" $REF runs/b1/ckpt4360 $REF
line $REF $L/blitzer "reference FP vs blitzer Shadow"; line runs/b1/ckpt4360 $L/blitzer "b1 FP vs blitzer Shadow"
r=1
while [ $r -le 2 ] && [ ! -e $L/STOP ]; do
  echo "=== $(date +%H:%M) round $r: main Shadow" >> runs/league.results
  if [ $r = 1 ]; then s0=$REF; f0=$REF; fpl=runs/b1/ckpt4360; shl=runs/a1/ckpt3840; else s0=$L/shadow; f0=$L/fp; fpl=$L/fp; shl=$L/shadow; fi
  train shadow SH $s0 - $L/raider $REF $fpl $L/raider $REF $fpl
  line $REF $L/shadow "round $r: reference FP vs main Shadow"; line $L/raider $L/shadow "round $r: raider FP vs main Shadow"; line runs/b1/ckpt4360 $L/shadow "round $r: b1 FP (Ring-runner) vs main Shadow"
  echo "=== $(date +%H:%M) round $r: main Free Peoples" >> runs/league.results
  train fp FP $f0 - $L/blitzer $REF $shl $L/blitzer $REF $shl
  line $L/fp $REF "round $r: main FP vs reference Shadow"; line $L/fp $L/blitzer "round $r: main FP vs blitzer Shadow"; line $L/fp runs/a1/ckpt3840 "round $r: main FP vs a1 Shadow"; line $L/fp $L/shadow "round $r: main FP vs main Shadow"
  for x in fp sh pfp psh; do cp $L/shadow.$x $L/shadow_r$r.$x; cp $L/fp.$x $L/fp_r$r.$x; done
  r=$((r + 1))
done
echo "=== $(date +%H:%M) finished" >> runs/league.results
touch runs/league.done
