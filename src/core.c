/* core.c -- state setup, unit/politics/control primitives, frame runner */
#include <stdio.h>
#include <stdlib.h>
#include "wotr.h"

uint8_t DIST[NREG][NREG];
static int g_init = 0;

void wotr_global_init(void) {
    if (g_init) return;
    memset(DIST, 0xFF, sizeof DIST);
    for (int a = 0; a < NREG; a++) {
        uint8_t q[NREG]; int qh = 0, qt = 0;
        DIST[a][a] = 0; q[qt++] = (uint8_t)a;
        while (qh < qt) {
            int u = q[qh++];
            for (int i = 0; i < REGION[u].nadj; i++) {
                int v = REGION[u].adj[i];
                if (DIST[a][v] == 0xFF) { DIST[a][v] = DIST[a][u] + 1; q[qt++] = (uint8_t)v; }
            }
        }
    }
    g_init = 1;
}

/* ---------- frames ---------- */
Frame *push(State *s, int kind) {
    if (s->sp >= MAXSTACK) { fprintf(stderr, "frame stack overflow\n"); abort(); }
    Frame *f = &s->st[s->sp++];
    memset(f, 0, sizeof *f);
    f->kind = (uint8_t)kind;
    return f;
}
void pop(State *s) { s->sp--; }

static const FrameFn FN[NFRAME] = {
    [FR_GAME] = fr_game, [FR_ACTIONS] = fr_actions, [FR_DISCARD] = fr_discard, [FR_FSP_PHASE] = fr_fsp_phase,
    [FR_HUNT] = fr_hunt, [FR_REVEAL] = fr_reveal, [FR_SEPARATE] = fr_separate, [FR_ARMY2] = fr_army2,
    [FR_MUSTER2] = fr_muster2, [FR_COMPMOVE] = fr_compmove, [FR_NAZMOVE] = fr_nazmove, [FR_BATTLE] = fr_battle,
    [FR_ROUND] = fr_round, [FR_CASUALTY] = fr_casualty, [FR_CARD] = fr_card, [FR_FSP_MOVE] = fr_fsp_move, [FR_CARDFX] = fr_cardfx,
};

static void run(State *s) {
    while (!s->winner && s->dkind == D_NONE) {
        table_upkeep(s);
        Frame *f = &s->st[s->sp - 1];
        FN[f->kind](s, f);
    }
    if (s->winner) s->dkind = D_NONE;
}

void wotr_apply(State *s, Move m) {
    s->mv = m;
    s->dkind = D_NONE;
    run(s);
}

int wotr_moves(const State *s, Move *out) {
    if (s->winner) return 0;
    int n = 0;
    Move z; memset(&z, 0, sizeof z);
    switch (s->dkind) {
    case D_ACTION: return gen_action(s, out);
    case D_ARMY_MOVE2:
        z.type = M_ARMY; z.f = 1;
        n = gen_army_moves(s, s->dplayer, 0, z, out, MAXMOVES - 1);
        {   /* card-driven moves may require a Nazgul (1) or the Witch-king (2) in the army */
            int filt = s->st[s->sp - 1].v[2], prev = s->st[s->sp - 1].v[3], k = 0;   /* prev = source of the first move + 1 */
            for (int i = 0; i < n; i++) {
                int src = out[i].a;
                if (filt == 1 && !(s->naz[src] || s->minion_loc[M_WK] == src)) continue;
                if (filt == 2 && s->minion_loc[M_WK] != src) continue;
                if (filt) out[i].e = 1;
                /* A-then-B equals B-then-A unless B enters the region A left: search one order only */
                if (wotr_prune && prev && !(src > prev - 1 || out[i].b == prev - 1)) continue;
                out[k++] = out[i];
            }
            n = k;
        }
        z.type = M_DONE; z.f = 0; out[n++] = z; return n;
    case D_MUSTER2:
        z.type = M_MUSTER;
        n = gen_muster(s, s->dplayer, z, s->st[s->sp - 1].v[0], out);
        if (wotr_prune) { int k = 0; for (int i = 0; i < n; i++) if (out[i].a > s->st[s->sp - 1].v[0]) out[k++] = out[i]; n = k; }
        z.type = M_DONE; out[n++] = z; return n;
    case D_COMP_MOVE:
        n = gen_comp_moves(s, out);
        z.type = M_DONE; out[n++] = z; return n;
    case D_NAZ_MOVE:
        n = gen_naz_moves(s, out);
        z.type = M_DONE; out[n++] = z; return n;
    default:
        for (int i = 0; i < s->nopt; i++) { z.type = M_CHOICE; z.a = s->opt[i]; out[n++] = z; }
        return n;
    }
}

