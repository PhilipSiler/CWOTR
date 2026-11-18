/* features.c -- encode a State as a sparse feature vector (absolute orientation: FP vs Shadow) */
#include <math.h>
#include "features.h"
#include <stdlib.h>

static uint8_t VPS[NVPS];
static uint8_t FRONTR[NREG];        /* 1: two regions from a Free Peoples city or stronghold; 2: next to one; 3: in one */
static uint8_t DIST_MORDOR[NREG];
static uint8_t N1[NVPS][8], N1n[NVPS], N2[NVPS][24], N2n[NVPS];   /* regions at distance 1 and 2 of each VP settlement */
static uint8_t N23[NVPS][64], N23n[NVPS], NEAR_FP[NREG];   /* regions 2-3 steps from each VP settlement; steps from each region to the nearest Free Peoples city or stronghold */
static uint32_t VMASK[NREG];            /* which VP settlements have region r in their two-step neighbourhood */
static int inited = 0;
#define TF_N (64 * 12 * 11 * 8 * 2)
static uint32_t tf_memo[TF_N];       /* see take_forecast_plan */

void feat_init(void) {
    if (inited) return;
    memset(tf_memo, 0xFF, sizeof tf_memo);
    wotr_global_init();
    int k = 0;
    for (int side = 0; side < 2; side++)
        for (int r = 0; r < NREG; r++) {
            int t = REGION[r].settle;
            if ((t == S_CITY || t == S_STRONGHOLD) && orig_side(r) == side) VPS[k++] = (uint8_t)r;
        }
    for (int q = 0; q < NVPS; q++)
        for (int r = 0; r < NREG; r++) {
            if (DIST[VPS[q]][r] == 1) N1[q][N1n[q]++] = (uint8_t)r;
            if (DIST[VPS[q]][r] == 2 && N2n[q] < 24) N2[q][N2n[q]++] = (uint8_t)r;
        }
    for (int q = 0; q < NVPS; q++) for (int r = 0; r < NREG; r++) if ((DIST[VPS[q]][r] == 2 || DIST[VPS[q]][r] == 3) && N23n[q] < 64) N23[q][N23n[q]++] = (uint8_t)r;
    for (int r = 0; r < NREG; r++) { NEAR_FP[r] = 9; for (int q = 0; q < NVPS; q++) if (orig_side(VPS[q]) == FP && DIST[VPS[q]][r] < NEAR_FP[r]) NEAR_FP[r] = DIST[VPS[q]][r]; }
    for (int q = 0; q < NVPS; q++) for (int r = 0; r < NREG; r++) if (DIST[VPS[q]][r] <= 2) VMASK[r] |= 1u << q;
    for (int q = 0; q < NVPS; q++) if (orig_side(VPS[q]) == FP) for (int r = 0; r < NREG; r++) if (DIST[VPS[q]][r] <= 2) { uint8_t v = (uint8_t)(3 - DIST[VPS[q]][r]); if (v > FRONTR[r]) FRONTR[r] = v; }
    for (int r = 0; r < NREG; r++) DIST_MORDOR[r] = DIST[r][R_MORANNON] < DIST[r][R_MINAS_MORGUL] ? DIST[r][R_MORANNON] : DIST[r][R_MINAS_MORGUL];
    inited = 1;
}

/* Where Shadow's armies stand, for comparison with human games: units still in the five Mordor regions, units within
   two regions of a Free Peoples city or stronghold, captured or not ("at the front"), those in or next to one ("at the walls"), those in one (besieging or holding it), and
   Free Peoples units shut inside a besieged stronghold. */
void shadow_deploy(const State *s, int *mordor, int *front, int *walls, int *inside, int *besieged) {
    int m = 0, fr = 0, wa = 0, in = 0, be = 0;
    for (int r = 0; r < NREG; r++) {
        int u = 0;
        for (int n = NFPNAT; n < NNAT; n++) u += s->reg[r][n] + s->eli[r][n];
        if (s->siege[r] && s->ctrl[r] == FP && orig_side(r) == FP) for (int n = 0; n < NFPNAT; n++) be += s->reg[r][n] + s->eli[r][n];
        if (!u) continue;
        if (r == R_BARAD_DUR || r == R_GORGOROTH || r == R_NURN || r == R_MINAS_MORGUL || r == R_MORANNON) m += u;
        if (FRONTR[r]) fr += u;                     /* taken or not: holding a settlement is being there */
        if (FRONTR[r] >= 2) wa += u;
        if (FRONTR[r] == 3) in += u;
    }
    *mordor = m; *front = fr; *walls = wa; *inside = in; *besieged = be;
}

/* The local picture around victory-point settlement k (0..NVPS-1), for fitting and checking the fall forecast:
   its region, who holds it, the holder's units there, enemy units next to it (or besieging it), enemy units two
   regions away, whether it is under siege, the besieger's/neighbour's leadership and the defenders' leadership. */
int vps_local(const State *s, int k, int *holder, int *mine, int *e1, int *e2, int *siege, int *la, int *ld) {
    int r = VPS[k], own = s->ctrl[r], en = 1 - own, m = 0, a1 = 0, a2 = 0, l1 = 0, l0 = 0;
    #define UN(side, q) ({ int u_ = 0; for (int n_ = (side) == FP ? 0 : NFPNAT; n_ < ((side) == FP ? NFPNAT : NNAT); n_++) u_ += s->reg[q][n_] + s->eli[q][n_]; u_; })
    #define LD(side, q) ({ int l_ = 0; if ((side) == FP) { for (int n_ = 0; n_ < NFPNAT; n_++) l_ += s->ldr[q][n_]; for (int c_ = 0; c_ < NCOMP; c_++) l_ += s->comp_loc[c_] == (q); } else { l_ = s->naz[q]; for (int m_ = 0; m_ < NMINION; m_++) l_ += s->minion_loc[m_] == (q); } l_; })
    m = UN(own, r); l0 = LD(own, r);
    for (int i = 0; i < N1n[k]; i++) { a1 += UN(en, N1[k][i]); l1 += LD(en, N1[k][i]); }
    for (int i = 0; i < N2n[k]; i++) a2 += UN(en, N2[k][i]);
    if (s->siege[r]) { a1 += UN(en, r); l1 += LD(en, r); }
    #undef UN
    #undef LD
    *holder = own; *mine = m; *e1 = a1; *e2 = a2; *siege = s->siege[r]; *la = l1; *ld = l0;
    return r;
}

int feat_front(int r) { return FRONTR[r]; }

int fsp_steps_to_mordor(const State *s) {
    if (s->mordor >= 0) return 0;
    int d = (int)DIST_MORDOR[s->fsp_loc] - s->progress;
    return d < 0 ? 0 : d;
}

#define PUT(i, v) do { float v_ = (float)(v); if (v_ != 0.0f) { f->idx[f->n] = (uint16_t)(i); f->val[f->n++] = v_; } } while (0)


/* ===================================================================================================
 * The encoding is organised in blocks (one per region, one per victory-point settlement, eight global
 * blocks), each emitting its features in increasing index order. A full encode runs every block. A delta
 * encode (feat_delta) runs only the blocks whose inputs differ from a cached root position and returns
 * the changes -- positions one move apart differ in a handful of blocks.
 * =================================================================================================== */
enum { GP = F_GLOBAL, GF = GP + 24, GT = GF + 44, GD = GT + 30, GA = GD + 32, GO = GA + 16, GC = GO + 22, GX = GC + 198, GH = F_GLOBAL + 424 };
typedef struct { int tot[2], vps, vpf, sfp, ssh, thr; float danger[2], gap, sg, fsg; } Agg;
static float take_forecast(float A, float La, float D, float Ld, int stronghold, int besieged, float reserve);
/* The conquest plan asks take_forecast the same few hundred questions over and over (army strength in quarters,
   leadership, garrison, leaders, stronghold or not), about a hundred times per position scored. The answers are kept
   in a table: the same numbers, without the arithmetic. */
/* tf_memo holds float bits; 0xFFFFFFFF = not worked out yet (set in feat_init) */
static float take_forecast_plan(float A, float La, int D, float Ld, int sh, float reserve) {
    int a4 = (int)(A * 4.0f), la = (int)La, ld = (int)Ld;
    if (A < 0 || a4 > 63 || (float)a4 != A * 4.0f || la > 11 || (float)la != La || D > 10 || ld > 7 || (float)ld != Ld) return take_forecast(A, La, (float)D, Ld, sh, 1, reserve);
    uint32_t *e = &tf_memo[(((a4 * 12 + la) * 11 + D) * 8 + ld) * 2 + sh];
    union { float f; uint32_t u; } v; v.u = *e;
    if (v.u == 0xFFFFFFFFu) { v.f = take_forecast(A, La, (float)D, Ld, sh, 1, reserve); *e = v.u; }
    return v.f;
}
static int reserve_on(void) { static int on = -1; if (on < 0) { const char *v = getenv("WOTR_RESERVE"); on = !(v && v[0] == '0'); } return on; }

static void chars(const State *s, uint8_t *cl, uint8_t *ml, int *fsp) {
    memset(cl, 0, NREG); memset(ml, 0, NREG);
    for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] < NREG) cl[s->comp_loc[c]] += (uint8_t)comp_leadership(s, c);
    for (int m = 0; m < NMINION; m++) if (s->minion_loc[m] < NREG) ml[s->minion_loc[m]]++;
    *fsp = s->mordor < 0 ? s->fsp_loc : -1;
}

