/* test_rules.c -- scenario and statistical tests of specific rules */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "wotr.h"
const char *wotr_dkind_name(int k);

static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(x, y, tol, name) CHECK(fabs((x) - (y)) <= (tol), "%s: got %.4f expected %.4f", name, (double)(x), (double)(y))
static Move mv[MAXMOVES];

static Move choice(int a) { Move m = {M_CHOICE, 0, (uint8_t)a, 0, 0, 0, 0, 0}; return m; }
static int has_choice(State *s, int a) { int n = wotr_moves(s, mv); for (int i = 0; i < n; i++) if (mv[i].type == M_CHOICE && mv[i].a == a) return 1; return 0; }
static int find(State *s, int type, int a, int b, Move *out) {   /* a/b = -1 wildcard */
    int n = wotr_moves(s, mv);
    for (int i = 0; i < n; i++) if (mv[i].type == type && (a < 0 || mv[i].a == a) && (b < 0 || mv[i].b == b)) { if (out) *out = mv[i]; return 1; }
    return 0;
}
/* answer set-up decisions with defaults until the first action decision of the turn */
static void to_action(State *s) {
    while (!s->winner && s->dkind != D_ACTION) { wotr_moves(s, mv); wotr_apply(s, mv[0]); }
}
static void fresh(State *s, uint64_t seed) { wotr_init(s, seed); to_action(s); }
static void set_dice(State *s, int side, int ch, int ar, int mu, int ev, int ma, int sp) {
    uint8_t d[NFACE] = {(uint8_t)ch, (uint8_t)ar, (uint8_t)mu, (uint8_t)ev, (uint8_t)ma, (uint8_t)sp};
    memcpy(s->dice[side], d, NFACE);
}
/* make it Shadow's action: FP passes with fewer dice */
static void shadow_to_act(State *s) {
    set_dice(s, FP, 0, 0, 1, 0, 0, 0);
    if (s->dice[SH][F_CHAR] + s->dice[SH][F_ARMY] + s->dice[SH][F_MUSTER] + s->dice[SH][F_EVENT] + s->dice[SH][F_MA] < 2) set_dice(s, SH, 2, 2, 2, 1, 1, 0);
    Move m; if (find(s, M_PASS, -1, -1, &m)) wotr_apply(s, m);
}
static void clear_region(State *s, int r) {
    for (int n = 0; n < NNAT; n++) { s->pool_reg[n] += s->reg[r][n]; s->pool_eli[n] += s->eli[r][n]; s->reg[r][n] = s->eli[r][n] = 0; }
    for (int n = 0; n < NFPNAT; n++) { s->pool_ldr[n] += s->ldr[r][n]; s->ldr[r][n] = 0; }
    s->pool_naz += s->naz[r]; s->naz[r] = 0;
}
static void put(State *s, int r, int n, int reg, int eli) { s->reg[r][n] += (uint8_t)reg; s->eli[r][n] += (uint8_t)eli; s->pool_reg[n] -= (uint8_t)reg; s->pool_eli[n] -= (uint8_t)eli; }
static int tiles_in_pool(const State *s) { int t = 0; for (int i = 0; i < NTILE; i++) t += s->tile_pool[i]; return t; }

static void test_setup(void) {
    printf("setup\n");
    State s; wotr_init(&s, 7);
    int ureg[NNAT] = {0}, ueli[NNAT] = {0}, ldr = 0, naz = 0;
    for (int r = 0; r < NREG; r++) { for (int n = 0; n < NNAT; n++) { ureg[n] += s.reg[r][n]; ueli[n] += s.eli[r][n]; } ldr += fp_leaders(&s, r); naz += s.naz[r]; }
    /* on-board totals from rulebook p.16-17 */
    static const int R0[NNAT] = {3, 3, 9, 4, 4, 6, 28, 14}, E0[NNAT] = {2, 6, 1, 1, 1, 1, 2, 3};
    for (int n = 0; n < NNAT; n++) CHECK(ureg[n] == R0[n] && ueli[n] == E0[n], "nation %d on board %dR %dE, want %dR %dE", n, ureg[n], ueli[n], R0[n], E0[n]);
    CHECK(ldr == 8 && naz == 4, "leaders %d nazgul %d", ldr, naz);
    CHECK(s.dkind == D_GUIDE && s.dplayer == FP, "first decision should be the guide choice (Gandalf/Strider tie), got %s", wotr_dkind_name(s.dkind));
    CHECK(s.hand_n[FP] == 2 && s.hand_n[SH] == 2, "opening hands %d %d", s.hand_n[FP], s.hand_n[SH]);
    CHECK(wotr_vp(&s, FP) == 0 && wotr_vp(&s, SH) == 0, "vp");
    int sh_vp_total = 0, fp_vp_total = 0;
    for (int r = 0; r < NREG; r++) { int t = REGION[r].settle, v = t == S_CITY ? 1 : t == S_STRONGHOLD ? 2 : 0; if (orig_side(r) == FP) sh_vp_total += v; else if (orig_side(r) == SH) fp_vp_total += v; }
    CHECK(sh_vp_total == 20 && fp_vp_total == 18, "available VP: shadow can take %d, FP can take %d", sh_vp_total, fp_vp_total);
    to_action(&s);
    int fd = 0, sd = 0;
    for (int f = 0; f < NFACE; f++) { fd += s.dice[FP][f]; sd += s.dice[SH][f]; }
    CHECK(fd == 4 && sd + s.hunt_sh == 7, "dice pools fp %d sh %d + hunt %d", fd, sd, s.hunt_sh);
    CHECK(s.dplayer == FP, "FP acts first");
    CHECK(DIST[R_RIVENDELL][R_MORANNON] == 10, "Rivendell-Morannon distance %d (strategy guide: 10)", DIST[R_RIVENDELL][R_MORANNON]);
}