/* ---------- setup ---------- */
static const uint8_t POOL_REG[NNAT] = {2, 2, 6, 6, 6, 6, 8, 10};
static const uint8_t POOL_ELI[NNAT] = {3, 4, 4, 4, 4, 5, 4, 3};
static const uint8_t POOL_LDR[NFPNAT] = {3, 0, 3, 3, 3};
static const uint8_t POL0[NNAT] = {3, 3, 2, 3, 3, 1, 1, 2};
static const uint8_t TILE0[NSTD_TILE] = {4, 3, 2, 1, 2, 2, 2};

void wotr_init(State *s, uint64_t seed) {
    wotr_global_init();
    memset(s, 0, sizeof *s);
    s->rng = seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
    for (int r = 0; r < NREG; r++) {
        const RegionInfo *ri = &REGION[r];
        s->ctrl[r] = (uint8_t)orig_side(r);
        int n = ri->setup_nation;
        if (n == NO_NATION) continue;
        s->reg[r][n] = ri->setup_reg; s->eli[r][n] = ri->setup_eli;
        if (n < NFPNAT) s->ldr[r][n] = ri->setup_ldr;
        s->naz[r] = ri->setup_naz;
    }
    memcpy(s->pool_reg, POOL_REG, NNAT); memcpy(s->pool_eli, POOL_ELI, NNAT);
    memcpy(s->pool_ldr, POOL_LDR, NFPNAT); s->pool_naz = 4;
    memcpy(s->pol, POL0, NNAT);
    s->active[ELVES] = s->active[ISENGARD] = s->active[SAURON] = s->active[SOUTHRON] = 1;
    for (int c = 0; c < NCOMP; c++) s->comp_loc[c] = LOC_FSP;
    for (int m = 0; m < NMINION; m++) s->minion_loc[m] = LOC_NONE;
    s->fsp_loc = R_RIVENDELL; s->guide = C_GANDALF; s->mordor = -1;
    memcpy(s->tile_pool, TILE0, NSTD_TILE);
    s->rings[FP] = 3;
    push(s, FR_GAME);
    run(s);
}

/* ---------- queries ---------- */
int units(const State *s, int side, int r) {
    int n = 0;
    if (side == FP) for (int i = 0; i < NFPNAT; i++) n += s->reg[r][i] + s->eli[r][i];
    else for (int i = NFPNAT; i < NNAT; i++) n += s->reg[r][i] + s->eli[r][i];
    return n;
}
int elites(const State *s, int side, int r) {
    int n = 0;
    if (side == FP) for (int i = 0; i < NFPNAT; i++) n += s->eli[r][i];
    else for (int i = NFPNAT; i < NNAT; i++) n += s->eli[r][i];
    return n;
}
int fp_leaders(const State *s, int r) { int n = 0; for (int i = 0; i < NFPNAT; i++) n += s->ldr[r][i]; return n; }
int comps_at(const State *s, int r) { int m = 0; for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == r) m |= 1 << c; return m; }
int minions_at(const State *s, int r) { int m = 0; for (int c = 0; c < NMINION; c++) if (s->minion_loc[c] == r) m |= 1 << c; return m; }
int fsp_comps(const State *s) { return comps_at(s, LOC_FSP); }
int comp_level(const State *s, int c) { (void)s; return c <= C_STRIDER ? 3 : c <= C_GIMLI ? 2 : 1; }
int comp_leadership(const State *s, int c) { return (c == C_STRIDER && s->aragorn) ? 2 : 1; }
int besieged_side(const State *s, int r) { return s->siege[r] ? s->ctrl[r] : -1; }
int enemy_field_units(const State *s, int side, int r) {
    int e = units(s, 1 - side, r);
    if (e && s->siege[r] && s->ctrl[r] == 1 - side) return 0;
    return e;
}
int at_war(const State *s, int n) { return s->pol[n] == 0; }
int all_at_war(const State *s, int side) {
    if (side == FP) { for (int n = 0; n < NFPNAT; n++) if (s->pol[n]) return 0; }
    else { for (int n = NFPNAT; n < NNAT; n++) if (s->pol[n]) return 0; }
    return 1;
}
int wotr_vp(const State *s, int side) {
    int vp = 0;
    for (int r = 0; r < NREG; r++) {
        int t = REGION[r].settle;
        if ((t == S_CITY || t == S_STRONGHOLD) && orig_side(r) == 1 - side && s->ctrl[r] == side) vp += t == S_CITY ? 1 : 2;
    }
    return vp;
}
int protected_region(const State *s, int r) {
    if (s->card_where[CARD_A_POWER_TOO_GREAT] == W_TABLE && (r == R_LORIEN || r == R_RIVENDELL || r == R_GREY_HAVENS)) return 1;
    if (s->card_where[CARD_THE_POWER_OF_TOM_BOMBADIL] == W_TABLE && (r == R_OLD_FOREST || r == R_THE_SHIRE || r == R_BUCKLAND)) return 1;
    return 0;
}
int shadow_leadership_units(const State *s, int r) {
    return (s->minion_loc[M_SARUMAN] < NREG) ? s->eli[r][ISENGARD] : 0;
}
int has_army_leader(const State *s, int side, int r) {
    if (side == FP) return fp_leaders(s, r) > 0 || comps_at(s, r) != 0;
    return s->naz[r] > 0 || minions_at(s, r) != 0 || shadow_leadership_units(s, r) > 0;
}

