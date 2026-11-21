/* flow.c -- turn structure, action dice, and the simple action chains */
#include "wotr.h"

#define MAX_TURNS 200     /* no turn cap in the rules: the referee ends a game at its own (was 30, as a Shadow win) */

static int ndice(const State *s, int side) { int n = 0; for (int f = 0; f < NFACE; f++) n += s->dice[side][f]; return n; }
static int pool_size(const State *s, int side) {
    if (side == FP) return 4 + (s->aragorn && s->comp_loc[C_STRIDER] < NREG) + (s->gandalf_white && s->comp_loc[C_GANDALF] < NREG);
    int n = 7;
    for (int m = 0; m < NMINION; m++) n += s->minion_loc[m] < NREG;
    return n;
}

void fr_discard(State *s, Frame *f) {
    CO_BEGIN
    while (s->hand_n[f->v[0]] > 6) {
        s->nopt = 0;
        for (int c = f->v[0] * 48; c < f->v[0] * 48 + 48; c++) if (s->card_where[c] == W_HAND) s->opt[s->nopt++] = (uint8_t)c;
        ASK(D_DISCARD, f->v[0]);
        discard_card(s, s->mv.a);
    }
    CO_END
}
static void push_discard(State *s, int side) { push(s, FR_DISCARD)->v[0] = (uint8_t)side; }

void fr_game(State *s, Frame *f) {
    static const uint8_t FPF[6] = {F_CHAR, F_CHAR, F_MA, F_MUSTER, F_EVENT, F_SPECIAL};
    static const uint8_t SHF[6] = {F_CHAR, F_ARMY, F_MUSTER, F_MA, F_EVENT, F_SPECIAL};
    CO_BEGIN
    for (;;) {
        s->turn++;
        /* 1: recover dice, draw cards */
        s->hunt_fp_prev = s->hunt_fp; s->hunt_fp = 0; s->hunt_sh = 0;
        s->ring_used[0] = s->ring_used[1] = 0; s->mouth_used = 0; s->fsp_acted = 0;
        memset(s->dice, 0, sizeof s->dice);
        for (int deck = 0; deck < 4; deck++) draw_card(s, deck);
        CALL(push_discard(s, FP));
        CALL(push_discard(s, SH));
        /* 2: fellowship phase */
        CALL(push(s, FR_FSP_PHASE));
        /* 3: hunt allocation */
        {
            int lo = s->hunt_fp_prev ? 1 : 0, hi = __builtin_popcount((unsigned)fsp_comps(s));
            if (hi < 1) hi = 1;
            if (hi > pool_size(s, SH)) hi = pool_size(s, SH);
            s->nopt = 0;
            for (int k = lo; k <= hi; k++) s->opt[s->nopt++] = (uint8_t)k;
        }
        ASK(D_HUNT_ALLOC, SH);
        s->hunt_sh = s->mv.a;
        /* 4: action roll */
        for (int i = pool_size(s, FP); i > 0; i--) s->dice[FP][FPF[rndk(s, 6, 1)]]++;
        for (int i = pool_size(s, SH) - s->hunt_sh; i > 0; i--) {
            int face = SHF[rndk(s, 6, 2)];
            if (face == F_SPECIAL) s->hunt_sh++; else s->dice[SH][face]++;
        }
        /* 5: action resolution */
        CALL(push(s, FR_ACTIONS));
        if (s->rng_max) { s->winner = 3; return; }     /* card_has_effect: the trial stops before end-of-turn effects */
        /* 6: end of turn */
        if (s->mordor >= 0 && !s->fsp_acted) add_corruption(s, 1);
        if (s->winner) return;
        if (wotr_vp(s, SH) >= 10) { s->winner = 2; s->win_type = WIN_SH_MIL; return; }
        if (wotr_vp(s, FP) >= 4) { s->winner = 1; s->win_type = WIN_FP_MIL; return; }
        if (s->turn >= MAX_TURNS) { s->winner = 2; s->win_type = WIN_NONE; return; }
    }
    CO_END
}

static void use_die(State *s, int side, int die) {
    s->dice[side][die & 0x7F]--;
    if (die & DIE_MOUTH) s->mouth_used = 1;
}
static void do_muster(State *s, int side, int r, int leader) {
    int n = REGION[r].nation;
    if (!leader) recruit(s, r, n, 0);
    else if (side == FP) recruit_leader(s, r, n);
    else recruit_naz(s, r);
}

