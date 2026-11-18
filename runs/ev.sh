#!/bin/sh
# ev.sh <FP player> <Shadow player> [games]: one short line: FP win%, then Ring / corruption / Shadow conquest / FP conquest %, turns
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
${WOTR_BIN:-./bin/wotr} eval "$1" "$2" "${3:-1800}" ${EVT:-9} 2>&1 | awk -v a="$1" -v b="$2" '/^FP=/{for(i=1;i<=NF;i++){if($i=="ring")r=$(i+1); if($i=="military"&&$(i-1)=="FP")fm=$(i+1); if($i=="corruption")c=$(i+1); if($i=="military"&&$(i-1)=="SH")sm=$(i+1); if($i=="turns")t=$(i+1)}} END{printf "FP %s vs SH %s: FP wins %.1f%% | Ring %s corruption %s Shadow-conquest %s FP-conquest %s | turns %s\n", a, b, r+fm, r, c, sm, fm, t}'