/* P(hunt success) vs the table in strategy.txt */
static double hunt_rate(int sh_dice, int fp_dice, int rerolls, int trials) {
    int hit = 0;
    for (int i = 0; i < trials; i++) {
        State s; fresh(&s, 1000 + (uint64_t)i);
        set_dice(&s, FP, 1, 0, 0, 0, 0, 0);
        s.hunt_sh = (uint8_t)sh_dice; s.hunt_fp = (uint8_t)fp_dice;
        if (rerolls >= 1) s.naz[s.fsp_loc] = 1;
        if (rerolls >= 2) s.reg[s.fsp_loc][SAURON] = 1;
        int before = tiles_in_pool(&s);
        Move m; find(&s, M_FSP_MOVE, -1, -1, &m); wotr_apply(&s, m);
        hit += tiles_in_pool(&s) < before;
    }
    return (double)hit / trials;
}
static void test_hunt_odds(void) {
    printf("hunt odds\n");
    int T = 20000; double tol = 0.012;
    NEAR(hunt_rate(1, 0, 0, T), 1.0 / 6, tol, "1 die, first move");
    NEAR(hunt_rate(2, 0, 0, T), 0.3056, tol, "2 dice");
    NEAR(hunt_rate(3, 0, 0, T), 0.4213, tol, "3 dice");
    NEAR(hunt_rate(5, 0, 0, T), 0.5981, tol, "5 dice");
    NEAR(hunt_rate(7, 0, 0, T), 0.5981, tol, "7 dice roll only 5");
    NEAR(hunt_rate(1, 0, 1, T), 0.3056, tol, "1 die + 1 reroll");
    NEAR(hunt_rate(2, 0, 2, T), 0.5177, tol, "2 dice + 2 rerolls");
    NEAR(hunt_rate(1, 1, 0, T), 1.0 / 3, tol, "second move, 1 die");
    NEAR(hunt_rate(3, 1, 0, T), 0.7037, tol, "second move, 3 dice");
    NEAR(hunt_rate(2, 2, 0, T), 0.75, tol, "third move, 2 dice");
    NEAR(hunt_rate(1, 5, 0, T), 5.0 / 6, tol, "sixth move: a 1 still fails");
    NEAR(hunt_rate(0, 0, 0, 2000), 0.0, 1e-9, "empty hunt box never hunts");
}

static void test_fellowship(void) {
    printf("fellowship\n");
    State s; fresh(&s, 3);
    set_dice(&s, FP, 2, 0, 0, 0, 0, 0); s.hunt_sh = 0;
    Move m; find(&s, M_FSP_MOVE, -1, -1, &m); wotr_apply(&s, m);
    CHECK(s.progress == 1 && s.hunt_fp == 1, "progress %d hunt_fp %d", s.progress, s.hunt_fp);
    /* revealed fellowship cannot move, can hide */
    State t = s; while (t.dkind != D_ACTION || t.dplayer != FP) { wotr_moves(&t, mv); wotr_apply(&t, mv[0]); }
    set_dice(&t, FP, 1, 0, 0, 0, 0, 0); t.revealed = 1;
    CHECK(!find(&t, M_FSP_MOVE, -1, -1, 0) && find(&t, M_FSP_HIDE, -1, -1, &m), "revealed: no move, hide available");
    wotr_apply(&t, m);
    CHECK(!t.revealed, "hidden again");
    /* Strider as guide hides with any die */
    fresh(&t, 4); t.guide = C_STRIDER; t.revealed = 1; set_dice(&t, FP, 0, 0, 1, 0, 0, 0);
    CHECK(find(&t, M_FSP_HIDE, -1, -1, 0), "Strider hides with a muster die");
    t.guide = C_GANDALF;
    CHECK(!find(&t, M_FSP_HIDE, -1, -1, 0), "Gandalf cannot");
    /* declare in Lorien heals and activates nothing new (elves already active) */
    fresh(&t, 5);
    t.progress = 5; t.corruption = 3; t.dice[FP][F_CHAR] = 0; set_dice(&t, FP, 0, 0, 0, 0, 0, 0); set_dice(&t, SH, 0, 0, 0, 0, 0, 0);
    wotr_moves(&t, mv);   /* no dice: flow continues to next turn's fellowship phase on any apply; force it */
    t.dice[FP][F_MUSTER] = 1; Move sk; find(&t, M_SKIP, -1, -1, &sk); wotr_apply(&t, sk);
    while (t.dkind != D_FSP_DECLARE && !t.winner) { wotr_moves(&t, mv); wotr_apply(&t, mv[0]); }
    CHECK(has_choice(&t, R_LORIEN) && has_choice(&t, R_MORIA) && !has_choice(&t, R_FANGORN), "declare range 5 from Rivendell reaches Lorien, not Fangorn");
    wotr_apply(&t, choice(R_LORIEN));
    CHECK(t.fsp_loc == R_LORIEN && t.progress == 0 && t.corruption == 2 && !t.revealed, "declared in Lorien: loc %d prog %d corr %d", t.fsp_loc, t.progress, t.corruption);
    /* Mordor: needs to stand at Morannon/Minas Morgul in the fellowship phase */
    fresh(&t, 6); t.fsp_loc = R_MORANNON; t.progress = 0; set_dice(&t, FP, 0, 0, 1, 0, 0, 0); set_dice(&t, SH, 0, 0, 0, 0, 0, 0);
    find(&t, M_SKIP, -1, -1, &sk); wotr_apply(&t, sk);
    while (t.dkind != D_ENTER_MORDOR && !t.winner && t.turn < 3) { wotr_moves(&t, mv); wotr_apply(&t, mv[0]); }
    CHECK(t.dkind == D_ENTER_MORDOR, "offered to enter Mordor");
    wotr_apply(&t, choice(1));
    CHECK(t.mordor == 0, "on the Mordor track");
    to_action(&t);
    /* no move this turn => +1 corruption at end of turn */
    int c0 = t.corruption; set_dice(&t, FP, 0, 0, 1, 0, 0, 0); set_dice(&t, SH, 0, 0, 0, 0, 0, 0);
    find(&t, M_SKIP, -1, -1, &sk); wotr_apply(&t, sk);
    CHECK(t.corruption == c0 + 1, "idle in Mordor costs a corruption (%d -> %d)", c0, t.corruption);
    /* five clean steps win */
    to_action(&t);
    for (int step = 0; step < 5 && !t.winner; step++) {
        memset(t.tile_pool, 0, NTILE); t.tile_pool[T_1] = 1; memset(t.tile_out, 0, NTILE);   /* only a plain "1" tile */
        t.corruption = 0; t.revealed = 0; set_dice(&t, FP, 1, 0, 0, 0, 0, 0);
        while (t.dkind != D_ACTION || t.dplayer != FP) { wotr_moves(&t, mv); wotr_apply(&t, mv[0]); if (t.winner) break; }
        if (t.winner) break;
        t.revealed = 0; set_dice(&t, FP, 1, 0, 0, 0, 0, 0);
        find(&t, M_FSP_MOVE, -1, -1, &m); wotr_apply(&t, m);
        while (!t.winner && t.dkind == D_HUNT_RESPONSE) wotr_apply(&t, choice(0));
    }
    CHECK(t.winner == 1 && t.win_type == WIN_RING, "ring destroyed after 5 steps (winner %d mordor %d)", t.winner, t.mordor);
    /* corruption 12 loses at once */
    fresh(&t, 8); t.corruption = 11; set_dice(&t, FP, 1, 0, 0, 0, 0, 0); t.hunt_sh = 5; t.hunt_fp = 5;
    memset(t.tile_pool, 0, NTILE); t.tile_pool[T_3] = 3; memset(t.tile_out, 0, NTILE);
    find(&t, M_FSP_MOVE, -1, -1, &m);
    for (int i = 0; i < 50 && !t.winner; i++) { State u = t; u.rng += (uint64_t)i * 77; wotr_apply(&u, m); while (!u.winner && u.dkind == D_HUNT_RESPONSE) wotr_apply(&u, choice(0)); if (u.winner) t = u; }
    CHECK(t.winner == 2 && t.win_type == WIN_CORRUPTION, "corruption victory");
}