void do_naz_move(State *s, Move m) {
    if (m.d == 0) { s->naz[m.a] -= m.c; s->naz[m.b] += m.c; s->naz_moved[m.b] += m.c; }   /* (the region is not locked: Nazgul there that have not moved may still go) */
    else { s->minion_loc[m.d == 1 ? M_WK : M_MOUTH] = m.b; s->comp_moved |= (uint8_t)(1 << (m.d - 1)); }
}

static void do_action(State *s, int side) {
    Move m = s->mv;
    s->lock[0] = s->lock[1] = 0; s->comp_moved = 0; memset(s->naz_moved, 0, sizeof s->naz_moved);
    if (m.type != M_PASS) use_die(s, side, m.die);
    switch (m.type) {
    case M_PASS: case M_SKIP: break;
    case M_FSP_MOVE: push(s, FR_FSP_MOVE)->v[0] = 1; break;
    case M_FSP_HIDE: s->revealed = 0; s->fsp_acted = 1; break;
    case M_SEPARATE: push_separate(s, m.a, 0, 0, 0); break;
    case M_COMPS: push(s, FR_COMPMOVE); break;
    case M_ARMY:
        move_army(s, side, m.a, m.b, m.c, m.d, m.e, 0);
        if (m.f) { Frame *f = push(s, FR_ARMY2); f->v[0] = (uint8_t)side; f->v[3] = (uint8_t)(m.a + 1); }
        break;
    case M_ATTACK: push_battle(s, side, m.a, m.b, m.c, m.d); break;
    /* a Will of the West result may become an Event result first (rulebook p. 21): Gandalf the Grey's draw too */
    case M_PLAY: push_card(s, m.a, (m.die & 0x7F) == F_EVENT || (side == FP && (m.die & 0x7F) == F_SPECIAL)); break;
    case M_DRAW: draw_card(s, side * 2 + m.a); break;
    case M_DIPLO: advance_pol(s, m.a, 0); break;
    case M_MUSTER_ELITE: recruit(s, m.a, REGION[m.a].nation, 1); break;
    case M_MUSTER:
        do_muster(s, side, m.a, m.b);
        { Frame *f = push(s, FR_MUSTER2); f->v[0] = m.a; f->v[1] = (uint8_t)side; }
        break;
    case M_GANDALF_WHITE:
        s->gandalf_white = 1; place_comps(s, 1 << C_GANDALF, m.a); break;
    case M_ARAGORN:
        s->aragorn = 1; place_comps(s, 1 << C_STRIDER, s->comp_loc[C_STRIDER]); break;
    case M_COMP_POLITICS: advance_pol(s, m.a, 2); break;
    case M_DISCARD_TABLE:
        discard_card(s, m.a);
        if (side == FP) { if (m.b) { s->rings[FP]--; s->rings[SH]++; s->ring_used[FP] = 1; } }
        else {  /* costs one Strategy (Army) and one Character card from hand (first found) */
            for (int c = 48; c < 72; c++) if (s->card_where[c] == W_HAND) { discard_card(s, c); break; }
            for (int c = 72; c < 96; c++) if (s->card_where[c] == W_HAND && CARD[c].type == F_ARMY) { discard_card(s, c); break; }
        }
        break;
    case M_NAZGUL: push(s, FR_NAZMOVE); break;
    case M_MINION:
        s->minion_loc[m.a] = m.b; s->minion_seen = 1;
        if (m.a == M_WK) for (int n = 0; n < NFPNAT; n++)
            if (!(n == ROHAN && s->card_where[CARD_WORMTONGUE] == W_TABLE)) activate(s, n);
        break;
    case M_VOICE:
        if (m.a == 0) {
            static const uint8_t st[3] = {R_ORTHANC, R_NORTH_DUNLAND, R_SOUTH_DUNLAND};
            for (int i = 0; i < 3; i++) if (s->ctrl[st[i]] == SH && !units(s, FP, st[i])) recruit(s, st[i], ISENGARD, 0);
        } else for (int i = 0; i < 2; i++) if (s->reg[R_ORTHANC][ISENGARD] && s->pool_eli[ISENGARD]) {
            s->reg[R_ORTHANC][ISENGARD]--; s->pool_reg[ISENGARD]++; s->pool_eli[ISENGARD]--; s->eli[R_ORTHANC][ISENGARD]++;
        }
        break;
    default: break;
    }
}

