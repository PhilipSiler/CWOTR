/* battle.c -- battles, sieges, combat cards, casualties */
#include "wotr.h"

#define CAPTAINS ((1 << C_GANDALF) | (1 << C_STRIDER) | (1 << C_BOROMIR) | (1 << C_LEGOLAS) | (1 << C_GIMLI))

int nazgul_leadership(const State *s, int r) { return s->naz[r] + (s->minion_loc[M_WK] == r ? 2 : 0); }
int army_leadership(const State *s, int side, int r) {
    int n = 0;
    if (side == FP) {
        n = fp_leaders(s, r);
        for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == r) n += comp_leadership(s, c);
    } else {
        n = nazgul_leadership(s, r) + shadow_leadership_units(s, r);
        if (s->minion_loc[M_MOUTH] == r) n += 2;
        if (s->minion_loc[M_SARUMAN] == r) n += 1;
    }
    return n;
}

/* ---------- casualties ---------- */
/* frame vars: 0 side, 1 region, 2 hits, 3 kill characters if wiped */
void push_casualty(State *s, int side, int r, int hits, int kill_chars) {
    if (hits <= 0) return;
    Frame *f = push(s, FR_CASUALTY);
    f->v[0] = (uint8_t)side; f->v[1] = (uint8_t)r; f->v[2] = (uint8_t)hits; f->v[3] = (uint8_t)kill_chars;
}
void fr_casualty(State *s, Frame *f) {
    CO_BEGIN
    while (f->v[2] > 0 && units(s, f->v[0], f->v[1]) > 0) {
        int side = f->v[0], r = f->v[1], lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT;
        s->nopt = 0;
        for (int n = lo; n < hi; n++) {
            if (s->reg[r][n]) s->opt[s->nopt++] = (uint8_t)n;                           /* remove a regular */
            if (s->eli[r][n]) s->opt[s->nopt++] = (uint8_t)(8 + n);                     /* reduce an elite */
            if (s->eli[r][n] && f->v[2] >= 2) s->opt[s->nopt++] = (uint8_t)(16 + n);    /* remove an elite (2 hits) */
        }
        {   /* hits enough to destroy the whole Army: no choice to make, everything goes (RB p. 30) */
            int cap = 0;
            for (int n = lo; n < hi; n++) cap += s->reg[r][n] + 2 * s->eli[r][n];
            if (f->v[2] >= cap) for (int i = 0; i < s->nopt; i++) if ((s->opt[i] >> 3) != 1) { s->opt[0] = s->opt[i]; s->nopt = 1; break; }
        }
        if (s->nopt > 1) ASK(D_CASUALTY, f->v[0]); else s->mv.a = s->opt[0];
        {
            int r2 = f->v[1], n = s->mv.a & 7, k = s->mv.a >> 3;
            if (k == 0) { kill_unit(s, r2, n, 0); f->v[2]--; }
            else if (k == 1) { downgrade_elite(s, r2, n); f->v[2]--; }
            else { kill_unit(s, r2, n, 1); f->v[2] -= 2; }
        }
    }
    if (units(s, f->v[0], f->v[1]) == 0) {
        wipe_leaders(s, f->v[0], f->v[1], f->v[3]);
    }
    if (!s->b.active) fix_siege(s, f->v[1]);
    CO_END
}

