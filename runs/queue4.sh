#!/bin/sh
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
until [ -e runs/b2.done ]; do sleep 15; done
make > /dev/null 2>&1
runs/trial.sh e1 WOTR_TEMP_SH=0.15
runs/trial.sh e2 WOTR_TEMP_SH=0.3
touch runs/queue4.done
