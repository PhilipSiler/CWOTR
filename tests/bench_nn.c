#include <stdio.h>
#include <time.h>
#include "agent.h"
int main(void) {
    feat_init();
    static Net n; nn_init(&n, 1);
    for (int i = 0; i < H2; i++) for (int j = 0; j < NOUT; j++) n.p.W3[i][j] = 0.05f * (float)((i * 7 + j * 13) % 11 - 5);   /* non-zero head so the comparison is meaningful */
    static Move mv[MAXMOVES]; static State pos[512]; int np = 0;
    State s; wotr_init(&s, 5); uint64_t r = 99;
    while (!s.winner && np < 512) { int k = wotr_moves(&s, mv); r = r * 6364136223846793005ULL + 1; if (s.dkind == D_ACTION) pos[np++] = s; wotr_apply(&s, mv[(r >> 33) % (uint64_t)k]); }
    Feat f; float o[NOUT]; double chk = 0; long nnz = 0;
    clock_t t0 = clock();
    for (int it = 0; it < 200; it++) for (int i = 0; i < np; i++) { feat_encode(&pos[i], &f); nnz += f.n; for (int k = 0; k < f.n; k++) chk += f.val[k] * (f.idx[k] % 7 + 1); for (int k = 0; k < NPRIOR; k++) chk += f.prior[k]; }
    double te = (double)(clock() - t0) / CLOCKS_PER_SEC / (200.0 * np);
    t0 = clock();
    for (int i = 0; i < np; i++) { feat_encode(&pos[i], &f); for (int it = 0; it < 200; it++) { nn_forward(&n, &f, o); chk += o[0] * 1e-3; } }
    double tn = (double)(clock() - t0) / CLOCKS_PER_SEC / (200.0 * np);
    /* incremental path: evaluate every child of each position against the plain forward pass */
    double tdl = 0, tfl = 0, maxl = 0, maxp = 0; long ndl = 0;
    static float acc[H1], acc2[H1], accf[H1]; double maxd = 0, td = 0, maxa = 0; long nch = 0, ndiff = 0, unsorted = 0;
    for (int i = 0; i < np; i++) {
        Feat fr; feat_encode(&pos[i], &fr); nn_root(&n, &fr, acc);
        for (int k = 1; k < fr.n; k++) unsorted += fr.idx[k] <= fr.idx[k - 1];
        if (i > 0) {   /* carrying the sums from the previous position must match a rebuild */
            Feat fp_; feat_encode(&pos[i - 1], &fp_); nn_root(&n, &fp_, acc2); nn_acc_delta(&n, &fp_, acc2, &fr, accf);
            for (int k = 0; k < H1; k++) { double e = accf[k] - acc[k]; if (e < 0) e = -e; if (e > maxa) maxa = e; }
        }
        int k = wotr_moves(&pos[i], mv);
        for (int c = 0; c < k; c++) {
            State u = pos[i]; wotr_apply(&u, mv[c]); if (u.winner) continue;
            feat_encode(&u, &f); float o2[NOUT];
            nn_forward(&n, &f, o);
            {   /* change-list path: encode the child relative to the cached root */
                static FeatCache rc; static FeatDelta dl; float o3[NOUT];
                if (c == 0) feat_cache(&pos[i], &rc);
                clock_t a2 = clock(); for (int it = 0; it < 20; it++) feat_delta(&pos[i], &rc, &u, &dl); tdl += (double)(clock() - a2);
                { clock_t a3 = clock(); for (int it = 0; it < 20; it++) nn_forward_list(&n, acc, &dl, o3); tfl += (double)(clock() - a3); } ndl += dl.n;
                for (int q = 0; q < NOUT; q++) { double e = o[q] - o3[q]; if (e < 0) e = -e; if (e > maxl) maxl = e; }
                for (int q = 0; q < NPRIOR; q++) { double e = f.prior[q] - dl.prior[q]; if (e < 0) e = -e; if (e > maxp) maxp = e; }
            }
            clock_t a = clock(); for (int it = 0; it < 20; it++) nn_forward_delta(&n, &fr, acc, &f, o2); td += (double)(clock() - a);
            for (int q = 0; q < NOUT; q++) { double e = o[q] - o2[q]; if (e < 0) e = -e; if (e > maxd) maxd = e; }
            nch++;
        }
    }
    (void)ndiff;
    printf("change-list forward: %.3f us per position\n", tfl / CLOCKS_PER_SEC / (20.0 * nch) * 1e6);
    printf("change-list encode: %.3f us, avg %.1f changed features, max abs diff of outputs %.2e, of prior terms %.2e\n", tdl / CLOCKS_PER_SEC / (20.0 * nch) * 1e6, (double)ndl / nch, maxl, maxp);
    printf("features out of index order: %ld; carried first-layer sums max abs diff %.2e\n", unsorted, maxa);
    printf("incremental: %ld children, max abs diff vs full forward %.2e, %.2f us per eval\n", nch, maxd, td / CLOCKS_PER_SEC / (20.0 * nch) * 1e6);
    printf("positions %d, avg nnz %.0f, encode %.2f us, forward %.2f us, checksum %.6f\n", np, (double)nnz / (200.0 * np), te * 1e6, tn * 1e6, chk);
    return 0;
}
