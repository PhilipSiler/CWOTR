#!/bin/sh
# Formula against formula (no training): the yardstick for the hand-written starting heuristics. Prints the lines that
# matter next to the human figures (1,508 ladder games: Shadow conquest 41%, Ring destroyed 37%, corruption 12%,
# Free Peoples conquest 5%, 9.7 turns, Mordor entered around turn 6-7).
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
./bin/wotr eval prior prior ${1:-1800} 9 2>&1 | grep -E "FP=prior|average turns|Shadow military victories|Fellowship steps left|Free Peoples captured|Shadow tempo|dice per turn" | sed -E 's/; Free Peoples strongholds besieged.*//; s/; units in Mordor/\n     units in Mordor/' | cut -c1-${2:-330}
