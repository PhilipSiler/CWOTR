#!/bin/sh
# s1: the stripped-down trainer -- one version per side playing itself and its own earlier versions, no contest between
#     versions, no round-by-round tuning of the formula weights, no Ring-or-conquest lean. Everything else as p1
#     (Shadow guides at full strength, guides as inputs, human examples). From untrained networks, 120 rounds.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5 WOTR_GARRISON=1 WOTR_FALL=1 WOTR_VPW=1 WOTR_INPUTS=1
export WOTR_PLAIN=1 WOTR_TUNE=0 WOTR_LEAN=0
until [ -e runs/shadow14.progress ]; do sleep 30; done
rm -rf runs/s1x; mkdir runs/s1x
WOTR_HUMAN=data/ladder2022/human.bin ./bin/wotr_s1 train runs/s1x 120 2000 9 > runs/s1x.log 2>&1
echo s1x >> runs/shadow15.progress
