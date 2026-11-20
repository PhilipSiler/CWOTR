/* util.c -- printing and invariant checking */
#include <stdio.h>
#include "wotr.h"

static const char *FACE[2][NFACE] = {{"Char", "Army", "Muster", "Event", "MusArmy", "Will"}, {"Char", "Army", "Muster", "Event", "MusArmy", "Eye"}};
static const char *NATN[NNAT] = {"Dwarves", "Elves", "Gondor", "North", "Rohan", "Isengard", "Sauron", "Southron"};
static const char *COMPN[NCOMP + 1] = {"Gandalf", "Strider", "Boromir", "Legolas", "Gimli", "Merry", "Pippin", "Gollum"};
static const char *MINN[NMINION] = {"Witch-king", "Saruman", "Mouth"};
static const char *DK[ND] = {"none", "action", "discard", "declare", "guide", "enter-mordor", "hunt-alloc", "army-move2", "muster2",
    "comp-move", "naz-move", "sep-dest", "battle-card", "casualty", "continue", "retreat", "siege-retreat", "advance", "siege-extend",
    "self-hits", "hunt-response", "reveal-dest", "mithril", "staff", "card-region", "card-option", "card-nation", "card-card", "card-comp", "card-army"};
const char *wotr_dkind_name(int k) { return DK[k]; }

static int maskstr(int mask, char *b, int n) {
    int o = 0;
    for (int c = 0; c < NCOMP; c++) if ((mask >> c) & 1) o += snprintf(b + o, (size_t)(n - o), "%s%s", o ? "+" : "", COMPN[c]);
    return o;
}
#define RN(r) ((r) < NREG ? REGION[r].name : "-")
void wotr_move_str(const State *s, Move m, char *b, int n) {
    int side = s->dplayer, f = m.die & 0x7F;
    char t[96];
    const char *dn = FACE[side][f < NFACE ? f : 0];
    switch (m.type) {
    case M_CHOICE:
        switch (s->dkind) {
        case D_DISCARD: case D_CARD_CARD: snprintf(b, (size_t)n, "card %s", m.a < NCARD ? CARD[m.a].name : "none"); break;
        case D_BATTLE_CARD: snprintf(b, (size_t)n, "combat card %s", m.a < NCARD ? CC_NAME[CARD[m.a].combat] : "none"); break;
        case D_FSP_DECLARE: case D_SEP_DEST: case D_REVEAL_DEST: case D_RETREAT: case D_CARD_REGION: case D_CARD_ARMY:
            snprintf(b, (size_t)n, "%s -> %s", DK[s->dkind], m.a < NREG ? REGION[m.a].name : "no"); break;
        case D_GUIDE: snprintf(b, (size_t)n, "guide %s", COMPN[m.a]); break;
        case D_CARD_COMP: maskstr(m.a & 0x7F, t, sizeof t); snprintf(b, (size_t)n, "companions %s", t); break;
        case D_CASUALTY: snprintf(b, (size_t)n, "casualty %s %s", (m.a >> 3) == 0 ? "regular" : (m.a >> 3) == 1 ? "reduce elite" : "elite", NATN[m.a & 7]); break;
        default: snprintf(b, (size_t)n, "%s %d", DK[s->dkind], m.a); break;
        }
        break;
    case M_PASS: snprintf(b, (size_t)n, "pass"); break;
    case M_SKIP: snprintf(b, (size_t)n, "[%s] discard die", dn); break;
    case M_RING: snprintf(b, (size_t)n, "elven ring: %s -> %s", FACE[side][m.a], FACE[side][m.b]); break;
    case M_FSP_MOVE: snprintf(b, (size_t)n, "[%s] move Fellowship", dn); break;
    case M_FSP_HIDE: snprintf(b, (size_t)n, "[%s] hide Fellowship", dn); break;
    case M_SEPARATE: maskstr(m.a, t, sizeof t); snprintf(b, (size_t)n, "[%s] separate %s", dn, t); break;
    case M_COMPS: snprintf(b, (size_t)n, "[%s] move companions", dn); break;
    case M_ARMY: snprintf(b, (size_t)n, "[%s] army %s -> %s (%dR %dE%s)", s->dkind == D_ACTION ? dn : "+", RN(m.a), RN(m.b), m.c, m.d, m.e ? " +ldr" : ""); break;
    case M_ATTACK: snprintf(b, (size_t)n, "[%s] attack %s -> %s (%dR %dE)", dn, RN(m.a), RN(m.b), m.c, m.d); break;
    case M_PLAY: snprintf(b, (size_t)n, "[%s] play %s", dn, CARD[m.a].name); break;
    case M_DRAW: snprintf(b, (size_t)n, "[%s] draw %s card", dn, m.a ? "strategy" : "character"); break;
    case M_DIPLO: snprintf(b, (size_t)n, "[%s] diplomacy %s", dn, NATN[m.a]); break;
    case M_MUSTER_ELITE: snprintf(b, (size_t)n, "[%s] muster elite in %s", dn, RN(m.a)); break;
    case M_MUSTER: snprintf(b, (size_t)n, "[%s] muster %s in %s", s->dkind == D_ACTION ? dn : "+", m.b ? "leader" : "regular", RN(m.a)); break;
    case M_GANDALF_WHITE: snprintf(b, (size_t)n, "[Will] Gandalf the White in %s", RN(m.a)); break;
    case M_ARAGORN: snprintf(b, (size_t)n, "[Will] crown Aragorn"); break;
    case M_COMP_POLITICS: snprintf(b, (size_t)n, "[%s] companion advances %s", dn, NATN[m.a]); break;
    case M_DISCARD_TABLE: snprintf(b, (size_t)n, "[%s] force discard of %s", dn, CARD[m.a].name); break;
    case M_NAZGUL: snprintf(b, (size_t)n, "[%s] move Nazgul/minions", dn); break;
    case M_MINION: snprintf(b, (size_t)n, "[%s] bring %s into %s", dn, MINN[m.a], RN(m.b)); break;
    case M_VOICE: snprintf(b, (size_t)n, "[%s] Voice of Saruman (%s)", dn, m.a ? "elites" : "regulars"); break;
    case M_COMP_MOVE: maskstr(m.a, t, sizeof t); snprintf(b, (size_t)n, "  %s -> %s", t, RN(m.b)); break;
    case M_NAZ_MOVE: snprintf(b, (size_t)n, "  %s %s -> %s", m.d == 1 ? "Witch-king" : m.d == 2 ? "Mouth" : m.c > 1 ? "Nazgul(all)" : "Nazgul", RN(m.a), RN(m.b)); break;
    case M_DONE: snprintf(b, (size_t)n, "  done"); break;
    case M_NAZ_PLAN: snprintf(b, (size_t)n, "  Nazgul to %s%s%s%s", RN(m.a), m.b < NREG ? " then " : "", m.b < NREG ? REGION[m.b].name : "", m.c ? " +1 hunting the Fellowship" : ""); break;
    default: snprintf(b, (size_t)n, "?"); break;
    }
}

