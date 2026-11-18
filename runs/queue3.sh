#!/bin/sh
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
until [ -e runs/b1.done ]; do sleep 15; done
sleep 150    # b1's own measuring games
make > /dev/null 2>&1
runs/trial.sh b2 WOTR_BALANCE=58
