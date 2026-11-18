#!/bin/sh
# res.sh <shadow run name> <fp player>...: one line per opponent for a finished Shadow run (share 0.25, look-ahead, 1,800 games):
# Shadow wins, conquest share and turn, corruption share, strongholds lost, Shadow points after turns 4/6/8
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
n=$1; shift
for fp in "$@"; do
  WOTR_W=12:0,18:0 WOTR_NUDGE=0.3 WOTR_DEEP_SH=3,4,1,6 ./bin/wotr_t eval $fp runs/$n/s25 1800 9 2>&1 | awk -v fp=$fp '/^FP=/{for(i=1;i<=NF;i++){if($i=="wins")w=$(i+1); if($i=="corruption")c=$(i+1); if($i=="military"&&$(i-1)=="SH")d=$(i+1); if($i=="military"&&$(i-1)=="FP")f=$(i+1)}} /average turns to conclusion/{t=$NF} /Shadow tempo/{split($0,a,"victory points "); split(a[2],b,";"); vp=b[1]} END{print fp ": wins " w " conquest " d " t" t " corruption " c " FP-mil " f " | points t2/4/6/8/10 ours (humans): " vp}'
done
