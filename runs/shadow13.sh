#!/bin/sh
# n4: as n3 but the two deployment guides at three-quarter strength -- from untrained networks, 240 rounds
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1 WOTR_FALL=1 WOTR_VPW=1 WOTR_INPUTS=1
rm -rf runs/n4; mkdir runs/n4
WOTR_DEPLOYW=0.75 WOTR_HUMAN=data/ladder2022/human.bin ./bin/wotr_n3 train runs/n4 240 2000 9 > runs/n4.log 2>&1
echo n4 >> runs/shadow13.progress
