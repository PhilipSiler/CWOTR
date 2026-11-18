/* nn.c -- MLP forward/backward (sparse first layer), Adam */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include "nn.h"

/* weights on the FP-win logit: 0 ring-quest margin, 1 Shadow VP, 2 pressure on FP settlements, 3 road left to Mount Doom,
 * 4 FP extra dice, 5 Shadow extra dice (minions), 6 FP VP, 7 army balance, 8 pressure on Shadow settlements, 9 Mordor progress,
 * 10 Shadow mass near targets, 11 bias, 12 Shadow garrison gaps, 13 Shadow siege forecast, 14 Shadow nations at war,
 * 15 idle Shadow units, 16 crowning potential (Strider -> Aragorn) */
static const float PRIOR0[NPRIOR] = {1.5f, -8.0f, -0.3f, -6.0f, 2.0f, -4.5f, 4.0f, -3.0f, 0.5f, 0.5f, -2.5f, 5.0f, 1.5f, -1.5f, -3.0f, 0.6f, 1.5f, 2.0f, -5.0f, 3.0f, 3.0f, 4.0f, -6.0f, 1.5f};
/* WOTR_W="19:4.5" (or a list, "12:0,15:0") tries other weights for terms without rebuilding. */
float nn_formula(const float *prior) {
    static float w[NPRIOR]; static int ready;
    if (!ready) { memcpy(w, PRIOR0, sizeof w); const char *v = getenv("WOTR_W"); int k; float x; while (v && sscanf(v, "%d:%f", &k, &x) == 2) { if (k >= 0 && k < NPRIOR) w[k] = x; v = strchr(v, ','); if (v) v++; } ready = 1; }
    float z = 0; for (int k = 0; k < NPRIOR; k++) z += w[k] * prior[k]; return z;
}
/* A "style" for one training game (train.c, WOTR_STYLE): while nn_style_p points at a network, the formula's terms are
   multiplied by nn_style_w for that network's decisions, so one whole game is played with, say, massing valued three
   times as much. It explores whole plans where move-by-move randomness explores single moves. Training targets are
   not touched (the network learns from its own estimate, see blend). */
_Thread_local const Params *nn_style_p; _Thread_local float nn_style_w[NPRIOR];
static inline float formula_for(const Params *p, const float *prior) {
    if (p != nn_style_p) return nn_formula(prior);
    float st[NPRIOR]; for (int k = 0; k < NPRIOR; k++) st[k] = prior[k] * nn_style_w[k];
    return nn_formula(st);
}

static float urand(uint64_t *s) { *s = *s * 6364136223846793005ULL + 1442695040888963407ULL; return (float)((*s >> 40) / 16777216.0) * 2.0f - 1.0f; }

void nn_init(Net *n, uint64_t seed) {
    uint64_t s = seed * 2654435761ULL + 99;
    memset(n, 0, sizeof *n);
    float a1 = 0.06f, a2 = sqrtf(6.0f / H1);
    for (int i = 0; i < NFEAT; i++) for (int j = 0; j < H1; j++) n->p.W1[i][j] = a1 * urand(&s);
    for (int i = 0; i < H1; i++) for (int j = 0; j < H2; j++) n->p.W2[i][j] = a2 * urand(&s);
    /* output layer starts at zero: the initial value function is exactly the prior */
    n->p.alpha = 1.0f;
}

static inline float sigm(float x) { return 1.0f / (1.0f + expf(-x)); }

static void fwd(const Params *p, const Feat *f, float *h1, float *h2, float *o) {
    memcpy(h1, p->b1, sizeof(float) * H1);
    for (int k = 0; k < f->n; k++) {
        const float *w = p->W1[f->idx[k]]; float v = f->val[k];
        for (int j = 0; j < H1; j++) h1[j] += v * w[j];
    }
    for (int j = 0; j < H1; j++) if (h1[j] < 0) h1[j] = 0;
    memcpy(h2, p->b2, sizeof(float) * H2);
    for (int i = 0; i < H1; i++) {
        float v = h1[i]; if (v == 0) continue;
        const float *w = p->W2[i];
        for (int j = 0; j < H2; j++) h2[j] += v * w[j];
    }
    for (int j = 0; j < H2; j++) if (h2[j] < 0) h2[j] = 0;
    memcpy(o, p->b3, sizeof(float) * NOUT);
    for (int i = 0; i < H2; i++) { float v = h2[i]; if (v == 0) continue; for (int j = 0; j < NOUT; j++) o[j] += v * p->W3[i][j]; }
}