static void test_hunt_resolution(void) {
    printf("hunt resolution\n");
    State s; Move m;
    /* guide casualty absorbs up to his level */
    fresh(&s, 11); set_dice(&s, FP, 1, 0, 0, 0, 0, 0); s.hunt_sh = 5; s.hunt_fp = 5;
    memset(s.tile_pool, 0, NTILE); s.tile_pool[T_3] = 3; memset(s.tile_out, 0, NTILE);
    find(&s, M_FSP_MOVE, -1, -1, &m);
    State u; int ok = 0;
    for (int i = 0; i < 50 && !ok; i++) { u = s; u.rng += (uint64_t)i * 31; wotr_apply(&u, m); ok = u.dkind == D_HUNT_RESPONSE; }
    CHECK(ok, "hunt succeeded and asks FP");
    CHECK(has_choice(&u, 0) && has_choice(&u, 4) && has_choice(&u, 5) && !has_choice(&u, 3), "options: ring, guide casualty, random casualty; no hobbit ability for Gandalf");
    State v = u; wotr_apply(&v, choice(4));
    CHECK(v.comp_loc[C_GANDALF] == LOC_DEAD && v.corruption == 0 && v.guide == C_STRIDER, "Gandalf absorbs a 3 (corr %d guide %d)", v.corruption, v.guide);
    v = u; wotr_apply(&v, choice(0));
    CHECK(v.corruption == 3, "ring takes 3");
    /* hobbit guide: separate to reduce by one */
    v = s; for (int c = 0; c < C_MERRY; c++) v.comp_loc[c] = LOC_DEAD;
    v.guide = C_MERRY; ok = 0;
    { State w; for (int i = 0; i < 50 && !ok; i++) { w = v; w.rng += (uint64_t)i * 31; wotr_apply(&w, m); ok = w.dkind == D_HUNT_RESPONSE; } v = w; }
    CHECK(ok && has_choice(&v, 3), "Merry may separate");
    wotr_apply(&v, choice(3));
    if (v.dkind == D_SEP_DEST) wotr_apply(&v, choice(v.fsp_loc));
    CHECK(v.comp_loc[C_MERRY] < NREG && v.guide == C_PIPPIN, "Merry on the map, Pippin guides");
    CHECK(v.dkind == D_HUNT_RESPONSE && has_choice(&v, 3), "Pippin may then use his ability too");
    /* eye tile = number of successes, and reveals */
    fresh(&s, 12); set_dice(&s, FP, 1, 0, 0, 0, 0, 0); s.hunt_sh = 5; s.hunt_fp = 5; s.progress = 0;
    memset(s.tile_pool, 0, NTILE); s.tile_pool[T_EYE] = 4; memset(s.tile_out, 0, NTILE);
    find(&s, M_FSP_MOVE, -1, -1, &m);
    int tot = 0, n = 0, rev = 0;
    for (int i = 0; i < 400; i++) {
        u = s; u.rng += (uint64_t)i * 13; wotr_apply(&u, m);
        while (u.dkind == D_HUNT_RESPONSE) wotr_apply(&u, choice(0));
        while (u.dkind == D_REVEAL_DEST) { wotr_moves(&u, mv); wotr_apply(&u, mv[0]); }
        while (u.dkind == D_HUNT_RESPONSE) wotr_apply(&u, choice(0));
        if (u.corruption) { tot += u.corruption; n++; rev += u.revealed; }
    }
    NEAR((double)tot / n, 5 * 5.0 / 6, 0.25, "eye damage = successes (5 dice hitting on 2+)");
    CHECK(rev == n, "eye tile reveals (%d/%d)", rev, n);
    /* revealed fellowship may not stop in a free city/stronghold */
    fresh(&s, 13); s.progress = 3; s.revealed = 0; set_dice(&s, FP, 1, 0, 0, 0, 0, 0); s.hunt_sh = 5; s.hunt_fp = 5;
    memset(s.tile_pool, 0, NTILE); s.tile_pool[T_0R] = 2; memset(s.tile_out, 0, NTILE);
    find(&s, M_FSP_MOVE, -1, -1, &m); ok = 0;
    for (int i = 0; i < 50 && !ok; i++) { u = s; u.rng += (uint64_t)i; wotr_apply(&u, m); ok = u.dkind == D_REVEAL_DEST; }
    CHECK(ok, "0-reveal tile asks for a destination");
    CHECK(!has_choice(&u, R_RIVENDELL) && has_choice(&u, R_MORIA) && has_choice(&u, R_GOBLINS_GATE), "cannot be revealed in Rivendell; Moria and Goblin's Gate ok");
    v = u; int p0 = tiles_in_pool(&v); wotr_apply(&v, choice(R_GOBLINS_GATE));
    CHECK(v.revealed && v.fsp_loc == R_GOBLINS_GATE && v.progress == 0 && tiles_in_pool(&v) == p0, "revealed at Goblin's Gate, no extra tile");
    v = u; v.tile_pool[T_1] = 1; p0 = tiles_in_pool(&v); wotr_apply(&v, choice(R_MORIA));
    while (v.dkind == D_HUNT_RESPONSE) wotr_apply(&v, choice(0));
    CHECK(tiles_in_pool(&v) == p0 - 1, "revealed in Moria draws an extra tile");
}