/* ---------- politics ---------- */
void activate(State *s, int n) {
    if (n >= NFPNAT || s->active[n]) return;
    s->active[n] = 1;
}
/* activation that Wormtongue can block */
static void activate_soft(State *s, int n) {
    if (n == ROHAN && s->card_where[CARD_WORMTONGUE] == W_TABLE) return;
    activate(s, n);
}
int advance_pol(State *s, int n, int cause) {
    if (s->pol[n] == 0) return 0;
    if (n < NFPNAT && !s->active[n] && s->pol[n] == 1) return 0;
    s->pol[n]--;
    if (n < NFPNAT && (cause == 1 || cause == 2) && s->card_where[CARD_THREATS_AND_PROMISES] == W_TABLE) discard_card(s, CARD_THREATS_AND_PROMISES);
    return 1;
}
void set_ctrl(State *s, int r, int side) {
    if (!is_settlement(r) || s->ctrl[r] == side) return;
    if (side != orig_side(r)) {
        int v = REGION[r].settle == S_CITY ? 1 : REGION[r].settle == S_STRONGHOLD ? 2 : 0;
        if (s->b.active) s->stat_vp_fight[side] += (uint8_t)v; else s->stat_vp_walk[side] += (uint8_t)v;
    }
    s->ctrl[r] = (uint8_t)side;
    if (side != orig_side(r)) advance_pol(s, REGION[r].nation, 3);
}
void army_entered(State *s, int side, int r) {
    int n = REGION[r].nation;
    if (side == SH && n < NFPNAT) activate_soft(s, n);   /* Wormtongue: only an attack on Edoras/Helm's Deep rouses Rohan */
    if (is_settlement(r) && s->ctrl[r] != side && units(s, 1 - side, r) == 0) set_ctrl(s, r, side);
}

