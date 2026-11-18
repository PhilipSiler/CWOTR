#!/bin/sh
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
OLD=2:0.025,3:0.05,4:0,5:0,6:0,7:0,8:0,9:0,10:0,11:0,12:0,13:0,14:0
runs/trial.sh c1 WOTR_REW=$OLD
runs/trial.sh c2 WOTR_REW=$OLD
runs/trial.sh q1b
touch runs/queue2.done