void wotr_print(const State *s) {
    printf("=== turn %d | FP vp %d  SH vp %d | corruption %d  progress %d%s  at %s  mordor %d  guide %s | hunt box sh %d fp %d\n",
           s->turn, wotr_vp(s, FP), wotr_vp(s, SH), s->corruption, s->progress, s->revealed ? " REVEALED" : "", REGION[s->fsp_loc].name,
           s->mordor, COMPN[s->guide], s->hunt_sh, s->hunt_fp);
    printf("    companions:");
    for (int c = 0; c < NCOMP; c++) printf(" %s=%s", COMPN[c], s->comp_loc[c] == LOC_FSP ? "fellowship" : s->comp_loc[c] == LOC_DEAD ? "dead" : s->comp_loc[c] < NREG ? "map" : "-");
    printf(" | Gandalf the White %s, Aragorn %s, a minion has entered: %s\n", s->gandalf_white ? "yes" : "no", s->aragorn ? "yes" : "no", s->minion_seen ? "yes" : "no");
    printf("    politics:");
    for (int n = 0; n < NNAT; n++) printf(" %s %d%s", NATN[n], s->pol[n], s->active[n] ? "" : "(passive)");
    printf("\n    dice FP:");
    for (int f = 0; f < NFACE; f++) if (s->dice[FP][f]) printf(" %s x%d", FACE[FP][f], s->dice[FP][f]);
    printf(" | SH:");
    for (int f = 0; f < NFACE; f++) if (s->dice[SH][f]) printf(" %s x%d", FACE[SH][f], s->dice[SH][f]);
    printf("\n");
    for (int r = 0; r < NREG; r++) {
        int fu = units(s, FP, r), su = units(s, SH, r), cm = comps_at(s, r), mm = minions_at(s, r);
        int flip = is_settlement(r) && s->ctrl[r] != orig_side(r);
        if (!fu && !su && !cm && !mm && !s->naz[r] && !flip) continue;
        printf("    %-22s", REGION[r].name);
        if (flip) printf(" [held by %s]", s->ctrl[r] == FP ? "FP" : "SH");
        if (s->siege[r]) printf(" [SIEGE]");
        for (int n = 0; n < NNAT; n++) if (s->reg[r][n] + s->eli[r][n]) printf(" %s %dR/%dE", NATN[n], s->reg[r][n], s->eli[r][n]);
        if (fp_leaders(s, r)) printf(" ldr x%d", fp_leaders(s, r));
        if (s->naz[r]) printf(" Nazgul x%d", s->naz[r]);
        for (int c = 0; c < NCOMP; c++) if ((cm >> c) & 1) printf(" %s", c == C_STRIDER && s->aragorn ? "Aragorn" : c == C_GANDALF && s->gandalf_white ? "Gandalf-White" : COMPN[c]);
        for (int m = 0; m < NMINION; m++) if ((mm >> m) & 1) printf(" %s", MINN[m]);
        printf("\n");
    }
}