/* The win logit used for decisions is a blend: alpha of the hand-written formula, the rest the network's own
   estimate. The network is trained on its own estimate alone (fwd), so it learns to stand by itself and gets its
   say only as the formula fades. (Adding the network on top at full strength let it swamp the formula's small
   preferences within 20 rounds: Shadow conquest wins went from 44% to 4%.) */
static float blend(const Params *p, const float *prior, float raw) { return p->alpha * formula_for(p, prior) + (1 - p->alpha) * raw; }
void nn_forward(const Net *n, const Feat *f, float *out) {
    float h1[H1], h2[H2];
    fwd(&n->p, f, h1, h2, out);
    out[0] = blend(&n->p, f->prior, out[0]);
}

static void tail(const Params *p, const float *prior, float *h1, float *o, int nout) {      /* nout: how many outputs to compute (the search needs only the first) */
    float h2[H2];
    memcpy(h2, p->b2, sizeof h2);
    /* About half the hidden units are inactive. Testing each one inside the multiply loop mispredicts
       constantly, so first compact the active indices without branching, then touch only those rows. */
    uint16_t act[H1]; int na = 0;
    for (int i = 0; i < H1; i++) { act[na] = (uint16_t)i; na += h1[i] > 0; }
    for (int k = 0; k < na; k++) {
        float v = h1[act[k]];
        const float *w = p->W2[act[k]];
        for (int j = 0; j < H2; j++) h2[j] += v * w[j];
    }
    memcpy(o, p->b3, sizeof(float) * (size_t)nout);
    for (int i = 0; i < H2; i++) { float v = h2[i] > 0 ? h2[i] : 0; for (int j = 0; j < nout; j++) o[j] += v * p->W3[i][j]; }
    o[0] = p->alpha * formula_for(p, prior) + (1 - p->alpha) * o[0];
    /* outputs are left raw: O_WIN is used as a logit, the probability heads are read with nn_prob */
}
void nn_root(const Net *n, const Feat *root, float *acc) {
    memcpy(acc, n->p.b1, sizeof(float) * H1);
    for (int k = 0; k < root->n; k++) {
        const float *w = n->p.W1[root->idx[k]]; float v = root->val[k];
        for (int j = 0; j < H1; j++) acc[j] += v * w[j];
    }
}
static inline void axpy(float *h, const float *w, float v) { for (int j = 0; j < H1; j++) h[j] += v * w[j]; }
/* h += W1 * (to - from), by merging the two index-sorted feature lists */
static void add_diff(const Params *p, const Feat *from, const Feat *to, float *h) {
    int i = 0, j = 0, ni = from->n, nj = to->n;
    while (i < ni && j < nj) {
        int a = from->idx[i], b = to->idx[j];
        if (a == b) { float d = to->val[j] - from->val[i]; if (d != 0) axpy(h, p->W1[a], d); i++; j++; }
        else if (a < b) { axpy(h, p->W1[a], -from->val[i]); i++; }
        else { axpy(h, p->W1[b], to->val[j]); j++; }
    }
    for (; i < ni; i++) axpy(h, p->W1[from->idx[i]], -from->val[i]);
    for (; j < nj; j++) axpy(h, p->W1[to->idx[j]], to->val[j]);
}
void nn_acc_delta(const Net *n, const Feat *from, const float *acc_from, const Feat *to, float *acc_to) {
    memcpy(acc_to, acc_from, sizeof(float) * H1);
    add_diff(&n->p, from, to, acc_to);
}
void nn_forward_delta(const Net *n, const Feat *root, const float *acc, const Feat *f, float *out) {
    float h1[H1];
    memcpy(h1, acc, sizeof h1);
    add_diff(&n->p, root, f, h1);
    tail(&n->p, f->prior, h1, out, NOUT);
}
void nn_forward_list(const Net *n, const float *acc, const FeatDelta *d, float *out) {
    float h1[H1];
    memcpy(h1, acc, sizeof h1);
    for (int k = 0; k < d->n; k++) axpy(h1, n->p.W1[d->idx[k]], d->dv[k]);
    tail(&n->p, d->prior, h1, out, NOUT);
}

float nn_win_list(const Net *n, const float *acc, const FeatDelta *d) {       /* nn_forward_list, win logit only */
    float h1[H1], o[1];
    memcpy(h1, acc, sizeof h1);
    for (int k = 0; k < d->n; k++) axpy(h1, n->p.W1[d->idx[k]], d->dv[k]);
    tail(&n->p, d->prior, h1, o, 1);
    return o[0];
}

