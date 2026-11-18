/* policy.c -- linear-in-h move scorer with per-field embeddings */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include "policy.h"

void pol_init(Policy *p) { memset(p, 0, sizeof *p); }

typedef struct { int n; int off[8]; float sc[8]; } Rows;     /* offsets (in floats) of the embedding rows of a move */
#define ROW(field, i) ((int)(offsetof(PolParams, field) / sizeof(float)) + (i) * H1)
static void add(Rows *r, int off, float sc) { r->off[r->n] = off; r->sc[r->n++] = sc; }

static void move_rows(int dkind, Move m, Rows *r) {
    r->n = 0;
    add(r, ROW(type, m.type), 1);
    add(r, ROW(kind, dkind), 1);
    if (dkind == D_ACTION && m.type != M_PASS && m.type != M_RING) add(r, ROW(die, m.die & 7), 1);
    switch (m.type) {
    case M_CHOICE:
        switch (dkind) {
        case D_FSP_DECLARE: case D_SEP_DEST: case D_REVEAL_DEST: case D_RETREAT: case D_CARD_REGION: case D_CARD_ARMY:
            add(r, ROW(dst, m.a < NREG ? m.a : NREG), 1); break;
        case D_DISCARD: case D_BATTLE_CARD: case D_CARD_CARD:
            add(r, ROW(card, m.a < NCARD ? m.a : NCARD), 1); break;
        default: add(r, ROW(aux, m.a & 31), 1); break;
        }
        break;
    case M_ARMY: case M_ATTACK:
        add(r, ROW(src, m.a), 1); add(r, ROW(dst, m.b), 1);
        add(r, ROW(cnt, 0), m.c * 0.1f); add(r, ROW(cnt, 1), m.d * 0.1f);
        break;
    case M_PLAY: case M_DISCARD_TABLE: add(r, ROW(card, m.a), 1); break;
    case M_DRAW: add(r, ROW(aux, 32 + m.a), 1); break;
    case M_DIPLO: case M_COMP_POLITICS: add(r, ROW(aux, 34 + m.a), 1); break;
    case M_MUSTER_ELITE: case M_GANDALF_WHITE: add(r, ROW(dst, m.a), 1); break;
    case M_MUSTER: add(r, ROW(dst, m.a), 1); add(r, ROW(aux, 42 + m.b), 1); break;
    case M_MINION: add(r, ROW(dst, m.b), 1); add(r, ROW(aux, 44 + m.a), 1); break;
    case M_VOICE: add(r, ROW(aux, 47 + m.a), 1); break;
    case M_SEPARATE: case M_COMP_MOVE:
        for (int c = 0; c < NCOMP && r->n < 7; c++) if ((m.a >> c) & 1) add(r, ROW(aux, 49 + c), 1);
        if (m.type == M_COMP_MOVE && r->n < 8) add(r, ROW(dst, m.b), 1);
        break;
    case M_NAZ_MOVE: add(r, ROW(src, m.a), 1); add(r, ROW(dst, m.b), 1); add(r, ROW(aux, 56 + m.d * 2 + (m.c > 1)), 1); break;
    case M_NAZ_PLAN: add(r, ROW(dst, m.a < NREG ? m.a : NREG), 1); add(r, ROW(src, m.b < NREG ? m.b : NREG), 1); add(r, ROW(aux, 80 + m.c), 1); break;
    case M_RING: add(r, ROW(aux, 62 + m.a), 1); add(r, ROW(aux, 68 + m.b), 1); break;
    case M_SKIP: add(r, ROW(aux, 74 + (m.die & 7)), 1); break;
    default: break;
    }
}

static inline float dot(const float *a, const float *b) { float s = 0; for (int j = 0; j < H1; j++) s += a[j] * b[j]; return s; }

void pol_logits(const Policy *p, const float *h, int dkind, const Move *mv, int n, float *logit) {
    const float *base = (const float *)&p->p;
    /* rows repeat heavily across the move list (same type, same source region...): cache their dot products */
    static _Thread_local float cache[sizeof(PolParams) / sizeof(float) / H1 + 1];
    static _Thread_local uint32_t stamp[sizeof(PolParams) / sizeof(float) / H1 + 1], cur = 0;
    cur++;
    Rows r;
    for (int i = 0; i < n; i++) {
        move_rows(dkind, mv[i], &r);
        float s = p->p.bias[mv[i].type];
        for (int k = 0; k < r.n; k++) {
            int row = r.off[k] / H1;
            if (stamp[row] != cur) { cache[row] = dot(base + r.off[k], h); stamp[row] = cur; }
            s += r.sc[k] * cache[row];
        }
        logit[i] = s;
    }
}

float pol_backward(Policy *p, const float *h, int dkind, const Move *mv, int n, const float *target) {
    float logit[MAXMOVES], mx = -1e30f, z = 0, loss = 0;
    if (!p->g) { p->m = calloc(1, sizeof(PolParams)); p->v = calloc(1, sizeof(PolParams)); p->g = calloc(1, sizeof(PolParams)); }
    pol_logits(p, h, dkind, mv, n, logit);
    for (int i = 0; i < n; i++) if (logit[i] > mx) mx = logit[i];
    for (int i = 0; i < n; i++) { logit[i] = expf(logit[i] - mx); z += logit[i]; }
    float *g = (float *)p->g;
    Rows r;
    for (int i = 0; i < n; i++) {
        float pr = logit[i] / z, d = pr - target[i];
        if (target[i] > 0) loss -= target[i] * logf(pr + 1e-9f);
        if (fabsf(d) < 1e-5f) continue;
        move_rows(dkind, mv[i], &r);
        p->g->bias[mv[i].type] += d;
        for (int k = 0; k < r.n; k++) { float c = d * r.sc[k]; float *gr = g + r.off[k]; for (int j = 0; j < H1; j++) gr[j] += c * h[j]; }
    }
    return loss;
}

void pol_step(Policy *p, float lr, int batch) {
    float *w = (float *)&p->p, *g = (float *)p->g, *m = (float *)p->m, *v = (float *)p->v;
    const float b1 = 0.9f, b2 = 0.999f, inv = 1.0f / (float)batch;
    size_t np = sizeof(PolParams) / sizeof(float);
    p->step++;
    float a = lr * sqrtf(1.0f - powf(b2, (float)p->step)) / (1.0f - powf(b1, (float)p->step));
    for (size_t i = 0; i < np; i++) {
        float gi = g[i] * inv;
        if (gi == 0 && m[i] == 0) continue;
        m[i] = b1 * m[i] + (1 - b1) * gi; v[i] = b2 * v[i] + (1 - b2) * gi * gi;
        w[i] -= a * m[i] / (sqrtf(v[i]) + 1e-8f);
        g[i] = 0;
    }
}

int pol_save(const Policy *p, const char *path) {
    FILE *fp = fopen(path, "wb"); if (!fp) return -1;
    uint32_t hdr[2] = {0x504F4C31, (uint32_t)sizeof(PolParams)};
    fwrite(hdr, sizeof hdr, 1, fp); fwrite(&p->p, sizeof(PolParams), 1, fp); fclose(fp);
    return 0;
}
int pol_load(Policy *p, const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return -1;
    uint32_t hdr[2];
    memset(p, 0, sizeof *p);
    int ok = fread(hdr, sizeof hdr, 1, fp) == 1 && hdr[0] == 0x504F4C31 && hdr[1] == sizeof(PolParams) && fread(&p->p, sizeof(PolParams), 1, fp) == 1;
    fclose(fp);
    return ok ? 0 : -2;
}