#define FAIL(...) do { snprintf(err, (size_t)n, __VA_ARGS__); return 1; } while (0)
int wotr_check(const State *s, char *err, int n) {
    static int tot_reg[NNAT], tot_eli[NNAT], have = 0;
    if (!have) {
        State t; wotr_init(&t, 1);
        for (int k = 0; k < NNAT; k++) {
            tot_reg[k] = t.pool_reg[k]; tot_eli[k] = t.pool_eli[k];
            for (int r = 0; r < NREG; r++) { tot_reg[k] += t.reg[r][k]; tot_eli[k] += t.eli[r][k]; }
        }
        have = 1;
    }
    /* recruiting may exceed the stacking limit until the action ends, when the owner removes the excess (RB p. 26-27):
       the limits hold between actions and phases */
    int between = s->dkind == D_ACTION || s->dkind == D_FSP_DECLARE || s->dkind == D_HUNT_ALLOC;
    int nz = s->pool_naz, hn[2] = {0, 0};
    for (int k = 0; k < NNAT; k++) {
        int a = s->pool_reg[k], e = s->pool_eli[k];
        for (int r = 0; r < NREG; r++) { a += s->reg[r][k]; e += s->eli[r][k]; }
        if (k < NFPNAT) a += s->cas_reg[k];
        if (!s->b.active) {
            if (k >= NFPNAT ? (a != tot_reg[k] || e != tot_eli[k]) : (a > tot_reg[k] + tot_eli[k] || e > tot_eli[k]))
                FAIL("unit conservation %s: reg %d/%d eli %d/%d", NATN[k], a, tot_reg[k], e, tot_eli[k]);
        }
    }
    for (int r = 0; r < NREG; r++) {
        int fu = units(s, FP, r), su = units(s, SH, r);
        nz += s->naz[r];
        if (s->b.active) continue;
        if (fu && su && !s->siege[r]) FAIL("both sides in %s without siege", REGION[r].name);
        if (s->siege[r]) {
            if (REGION[r].settle != S_STRONGHOLD) FAIL("siege outside stronghold %s", REGION[r].name);
            if (!fu || !su) FAIL("siege with a side missing in %s", REGION[r].name);
            if (units(s, s->ctrl[r], r) > 5 && between) FAIL("more than 5 besieged in %s", REGION[r].name);
        }
        if ((fu > 10 || su > 10) && between) FAIL("stacking in %s (%d/%d)", REGION[r].name, fu, su);
        if (fp_leaders(s, r) && !fu) FAIL("FP leaders without army in %s", REGION[r].name);
        if (is_settlement(r) && s->ctrl[r] > 1) FAIL("bad ctrl in %s", REGION[r].name);
        if (is_settlement(r) && !s->siege[r] && units(s, 1 - s->ctrl[r], r)) FAIL("enemy army sits in unconquered %s", REGION[r].name);
    }
    if (nz != 8) FAIL("nazgul count %d", nz);
    for (int c = 0; c < NCARD; c++) if (s->card_where[c] == W_HAND) hn[CARD_SIDE(c)]++;
    if (hn[0] != s->hand_n[0] || hn[1] != s->hand_n[1]) FAIL("hand count mismatch");
    if (s->dkind == D_ACTION && (hn[0] > 6 || hn[1] > 6)) FAIL("hand over limit at action");
    if (s->corruption > 12 || s->progress > 12 || s->mordor > 5) FAIL("track overflow");
    for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] >= NREG && s->comp_loc[c] != LOC_FSP && s->comp_loc[c] != LOC_DEAD) FAIL("bad companion loc");
    int fc = fsp_comps(s);
    if (fc ? (s->guide >= NCOMP || !((fc >> s->guide) & 1)) : s->guide != GOLLUM) FAIL("bad guide %d (fsp mask %x)", s->guide, fc);
    for (int t = 0; t < NSTD_TILE; t++) { static const int T0[NSTD_TILE] = {4, 3, 2, 1, 2, 2, 2}; if (s->tile_pool[t] + s->tile_out[t] > T0[t]) FAIL("tile %d overflow", t); }
    if (s->fsp_loc >= NREG) FAIL("fsp loc");
    if (s->sp < 1 || s->sp > MAXSTACK) FAIL("stack");
    if (!s->winner && s->dkind == D_NONE) FAIL("no decision and no winner");
    return 0;
}

