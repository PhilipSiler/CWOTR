#!/bin/sh
# raidbase.sh <shadow player> [games]: one Shadow read alone against the four Free Peoples of raid1.sh (same seeds as its readings)
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
for fpp in runs/slow2/fp31/latest runs/keep/fp_aw7 runs/keep/fp_aw4 runs/slow2/fp15/latest; do
  env WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 ./bin/wotr_x eval $fpp $1 ${2:-400} 9 2>&1 | grep -E "^FP=" | cut -c1-230
done