static inline void emit_region(const State *s, int r, const uint8_t *cl, const uint8_t *ml, int fsp, uint8_t un[2][NREG], Feat *f) {
    int b = F_REGION + r * REG_F;
    uint64_t xr, xe;                  /* the 8 per-nation counts as one word: FP nations are the low 5 bytes */
    memcpy(&xr, s->reg[r], 8); memcpy(&xe, s->eli[r], 8);
    if (xr | xe) {
        int fr = (int)(((xr & 0xFFFFFFFFFFULL) * 0x0101010101010101ULL) >> 56), fe = (int)(((xe & 0xFFFFFFFFFFULL) * 0x0101010101010101ULL) >> 56);
        int sr = (int)(((xr >> 40) * 0x0101010101010101ULL) >> 56), se = (int)(((xe >> 40) * 0x0101010101010101ULL) >> 56);
        un[FP][r] = (uint8_t)(fr + fe); un[SH][r] = (uint8_t)(sr + se);
        PUT(b + 0, fr * 0.2f); PUT(b + 1, fe * 0.2f);
        if (fr + fe) PUT(b + 2, (s->ldr[r][0] + s->ldr[r][1] + s->ldr[r][2] + s->ldr[r][3] + s->ldr[r][4]) * 0.33f);
        PUT(b + 3, sr * 0.2f); PUT(b + 4, se * 0.2f);
    } else un[FP][r] = un[SH][r] = 0;
    if (s->naz[r]) PUT(b + 5, s->naz[r] * 0.33f);
    if (cl[r]) PUT(b + 6, 0.34f * cl[r]);
    if (ml[r]) PUT(b + 7, 0.5f * ml[r]);
    if (REGION[r].settle && REGION[r].settle != S_FORT && s->ctrl[r] != orig_side(r)) PUT(b + 8, 1);
    if (s->siege[r]) PUT(b + 9, 1);
    if (r == fsp) PUT(b + 10, 1);
}

/* leadership a side has in a region: leaders and companions, or Nazgul and minions */
static inline int lead_at(const State *s, int side, int q) {
    int l = 0;
    if (side == FP) { for (int n = 0; n < NFPNAT; n++) l += s->ldr[q][n]; for (int c = 0; c < NCOMP; c++) l += s->comp_loc[c] == q; }
    else { l = s->naz[q]; for (int m = 0; m < NMINION; m++) l += s->minion_loc[m] == q; }
    return l;
}
static int fall_on(void) { static int on = -1; if (on < 0) { const char *v = getenv("WOTR_FALL"); on = !(v && v[0] == '0'); } return on; }

/* one victory-point settlement: garrison and nearby threat; returns its contribution to the aggregates */
static inline FeatVC emit_vps(const State *s, int k, uint8_t un[2][NREG], Feat *f) {
    int r = VPS[k], own = orig_side(r), en = 1 - own, b = F_VPS + k * VPS_F, e1 = 0, e2 = 0, mine = un[own][r];
    FeatVC c = {0, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < N1n[k]; i++) e1 += un[en][N1[k][i]];
    for (int i = 0; i < N2n[k]; i++) e2 += un[en][N2[k][i]];
    e1 += s->siege[r] ? un[en][r] : 0;
    PUT(b + 0, mine * 0.1f); PUT(b + 1, e1 * 0.1f); PUT(b + 2, e2 * 0.05f);
    PUT(b + 3, s->ctrl[r] != own); PUT(b + 4, s->siege[r]);
    if (s->ctrl[r] != own) { int v = REGION[r].settle == S_CITY ? 1 : 2; if (own == FP) c.vps = (int8_t)v; else c.vpf = (int8_t)v; }
    if (s->siege[r]) { if (own == FP) c.sfp = 1; else c.ssh = 1; }
    if (own == FP && s->ctrl[r] == FP) c.thr = (int8_t)(e1 > 10 ? 10 : e1);
    if (s->ctrl[r] == own) {
        int sh = REGION[r].settle == S_STRONGHOLD;
        /* a stronghold's defenders hit twice as often as its besiegers, but four attackers still wear down two */
        float d = (e1 + 0.5f * e2 - (sh ? 1.2f : 0.8f) * mine) / 6.0f;
        if (!mine && e1 + e2 > 0) d += 0.5f;        /* an open door */
        d = d < 0 ? 0 : d > 1 ? 1 : d;
        c.dng = d * (sh ? 2 : 1);
    }
    /* The settlement's share of the strategy formula's garrison and siege terms. Computed here, per settlement, so that
       a searched position only recomputes the settlements near what moved (the forecast is the costly part). */
    if (own == SH && s->ctrl[r] == SH) {       /* strongholds and the two cities, Angmar and Far Harad */
        int near = 0, need;
        for (int i = 0; i < N1n[k]; i++) near += un[FP][N1[k][i]];       /* any detachment can lay a siege */
        for (int i = 0; i < N2n[k]; i++) near += un[FP][N2[k][i]];
        near += un[FP][r];
        need = near ? (near + 1 > 5 ? 5 : near + 1) : 1;                  /* one more than what could come knocking */
        if (un[SH][r] < need) c.gap = (float)(need - un[SH][r]) / need;
    }
    if (s->ctrl[r] == own) {                /* siege forecast for whoever threatens this settlement */
        int sh = REGION[r].settle == S_STRONGHOLD, from = -1, best = 0;
        if (s->siege[r]) { from = r; best = un[en][r]; }
        else for (int i = 0; i < N1n[k]; i++) if (un[en][N1[k][i]] > best) { best = un[en][N1[k][i]]; from = N1[k][i]; }
        if (from >= 0 && best > 0) {
            float la, ld;
            if (en == SH) {
                la = s->naz[from] + (s->minion_loc[M_WK] == from ? 2 : 0) + (s->minion_loc[M_MOUTH] == from ? 2 : 0);
                ld = s->ldr[r][0] + s->ldr[r][1] + s->ldr[r][2] + s->ldr[r][3] + s->ldr[r][4];
                for (int q = 0; q < NCOMP; q++) ld += s->comp_loc[q] == r;
                c.sg = (sh ? 2 : 1) * take_forecast(best, la, un[FP][r], ld, sh, s->siege[r], reserve_on() ? 2 + 0.7f * (e1 + e2 - best) : 0);   /* other Shadow units within two regions, and steady recruiting */
            } else {
                la = s->ldr[from][0] + s->ldr[from][1] + s->ldr[from][2] + s->ldr[from][3] + s->ldr[from][4];
                for (int q = 0; q < NCOMP; q++) la += s->comp_loc[q] == from;
                ld = s->naz[r] + (s->minion_loc[M_WK] == r ? 2 : 0) + (s->minion_loc[M_MOUTH] == r ? 2 : 0) + (s->minion_loc[M_SARUMAN] == r ? 1 : 0);
                c.fsg = (sh ? 2 : 1) * take_forecast(best, la, un[SH][r], ld, sh, s->siege[r], 0);
            }
        }
    }
    /* Fall forecast: the chance that the enemy holds this settlement two turns from now, from the local picture.
       Fitted on 133,521 settlement-positions of 1,508 human games (tools/humanlogs/falls.py): on games kept out of
       the fit it ranks the settlements that did fall at 0.84 (the hand-made danger figure above: 0.82) and its
       percentages are right -- of those it gives 20%, 25% fell; of those it gives 65%, 60%. The same weights serve
       for Shadow's own settlements: the rules of a siege are the same for both sides, and humans lose a Shadow
       settlement too rarely (0.7%) to fit that direction on its own. Where it is on, it replaces the siege forecast
       and the danger figure in the victory-point terms: a settlement about to fall counts as 0.8 of its points.
       WOTR_FALL=0 keeps the older figures, for control runs. */
    if (fall_on() && s->ctrl[r] == own) {
        float v = 0;
        if (e1 + e2 > 0) {
            int sh = REGION[r].settle == S_STRONGHOLD, la = s->siege[r] ? lead_at(s, en, r) : 0, ld = lead_at(s, own, r);
            for (int i = 0; i < N1n[k]; i++) if (un[en][N1[k][i]]) la += lead_at(s, en, N1[k][i]);
            float x1 = e1 > 10 ? 10 : e1, x2 = e2 > 10 ? 10 : e2, m = mine > 10 ? 10 : mine;
            /* "Under siege" is deliberately not an input, though it is the strongest single sign in the data: the
               defender chooses whether to withdraw behind the walls, and a forecast that reads a siege as bad news
               taught both sides to stand and fight in the field instead. Without it the forecast ranks as well
               (0.842 against 0.844). */
            float z = 0.216f * x1 + 0.138f * x2 - 0.319f * m - 0.090f * sh + 0.387f * (la > 4 ? 4 : la) - 0.161f * (ld > 4 ? 4 : ld)
                    + 0.410f * (mine == 0) + 0.081f * x1 * sh - 2.522f;
            v = 2.0f * (sh ? 2 : 1) / (1.0f + expf(-z));        /* twice the expected points: the terms below take 0.4 of it */
        }
        if (own == FP) c.sg = v; else c.fsg = v;
        c.dng = 0;
    }
    return c;
}
static void aggregate(const FeatVC *vc, uint8_t un[2][NREG], Agg *a) {
    memset(a, 0, sizeof *a);
    for (int r = 0; r < NREG; r++) { a->tot[FP] += un[FP][r]; a->tot[SH] += un[SH][r]; }
    for (int k = 0; k < NVPS; k++) {
        a->vps += vc[k].vps; a->vpf += vc[k].vpf; a->sfp += vc[k].sfp; a->ssh += vc[k].ssh; a->thr += vc[k].thr;
        a->danger[orig_side(VPS[k])] += vc[k].dng;
        a->gap += vc[k].gap; a->sg += vc[k].sg; a->fsg += vc[k].fsg;
    }
}

