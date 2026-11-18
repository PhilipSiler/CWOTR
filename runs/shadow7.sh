#!/bin/sh
# After c12 (every Shadow guide on, running now), each 240 rounds from the same saved state (runs/c1):
#  c13: c12 plus the garrison term nudged back with the others
#  c14: c13 plus the Free Peoples' side: no dice thrown away, a small push to move the Fellowship, the Fellowship pace term
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1
until ! pgrep -x wotr_c12 >/dev/null; do sleep 30; done
echo c12 >> runs/shadow5.progress
rm -rf runs/c13; cp -r runs/c1 runs/c13
./bin/wotr_c13 train runs/c13 240 2000 9 > runs/c13.log 2>&1
echo c13 >> runs/shadow7.progress
rm -rf runs/c14; cp -r runs/c1 runs/c14
./bin/wotr_c14 train runs/c14 240 2000 9 > runs/c14.log 2>&1
echo c14 >> runs/shadow7.progress