/* ---------- units ---------- */
void kill_unit(State *s, int r, int n, int elite) {
    s->stat_hits[NAT_SIDE(n)] += elite ? 2 : 1;
    if (elite) { s->eli[r][n]--; if (n >= NFPNAT) s->pool_eli[n]++; }
    else { s->reg[r][n]--; if (n >= NFPNAT) s->pool_reg[n]++; else s->cas_reg[n]++; }
}
int downgrade_elite(State *s, int r, int n) {
    s->stat_hits[NAT_SIDE(n)]++;
    s->eli[r][n]--;
    if (n >= NFPNAT) {
        s->pool_eli[n]++;
        if (s->pool_reg[n]) { s->pool_reg[n]--; s->reg[r][n]++; return 1; }
        return 0;
    }
    if (s->cas_reg[n]) { s->cas_reg[n]--; s->reg[r][n]++; return 1; }
    if (s->pool_reg[n]) { s->pool_reg[n]--; s->reg[r][n]++; return 1; }
    return 0;
}
void kill_comp(State *s, int c) {
    s->comp_loc[c] = LOC_DEAD;
    fix_guide(s);
}
void kill_minion(State *s, int m) { s->minion_loc[m] = LOC_DEAD; }
void wipe_leaders(State *s, int side, int r, int kill_chars) {
    if (units(s, side, r) > 0) return;
    if (side == FP) {
        for (int n = 0; n < NFPNAT; n++) s->ldr[r][n] = 0;
        if (kill_chars) for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == r) kill_comp(s, c);
    } else if (kill_chars) {
        s->pool_naz += s->naz[r]; s->naz[r] = 0;
        for (int m = 0; m < NMINION; m++) if (s->minion_loc[m] == r) kill_minion(s, m);
    }
}
int recruit(State *s, int r, int n, int elite) {
    int side = NAT_SIDE(n);
    if (units(s, side, r) >= ((s->siege[r] && s->ctrl[r] == side) ? 5 : 10)) return 0;
    if (elite) { if (!s->pool_eli[n]) return 0; s->pool_eli[n]--; s->eli[r][n]++; }
    else { if (!s->pool_reg[n]) return 0; s->pool_reg[n]--; s->reg[r][n]++; }
    return 1;
}
int recruit_leader(State *s, int r, int n) {
    if (!s->pool_ldr[n] || units(s, FP, r) == 0) return 0;
    s->pool_ldr[n]--; s->ldr[r][n]++;
    return 1;
}
int recruit_naz(State *s, int r) {
    if (!s->pool_naz) return 0;
    s->pool_naz--; s->naz[r]++;
    return 1;
}
void enforce_stacking(State *s, int side, int r) {
    int lim = (s->siege[r] && s->ctrl[r] == side) ? 5 : 10;
    int lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT;
    while (units(s, side, r) > lim) {
        int done = 0;
        for (int n = hi - 1; n >= lo && !done; n--) if (s->reg[r][n]) { s->reg[r][n]--; s->pool_reg[n]++; done = 1; }
        for (int n = hi - 1; n >= lo && !done; n--) if (s->eli[r][n]) { s->eli[r][n]--; s->pool_eli[n]++; done = 1; }
    }
}
int free_for_recruit(const State *s, int side, int r) {
    if (s->siege[r]) return s->ctrl[r] == side;
    if (units(s, 1 - side, r)) return 0;
    if (is_settlement(r) && s->ctrl[r] != side) return 0;
    return 1;
}

/* ---------- ring ---------- */
void add_corruption(State *s, int n) {
    if (n <= 0) return;
    s->corruption = (uint8_t)(s->corruption + n > 12 ? 12 : s->corruption + n);
    if (s->corruption >= 12 && !s->winner) { s->winner = 2; s->win_type = WIN_CORRUPTION; }
}
void heal(State *s, int n) { s->corruption = (uint8_t)(s->corruption > n ? s->corruption - n : 0); }

void fix_guide(State *s) {
    int m = fsp_comps(s);
    if (!m) { s->guide = GOLLUM; return; }
    int best = 0;
    for (int c = 0; c < NCOMP; c++) if ((m >> c) & 1 && comp_level(s, c) > best) best = comp_level(s, c);
    if (s->guide < NCOMP && ((m >> s->guide) & 1) && comp_level(s, s->guide) == best) return;
    for (int c = 0; c < NCOMP; c++) if ((m >> c) & 1 && comp_level(s, c) == best) { s->guide = (uint8_t)c; return; }
}

