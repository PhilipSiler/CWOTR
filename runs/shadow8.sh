#!/bin/sh
# Each 240 rounds from the same saved state (runs/c1), all Shadow guides on, garrison term kept alive:
#  c14: plus the Free Peoples' guides (no dice thrown away, a push to move the Fellowship, the Fellowship pace term)
#  c15: c14 plus the two-turn fall forecast with the victory-point weights kept alive, Minas Morgul left out of
#       "empty Mordor", and the attack nudge only for attacks on Free Peoples settlements
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1
until ! pgrep -x wotr_c13 >/dev/null; do sleep 30; done
echo c13 >> runs/shadow7.progress
rm -rf runs/c14; cp -r runs/c1 runs/c14
./bin/wotr_c14 train runs/c14 240 2000 9 > runs/c14.log 2>&1
echo c14 >> runs/shadow8.progress
rm -rf runs/c15; cp -r runs/c1 runs/c15
WOTR_FALL=1 WOTR_VPW=1 ./bin/wotr_c15 train runs/c15 240 2000 9 > runs/c15.log 2>&1
echo c15 >> runs/shadow8.progress