/* average hits per round for a given matchup */
static void battle_stats(int att_units, int att_ldr, int def_units, int def_ldr, int dst, int trials, double *ah, double *dh) {
    long a = 0, d = 0;
    for (int i = 0; i < trials; i++) {
        State s; fresh(&s, 500 + (uint64_t)i);
        for (int c = 0; c < NCARD; c++) if (s.card_where[c] == W_HAND) { s.card_where[c] = W_DISCARD; s.hand_n[CARD_SIDE(c)]--; }
        shadow_to_act(&s);
        int src = REGION[dst].adj[0];
        clear_region(&s, src); clear_region(&s, dst);
        s.pol[SAURON] = 0; s.pol[GONDOR] = 0; s.active[GONDOR] = 1;
        put(&s, src, SAURON, att_units, 0); s.naz[src] = (uint8_t)att_ldr; s.pool_naz -= (uint8_t)att_ldr;
        put(&s, dst, GONDOR, def_units, 0); s.ldr[dst][GONDOR] = (uint8_t)def_ldr;
        set_dice(&s, SH, 0, 1, 0, 0, 0, 0);
        Move m; if (!find(&s, M_ATTACK, src, dst, &m)) { printf("  no attack move!\n"); fails++; return; }
        int a0 = units(&s, SH, src), d0 = units(&s, FP, dst);
        wotr_apply(&s, m);
        if (s.dkind == D_SIEGE_RETREAT) wotr_apply(&s, choice(0));
        while (s.dkind == D_CASUALTY) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
        a += d0 - units(&s, FP, dst); d += a0 - units(&s, SH, src);
    }
    *ah = (double)a / trials; *dh = (double)d / trials;
}
static void test_combat_odds(void) {
    printf("combat odds\n");
    double a, d; int T = 6000;
    battle_stats(5, 0, 5, 0, R_LOSSARNACH, T, &a, &d);             /* town: plain field battle */
    NEAR(a, 5.0 / 3, 0.06, "5 dice hit on 5+"); NEAR(d, 5.0 / 3, 0.06, "defender 5 dice hit on 5+");
    battle_stats(8, 0, 8, 0, R_LOSSARNACH, T, &a, &d);
    NEAR(a, 5.0 / 3, 0.06, "8 units still roll 5 dice");
    battle_stats(8, 0, 2, 0, R_LOSSARNACH, T, &a, &d);
    NEAR(d, 2.0 / 3, 0.05, "2 units roll 2 dice");
    battle_stats(5, 0, 5, 0, R_PELARGIR, T, &a, &d);               /* city: attacker needs 6 in round 1 */
    NEAR(a, 5.0 / 6, 0.05, "attacking a city: 6+ in first round"); NEAR(d, 5.0 / 3, 0.06, "city defender unaffected");
    battle_stats(5, 0, 5, 0, R_OSGILIATH, T, &a, &d);              /* fortification */
    NEAR(a, 5.0 / 6, 0.05, "attacking a fortification: 6+ in first round");
    battle_stats(5, 5, 5, 0, R_LOSSARNACH, T, &a, &d);             /* leader re-roll: misses re-rolled once */
    NEAR(a, 5 * (1.0 / 3 + 2.0 / 3 / 3), 0.07, "5 dice with 5 leaders");
    battle_stats(5, 2, 5, 0, R_LOSSARNACH, T, &a, &d);
    /* E[rerolled dice] = E[min(2, misses)], misses ~ Bin(5, 2/3) */
    { double e = 0; for (int k = 0; k <= 5; k++) { double p = 1; int c = 1; for (int j = 0; j < k; j++) c = c * (5 - j) / (j + 1); p = c * pow(2.0 / 3, k) * pow(1.0 / 3, 5 - k); e += p * (k < 2 ? k : 2); }
      NEAR(a, 5.0 / 3 + e / 3, 0.07, "5 dice with 2 leaders"); }
}

