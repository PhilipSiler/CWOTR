/* fellowship.c -- fellowship phase, movement, the Hunt, reveal, separation */
#include "wotr.h"

static const int8_t TILE_VAL[NTILE] = {0, 3, 2, 2, 1, 1, 0, 0, 0, -2, -1, 0, 0, 3, 1};
#define TILE_IS_EYE(t) ((t) == T_EYE || (t) == T_RING_MINE)
#define TILE_REVEAL(t) ((t) == T_EYE || (t) == T_2R || (t) == T_1R || (t) == T_0R || (t) == T_RING_MINE || (t) == T_GIVE_USS)
#define TILE_FP_SPECIAL(t) ((t) >= T_CLOAKS && (t) <= T_SMEAGOL)
#define TILE_STOP(t) ((t) >= T_SHELOB)

int draw_tile(State *s) {
    int tot = 0;
    for (int t = 0; t < NTILE; t++) tot += s->tile_pool[t];
    if (!tot) {
        for (int t = 0; t < NSTD_TILE; t++) { s->tile_pool[t] += s->tile_out[t]; s->tile_out[t] = 0; tot += s->tile_pool[t]; }
        if (!tot) return -1;
    }
    int k = (int)rnd(s, (uint32_t)tot);
    for (int t = 0; t < NTILE; t++) {
        if (k < s->tile_pool[t]) { s->tile_pool[t]--; if (t < NSTD_TILE) s->tile_out[t]++; s->stat_tiles++; return t; }
        k -= s->tile_pool[t];
    }
    return -1;
}
void enter_special_tile(State *s, int t) {
    if (s->mordor >= 0) { s->tile_pool[t]++; s->special_st[t] = 2; } else s->special_st[t] = 1;
}
static void enter_mordor(State *s) {
    s->mordor = 0; s->progress = 0;
    s->tile_pool[T_EYE] += s->tile_out[T_EYE]; s->tile_out[T_EYE] = 0;
    for (int t = NSTD_TILE; t < NTILE; t++) if (s->special_st[t] == 1) { s->special_st[t] = 2; s->tile_pool[t]++; }
}

/* min over paths (<= steps long) of 10*(#Shadow strongholds) + (Moria while the Balrog is in play) */
static int path_cost(const State *s, int src, int dst, int steps) {
    int cur[NREG], nxt[NREG], w[NREG];
    int balrog = s->card_where[CARD_BALROG_OF_MORIA] == W_TABLE;
    for (int r = 0; r < NREG; r++) {
        w[r] = (REGION[r].settle == S_STRONGHOLD && orig_side(r) == SH && s->ctrl[r] == SH) ? 10 : 0;
        if (r == R_MORIA && balrog) w[r] += 1;
        cur[r] = 1 << 20;
    }
    cur[src] = w[src];
    for (int k = 0; k < steps; k++) {
        for (int v = 0; v < NREG; v++) {
            int b = cur[v];
            for (int i = 0; i < REGION[v].nadj; i++) { int c = cur[REGION[v].adj[i]] + w[v]; if (c < b) b = c; }
            nxt[v] = b;
        }
        memcpy(cur, nxt, sizeof cur);
    }
    return cur[dst];
}
static int mordor_dist(int r) { return DIST[r][R_MORANNON] < DIST[r][R_MINAS_MORGUL] ? DIST[r][R_MORANNON] : DIST[r][R_MINAS_MORGUL]; }
static int fp_haven(const State *s, int r) {
    int t = REGION[r].settle;
    return (t == S_CITY || t == S_STRONGHOLD) && orig_side(r) == FP && s->ctrl[r] == FP;
}
static void fsp_exposed(State *s) {   /* declared or revealed */
    if (s->card_where[CARD_THE_LAST_BATTLE] == W_TABLE) discard_card(s, CARD_THE_LAST_BATTLE);
}

/* The Fellowship phase as the rules have it (rulebook p. 17, 37, 43): the Free Peoples may declare the Fellowship,
 * change the Guide (among the Companions of the highest Level) and enter Mordor, each once, in any order (no
 * declaration after a change of Guide while Balrog of Moria is on the table: its tile depends on the Guide), until
 * they end the phase. One question (D_FSP_DECLARE): 255 ends it, a region declares, 200 + c names Guide c, 199 enters
 * Mordor. Frame vars: 0, 1 declaration scratch; 2 declared; 3 Guide changed. */
