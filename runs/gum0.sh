#!/bin/sh
# gum0.sh: does Gumbel search at play time help the Shadow? Same seeds in every row; the Free Peoples play alone (one step ahead).
# usage: sh runs/gum0.sh <free peoples player> <shadow player> [games]
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
FPP=$1; SHP=$2; N=${3:-360}
run() { echo "== $1"; shift; s=$(date +%s); env WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 "$@" ./bin/wotr_x eval $FPP $SHP $N 9 2>&1 | grep -E "^FP=" | cut -c1-230; echo "   $(( $(date +%s) - s )) s"; }
run "alone (one step ahead)" WOTR_X=0
run "look-ahead 3 deep, 4 wide" WOTR_DEEP_SH=3,4,1,6
run "Gumbel 8 evaluations on 4 moves, look-ahead evaluations" WOTR_DEEP_SH=3,4,1,6 WOTR_GUM_SH=8,4,1,0,1
run "Gumbel 16 on 8, look-ahead evaluations" WOTR_DEEP_SH=3,4,1,6 WOTR_GUM_SH=16,8,1,0,1
run "Gumbel 16 on 8, one-step evaluations" WOTR_GUM_SH=16,8,1,0,1
