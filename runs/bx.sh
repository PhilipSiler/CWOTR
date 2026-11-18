#!/bin/sh
# bx.sh: build the experimental program bin/wotr_x from the current source (bin/wotr stays as it is for running training)
cd /Users/philipsiler/Code/WOTR-RL/CWOTR
clang -O3 -flto -march=native -ffast-math -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -o bin/wotr_x src/train.c src/features.c src/nn.c src/agent.c src/policy.c src/core.c src/data.c src/movegen.c src/flow.c src/fellowship.c src/battle.c src/cards.c src/util.c -lm -lpthread 2>&1 | grep -E "error|warning"