/* ---- positions from outside (human game logs, another engine) ----
   wotr_blank gives an empty board resting at an action decision of turn 1; wotr_set applies one line of the position
   language (the lines tools/humanlogs/positions.py and tools/oracle/compare.py write); wotr_begin_turn turns a board
   as it stood at the END of a turn into a position at the first decision of the next one, ready to be played on. */
void wotr_blank(State *s) {
    wotr_init(s, 1);
    while (s->dkind != D_ACTION) { Move mv[MAXMOVES]; wotr_moves(s, mv); wotr_apply(s, mv[0]); }
    memset(s->reg, 0, sizeof s->reg); memset(s->eli, 0, sizeof s->eli); memset(s->ldr, 0, sizeof s->ldr);
    memset(s->naz, 0, sizeof s->naz); memset(s->siege, 0, sizeof s->siege); memset(s->dice, 0, sizeof s->dice);
    for (int r = 0; r < NREG; r++) s->ctrl[r] = (uint8_t)orig_side(r);
    for (int c = 0; c < NCARD; c++) s->card_where[c] = W_DECK;
    s->hand_n[0] = s->hand_n[1] = 0; s->lock[0] = s->lock[1] = 0; s->comp_moved = 0;
}
int wotr_set(State *s, const char *line) {
    int a = 0, b = 0, c = 0, d = 0; char k[16];
    if (sscanf(line, "%15s %d %d %d %d", k, &a, &b, &c, &d) < 1) return 0;
    if (!strcmp(k, "u")) { s->reg[a][b] = (uint8_t)c; s->eli[a][b] = (uint8_t)d; }
    else if (!strcmp(k, "l")) s->ldr[a][b] = (uint8_t)c;
    else if (!strcmp(k, "z")) s->naz[a] = (uint8_t)b;
    else if (!strcmp(k, "c")) s->ctrl[a] = (uint8_t)b;
    else if (!strcmp(k, "g")) s->siege[a] = 1;
    else if (!strcmp(k, "comp")) s->comp_loc[a] = (uint8_t)b;
    else if (!strcmp(k, "min")) s->minion_loc[a] = (uint8_t)b;
    else if (!strcmp(k, "flags")) { s->gandalf_white = (uint8_t)a; s->aragorn = (uint8_t)b; s->minion_seen = (uint8_t)c; s->mouth_used = (uint8_t)d; }
    else if (!strcmp(k, "pool")) { s->pool_reg[a] = (uint8_t)b; s->pool_eli[a] = (uint8_t)c; if (a < NFPNAT) s->pool_ldr[a] = (uint8_t)d; else if (a == SAURON) s->pool_naz = (uint8_t)d; }
    else if (!strcmp(k, "pol")) { s->pol[a] = (uint8_t)b; s->active[a] = (uint8_t)c; }
    else if (!strcmp(k, "fsp")) { s->fsp_loc = (uint8_t)a; s->progress = (uint8_t)b; s->revealed = (uint8_t)c; s->corruption = (uint8_t)d; }
    else if (!strcmp(k, "fsp2")) { s->guide = (uint8_t)a; s->mordor = (int8_t)b; s->fsp_acted = (uint8_t)c; }
    else if (!strcmp(k, "hunt")) { s->hunt_sh = (uint8_t)a; s->hunt_fp = (uint8_t)b; }
    else if (!strcmp(k, "die")) s->dice[a][b] = (uint8_t)c;
    else if (!strcmp(k, "ring")) { s->rings[a] = (uint8_t)b; s->ring_used[a] = (uint8_t)c; }
    else if (!strcmp(k, "card")) { s->card_where[a] = (uint8_t)b; if (b == W_HAND) s->hand_n[CARD_SIDE(a)]++; }
    else if (!strcmp(k, "turn")) s->turn = (uint16_t)a;
    else return 0;
    return 1;
}
/* s: a blank position filled with the board at the end of turn s->turn - 1 (the logs give "turn" as the one about to
   start). What the logs do not show is filled in: Shadow's reinforcements are everything not on the board (its dead
   return to the pool); the Free Peoples' are what they started with, less if more than that is already on the board;
   hands start empty and the turn's draw follows. Returns 0 if the position passes wotr_check and the game goes on. */
