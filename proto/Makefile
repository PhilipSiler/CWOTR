CC = clang
S = ../src
ENGINE = $(S)/core.c $(S)/data.c $(S)/movegen.c $(S)/flow.c $(S)/fellowship.c $(S)/battle.c $(S)/cards.c $(S)/util.c
AI = $(S)/features.c $(S)/nn.c $(S)/agent.c $(S)/policy.c
HDRS = $(S)/wotr.h $(S)/data.h $(S)/features.h $(S)/nn.h $(S)/agent.h $(S)/policy.h
VOCAB = ../../wotr-protocol/vocabulary/vocabulary-0.2.json
WARN = -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -std=c11

all: bridge bridge_check

# the player is built as bin/wotr builds it, so a trained player plays as it was measured
bridge: bridge.c $(ENGINE) $(AI) $(HDRS)
	$(CC) -O3 -march=native -ffast-math $(WARN) -I$(S) -o $@ bridge.c $(AI) $(ENGINE) -lm

# the same program with run-time checks, for the tests
bridge_check: bridge.c $(ENGINE) $(AI) $(HDRS)
	$(CC) -O1 -g $(WARN) -fsanitize=undefined,bounds -fno-sanitize-recover=all -I$(S) -o $@ bridge.c $(AI) $(ENGINE) -lm

test: bridge_check
	./bridge_check names $(VOCAB)
	./bridge_check mock prior 40 1

clean:
	rm -rf bridge bridge_check *.dSYM