/* ---- global blocks ---- */
static void g_pol(const State *s, Feat *f) {
    for (int n = 0; n < NNAT; n++) { PUT(GP + n * 3, s->pol[n] * 0.33f); PUT(GP + n * 3 + 1, s->active[n]); PUT(GP + n * 3 + 2, s->pol[n] == 0); }
}
static void g_fsp(const State *s, Feat *f) {
    int g = GF, fc = fsp_comps(s), nfc = __builtin_popcount((unsigned)fc), shield = 0;
    for (int c = 0; c < NCOMP; c++) if ((fc >> c) & 1) shield += comp_level(s, c);
    int steps_out = fsp_steps_to_mordor(s), steps_in = s->mordor < 0 ? 5 : 5 - s->mordor;
    PUT(g + 0, s->progress / 12.0f); PUT(g + 1, s->revealed); PUT(g + 2, s->corruption / 12.0f);
    PUT(g + 3, s->mordor >= 0); if (s->mordor >= 0) PUT(g + 4 + s->mordor, 1);
    PUT(g + 10 + s->guide, 1);
    for (int c = 0; c < NCOMP; c++) PUT(g + 18 + c, s->comp_loc[c] == LOC_FSP);
    for (int c = 0; c < NCOMP; c++) PUT(g + 25 + c, s->comp_loc[c] == LOC_DEAD);
    PUT(g + 32, s->gandalf_white && s->comp_loc[C_GANDALF] < NREG); PUT(g + 33, s->aragorn && s->comp_loc[C_STRIDER] < NREG);
    PUT(g + 34, steps_out / 12.0f); PUT(g + 35, steps_in / 5.0f); PUT(g + 36, shield / 14.0f); PUT(g + 37, nfc / 7.0f);
    PUT(g + 38, s->fsp_acted); PUT(g + 39, s->corruption >= 8); PUT(g + 40, s->corruption >= 10);
    { int st = s->comp_loc[C_STRIDER]; if (st < NREG && !s->aragorn) { int d = DIST[st][R_MINAS_TIRITH]; if (DIST[st][R_DOL_AMROTH] < d) d = DIST[st][R_DOL_AMROTH]; if (DIST[st][R_PELARGIR] < d) d = DIST[st][R_PELARGIR]; PUT(g + 41, 1.0f / (1 + d)); } }
    PUT(g + 42, s->minion_seen);
}
static void g_tiles(const State *s, Feat *f) {
    int g = GT, tp = 0, dm = 0, rev = 0;
    static const int8_t TV[NTILE] = {2, 3, 2, 2, 1, 1, 0, 0, 0, -2, -1, 3, 3, 3, 1};
    for (int t = 0; t < NTILE; t++) { PUT(g + t, s->tile_pool[t] * 0.25f); tp += s->tile_pool[t]; }
    for (int t = 0; t < NTILE; t++) { dm += TV[t] * s->tile_pool[t]; if (t == T_EYE || t == T_2R || t == T_1R || t == T_0R || t == T_RING_MINE || t == T_GIVE_USS) rev += s->tile_pool[t]; }
    PUT(g + 15, tp / 16.0f); PUT(g + 16, tp ? (float)dm / tp / 3.0f : 0); PUT(g + 17, tp ? (float)rev / tp : 0);
    for (int t = NSTD_TILE; t < NTILE; t++) PUT(g + 18 + t - NSTD_TILE, s->special_st[t] == 1);
    PUT(g + 26, s->hunt_sh * 0.2f); PUT(g + 27, s->hunt_fp * 0.2f); PUT(g + 28, s->hunt_fp_prev > 0);
}
static void pools(const State *s, int *pool_fp, int *pool_sh) {
    *pool_fp = 4 + (s->aragorn && s->comp_loc[C_STRIDER] < NREG) + (s->gandalf_white && s->comp_loc[C_GANDALF] < NREG); *pool_sh = 7;
    for (int m = 0; m < NMINION; m++) *pool_sh += s->minion_loc[m] < NREG;
}
static void g_dice(const State *s, Feat *f) {
    int g = GD, nd[2] = {0, 0}, pool_fp, pool_sh;
    for (int side = 0; side < 2; side++) for (int k = 0; k < NFACE; k++) { PUT(g + side * NFACE + k, s->dice[side][k] * 0.25f); nd[side] += s->dice[side][k]; }
    PUT(g + 12, nd[FP] * 0.2f); PUT(g + 13, nd[SH] * 0.1f); PUT(g + 14, (nd[SH] - nd[FP]) * 0.2f);
    PUT(g + 15, s->rings[FP] * 0.33f); PUT(g + 16, s->rings[SH] * 0.33f); PUT(g + 17, s->ring_used[FP]); PUT(g + 18, s->ring_used[SH]);
    pools(s, &pool_fp, &pool_sh);
    for (int m = 0; m < NMINION; m++) { PUT(g + 19 + m * 3, s->minion_loc[m] < NREG); PUT(g + 20 + m * 3, s->minion_loc[m] == LOC_DEAD); PUT(g + 21 + m * 3, s->minion_loc[m] == LOC_NONE); }
    PUT(g + 28, (pool_fp - 4) * 0.5f); PUT(g + 29, (pool_sh - 7) * 0.33f); PUT(g + 30, s->mouth_used);
}
static void g_agg(const State *s, const Agg *a, Feat *f) {
    int g = GA, vps = a->vps, vpf = a->vpf;
    PUT(g + 0, vps * 0.1f); PUT(g + 1, vpf * 0.25f); PUT(g + 2, vps >= 6); PUT(g + 3, vps >= 8); PUT(g + 4, vps >= 10); PUT(g + 5, vpf >= 2); PUT(g + 6, vpf >= 4);
    PUT(g + 7, s->turn / 30.0f); PUT(g + 8, s->turn >= 10); PUT(g + 9, s->turn >= 15); PUT(g + 10, s->turn >= 20);
    PUT(g + 11, a->tot[FP] / 50.0f); PUT(g + 12, a->tot[SH] / 80.0f); PUT(g + 13, a->sfp * 0.5f); PUT(g + 14, a->ssh * 0.5f); PUT(g + 15, a->thr / 40.0f);
}
static void g_pools(const State *s, Feat *f) {
    int g = GO;
    for (int n = 0; n < NNAT; n++) { PUT(g + n * 2, s->pool_reg[n] * 0.1f); PUT(g + n * 2 + 1, s->pool_eli[n] * 0.2f); }
    PUT(g + 16, s->pool_naz * 0.125f);
    for (int n = 0; n < NFPNAT; n++) PUT(g + 17 + n, s->pool_ldr[n] * 0.33f);
}
/* cards: hands of both sides (search determinizes the opponent's), table cards, deck sizes */
static void g_cards(const State *s, Feat *f) {
    int g = GC, dk[4] = {0, 0, 0, 0};
    for (int pass = 0; pass < 2; pass++) {                 /* hand cards, then table cards; 8 cards per word */
        uint64_t pat = (pass ? W_TABLE : W_HAND) * 0x0101010101010101ULL;
        for (int w = 0; w < NCARD / 8; w++) {
            uint64_t x; memcpy(&x, s->card_where + w * 8, 8);
            if (!pass) {                                   /* count cards still in the deck (bytes equal to W_DECK = 0) */
                uint64_t z = ~(((x & 0x7F7F7F7F7F7F7F7FULL) + 0x7F7F7F7F7F7F7F7FULL) | x | 0x7F7F7F7F7F7F7F7FULL);
                dk[w / 3] += __builtin_popcountll(z);
            }
            uint64_t y = x ^ pat, hit = ~(((y & 0x7F7F7F7F7F7F7F7FULL) + 0x7F7F7F7F7F7F7F7FULL) | y | 0x7F7F7F7F7F7F7F7FULL);
            while (hit) { int c = w * 8 + (__builtin_ctzll(hit) >> 3); PUT(g + pass * 96 + c, 1); hit &= hit - 1; }
        }
    }
    g += 192;
    for (int k = 0; k < 4; k++) PUT(g + k, dk[k] / 24.0f);
    PUT(g + 4, s->hand_n[FP] / 6.0f); PUT(g + 5, s->hand_n[SH] / 6.0f);
}
static void g_ctx(const State *s, uint8_t un[2][NREG], Feat *f) {
    int g = GX;
    PUT(g + s->dkind, 1); PUT(g + ND, s->dplayer);
    g += ND + 1;
    if (s->b.active) {
        const Battle *b = &s->b;
        PUT(g + 0, 1); PUT(g + 1, b->att); PUT(g + 2, b->round * 0.2f); PUT(g + 3, b->siege); PUT(g + 4, b->sortie);
        PUT(g + 5, un[b->att][b->src] * 0.1f); PUT(g + 6, un[1 - b->att][b->dst] * 0.1f);
        PUT(g + 7, REGION[b->dst].settle == S_STRONGHOLD); PUT(g + 8, b->relief);
    }
    const Frame *t = &s->st[s->sp - 1];
    if (t->kind == FR_HUNT) { PUT(g + 9, t->v[4] * 0.33f); PUT(g + 10, t->v[6]); }
    if (t->kind == FR_CASUALTY) PUT(g + 11, t->v[2] * 0.33f);
    if (t->kind == FR_ARMY2) PUT(g + 12, 1);
}

/* The guides as inputs of the network, so that it can learn for itself how much each is worth in which position
   instead of being told through fixed weights: every term of the strategy formula, the two-turn fall forecast of
   every settlement, and where Shadow's units stand (in Mordor, at the front, at the walls, inside). The network's
   weights on these start at zero, so a network saved before they existed plays exactly as it did.
   WOTR_INPUTS=0 leaves the block empty, for control runs. */
static void g_guides(const State *s, const float *prior, const FeatVC *vc, uint8_t un[2][NREG], Feat *f) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("WOTR_INPUTS"); on = !(v && v[0] == '0'); }
    if (!on) return;
    int g = GH;
    for (int k = 0; k < NPRIOR; k++) if (k != 11) PUT(g + k, prior[k]);
    g += 24;
    for (int k = 0; k < NVPS; k++) PUT(g + k, 0.25f * (vc[k].sg + vc[k].fsg));     /* expected points about to change hands */
    g += NVPS;
    {
        int in = un[SH][R_BARAD_DUR] + un[SH][R_GORGOROTH] + un[SH][R_NURN] + un[SH][R_MORANNON], front = 0, walls = 0, inside = 0;
        for (int r = 0; r < NREG; r++) if (FRONTR[r]) { front += un[SH][r]; if (FRONTR[r] >= 2) walls += un[SH][r]; if (FRONTR[r] == 3) inside += un[SH][r]; }
        PUT(g + 0, in / 20.0f); PUT(g + 1, front / 40.0f); PUT(g + 2, walls / 30.0f); PUT(g + 3, inside / 20.0f);
        PUT(g + 4, un[SH][R_MINAS_MORGUL] * 0.2f); PUT(g + 5, un[SH][R_ORTHANC] * 0.2f); PUT(g + 6, un[SH][R_MORIA] * 0.2f);
    }
}