void fr_fsp_phase(State *s, Frame *f) {
    CO_BEGIN
    f->v[2] = f->v[3] = 0;
    for (;;) {
        s->nopt = 0; s->opt[s->nopt++] = 255;
        if (!f->v[2] && !s->revealed && s->mordor < 0 && !(f->v[3] && s->card_where[CARD_BALROG_OF_MORIA] == W_TABLE)) {
            int best = -1;      /* worth declaring: a haven (heal, rouse), a gate of Mordor, or the spot nearest to Mordor */
            for (int r = 0; r < NREG; r++) if (DIST[s->fsp_loc][r] <= s->progress && (best < 0 || mordor_dist(r) < mordor_dist(best))) best = r;
            for (int r = 0; r < NREG; r++) {
                if (DIST[s->fsp_loc][r] > s->progress) continue;
                if (wotr_prune && !(fp_haven(s, r) || r == R_MORANNON || r == R_MINAS_MORGUL || r == best)) continue;
                if (wotr_prune && s->progress == 0 && !(fp_haven(s, r) && (s->corruption > 0 || !s->active[REGION[r].nation]))) continue;
                s->opt[s->nopt++] = (uint8_t)r;
            }
        }
        if (!f->v[3] && s->guide < NCOMP) {
            int m = fsp_comps(s), best = 0;
            for (int c = 0; c < NCOMP; c++) if ((m >> c) & 1 && comp_level(s, c) > best) best = comp_level(s, c);
            for (int c = 0; c < NCOMP; c++) if ((m >> c) & 1 && comp_level(s, c) == best && c != s->guide) s->opt[s->nopt++] = (uint8_t)(FSP_GUIDE + c);
        }
        if (s->mordor < 0 && s->progress == 0 && (s->fsp_loc == R_MORANNON || s->fsp_loc == R_MINAS_MORGUL)) s->opt[s->nopt++] = FSP_MORDOR;
        if (s->nopt == 1) break;
        ASK(D_FSP_DECLARE, FP);
        if (s->mv.a == 255) break;
        if (s->mv.a >= FSP_GUIDE) { s->guide = (uint8_t)(s->mv.a - FSP_GUIDE); f->v[3] = 1; continue; }
        if (s->mv.a == FSP_MORDOR) { enter_mordor(s); continue; }
        {
            int dst = s->mv.a;
            f->v[2] = 1;
            f->v[0] = (uint8_t)(path_cost(s, s->fsp_loc, dst, s->progress) % 10);
            s->fsp_loc = (uint8_t)dst; s->progress = 0;
            fsp_exposed(s);
            f->v[1] = (uint8_t)fp_haven(s, dst);
            if (f->v[1]) {
                heal(s, 1);
                activate(s, REGION[dst].nation);
                if (s->card_where[CARD_WORN_WITH_SORROW_AND_TOIL] == W_TABLE) discard_card(s, CARD_WORN_WITH_SORROW_AND_TOIL);
                if (s->card_where[CARD_FLOCKS_OF_CREBAIN] == W_TABLE) discard_card(s, CARD_FLOCKS_OF_CREBAIN);
            }
        }
        if (f->v[0] && s->card_where[CARD_BALROG_OF_MORIA] == W_TABLE) {
            /* "You may discard" (the card's text): asked, as the BehindMedia referee does */
            s->nopt = 2; s->opt[0] = 1; s->opt[1] = 255; s->qtag = Q_BALROG; ASK(D_CARD_OPTION, SH); s->qtag = Q_NONE;
            if (s->mv.a == 1) {
                discard_card(s, CARD_BALROG_OF_MORIA);
                CALL(push_hunt(s, HM_CARD, HF_EYE_DISCARD | (f->v[1] ? HF_IGNORE_REVEAL : 0)));
            }
        }
    }
    CO_END
}

void fr_fsp_move(State *s, Frame *f) {
    CO_BEGIN
    s->fsp_acted = 1; s->stat_fsp_moves++;
    if (s->mordor < 0) { s->progress++; CALL(push_hunt(s, HM_ROLL, 0)); }
    else {
        CALL(push_hunt(s, HM_MORDOR, 0));
        if (!s->ret && !s->winner) {
            if (++s->mordor >= 5) { s->winner = 1; s->win_type = WIN_RING; }
        }
    }
    if (f->v[0] && s->card_where[CARD_THE_LAST_BATTLE] != W_TABLE) s->hunt_fp++;
    CO_END
}

