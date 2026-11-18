#!/bin/sh
# Tests of the human-derived Shadow guidance, each 240 rounds from the same saved state (runs/c1) with the same settings as
# the control (runs/t0). name:lane:deploy:tempo:plan:reserve:human
#  m2: lane guards + deployment terms (the package)      g1: lane guards only      m1: deployment terms only
#  m3: the package + behind schedule, conquest plan, sieges count reinforcements     h1: control + human positions as examples
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0 WOTR_REINJECT=20 WOTR_NUDGE=0.5
until ! pgrep -x wotr_plan >/dev/null; do sleep 30; done
for spec in m2:1:1:0:0:0:0 g1:1:0:0:0:0:0 m1:0:1:0:0:0:0 m3:1:1:1:1:1:0 h1:0:0:0:0:0:1; do
  IFS=: read name lane deploy tempo plan reserve human <<EOF
$spec
EOF
  rm -rf runs/$name; cp -r runs/c1 runs/$name
  if [ "$human" = 1 ]; then export WOTR_HUMAN=data/ladder2022/human.bin; else unset WOTR_HUMAN; fi
  WOTR_LANE=$lane WOTR_DEPLOY=$deploy WOTR_TEMPO=$tempo WOTR_PLAN=$plan WOTR_RESERVE=$reserve ./bin/wotr_lane train runs/$name 240 2000 9 > runs/$name.log 2>&1
  echo $name >> runs/shadow4.progress
done
echo done > runs/shadow4.done