/* "Can this place be taken, can it be held?" (added 2026-11-16, Philip's idea). For each victory-point settlement,
   seen from whoever holds it now:
     0 the largest single enemy army next to it or besieging it      3 the most enemy units of nations AT WAR standing in one
     1 the largest single enemy army two or three moves away           region within three moves (only those may attack)
     2 nobody inside (one unit can walk in)                          4 the holder could recruit here now (its own settlement,
     5 leadership with the largest enemy army next to it               not besieged, nation at war, units left in the pool)
   and, for the whole board, the sizes of Shadow's three largest armies (the older mass input stops at five units)
   with how close each stands to a Free Peoples city or stronghold; and last, per settlement, ALL enemy strength
   within three moves (several small armies can join in a move or two).
   Army sizes here are in hits the army can take (a regular 1, an elite 2; a full stack of ten elites is 20), not units. The earlier per-settlement inputs give
   totals within one and two moves: five units in three regions looked like an army of five, a threat showed only
   two moves out, an empty stronghold was just "0.0 units", and an army that may not cross the border looked like
   one that may. The network's weights on these start at zero. */
static void g_watch(const State *s, uint8_t un[2][NREG], const uint8_t *cl, const uint8_t *ml, Feat *f) {
    uint8_t war[2][NREG], hp[2][NREG], tot[NVPS];
    int top[3] = {0, 0, 0}, topr[3] = {0, 0, 0};
    for (int r = 0; r < NREG; r++) {
        int w0 = 0, w1 = 0, h0 = 0, h1 = 0;
        if (un[FP][r]) for (int n = 0; n < NFPNAT; n++) { int h = s->reg[r][n] + 2 * s->eli[r][n]; h0 += h; if (s->pol[n] == 0) w0 += h; }
        if (un[SH][r]) {
            for (int n = NFPNAT; n < NNAT; n++) { int h = s->reg[r][n] + 2 * s->eli[r][n]; h1 += h; if (s->pol[n] == 0) w1 += h; }
            int v = h1, q = r;
            for (int k = 0; k < 3; k++) if (v > top[k]) { int tv = top[k], tq = topr[k]; top[k] = v; topr[k] = q; v = tv; q = tq; }
        }
        war[FP][r] = (uint8_t)w0; war[SH][r] = (uint8_t)w1; hp[FP][r] = (uint8_t)h0; hp[SH][r] = (uint8_t)h1;
    }
    for (int k = 0; k < NVPS; k++) {
        int r = VPS[k], hold = s->ctrl[r] == FP ? FP : SH, en = 1 - hold, b = F_WATCH + k * WATCH_F;
        int a1 = s->siege[r] ? hp[en][r] : 0, a1r = s->siege[r] ? r : -1, a2 = 0, w = s->siege[r] ? war[en][r] : 0, all = a1;
        for (int i = 0; i < N1n[k]; i++) { int x = N1[k][i]; all += hp[en][x]; if (hp[en][x] > a1) { a1 = hp[en][x]; a1r = x; } if (war[en][x] > w) w = war[en][x]; }
        for (int i = 0; i < N23n[k]; i++) { int x = N23[k][i]; all += hp[en][x]; if (hp[en][x] > a2) a2 = hp[en][x]; if (war[en][x] > w) w = war[en][x]; }
        PUT(b + 0, a1 * 0.05f); PUT(b + 1, a2 * 0.05f);
        PUT(b + 2, un[hold][r] == 0);
        PUT(b + 3, w * 0.05f);
        tot[k] = (uint8_t)(all > 60 ? 60 : all);
        { int n = REGION[r].nation; PUT(b + 4, hold == orig_side(r) && !s->siege[r] && s->pol[n] == 0 && s->pool_reg[n] + s->pool_eli[n] > 0); }
        if (a1r >= 0) {
            int ld = en == FP ? s->ldr[a1r][0] + s->ldr[a1r][1] + s->ldr[a1r][2] + s->ldr[a1r][3] + s->ldr[a1r][4] + cl[a1r] : s->naz[a1r] + ml[a1r];
            PUT(b + 5, (ld > 5 ? 5 : ld) * 0.2f);
        }
    }
    for (int k = 0; k < 3; k++) if (top[k]) {
        int b = F_WATCH + NVPS * WATCH_F + k * 2, d = NEAR_FP[topr[k]];
        PUT(b + 0, top[k] * 0.05f); PUT(b + 1, d >= 4 ? 0 : (4 - d) * 0.25f);
    }
    for (int k = 0; k < NVPS; k++) PUT(F_WATCH + NVPS * WATCH_F + 6 + k, tot[k] / 60.0f);      /* (inputs must be written in rising order) */
}

/* Danger and attack inputs (2026-11-17, Philip: "neural net inputs that would correctly detect when there is
   significant danger of FP attacking ... it's just a big graph with nodes and edges"; "if we have an empty shadow
   stronghold and there is an FP army two distance away and it cannot be intercepted, that is still a problem").
   Why these: against AlphaWOTR our Shadow lost 10 of 40 games to three units walking into an empty Morannon or storming
   a Dol Guldur held by one regular, and our Free Peoples lost Minas Tirith by turn 3; and the rollout chooser, which
   plays the turn out, overrules the ordinary choice most of all by NOT attacking (attacks 39% -> 20% of Shadow's dice,
   army moves 35% -> 49%). The pieces were among the inputs already (who is next door, is anyone inside); these do the
   sums the rollouts do by playing. For every victory-point settlement, seen from whoever holds it (so for a Free
   Peoples stronghold they are Shadow's chances, and for a captured town the chance of losing it again):
     0  fall forecast of the strongest enemy army NEXT DOOR (or besieging) against the garrison; 1 if nobody is inside
     1  the same for the strongest enemy army TWO regions away that has a free road in (a region between with none of
        the holder's units in it)
     2  input 0 again, but only if the enemy still has a die this turn that can move or attack with an army
     3  the open door: nobody inside and an enemy army next door 1.0; two away with a free road and the holder unable to
        get there first (no unit of its own next door, cannot recruit there now) 0.7; two away but coverable 0.35
     4  hits the attacking army lands in one combat round, 5 hits the defenders land (real odds: sixes against a
        stronghold, fives for the defence, leaders re-roll, five dice at most), in thirds
     6  combat rounds that army can fight on one die at a stronghold: one, plus one for each elite it can reduce
   then six totals: the points Shadow / the Free Peoples stand to lose that way (as a share of the 4 / 10 that end the
   game); a flag each for "the points the enemy has plus those it can take right now would end the game"; and for
   each side the actions it will still have this turn after the other has run out of dice (nobody can answer those). */
static void g_danger(const State *s, uint8_t un[2][NREG], const uint8_t *cl, const uint8_t *ml, Feat *f) {
    float expo[2] = {0, 0}; int now[2] = {0, 0}, die[2], mus[2], nd[2] = {0, 0};
    die[FP] = s->dice[FP][F_CHAR] + s->dice[FP][F_ARMY] + s->dice[FP][F_MA] + s->dice[FP][F_SPECIAL] > 0;
    die[SH] = s->dice[SH][F_CHAR] + s->dice[SH][F_ARMY] + s->dice[SH][F_MA] > 0;
    mus[FP] = s->dice[FP][F_MUSTER] + s->dice[FP][F_MA] + s->dice[FP][F_SPECIAL] > 0; mus[SH] = s->dice[SH][F_MUSTER] + s->dice[SH][F_MA] > 0;
    for (int k = 0; k < NFACE; k++) { nd[FP] += s->dice[FP][k]; nd[SH] += s->dice[SH][k]; }
    #define LEAD(side, x) ((side) == FP ? s->ldr[x][0] + s->ldr[x][1] + s->ldr[x][2] + s->ldr[x][3] + s->ldr[x][4] + cl[x] : s->naz[x] + ml[x])
    for (int k = 0; k < NVPS; k++) {
        int r = VPS[k], hold = s->ctrl[r] == FP ? FP : SH, en = 1 - hold, b = F_DANGER + k * DANGER_F;
        int sh = REGION[r].settle == S_STRONGHOLD, vp = sh ? 2 : 1, D = un[hold][r], a1 = 0, a1r = -1, a2 = 0, a2r = -1, cover = 0;
        if (s->siege[r]) { a1 = un[en][r]; a1r = r; }
        for (int i = 0; i < N1n[k]; i++) { int x = N1[k][i]; cover |= un[hold][x] > 0; if (un[en][x] > a1 && !(s->siege[x] && s->ctrl[x] == en)) { a1 = un[en][x]; a1r = x; } }
        for (int i = 0; i < N2n[k]; i++) {
            int x = N2[k][i], open = 0;
            if (un[en][x] <= a2 || (s->siege[x] && s->ctrl[x] == en)) continue;
            for (int j = 0; j < N1n[k] && !open; j++) open = DIST[N1[k][j]][x] == 1 && !un[hold][N1[k][j]];      /* a road in that nobody blocks */
            if (open) { a2 = un[en][x]; a2r = x; }
        }
        float ld = (float)LEAD(hold, r);
        float d1 = a1 ? take_forecast_plan((float)a1, (float)LEAD(en, a1r), D, ld, sh, 0) : 0;
        float d2 = a2 ? take_forecast_plan((float)a2, (float)LEAD(en, a2r), D, ld, sh, 0) : 0;
        if (d1 > 0) PUT(b + 0, d1);
        if (d2 > 0) PUT(b + 1, d2);
        if (d1 > 0 && die[en]) PUT(b + 2, d1);
        if (!D && (a1 || a2)) {
            int n = REGION[r].nation, recruit = hold == orig_side(r) && !s->siege[r] && n != NO_NATION && s->pol[n] == 0 && s->pool_reg[n] + s->pool_eli[n] > 0 && mus[hold];
            PUT(b + 3, a1 ? 1.0f : cover || recruit ? 0.35f : 0.7f);
        }
        if (a1) {
            float la = (float)LEAD(en, a1r), da = a1 > 5 ? 5 : a1, dd = D > 5 ? 5 : D, pa = sh ? 1.0f / 6 : 0.28f, ma = da * (1 - pa), md = dd * (2.0f / 3);
            float ha = da * pa + (la < ma ? la : ma) * pa, hd = dd / 3 + (ld < md ? ld : md) / 3;
            int el = 0; if (en == FP) for (int q = 0; q < NFPNAT; q++) el += s->eli[a1r][q]; else for (int q = NFPNAT; q < NNAT; q++) el += s->eli[a1r][q];
            PUT(b + 4, ha / 3.0f); PUT(b + 5, hd / 3.0f);
            if (sh) PUT(b + 6, (1 + (el > 4 ? 4 : el)) * 0.2f);
        }
        expo[hold] += vp * d1; if (d1 >= 0.5f && die[en]) now[hold] += vp;
    }
    #undef LEAD
    int b = F_DANGER + NVPS * DANGER_F;
    if (expo[SH] > 0) PUT(b + 0, expo[SH] / 4.0f);
    if (expo[FP] > 0) PUT(b + 1, expo[FP] / 10.0f);
    if (wotr_vp(s, FP) + now[SH] >= 4) PUT(b + 2, 1);
    if (wotr_vp(s, SH) + now[FP] >= 10) PUT(b + 3, 1);
    if (nd[FP] > nd[SH]) PUT(b + 4, (nd[FP] - nd[SH]) * 0.25f);
    if (nd[SH] > nd[FP]) PUT(b + 5, (nd[SH] - nd[FP]) * 0.25f);
}

