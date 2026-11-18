/* test_feat.c -- the incremental encoder must agree with the full one.
 * Random games; at sampled positions every legal move (40 at most) is applied to a copy, and the position's inputs
 * are computed twice: from scratch (feat_encode) and as changes against the cached root (feat_delta). Inputs and
 * strategy terms must match. This covers every input block, including the guides block. */
#include "wotr.h"
#include "features.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    feat_init();
    static FeatCache rc; static FeatDelta d; static Feat full; static float a[NFEAT], b[NFEAT];
    long checked = 0, bad_in = 0, bad_prior = 0; uint64_t r = 88172645463325252ULL; double worst = 0;
    for (int game = 0; game < 60; game++) {
        State s; wotr_init(&s, (uint64_t)(game + 1) * 7919);
        for (int ply = 0; ply < 4000 && !s.winner; ply++) {
            Move mv[MAXMOVES]; int n = wotr_moves(&s, mv);
            if (n > 1 && ply % 7 == 0) {
                feat_cache(&s, &rc);
                for (int i = 0; i < n && i < 40; i++) {
                    State u = s; wotr_apply(&u, mv[i]);
                    if (u.winner) continue;
                    feat_delta(&s, &rc, &u, &d); feat_encode(&u, &full);
                    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
                    for (int k = 0; k < rc.f.n; k++) a[rc.f.idx[k]] += rc.f.val[k];
                    for (int k = 0; k < d.n; k++) a[d.idx[k]] += d.dv[k];
                    for (int k = 0; k < full.n; k++) b[full.idx[k]] += full.val[k];
                    int wrong = 0;
                    for (int k = 0; k < NFEAT; k++) { double e = fabs((double)a[k] - b[k]); if (e > 1e-4) { wrong = 1; if (e > worst) worst = e; if (bad_in < 5) printf("input %d: incremental %.5f, full %.5f (game %d ply %d move %d)\n", k, a[k], b[k], game, ply, i); } }
                    bad_in += wrong;
                    wrong = 0;
                    for (int k = 0; k < NPRIOR; k++) if (fabs((double)d.prior[k] - full.prior[k]) > 1e-4) { wrong = 1; if (bad_prior < 5) printf("strategy term %d: incremental %.5f, full %.5f (game %d ply %d move %d)\n", k, d.prior[k], full.prior[k], game, ply, i); }
                    bad_prior += wrong; checked++;
                }
            }
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            wotr_apply(&s, mv[r % (uint64_t)n]);
        }
    }
    printf("%ld positions checked: %ld with wrong inputs (largest error %.5f), %ld with wrong strategy terms\n", checked, bad_in, worst, bad_prior);
    return bad_in || bad_prior;
}