void nn_train_begin(Net *n) {
    if (!n->m) { n->m = calloc(1, sizeof(Params)); n->v = calloc(1, sizeof(Params)); n->g = calloc(1, sizeof(Params)); }
}

float nn_backward(Net *n, const Feat *f, const float *target, float weight) {
    float h1[H1], h2[H2], o[NOUT], d[NOUT], d2[H2], d1[H1];
    Params *g = n->g; const Params *p = &n->p;
    fwd(p, f, h1, h2, o);
    float loss = 0;
    for (int k = 0; k < NOUT; k++) {
        if (target[k] < -50) { d[k] = 0; continue; }       /* -100 = no target (VP changes can be negative) */
        if (nn_is_prob(k)) {               /* cross-entropy on a sigmoid */
            float y = sigm(o[k]); d[k] = (y - target[k]) * weight * (k == O_WIN ? 1.0f : 0.5f);
            if (k == O_WIN) { float e = 1e-6f; loss = -(target[k] * logf(y + e) + (1 - target[k]) * logf(1 - y + e)); }
        } else d[k] = (o[k] - target[k]) * weight * 0.5f;
    }
    for (int j = 0; j < NOUT; j++) g->b3[j] += d[j];
    for (int i = 0; i < H2; i++) {
        float v = h2[i], acc = 0;
        for (int j = 0; j < NOUT; j++) { acc += d[j] * p->W3[i][j]; g->W3[i][j] += d[j] * v; }
        d2[i] = v > 0 ? acc : 0; g->b2[i] += d2[i];
    }
    for (int i = 0; i < H1; i++) {
        float v = h1[i], acc = 0;
        const float *w = p->W2[i]; float *gw = g->W2[i];
        for (int j = 0; j < H2; j++) { acc += d2[j] * w[j]; gw[j] += d2[j] * v; }
        d1[i] = v > 0 ? acc : 0; g->b1[i] += d1[i];
    }
    for (int k = 0; k < f->n; k++) {
        float *gw = g->W1[f->idx[k]]; float v = f->val[k];
        for (int j = 0; j < H1; j++) gw[j] += v * d1[j];
    }
    return loss;
}

void nn_step(Net *n, float lr, int batch) {
    float *w = (float *)&n->p, *g = (float *)n->g, *m = (float *)n->m, *v = (float *)n->v;
    const float b1 = 0.9f, b2 = 0.999f, inv = 1.0f / (float)batch;
    n->step++;
    float c1 = 1.0f - powf(b1, (float)n->step), c2 = 1.0f - powf(b2, (float)n->step);
    float a = lr * sqrtf(c2) / c1;
    for (size_t i = 0; i < NPARAM; i++) {
        float gi = g[i] * inv;
        if (gi == 0 && m[i] == 0) continue;
        m[i] = b1 * m[i] + (1 - b1) * gi; v[i] = b2 * v[i] + (1 - b2) * gi * gi;
        w[i] -= a * m[i] / (sqrtf(v[i]) + 1e-8f);
        g[i] = 0;
    }
}

int nn_save(const Net *n, const char *path) {
    FILE *fp = fopen(path, "wb"); if (!fp) return -1;
    uint32_t hdr[4] = {0x574F5433, NFEAT, H1, H2};
    fwrite(hdr, sizeof hdr, 1, fp); fwrite(&n->p, sizeof(Params), 1, fp); fclose(fp);
    return 0;
}
int nn_load(Net *n, const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return -1;
    uint32_t hdr[4];
    if (fread(hdr, sizeof hdr, 1, fp) != 1 || hdr[0] != 0x574F5433 || hdr[1] > NFEAT || hdr[2] != H1 || hdr[3] != H2) { fclose(fp); return -2; }
    memset(n, 0, sizeof *n);
    /* A network saved before inputs were added at the end of the list: its first layer has fewer rows. The new rows
       start at zero, so it plays exactly as it did (the first layer is the first thing in Params). */
    size_t w1_old = (size_t)hdr[1] * H1 * sizeof(float), w1_new = sizeof n->p.W1;
    int ok = fread(&n->p, w1_old, 1, fp) == 1 && fread((char *)&n->p + w1_new, sizeof(Params) - w1_new, 1, fp) == 1; fclose(fp);
    return ok ? 0 : -3;
}