/* ---------- combat card requirements ---------- */
static int def_region(const State *s) { return s->b.dst; }
static int army_region(const State *s, int side) { return side == s->b.att ? s->b.src : s->b.dst; }
static int battle_comps(const State *s) {
    int m = comps_at(s, army_region(s, FP));
    if (s->b.wop_comp < NCOMP) m &= ~(1 << s->b.wop_comp);
    return m;
}
static int naz_lead_now(const State *s) {
    if (s->b.white_rider) return 0;
    int n = nazgul_leadership(s, army_region(s, SH)) - s->b.forfeit[SH];
    return n < 0 ? 0 : n;
}
int cc_playable(const State *s, int side, int c) {
    const Battle *b = &s->b;
    int fr = army_region(s, FP), sr = army_region(s, SH), d = def_region(s);
    int comps = comps_at(s, fr), dn = REGION[d].nation;
    int ents = dn == ROHAN || d == R_FANGORN || d == R_ORTHANC;
    (void)side;
    switch (CARD[c].combat) {
    case CC_IT_IS_A_GIFT: case CC_ONE_FOR_THE_DARK_LORD: return s->mordor < 0 && s->fsp_loc == d;
    case CC_BLADE_OF_WESTERNESSE: return (comps & ((1 << C_MERRY) | (1 << C_PIPPIN))) != 0;
    case CC_DARING_DEFIANCE: return comps != 0 && !b->white_rider;     /* The White Rider: it changes nothing (Card Almanac p. 38) */
    case CC_MIGHTY_ATTACK: case CC_BRAVE_STAND: return comps != 0;
    case CC_SERVANT_OF_THE_SECRET_FIRE: return (comps & 1) != 0;
    case CC_ANDURIL: return (comps & (1 << C_STRIDER)) != 0;
    case CC_HEROIC_DEATH: case CC_FATEFUL_STRIKE: case CC_SUDDEN_STRIKE: return comps != 0 || fp_leaders(s, fr) > 0;
    case CC_ENTS_RAGE: case CC_HUORN_DARK: case CC_NAMELESS_WOOD: return ents;
    case CC_ADVANTAGEOUS_POSITION: return dn < NFPNAT;
    case CC_SCOUTS: return b->att == SH && !b->siege;
    case CC_CHARGE: case CC_VALOUR: return elites(s, FP, fr) > 0;
    case CC_CRUEL_AS_DEATH: return naz_lead_now(s) >= 2;
    case CC_THEY_ARE_TERRIBLE: case CC_DREAD_AND_DESPAIR: case CC_FOUL_STENCH: return naz_lead_now(s) >= 1;
    case CC_BLACK_BREATH: case CC_WORDS_OF_POWER: return s->naz[sr] > 0 || s->minion_loc[M_WK] == sr;
    case CC_DURINS_BANE: return DIST[d][R_MORIA] <= 2;
    /* "the defending Army is in a Stronghold": inside it, a siege battle (Card Almanac) */
    case CC_DEVILRY_OF_ORTHANC: return s->reg[sr][ISENGARD] + s->eli[sr][ISENGARD] > 0 && REGION[d].settle == S_STRONGHOLD && b->siege;
    case CC_MUMAKIL: return s->eli[sr][SOUTHRON] > 0;
    case CC_WE_COME_TO_KILL: return elites(s, SH, sr) > 0;
    default: return 1;
    }
}

/* ---------- battle ---------- */
void attacked_politics(State *s, int side, int r) {   /* army of `side` in r is attacked */
    int lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT;
    for (int n = lo; n < hi; n++) {
        if (!(s->reg[r][n] + s->eli[r][n])) continue;
        if (n < NFPNAT && !(n == ROHAN && s->card_where[CARD_WORMTONGUE] == W_TABLE && r != R_EDORAS && r != R_HELMS_DEEP)) activate(s, n);
        advance_pol(s, n, 1);
    }
}

void push_battle(State *s, int side, int src, int dst, int nr, int ne) {
    Battle *b = &s->b;
    memset(b, 0, sizeof *b);
    b->active = 1; b->att = (uint8_t)side; b->src = (uint8_t)src; b->dst = (uint8_t)dst;
    b->card[0] = b->card[1] = 255; b->wop_comp = 255;
    if (src == dst) { if (s->ctrl[src] == side) b->sortie = 1; else b->siege = 1; }
    else if (s->siege[dst] && s->ctrl[dst] == side) b->relief = 1;
    b->max_rounds = b->siege ? 1 : 255;
    /* set the rearguard aside: everything that is not attacking */
    uint8_t mr[NNAT], me[NNAT];
    movable_units(s, side, src, src, 1, mr, me);
    int lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT;
    for (int n = lo; n < hi; n++) {
        int k = mr[n] < nr ? mr[n] : nr; nr -= k;
        b->rear_reg[n] = (uint8_t)(s->reg[src][n] - k); s->reg[src][n] = (uint8_t)k;
        k = me[n] < ne ? me[n] : ne; ne -= k;
        b->rear_eli[n] = (uint8_t)(s->eli[src][n] - k); s->eli[src][n] = (uint8_t)k;
    }
    attacked_politics(s, 1 - side, dst);
    s->stat_battles++;
    /* The White Rider */
    int fr = side == FP ? src : dst, sr = side == FP ? dst : src;
    if (s->gandalf_white && s->comp_loc[C_GANDALF] == fr && nazgul_leadership(s, sr) > 0) b->white_rider = 2;     /* asked first (fr_battle) */
    push(s, FR_BATTLE);
}

static void restore_rearguard(State *s) {
    Battle *b = &s->b;
    for (int n = 0; n < NNAT; n++) { s->reg[b->src][n] += b->rear_reg[n]; s->eli[b->src][n] += b->rear_eli[n]; b->rear_reg[n] = b->rear_eli[n] = 0; }
}
static int rear_units(const State *s) { int k = 0; for (int n = 0; n < NNAT; n++) k += s->b.rear_reg[n] + s->b.rear_eli[n]; return k; }

