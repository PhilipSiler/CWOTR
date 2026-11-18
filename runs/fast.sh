#!/bin/sh
# fast.sh <name>: one 120-round stretch of Shadow alone (as alone.sh) continuing from runs/a1/ckpt3840, with half the games
# starting from boards saved in our Free Peoples' games against AlphaWOTR's Shadow (turns 2-6, its big army already built).
# Then the reference Free Peoples play it (3,600 games); one line goes to runs/<name>.results. Compare a1.results @3840.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; shift
rm -rf runs/$n runs/$n.log; mkdir runs/$n
for c in 3780 3800 3820 3840; do for x in fp sh pfp psh; do cp runs/a1/ckpt$c.$x runs/$n/ 2>/dev/null; done; done
for x in fp sh pfp psh; do cp runs/a1/ckpt3840.$x runs/$n/latest.$x; done; echo 3840 > runs/$n/rounds
env WOTR_STATES=proto/games/r7-ckpt3120-fp60-positions/all-fp.states WOTR_START_SHARE=0.5 WOTR_FREEZE_FP=runs/r7/ckpt3120 WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 "$@" ./bin/wotr train runs/$n 120 2000 9 960 >> runs/$n.log 2>&1
r=$(cat runs/$n/rounds)
./bin/wotr eval runs/r7/ckpt3120 runs/$n/latest 3600 9 2>&1 | awk -v r=$r '/^FP=/{for(i=1;i<=NF;i++){if($i=="ring")a=$(i+1); if($i=="military"&&$(i-1)=="FP")b=$(i+1); if($i=="corruption")c=$(i+1); if($i=="military"&&$(i-1)=="SH")d=$(i+1)}} /average turns to conclusion/{t=$0; sub(/.*all games [0-9.]+; /,"",t)} /Shadow military victories/{e=$0; sub(/.*average turn [0-9.]+; /,"",e)} END{print "@" r " | Ring " a " FP-conquest " b " corruption " c " Shadow-conquest " d " | turns: " t " | " e}' >> runs/$n.results
touch runs/$n.done
