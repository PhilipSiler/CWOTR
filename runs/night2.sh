#!/bin/sh
# After the 480-round session: three removal tests, 240 rounds each, all from the same start as session 1 (runs/c1),
# with the same settings as session 1 except for the one thing removed.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0
until ! pgrep -x wotr_night >/dev/null; do sleep 30; done
for spec in a1:WOTR_LEAN=0 a2:WOTR_TUNE=0 a3:both; do
  name=${spec%%:*}; what=${spec##*:}
  rm -rf runs/$name; cp -r runs/c1 runs/$name
  if [ "$what" = both ]; then WOTR_LEAN=0 WOTR_TUNE=0 ./bin/wotr_night train runs/$name 240 2000 9 > runs/$name.log 2>&1
  else env $what ./bin/wotr_night train runs/$name 240 2000 9 > runs/$name.log 2>&1; fi
done
