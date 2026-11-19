#!/bin/sh
# back.sh <name> <first stage> <last stage> [extra env...]: the backward sweep (Philip, 2026-11-18). Stage k trains BOTH
# sides at once, with most games starting from human boards k turns before the end of their game (dice rerolled, played
# out by our players), mixed with boards from the stages already done so that those are not forgotten; then stage k+1
# steps one turn further back, until the opening is reached. The AlphaWOTR lessons stay in every round's fit.
# Honest results for both sides: NO WOTR_CORRWIN here (it rewrites the result both value nets learn from - with it the Free
# Peoples learned that being corrupted is nearly a win; run bk1, 2026-11-18).
# The folder runs/<name> is created on the first call and continued on later calls. After each stage: a snapshot
# runs/<name>/stage<k>.* and 600 test games for each side against today's fixed players (appended to runs/<name>/sweep.out).
#   ROUNDS (40)  rounds per stage      SHARE (0.7)  share of training games that start from a board
#   TEACH (2)    stages up to this one also let rollouts make and teach a tenth of Shadow's decisions
#   FROM (shadow_aw6 + fp_aw7)  the players to start from: FROM_SH, FROM_FP
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; k0=$2; k1=$3; shift 3; d=runs/$n; B=data/ladder2022/back
SHF=${FROM_SH:-runs/keep/shadow_aw6}; FPF=${FROM_FP:-runs/keep/fp_aw7}
if [ ! -d $d ]; then
  mkdir $d || exit 1
  cp $FPF.fp $d/latest.fp; cp $FPF.pfp $d/latest.pfp; cp $SHF.sh $d/latest.sh; cp $SHF.psh $d/latest.psh; echo 4200 > $d/rounds
fi
k=$k0
while [ $k -le $k1 ]; do
  # this stage's boards, plus as many again drawn from the stages already done
  python3 - $B $k $d/boards.txt <<'E'
import sys, random
B, k, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
def blocks(p):
    r, b = [], []
    for l in open(p):
        b.append(l)
        if l.startswith('end'): r.append(''.join(b)); b = []
    return r
cur = blocks('%s/b%d.txt' % (B, k)); old = []
for j in range(k): old += blocks('%s/b%d.txt' % (B, j))
random.seed(k); random.shuffle(old)
open(out, 'w').write(''.join(cur + old[:len(cur)]))
print('stage %d: %d boards of this stage + %d of earlier stages' % (k, len(cur), min(len(old), len(cur))))
E
  teach=""; [ $k -le ${TEACH:-2} ] && teach="WOTR_ROLL_LEAN=1 WOTR_ROLL_TEACH=100,8,0.1,0.5"
  env WOTR_TD=0 WOTR_EXACT=1,0 WOTR_EXEMPT=0,1 WOTR_FULL=0.04 WOTR_TEMP=0.06 WOTR_FLOOR=0.3 WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 \
      WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 WOTR_STARTS=$d/boards.txt WOTR_START_SHARE=${SHARE:-0.7} \
      WOTR_AWLES_SH=0.5,$(cat runs/keep/aw_shadow_files.txt) WOTR_AWLES_FP=0.5,$(cat runs/keep/aw_fp_files.txt) $teach "$@" \
      ./bin/wotr_t train $d ${ROUNDS:-40} 2000 9 960 > $d/train$k.log 2>&1
  for x in fp sh pfp psh; do cp $d/latest.$x $d/stage$k.$x; done
  ./bin/wotr_t share $d/latest $d/s25 0.25
  {
    echo "== after stage $k ($(grep -c '^it ' $d/train$k.log) rounds, $(grep '^it ' $d/train$k.log | tail -1 | cut -d' ' -f3-4) a round)"
    echo " Shadow of the sweep against fp_aw7:"
    WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 ./bin/wotr_t eval runs/keep/fp_aw7 $d/s25 600 9 2>&1 | grep -E "^FP=|average turns to conclusion|Gondor falls" | cut -c1-290
    echo " Free Peoples of the sweep against shadow_aw6:"
    WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 ./bin/wotr_t eval $d/latest runs/keep/shadow_aw6 600 9 2>&1 | grep -E "^FP=|average turns to conclusion|Gondor falls" | cut -c1-290
    echo " the same two pairings started from this stage's boards (the base pair from these boards: see the log):"
    WOTR_STARTS=$d/boards.txt WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 ./bin/wotr_t eval runs/keep/fp_aw7 $d/s25 600 9 2>&1 | grep -E "^FP=" | cut -c1-200
    WOTR_STARTS=$d/boards.txt WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 WOTR_HUMAN_SH=0.1 WOTR_HUMAN_FP=0.1 ./bin/wotr_t eval $d/latest runs/keep/shadow_aw6 600 9 2>&1 | grep -E "^FP=" | cut -c1-200
  } >> $d/sweep.out 2>&1
  k=$((k + 1))
done
touch $d/done
