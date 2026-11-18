#!/bin/sh
# n3: the human-examples test (n2) with the two deployment guides at half strength -- from untrained networks, 240 rounds
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1 WOTR_FALL=1 WOTR_VPW=1 WOTR_INPUTS=1
until grep -q n2 runs/shadow11.progress 2>/dev/null; do sleep 30; done
rm -rf runs/n3; mkdir runs/n3
WOTR_DEPLOYW=0.5 WOTR_HUMAN=data/ladder2022/human.bin ./bin/wotr_n3 train runs/n3 240 2000 9 > runs/n3.log 2>&1
echo n3 >> runs/shadow12.progress