/* ---------- cards ---------- */
int draw_card(State *s, int deck) {
    int cand[24], n = 0;
    for (int c = deck * 24; c < deck * 24 + 24; c++) if (s->card_where[c] == W_DECK) cand[n++] = c;
    if (!n) return -1;
    int c = cand[rnd(s, (uint32_t)n)];
    s->card_where[c] = W_HAND; s->hand_n[CARD_SIDE(c)]++;
    if (s->rng_max == 1) s->rng_max = 3;        /* card_has_effect: a draw within the turn being tried */
    return c;
}
void discard_card(State *s, int c) {
    if (s->card_where[c] == W_HAND) s->hand_n[CARD_SIDE(c)]--;
    s->card_where[c] = W_DISCARD;
}
void table_upkeep(State *s) {
    const uint8_t *w = s->card_where;
    if (w[CARD_AXE_AND_BOW] == W_TABLE && s->comp_loc[C_GIMLI] != LOC_FSP && s->comp_loc[C_LEGOLAS] != LOC_FSP) discard_card(s, CARD_AXE_AND_BOW);
    if (w[CARD_HORN_OF_GONDOR] == W_TABLE && s->comp_loc[C_BOROMIR] != LOC_FSP) discard_card(s, CARD_HORN_OF_GONDOR);
    if (w[CARD_WIZARDS_STAFF] == W_TABLE && s->comp_loc[C_GANDALF] != LOC_FSP) discard_card(s, CARD_WIZARDS_STAFF);
    if (w[CARD_DENETHORS_FOLLY] == W_TABLE && !(s->siege[R_MINAS_TIRITH] && s->ctrl[R_MINAS_TIRITH] == FP) && !s->b.active)
        discard_card(s, CARD_DENETHORS_FOLLY);
    if (w[CARD_THE_LAST_BATTLE] == W_TABLE) {              /* its play condition must keep holding */
        int r = s->comp_loc[C_STRIDER];
        if (!(s->aragorn && r < NREG && (units(s, FP, r) || s->b.active) && REGION[r].nation >= NFPNAT)) discard_card(s, CARD_THE_LAST_BATTLE);
    }
    int sar = s->minion_loc[M_SARUMAN] < NREG;
    if (w[CARD_THE_PALANTIR_OF_ORTHANC] == W_TABLE && !sar) discard_card(s, CARD_THE_PALANTIR_OF_ORTHANC);
    if (w[CARD_WORMTONGUE] == W_TABLE && (!sar || s->active[ROHAN])) discard_card(s, CARD_WORMTONGUE);
}

/* ---------- army movement ---------- */
int can_enter_army(const State *s, int side, int src, int dst) {
    (void)src;
    if (enemy_field_units(s, side, dst)) return 0;
    if (side == SH && protected_region(s, dst)) return 0;
    return 1;
}
/* per-nation units of `side` in src allowed to move to dst (or, if attack, to attack at all) */
int movable_units(const State *s, int side, int src, int dst, int attack, uint8_t *mr, uint8_t *me) {
    int lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT, tot = 0;
    memset(mr, 0, NNAT); memset(me, 0, NNAT);
    if (!attack && s->siege[src] && s->ctrl[src] == side) return 0;
    int dn = REGION[dst].nation;
    for (int n = lo; n < hi; n++) {
        if (!at_war(s, n)) {
            if (attack) continue;
            if (dn != NO_NATION && dn != n) continue;
        }
        mr[n] = s->reg[src][n]; me[n] = s->eli[src][n];
        tot += mr[n] + me[n];
    }
    return tot;
}
void move_army(State *s, int side, int src, int dst, int nr, int ne, int leaders, int attack_pick) {
    uint8_t mr[NNAT], me[NNAT];
    movable_units(s, side, src, dst, attack_pick, mr, me);
    int lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT;
    for (int pass = 0; pass < 2; pass++)               /* at-war nations first */
        for (int n = lo; n < hi; n++) {
            if ((at_war(s, n) ? 0 : 1) != pass) continue;
            int k = mr[n] < nr ? mr[n] : nr; s->reg[src][n] -= k; s->reg[dst][n] += k; nr -= k;
            k = me[n] < ne ? me[n] : ne; s->eli[src][n] -= k; s->eli[dst][n] += k; ne -= k;
        }
    int emptied = units(s, side, src) == 0;
    if (side == FP) {
        if (leaders || emptied) for (int n = 0; n < NFPNAT; n++) { s->ldr[dst][n] += s->ldr[src][n]; s->ldr[src][n] = 0; }
        if (leaders) for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == src) s->comp_loc[c] = (uint8_t)dst;
    } else if (leaders) {
        s->naz[dst] += s->naz[src]; s->naz[src] = 0;
        for (int m = 0; m < NMINION; m++) if (m != M_SARUMAN && s->minion_loc[m] == src) s->minion_loc[m] = (uint8_t)dst;
    }
    if (emptied && src != dst && s->siege[src] && s->ctrl[src] != side) s->siege[src] = 0;
    army_entered(s, side, dst);
    if (side == FP && leaders) place_comps(s, comps_at(s, dst), dst);
    enforce_stacking(s, side, dst);
    s->lock[dst >> 6] |= 1ULL << (dst & 63);
}