/* Fast stand-in for multi-turn lookahead on a siege or assault: how likely, and how soon, A attackers with
 * leadership La take a settlement held by D defenders with leadership Ld. Uses the real odds: attackers hit a
 * stronghold on 6 (a city on 6 then 5), defenders on 5; leaders re-roll misses; five dice at most per side. */
static float take_forecast(float A, float La, float D, float Ld, int stronghold, int besieged, float reserve) {
    if (A <= 0) return 0;
    if (D <= 0) return 1;                                   /* an open door */
    float da = A > 5 ? 5 : A, dd = D > 5 ? 5 : D, pa = stronghold ? 1.0f / 6 : 0.28f;
    float ma = da * (1 - pa), md = dd * (2.0f / 3);
    float ha = da * pa + (La < ma ? La : ma) * pa, hd = dd / 3 + (Ld < md ? Ld : md) / 3;
    float rounds = D * 1.3f / ha;                           /* elites soak two hits: ~1.3 hits per unit */
    /* reserve: units that can replace the attackers' losses while the siege lasts. Shadow's dead return to its pool and
       are recruited again, and in human games the besiegers stay at about seven against four or five defenders for
       turns on end -- a fight the army on the spot loses on paper. Without the reserve a typical human siege was
       rated a one-in-ten chance and Shadow never pressed one. */
    float margin = (A + reserve) * 1.2f - rounds * hd;      /* attackers left standing when the garrison is gone */
    float win = 1.0f / (1.0f + expf(-margin / 2));
    float speed = 1.0f / (1.0f + rounds / (stronghold ? 4.0f : 6.0f));   /* a siege round costs an action die */
    return win * speed * (besieged ? 1.0f : 0.3f);          /* standing next door is a threat, not yet a siege */
}

/* ---- strategic summary terms (inputs of the linear prior) ---- */
/* reuse: the root's terms, given when no region, character or political track differs from the root -- the military
   terms (the expensive part) are then unchanged and are copied instead of recomputed */