/* The Witch-king's "Sorcerer": a Shadow Combat card played in round 1 with him in the battle earns a card of its type,
 * drawn once that round is fully over and only if he survived it (Card Almanac p. 8, 36-37). */
static void sorcerer(State *s) {
    Battle *b = &s->b;
    if (!b->sorc) return;
    int deck = b->sorc - 1;
    b->sorc = 0;
    if (s->minion_loc[M_WK] < NREG) draw_card(s, deck);
}

static int retreat_options(State *s, int side, int from) {
    s->nopt = 0;
    for (int i = 0; i < REGION[from].nadj; i++) {
        int v = REGION[from].adj[i];
        if (units(s, 1 - side, v)) continue;
        if (is_settlement(v) && s->ctrl[v] != side) continue;
        if (side == SH && protected_region(s, v)) continue;
        s->opt[s->nopt++] = (uint8_t)v;
    }
    return s->nopt;
}
/* attacker moves the whole attacking army into dst */
static void advance(State *s) {
    Battle *b = &s->b;
    int side = b->att, src = b->src, dst = b->dst;
    if (src == dst) return;
    int had_siege = s->siege[src] && s->ctrl[src] != side, rear = rear_units(s);
    for (int n = 0; n < NNAT; n++) {
        if (NAT_SIDE(n) != side) continue;
        s->reg[dst][n] += s->reg[src][n]; s->reg[src][n] = 0;
        s->eli[dst][n] += s->eli[src][n]; s->eli[src][n] = 0;
    }
    if (side == FP) {
        for (int n = 0; n < NFPNAT; n++) { s->ldr[dst][n] += s->ldr[src][n]; s->ldr[src][n] = 0; }
        for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == src) s->comp_loc[c] = (uint8_t)dst;
    } else {
        s->naz[dst] += s->naz[src]; s->naz[src] = 0;
        for (int m = 0; m < NMINION; m++) if (m != M_SARUMAN && s->minion_loc[m] == src) s->minion_loc[m] = (uint8_t)dst;
    }
    if (had_siege && !rear) s->siege[src] = 0;
    if (s->siege[dst] && s->ctrl[dst] == side) s->siege[dst] = 0;      /* siege relieved */
    army_entered(s, side, dst);
    if (side == FP) place_comps(s, comps_at(s, dst), dst);
    enforce_stacking(s, side, dst);
}

