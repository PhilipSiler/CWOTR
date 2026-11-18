#!/bin/sh
# After the first three tests: two more 240-round tests from the same saved state (runs/c1), same settings.
#  h1: the control plus human positions as extra training examples (nothing else changed)
#  t3: "behind schedule", "conquest plan" and "sieges count reinforcements" together, no human positions
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0
until [ -e runs/shadow1.done ]; do sleep 30; done
rm -rf runs/h1; cp -r runs/c1 runs/h1
WOTR_TEMPO=0 WOTR_PLAN=0 WOTR_RESERVE=0 WOTR_HUMAN=data/ladder2022/human.bin ./bin/wotr_hum train runs/h1 240 2000 9 > runs/h1.log 2>&1
rm -rf runs/t3; cp -r runs/c1 runs/t3
WOTR_TEMPO=1 WOTR_PLAN=1 WOTR_RESERVE=1 ./bin/wotr_hum train runs/t3 240 2000 9 > runs/t3.log 2>&1
echo done > runs/shadow2.done