static void prior_terms(const State *s, uint8_t un[2][NREG], const Agg *a, float *p, const float *reuse) {
    int fc = fsp_comps(s), shield = 0, pool_fp, pool_sh;
    for (int c = 0; c < NCOMP; c++) if ((fc >> c) & 1) shield += comp_level(s, c);
    int steps_out = fsp_steps_to_mordor(s), steps_in = s->mordor < 0 ? 5 : 5 - s->mordor;
    pools(s, &pool_fp, &pool_sh);
    /* expected corruption still to come vs. what the Ring-bearers can absorb */
    /* Exact Hunt arithmetic (the table in the strategy guide). A Fellowship step is hit unless every Hunt die and
       every re-roll misses: with n dice in the box (at most 5), r re-rolls (a Shadow stronghold, Shadow units, a Nazgul
       in the Fellowship's region: one each, never more than the dice that missed) and a to-hit number that drops by
       one for each step already taken this turn, P(hit) = 1 - q^(n + min(r, n)). A hit costs the average tile left
       in the pool, an Eye counting as the number of hits rolled. Steps still possible this turn are priced this way
       against the box as it stands; later steps at the ordinary two-dice, first-move rate. */
    int now = s->dice[FP][F_CHAR] + s->dice[FP][F_SPECIAL], eyes = s->hunt_sh > 5 ? 5 : s->hunt_sh, loc = s->fsp_loc;
    if (now > 2) now = 2;
    if (now > steps_out) now = steps_out;
    int rr = (REGION[loc].settle == S_STRONGHOLD && s->ctrl[loc] == SH) + (un[SH][loc] > 0) + (s->naz[loc] > 0 || s->minion_loc[M_WK] == loc);
    if (rr > eyes) rr = eyes;
    float tiles = 0, dmg = 0;
    static const float TV[NSTD_TILE] = {0, 3, 2, 2, 1, 1, 0};
    for (int t = 1; t < NSTD_TILE; t++) { tiles += s->tile_pool[t]; dmg += s->tile_pool[t] * TV[t]; }
    tiles += s->tile_pool[T_EYE];
    float exp_corr = 1.5f * steps_in;
    if (steps_in > 0) {
        /* In Mordor every move draws a tile, and "an Eye tile drawn in Mordor adds Corruption equal to the number of
           dice in the Hunt box, including the Fellowship's Character and Will of the West Action Dice previously used
           to move the Fellowship in the same turn" (Strategy 101, Into Mordor). So the price of the next step is read
           off the pool and the box as they stand (plus the die this step adds). The Shadow's four special tiles also stop the step, so more draws are needed than
           steps remain. Before the Fellowship enters, the same sum is made for the pool as it will be on entry -- the
           Eye tiles back in, the special tiles in play added -- so that walking in changes nothing but the box:
           priced two different ways, entering looked like a sudden loss and the Fellowship stayed at the gates.
           "You should at all times try to be up to speed on what Hunt tiles are left." */
        float tp[NTILE], all = 0;
        for (int t = 0; t < NTILE; t++) tp[t] = s->tile_pool[t];
        if (s->mordor < 0) { tp[T_EYE] = 4; for (int t = NSTD_TILE; t < NTILE; t++) if (s->special_st[t] == 1) tp[t] = 1; }
        float stop = tp[T_SHELOB] + tp[T_RING_MINE] + tp[T_ON_ON] + tp[T_GIVE_USS];
        for (int t = 0; t < NTILE; t++) all += tp[t];
        if (all > 0 && stop < all) {
            float base = 3 * tp[T_3] + 2 * (tp[T_2] + tp[T_2R]) + tp[T_1] + tp[T_1R] - 2 * tp[T_PHIAL] - tp[T_SMEAGOL] + 3.5f * tp[T_SHELOB] + tp[T_RING_MINE] + 3 * tp[T_ON_ON] + 2 * tp[T_GIVE_USS];
            /* later steps are priced at a typical box of two and a half. (Pricing them at "companions + 2", to make each
               companion count for the Hunt die it allows, was tried: the Free Peoples then shed their shields too
               early and corruption wins went from 21% to 55%.) */
            float later = (base + tp[T_EYE] * 2.5f) / all, first = s->mordor >= 0 ? (base + tp[T_EYE] * (s->hunt_sh + s->hunt_fp + 1)) / all : later;
            if (first < 0) first = 0;
            if (later < 0) later = 0;
            exp_corr = (first + (steps_in - 1) * later) * all / (all - stop);
        }
    }
    for (int k = 0; k < now; k++) {
        int thr = 6 - (s->hunt_fp + k); if (thr < 2) thr = 2;
        float q = (thr - 1) / 6.0f, miss = 1, qn = 1;
        for (int i = 0; i < eyes + rr; i++) miss *= q;
        for (int i = 0; i < eyes; i++) qn *= q;
        float eye = eyes && qn < 1 ? eyes * (1 - q) / (1 - qn) : 1;            /* hits rolled, given at least one */
        exp_corr += (1 - miss) * (tiles > 0 ? (dmg + s->tile_pool[T_EYE] * eye) / tiles : 1.5f);
    }
    exp_corr += (steps_out - now) * 0.3056f * (tiles > 0 ? (dmg + s->tile_pool[T_EYE] * 1.09f) / tiles : 1.5f);
    {   /* WOTR_HUNT=0: the earlier rough estimate, for control runs */
        static int exact = -1;
        if (exact < 0) { const char *v = getenv("WOTR_HUNT"); exact = !(v && v[0] == '0'); }
        if (!exact) { static const float HIT[6] = {0.0f, 0.30f, 0.55f, 0.76f, 0.93f, 1.08f}; exp_corr = HIT[eyes] * now + 0.55f * (steps_out - now) + 1.5f * steps_in; }
    }
    /* Companions in the Fellowship are shields: half a point of corruption per level. Tried and undone: valuing
       them more (the Free Peoples then hoarded them and died of corruption, 31-50% of games), and valuing them more
       outside Mordor than inside (entering Mordor then looked like a loss, and the Fellowship stayed outside).
       Human Ring wins enter Mordor with 4.1 companions and 2.6 corruption; ours with about 2 and 2.8. */
    p[0] = ((12 - s->corruption) + 0.5f * shield - exp_corr) / 6.0f;      /* ring-quest margin */
    p[1] = a->vps / 10.0f;                                               /* completed below with forecast conquests */
    p[2] = a->sfp * 0.15f;                                               /* sieges of FP strongholds in progress */
    p[3] = (steps_out + steps_in + (s->revealed ? 0.7f : 0.0f)) / 15.0f;  /* road still ahead of the Fellowship */
    p[4] = (pool_fp - 4) * 0.5f;
    {
        /* Minions in play, plus partial credit for one that could be mustered right now. Bringing Isengard to war is
           what makes Saruman available, Sauron at war with a Free Peoples nation at war the Witch-king: the half
           credit lets a one-move search see why those steps come first. */
        float ready = 0; int fpwar = 0;
        for (int n = 0; n < NFPNAT; n++) fpwar |= s->pol[n] == 0;
        if (s->minion_loc[M_SARUMAN] == LOC_NONE && s->pol[ISENGARD] == 0 && s->ctrl[R_ORTHANC] == SH) ready += 0.3f;
        if (s->minion_loc[M_WK] == LOC_NONE && s->pol[SAURON] == 0 && fpwar) ready += 0.3f;
        /* The Mouth also becomes available once the Fellowship is in Mordor, but crediting that made entering
           Mordor look like a 0.45-logit gift to Shadow, and the Fellowship camped at the gates instead. */
        if (s->minion_loc[M_MOUTH] == LOC_NONE && all_at_war(s, FP)) ready += 0.3f;
        p[5] = (pool_sh - 7 + ready) * 0.33f;
    }
    p[6] = a->vpf / 4.0f;                                                /* completed below likewise */
    /* attrition: Free Peoples losses are permanent, Shadow's return to the pool -- Shadow should welcome even trades */
    p[7] = (0.3f * a->tot[SH] - 1.5f * a->tot[FP]) / 40.0f;
    p[8] = a->ssh * 0.25f;                                               /* sieges of Shadow strongholds in progress */
    p[9] = s->mordor >= 0 ? (1 + s->mordor) / 6.0f : 0;
    if (reuse) {
        p[1] = reuse[1]; p[6] = reuse[6]; p[10] = reuse[10]; p[12] = reuse[12]; p[14] = reuse[14]; p[15] = reuse[15]; p[18] = reuse[18]; p[19] = reuse[19]; p[20] = reuse[20]; p[21] = reuse[21];
        p[13] = (s->dice[SH][F_CHAR] + s->dice[SH][F_ARMY] + s->dice[SH][F_MUSTER] + s->dice[SH][F_MA] + s->dice[SH][F_EVENT]) / 7.0f;
    } else {
        /* Shadow military terms, after the strategy guide:
         *  mass      - force is worth more concentrated and close to a target ("you need some serious muscle")
         *  idle      - units far from every target do nothing
         *  garrison  - a Shadow settlement with the enemy near and too few defenders gives its VP away
         *  siege     - a siege is worth what its besiegers and their leadership can deliver ("hitting a defended
         *              Stronghold with little or no Leadership is a bad move ... finish one siege at a time")
         *  war       - nations must be At War before they can attack or muster */
        float mass = 0, idle = 0, gap = 0, siege = 0, fsiege = 0, totsh = 0;
        uint8_t tgt[NVPS]; int nt = 0;      /* FP victory-point settlements still in FP hands */
        for (int k = 0; k < NVPS; k++) if (orig_side(VPS[k]) == FP && s->ctrl[VPS[k]] == FP) tgt[nt++] = VPS[k];
        for (int r = 0; r < NREG; r++) {
            float u = 0; int d = 99;
            if (!un[SH][r]) continue;
            for (int n = NFPNAT; n < NNAT; n++) u += (s->reg[r][n] + 1.5f * s->eli[r][n]) * (1.0f - 0.2f * s->pol[n]);
            for (int k = 0; k < nt; k++) if (DIST[r][tgt[k]] < d) d = DIST[r][tgt[k]];
            mass += u * (u < 5 ? u : 5) / (1.0f + d);       /* concentration pays up to a full five-dice army; beyond that, linearly */
            totsh += un[SH][r];
            if (d > 3 && !(REGION[r].settle == S_STRONGHOLD && s->ctrl[r] == SH)) idle += un[SH][r];
        }
        gap = a->gap; siege = a->sg; fsiege = a->fsg;       /* summed per settlement in emit_vps */
        p[10] = mass / 60.0f;
        p[12] = gap / 2.0f;
        /* Threatened and forecast conquests count as a FIXED FRACTION of the settlement's victory points, inside the
           VP terms themselves: at most 0.4 for a certain, quick conquest plus 0.1 for pressure. Taking a settlement
           is therefore always worth clearly more than menacing it -- as separate terms the threat credit nearly
           cancelled the capture, and Shadow besieged strongholds without ever walking in. */
        p[1] = (a->vps + 0.4f * siege + 0.1f * a->danger[FP]) / 10.0f;
        p[6] = (a->vpf + 0.4f * fsiege + 0.1f * a->danger[SH]) / 4.0f;
        /* Shadow action dice still to spend this turn: tempo. Every die sent to the Hunt box is one not rolled. */
        p[13] = (s->dice[SH][F_CHAR] + s->dice[SH][F_ARMY] + s->dice[SH][F_MUSTER] + s->dice[SH][F_MA] + s->dice[SH][F_EVENT]) / 7.0f;
        p[14] = (4 - s->pol[ISENGARD] - s->pol[SAURON] - s->pol[SOUTHRON]) / 4.0f;
        p[15] = totsh > 0 ? idle / totsh : 0;
        {
            /* Conquest plan: a stand-in for planning a campaign several turns ahead. Every Shadow army of three or more
               is matched to at most one Free Peoples settlement and every settlement to at most one army, best pairs
               first (four at most: humans run about three big armies). A pair is worth the settlement's victory
               points times the odds that this army takes this garrison (the siege forecast) times a discount for each
               region still to march. So a step toward a weakly held settlement, an attack, and a capture each raise
               the score, and a second army is drawn to a second target instead of crowding the first.
               WOTR_PLAN=0 switches the term off, for control runs. */
            static int on = -1;
            if (on < 0) { const char *v = getenv("WOTR_PLAN"); on = !(v && v[0] == '0'); }
            float plan = 0;
            if (on && nt) {
                static const float G[7] = {1.0f, 0.8f, 0.62f, 0.48f, 0.36f, 0.27f, 0.2f};
                uint8_t ar[16]; float aw[16], al[16], sc[16][NVPS]; int na = 0;
                for (int r = 0; r < NREG && na < 16; r++) {
                    if (un[SH][r] < 3) continue;
                    float u = 0;
                    for (int n = NFPNAT; n < NNAT; n++) u += (s->reg[r][n] + 1.5f * s->eli[r][n]) * (s->pol[n] ? 0.5f : 1.0f);   /* a nation not yet at war cannot attack */
                    float l = s->naz[r];
                    for (int m = 0; m < NMINION; m++) l += s->minion_loc[m] == r;
                    ar[na] = (uint8_t)r; aw[na] = u; al[na] = l; na++;
                }
                for (int k = 0; k < nt; k++) {
                    int t = tgt[k], sh = REGION[t].settle == S_STRONGHOLD; float ld = 0;
                    for (int n = 0; n < NFPNAT; n++) ld += s->ldr[t][n];
                    for (int i = 0; i < na; i++) {
                        int d = DIST[ar[i]][t];
                        sc[i][k] = d > 6 ? 0 : (sh ? 2 : 1) * G[d] * take_forecast_plan(aw[i], al[i], un[FP][t], ld, sh, reserve_on() ? 2 : 0);
                    }
                }
                unsigned ua = 0, ut = 0;
                for (int pick = 0; pick < 4; pick++) {
                    float best = 0; int bi = -1, bk = -1;
                    for (int i = 0; i < na; i++) if (!((ua >> i) & 1)) for (int k = 0; k < nt; k++) if (!((ut >> k) & 1) && sc[i][k] > best) { best = sc[i][k]; bi = i; bk = k; }
                    if (bi < 0) break;
                    plan += best; ua |= 1u << bi; ut |= 1u << bk;
                }
            }
            p[18] = plan / 10.0f;
        }
        {
            /* Deployment and pressure against what humans manage by the same turn (1,508 ladder games of 2022):
                 units still in Mordor          17 12 10  9  8  7  7  6 ...   (it empties on turns 1-3)
                 units at the front             21 29 34 37 39 40 ...          (within two regions of a Free Peoples
                                                                                city or stronghold, captured or not)
                 units at the walls              2  7 13 17 21 24 26 27 ...    (in or next to such a settlement)
                 units inside                    0  3  7 11 14 15 17 18 ...    (in its region: besieging or holding it;
                                                                                the only way in is to attack)
               p[19] counts every unit too many in Mordor and every unit too few at the front; p[20] every unit too
               few at the walls and every unit too few inside. Both are zero for a Shadow that keeps the human pace,
               so they push only a laggard.
               (Counting besieged defenders instead was tried first: the Free Peoples then simply stopped withdrawing
               into their strongholds, since being besieged had become a point against them.)
               WOTR_DEPLOY=0 switches both off, for control runs. */
            static int on = -1;
            if (on < 0) { const char *v = getenv("WOTR_DEPLOY"); on = !(v && v[0] == '0'); }
            float dep = 0, press = 0;
            if (on) {
                /* the figures used are those of the 589 human games Shadow won by conquest within twelve turns,
                   a little faster than the all-game averages in the comment above */
                /* Minas Morgul is not counted: humans keep four or five units there all game (it faces Minas Tirith)
                   while Barad-dur, Gorgoroth, Nurn and the Morannon empty. Counting it pushed that garrison out as
                   well, and the Free Peoples then took Minas Morgul in six games out of ten. */
                static const float HM[10] = {15, 11.3f, 7.5f, 6.1f, 5.1f, 4.6f, 4.1f, 3.8f, 3.6f, 3.4f};
                static const float HF[10] = {17, 21.1f, 29.7f, 34.7f, 37.8f, 39.2f, 39.2f, 39.2f, 39.2f, 39.2f};
                static const float HW[10] = {0, 1.6f, 7.6f, 13.6f, 18.1f, 21.9f, 24.4f, 26.0f, 27.3f, 27.3f};
                static const float HI[10] = {0, 0.3f, 3.2f, 7.8f, 11.7f, 14.9f, 16.9f, 18.0f, 19.1f, 19.4f};
                int t = s->turn < 10 ? s->turn : 9, front = 0, walls = 0, inside = 0;
                int in = un[SH][R_BARAD_DUR] + un[SH][R_GORGOROTH] + un[SH][R_NURN] + un[SH][R_MORANNON], all = 0;
                for (int r = 0; r < NREG; r++) { all += un[SH][r]; if (FRONTR[r]) { front += un[SH][r]; if (FRONTR[r] >= 2) walls += un[SH][r]; if (FRONTR[r] == 3) inside += un[SH][r]; } }
                /* The human figures are taken as shares of the Shadow units on the map (humans keep about 57 there), so
                   the terms ask where Shadow's units stand, not how many there are. Counted in plain units, every
                   besieger the Free Peoples killed raised Shadow's shortfall on three counts at once, and the Free
                   Peoples spent half their Character and Will dice attacking out of and into their own sieges -- the
                   "war of attrition" the guide says they must never commit to -- while the Fellowship stood still. */
                float sc = all / 57.0f;
                if (in > HM[t] * sc) dep += in - HM[t] * sc;       /* whether or not the Fellowship is inside: dropping this once it entered made entering Mordor look like a gift to Shadow, and the Fellowship sat at the gates in 98% of games */
                if (front < HF[t] * sc) dep += HF[t] * sc - front;
                if (walls < HW[t] * sc) press = HW[t] * sc - walls;
                if (inside < HI[t] * sc) press += HI[t] * sc - inside;
            }
            p[19] = dep / 20.0f; p[20] = press / 20.0f;
        }
        {
            /* Conquest clock: ten victory points by the end of turn 12. In the 589 human games Shadow won that way it
               held 1 point after turn 3, 2 after turn 4, 3.5 after turn 5, 5 after turn 6, 6.3 after turn 7, 7.2
               after turn 8 and 8 after turn 9. Every point Shadow is short of that clock -- counting captures in
               full and a siege for what the forecast gives it -- weighs on it, more each turn it falls further
               behind, so taking a settlement is worth the most to a Shadow that is late.
               WOTR_CLOCK=0 switches the term off, for control runs. */
            static int on = -1;
            if (on < 0) { const char *v = getenv("WOTR_CLOCK"); on = !(v && v[0] == '0'); }
            static const float CLOCK[13] = {0, 0, 0.3f, 1.0f, 2.0f, 3.5f, 5.0f, 6.3f, 7.2f, 8.0f, 8.7f, 9.3f, 10.0f};
            float have = a->vps + 0.4f * siege, due = CLOCK[s->turn < 12 ? s->turn : 12];
            p[21] = on && have < due ? (due - have) / 10.0f : 0;
        }
    }
    p[11] = 1.0f;
    {
        /* Free Peoples nations on the way to war: only a nation At War can recruit, and "a prudent Free Peoples player
           might have ... already moved Gondor on the Political track to enable him to muster defenders in Minas
           Tirith before your Armies can besiege it" (Strategy 101, Shadows over Middle-earth). In human games the
           Elves are at war by turn 4.4, the North by 4.5, Gondor by 5.1; the formula had no word for it and spent
           almost no dice there (0.2 a turn early against 0.5). Steps taken toward war, the three nations with
           strongholds on the Shadow's usual roads counting in full. */
        static const float W[NFPNAT] = {0.7f, 1.0f, 1.0f, 0.7f, 1.0f};      /* Dwarves, Elves, Gondor, North, Rohan */
        float pol = 0;
        for (int n = 0; n < NFPNAT; n++) pol += W[n] * (3 - (s->pol[n] > 3 ? 3 : s->pol[n])) / 3.0f;
        p[23] = pol / 4.4f;
    }
    {
        /* Fellowship behind: the Free Peoples' clock. Two measures, the larger counts.
           (a) The human pace: in 1,508 games the Fellowship moves about one and a half times a turn, is at the gates of
               Mordor after turn 6-7 and on Mount Doom around turn 10 -- 13 steps left after turn 1, 12, 10.5, 9, 7.5, 6,
               4.7, 3.3, 2, 1. This now runs through Mordor too: before, the pressure stopped at the gates and the
               Fellowship took seven turns over the last five steps.
           (b) The race: "The closer to ten Victory points you get the more pressured the Free Peoples player will be
               to move the Fellowship" (Strategy 101, The Hunt for the Ring). Shadow gains about a point a turn; if the
               Fellowship, at a step and a half a turn, needs more turns than Shadow has points left to take, every
               turn of that gap counts as a step and a half behind.
           WOTR_FSPCLOCK=0 switches the term off. */
        static int on = -1;
        if (on < 0) { const char *v = getenv("WOTR_FSPCLOCK"); on = !(v && v[0] == '0'); }
        static const float LEFT[12] = {15, 13.3f, 11.85f, 10.5f, 9.05f, 7.55f, 6.05f, 4.7f, 3.3f, 2.0f, 1.0f, 0};
        float left = (float)(steps_out + steps_in), behind = left - LEFT[s->turn < 11 ? s->turn : 11];
        float race = 1.5f * (left / 1.5f - (10.0f - a->vps));
        if (race > behind) behind = race;
        p[22] = on && behind > 0 ? behind / 10.0f : 0;
    }
    {
        /* Shadow behind schedule. In 293 human games Isengard is at war on turn 1, Saruman in by turn 2, Sauron at war
           by turn 2, the Witch-king in by turn 4 and the Southrons and Easterlings at war by turn 4-5. Each step
           still missing counts for every turn it is overdue (four at most).
           WOTR_TEMPO=0 switches the term off, for control runs. */
        static int on = -1;
        if (on < 0) { const char *v = getenv("WOTR_TEMPO"); on = !(v && v[0] == '0'); }
        float late = 0;
        if (on) {
            int t = s->turn;
            #define OVER(due) (t > (due) ? (t - (due) > 4 ? 4 : t - (due)) : 0)
            late += OVER(1) * s->pol[ISENGARD] + OVER(2) * s->pol[SAURON] + OVER(4) * s->pol[SOUTHRON] * 0.5f;
            late += OVER(2) * (s->minion_loc[M_SARUMAN] == LOC_NONE) + OVER(4) * (s->minion_loc[M_WK] == LOC_NONE);
            #undef OVER
            late /= 6.0f;
        }
        p[17] = late;
    }
    {
        /* Crowning potential: how far along Strider is toward becoming Aragorn (an extra action die). It grows as
           the Fellowship nears Gondor, again when he is separated and walking there, and peaks on arrival.
           Leaving the Fellowship costs the ring-quest term his protection; this is the other side of that ledger. */
        static const uint8_t city[3] = {R_MINAS_TIRITH, R_DOL_AMROTH, R_PELARGIR};
        int st = s->comp_loc[C_STRIDER], d = 99, from = st < NREG ? st : st == LOC_FSP && s->mordor < 0 ? s->fsp_loc : -1;
        if (from >= 0) for (int i = 0; i < 3; i++) if (s->ctrl[city[i]] == FP && DIST[from][city[i]] < d) d = DIST[from][city[i]];
        if (s->aragorn) p[16] = st < NREG ? 1.0f : 0.0f;
        else if (d == 99) p[16] = 0;
        else if (st < NREG) p[16] = 0.4f + 0.5f / (1 + d);
        else {
            /* still in the Fellowship: the option fades over the last three steps to the gates of Mordor, so that
               arriving there and going in costs nothing (it used to cost the whole credit, and the Fellowship sat
               at the Morannon for turns on end) */
            int df = d - s->progress, so = fsp_steps_to_mordor(s); if (df < 0) df = 0;
            p[16] = 0.3f / (1.0f + df / 3.0f) * (so >= 3 ? 1.0f : so / 3.0f);
        }
    }
}