void push_hunt(State *s, int mode, int flags) {
    Frame *f = push(s, FR_HUNT);
    f->v[0] = (uint8_t)mode; f->v[1] = (uint8_t)flags;
}
void push_reveal(State *s) { push(s, FR_REVEAL); }
void push_separate(State *s, int mask, int bonus, int siege_end, int fixed_level) {
    Frame *f = push(s, FR_SEPARATE);
    f->v[0] = (uint8_t)mask; f->v[1] = (uint8_t)bonus; f->v[2] = (uint8_t)siege_end; f->v[3] = (uint8_t)fixed_level;
}


/* frame vars: 0 mode, 1 flags, 2 successes, 3 tile, 4 damage, 5 stage, 6 reveal, 7 stop, 8 gollum-used */
void fr_hunt(State *s, Frame *f) {
    CO_BEGIN
    f->v[3] = 255; s->ret = 0; s->hunt_drawn = 255;
    if (f->v[0] == HM_ROLL) {
        if (!s->hunt_sh) break;
        f->v[2] = 0;
        if (s->card_where[CARD_FLOCKS_OF_CREBAIN] == W_TABLE) {     /* "you may discard" before the roll: asked */
            s->nopt = 2; s->opt[0] = 1; s->opt[1] = 255; s->qtag = Q_FLOCKS; ASK(D_CARD_OPTION, SH); s->qtag = Q_NONE;
            if (s->mv.a == 1) { discard_card(s, CARD_FLOCKS_OF_CREBAIN); f->v[2] = 1; }
        }
    }
    if (f->v[0] == HM_ROLL) {
        int n = s->hunt_sh > 5 ? 5 : s->hunt_sh, bonus = s->hunt_fp + f->v[2], succ = 0, fails = 0, loc = s->fsp_loc;
        if (!n) break;
        int thr = 6 - bonus; if (thr < 2) thr = 2;
        for (int i = 0; i < n; i++) { if (d6(s) >= thr) succ++; else fails++; }
        int rr = (REGION[loc].settle == S_STRONGHOLD && s->ctrl[loc] == SH) + (units(s, SH, loc) > 0)
               + (s->naz[loc] > 0 || s->minion_loc[M_WK] == loc);
        if (rr > fails) rr = fails;
        for (int i = 0; i < rr; i++) if (d6(s) >= thr) succ++;
        if (!succ) break;
        f->v[2] = (uint8_t)succ;
    }
    if (s->card_where[CARD_WIZARDS_STAFF] == W_TABLE) {
        s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1;
        ASK(D_STAFF, FP);
        if (s->mv.a) { discard_card(s, CARD_WIZARDS_STAFF); break; }
    }
    {
        int t = draw_tile(s);
        if (t < 0) break;
        f->v[3] = (uint8_t)t;
    }
    if (s->card_where[CARD_MITHRIL_COAT_AND_STING] == W_TABLE) {
        s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1;
        ASK(D_MITHRIL, FP);
        if (s->mv.a) {
            discard_card(s, CARD_MITHRIL_COAT_AND_STING);
            int t = f->v[3], t2;
            if (t < NSTD_TILE) s->tile_out[t]--;        /* the first tile is held aside until this Hunt ends (Card Almanac p. 11) */
            t2 = draw_tile(s);
            if (t2 >= 0) { f->v[3] = (uint8_t)t2; s->mithril_tile = (uint8_t)(t + 1); s->mithril_frame = (uint8_t)(f - s->st); }
            else if (t < NSTD_TILE) s->tile_out[t]++;
        }
    }
    if (f->v[1] & HF_DRAW_ONLY) { s->hunt_drawn = f->v[3]; break; }
    {
        int t = f->v[3], dmg = TILE_VAL[t];
        if ((f->v[1] & HF_EYE_DISCARD) && (TILE_IS_EYE(t) || TILE_FP_SPECIAL(t))) break;
        if (TILE_IS_EYE(t)) dmg = f->v[0] == HM_ROLL ? f->v[2] : f->v[0] == HM_MORDOR ? s->hunt_sh + s->hunt_fp : 0;
        else if (t == T_SHELOB) dmg = d6(s);
        if (dmg < 0) { heal(s, -dmg); dmg = 0; }
        f->v[4] = (uint8_t)dmg;
        f->v[6] = TILE_REVEAL(t) && !(f->v[1] & HF_IGNORE_REVEAL);
        f->v[7] = TILE_STOP(t);
    }
    /* Gollum as Guide ignores the Reveal icon of standard numbered tiles (checked when it matters: he may
       become the Guide in the middle of this very Hunt) */
#define GOLLUM_WAIVES() (s->guide == GOLLUM && !f->v[8] && (f->v[3] == T_2R || f->v[3] == T_1R || f->v[3] == T_0R))
    while (f->v[4] > 0) {
        {
            /* Foul Thing from the Deep: after cards and Guide abilities a random Companion must be lost */
            int forced = (f->v[1] & HF_RANDOM_FIRST) && f->v[5] < 3 && fsp_comps(s);
            s->nopt = 0;
            if (!forced) s->opt[s->nopt++] = 0;
            if (!(f->v[1] & HF_NO_REDUCE)) {
                if (f->v[5] == 0 && s->card_where[CARD_AXE_AND_BOW] == W_TABLE) s->opt[s->nopt++] = 1;
                if (f->v[5] == 0 && s->card_where[CARD_HORN_OF_GONDOR] == W_TABLE) s->opt[s->nopt++] = 2;
                if (s->guide == C_MERRY || s->guide == C_PIPPIN ||
                    (s->guide == GOLLUM && !(f->v[6] && !GOLLUM_WAIVES()) && !s->revealed && !f->v[8])) s->opt[s->nopt++] = 3;
                if (forced) s->opt[s->nopt++] = 5;
                else if (f->v[5] <= 2 && s->guide < NCOMP) { s->opt[s->nopt++] = 4; s->opt[s->nopt++] = 5; }
            }
        }
        if (s->nopt > 1) ASK(D_HUNT_RESPONSE, FP); else s->mv.a = s->opt[0];
        if (s->mv.a == 0) { add_corruption(s, f->v[4]); f->v[4] = 0; }
        else if (s->mv.a == 1 || s->mv.a == 2) {
            discard_card(s, s->mv.a == 1 ? CARD_AXE_AND_BOW : CARD_HORN_OF_GONDOR); f->v[4]--; if (f->v[5] < 1) f->v[5] = 1;
        } else if (s->mv.a == 3) {
            f->v[4]--; if (f->v[5] < 1) f->v[5] = 1;
            if (s->guide == GOLLUM) { f->v[6] = 1; f->v[8] = 1; }
            else if (s->mordor >= 0) kill_comp(s, s->guide);
            else CALL(push_separate(s, 1 << s->guide, 0, 0, 0));
        } else {
            int c = s->guide, lvl;
            if (s->mv.a == 5) {
                int m = fsp_comps(s), k = (int)rnd(s, (uint32_t)__builtin_popcount((unsigned)m));
                for (c = 0; c < NCOMP; c++) if ((m >> c) & 1 && k-- == 0) break;
            }
            lvl = comp_level(s, c);
            f->v[5] = 3;
            f->v[4] = (uint8_t)(f->v[4] > lvl ? f->v[4] - lvl : 0);
            if (c >= C_MERRY && s->mordor < 0) CALL(push_separate(s, 1 << c, 0, 0, 0));
            else kill_comp(s, c);
            /* Worn with Sorrow and Toil: the Shadow may discard a random Free Peoples Character card from the hand or
               one from the table, or nothing (card text; Card Almanac p. 26) */
            if (s->card_where[CARD_WORN_WITH_SORROW_AND_TOIL] == W_TABLE && !s->winner) {
                s->nopt = 0;
                for (int k = 0; k < 24; k++) if (s->card_where[k] == W_HAND) { s->opt[s->nopt++] = Q_HAND; break; }
                for (int k = 0; k < 24; k++) if (s->card_where[k] == W_TABLE) s->opt[s->nopt++] = (uint8_t)k;
                if (s->nopt) {
                    s->opt[s->nopt++] = 255;
                    s->qtag = Q_WORN_WITH_SORROW; ASK(D_CARD_OPTION, SH); s->qtag = Q_NONE;
                    if (s->mv.a == Q_HAND) {
                        int cand[24], n = 0;
                        for (int k = 0; k < 24; k++) if (s->card_where[k] == W_HAND) cand[n++] = k;
                        if (n) discard_card(s, cand[rnd(s, (uint32_t)n)]);
                    } else if (s->mv.a < 24) discard_card(s, s->mv.a);
                }
            }
        }
    }
    if (s->winner) return;
    if (f->v[6] && !GOLLUM_WAIVES()) {
        if (s->mordor >= 0) { s->revealed = 1; fsp_exposed(s); }
        else if (!s->revealed) CALL(push_reveal(s));
    }
    s->ret = f->v[7];
    }
    /* Mithril Coat and Sting: the first tile goes back to the pool when its Hunt is over */
    if (s->mithril_tile && s->mithril_frame == (uint8_t)(f - s->st)) { s->tile_pool[s->mithril_tile - 1]++; s->mithril_tile = 0; }
    pop(s); return;
}