static void test_politics_and_capture(void) {
    printf("politics, capture, siege\n");
    State s; fresh(&s, 21); shadow_to_act(&s);
    Move m;
    /* non-belligerent armies cannot attack or cross into another nation */
    CHECK(s.pol[SAURON] == 1 && s.pol[GONDOR] == 2 && !s.active[GONDOR], "start politics");
    CHECK(!find(&s, M_ATTACK, -1, -1, 0), "nobody at war: no attacks");
    CHECK(find(&s, M_ARMY, R_MINAS_MORGUL, R_NORTH_ITHILIEN, 0), "Sauron may enter neutral North Ithilien");
    clear_region(&s, R_SOUTH_ITHILIEN); put(&s, R_SOUTH_ITHILIEN, SAURON, 3, 0);
    CHECK(find(&s, M_ARMY, R_SOUTH_ITHILIEN, R_OSGILIATH, 0) == 0, "Osgiliath holds Gondor units: cannot move in");
    clear_region(&s, R_DAGORLAD); put(&s, R_DAGORLAD, SAURON, 2, 0);
    /* muster needs At War */
    CHECK(!find(&s, M_MUSTER, -1, -1, 0) && !find(&s, M_MUSTER_ELITE, -1, -1, 0), "no muster before war");
    CHECK(find(&s, M_DIPLO, SAURON, -1, &m), "diplomacy available");
    wotr_apply(&s, m);
    CHECK(s.pol[SAURON] == 0, "Sauron at war");
    while (s.dkind != D_ACTION || s.dplayer != SH) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    CHECK(find(&s, M_MUSTER, R_BARAD_DUR, 0, 0) && find(&s, M_MUSTER, R_BARAD_DUR, 1, 0) && find(&s, M_MUSTER_ELITE, R_MORIA, -1, 0), "Sauron musters regulars, Nazgul, elites");
    CHECK(!find(&s, M_MUSTER, R_UMBAR, -1, 0), "Southrons still cannot");
    /* attack Osgiliath: Gondor activates and advances one step */
    set_dice(&s, SH, 0, 2, 0, 0, 0, 0);
    CHECK(find(&s, M_ATTACK, R_SOUTH_ITHILIEN, R_OSGILIATH, &m), "attack available once at war");
    wotr_apply(&s, m);
    CHECK(s.active[GONDOR] && s.pol[GONDOR] == 1, "Gondor attacked: active %d step %d", s.active[GONDOR], s.pol[GONDOR]);
    /* capture of a city: Pelargir */
    fresh(&s, 22); shadow_to_act(&s);
    s.pol[SOUTHRON] = 0; clear_region(&s, R_PELARGIR); clear_region(&s, R_WEST_HARONDOR); put(&s, R_WEST_HARONDOR, SOUTHRON, 3, 0);
    set_dice(&s, SH, 0, 1, 0, 0, 0, 0);
    find(&s, M_ARMY, R_WEST_HARONDOR, R_PELARGIR, &m); wotr_apply(&s, m);
    CHECK(s.ctrl[R_PELARGIR] == SH && wotr_vp(&s, SH) == 1, "Pelargir captured: 1 VP (got %d)", wotr_vp(&s, SH));
    CHECK(s.active[GONDOR] && s.pol[GONDOR] == 1, "capture activates and advances Gondor (%d,%d)", s.active[GONDOR], s.pol[GONDOR]);
    /* towns give no VP */
    fresh(&s, 23); shadow_to_act(&s);
    s.pol[SOUTHRON] = 0; clear_region(&s, R_PELARGIR); put(&s, R_PELARGIR, SOUTHRON, 3, 0); s.ctrl[R_PELARGIR] = SH;
    set_dice(&s, SH, 0, 1, 0, 0, 0, 0);
    find(&s, M_ARMY, R_PELARGIR, R_LOSSARNACH, &m); wotr_apply(&s, m);
    CHECK(s.ctrl[R_LOSSARNACH] == SH && wotr_vp(&s, SH) == 1, "Lossarnach (town) captured, VP stays 1");
    /* siege: defender retreats into stronghold, attacker advances; 6 defenders trimmed to 5 */
    fresh(&s, 24); shadow_to_act(&s);
    s.pol[SAURON] = 0; clear_region(&s, R_MINAS_TIRITH); put(&s, R_MINAS_TIRITH, GONDOR, 6, 0);
    clear_region(&s, R_OSGILIATH); put(&s, R_OSGILIATH, SAURON, 6, 2); s.naz[R_OSGILIATH] = 2; s.pool_naz -= 2;
    set_dice(&s, SH, 0, 2, 0, 0, 0, 0);
    find(&s, M_ATTACK, R_OSGILIATH, R_MINAS_TIRITH, &m); wotr_apply(&s, m);
    CHECK(s.dkind == D_SIEGE_RETREAT && s.dplayer == FP, "defender in a stronghold chooses field battle or siege");
    wotr_apply(&s, choice(1));
    CHECK(s.dkind == D_ADVANCE && s.dplayer == SH, "attacker may advance");
    wotr_apply(&s, choice(1));
    CHECK(s.siege[R_MINAS_TIRITH] && units(&s, FP, R_MINAS_TIRITH) == 5 && units(&s, SH, R_MINAS_TIRITH) == 8 && s.ctrl[R_MINAS_TIRITH] == FP,
          "Minas Tirith besieged: %d defenders, %d besiegers", units(&s, FP, R_MINAS_TIRITH), units(&s, SH, R_MINAS_TIRITH));
    CHECK(wotr_vp(&s, SH) == 0, "no VP for a siege");
    while (s.dkind != D_ACTION || s.dplayer != SH) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    /* siege assault lasts one round unless an elite is reduced */
    for (int c = 0; c < NCARD; c++) if (s.card_where[c] == W_HAND) { s.card_where[c] = W_DISCARD; s.hand_n[CARD_SIDE(c)]--; }
    set_dice(&s, SH, 0, 1, 0, 0, 0, 0);
    CHECK(find(&s, M_ATTACK, R_MINAS_TIRITH, R_MINAS_TIRITH, &m), "siege assault available");
    CHECK(!find(&s, M_MUSTER, R_MINAS_TIRITH, -1, 0), "no muster in a besieged stronghold");
    wotr_apply(&s, m);
    while (s.dkind == D_CASUALTY) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    if (units(&s, FP, R_MINAS_TIRITH) && elites(&s, SH, R_MINAS_TIRITH)) {
        CHECK(s.dkind == D_SIEGE_EXTEND, "after one siege round the attacker may reduce an elite to continue (got %s)", wotr_dkind_name(s.dkind));
        int e0 = elites(&s, SH, R_MINAS_TIRITH), u0 = units(&s, SH, R_MINAS_TIRITH);
        wotr_apply(&s, choice(SAURON));
        CHECK(elites(&s, SH, R_MINAS_TIRITH) <= e0 - 1 && units(&s, SH, R_MINAS_TIRITH) <= u0, "elite reduced");
    }
    /* stronghold falls when the garrison dies: 2 VP */
    fresh(&s, 25); shadow_to_act(&s);
    s.pol[SAURON] = 0; clear_region(&s, R_MINAS_TIRITH); put(&s, R_MINAS_TIRITH, GONDOR, 1, 0); put(&s, R_MINAS_TIRITH, SAURON, 8, 0); s.siege[R_MINAS_TIRITH] = 1;
    for (int c = 0; c < NCARD; c++) if (s.card_where[c] == W_HAND) { s.card_where[c] = W_DISCARD; s.hand_n[CARD_SIDE(c)]--; }
    int fell = 0;
    for (int i = 0; i < 200 && !fell; i++) {
        State u = s; u.rng += (uint64_t)i * 5; set_dice(&u, SH, 0, 1, 0, 0, 0, 0);
        find(&u, M_ATTACK, R_MINAS_TIRITH, R_MINAS_TIRITH, &m); wotr_apply(&u, m);
        while (u.dkind == D_CASUALTY) { wotr_moves(&u, mv); wotr_apply(&u, mv[0]); }
        if (!units(&u, FP, R_MINAS_TIRITH)) { fell = 1; CHECK(u.ctrl[R_MINAS_TIRITH] == SH && !u.siege[R_MINAS_TIRITH] && wotr_vp(&u, SH) == 2, "Minas Tirith falls: 2 VP (got %d)", wotr_vp(&u, SH)); }
    }
    CHECK(fell, "garrison eventually dies");
    /* military victory at end of turn: 10 VP */
    fresh(&s, 26);
    static const uint8_t take[] = {R_MINAS_TIRITH, R_DOL_AMROTH, R_HELMS_DEEP, R_LORIEN, R_EREBOR};
    for (int i = 0; i < 5; i++) { clear_region(&s, take[i]); s.ctrl[take[i]] = SH; put(&s, take[i], SAURON, 1, 0); }
    CHECK(wotr_vp(&s, SH) == 10, "five strongholds = 10 VP");
    set_dice(&s, FP, 0, 0, 1, 0, 0, 0); set_dice(&s, SH, 0, 0, 0, 0, 0, 0);
    find(&s, M_SKIP, -1, -1, &m); wotr_apply(&s, m);
    CHECK(s.winner == 2 && s.win_type == WIN_SH_MIL, "Shadow military victory at end of turn");
}

