#!/bin/sh
# After c12 (every Shadow guide on, running now):
#  c13:  the same plus the garrison term nudged back with the others -- 240 rounds from the same saved state (runs/c1)
#  c13b: c13 carried on for 480 more rounds from where it ends
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5
until ! pgrep -x wotr_c12 >/dev/null; do sleep 30; done
echo c12 >> runs/shadow5.progress
rm -rf runs/c13; cp -r runs/c1 runs/c13
WOTR_GARRISON=1 ./bin/wotr_c13 train runs/c13 240 2000 9 > runs/c13.log 2>&1
echo c13 >> runs/shadow6.progress
rm -rf runs/c13b; cp -r runs/c13 runs/c13b
WOTR_GARRISON=1 ./bin/wotr_c13 train runs/c13b 480 2000 9 > runs/c13b.log 2>&1
echo c13b >> runs/shadow6.progress