void fr_battle(State *s, Frame *f) {
    Battle *b = &s->b;
    int att = b->att, def = 1 - att;
    CO_BEGIN
    /* The White Rider: "you may" cancel the Nazgul's Leadership (forfeiting Gandalf's): the Free Peoples choose, once,
       when the battle begins (the BehindMedia referee's question) */
    if (b->white_rider == 2) {
        s->nopt = 2; s->opt[0] = 1; s->opt[1] = 255; s->qtag = Q_WHITE_RIDER; ASK(D_CARD_OPTION, FP); s->qtag = Q_NONE;
        b->white_rider = s->mv.a == 1;
    }
    for (;;) {
        if (b->round >= 1) sorcerer(s);                               /* round 1 is over: before the next */
        b->round++;
        /* defender in an own stronghold: field battle or retreat into siege */
        if (!b->siege && !b->sortie && !b->relief && REGION[b->dst].settle == S_STRONGHOLD && s->ctrl[b->dst] == def && !s->siege[b->dst]) {
            s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1;
            ASK(D_SIEGE_RETREAT, def);
            if (s->mv.a) { b->ended = 4; break; }
        }
        CALL(push(s, FR_ROUND));
        if (s->winner) return;
        if (b->ended) break;
        if (!units(s, def, b->dst)) { b->ended = 2; break; }
        if (!units(s, att, b->src)) { b->ended = 3; break; }
        if (b->siege && b->round >= b->max_rounds) {
            if (!elites(s, att, b->src)) { b->ended = 1; break; }
            s->nopt = 0; s->opt[s->nopt++] = 255;
            for (int n = 0; n < NNAT; n++) if (NAT_SIDE(n) == att && s->eli[b->src][n]) s->opt[s->nopt++] = (uint8_t)n;
            ASK(D_SIEGE_EXTEND, att);
            if (s->mv.a == 255) { b->ended = 1; break; }
            downgrade_elite(s, b->src, s->mv.a);
            b->max_rounds++;
            continue;
        }
        if (!b->siege) {
            if (!b->no_cease) {
                s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1;
                ASK(D_CONTINUE, att);
                if (!s->mv.a) { b->ended = 1; break; }
            }
            if (!(s->siege[b->dst] && s->ctrl[b->dst] == def)) {         /* a besieged army cannot retreat */
                retreat_options(s, def, b->dst);
                if (s->nopt) {
                    memmove(s->opt + 1, s->opt, s->nopt); s->opt[0] = 255; s->nopt++;
                    ASK(D_RETREAT, def);
                    if (s->mv.a != 255) { relocate_army(s, def, b->dst, s->mv.a); b->ended = 2; break; }
                }
            }
        }
    }
    /* ---- end of battle ---- */
    if (b->ended == 3 || !units(s, att, b->src)) wipe_leaders(s, att, b->src, 1);
    if (b->ended == 4) {                                               /* defender withdrew into the stronghold */
        sorcerer(s);
        s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1;
        ASK(D_ADVANCE, att);
        if (s->mv.a) {
            advance(s);
            s->siege[b->dst] = 1;
        }
        /* inside a Stronghold under siege at most 5 units: the owner removes the excess, back to the reinforcements
           (RB p. 26-27; the BehindMedia referee's Remove excess unit) */
        while (units(s, def, b->dst) > (s->siege[b->dst] && s->ctrl[b->dst] == def ? 5 : 10)) {
            int r = b->dst, lo = def == FP ? 0 : NFPNAT, hi = def == FP ? NFPNAT : NNAT;
            s->nopt = 0;
            for (int n = lo; n < hi; n++) { if (s->reg[r][n]) s->opt[s->nopt++] = (uint8_t)n; if (s->eli[r][n]) s->opt[s->nopt++] = (uint8_t)(8 + n); }
            s->qreg = (uint8_t)r; s->qtag = Q_EXCESS;
            if (s->nopt > 1) ASK(D_CARD_OPTION, def); else s->mv.a = s->opt[0];
            s->qtag = Q_NONE;
            { int r2 = b->dst, n = s->mv.a & 7;
              if (s->mv.a >= 8) { s->eli[r2][n]--; s->pool_eli[n]++; } else { s->reg[r2][n]--; s->pool_reg[n]++; } }
        }
    } else if (b->ended == 2) {
        if (!units(s, def, b->dst)) wipe_leaders(s, def, b->dst, 1);
        if (b->siege) {                                               /* stronghold falls */
            if (!units(s, def, b->dst)) {
                s->siege[b->dst] = 0;
                if (units(s, att, b->src) + rear_units(s) > 0) { set_ctrl(s, b->dst, att); army_entered(s, att, b->dst); }   /* a mutual wipe captures nothing */
            }
        } else if (b->sortie) {
            /* besiegers gone: fix_siege below lifts the siege */
        } else if (units(s, att, b->src) && can_enter_army(s, att, b->src, b->dst) &&
                   units(s, att, b->src) + units(s, att, b->dst) <= 10) {
            sorcerer(s);
            s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1;
            ASK(D_ADVANCE, att);
            if (s->mv.a) advance(s);
        }
    }
    sorcerer(s);
    restore_rearguard(s);
    fix_siege(s, b->src); fix_siege(s, b->dst);
    b->active = 0;
    CO_END
}

/* ---------- one combat round ---------- */
static int roll_hits(State *s, int n, int thr, int mod, int *ones, int *misses) {
    int h = 0;
    for (int i = 0; i < n; i++) {
        int d = d6(s);
        if (d == 6 || (d != 1 && d + mod >= thr)) h++;
        else { if (misses) (*misses)++; if (d == 1 && ones) (*ones)++; }
    }
    return h;
}
static int base_threshold(const State *s, int side) {
    const Battle *b = &s->b;
    if (side != b->att) return 5;
    if (b->siege) return 6;
    int t = REGION[b->dst].settle;
    if (b->round == 1 && !b->sortie && (t == S_FORT || (t == S_CITY && s->ctrl[b->dst] != side))) return 6;
    return 5;
}
static int has_cc(const State *s, int side, int cc) { return s->b.card[side] != 255 && CARD[s->b.card[side]].combat == cc; }

