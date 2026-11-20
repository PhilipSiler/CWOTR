#!/bin/sh
# home0.sh: does guarding thinly held Shadow strongholds stop the raiding Free Peoples (runs/slow2/fp31) without slowing the attack?
# usage: sh runs/home0.sh [games]   Same seeds in every row; standard play settings with leans.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
N=${1:-400}
run() { echo "== $1"; fpp=$2; shift; shift; env WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 "$@" ./bin/wotr_x eval $fpp runs/keep/shadow_aw6 $N 9 2>&1 | grep -E "^FP=|Free Peoples captured|Shadow conquest wins on|average turns" | cut -c1-240; }
run "raider, as now" runs/slow2/fp31/latest WOTR_X=0
run "raider, thin guard" runs/slow2/fp31/latest WOTR_HOME_THIN=1
run "fp_aw7, as now" runs/keep/fp_aw7 WOTR_X=0
run "fp_aw7, thin guard" runs/keep/fp_aw7 WOTR_HOME_THIN=1