static void test_characters(void) {
    printf("characters\n");
    State s; Move m;
    /* Saruman needs Isengard at war; adds a die next turn; Voice of Saruman */
    fresh(&s, 31); shadow_to_act(&s); set_dice(&s, SH, 0, 0, 3, 0, 0, 0);
    CHECK(!find(&s, M_MINION, M_SARUMAN, -1, 0), "no Saruman before Isengard is at war");
    s.pol[ISENGARD] = 0;
    CHECK(find(&s, M_MINION, M_SARUMAN, R_ORTHANC, &m), "Saruman available");
    wotr_apply(&s, m);
    while (s.dkind != D_ACTION || s.dplayer != SH) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    int r0 = s.reg[R_ORTHANC][ISENGARD], e0 = s.eli[R_ORTHANC][ISENGARD], n0 = s.reg[R_NORTH_DUNLAND][ISENGARD];
    State u = s; find(&u, M_VOICE, 0, -1, &m); wotr_apply(&u, m);
    CHECK(u.reg[R_ORTHANC][ISENGARD] == r0 + 1 && u.reg[R_NORTH_DUNLAND][ISENGARD] == n0 + 1 && u.reg[R_SOUTH_DUNLAND][ISENGARD] == 2, "Voice: a regular in each Isengard settlement");
    u = s; find(&u, M_VOICE, 1, -1, &m); wotr_apply(&u, m);
    CHECK(u.reg[R_ORTHANC][ISENGARD] == r0 - 2 && u.eli[R_ORTHANC][ISENGARD] == e0 + 2, "Voice: two regulars become elites");
    /* Witch-king: Sauron and a FP nation at war; activates everyone */
    CHECK(!find(&s, M_MINION, M_WK, -1, 0), "no Witch-king yet");
    s.pol[SAURON] = 0; s.pol[ELVES] = 0;
    CHECK(find(&s, M_MINION, M_WK, R_MINAS_MORGUL, &m) && !find(&s, M_MINION, M_WK, R_ORTHANC, 0), "Witch-king joins an army with Sauron units");
    wotr_apply(&s, m);
    CHECK(s.active[GONDOR] && s.active[ROHAN] && s.active[NORTH] && s.active[DWARVES], "all Free Peoples nations activated");
    /* Mouth needs Mordor track or all FP at war */
    while (s.dkind != D_ACTION || s.dplayer != SH) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    set_dice(&s, SH, 0, 0, 2, 0, 0, 0);
    CHECK(!find(&s, M_MINION, M_MOUTH, -1, 0), "no Mouth yet");
    s.mordor = 0;
    CHECK(find(&s, M_MINION, M_MOUTH, R_BARAD_DUR, 0), "Mouth once the Fellowship is in Mordor");
    s.mordor = -1;
    /* next turn: 9 dice */
    set_dice(&s, FP, 0, 0, 1, 0, 0, 0); set_dice(&s, SH, 0, 0, 0, 0, 0, 0);
    while (s.dkind != D_ACTION || s.dplayer != FP) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    find(&s, M_SKIP, -1, -1, &m); wotr_apply(&s, m); to_action(&s);
    int sd = s.hunt_sh; for (int f = 0; f < NFACE; f++) sd += s.dice[SH][f];
    CHECK(sd == 9, "Shadow rolls 9 dice with two minions (got %d)", sd);
    /* Gandalf the White: needs Grey gone and a minion seen; +1 die */
    CHECK(s.minion_seen, "minion seen");
    set_dice(&s, FP, 0, 0, 0, 0, 0, 1);
    CHECK(!find(&s, M_GANDALF_WHITE, -1, -1, 0), "Gandalf still in the Fellowship");
    kill_comp(&s, C_GANDALF);
    CHECK(find(&s, M_GANDALF_WHITE, R_FANGORN, -1, &m) && find(&s, M_GANDALF_WHITE, R_LORIEN, -1, 0), "Gandalf the White in Fangorn or an elven stronghold");
    wotr_apply(&s, m);
    CHECK(s.gandalf_white && s.comp_loc[C_GANDALF] == R_FANGORN, "placed");
    /* Aragorn */
    fresh(&s, 32); set_dice(&s, FP, 0, 0, 0, 0, 0, 1);
    s.comp_loc[C_STRIDER] = R_EDORAS; fix_guide(&s);
    CHECK(!find(&s, M_ARAGORN, -1, -1, 0), "not in Edoras");
    s.comp_loc[C_STRIDER] = R_PELARGIR;
    CHECK(find(&s, M_ARAGORN, -1, -1, &m), "crown in Pelargir");
    wotr_apply(&s, m);
    CHECK(s.aragorn && s.active[GONDOR], "Aragorn crowned and Gondor activated");
    /* companion activation by movement */
    fresh(&s, 33); set_dice(&s, FP, 1, 0, 0, 0, 0, 0);
    find(&s, M_SEPARATE, 1 << C_BOROMIR, -1, &m); wotr_apply(&s, m);
    CHECK(s.dkind == D_SEP_DEST, "asks for destination");
    CHECK(has_choice(&s, R_FORDS_OF_BRUINEN) && has_choice(&s, R_HOLLIN) && !has_choice(&s, R_MORIA), "level 2 from Rivendell with progress 0: two regions");
    wotr_apply(&s, choice(R_HOLLIN));
    CHECK(s.comp_loc[C_BOROMIR] == R_HOLLIN && s.guide == C_GANDALF, "Boromir separated");
}

