#!/bin/sh
# cycle.sh <first half number> <halves> <Shadow player> <Free Peoples player>
# The back-and-forth: Shadow trains alone for three stretches, then the Free Peoples train alone for three, each against
# frozen opponents, and so on. To stop a side learning one opponent's habits, the frozen opponent alternates by stretch
# between the other side's latest player and the old reference (r7/ckpt3120). No partial rewards.
# Shadow's first stretch of each half is against $D/raider if it exists: a Free Peoples sparring partner trained to win
# by taking Shadow's strongholds (WOTR_RINGWIN), because a Shadow never attacked at home learns to empty its strongholds.
# After each half: the new side against the other side's latest player and against the reference, 3,600 games each ->
# $D.results (Free Peoples listed first in every line). Latest players: $D/shadow.* and $D/fp.*
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
REF=runs/r7/ckpt3120
D=${CYC:-runs/cyc}     # CYC=<folder>: a separate back-and-forth with its own players and results file
mkdir -p $D
[ -e $D/shadow.fp ] || for x in fp sh pfp psh; do cp $3.$x $D/shadow.$x; cp $4.$x $D/fp.$x; done
line() { ./bin/wotr eval $1 $2 3600 9 2>&1 | awk -v tag="$3" '/^FP=/{for(i=1;i<=NF;i++){if($i=="ring")a=$(i+1); if($i=="military"&&$(i-1)=="FP")b=$(i+1); if($i=="corruption")c=$(i+1); if($i=="military"&&$(i-1)=="SH")d=$(i+1)}} /average turns to conclusion/{t=$0; sub(/.*all games [0-9.]+; /,"",t)} END{print tag " | Ring " a " FP-conquest " b " corruption " c " Shadow-conquest " d " | turns: " t}'; }
half() {   # half <side SH|FP> <cycle>
  d=$D/train_$1; rm -rf $d; mkdir $d
  if [ $1 = SH ]; then src=$D/shadow; else src=$D/fp; fi
  for x in fp sh pfp psh; do cp $src.$x $d/latest.$x; done; echo 4000 > $d/rounds
  for st in $(seq 1 ${STRETCHES:-3}); do
    # the frozen opponents are MIXED within every round (the trainer deals them out among its threads): one after another,
    # each stretch undid the one before (after 120 rounds against the raider alone Shadow had stopped hunting)
    if [ $1 = SH ]; then opp=$D/fp,$REF; [ -e $D/raider.fp ] && opp=$opp,$D/raider; fz="WOTR_FREEZE_FP=$opp"; else fz="WOTR_FREEZE_SH=$D/shadow,$REF"; fi
    env $fz WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 $EXTRA ./bin/wotr train $d 120 2000 9 960 >> $D/train_$1.log 2>&1
  done
  for x in fp sh pfp psh; do cp $d/latest.$x $src.$x; cp $d/latest.$x $D/c$2_$1.$x; done
  if [ $1 = SH ]; then line $D/fp $D/shadow "cycle $2 Shadow trained: our FP vs new Shadow"; line $REF $D/shadow "cycle $2 Shadow trained: reference FP vs new Shadow"; [ -e $D/raider.fp ] && line $D/raider $D/shadow "cycle $2 Shadow trained: raider FP vs new Shadow"
  else line $D/fp $D/shadow "cycle $2 FP trained: new FP vs our Shadow"; line $D/fp $REF "cycle $2 FP trained: new FP vs reference Shadow"; fi >> $D.results
}
# Which side trains next: whichever is losing between our two latest players (Philip: "if either side is getting too
# dominant, then just train more stretches for the weaker side"). $2 is the number of halves to run.
c=$1; end=$(($1 + $2))
while [ $c -lt $end ]; do
  [ -e $D/STOP ] && break
  p=$(./bin/wotr eval $D/fp $D/shadow 3600 9 2>&1 | awk '/^FP=/{for(i=1;i<=NF;i++) if($i=="wins") w=$(i+1); sub(/%/,"",w); print 100 - w}')
  if [ "$(echo "$p > 50" | bc -l)" = 1 ]; then side=SH; else side=FP; fi
  [ -n "$SIDE" ] && side=$SIDE      # SIDE=SH or FP: train that side this time whatever the balance
  echo "half $c: Free Peoples win $p% between our latest pair -> training $side" >> $D.results
  half $side $c
  c=$((c + 1))
done
touch $D.done
