#!/bin/sh
# night.sh <run dir name> <fade rounds> <stretches>: 120-round stretches one after another; after each, three matchups
# (as FP against the untrained formula, as Shadow against it, against itself) appended to runs/<name>.results
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
for i in $(seq 1 $3); do
  ${WOTR_BIN:-./bin/wotr} train runs/$1 120 2000 9 $2 >> runs/$1.log 2>&1
  r=$(cat runs/$1/rounds)
  { echo "== $1 @$r $(grep 'still at' runs/$1.log | tail -1 | sed 's/^ *//')"
    runs/ev.sh runs/$1/latest prior; runs/ev.sh prior runs/$1/latest; runs/ev.sh runs/$1/latest runs/$1/latest; } >> runs/$1.results
done
touch runs/$1.done
