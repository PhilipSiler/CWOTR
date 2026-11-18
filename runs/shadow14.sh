#!/bin/sh
# p1: the human-examples test (n2: full-strength guides, guides as inputs, human examples) with one change --
#     the version in training plays the games it learns from. From untrained networks, 240 rounds.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1 WOTR_FALL=1 WOTR_VPW=1 WOTR_INPUTS=1 WOTR_ONPOLICY=1
until [ -e runs/shadow13.progress ]; do sleep 30; done
rm -rf runs/p1; mkdir runs/p1
WOTR_HUMAN=data/ladder2022/human.bin ./bin/wotr_p1 train runs/p1 240 2000 9 > runs/p1.log 2>&1
echo p1 >> runs/shadow14.progress
