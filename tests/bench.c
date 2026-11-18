#include <stdio.h>
#include <time.h>
#include "wotr.h"
int main(void) {
    static Move mv[MAXMOVES]; long steps = 0, gens = 0; clock_t t0 = clock();
    for (int g = 0; g < 2000; g++) {
        State s; wotr_init(&s, (uint64_t)g + 1); uint64_t r = (uint64_t)g * 77 + 5;
        while (!s.winner) { int k = wotr_moves(&s, mv); gens += k; r = r * 6364136223846793005ULL + 1442695040888963407ULL; wotr_apply(&s, mv[(r >> 33) % (uint64_t)k]); steps++; }
    }
    double dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%ld decisions, %.2fs: %.0f decisions/s, %.1f moves generated per decision, sizeof(State)=%zu\n", steps, dt, steps / dt, (double)gens / steps, sizeof(State));
    /* apply+copy cost */
    State s; wotr_init(&s, 3); while (s.dkind != D_ACTION) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    int k = wotr_moves(&s, mv); t0 = clock(); long n = 0;
    for (int it = 0; it < 20000; it++) for (int i = 0; i < k; i++) { State t = s; wotr_apply(&t, mv[i]); n++; }
    dt = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("copy+apply: %.2f M/s (%d moves at root)\n", n / dt / 1e6, k);
    return 0;
}
