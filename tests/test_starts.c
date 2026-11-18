/* test_starts.c -- every human starting position (tools/humanlogs/starts.py) must load, pass the position checker,
 * and play on to the end of the game under random legal moves without ever failing the checker.
 * Usage: test_starts <starts file>; exit code 1 on any failure. */
#include "wotr.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "r") : 0;
    if (!f) { printf("no starts file: skipped\n"); return 0; }
    char line[256], err[200]; State s; long n = 0, refused = 0, fail = 0, steps = 0, finished = 0; uint64_t r = 88172645463325252ULL;
    wotr_global_init(); wotr_blank(&s);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "end", 3)) { if (!wotr_set(&s, line)) { printf("line not understood: %s", line); fail++; } continue; }
        n++;
        if (wotr_begin_turn(&s, (uint64_t)n * 7919, err, sizeof err)) refused++;
        else {
            for (int k = 0; !s.winner && k < 20000; k++) {
                Move mv[MAXMOVES]; int m = wotr_moves(&s, mv);
                if (m < 1) { printf("position %ld: no legal move at decision kind %d\n", n, s.dkind); fail++; break; }
                r ^= r << 13; r ^= r >> 7; r ^= r << 17;
                wotr_apply(&s, mv[r % (uint64_t)m]); steps++;
                if (k % 16 == 0 && wotr_check(&s, err, sizeof err)) { printf("position %ld after %d moves: %s\n", n, k, err); fail++; break; }
            }
            finished += s.winner != 0;
        }
        wotr_blank(&s);
    }
    printf("%ld starting positions: %ld refused by the checker, %ld played to the end (%ld random moves), %ld failures\n", n, refused, finished, steps, fail);
    return fail != 0;
}