void fr_reveal(State *s, Frame *f) {
    CO_BEGIN
    s->revealed = 1; fsp_exposed(s);
    s->nopt = 0;
    if (s->progress > 0)
        for (int r = 0; r < NREG; r++) if (DIST[s->fsp_loc][r] <= s->progress && !fp_haven(s, r)) s->opt[s->nopt++] = (uint8_t)r;
    if (!s->nopt) s->opt[s->nopt++] = s->fsp_loc;
    if (wotr_prune && s->nopt > 1) {
        /* keep the Pareto front over (Hunt tiles from Shadow strongholds on the way, distance to Mordor, Hunt re-rolls there) */
        int key[MAXOPT][3], k = 0;
        for (int i = 0; i < s->nopt; i++) {
            int r = s->opt[i];
            key[i][0] = path_cost(s, s->fsp_loc, r, s->progress);
            key[i][1] = mordor_dist(r);
            key[i][2] = (REGION[r].settle == S_STRONGHOLD && s->ctrl[r] == SH) + (units(s, SH, r) > 0) + (s->naz[r] > 0 || s->minion_loc[M_WK] == r);
        }
        for (int i = 0; i < s->nopt; i++) {
            int dom = 0;
            for (int j = 0; j < s->nopt && !dom; j++) {
                if (j == i) continue;
                int le = key[j][0] <= key[i][0] && key[j][1] <= key[i][1] && key[j][2] <= key[i][2];
                int lt = key[j][0] < key[i][0] || key[j][1] < key[i][1] || key[j][2] < key[i][2];
                dom = le && (lt || j < i);
            }
            if (!dom) { s->opt[k] = s->opt[i]; k++; }
        }
        s->nopt = (uint8_t)k;
    }
    if (s->nopt > 1) ASK(D_REVEAL_DEST, FP); else s->mv.a = s->opt[0];
    {
        int cost = path_cost(s, s->fsp_loc, s->mv.a, s->progress);
        s->fsp_loc = s->mv.a; s->progress = 0;
        f->v[0] = (uint8_t)(cost / 10); f->v[1] = (uint8_t)(cost % 10);
    }
    if (f->v[1] && s->card_where[CARD_BALROG_OF_MORIA] == W_TABLE) {
        s->nopt = 2; s->opt[0] = 1; s->opt[1] = 255; s->qtag = Q_BALROG; ASK(D_CARD_OPTION, SH); s->qtag = Q_NONE;
        if (s->mv.a == 1) {
            discard_card(s, CARD_BALROG_OF_MORIA);
            CALL(push_hunt(s, HM_CARD, HF_EYE_DISCARD));
        }
    }
    while (f->v[0] > 0 && !s->winner) { f->v[0]--; CALL(push_hunt(s, HM_STRONGHOLD, 0)); }
    CO_END
}