void feat_cache(const State *s, FeatCache *c) {
    Feat *f = &c->f;
    Agg a;
    f->n = 0;
    chars(s, c->cl, c->ml, &c->fsp);
    for (int r = 0; r < NREG; r++) { c->rs[r] = (uint16_t)f->n; emit_region(s, r, c->cl, c->ml, c->fsp, c->un, f); }
    c->rs[NREG] = (uint16_t)f->n;
    for (int k = 0; k < NVPS; k++) { c->vs[k] = (uint16_t)f->n; c->vc[k] = emit_vps(s, k, c->un, f); }
    c->vs[NVPS] = (uint16_t)f->n;
    aggregate(c->vc, c->un, &a);
    c->gs[0] = (uint16_t)f->n; g_pol(s, f);
    c->gs[1] = (uint16_t)f->n; g_fsp(s, f);
    c->gs[2] = (uint16_t)f->n; g_tiles(s, f);
    c->gs[3] = (uint16_t)f->n; g_dice(s, f);
    c->gs[4] = (uint16_t)f->n; g_agg(s, &a, f);
    c->gs[5] = (uint16_t)f->n; g_pools(s, f);
    c->gs[6] = (uint16_t)f->n; g_cards(s, f);
    c->gs[7] = (uint16_t)f->n; g_ctx(s, c->un, f);
    prior_terms(s, c->un, &a, f->prior, NULL);
    c->gs[8] = (uint16_t)f->n; g_guides(s, f->prior, c->vc, c->un, f);
    c->gs[9] = (uint16_t)f->n; g_watch(s, c->un, c->cl, c->ml, f);
    c->gs[10] = (uint16_t)f->n; g_danger(s, c->un, c->cl, c->ml, f);
    c->gs[11] = (uint16_t)f->n;
}

void feat_encode(const State *s, Feat *f) {
    static _Thread_local FeatCache c;
    feat_cache(s, &c);
    f->n = c.f.n;
    memcpy(f->idx, c.f.idx, sizeof(uint16_t) * (size_t)c.f.n); memcpy(f->val, c.f.val, sizeof(float) * (size_t)c.f.n);
    memcpy(f->prior, c.f.prior, sizeof f->prior);
}