static void test_dice_and_cards(void) {
    printf("dice, rings, cards\n");
    State s; Move m;
    /* die faces: FP has no pure Army face; frequencies */
    long f[2][NFACE] = {{0}};
    for (int i = 0; i < 4000; i++) { fresh(&s, 9000 + (uint64_t)i); for (int k = 0; k < NFACE; k++) { f[0][k] += s.dice[FP][k]; f[1][k] += s.dice[SH][k]; } f[1][F_SPECIAL] += s.hunt_sh; }
    NEAR(f[0][F_CHAR] / 16000.0, 2.0 / 6, 0.012, "FP character faces"); CHECK(f[0][F_ARMY] == 0, "FP has no Army face");
    NEAR(f[0][F_SPECIAL] / 16000.0, 1.0 / 6, 0.01, "Will of the West face");
    NEAR(f[1][F_ARMY] / 28000.0, 1.0 / 6, 0.01, "Shadow army face");
    /* passing requires fewer dice */
    fresh(&s, 41);
    CHECK(find(&s, M_PASS, -1, -1, 0), "FP (4 dice) may pass against Shadow");
    set_dice(&s, SH, 1, 0, 0, 0, 0, 0); set_dice(&s, FP, 1, 1, 0, 0, 0, 0);
    CHECK(!find(&s, M_PASS, -1, -1, 0), "no pass with more dice");
    /* elven ring: FP cannot make a Will of the West; one per turn; ring passes to Shadow */
    fresh(&s, 42); set_dice(&s, FP, 0, 0, 2, 0, 0, 0);
    CHECK(find(&s, M_RING, F_MUSTER, F_CHAR, &m) && !find(&s, M_RING, F_MUSTER, F_SPECIAL, 0), "ring options");
    wotr_apply(&s, m);
    CHECK(s.dkind == D_ACTION && s.dplayer == FP && s.dice[FP][F_CHAR] == 1 && s.dice[FP][F_MUSTER] == 1 && s.rings[FP] == 2 && s.rings[SH] == 1, "ring used, FP still to act");
    CHECK(!find(&s, M_RING, -1, -1, 0), "one ring per turn");
    /* hand limit 6 */
    fresh(&s, 43); set_dice(&s, FP, 0, 0, 0, 6, 0, 0); set_dice(&s, SH, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < 4; i++) { find(&s, M_DRAW, 0, -1, &m); wotr_apply(&s, m); }
    CHECK(s.hand_n[FP] == 6, "four draws on top of two: hand 6");
    find(&s, M_DRAW, 1, -1, &m); wotr_apply(&s, m);
    CHECK(s.dkind == D_DISCARD && s.dplayer == FP && s.hand_n[FP] == 7, "seventh card forces a discard");
    wotr_moves(&s, mv); wotr_apply(&s, mv[0]);
    CHECK(s.hand_n[FP] == 6, "back to 6");
    /* Gandalf guide draws after an Event-die card play */
    fresh(&s, 44);
    for (int c = 0; c < NCARD; c++) if (s.card_where[c] == W_HAND) { s.card_where[c] = W_DECK; s.hand_n[CARD_SIDE(c)]--; }
    s.card_where[CARD_ELVEN_CLOAKS] = W_HAND; s.hand_n[FP] = 1; s.guide = C_GANDALF;
    set_dice(&s, FP, 0, 0, 0, 1, 0, 0);
    find(&s, M_PLAY, CARD_ELVEN_CLOAKS, -1, &m); wotr_apply(&s, m);
    CHECK(s.hand_n[FP] == 1 && s.special_st[T_CLOAKS] == 1, "Elven Cloaks set aside; Gandalf drew a replacement (hand %d)", s.hand_n[FP]);
    /* special tiles join the pool on entering Mordor together with drawn Eyes */
    s.tile_pool[T_EYE] = 1; s.tile_out[T_EYE] = 3; s.fsp_loc = R_MINAS_MORGUL; s.progress = 0;
    set_dice(&s, FP, 0, 0, 1, 0, 0, 0); set_dice(&s, SH, 0, 0, 0, 0, 0, 0);
    while (s.dkind != D_ACTION || s.dplayer != FP) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    find(&s, M_SKIP, -1, -1, &m); wotr_apply(&s, m);
    while (s.dkind != D_ENTER_MORDOR && s.turn < 4) { wotr_moves(&s, mv); wotr_apply(&s, mv[0]); }
    wotr_apply(&s, choice(1));
    CHECK(s.tile_pool[T_EYE] == 4 && s.tile_pool[T_CLOAKS] == 1, "Mordor pool: eyes %d cloaks %d", s.tile_pool[T_EYE], s.tile_pool[T_CLOAKS]);
    /* A Power too Great protects Lorien */
    fresh(&s, 45); shadow_to_act(&s);
    s.pol[SAURON] = 0; clear_region(&s, R_DIMRILL_DALE); put(&s, R_DIMRILL_DALE, SAURON, 5, 0); set_dice(&s, SH, 0, 2, 0, 0, 0, 0);
    CHECK(find(&s, M_ATTACK, R_DIMRILL_DALE, R_LORIEN, 0), "Lorien attackable");
    s.card_where[CARD_A_POWER_TOO_GREAT] = W_TABLE;
    CHECK(!find(&s, M_ATTACK, R_DIMRILL_DALE, R_LORIEN, 0), "protected by A Power too Great");
}

/* regressions for bugs found by the independent rules audit */
static void test_audit(void) {
    printf("audit regressions\n");
    State s; Move m;
    /* Shadow hunt cards stay playable on the Mordor Track; Nazgul Search does not */
    fresh(&s, 51); s.mordor = 2; s.revealed = 1;
    CHECK(card_playable(&s, CARD_ORC_PATROL) && card_playable(&s, CARD_ISILDURS_BANE) && card_playable(&s, CARD_MORGUL_WOUND) &&
          card_playable(&s, CARD_LURE_OF_THE_RING) && card_playable(&s, CARD_THE_BREAKING_OF_THE_FELLOWSHIP), "Shadow Fellowship cards playable in Mordor");
    CHECK(!card_playable(&s, CARD_NAZGUL_SEARCH) && !card_playable(&s, CARD_CRUEL_WEATHER), "track-step cards are not");
    s.mordor = -1; s.revealed = 0;
    CHECK(!card_playable(&s, CARD_ORC_PATROL), "Orc Patrol blocked while the Fellowship sits in Rivendell");
    /* I Will Go Alone in Mordor: the companion is lost but the heal applies */
    fresh(&s, 52); s.mordor = 1; s.corruption = 5;
    for (int c = 0; c < NCARD; c++) if (s.card_where[c] == W_HAND) { s.card_where[c] = W_DECK; s.hand_n[CARD_SIDE(c)]--; }
    s.card_where[CARD_I_WILL_GO_ALONE] = W_HAND; s.hand_n[FP] = 1; set_dice(&s, FP, 1, 0, 0, 0, 0, 0);
    CHECK(find(&s, M_PLAY, CARD_I_WILL_GO_ALONE, -1, &m), "I Will Go Alone playable on the Mordor Track");
    wotr_apply(&s, m);
    if (s.dkind == D_CARD_COMP) wotr_apply(&s, choice(1 << C_BOROMIR));
    CHECK(s.comp_loc[C_BOROMIR] == LOC_DEAD && s.corruption == 4, "Boromir removed, one corruption healed (corr %d)", s.corruption);
    /* walking into Edoras does not rouse Rohan under Wormtongue; capture does not discard Threats and Promises */
    fresh(&s, 53); shadow_to_act(&s);
    s.pol[ISENGARD] = 0; s.card_where[CARD_WORMTONGUE] = W_TABLE; s.minion_loc[M_SARUMAN] = R_ORTHANC; s.card_where[CARD_THREATS_AND_PROMISES] = W_TABLE;
    clear_region(&s, R_EDORAS); clear_region(&s, R_WESTEMNET); put(&s, R_WESTEMNET, ISENGARD, 3, 0); s.ctrl[R_WESTEMNET] = SH;
    set_dice(&s, SH, 0, 1, 0, 0, 0, 0);
    find(&s, M_ARMY, R_WESTEMNET, R_EDORAS, &m); wotr_apply(&s, m);
    CHECK(s.ctrl[R_EDORAS] == SH && !s.active[ROHAN], "Edoras taken by a walk-in, Rohan still passive under Wormtongue");
    CHECK(s.card_where[CARD_THREATS_AND_PROMISES] == W_TABLE && s.pol[ROHAN] == 2, "capture advances Rohan but keeps Threats and Promises (step %d)", s.pol[ROHAN]);
    /* Gollum, promoted mid-hunt, ignores the reveal icon of a numbered tile */
    fresh(&s, 54); for (int c = 0; c < NCOMP; c++) s.comp_loc[c] = LOC_DEAD;
    fix_guide(&s); s.progress = 2; set_dice(&s, FP, 1, 0, 0, 0, 0, 0); s.hunt_sh = 5; s.hunt_fp = 5;
    memset(s.tile_pool, 0, NTILE); s.tile_pool[T_1R] = 2; memset(s.tile_out, 0, NTILE);
    find(&s, M_FSP_MOVE, -1, -1, &m);
    { int seen = 0;
      for (int i = 0; i < 40 && !seen; i++) { State u = s; u.rng += (uint64_t)i * 3; wotr_apply(&u, m);
        if (u.dkind == D_HUNT_RESPONSE) { seen = 1; CHECK(has_choice(&u, 3), "Gollum may reveal to reduce"); State v = u; wotr_apply(&v, choice(0)); CHECK(!v.revealed && v.corruption == 1, "1-reveal tile does not reveal under Gollum"); } }
      CHECK(seen, "hunt reached"); }
}

