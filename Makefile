CC = clang
CFLAGS = -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11
ENGINE = src/core.c src/data.c src/movegen.c src/flow.c src/fellowship.c src/battle.c src/cards.c src/util.c
HDRS = src/wotr.h src/data.h

all: bin/fuzz bin/test_rules bin/wotr

test: bin/fuzz bin/test_rules bin/test_feat bin/test_starts
	./bin/test_rules
	./bin/fuzz 3000 1 | head -3
	./bin/test_feat | tail -1
	./bin/test_starts data/ladder2022/starts.txt

src/data.c src/data.h: tools/gen_data.py
	python3 tools/gen_data.py

bin/fuzz: tests/fuzz.c $(ENGINE) $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -fsanitize=undefined,bounds -fno-sanitize-recover=all -fstack-protector-all -Isrc -o $@ tests/fuzz.c $(ENGINE)

bin/test_rules: tests/test_rules.c $(ENGINE) $(HDRS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -fsanitize=undefined,bounds -fno-sanitize-recover=all -Isrc -o $@ tests/test_rules.c $(ENGINE) -lm

AI = src/features.c src/nn.c src/agent.c src/policy.c
bin/wotr: src/train.c $(AI) $(ENGINE) $(HDRS) src/features.h src/nn.h src/agent.h src/policy.h
	@mkdir -p bin
	$(CC) -O3 -flto -march=native -ffast-math -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -o $@ src/train.c $(AI) $(ENGINE) -lm -lpthread

bin/bench_nn: tests/bench_nn.c $(AI) $(ENGINE) $(HDRS) src/features.h src/nn.h
	@mkdir -p bin
	$(CC) -O3 -march=native -ffast-math -std=c11 -Isrc -o $@ tests/bench_nn.c $(AI) $(ENGINE) -lm

bin/inject: tests/inject.c src/features.c src/nn.c $(ENGINE) $(HDRS)
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -o $@ tests/inject.c src/features.c src/nn.c $(ENGINE) -lm

bin/bench: tests/bench.c $(ENGINE) $(HDRS)
	@mkdir -p bin
	$(CC) -O3 -std=c11 -Isrc -o $@ tests/bench.c $(ENGINE)

clean:
	rm -rf bin

bin/test_starts: tests/test_starts.c $(ENGINE) $(HDRS)
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -o $@ tests/test_starts.c $(ENGINE) -lm

bin/test_feat: tests/test_feat.c src/features.c $(ENGINE) $(HDRS)
	$(CC) -O2 -Wall -Wextra -Wno-unused-parameter -std=c11 -Isrc -o $@ tests/test_feat.c src/features.c $(ENGINE) -lm