void fr_actions(State *s, Frame *f) {
    CO_BEGIN
    f->v[0] = FP;
    for (;;) {
        if (!ndice(s, FP) && !ndice(s, SH)) break;
        if (!ndice(s, f->v[0])) f->v[0] ^= 1;
        ASK(D_ACTION, f->v[0]);
        if (s->mv.type == M_RING) {
            int side = f->v[0];
            s->dice[side][s->mv.a]--;
            if (side == SH && s->mv.b == F_SPECIAL) s->hunt_sh++; else s->dice[side][s->mv.b]++;
            s->rings[side]--; s->ring_used[side] = 1;
            if (side == FP) s->rings[SH]++;
            continue;
        }
        CALL(do_action(s, f->v[0]));
        /* the stacking limit after the action (recruitment may exceed it): the owner removes the excess, figure by
           figure, back to the reinforcements (RB p. 26-27) */
        for (f->v[1] = 0; f->v[1] < NREG; f->v[1]++) for (f->v[2] = 0; f->v[2] < 2; f->v[2]++) {
            while (!s->winner && units(s, f->v[2], f->v[1]) > ((s->siege[f->v[1]] && s->ctrl[f->v[1]] == f->v[2]) ? 5 : 10)) {
                int r = f->v[1], lo = f->v[2] == FP ? 0 : NFPNAT, hi = f->v[2] == FP ? NFPNAT : NNAT;
                s->nopt = 0;
                for (int n = lo; n < hi; n++) { if (s->reg[r][n]) s->opt[s->nopt++] = (uint8_t)n; if (s->eli[r][n]) s->opt[s->nopt++] = (uint8_t)(8 + n); }
                s->qreg = (uint8_t)r; s->qtag = Q_EXCESS;
                if (s->nopt > 1) ASK(D_CARD_OPTION, f->v[2]); else s->mv.a = s->opt[0];
                s->qtag = Q_NONE;
                { int r2 = f->v[1], n = s->mv.a & 7;
                  if (s->mv.a >= 8) { s->eli[r2][n]--; s->pool_eli[n]++; } else { s->reg[r2][n]--; s->pool_reg[n]++; } }
            }
        }
        s->lock[0] = s->lock[1] = 0; s->comp_moved = 0;
        if (s->hand_n[FP] > 6) CALL(push_discard(s, FP));
        if (s->hand_n[SH] > 6) CALL(push_discard(s, SH));
        f->v[0] ^= 1;
    }
    CO_END
}

void fr_army2(State *s, Frame *f) {
    CO_BEGIN
    ASK(D_ARMY_MOVE2, f->v[0]);
    if (s->mv.type == M_ARMY) move_army(s, f->v[0], s->mv.a, s->mv.b, s->mv.c, s->mv.d, s->mv.e, 0);
    CO_END
}
void fr_muster2(State *s, Frame *f) {
    CO_BEGIN
    ASK(D_MUSTER2, f->v[1]);
    if (s->mv.type == M_MUSTER) do_muster(s, f->v[1], s->mv.a, s->mv.b);
    CO_END
}
void fr_compmove(State *s, Frame *f) {
    CO_BEGIN
    for (;;) {
        { Move tmp[MAXMOVES]; if (!gen_comp_moves(s, tmp)) break; }
        ASK(D_COMP_MOVE, FP);
        if (s->mv.type != M_COMP_MOVE) break;
        place_comps(s, s->mv.a, s->mv.b);
        s->comp_moved |= s->mv.a;
    }
    CO_END
}
void fr_nazmove(State *s, Frame *f) {
    CO_BEGIN
    for (;;) {
        { Move tmp[MAXMOVES]; if (!gen_naz_moves(s, tmp)) break; }
        ASK(D_NAZ_MOVE, SH);
        if (s->mv.type == M_NAZ_PLAN) { apply_naz_plan(s, s->mv); break; }
        if (s->mv.type != M_NAZ_MOVE) break;
        do_naz_move(s, s->mv);
    }
    CO_END
}
