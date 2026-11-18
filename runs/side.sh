#!/bin/sh
# side.sh <name> <FP|SH> <frozen opponent> <start player> <stretches>
# Trains one side alone (no partial rewards) from <start player>, with <frozen opponent> unchanged in the other seat. After each
# 120-round stretch the new side plays the frozen opponent AND the old reference (r7/ckpt3120) on the other side, 3,600 games each;
# one line each goes to runs/<name>.results (Free Peoples always listed first).
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; side=$2; opp=$3; start=$4
if [ ! -d runs/$n ]; then mkdir runs/$n; for x in fp sh pfp psh; do cp $start.$x runs/$n/latest.$x; done; echo 4000 > runs/$n/rounds; fi      # (any count past 672 keeps the formula at its 30% floor)
line() { ./bin/wotr eval $1 $2 3600 9 2>&1 | awk -v tag="$3" '/^FP=/{for(i=1;i<=NF;i++){if($i=="ring")a=$(i+1); if($i=="military"&&$(i-1)=="FP")b=$(i+1); if($i=="corruption")c=$(i+1); if($i=="military"&&$(i-1)=="SH")d=$(i+1)}} /average turns to conclusion/{t=$0; sub(/.*all games [0-9.]+; /,"",t)} END{print tag " | Ring " a " FP-conquest " b " corruption " c " Shadow-conquest " d " | turns: " t}'; }
for i in $(seq 1 $5); do
  if [ $side = FP ]; then fz="WOTR_FREEZE_SH=$opp"; else fz="WOTR_FREEZE_FP=$opp"; fi
  env $fz WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 ./bin/wotr train runs/$n 120 2000 9 960 >> runs/$n.log 2>&1
  r=$(cat runs/$n/rounds)
  if [ $side = FP ]; then line runs/$n/latest $opp "@$r new FP vs its opponent"; line runs/$n/latest runs/r7/ckpt3120 "@$r new FP vs reference Shadow"
  else line $opp runs/$n/latest "@$r its opponent vs new Shadow"; line runs/r7/ckpt3120 runs/$n/latest "@$r reference FP vs new Shadow"; fi >> runs/$n.results
done
touch runs/$n.done