/* frame vars: 0 mask, 1 bonus, 2 may end in besieged stronghold, 3 fixed level (0 = own), 4 stay in place */
void fr_separate(State *s, Frame *f) {
    CO_BEGIN
    f->v[0] &= (uint8_t)fsp_comps(s);
    if (!f->v[0]) break;
    if (s->mordor >= 0) { for (int c = 0; c < NCOMP; c++) if ((f->v[0] >> c) & 1) kill_comp(s, c); break; }
    s->nopt = 0;
    if (f->v[4]) s->opt[s->nopt++] = s->fsp_loc;
    else {
        int lvl = f->v[3];
        uint8_t reach[NREG];
        if (!lvl) for (int c = 0; c < NCOMP; c++) if ((f->v[0] >> c) & 1 && comp_level(s, c) > lvl) lvl = comp_level(s, c);
        comp_reach(s, s->fsp_loc, lvl + s->progress + f->v[1], f->v[2], reach);
        s->nopt = (uint8_t)comp_dests(s, f->v[0], reach, 255, s->opt);
        if (!s->nopt || !wotr_prune) { int dup = 0; for (int i = 0; i < s->nopt; i++) dup |= s->opt[i] == s->fsp_loc; if (!dup) s->opt[s->nopt++] = s->fsp_loc; }
    }
    if (s->nopt > 1) ASK(D_SEP_DEST, FP); else s->mv.a = s->opt[0];
    place_comps(s, f->v[0], s->mv.a);
    CO_END
}