/* append (block of the leaf) - (block [a,b) of the root) to the delta */
static inline void diff_block(const Feat *root, int a, int b, const Feat *blk, FeatDelta *d) {
    int j = 0, n = blk->n, k = d->n;
    while (a < b && j < n) {
        int x = root->idx[a], y = blk->idx[j];
        if (x == y) { float dv = blk->val[j] - root->val[a]; if (dv != 0) { d->idx[k] = (uint16_t)x; d->dv[k++] = dv; } a++; j++; }
        else if (x < y) { d->idx[k] = (uint16_t)x; d->dv[k++] = -root->val[a]; a++; }
        else { d->idx[k] = (uint16_t)y; d->dv[k++] = blk->val[j]; j++; }
    }
    for (; a < b; a++) { d->idx[k] = root->idx[a]; d->dv[k++] = -root->val[a]; }
    for (; j < n; j++) { d->idx[k] = blk->idx[j]; d->dv[k++] = blk->val[j]; }
    d->n = k;
}

void feat_delta(const State *s0, const FeatCache *c, const State *s, FeatDelta *d) {
    static _Thread_local Feat blk;
    uint8_t un[2][NREG], cl[NREG], ml[NREG], chg[NREG];
    FeatVC vc[NVPS];
    int fsp;
    uint32_t vmask = 0;
    Agg a;
    d->n = 0;
    chars(s, cl, ml, &fsp);
    /* which regions changed? unit counts word by word; the rarely-changing arrays only if they differ at all */
    for (int r = 0; r < NREG; r++) {
        uint64_t x, y, u, v;
        memcpy(&x, s->reg[r], 8); memcpy(&y, s0->reg[r], 8); memcpy(&u, s->eli[r], 8); memcpy(&v, s0->eli[r], 8);
        chg[r] = (x != y) | (u != v);
    }
    if (memcmp(s->ldr, s0->ldr, sizeof s->ldr)) for (int r = 0; r < NREG; r++) chg[r] |= memcmp(s->ldr[r], s0->ldr[r], NFPNAT) != 0;
    if (memcmp(s->naz, s0->naz, NREG)) for (int r = 0; r < NREG; r++) chg[r] |= s->naz[r] != s0->naz[r];
    if (memcmp(s->ctrl, s0->ctrl, NREG)) for (int r = 0; r < NREG; r++) chg[r] |= s->ctrl[r] != s0->ctrl[r];
    if (memcmp(s->siege, s0->siege, NREG)) for (int r = 0; r < NREG; r++) chg[r] |= s->siege[r] != s0->siege[r];
    if (memcmp(cl, c->cl, NREG)) for (int r = 0; r < NREG; r++) chg[r] |= cl[r] != c->cl[r];
    if (memcmp(ml, c->ml, NREG)) for (int r = 0; r < NREG; r++) chg[r] |= ml[r] != c->ml[r];
    if (fsp != c->fsp) { if (fsp >= 0) chg[fsp] = 1; if (c->fsp >= 0) chg[c->fsp] = 1; }
    memcpy(un, c->un, sizeof un);
    for (int r = 0; r < NREG; r++) {
        if (!chg[r]) continue;
        blk.n = 0; emit_region(s, r, cl, ml, fsp, un, &blk);
        diff_block(&c->f, c->rs[r], c->rs[r + 1], &blk, d);
        vmask |= VMASK[r];
    }
    memcpy(vc, c->vc, sizeof vc);
    while (vmask) {
        int k = __builtin_ctz(vmask); vmask &= vmask - 1;
        blk.n = 0; vc[k] = emit_vps(s, k, un, &blk);
        diff_block(&c->f, c->vs[k], c->vs[k + 1], &blk, d);
    }
    aggregate(vc, un, &a);
#define BLOCK(i, call) do { blk.n = 0; call; diff_block(&c->f, c->gs[i], c->gs[i + 1], &blk, d); } while (0)
    if (memcmp(s->pol, s0->pol, NNAT) || memcmp(s->active, s0->active, NNAT)) BLOCK(0, g_pol(s, &blk));
    BLOCK(1, g_fsp(s, &blk));
    if (memcmp(s->tile_pool, s0->tile_pool, NTILE) || memcmp(s->special_st, s0->special_st, NTILE) || s->hunt_sh != s0->hunt_sh || s->hunt_fp != s0->hunt_fp || s->hunt_fp_prev != s0->hunt_fp_prev) BLOCK(2, g_tiles(s, &blk));
    BLOCK(3, g_dice(s, &blk));
    BLOCK(4, g_agg(s, &a, &blk));
    if (memcmp(s->pool_reg, s0->pool_reg, NNAT) || memcmp(s->pool_eli, s0->pool_eli, NNAT) || memcmp(s->pool_ldr, s0->pool_ldr, NFPNAT) || s->pool_naz != s0->pool_naz) BLOCK(5, g_pools(s, &blk));
    if (memcmp(s->card_where, s0->card_where, NCARD) || s->hand_n[0] != s0->hand_n[0] || s->hand_n[1] != s0->hand_n[1]) BLOCK(6, g_cards(s, &blk));
    BLOCK(7, g_ctx(s, un, &blk));
    {
        int same = !memcmp(s->pol, s0->pol, NNAT) && s->turn == s0->turn;       /* the timetable terms depend on the turn (tests/test_feat.c caught the stale copies) */
        for (int r = 0; r < NREG && same; r++) same = !chg[r];
        prior_terms(s, un, &a, d->prior, same ? c->f.prior : NULL);
    }
    BLOCK(8, g_guides(s, d->prior, vc, un, &blk));
    BLOCK(9, g_watch(s, un, cl, ml, &blk));
    BLOCK(10, g_danger(s, un, cl, ml, &blk));
}

/* Partial rewards. A position's "progress score" is a sum of small credits for things that have happened or been
   built up, seen from the Free Peoples' side (in win-probability units); the partial reward between two decisions is
   the change in it, so nothing is paid for standing still. Each line of RW is one kind of credit and its size;
   WOTR_REW="k:x,k:x" tries other sizes (k = position in the list, from 0) and WOTR_TD scales them all.
   The conquest lines follow the advice in strategy.txt: bring nine- and ten-unit armies with leadership next to a
   stronghold, besiege, take it; the Free Peoples' answer is units inside the threatened stronghold.
   Tried and dropped (2026-11-15): adding a position's score to the network's estimate when the player compares
   moves. With the old four-line table it made the side that did it far worse (Free Peoples 31% from 56.5%,
   Shadow 20% from 43.5%): corruption and steps then decide every close choice. */
float feat_td = 1.0f;
float feat_rw[NRW] = {     /* the four in use since 2026-11-14; the rest were tried at the sizes in brackets on 2026-11-15 and changed nothing, so they are off */
    0.010f,     /*  0 each Fellowship step still to walk (against the Free Peoples) */
    0.012f,     /*  1 each point of corruption */
    0.025f,     /*  2 each Shadow victory point (a stronghold is two) */
    0.050f,     /*  3 each Free Peoples victory point */
    0,          /*  4 a stronghold under siege, against its owner (0.020) */
    0,          /*  5 each Free Peoples unit alive, an elite counting two (0.003) */
    0,          /*  6 each Shadow unit on the board, an elite counting two (0.0015) */
    0,          /*  7 a nation at war, for its side (0.010) */
    0,          /*  8 a Shadow minion in play (0.010) */
    0,          /*  9 Gandalf the White, Aragorn crowned (0.015) */
    0,          /* 10 a companion dead (0.010) */
    0,          /* 11 each Shadow unit beyond the fifth, up to ten, next to or besieging a stronghold the Free Peoples hold (0.004) */
    0,          /* 12 each point of leadership, up to four, with such an army of six or more (0.004) */
    0,          /* 13 each Free Peoples unit, up to five, in a stronghold with five or more enemies next to it (0.004) */
    0,          /* 14 the Fellowship revealed (0.010) */
};
float feat_progress(const State *s, const float *rw) {
    float z = 0;
    int left = fsp_steps_to_mordor(s) + (s->mordor < 0 ? 5 : 5 - s->mordor);
    z -= rw[RW_STEP] * (float)left + rw[RW_CORR] * (float)s->corruption;
    z += rw[RW_FPVP] * (float)wotr_vp(s, FP) - rw[RW_SHVP] * (float)wotr_vp(s, SH);
    if (s->revealed && s->mordor < 0) z -= rw[RW_REVEALED];
    if (rw[RW_SIEGE] != 0 || rw[RW_FPUNIT] != 0 || rw[RW_SHUNIT] != 0) for (int r = 0; r < NREG; r++) {      /* (skipped while those sizes are zero: this runs at every decision of every training game) */
        if (s->siege[r] && REGION[r].settle == S_STRONGHOLD) z += s->ctrl[r] == FP ? -rw[RW_SIEGE] : rw[RW_SIEGE];
        for (int k = 0; k < NFPNAT; k++) z += rw[RW_FPUNIT] * (float)(s->reg[r][k] + 2 * s->eli[r][k]);
        for (int k = NFPNAT; k < NNAT; k++) z -= rw[RW_SHUNIT] * (float)(s->reg[r][k] + 2 * s->eli[r][k]);
    }
    if (rw[RW_THREAT] != 0 || rw[RW_LEAD] != 0 || rw[RW_GARRISON] != 0) for (int k = 0; k < NVPS; k++) {
        int h, mine, e1, e2, sg, la, ld, r = vps_local(s, k, &h, &mine, &e1, &e2, &sg, &la, &ld);
        if (REGION[r].settle != S_STRONGHOLD || orig_side(r) != FP || h != FP) continue;
        if (e1 > 10) e1 = 10;
        if (e1 > 5) z -= rw[RW_THREAT] * (float)(e1 - 5) + rw[RW_LEAD] * (float)(la > 4 ? 4 : la);
        if (e1 >= 5) z += rw[RW_GARRISON] * (float)(mine > 5 ? 5 : mine);
    }
    for (int k = 0; k < NNAT; k++) if (s->pol[k] == 0) z += k < NFPNAT ? rw[RW_WAR] : -rw[RW_WAR];
    for (int m = 0; m < NMINION; m++) if (s->minion_loc[m] < NREG) z -= rw[RW_MINION];
    z += rw[RW_CROWN] * (float)(s->gandalf_white + s->aragorn);
    for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == LOC_DEAD) z -= rw[RW_DEAD];
    return z;
}
