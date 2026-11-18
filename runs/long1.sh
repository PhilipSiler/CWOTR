#!/bin/sh
# After the third removal test: 1,000 more rounds of the baseline settings, continuing from the end of the 480-round session (runs/s3).
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0
until ! pgrep -x wotr_night >/dev/null; do sleep 30; done
rm -rf runs/L1; cp -r runs/s3 runs/L1
./bin/wotr_night train runs/L1 1000 2000 9 > runs/L1.log 2>&1
