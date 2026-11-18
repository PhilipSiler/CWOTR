#!/bin/sh
# Tests of the Shadow heuristics taken from the human games, each 240 rounds from the same saved state (runs/c1) with the
# same settings as the control (runs/t0). The heuristics are put back at their starting weights every 20 rounds.
#  m1: "not deployed" and "not at the walls" (empty Mordor, reach the settlements, go in)
#  m2: m1 plus "behind schedule", "conquest plan" and "sieges count reinforcements"
#  h1: the control plus human positions as extra training examples
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0
until ! pgrep -x wotr_plan >/dev/null; do sleep 30; done
rm -rf runs/m1; cp -r runs/c1 runs/m1
WOTR_TEMPO=0 WOTR_PLAN=0 WOTR_RESERVE=0 WOTR_DEPLOY=1 WOTR_REINJECT=20 ./bin/wotr_dep train runs/m1 240 2000 9 > runs/m1.log 2>&1
rm -rf runs/m2; cp -r runs/c1 runs/m2
WOTR_TEMPO=1 WOTR_PLAN=1 WOTR_RESERVE=1 WOTR_DEPLOY=1 WOTR_REINJECT=20 ./bin/wotr_dep train runs/m2 240 2000 9 > runs/m2.log 2>&1
rm -rf runs/h1; cp -r runs/c1 runs/h1
WOTR_TEMPO=0 WOTR_PLAN=0 WOTR_RESERVE=0 WOTR_DEPLOY=0 WOTR_REINJECT=0 WOTR_HUMAN=data/ladder2022/human.bin ./bin/wotr_dep train runs/h1 240 2000 9 > runs/h1.log 2>&1
echo done > runs/shadow3.done
