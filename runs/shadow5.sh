#!/bin/sh
# After the test now running (m2: lane guards + deployment terms):
#  c12:  every Shadow guide on, including the conquest clock -- 240 rounds from the same saved state as the control (runs/c1)
#  c12b: the same settings carried on for 480 more rounds from where c12 ends, to see whether it holds
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5
until ! pgrep -x wotr_lane >/dev/null; do sleep 30; done
echo m2 >> runs/shadow4.progress
rm -rf runs/c12; cp -r runs/c1 runs/c12
./bin/wotr_c12 train runs/c12 240 2000 9 > runs/c12.log 2>&1
echo c12 >> runs/shadow5.progress
rm -rf runs/c12b; cp -r runs/c12 runs/c12b
./bin/wotr_c12 train runs/c12b 480 2000 9 > runs/c12b.log 2>&1
echo c12b >> runs/shadow5.progress
