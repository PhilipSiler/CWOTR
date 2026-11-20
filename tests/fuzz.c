/* fuzz.c -- random-playout fuzzer: invariants at every decision, coverage statistics */
#include <stdio.h>
#include <stdlib.h>
#include "wotr.h"
const char *wotr_dkind_name(int k);

/* biased random policy: avoids wasting dice, pushes the Fellowship, fights and brings characters in */
static double weight(const State *s, Move m) {
    switch (m.type) {
    case M_SKIP: return 0.02;
    case M_PASS: return 0.3;
    case M_RING: return 0.05;
    case M_FSP_MOVE: return 40;
    case M_FSP_HIDE: return 30;
    case M_ATTACK: return 4;
    case M_PLAY: return 4;
    case M_MINION: case M_GANDALF_WHITE: case M_ARAGORN: return 30;
    case M_DISCARD_TABLE: return 5;
    case M_SEPARATE: return 0.2;
    case M_DONE: return 4;
    case M_CHOICE:
        if (s->dkind == D_FSP_DECLARE && m.a == 255) return 15;
        if (s->dkind == D_FSP_DECLARE && m.a == FSP_MORDOR) return 8;
        if (s->dkind == D_FSP_DECLARE && m.a >= FSP_GUIDE) return 1;
        if (s->dkind == D_FSP_DECLARE || s->dkind == D_REVEAL_DEST) {   /* head for Mordor */
            int d = DIST[m.a][R_MORANNON] < DIST[m.a][R_MINAS_MORGUL] ? DIST[m.a][R_MORANNON] : DIST[m.a][R_MINAS_MORGUL];
            int d0 = DIST[s->fsp_loc][R_MORANNON] < DIST[s->fsp_loc][R_MINAS_MORGUL] ? DIST[s->fsp_loc][R_MORANNON] : DIST[s->fsp_loc][R_MINAS_MORGUL];
            return d0 - d >= s->progress ? 200 : 0.01;
        }
        if (s->dkind == D_ENTER_MORDOR && m.a == 1) return 8;
        if (s->dkind == D_CONTINUE && m.a == 1) return 3;
        return 1;
    default: return 1;
    }
}
int main(int argc, char **argv) {
    int games = argc > 1 ? atoi(argv[1]) : 2000, verbose = argc > 3 ? atoi(argv[3]) : 0, biased = argc > 4 ? atoi(argv[4]) : 1;
    wotr_prune = argc > 5 ? atoi(argv[5]) : 1;     /* 0: fuzz the full move generator */
    uint64_t seed0 = argc > 2 ? strtoull(argv[2], 0, 10) : 1;
    static Move mv[MAXMOVES];
    long wins[3] = {0}, wt[5] = {0}, turns = 0, steps = 0, dk[ND] = {0}, played[NCARD] = {0}, cc[NCARD] = {0}, mt[NMOVETYPE] = {0};
    long maxmoves = 0, maxsp = 0, summoves = 0;
    char err[256], buf[160];
    for (int g = 0; g < games; g++) {
        State s; uint64_t r = (seed0 + (uint64_t)g) * 0x9E3779B97F4A7C15ULL;
        wotr_init(&s, seed0 + (uint64_t)g);
        long n = 0;
        while (!s.winner) {
            int k = wotr_moves(&s, mv);
            if (wotr_check(&s, err, sizeof err) || k <= 0 || k > MAXMOVES) {
                printf("FAIL game seed %llu step %ld: %s (moves %d, dkind %s)\n", (unsigned long long)(seed0 + (uint64_t)g), n, k <= 0 ? "no moves" : err, k, wotr_dkind_name(s.dkind));
                wotr_print(&s); return 1;
            }
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            Move m = mv[r % (uint64_t)k];
            if (biased) {
                double tot = 0, x;
                for (int i = 0; i < k; i++) tot += weight(&s, mv[i]);
                x = (double)(r >> 11) / 9007199254740992.0 * tot;
                for (int i = 0; i < k; i++) { x -= weight(&s, mv[i]); if (x <= 0 || i == k - 1) { m = mv[i]; break; } }
            }
            if (verbose) { wotr_move_str(&s, m, buf, sizeof buf); printf("%s %s: %s\n", s.dplayer ? "SH" : "FP", wotr_dkind_name(s.dkind), buf); }
            dk[s.dkind]++; mt[m.type]++; summoves += k;
            if (k > maxmoves) maxmoves = k;
            if (s.sp > maxsp) maxsp = s.sp;
            if (m.type == M_PLAY) played[m.a]++;
            if (s.dkind == D_BATTLE_CARD && m.a < NCARD) cc[m.a]++;
            wotr_apply(&s, m);
            if (++n > 200000) { printf("FAIL game seed %llu: runaway\n", (unsigned long long)(seed0 + (uint64_t)g)); wotr_print(&s); return 1; }
        }
        if (verbose) wotr_print(&s);
        wins[s.winner]++; wt[s.win_type]++; turns += s.turn; steps += n;
    }
    printf("%d games ok. FP wins %ld, SH wins %ld | ring %ld corruption %ld FPmil %ld SHmil %ld timeout %ld\n", games, wins[1], wins[2], wt[WIN_RING], wt[WIN_CORRUPTION], wt[WIN_FP_MIL], wt[WIN_SH_MIL], wt[WIN_NONE]);
    printf("avg turns %.1f, avg decisions %.0f, avg branching %.1f, max branching %ld, max stack %ld\n", (double)turns / games, (double)steps / games, (double)summoves / (double)steps, maxmoves, maxsp);
    printf("decisions:"); for (int k = 1; k < ND; k++) printf(" %s=%ld", wotr_dkind_name(k), dk[k]); printf("\n");
    int unplayed = 0, uncc = 0;
    for (int c = 0; c < NCARD; c++) { if (!played[c]) { printf("  never played as event: %s\n", CARD[c].name); unplayed++; } if (!cc[c]) uncc++; }
    printf("cards never played as event: %d; never as combat card: %d; move types unused:", unplayed, uncc);
    for (int t = 0; t < NMOVETYPE; t++) if (!mt[t]) printf(" %d", t);
    printf("\n");
    return 0;
}
