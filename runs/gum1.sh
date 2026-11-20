#!/bin/sh
# gum1.sh: Gumbel rows with the self-play suggester as the prior, and with a larger budget. Same seeds as gum0.sh.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
FPP=$1; SHP=$2; N=${3:-360}
run() { echo "== $1"; shift; s=$(date +%s); env WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 "$@" ./bin/wotr_x eval $FPP $SHP $N 9 2>&1 | grep -E "^FP=" | cut -c1-230; echo "   $(( $(date +%s) - s )) s"; }
run "Gumbel 16 on 8, look-ahead evaluations, self-play suggester" WOTR_DEEP_SH=3,4,1,6 WOTR_GUM_SH=16,8,1,0,0
run "Gumbel 16 on 8, one-step evaluations, self-play suggester" WOTR_GUM_SH=16,8,1,0,0
run "Gumbel 32 on 8, look-ahead evaluations, taught suggester" WOTR_DEEP_SH=3,4,1,6 WOTR_GUM_SH=32,8,1,0,1
