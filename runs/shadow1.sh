#!/bin/sh
# After the 1,000-round session: three 240-round tests from the same saved state as session 1 (runs/c1), same settings,
# with the new program: t0 both new Shadow terms off (control), t1 "behind schedule" on, t2 "behind schedule" and "conquest plan" on.
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
export WOTR_HOLD=0 WOTR_FIRST=0 WOTR_HUNT=0
until ! pgrep -x wotr_night >/dev/null; do sleep 30; done
for spec in t0:0:0 t1:1:0 t2:1:1; do
  name=${spec%%:*}; rest=${spec#*:}; tempo=${rest%%:*}; plan=${rest##*:}
  rm -rf runs/$name; cp -r runs/c1 runs/$name
  WOTR_TEMPO=$tempo WOTR_PLAN=$plan ./bin/wotr_plan train runs/$name 240 2000 9 > runs/$name.log 2>&1
done
echo done > runs/shadow1.done
