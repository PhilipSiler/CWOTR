#!/bin/sh
# trial.sh <name> [VAR=value ...]: one 120-round trial from the reference player (r7 round 3120) with r7's settings plus
# the given settings, then 3,600 games each way against the reference from turn 1. One line is appended to
# runs/training-log.txt. Reference against itself: Free Peoples win 56.5% (Shadow conquest 31.7%, turn 11.4).
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; shift
rm -rf runs/$n runs/$n.log; mkdir runs/$n
for c in 3060 3080 3100 3120; do for x in fp sh pfp psh; do cp runs/r7/ckpt$c.$x runs/$n/; done; done
for x in fp sh pfp psh; do cp runs/r7/ckpt3120.$x runs/$n/latest.$x; done; echo 3120 > runs/$n/rounds
env WOTR_TEMP=0.06 WOTR_FLOOR=0.3 "$@" ./bin/wotr train runs/$n ${ROUNDS:-120} ${GAMES:-2000} 9 960 > runs/$n.log 2>&1
a=$(runs/ev.sh runs/$n/latest runs/r7/ckpt3120 3600 | sed -E 's/.*: FP wins //')
b=$(runs/ev.sh runs/r7/ckpt3120 runs/$n/latest 3600 | sed -E 's/.*: FP wins //')
echo "trial $n [$*] | as Free Peoples vs reference Shadow: FP wins $a || reference Free Peoples vs its Shadow: FP wins $b" >> runs/training-log.txt
touch runs/$n.done
