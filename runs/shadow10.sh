#!/bin/sh
# c16: c15 plus "holding a threatened home counts as being at the front" -- 240 rounds from the same saved state (runs/c1)
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1 WOTR_FALL=1 WOTR_VPW=1 WOTR_HOME=1
until [ -e runs/shadow9.progress ]; do sleep 30; done
rm -rf runs/c16; cp -r runs/c1 runs/c16
./bin/wotr_c16 train runs/c16 240 2000 9 > runs/c16.log 2>&1
echo c16 >> runs/shadow10.progress
