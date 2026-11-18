#!/bin/sh
# alone.sh <name> <stretches>: Shadow trained alone against the frozen reference Free Peoples, no partial rewards; after each
# 120-round stretch the reference Free Peoples play it (3,600 games) and one line goes to runs/<name>.results
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1
if [ ! -d runs/$n ]; then mkdir runs/$n; for c in 3060 3080 3100 3120; do for x in fp sh pfp psh; do cp runs/r7/ckpt$c.$x runs/$n/; done; done; for x in fp sh pfp psh; do cp runs/r7/ckpt3120.$x runs/$n/latest.$x; done; echo 3120 > runs/$n/rounds; fi
for i in $(seq 1 $2); do
  env WOTR_FREEZE_FP=runs/r7/ckpt3120 WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 ./bin/wotr train runs/$n 120 2000 9 960 >> runs/$n.log 2>&1
  r=$(cat runs/$n/rounds)
  ./bin/wotr eval runs/r7/ckpt3120 runs/$n/latest 3600 9 2>&1 | awk -v r=$r '/^FP=/{for(i=1;i<=NF;i++){if($i=="ring")a=$(i+1); if($i=="military"&&$(i-1)=="FP")b=$(i+1); if($i=="corruption")c=$(i+1); if($i=="military"&&$(i-1)=="SH")d=$(i+1)}} /average turns to conclusion/{t=$0; sub(/.*all games [0-9.]+; /,"",t)} /Free Peoples captured/{k=$0; sub(/.*captured: /,"",k)} END{print "@" r " | Ring " a " FP-conquest " b " corruption " c " Shadow-conquest " d " | turns: " t " | taken: " k}' >> runs/$n.results
done
touch runs/$n.done
