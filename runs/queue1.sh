#!/bin/sh
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
until [ -e runs/r10.done ]; do sleep 15; done
sleep 60     # let r10's own head-to-head finish
make > /dev/null 2>&1
runs/trial.sh q1
runs/trial.sh q2 WOTR_REW=2:0.06,11:0.008,12:0.008
runs/trial.sh q3 WOTR_TD=2
touch runs/queue1.done