/* the structure-aware generator: far fewer moves, the purposeful ones kept */
static void test_pruning(void) {
    printf("pruned generation\n");
    State s; Move m;
    wotr_prune = 1;
    fresh(&s, 61); shadow_to_act(&s);
    s.pol[SAURON] = 0; s.pol[ISENGARD] = 0; s.pol[SOUTHRON] = 0; set_dice(&s, SH, 1, 1, 0, 0, 0, 0);
    int np = wotr_moves(&s, mv), army_p = 0, army_f = 0;
    for (int i = 0; i < np; i++) army_p += mv[i].type == M_ARMY;
    CHECK(find(&s, M_ARMY, R_MINAS_MORGUL, R_NORTH_ITHILIEN, 0), "advance on Gondor kept");
    CHECK(find(&s, M_ARMY, R_DOL_GULDUR, R_NORTH_ANDUIN_VALE, 0) || find(&s, M_ARMY, R_DOL_GULDUR, R_SOUTH_ANDUIN_VALE, 0), "Dol Guldur can march on Lorien");
    CHECK(!find(&s, M_ARMY, R_FAR_HARAD, R_KHAND, 0), "Far Harad -> Khand (away from everything) dropped");
    wotr_prune = 0;
    int nf = wotr_moves(&s, mv);
    for (int i = 0; i < nf; i++) army_f += mv[i].type == M_ARMY;
    CHECK(find(&s, M_ARMY, R_FAR_HARAD, R_KHAND, 0), "...but still legal in the full generator");
    /* (was "by more than half" until Shadow was given marches toward any nearby target and round a blocked road, 2026-11-16/18) */
    CHECK(army_p * 5 < army_f * 3, "army moves pruned to under three fifths (%d vs %d)", army_p, army_f);
    printf("  action moves at this position: %d pruned vs %d full\n", np, nf);
    /* Nazgul allocation: capped by the leadership the army can use, one may hunt, the rest stay */
    wotr_prune = 1;
    fresh(&s, 62); shadow_to_act(&s);
    s.pol[SAURON] = 0; clear_region(&s, R_OSGILIATH); put(&s, R_OSGILIATH, SAURON, 3, 0); s.progress = 1; s.fsp_loc = R_HOLLIN;   /* Nazgul may not enter Rivendell */
    set_dice(&s, SH, 1, 0, 0, 0, 0, 0);
    CHECK(find(&s, M_NAZGUL, -1, -1, &m), "nazgul action");
    wotr_apply(&s, m);
    int n = wotr_moves(&s, mv);
    CHECK(s.dkind == D_NAZ_MOVE && n <= 16, "a handful of allocation plans instead of a move chain (%d)", n);
    CHECK(find(&s, M_NAZ_PLAN, R_OSGILIATH, -1, &m), "plan: lead the army at Osgiliath");
    State u = s; wotr_apply(&u, m);
    CHECK(u.naz[R_OSGILIATH] == 3, "three units can use three leaders (got %d)", u.naz[R_OSGILIATH]);
    { int tot = 0; for (int r = 0; r < NREG; r++) tot += u.naz[r]; CHECK(tot == 4, "no Nazgul lost or invented (%d)", tot); }
    m.c = 1; u = s;
    if (find(&u, M_NAZ_PLAN, R_OSGILIATH, -1, 0)) { Move h; int ok = 0; int k = wotr_moves(&u, mv); for (int i = 0; i < k; i++) if (mv[i].type == M_NAZ_PLAN && mv[i].a == R_OSGILIATH && mv[i].c) { h = mv[i]; ok = 1; }
        CHECK(ok, "hunter variant offered");
        if (ok) { wotr_apply(&u, h); CHECK(u.naz[u.fsp_loc] == 1 && u.naz[R_OSGILIATH] == 3, "one Nazgul on the Fellowship, three with the army"); } }
    CHECK(u.dkind == D_ACTION, "the plan completes the action");
    /* companions: only purposeful destinations */
    fresh(&s, 63); set_dice(&s, FP, 1, 0, 0, 0, 0, 0); s.progress = 6;
    find(&s, M_SEPARATE, 1 << C_STRIDER, -1, &m);
    u = s; wotr_apply(&u, m); n = wotr_moves(&u, mv);
    CHECK(u.dkind == D_SEP_DEST && n <= 25, "Strider's destinations are few (%d)", n);
    wotr_prune = 0; u = s; wotr_apply(&u, m); int nfull = wotr_moves(&u, mv);
    CHECK(nfull > 2 * n, "full generator offers many more (%d vs %d)", nfull, n);
    wotr_prune = 0;
}

int main(void) {
    wotr_global_init();
    wotr_prune = 0;     /* rules are tested against the full generator */
    test_setup(); test_hunt_odds(); test_fellowship(); test_hunt_resolution(); test_combat_odds();
    test_politics_and_capture(); test_characters(); test_dice_and_cards(); test_audit(); test_pruning();
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
