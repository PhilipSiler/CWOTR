#!/bin/sh
# fpres.sh <fp player>...: each Free Peoples player against the two fast Shadows (Shadow with look-ahead, 1,800 games):
# result, Ring victories and Shadow conquests turn by turn, and the Fellowship's pace to and inside Mordor
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
for fp in "$@"; do for sh in ${SHADOWS:-runs/keep/shadow_sd33 runs/keep/shadow_sd30}; do
  echo "== $fp vs $sh"
  WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 ./bin/wotr_t eval $fp $sh 1800 9 2>&1 | grep -E "^FP=|average turns to conclusion|Ring destroyed by turn|Shadow conquest by turn|at the gate of Mordor|entered Mordor in|Gondor falls" | cut -c1-330
done; done