/* frame vars: 0 phase scratch, 1 fp misses, 2 sh misses, 3 fp ones-locked, 4 self hits chosen, 5 fp dice, 6 sh dice */
void fr_round(State *s, Frame *f) {
    Battle *b = &s->b;
    int att = b->att, def = 1 - att;
    int fr = att == FP ? b->src : b->dst, sr = att == SH ? b->src : b->dst;
    CO_BEGIN
    b->card[0] = b->card[1] = 255; b->hits[0] = b->hits[1] = 0; b->pre_fp_hits = 0;
    b->forfeit[0] = b->forfeit[1] = 0; b->wop_comp = 255; b->reroll_hits[0] = b->reroll_hits[1] = 0;
    /* 1: combat cards, attacker first */
    for (f->v[0] = 0; f->v[0] < 2; f->v[0]++) {
        int side = f->v[0] == 0 ? att : def;
        s->nopt = 0; s->opt[s->nopt++] = 255;
        int blocked = side == FP && ((b->nocard_r1 && b->round == 1 && !comps_at(s, fr)) ||
                                     (s->card_where[CARD_DENETHORS_FOLLY] == W_TABLE && b->dst == R_MINAS_TIRITH));
        if (!blocked) {
            for (int c = side * 48; c < side * 48 + 48; c++) if (s->card_where[c] == W_HAND && cc_playable(s, side, c)) s->opt[s->nopt++] = (uint8_t)c;
            if (side == SH && s->card_where[CARD_BALROG_OF_MORIA] == W_TABLE && cc_playable(s, SH, CARD_BALROG_OF_MORIA)) s->opt[s->nopt++] = CARD_BALROG_OF_MORIA;
        }
        if (s->nopt > 1) {
            ASK(D_BATTLE_CARD, f->v[0] == 0 ? att : def);
            side = f->v[0] == 0 ? att : def;
            if (s->mv.a != 255) {
                b->card[side] = s->mv.a; discard_card(s, s->mv.a);
                if (side == SH && b->round == 1 && s->minion_loc[M_WK] == sr) b->sorc = (uint8_t)(CARD_DECK(s->mv.a) + 1);   /* Sorcerer: drawn after the round */
            }
        }
    }
    /* cancellations (initiative 0; defender's card first) */
    for (int k = 0; k < 2; k++) {
        int side = k == 0 ? def : att;
        if (side == FP && has_cc(s, FP, CC_DARING_DEFIANCE) && b->card[SH] != 255 && !b->white_rider) {
            for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == fr) b->forfeit[FP] += comp_leadership(s, c);
            b->card[SH] = 255; b->dd = 1;
        }
        if (side == SH && has_cc(s, SH, CC_SWARM_OF_BATS) && b->card[FP] != 255) { b->card[FP] = 255; b->card[SH] = 255; }
    }
    /* Words of Power (initiative 1): the Shadow chooses the Companion (card text: "Choose a Companion") */
    if (has_cc(s, SH, CC_WORDS_OF_POWER) && comps_at(s, fr)) {
        s->nopt = 0;
        for (int c = 0; c < NCOMP; c++) if ((comps_at(s, fr) >> c) & 1) s->opt[s->nopt++] = (uint8_t)c;
        s->qtag = Q_WORDS_OF_POWER;
        ASK(D_CARD_OPTION, SH);
        s->qtag = Q_NONE;
        b->wop_comp = s->mv.a; b->forfeit[FP] += (uint8_t)comp_leadership(s, s->mv.a);
    }
    /* Scouts (initiative 1) */
    if (has_cc(s, FP, CC_SCOUTS)) {
        retreat_options(s, FP, b->dst);
        if (REGION[b->dst].settle == S_STRONGHOLD && s->ctrl[b->dst] == FP) s->opt[s->nopt++] = b->dst;
        if (s->nopt) {
            if (s->nopt > 1) ASK(D_RETREAT, FP); else s->mv.a = s->opt[0];
            if (s->mv.a == b->dst) b->ended = 4; else { relocate_army(s, FP, b->dst, s->mv.a); b->ended = 2; }
            break;
        }
    }
    /* pre-combat attacks (initiative 2; defender first) */
    for (f->v[0] = 0; f->v[0] < 2; f->v[0]++) {
        int side = f->v[0] == 0 ? def : att, h = 0;
        if (side == FP) {
            if (has_cc(s, FP, CC_SUDDEN_STRIKE)) { int n = army_leadership(s, FP, fr) - b->forfeit[FP]; h = roll_hits(s, n > 5 ? 5 : n < 0 ? 0 : n, base_threshold(s, FP), 0, 0, 0); }
            if (has_cc(s, FP, CC_CHARGE)) { int n = elites(s, FP, fr); h = roll_hits(s, n > 5 ? 5 : n, base_threshold(s, FP), 0, 0, 0); }
            b->pre_fp_hits = (uint8_t)h;
            if (h) CALL(push_casualty(s, SH, sr, h, 0));
        } else if (has_cc(s, SH, CC_DURINS_BANE)) {
            h = roll_hits(s, 3, 4, 0, 0, 0);
            b->pre_sh_hits = (uint8_t)h;
            if (h) CALL(push_casualty(s, FP, fr, h, 0));
        }
    }
    if (!units(s, FP, fr) || !units(s, SH, sr)) break;
    /* Relentless Assault (initiative 3) */
    f->v[4] = 0;
    if (has_cc(s, SH, CC_RELENTLESS_ASSAULT)) {
        int mx = 2;         /* "up to two hits against your units" (no other limit: the BehindMedia referee's reading) */
        s->nopt = 0;
        for (int k = 0; k <= mx; k++) s->opt[s->nopt++] = (uint8_t)k;
        if (s->nopt > 1) {
            ASK(D_SELF_HITS, SH);
            f->v[4] = s->mv.a;
            if (f->v[4]) CALL(push_casualty(s, SH, sr, f->v[4], 0));
        }
    }
    f->v[6] = 255;
    if (has_cc(s, SH, CC_DREAD_AND_DESPAIR) && naz_lead_now(s) >= 1) {
        s->nopt = 0;
        for (int k = 1; k <= naz_lead_now(s) && s->nopt < MAXOPT; k++) s->opt[s->nopt++] = (uint8_t)k;
        ASK(D_SELF_HITS, SH); f->v[6] = s->mv.a;   /* asked even with one answer, as the BehindMedia referee does */
    }
    {   /* 2+3: combat roll and leader re-roll; 5: hits after the re-roll */
        int comps = battle_comps(s);
        int caps = CAPTAINS & ~(s->gandalf_white ? 1 << C_GANDALF : 0);   /* Gandalf the White is no Captain of the West */
        int fstr = units(s, FP, fr) + __builtin_popcount((unsigned)(comps & caps)), sstr = units(s, SH, sr);
        int fd = fstr > 5 ? 5 : fstr, sd = sstr > 5 ? 5 : sstr;
        int fmod = 0, smod = 0, frmod = 0, srmod = 0;
        int nl = naz_lead_now(s);
        if (has_cc(s, FP, CC_IT_IS_A_GIFT)) { fmod++; frmod++; }
        if (has_cc(s, FP, CC_SERVANT_OF_THE_SECRET_FIRE) || has_cc(s, FP, CC_VALOUR)) fmod++;
        if (has_cc(s, FP, CC_ENTS_RAGE)) fmod += 2;
        if (has_cc(s, FP, CC_ADVANTAGEOUS_POSITION)) smod--;
        if (has_cc(s, FP, CC_DAYLIGHT) && sd > 3) sd = 3;
        if (has_cc(s, FP, CC_HUORN_DARK) && sd > 2) sd = 2;
        if (has_cc(s, FP, CC_BRAVE_STAND)) sd -= __builtin_popcount((unsigned)comps_at(s, fr));
        if (has_cc(s, SH, CC_ONE_FOR_THE_DARK_LORD)) { smod++; srmod++; }
        if (has_cc(s, SH, CC_CRUEL_AS_DEATH) && nl >= 2) { b->forfeit[SH] += 2; smod++; }
        if (has_cc(s, SH, CC_DREAD_AND_DESPAIR) && f->v[6] != 255) { int k = f->v[6]; b->forfeit[SH] += (uint8_t)k; fd -= k; if (fd < 1) fd = 1; }
        if (has_cc(s, SH, CC_DEVILRY_OF_ORTHANC) || has_cc(s, SH, CC_MUMAKIL)) smod++;
        if (has_cc(s, SH, CC_DEADLY_STRIFE)) { fmod += 2; smod += 2; frmod += 2; srmod += 2; }
        if (has_cc(s, SH, CC_DESPERATE_BATTLE)) { fmod++; smod++; frmod++; srmod++; }
        if (has_cc(s, SH, CC_SWARM_OF_BATS)) srmod++;                 /* only still set if FP played no card */
        smod += f->v[4];
        if (b->help_unlooked) sd -= b->help_unlooked;
        if (fd < 1) fd = 1;
        if (sd < 1) sd = 1;
        /* Foul Stench (initiative 3, before the roll): with the Nazgul Leadership at least the Free Peoples', no Free
           Peoples re-roll; it negates Mighty Attack, Anduril, Blade of Westernesse and Fateful Strike (Card Almanac p. 43) */
        int flead = army_leadership(s, FP, fr) - b->forfeit[FP]; if (flead < 0) flead = 0;
        if (b->white_rider) flead = flead > 0 ? flead - 1 : 0;
        int foul = has_cc(s, SH, CC_FOUL_STENCH) && naz_lead_now(s) >= flead;
        if (has_cc(s, SH, CC_THEY_ARE_TERRIBLE) && nl >= 1) { b->forfeit[SH] += 1; srmod++; }
        int fthr = base_threshold(s, FP), sthr = base_threshold(s, SH);
        int fmiss = 0, smiss = 0, sones = 0;
        wotr_rk_d6 = 7; int fh = roll_hits(s, fd, fthr, fmod, 0, &fmiss);
        wotr_rk_d6 = 8; int sh = roll_hits(s, sd, sthr, smod, &sones, &smiss);
        if (has_cc(s, FP, CC_CONFUSION)) { fh += sones; smiss -= sones; }
        /* leader re-rolls */
        int slead = army_leadership(s, SH, sr) - (b->white_rider ? nazgul_leadership(s, sr) : b->forfeit[SH]);
        if (slead < 0) slead = 0;
        if (foul) flead = 0;
        else {
            /* the Companions still able to forfeit their Leadership */
            int lead = 0;
            for (int c = 0; c < NCOMP; c++) if ((comps >> c) & 1 && !b->dd && comp_leadership(s, c) > 0 && !(b->white_rider && c == C_GANDALF && s->gandalf_white)) lead |= 1 << c;
            /* Mighty Attack: the Companion of the lowest Leadership forfeits it; one missed die becomes a hit */
            if (has_cc(s, FP, CC_MIGHTY_ATTACK) && lead) {
                int best = -1;
                for (int c = 0; c < NCOMP; c++) if ((lead >> c) & 1 && (best < 0 || comp_leadership(s, c) < comp_leadership(s, best))) best = c;
                flead -= comp_leadership(s, best);
                if (fmiss > 0) { fmiss--; fh++; }
            }
            /* Anduril: Strider forfeits his Leadership (Aragorn: 2); one missed die (Aragorn: two) becomes a hit */
            if (has_cc(s, FP, CC_ANDURIL) && (lead >> C_STRIDER) & 1) {
                int k = s->aragorn ? 2 : 1;
                flead -= comp_leadership(s, C_STRIDER);
                if (k > fmiss) k = fmiss;
                fmiss -= k; fh += k;
            }
            if (flead < 0) flead = 0;
        }
        wotr_rk_d6 = 9; int frr = roll_hits(s, flead < fmiss ? (flead > 5 ? 5 : flead) : (fmiss > 5 ? 5 : fmiss), fthr, frmod, 0, 0);
        wotr_rk_d6 = 10; int srr = roll_hits(s, slead < smiss ? (slead > 5 ? 5 : slead) : (smiss > 5 ? 5 : smiss), sthr, srmod, 0, 0);
        wotr_rk_d6 = 0; b->reroll_hits[FP] = (uint8_t)frr; b->reroll_hits[SH] = (uint8_t)srr;
        /* initiative 5, the defender's card first: No Quarter / Nameless Wood with a hit in the roll or re-roll, and
           Mumakil with more hits scored this round than the opponent (pre-combat hits included) */
        int fsc = b->pre_fp_hits + fh + frr, ssc = b->pre_sh_hits + sh + srr, fadd = 0, sadd = 0;
        for (int k = 0; k < 2; k++) {
            int side = k == 0 ? def : att;
            if (side == FP) {
                if (has_cc(s, FP, CC_NO_QUARTER) && fh + frr > 0) fadd += 1;
                if (has_cc(s, FP, CC_NAMELESS_WOOD) && fh + frr > 0) fadd += 2;
                fsc += fadd;
            } else if (has_cc(s, SH, CC_MUMAKIL) && ssc > fsc) { sadd++; ssc++; }
        }
        b->hits[FP] = (int8_t)(fh + frr + fadd); b->hits[SH] = (int8_t)(sh + srr + sadd);
    }
    /* initiative 6 (before casualties), the defender's card first: the player whose card it is chooses */
    for (f->v[0] = 0; f->v[0] < 2; f->v[0]++) {
        if ((f->v[0] == 0 ? def : att) == FP) {
            /* Blade of Westernesse: one re-roll hit eliminates a Minion of the Free Peoples' choice */
            if (has_cc(s, FP, CC_BLADE_OF_WESTERNESSE) && b->reroll_hits[FP] > 0 && minions_at(s, sr)) {
                s->nopt = 0;
                for (int m = 0; m < NMINION; m++) if (s->minion_loc[m] == sr) s->opt[s->nopt++] = (uint8_t)(Q_MINION + m);
                if (s->nopt > 1) { s->qtag = Q_BLADE; ASK(D_CARD_OPTION, FP); s->qtag = Q_NONE; } else s->mv.a = s->opt[0];
                kill_minion(s, s->mv.a - Q_MINION);
                if (b->hits[FP] > 0) b->hits[FP]--;
            }
            /* Fateful Strike: a re-roll hit eliminates a Nazgul, two a Minion (the Witch-king is both): the Free
               Peoples' choice */
            if (has_cc(s, FP, CC_FATEFUL_STRIKE) && b->reroll_hits[FP] > 0) {
                s->nopt = 0;
                if (s->naz[sr]) s->opt[s->nopt++] = Q_NAZGUL;
                if (b->reroll_hits[FP] >= 2) for (int m = 0; m < NMINION; m++) if (s->minion_loc[m] == sr) s->opt[s->nopt++] = (uint8_t)(Q_MINION + m);
                if (s->nopt) {
                    if (s->nopt > 1) { s->qtag = Q_FATEFUL_STRIKE; ASK(D_CARD_OPTION, FP); s->qtag = Q_NONE; } else s->mv.a = s->opt[0];
                    if (s->mv.a == Q_NAZGUL) { s->naz[sr]--; s->pool_naz++; } else kill_minion(s, s->mv.a - Q_MINION);
                }
            }
            if (has_cc(s, FP, CC_SHIELD_WALL) && b->hits[SH] >= 2) b->hits[SH]--;
            /* Heroic Death: a Leader cancels one hit, a Companion up to his Level (Gandalf the White: 3), or none */
            if (has_cc(s, FP, CC_HEROIC_DEATH) && b->hits[SH] > 0) {
                s->nopt = 0;
                for (int n = 0; n < NFPNAT; n++) if (s->ldr[fr][n]) s->opt[s->nopt++] = (uint8_t)(Q_LEADER + n);
                for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == fr) s->opt[s->nopt++] = (uint8_t)c;
                if (s->nopt) {
                    s->opt[s->nopt++] = 255;
                    s->qtag = Q_HEROIC_DEATH; ASK(D_CARD_OPTION, FP); s->qtag = Q_NONE;
                    if (s->mv.a >= Q_LEADER && s->mv.a < Q_LEADER + NFPNAT) { s->ldr[fr][s->mv.a - Q_LEADER]--; b->hits[SH]--; }
                    else if (s->mv.a < NCOMP) {
                        int l = comp_level(s, s->mv.a); if (l > 3) l = 3;
                        kill_comp(s, s->mv.a);
                        b->hits[SH] = (int8_t)(b->hits[SH] > l ? b->hits[SH] - l : 0);
                    }
                }
            }
        } else if (has_cc(s, SH, CC_BLACK_BREATH) && b->reroll_hits[SH] > 0) {
            /* Black Breath: the Shadow may eliminate a Leader, or a Companion of Level up to its re-roll hits */
            s->nopt = 0;
            for (int n = 0; n < NFPNAT; n++) if (s->ldr[fr][n]) s->opt[s->nopt++] = (uint8_t)(Q_LEADER + n);
            for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == fr && (comp_level(s, c) > 3 ? 3 : comp_level(s, c)) <= b->reroll_hits[SH]) s->opt[s->nopt++] = (uint8_t)c;
            if (s->nopt) {
                s->opt[s->nopt++] = 255;
                s->qtag = Q_BLACK_BREATH; ASK(D_CARD_OPTION, SH); s->qtag = Q_NONE;
                if (s->mv.a >= Q_LEADER && s->mv.a < Q_LEADER + NFPNAT) s->ldr[fr][s->mv.a - Q_LEADER]--;
                else if (s->mv.a < NCOMP) kill_comp(s, s->mv.a);
            }
        }
    }
    /* 4: casualties, attacker first */
    CALL(push_casualty(s, att, b->src, b->hits[def], 0));
    CALL(push_casualty(s, def, b->dst, b->hits[att], 0));
    if (!units(s, FP, fr) || !units(s, SH, sr)) break;
    /* after-casualty effects (initiative 7) */
    if (has_cc(s, SH, CC_ONSLAUGHT)) {
        int mx = 0;         /* up to four, no more than the units can take: each hit taken earns a die (Card Almanac p. 44) */
        for (int n = NFPNAT; n < NNAT; n++) mx += s->reg[sr][n] + 2 * s->eli[sr][n];
        if (mx > 4) mx = 4;
        s->nopt = 0;
        for (int k = 0; k <= mx; k++) s->opt[s->nopt++] = (uint8_t)k;
        f->v[4] = 0;
        if (s->nopt > 1) { ASK(D_SELF_HITS, SH); f->v[4] = s->mv.a; }
        if (f->v[4]) {
            CALL(push_casualty(s, SH, sr, f->v[4], 0));
            f->v[5] = (uint8_t)roll_hits(s, f->v[4], 4, 0, 0, 0);
            if (f->v[5]) CALL(push_casualty(s, FP, fr, f->v[5], 0));
        }
    }
    if (has_cc(s, SH, CC_WE_COME_TO_KILL)) {
        int n = elites(s, SH, sr);
        f->v[5] = (uint8_t)roll_hits(s, n > 5 ? 5 : n, 5, 0, 0, 0);
        if (f->v[5]) CALL(push_casualty(s, FP, fr, f->v[5], 0));
    }
    if (has_cc(s, SH, CC_GREAT_HOST) && units(s, SH, sr) >= 2 * units(s, FP, fr)) CALL(push_casualty(s, FP, fr, 1, 0));
    CO_END
}
