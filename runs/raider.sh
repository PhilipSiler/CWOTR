#!/bin/sh
# raider.sh <stretches>: the stronghold-raiding sparring partner. Free Peoples trained alone from the reference against the
# frozen reference Shadow, with a Ring win counting only half (WOTR_RINGWIN=0.5). Result: runs/cyc/raider.*
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
mkdir -p runs/cyc; d=runs/cyc/train_raider; rm -rf $d; mkdir $d
for x in fp sh pfp psh; do cp runs/r7/ckpt3120.$x $d/latest.$x; done; echo 4000 > $d/rounds
for st in $(seq 1 $1); do
  opp=runs/r7/ckpt3120; [ $st = 2 ] && opp=runs/a1/ckpt3840
  env WOTR_FREEZE_SH=$opp WOTR_RINGWIN=0.5 WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 ./bin/wotr train $d 120 2000 9 960 >> runs/cyc/train_raider.log 2>&1
done
for x in fp sh pfp psh; do cp $d/latest.$x runs/cyc/raider.$x; done
for sh in runs/r7/ckpt3120 runs/a1/ckpt3840; do ./bin/wotr eval runs/cyc/raider $sh 3600 9 2>&1 | awk -v tag="raider FP vs Shadow $sh" '/^FP=/{for(i=1;i<=NF;i++){if($i=="ring")a=$(i+1); if($i=="military"&&$(i-1)=="FP")b=$(i+1); if($i=="corruption")c=$(i+1); if($i=="military"&&$(i-1)=="SH")d=$(i+1)}} END{print tag " | Ring " a " FP-conquest " b " corruption " c " Shadow-conquest " d}'; done >> runs/cycle.results
touch runs/raider.done
