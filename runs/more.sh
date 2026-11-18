#!/bin/sh
# more.sh <trial>: one more 120-round stretch of a trial at the fast settings, then its last three checkpoints against the reference
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
env WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_EXACT=1 WOTR_EXEMPT=0 WOTR_FULL=0.04 ./bin/wotr train runs/$1 120 2000 9 960 >> runs/$1.log 2>&1
r=$(cat runs/$1/rounds)
echo "$1 @$r | $(LAST=$r runs/avg.sh $1)" >> runs/training-log.txt
touch runs/$1.more.done