void relocate_army(State *s, int side, int from, int to) {
    for (int n = 0; n < NNAT; n++) {
        if (NAT_SIDE(n) != side) continue;
        s->reg[to][n] += s->reg[from][n]; s->reg[from][n] = 0;
        s->eli[to][n] += s->eli[from][n]; s->eli[from][n] = 0;
    }
    if (side == FP) {
        for (int n = 0; n < NFPNAT; n++) { s->ldr[to][n] += s->ldr[from][n]; s->ldr[from][n] = 0; }
        for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == from) s->comp_loc[c] = (uint8_t)to;
    } else {
        s->naz[to] += s->naz[from]; s->naz[from] = 0;
        for (int m = 0; m < NMINION; m++) if (m != M_SARUMAN && s->minion_loc[m] == from) s->minion_loc[m] = (uint8_t)to;
    }
    fix_siege(s, from);
    army_entered(s, side, to);
    if (side == FP) place_comps(s, comps_at(s, to), to);
    enforce_stacking(s, side, to);
}
/* repair siege status after units vanish: no besiegers -> lifted; no defenders -> stronghold falls */
void fix_siege(State *s, int r) {
    if (!s->siege[r]) return;
    int in = s->ctrl[r], out = 1 - in;
    if (!units(s, out, r)) { s->siege[r] = 0; return; }
    if (!units(s, in, r)) { s->siege[r] = 0; wipe_leaders(s, in, r, 1); set_ctrl(s, r, out); }
}

/* ---------- companions ---------- */
int comp_reach(const State *s, int from, int range, int siege_end, uint8_t *out) {
    uint8_t d[NREG], q[NREG]; int qh = 0, qt = 0, cnt = 0;
    memset(d, 0xFF, NREG); memset(out, 0, NREG);
    d[from] = 0; q[qt++] = (uint8_t)from; out[from] = 1; cnt = 1;
    if (s->siege[from] && s->ctrl[from] == FP) return cnt;
    while (qh < qt) {
        int u = q[qh++];
        if (d[u] >= range) continue;
        if (u != from && REGION[u].settle == S_STRONGHOLD && s->ctrl[u] == SH) continue;
        if (u != from && s->siege[u] && s->ctrl[u] == FP) continue;
        for (int i = 0; i < REGION[u].nadj; i++) {
            int v = REGION[u].adj[i];
            if (d[v] != 0xFF) continue;
            if (s->siege[v] && s->ctrl[v] == FP && !siege_end) continue;
            d[v] = d[u] + 1; q[qt++] = (uint8_t)v; out[v] = 1; cnt++;
        }
    }
    return cnt;
}
static int comp_activates(const State *s, int c, int n) {
    switch (c) {
    case C_GANDALF: case C_MERRY: case C_PIPPIN: return 1;
    case C_STRIDER: return s->aragorn || n == NORTH;
    case C_BOROMIR: return n == GONDOR;
    case C_LEGOLAS: return n == ELVES;
    case C_GIMLI: return n == DWARVES;
    }
    return 0;
}
void place_comps(State *s, int mask, int r) {
    int t = REGION[r].settle, n = REGION[r].nation;
    for (int c = 0; c < NCOMP; c++) {
        if (!((mask >> c) & 1)) continue;
        s->comp_loc[c] = (uint8_t)r;
        if ((t == S_CITY || t == S_STRONGHOLD) && n < NFPNAT && s->ctrl[r] == FP && comp_activates(s, c, n)) activate(s, n);
    }
    fix_guide(s);
}

/* ---------- determinization: reshuffle everything hidden from pov ---------- */
void wotr_determinize(State *s, int pov, uint64_t seed) {
    s->rng = seed * 0x9E3779B97F4A7C15ULL + 0xABCDEFULL;
    int opp = 1 - pov;
    for (int deck = opp * 2; deck < opp * 2 + 2; deck++) {
        int ids[24], n = 0, nh = 0;
        for (int c = deck * 24; c < deck * 24 + 24; c++)
            if (s->card_where[c] == W_DECK || s->card_where[c] == W_HAND) { ids[n++] = c; nh += s->card_where[c] == W_HAND; }
        for (int i = n - 1; i > 0; i--) { int j = (int)rnd(s, (uint32_t)i + 1), t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
        for (int i = 0; i < n; i++) s->card_where[ids[i]] = i < nh ? W_HAND : W_DECK;
    }
    s->nrand = 0;
}
