#!/bin/sh
# c15, restarted with the fall forecast that does not read "under siege": 240 rounds from the same saved state (runs/c1),
# all Shadow guides, garrison and victory-point weights kept alive, the Free Peoples' guides, Minas Morgul left out of
# "empty Mordor", the attack nudge only for attacks on Free Peoples settlements.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1 WOTR_FALL=1 WOTR_VPW=1
rm -rf runs/c15; cp -r runs/c1 runs/c15
./bin/wotr_c15 train runs/c15 240 2000 9 > runs/c15.log 2>&1
echo c15 >> runs/shadow9.progress