#define NO(msg) do { snprintf(err, (size_t)en, "%s", msg); return 1; } while (0)
static int fill_unseen(State *s, char *err, int en) {
    State t; wotr_init(&t, 1);
    int naz = 8;
    for (int k = 0; k < NNAT; k++) {
        int tr = t.pool_reg[k], te = t.pool_eli[k], br = 0, be = 0;
        for (int r = 0; r < NREG; r++) { tr += t.reg[r][k]; te += t.eli[r][k]; br += s->reg[r][k]; be += s->eli[r][k]; }
        if (br > tr + (k < NFPNAT ? te : 0) || be > te) NO("more units of a nation on the board than exist");
        if (k >= NFPNAT) { if (br > tr) NO("more Shadow regulars on the board than exist"); s->pool_reg[k] = (uint8_t)(tr - br); s->pool_eli[k] = (uint8_t)(te - be); }
        else {
            int pr = tr - br, pe = te - be;
            s->pool_reg[k] = (uint8_t)(pr < 0 ? 0 : pr < t.pool_reg[k] ? pr : t.pool_reg[k]);
            s->pool_eli[k] = (uint8_t)(pe < t.pool_eli[k] ? pe : t.pool_eli[k]);
            s->cas_reg[k] = 0;
            int tl = t.pool_ldr[k], bl = 0;
            for (int r = 0; r < NREG; r++) { tl += t.ldr[r][k]; bl += s->ldr[r][k]; }
            if (bl > tl) NO("more leaders on the board than exist");
            s->pool_ldr[k] = (uint8_t)(tl - bl < t.pool_ldr[k] ? tl - bl : t.pool_ldr[k]);
        }
    }
    for (int r = 0; r < NREG; r++) naz -= s->naz[r];
    if (naz < 0) NO("more than eight Nazgul");
    s->pool_naz = (uint8_t)naz;
    for (int r = 0; r < NREG; r++) if (fp_leaders(s, r) && !units(s, FP, r)) memset(s->ldr[r], 0, sizeof s->ldr[r]);
    /* an army standing alone in an enemy settlement holds it (players often leave the control marker off towns) */
    for (int r = 0; r < NREG; r++) if (is_settlement(r) && !s->siege[r] && s->ctrl[r] <= 1 && units(s, 1 - s->ctrl[r], r) && !units(s, s->ctrl[r], r)) s->ctrl[r] ^= 1;
    return 0;
}
int wotr_begin_turn(State *s, uint64_t seed, char *err, int en) {
    if (fill_unseen(s, err, en)) return 1;
    if (s->turn < 2) NO("no turn has been played");
    s->turn--;                                   /* the turn that has just ended; the engine counts up as the next begins */
    s->rng = seed; s->hunt_fp = 1; s->hunt_sh = 0; s->fsp_acted = 1;
    memset(s->dice, 0, sizeof s->dice); s->dice[FP][F_MUSTER] = 1;
    s->dplayer = FP; s->st[s->sp - 1].v[0] = FP;
    if (wotr_check(s, err, en)) return 1;
    Move mv[MAXMOVES]; int n = wotr_moves(s, mv), k = -1;
    for (int i = 0; i < n; i++) if (mv[i].type == M_SKIP) k = i;         /* throw the one die away: the turn ends and the next begins */
    if (k < 0) NO("cannot end the turn");
    wotr_apply(s, mv[k]);
    if (s->winner) NO("the game is over at the end of that turn");
    if (wotr_check(s, err, en)) return 1;
    return 0;
}
/* s: a blank position filled with the board in the MIDDLE of turn s->turn, with both sides' unspent dice and the Hunt
   box set by the caller; `side` is about to spend an action die. Hands are empty (the logs do not show them). */
int wotr_mid_turn(State *s, int side, uint64_t seed, char *err, int en) {
    if (fill_unseen(s, err, en)) return 1;
    if (!s->dice[side][F_CHAR] && !s->dice[side][F_ARMY] && !s->dice[side][F_MUSTER] && !s->dice[side][F_EVENT] && !s->dice[side][F_MA] && !s->dice[side][F_SPECIAL]) NO("no die to spend");
    s->rng = seed; s->dplayer = (uint8_t)side; s->st[s->sp - 1].v[0] = side;
    return wotr_check(s, err, en);
}
#undef NO
