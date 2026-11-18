#!/bin/sh
# After c15. Like c12-c15 these start from untrained networks (a new, empty folder), 240 rounds each, c15's settings:
#  n1: plus the guides as inputs of the network
#  n2: n1 plus the human positions as extra training examples
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1 WOTR_FALL=1 WOTR_VPW=1 WOTR_INPUTS=1
until [ -e runs/shadow9.progress ]; do sleep 30; done
rm -rf runs/n1; mkdir runs/n1
./bin/wotr_n1 train runs/n1 240 2000 9 > runs/n1.log 2>&1
echo n1 >> runs/shadow11.progress
rm -rf runs/n2; mkdir runs/n2
WOTR_HUMAN=data/ladder2022/human.bin ./bin/wotr_n1 train runs/n2 240 2000 9 > runs/n2.log 2>&1
echo n2 >> runs/shadow11.progress
