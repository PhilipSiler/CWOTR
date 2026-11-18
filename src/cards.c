/* cards.c -- the 96 event cards: play conditions and event effects */
#include "wotr.h"

#define ON_TABLE(c) (s->card_where[c] == W_TABLE)
#define YESNO() do { s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1; } while (0)
/* ask only when there is a real choice; answer in s->mv.a (255 if no option) */
#define CHOOSE(kind, who) do { if (s->nopt > 1) { ASK(kind, who); } else s->mv.a = s->nopt ? s->opt[0] : 255; } while (0)

static int strider_army_region(const State *s) { int r = s->comp_loc[C_STRIDER]; return (r < NREG && units(s, FP, r)) ? r : -1; }
static int naz_in_army(const State *s, int r, int with_wk) { return units(s, SH, r) && (s->naz[r] || (with_wk && s->minion_loc[M_WK] == r)); }
static int adj_or_same_fp_army(const State *s, int r, int need_comp) {
    if (units(s, FP, r) && (!need_comp || comps_at(s, r))) return 1;
    for (int i = 0; i < REGION[r].nadj; i++) { int v = REGION[r].adj[i]; if (units(s, FP, v) && (!need_comp || comps_at(s, v))) return 1; }
    return 0;
}
static int shadow_nations_in(const State *s, int r) {
    int k = 0;
    for (int n = NFPNAT; n < NNAT; n++) k += (s->reg[r][n] + s->eli[r][n]) > 0;
    return k;
}
static int besieging_shadow(const State *s, int r) { return s->siege[r] && s->ctrl[r] == FP && units(s, SH, r) > 0; }
static int fsp_in_fp_settlement(const State *s) { return is_settlement(s->fsp_loc) && orig_side(s->fsp_loc) == FP; }
static const uint8_t GONDOR_COAST[4] = {R_ANFALAS, R_DOL_AMROTH, R_LAMEDON, R_PELARGIR};
static const uint8_t EAST_EDGE[4] = {R_EAST_RHUN, R_SOUTH_RHUN, R_KHAND, R_FAR_HARAD};
static const uint8_t SE_SETTLE[5] = {R_FAR_HARAD, R_NEAR_HARAD, R_NORTH_RHUN, R_SOUTH_RHUN, R_UMBAR};
static const uint8_t ELF_SH[4] = {R_LORIEN, R_RIVENDELL, R_GREY_HAVENS, R_WOODLAND_REALM};

/* regions an army in src could reach in <= steps through regions free for movement */
static int army_reach(const State *s, int side, int src, int steps, uint8_t *out) {
    uint8_t d[NREG], q[NREG]; int qh = 0, qt = 0, cnt = 0;
    memset(d, 0xFF, NREG); memset(out, 0, NREG);
    d[src] = 0; q[qt++] = (uint8_t)src;
    while (qh < qt) {
        int u = q[qh++];
        if (d[u] >= steps) continue;
        for (int i = 0; i < REGION[u].nadj; i++) {
            int v = REGION[u].adj[i];
            if (d[v] != 0xFF || !can_enter_army(s, side, u, v)) continue;
            d[v] = d[u] + 1; q[qt++] = (uint8_t)v; out[v] = 1; cnt++;
        }
    }
    return cnt;
}
static void shadows_gather_opts(State *s, int src, int steps) {
    uint8_t reach[NREG];
    army_reach(s, SH, src, steps, reach);
    s->nopt = 0;
    for (int r = 0; r < NREG; r++)
        if (reach[r] && units(s, SH, r) && !(s->siege[r] && s->ctrl[r] == SH) && units(s, SH, r) + units(s, SH, src) <= 10) s->opt[s->nopt++] = (uint8_t)r;
}
static int woses_dest(const State *s, int r) {
    if (r == R_MINAS_TIRITH) return s->ctrl[r] == FP && !s->siege[r] && can_enter_army(s, FP, r, r);
    if (s->ctrl[R_MINAS_TIRITH] == FP && !s->siege[R_MINAS_TIRITH]) return 0;
    return DIST[r][R_MINAS_TIRITH] == 1 && can_enter_army(s, FP, r, r);
}
static void rec_unit(State *s, int r, int n) { if (!recruit(s, r, n, 1)) recruit(s, r, n, 0); }
static void rec_n(State *s, int r, int n, int k) { if (free_for_recruit(s, NAT_SIDE(n), r)) for (int i = 0; i < k; i++) recruit(s, r, n, 0); }
static int attack_list(const State *s, int filter, Move *out) {
    Move z; memset(&z, 0, sizeof z); z.type = M_ATTACK;
    Move tmp[MAXMOVES];
    int k = gen_attacks(s, SH, 0, z, tmp), n = 0;
    for (int i = 0; i < k && n < MAXOPT; i++) {
        int src = tmp[i].a;
        if (filter == 1 && !naz_in_army(s, src, 1)) continue;
        if (filter == 2 && s->minion_loc[M_WK] != src) continue;
        out[n++] = tmp[i];
    }
    return n;
}

int card_playable(const State *s, int c) {
    int st = s->comp_loc[C_STRIDER], gw = s->gandalf_white && s->comp_loc[C_GANDALF] < NREG;
    int wk = s->minion_loc[M_WK], sar = s->minion_loc[M_SARUMAN] < NREG;
    if (CARD[c].table && ON_TABLE(c)) return 0;
    switch (c) {
    case CARD_AXE_AND_BOW: return s->comp_loc[C_GIMLI] == LOC_FSP || s->comp_loc[C_LEGOLAS] == LOC_FSP;
    case CARD_HORN_OF_GONDOR: return s->comp_loc[C_BOROMIR] == LOC_FSP;
    case CARD_WIZARDS_STAFF: return s->comp_loc[C_GANDALF] == LOC_FSP;
    case CARD_I_WILL_GO_ALONE: case CARD_THERE_AND_BACK_AGAIN: return fsp_comps(s) != 0;
    case CARD_GWAIHIR_THE_WINDLORD: case CARD_WE_PROVE_THE_SWIFTER:
        if (fsp_comps(s)) return 1;
        for (int k = 0; k < NCOMP; k++) if (s->comp_loc[k] < NREG) return 1;
        return 0;
    case CARD_CHALLENGE_OF_THE_KING: { int r = strider_army_region(s); return r >= 0 && (REGION[r].nation == GONDOR || REGION[r].nation == ROHAN); }
    case CARD_THE_EAGLES_ARE_COMING:
        for (int r = 0; r < NREG; r++) if (naz_in_army(s, r, 0) && adj_or_same_fp_army(s, r, 1)) return 1;
        return 0;
    case CARD_THE_ENTS_AWAKE_TREEBEARD: case CARD_THE_ENTS_AWAKE_HUORNS: case CARD_THE_ENTS_AWAKE_ENTMOOT:
        return gw && comps_at(s, R_FANGORN) != 0;
    case CARD_DEAD_MEN_OF_DUNHARROW: return st < NREG && REGION[st].nation == ROHAN;
    case CARD_HOUSE_OF_THE_STEWARDS: { int b = s->comp_loc[C_BOROMIR]; return b < NREG && REGION[b].nation == GONDOR; }
    case CARD_THE_GREY_COMPANY: return strider_army_region(s) >= 0;
    case CARD_THE_LAST_BATTLE: { int r = strider_army_region(s); return s->aragorn && r >= 0 && REGION[r].nation >= NFPNAT; }
    case CARD_THE_SPIRIT_OF_MORDOR:
        for (int r = 0; r < NREG; r++) if (shadow_nations_in(s, r) >= 2) return 1;
        return 0;
    case CARD_FARAMIRS_RANGERS:
        return units(s, SH, R_OSGILIATH) || units(s, SH, R_SOUTH_ITHILIEN) || units(s, SH, R_NORTH_ITHILIEN) || units(s, FP, R_OSGILIATH);
    case CARD_WISDOM_OF_ELROND:
        for (int n = 0; n < NFPNAT; n++) if (s->pol[n]) return 1;
        return 0;
    case CARD_THE_RED_ARROW: return s->active[GONDOR];
    case CARD_HELP_UNLOOKED_FOR:
        for (int r = 0; r < NREG; r++) if (besieging_shadow(s, r))
            for (int i = 0; i < REGION[r].nadj; i++) {
                uint8_t mr[NNAT], me[NNAT]; int v = REGION[r].adj[i];
                if (units(s, FP, v) && !s->siege[v] && movable_units(s, FP, v, v, 1, mr, me)) return 1;
            }
        return 0;
    case CARD_PATHS_OF_THE_WOSES:
        if (!at_war(s, ROHAN)) return 0;
        { int any = 0; for (int r = 0; r < NREG; r++) any |= woses_dest(s, r);
          if (!any) return 0; }
        for (int r = 0; r < NREG; r++) if (REGION[r].nation == ROHAN && units(s, FP, r)) return 1;
        return 0;
    case CARD_THROUGH_A_DAY_AND_A_NIGHT:
        for (int k = 0; k < NCOMP; k++) { int r = s->comp_loc[k]; if (r < NREG && units(s, FP, r) && !s->siege[r]) return 1; }
        return 0;
    case CARD_CIRDANS_SHIPS:
        if (!at_war(s, ELVES)) return 0;
        for (int r = 0; r < NREG; r++) if (REGION[r].coastal && units(s, FP, r)) return 1;
        return 0;
    case CARD_ORC_PATROL: case CARD_ISILDURS_BANE: case CARD_FOUL_THING_FROM_THE_DEEP: case CARD_CANDLES_OF_CORPSES:
        return s->mordor >= 0 || !fsp_in_fp_settlement(s);     /* on the Mordor Track the Fellowship is in no region */
    case CARD_THE_NAZGUL_STRIKE: case CARD_NAZGUL_SEARCH: case CARD_CRUEL_WEATHER: return s->mordor < 0 && s->progress >= 1;
    case CARD_MORGUL_WOUND: case CARD_LURE_OF_THE_RING: case CARD_THE_BREAKING_OF_THE_FELLOWSHIP: return s->revealed;
    case CARD_THE_LIDLESS_EYE: return 1;        /* "up to three" dice: none is allowed */
    case CARD_DREADFUL_SPELLS:
        for (int r = 0; r < NREG; r++) if (naz_in_army(s, r, 1) && adj_or_same_fp_army(s, r, 0)) return 1;
        return 0;
    case CARD_GROND_HAMMER_OF_THE_UNDERWORLD: return wk < NREG && besieging_shadow(s, wk) && !protected_region(s, wk);
    case CARD_THE_PALANTIR_OF_ORTHANC: case CARD_WORMTONGUE: case CARD_A_NEW_POWER_IS_RISING: return sar;
    case CARD_THE_BLACK_CAPTAIN_COMMANDS: case CARD_RETURN_OF_THE_WITCH_KING: return wk < NREG;
    case CARD_RETURN_TO_VALINOR:
        for (int i = 0; i < 4; i++) if (s->ctrl[ELF_SH[i]] == SH) return 1;
        return 0;
    case CARD_THE_FIGHTING_URUK_HAI:
        if (!sar) return 0;
        for (int r = 0; r < NREG; r++) if (besieging_shadow(s, r) && s->reg[r][ISENGARD] + s->eli[r][ISENGARD] && !protected_region(s, r)) return 1;
        return 0;
    case CARD_DENETHORS_FOLLY: return besieging_shadow(s, R_MINAS_TIRITH);
    case CARD_THE_DAY_WITHOUT_DAWN: case CARD_THE_SHADOW_IS_MOVING: case CARD_MUSTERINGS_OF_LONG_PLANNED_WAR: return all_at_war(s, SH);
    case CARD_STORMCROW:
        for (int n = 0; n < NFPNAT; n++) {
            if (at_war(s, n)) continue;
            if (s->mordor < 0 && REGION[s->fsp_loc].nation == n) return 1;
            for (int k = 0; k < NCOMP; k++) if (s->comp_loc[k] < NREG && REGION[s->comp_loc[k]].nation == n) return 1;
        }
        return 0;
    case CARD_SHADOWS_GATHER: case CARD_THE_SHADOW_LENGTHENS: {
        State *ms = (State *)s; uint8_t sn = s->nopt, so[MAXOPT]; memcpy(so, s->opt, MAXOPT);   /* scratch use of opt, restored */
        int ok = 0;
        for (int r = 0; r < NREG && !ok; r++) if (units(s, SH, r) && !(s->siege[r] && s->ctrl[r] == SH)) { shadows_gather_opts(ms, r, c == CARD_SHADOWS_GATHER ? 3 : 2); ok = ms->nopt > 0; }
        ms->nopt = sn; memcpy(ms->opt, so, MAXOPT);
        return ok; }
    case CARD_CORSAIRS_OF_UMBAR: return at_war(s, SOUTHRON) && units(s, SH, R_UMBAR) > 0 && !s->siege[R_UMBAR];
    case CARD_RAGE_OF_THE_DUNLENDINGS: case CARD_HALF_ORCS_AND_GOBLIN_MEN: return at_war(s, ISENGARD);
    case CARD_OLOG_HAI: case CARD_HILL_TROLLS: case CARD_PITS_OF_MORDOR: return at_war(s, SAURON);
    case CARD_THE_KING_IS_REVEALED: return s->aragorn && st < NREG;
    case CARD_HORDES_FROM_THE_EAST: return at_war(s, SOUTHRON);
    default: return 1;
    }
}

void push_card(State *s, int c, int by_event_die) {
    Frame *f = push(s, FR_CARD);
    f->v[0] = (uint8_t)c; f->v[8] = (uint8_t)by_event_die;
    if (s->card_where[c] == W_HAND) s->hand_n[CARD_SIDE(c)]--;
    s->card_where[c] = CARD[c].table ? W_TABLE : W_DISCARD;
}

void fr_card(State *s, Frame *f) {
    CO_BEGIN
    CALL(push(s, FR_CARDFX)->v[0] = f->v[0]);
    if (f->v[8] && !s->winner) {
        int c = f->v[0];
        /* (not while card_has_effect is trying the card out: these bonus draws are not the card's own effect) */
        if (!s->rng_max && CARD_SIDE(c) == FP && s->guide == C_GANDALF && s->comp_loc[C_GANDALF] == LOC_FSP) draw_card(s, CARD_DECK(c));
        if (!s->rng_max && CARD_SIDE(c) == SH && ON_TABLE(CARD_THE_PALANTIR_OF_ORTHANC) && c != CARD_THE_PALANTIR_OF_ORTHANC) {
            s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1;          /* either deck */
            ASK(D_CARD_OPTION, SH);
            draw_card(s, 2 + s->mv.a);
        }
    }
    CO_END
}

static void comp_group_opts(State *s, int include_map) {
    int fc = fsp_comps(s);      /* on the Mordor Track separation means elimination (fr_separate) */
    s->nopt = 0;
    for (int a = 0; a < NCOMP; a++) if ((fc >> a) & 1) {
        s->opt[s->nopt++] = (uint8_t)(1 << a);
        for (int b = a + 1; b < NCOMP; b++) if ((fc >> b) & 1) s->opt[s->nopt++] = (uint8_t)((1 << a) | (1 << b));
    }
    if (__builtin_popcount((unsigned)fc) > 2) s->opt[s->nopt++] = (uint8_t)fc;
    if (include_map) {
        int seen = 0;
        for (int c = 0; c < NCOMP; c++) {
            int r = s->comp_loc[c];
            if (r >= NREG || ((seen >> c) & 1)) continue;
            int all = comps_at(s, r); seen |= all;
            for (int k = 0; k < NCOMP; k++) if ((all >> k) & 1) s->opt[s->nopt++] = (uint8_t)(0x80 | (1 << k));
            if (__builtin_popcount((unsigned)all) > 1) s->opt[s->nopt++] = (uint8_t)(0x80 | all);
        }
    }
}
static int first_comp(int mask) { for (int c = 0; c < NCOMP; c++) if ((mask >> c) & 1) return c; return 0; }

/* frame vars: 0 card, 1..7 scratch */
void fr_cardfx(State *s, Frame *f) {
    int c = f->v[0];
    CO_BEGIN
    /* ================= Free Peoples character ================= */
    if (c == CARD_ELVEN_CLOAKS) enter_special_tile(s, T_CLOAKS);
    else if (c == CARD_ELVEN_ROPE) enter_special_tile(s, T_ROPE);
    else if (c == CARD_PHIAL_OF_GALADRIEL) enter_special_tile(s, T_PHIAL);
    else if (c == CARD_SMEAGOL_HELPS_NICE_MASTER) enter_special_tile(s, T_SMEAGOL);
    else if (c == CARD_SHELOBS_LAIR) enter_special_tile(s, T_SHELOB);
    else if (c == CARD_THE_RING_IS_MINE) enter_special_tile(s, T_RING_MINE);
    else if (c == CARD_ON_ON_THEY_WENT) enter_special_tile(s, T_ON_ON);
    else if (c == CARD_GIVE_IT_TO_USS) enter_special_tile(s, T_GIVE_USS);
    else if (c == CARD_ATHELAS) {
        int thr = s->guide == C_STRIDER ? 3 : 5, h = 0;
        for (int i = 0; i < 3; i++) h += d6(s) >= thr;
        heal(s, h);
    } else if (c == CARD_THERE_IS_ANOTHER_WAY) {
        heal(s, 1);
        if (s->guide == GOLLUM) {
            s->nopt = 0; s->opt[s->nopt++] = 0;
            if (s->revealed) s->opt[s->nopt++] = 1; else if (s->mordor >= 0 || s->progress < 12) s->opt[s->nopt++] = 2;
            CHOOSE(D_CARD_OPTION, FP);
            if (s->mv.a == 1) { s->revealed = 0; s->fsp_acted = 1; }
            else if (s->mv.a == 2) CALL(push(s, FR_FSP_MOVE));
        }
    } else if (c == CARD_BILBOS_SONG) heal(s, s->guide == GOLLUM ? 2 : 1);
    else if (c == CARD_MIRROR_OF_GALADRIEL) {
        if (s->dice[FP][F_CHAR]) { s->dice[FP][F_CHAR]--; s->dice[FP][F_SPECIAL]++; }
        if (s->mordor < 0 && s->fsp_loc == R_LORIEN && s->ctrl[R_LORIEN] == FP) heal(s, 1);
    } else if (c == CARD_CHALLENGE_OF_THE_KING) {
        int t[3], eyes = 0, n = 0;
        for (int i = 0; i < 3; i++) {
            int x = draw_tile(s);
            if (x < 0) break;
            if (x < NSTD_TILE) s->tile_out[x]--;        /* held aside until all three are drawn */
            t[n++] = x; eyes += (x == T_EYE || x == T_RING_MINE);
        }
        for (int i = 0; i < n; i++) {
            int x = t[i], eye = (x == T_EYE || x == T_RING_MINE);
            if (eye && !(eyes == 3 && n == 3)) continue;  /* Eye tiles are removed for good */
            s->tile_pool[x]++;
        }
        if (eyes == 3 && n == 3) kill_comp(s, C_STRIDER);
    } else if (c == CARD_I_WILL_GO_ALONE || c == CARD_THERE_AND_BACK_AGAIN) {
        comp_group_opts(s, 0);
        CHOOSE(D_CARD_COMP, FP);
        if (s->mv.a != 255) CALL(push_separate(s, s->mv.a, 1, 0, 0));
        if (c == CARD_I_WILL_GO_ALONE) heal(s, 1);
        else {
            int g = s->comp_loc[C_GIMLI], l = s->comp_loc[C_LEGOLAS];
            if (g == R_DALE || g == R_EREBOR || g == R_WOODLAND_REALM || l == R_DALE || l == R_EREBOR || l == R_WOODLAND_REALM) {
                activate(s, DWARVES); activate(s, NORTH);
                advance_pol(s, DWARVES, 0); advance_pol(s, ELVES, 0); advance_pol(s, NORTH, 0);
            }
        }
    } else if (c == CARD_GWAIHIR_THE_WINDLORD || c == CARD_WE_PROVE_THE_SWIFTER) {
        comp_group_opts(s, 1);
        CHOOSE(D_CARD_COMP, FP);
        if (s->mv.a != 255 && !(s->mv.a & 0x80)) {
            if (c == CARD_GWAIHIR_THE_WINDLORD) CALL(push_separate(s, s->mv.a, 0, 1, 4));
            else CALL(push_separate(s, s->mv.a, 2, 1, 0));
        } else if (s->mv.a != 255) {
            f->v[1] = s->mv.a & 0x7F;
            {
                uint8_t reach[NREG]; int lvl = 0, from = s->comp_loc[first_comp(f->v[1])];
                for (int k = 0; k < NCOMP; k++) if ((f->v[1] >> k) & 1 && comp_level(s, k) > lvl) lvl = comp_level(s, k);
                comp_reach(s, from, c == CARD_GWAIHIR_THE_WINDLORD ? 4 : lvl + 2, 1, reach);
                s->nopt = (uint8_t)comp_dests(s, f->v[1], reach, from, s->opt);
            }
            CHOOSE(D_CARD_REGION, FP);
            if (s->mv.a != 255) place_comps(s, f->v[1], s->mv.a);
        }
    } else if (c == CARD_THE_EAGLES_ARE_COMING) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) if (naz_in_army(s, r, 0) && adj_or_same_fp_army(s, r, 1)) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_REGION, FP);
        f->v[1] = s->mv.a;
        {
            int r = f->v[1], n = s->naz[r] > 5 ? 5 : s->naz[r], k = 0;
            for (int i = 0; i < n; i++) k += d6(s) >= 5;
            s->naz[r] -= (uint8_t)k; s->pool_naz += (uint8_t)k;
            s->nopt = 0;
            if (s->naz[r]) for (int q = 0; q < NREG; q++)
                if (REGION[q].nation == SAURON && REGION[q].settle == S_STRONGHOLD && s->ctrl[q] == SH) s->opt[s->nopt++] = (uint8_t)q;
        }
        if (s->nopt) {
            CHOOSE(D_CARD_REGION, SH);
            if (s->mv.a != f->v[1]) { s->naz[s->mv.a] += s->naz[f->v[1]]; s->naz[f->v[1]] = 0; }
        }
    } else if (c == CARD_THE_ENTS_AWAKE_TREEBEARD || c == CARD_THE_ENTS_AWAKE_HUORNS || c == CARD_THE_ENTS_AWAKE_ENTMOOT) {
        {
            int h = 0;
            for (int i = 0; i < 3; i++) h += d6(s) >= 4;
            if (units(s, SH, R_ORTHANC)) {
                attacked_politics(s, SH, R_ORTHANC);
                if (h) CALL(push_casualty(s, SH, R_ORTHANC, h, 1));
            } else if (s->minion_loc[M_SARUMAN] == R_ORTHANC) kill_minion(s, M_SARUMAN);
        }
        {
            int g = s->comp_loc[C_GANDALF];
            s->nopt = 0;
            if (s->gandalf_white && g < NREG && (g == R_FANGORN || REGION[g].nation == ROHAN)) {
                s->opt[s->nopt++] = 255;
                for (int k = 0; k < 24; k++) if (s->card_where[k] == W_HAND && card_playable(s, k)) s->opt[s->nopt++] = (uint8_t)k;
            }
        }
        if (s->nopt > 1) { ASK(D_CARD_CARD, FP); if (s->mv.a != 255) CALL(push_card(s, s->mv.a, 0)); }
    } else if (c == CARD_DEAD_MEN_OF_DUNHARROW) {
        s->nopt = 3; s->opt[0] = R_ERECH; s->opt[1] = R_LAMEDON; s->opt[2] = R_PELARGIR;
        ASK(D_CARD_REGION, FP);
        f->v[1] = s->mv.a;
        {
            int from = s->comp_loc[C_STRIDER];
            for (int k = 0; k < NCOMP; k++) if (s->comp_loc[k] == from) s->comp_loc[k] = f->v[1];
        }
        if (units(s, SH, f->v[1])) {
            attacked_politics(s, SH, f->v[1]);
            CALL(push_casualty(s, SH, f->v[1], d6(s), 1));
            if (units(s, SH, f->v[1])) {
                s->nopt = 0;
                for (int i = 0; i < REGION[f->v[1]].nadj; i++) {
                    int v = REGION[f->v[1]].adj[i];
                    if (!units(s, FP, v) && !(is_settlement(v) && s->ctrl[v] == FP) && units(s, SH, v) + units(s, SH, f->v[1]) <= 10) s->opt[s->nopt++] = (uint8_t)v;
                }
                if (!s->nopt) CALL(push_casualty(s, SH, f->v[1], 20, 1));
                else { CHOOSE(D_CARD_REGION, SH); relocate_army(s, SH, f->v[1], s->mv.a); }
            }
        }
        if (!units(s, SH, f->v[1])) {
            int k = 0;
            for (int i = 0; i < 3; i++) k += recruit(s, f->v[1], GONDOR, 0);
            if (k) set_ctrl(s, f->v[1], FP);
            place_comps(s, comps_at(s, f->v[1]), f->v[1]);
        }
    } else if (c == CARD_HOUSE_OF_THE_STEWARDS) {
        int b = s->comp_loc[C_BOROMIR];
        if (free_for_recruit(s, FP, b)) rec_unit(s, b, GONDOR);
        draw_card(s, 1); draw_card(s, 1);
    } else if (c == CARD_THE_GREY_COMPANY) {
        int r = s->comp_loc[C_STRIDER];
        for (int n = 0; n < NFPNAT; n++) if (s->reg[r][n] && s->pool_eli[n]) {
            s->reg[r][n]--; s->cas_reg[n]++; s->pool_eli[n]--; s->eli[r][n]++; break;
        }
        draw_card(s, 1); draw_card(s, 1);
    }
    /* ================= Free Peoples strategy ================= */
    else if (c == CARD_A_POWER_TOO_GREAT) advance_pol(s, ELVES, 0);
    else if (c == CARD_THE_POWER_OF_TOM_BOMBADIL) advance_pol(s, NORTH, 0);
    else if (c == CARD_BOOK_OF_MAZARBUL || c == CARD_FEAR_FEAR_FOES) {
        s->comp_moved = 0;
        CALL(push(s, FR_COMPMOVE));
        if (c == CARD_BOOK_OF_MAZARBUL) { if (comps_at(s, R_EREBOR) || comps_at(s, R_ERED_LUIN)) { activate(s, DWARVES); s->pol[DWARVES] = 0; } }
        else if (comps_at(s, R_THE_SHIRE) || comps_at(s, R_BREE)) { activate(s, NORTH); s->pol[NORTH] = 0; }
    } else if (c == CARD_THE_SPIRIT_OF_MORDOR) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) if (shadow_nations_in(s, r) >= 2) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_REGION, FP);
        { int h = 0; for (int i = 0; i < 5; i++) h += d6(s) >= 5; if (h) CALL(push_casualty(s, SH, s->mv.a, h, 0)); }
    } else if (c == CARD_FARAMIRS_RANGERS) {
        s->nopt = 0;
        if (units(s, SH, R_OSGILIATH)) s->opt[s->nopt++] = R_OSGILIATH;
        if (units(s, SH, R_SOUTH_ITHILIEN)) s->opt[s->nopt++] = R_SOUTH_ITHILIEN;
        if (units(s, SH, R_NORTH_ITHILIEN)) s->opt[s->nopt++] = R_NORTH_ITHILIEN;
        if (s->nopt) {
            CHOOSE(D_CARD_REGION, FP);
            { int h = 0; for (int i = 0; i < 3; i++) h += d6(s) >= 5; if (h) CALL(push_casualty(s, SH, s->mv.a, h, 0)); }
        }
        if (units(s, FP, R_OSGILIATH) && !units(s, SH, R_OSGILIATH)) { rec_unit(s, R_OSGILIATH, GONDOR); recruit_leader(s, R_OSGILIATH, GONDOR); }
    } else if (c == CARD_WISDOM_OF_ELROND) {
        s->nopt = 0;
        for (int n = 0; n < NFPNAT; n++) if (s->pol[n]) s->opt[s->nopt++] = (uint8_t)n;
        CHOOSE(D_CARD_NATION, FP);
        if (s->mv.a != 255) { activate(s, s->mv.a); advance_pol(s, s->mv.a, 0); }
    } else if (c == CARD_THE_RED_ARROW) {
        advance_pol(s, ROHAN, 0);
        if (free_for_recruit(s, FP, R_EDORAS)) { rec_unit(s, R_EDORAS, ROHAN); recruit_leader(s, R_EDORAS, ROHAN); }
    } else if (c == CARD_HELP_UNLOOKED_FOR) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) if (besieging_shadow(s, r)) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_REGION, FP);
        f->v[1] = s->mv.a;
        s->nopt = 0;
        for (int i = 0; i < REGION[f->v[1]].nadj; i++) {
            uint8_t mr[NNAT], me[NNAT]; int v = REGION[f->v[1]].adj[i];
            if (units(s, FP, v) && !s->siege[v] && movable_units(s, FP, v, v, 1, mr, me)) s->opt[s->nopt++] = (uint8_t)v;
        }
        if (s->nopt) {
            CHOOSE(D_CARD_ARMY, FP);
            { int k = units(s, FP, f->v[1]); push_battle(s, FP, s->mv.a, f->v[1], 10, 10); s->b.help_unlooked = (uint8_t)k; }
            CALL((void)0);
        }
    } else if (c == CARD_PATHS_OF_THE_WOSES) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) if (REGION[r].nation == ROHAN && units(s, FP, r)) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_ARMY, FP);
        f->v[1] = s->mv.a;
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) if (woses_dest(s, r) && units(s, FP, r) + units(s, FP, f->v[1]) <= 10) s->opt[s->nopt++] = (uint8_t)r;
        if (s->nopt) { CHOOSE(D_CARD_REGION, FP); relocate_army(s, FP, f->v[1], s->mv.a); fix_siege(s, f->v[1]); }
    } else if (c == CARD_THROUGH_A_DAY_AND_A_NIGHT) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) if (units(s, FP, r) && comps_at(s, r) && !s->siege[r]) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_ARMY, FP);
        f->v[1] = s->mv.a;
        {
            uint8_t reach[NREG];
            army_reach(s, FP, f->v[1], 2, reach);
            s->nopt = 0;
            for (int r = 0; r < NREG; r++) if (reach[r] && r != f->v[1] && units(s, FP, r) + units(s, FP, f->v[1]) <= 10) s->opt[s->nopt++] = (uint8_t)r;
        }
        if (s->nopt) { CHOOSE(D_CARD_REGION, FP); relocate_army(s, FP, f->v[1], s->mv.a); }
    } else if (c == CARD_CIRDANS_SHIPS) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) if (REGION[r].coastal && units(s, FP, r) && free_for_recruit(s, FP, r)) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_REGION, FP);
        if (s->mv.a != 255) { rec_unit(s, s->mv.a, ELVES); rec_unit(s, s->mv.a, ELVES); }
    } else if (c == CARD_GUARDS_OF_THE_CITADEL) {
        if (free_for_recruit(s, FP, R_MINAS_TIRITH)) { rec_unit(s, R_MINAS_TIRITH, GONDOR); recruit_leader(s, R_MINAS_TIRITH, GONDOR); }
    } else if (c == CARD_CELEBORNS_GALADHRIM) { if (free_for_recruit(s, FP, R_LORIEN)) rec_unit(s, R_LORIEN, ELVES); draw_card(s, 1); }
    else if (c == CARD_RIDERS_OF_THEODEN || c == CARD_EOMER_SON_OF_EOMUND) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) {
            if (REGION[r].nation != ROHAN || !free_for_recruit(s, FP, r)) continue;
            if (c == CARD_RIDERS_OF_THEODEN ? (r == R_EDORAS || comps_at(s, r)) : is_settlement(r)) s->opt[s->nopt++] = (uint8_t)r;
        }
        CHOOSE(D_CARD_REGION, FP);
        if (s->mv.a != 255) { rec_unit(s, s->mv.a, ROHAN); recruit_leader(s, s->mv.a, ROHAN); }
    } else if (c == CARD_GRIMBEORN_THE_OLD_SON_OF_BEORN) { if (free_for_recruit(s, FP, R_CARROCK)) { rec_unit(s, R_CARROCK, NORTH); recruit_leader(s, R_CARROCK, NORTH); } }
    else if (c == CARD_IMRAHIL_OF_DOL_AMROTH) { if (free_for_recruit(s, FP, R_DOL_AMROTH)) { rec_unit(s, R_DOL_AMROTH, GONDOR); recruit_leader(s, R_DOL_AMROTH, GONDOR); } }
    else if (c == CARD_KING_BRANDS_MEN) { rec_n(s, R_DALE, NORTH, 2); draw_card(s, 1); }
    else if (c == CARD_SWORDS_IN_ERIADOR) {
        if (free_for_recruit(s, FP, R_THE_SHIRE)) rec_unit(s, R_THE_SHIRE, NORTH);
        if (free_for_recruit(s, FP, R_ERED_LUIN)) rec_unit(s, R_ERED_LUIN, DWARVES);
        draw_card(s, 1);
    } else if (c == CARD_KINDRED_OF_GLORFINDEL) { if (free_for_recruit(s, FP, R_RIVENDELL)) rec_unit(s, R_RIVENDELL, ELVES); draw_card(s, 1); }
    else if (c == CARD_DAIN_IRONFOOTS_GUARD) { if (free_for_recruit(s, FP, R_EREBOR)) { rec_unit(s, R_EREBOR, DWARVES); recruit_leader(s, R_EREBOR, DWARVES); } }
    else if (c == CARD_THRANDUILS_ARCHERS) { if (free_for_recruit(s, FP, R_WOODLAND_REALM)) rec_unit(s, R_WOODLAND_REALM, ELVES); draw_card(s, 1); }
    /* ================= Shadow character ================= */
    else if (c == CARD_ORC_PATROL) CALL(push_hunt(s, HM_CARD, HF_EYE_DISCARD));
    else if (c == CARD_ISILDURS_BANE) CALL(push_hunt(s, HM_CARD, HF_EYE_DISCARD | HF_NO_REDUCE));
    else if (c == CARD_FOUL_THING_FROM_THE_DEEP) CALL(push_hunt(s, HM_CARD, HF_EYE_DISCARD | HF_RANDOM_FIRST));
    else if (c == CARD_CANDLES_OF_CORPSES) {
        int thr = s->guide == GOLLUM ? 6 : 4, h = 0;
        for (int i = 0; i < 3; i++) h += d6(s) >= thr;
        add_corruption(s, h);
    } else if (c == CARD_THE_NAZGUL_STRIKE || c == CARD_NAZGUL_SEARCH) {
        s->comp_moved = 0;
        CALL(push(s, FR_NAZMOVE));
        if (s->naz[s->fsp_loc] || s->minion_loc[M_WK] == s->fsp_loc) {
            if (c == CARD_NAZGUL_SEARCH) { if (!s->revealed) CALL(push_reveal(s)); }
            else {
                s->nopt = 0; s->opt[s->nopt++] = 0;
                for (int k = 0; k < 24; k++) if (ON_TABLE(k)) { s->opt[s->nopt++] = 1; break; }
                s->opt[s->nopt++] = 2;
                ASK(D_CARD_OPTION, SH);
                if (s->mv.a == 1) {
                    static const uint8_t pri[] = {CARD_MITHRIL_COAT_AND_STING, CARD_WIZARDS_STAFF, CARD_AXE_AND_BOW, CARD_HORN_OF_GONDOR};
                    for (int i = 0; i < 4; i++) if (ON_TABLE(pri[i])) { discard_card(s, pri[i]); break; }
                } else if (s->mv.a == 2) CALL(push_hunt(s, HM_ROLL, 0));
            }
        }
    } else if (c == CARD_CRUEL_WEATHER) {
        s->nopt = 0;
        for (int i = 0; i < REGION[s->fsp_loc].nadj; i++) s->opt[s->nopt++] = REGION[s->fsp_loc].adj[i];
        CHOOSE(D_CARD_REGION, SH);
        s->fsp_loc = s->mv.a;
    } else if (c == CARD_MORGUL_WOUND) add_corruption(s, s->corruption <= 3 ? 2 : 1);
    else if (c == CARD_LURE_OF_THE_RING) {
        if (s->guide == GOLLUM || !fsp_comps(s)) add_corruption(s, 1);
        else {
            {
                int m = fsp_comps(s), k = (int)rnd(s, (uint32_t)__builtin_popcount((unsigned)m)), x = 0;
                for (; x < NCOMP; x++) if ((m >> x) & 1 && k-- == 0) break;
                f->v[1] = (uint8_t)x;
            }
            YESNO();
            ASK(D_CARD_OPTION, FP);
            if (!s->mv.a) add_corruption(s, comp_level(s, f->v[1]));
            else if (f->v[1] >= C_MERRY) { CALL(push_separate(s, 1 << f->v[1], 0, 0, 0)); }
            else kill_comp(s, f->v[1]);
        }
    } else if (c == CARD_THE_BREAKING_OF_THE_FELLOWSHIP) {
        if (s->guide == GOLLUM || !fsp_comps(s)) add_corruption(s, 1);
        else {
            {
                int t = draw_tile(s), n = 0;
                if (t == T_3 || t == T_ON_ON) n = 3; else if (t == T_2 || t == T_2R) n = 2; else if (t == T_1 || t == T_1R || t == T_GIVE_USS) n = 1;
                else if (t == T_SHELOB) n = d6(s);
                int m = fsp_comps(s), pc = __builtin_popcount((unsigned)m);
                if (n > pc) n = pc;
                s->nopt = 0;
                if (n > 0) for (int x = 1; x < 128 && s->nopt < MAXOPT; x++) if ((x & m) == x && __builtin_popcount((unsigned)x) == n) s->opt[s->nopt++] = (uint8_t)x;
            }
            if (s->nopt) {
                CHOOSE(D_CARD_COMP, FP);
                { Frame *g; push_separate(s, s->mv.a, 0, 0, 0); g = &s->st[s->sp - 1]; g->v[4] = 1; }
                CALL((void)0);
            }
        }
    } else if (c == CARD_THE_LIDLESS_EYE) {
        for (f->v[1] = 0; f->v[1] < 3; f->v[1]++) {
            s->nopt = 0; s->opt[s->nopt++] = 255;
            for (int k = 0; k < NFACE; k++) if (s->dice[SH][k]) s->opt[s->nopt++] = (uint8_t)k;
            if (s->nopt == 1) break;
            ASK(D_CARD_OPTION, SH);
            if (s->mv.a == 255) break;
            s->dice[SH][s->mv.a]--; s->hunt_sh++;
        }
    } else if (c == CARD_DREADFUL_SPELLS) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) {
            if (!units(s, FP, r)) continue;
            int ok = naz_in_army(s, r, 1);
            for (int i = 0; i < REGION[r].nadj && !ok; i++) ok = naz_in_army(s, REGION[r].adj[i], 1);
            if (ok) s->opt[s->nopt++] = (uint8_t)r;
        }
        CHOOSE(D_CARD_REGION, SH);
        if (s->mv.a != 255) {
            int r = s->mv.a, best = naz_in_army(s, r, 1) ? nazgul_leadership(s, r) - (s->minion_loc[M_WK] == r) : 0, h = 0;
            for (int i = 0; i < REGION[r].nadj; i++) {
                int v = REGION[r].adj[i], k = naz_in_army(s, v, 1) ? s->naz[v] + (s->minion_loc[M_WK] == v) : 0;
                if (k > best) best = k;
            }
            if (best > 5) best = 5;
            for (int i = 0; i < best; i++) h += d6(s) >= 5;
            if (h) CALL(push_casualty(s, FP, r, h, 0));
        }
    } else if (c == CARD_GROND_HAMMER_OF_THE_UNDERWORLD || c == CARD_THE_FIGHTING_URUK_HAI) {
        s->nopt = 0;
        if (c == CARD_GROND_HAMMER_OF_THE_UNDERWORLD) s->opt[s->nopt++] = s->minion_loc[M_WK];
        else for (int r = 0; r < NREG; r++) if (besieging_shadow(s, r) && s->reg[r][ISENGARD] + s->eli[r][ISENGARD] && !protected_region(s, r)) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_REGION, SH);
        if (s->mv.a != 255) {
            push_battle(s, SH, s->mv.a, s->mv.a, 10, 10);
            s->b.max_rounds = 3; s->b.nocard_r1 = 1;
            CALL((void)0);
        }
    } else if (c == CARD_THE_RINGWRAITHS_ARE_ABROAD || c == CARD_THE_BLACK_CAPTAIN_COMMANDS) {
        f->v[2] = c == CARD_THE_RINGWRAITHS_ARE_ABROAD ? 1 : 2;       /* army filter: needs Nazgul / needs the Witch-king */
        if (c == CARD_THE_BLACK_CAPTAIN_COMMANDS) {
            s->nopt = 2; s->opt[0] = 0; s->opt[1] = 1;
            ASK(D_CARD_OPTION, SH);
            if (s->mv.a == 0) { recruit_naz(s, s->minion_loc[M_WK]); recruit_naz(s, s->minion_loc[M_WK]); f->v[3] = 1; }
        }
        if (!f->v[3]) { s->comp_moved = 0; s->lock[0] = s->lock[1] = 0; CALL(push(s, FR_NAZMOVE)); }
        s->lock[0] = s->lock[1] = 0;
        {
            Move tmp[MAXOPT];
            s->nopt = 0; s->opt[s->nopt++] = 0; s->opt[s->nopt++] = 1;
            if (attack_list(s, f->v[2], tmp)) s->opt[s->nopt++] = 2;
        }
        ASK(D_CARD_OPTION, SH);
        if (s->mv.a == 1) {
            { Frame *g = push(s, FR_ARMY2); g->v[0] = SH; g->v[2] = f->v[2]; }
            CALL((void)0);
            if (c == CARD_THE_RINGWRAITHS_ARE_ABROAD) {
                { Frame *g = push(s, FR_ARMY2); g->v[0] = SH; g->v[2] = f->v[2]; }
                CALL((void)0);
            }
        } else if (s->mv.a == 2) {
            { Move tmp[MAXOPT]; int n = attack_list(s, f->v[2], tmp); s->nopt = 0; for (int i = 0; i < n; i++) s->opt[s->nopt++] = (uint8_t)i; }
            CHOOSE(D_CARD_ARMY, SH);
            { Move tmp[MAXOPT]; attack_list(s, f->v[2], tmp); Move m = tmp[s->mv.a]; push_battle(s, SH, m.a, m.b, m.c, m.d); }
            CALL((void)0);
        }
    }
    /* ================= Shadow strategy ================= */
    else if (c == CARD_RETURN_TO_VALINOR) {
        for (f->v[1] = 0; f->v[1] < 4; f->v[1]++) {
            int r = ELF_SH[f->v[1]], n, h = 0;
            if (s->siege[r]) continue;
            n = s->reg[r][ELVES] + s->eli[r][ELVES]; if (n > 5) n = 5;
            for (int i = 0; i < n; i++) h += d6(s) == 6;
            if (h) CALL(push_casualty(s, FP, r, h, 0));
        }
    } else if (c == CARD_DENETHORS_FOLLY) {
        for (int n = 0; n < NFPNAT; n++) if (s->ldr[R_MINAS_TIRITH][n]) { s->ldr[R_MINAS_TIRITH][n]--; break; }
    } else if (c == CARD_THE_DAY_WITHOUT_DAWN) s->dice[FP][F_SPECIAL] = 0;
    else if (c == CARD_STORMCROW) {
        s->nopt = 0;
        for (int n = 0; n < NFPNAT; n++) {
            if (at_war(s, n)) continue;
            int ok = s->mordor < 0 && REGION[s->fsp_loc].nation == n;
            for (int k = 0; k < NCOMP; k++) if (s->comp_loc[k] < NREG && REGION[s->comp_loc[k]].nation == n) ok = 1;
            if (ok) s->opt[s->nopt++] = (uint8_t)n;
        }
        CHOOSE(D_CARD_NATION, SH);
        if (s->mv.a != 255) {
            int n = s->mv.a, done = 0, best = -1;
            if (s->pol[n] < 3) s->pol[n]++;
            for (int r = 0; r < NREG && !done; r++) if (s->ldr[r][n]) { s->ldr[r][n]--; done = 1; }
            if (!done) {
                for (int r = 0; r < NREG; r++) if (s->reg[r][n] && (best < 0 || units(s, FP, r) > units(s, FP, best))) best = r;
                if (best >= 0) { kill_unit(s, best, n, 0); done = 1; }
            }
            if (!done) for (int r = 0; r < NREG && !done; r++) if (s->eli[r][n]) { kill_unit(s, r, n, 1); best = r; done = 1; }
            if (best >= 0) { wipe_leaders(s, FP, best, 0); fix_siege(s, best); }
        }
    } else if (c == CARD_SHADOWS_GATHER || c == CARD_THE_SHADOW_LENGTHENS) {
        for (f->v[3] = 0; f->v[3] < (c == CARD_SHADOWS_GATHER ? 1 : 2); f->v[3]++) {
            {
                uint8_t src[NREG]; int n = 0;
                for (int r = 0; r < NREG; r++) if (units(s, SH, r) && !(s->siege[r] && s->ctrl[r] == SH) && !((s->lock[r >> 6] >> (r & 63)) & 1)) {
                    shadows_gather_opts(s, r, c == CARD_SHADOWS_GATHER ? 3 : 2);
                    if (s->nopt) src[n++] = (uint8_t)r;
                }
                s->nopt = 0;
                if (f->v[3]) s->opt[s->nopt++] = 255;
                for (int i = 0; i < n; i++) s->opt[s->nopt++] = src[i];
            }
            if (!s->nopt) break;
            CHOOSE(D_CARD_ARMY, SH);
            if (s->mv.a == 255) break;
            f->v[1] = s->mv.a;
            shadows_gather_opts(s, f->v[1], c == CARD_SHADOWS_GATHER ? 3 : 2);
            CHOOSE(D_CARD_REGION, SH);
            relocate_army(s, SH, f->v[1], s->mv.a);
            s->lock[s->mv.a >> 6] |= 1ULL << (s->mv.a & 63);
        }
    } else if (c == CARD_THE_SHADOW_IS_MOVING) {
        for (f->v[1] = 0; f->v[1] < 4; f->v[1]++) {
            push(s, FR_ARMY2)->v[0] = SH;
            CALL((void)0);
            if (s->mv.type != M_ARMY) break;
        }
    } else if (c == CARD_CORSAIRS_OF_UMBAR) {
        s->nopt = 0;
        for (int i = 0; i < 4; i++) {
            int r = GONDOR_COAST[i];
            if (enemy_field_units(s, SH, r)) s->opt[s->nopt++] = (uint8_t)r;
            else if (units(s, SH, r) + units(s, SH, R_UMBAR) <= 10) s->opt[s->nopt++] = (uint8_t)r;
        }
        CHOOSE(D_CARD_REGION, SH);
        if (s->mv.a != 255) {
            if (enemy_field_units(s, SH, s->mv.a)) {
                uint8_t mr[NNAT], me[NNAT];
                if (movable_units(s, SH, R_UMBAR, R_UMBAR, 1, mr, me)) { push_battle(s, SH, R_UMBAR, s->mv.a, 10, 10); s->b.no_cease = 1; CALL((void)0); }
            } else relocate_army(s, SH, R_UMBAR, s->mv.a);
        }
    } else if (c == CARD_RAGE_OF_THE_DUNLENDINGS) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++)
            if ((DIST[r][R_NORTH_DUNLAND] == 1 || DIST[r][R_SOUTH_DUNLAND] == 1) && free_for_recruit(s, SH, r) && !s->siege[r]) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_REGION, SH);
        if (s->mv.a != 255) { recruit(s, s->mv.a, ISENGARD, 0); recruit(s, s->mv.a, ISENGARD, 0); army_entered(s, SH, s->mv.a); }
    } else if (c == CARD_RETURN_OF_THE_WITCH_KING) {
        s->minion_loc[M_WK] = R_ANGMAR;
        if (free_for_recruit(s, SH, R_ANGMAR)) { recruit(s, R_ANGMAR, SAURON, 0); recruit(s, R_ANGMAR, SAURON, 0); recruit(s, R_ANGMAR, SAURON, 1); }
    } else if (c == CARD_HALF_ORCS_AND_GOBLIN_MEN || c == CARD_OLOG_HAI) {
        s->nopt = 0;
        for (int r = 0; r < NREG; r++) if (units(s, SH, r) && units(s, SH, r) < 10) s->opt[s->nopt++] = (uint8_t)r;
        CHOOSE(D_CARD_REGION, SH);
        if (s->mv.a != 255) rec_unit(s, s->mv.a, c == CARD_OLOG_HAI ? SAURON : ISENGARD);
    } else if (c == CARD_HILL_TROLLS) {
        for (f->v[1] = 0; f->v[1] < 2; f->v[1]++) {
            s->nopt = 0;
            if (s->pool_eli[SAURON]) for (int r = 0; r < NREG; r++) if (s->reg[r][SAURON]) s->opt[s->nopt++] = (uint8_t)r;
            if (!s->nopt) break;
            CHOOSE(D_CARD_REGION, SH);
            s->reg[s->mv.a][SAURON]--; s->pool_reg[SAURON]++; s->pool_eli[SAURON]--; s->eli[s->mv.a][SAURON]++;
        }
    } else if (c == CARD_A_NEW_POWER_IS_RISING) {
        rec_n(s, R_NORTH_DUNLAND, ISENGARD, 2); rec_n(s, R_SOUTH_DUNLAND, ISENGARD, 2);
        if (free_for_recruit(s, SH, R_ORTHANC)) { rec_unit(s, R_ORTHANC, ISENGARD); rec_unit(s, R_ORTHANC, ISENGARD); }
    } else if (c == CARD_MANY_KINGS_TO_THE_SERVICE_OF_MORDOR || c == CARD_PITS_OF_MORDOR) {
        f->v[2] = 0;
        for (f->v[1] = 0; f->v[1] < 3; f->v[1]++) {
            s->nopt = 0;
            if (c == CARD_PITS_OF_MORDOR) {
                for (int r = 0; r < NREG; r++)
                    if (REGION[r].nation == SAURON && REGION[r].settle == S_STRONGHOLD && s->ctrl[r] == SH && free_for_recruit(s, SH, r) && !((s->lock[r >> 6] >> (r & 63)) & 1)) s->opt[s->nopt++] = (uint8_t)r;
            } else for (int i = 0; i < 5; i++) {
                int r = SE_SETTLE[i];
                if (s->ctrl[r] == SH && free_for_recruit(s, SH, r) && !((s->lock[r >> 6] >> (r & 63)) & 1)) s->opt[s->nopt++] = (uint8_t)r;
            }
            if (!s->nopt) break;
            CHOOSE(D_CARD_REGION, SH);
            rec_n(s, s->mv.a, c == CARD_PITS_OF_MORDOR ? SAURON : SOUTHRON, 2);
            s->lock[s->mv.a >> 6] |= 1ULL << (s->mv.a & 63);
        }
    } else if (c == CARD_THE_KING_IS_REVEALED) { rec_n(s, R_MINAS_MORGUL, SAURON, 5); if (free_for_recruit(s, SH, R_MINAS_MORGUL)) recruit_naz(s, R_MINAS_MORGUL); }
    else if (c == CARD_SHADOWS_ON_THE_MISTY_MOUNTAINS) {
        s->nopt = 0;
        if (free_for_recruit(s, SH, R_MOUNT_GRAM)) s->opt[s->nopt++] = R_MOUNT_GRAM;
        if (free_for_recruit(s, SH, R_MORIA)) s->opt[s->nopt++] = R_MORIA;
        CHOOSE(D_CARD_REGION, SH);
        if (s->mv.a != 255) { rec_unit(s, s->mv.a, SAURON); rec_unit(s, s->mv.a, SAURON); recruit_naz(s, s->mv.a); }
    } else if (c == CARD_ORCS_MULTIPLYING_AGAIN) { rec_n(s, R_DOL_GULDUR, SAURON, 3); rec_n(s, R_MOUNT_GUNDABAD, SAURON, 3); }
    else if (c == CARD_HORDES_FROM_THE_EAST) {
        s->nopt = 0;
        for (int i = 0; i < 4; i++) if (free_for_recruit(s, SH, EAST_EDGE[i])) s->opt[s->nopt++] = EAST_EDGE[i];
        CHOOSE(D_CARD_REGION, SH);
        if (s->mv.a != 255) rec_n(s, s->mv.a, SOUTHRON, 5);
    } else if (c == CARD_MONSTERS_ROUSED) {
        rec_n(s, R_ANGMAR, SAURON, 1); rec_n(s, R_ETTENMOORS, SAURON, 1); rec_n(s, R_WEATHER_HILLS, SAURON, 1);
        if (free_for_recruit(s, SH, R_TROLLSHAWS)) recruit(s, R_TROLLSHAWS, SAURON, 1);
    } else if (c == CARD_MUSTERINGS_OF_LONG_PLANNED_WAR) { rec_n(s, R_GORGOROTH, SOUTHRON, 5); rec_n(s, R_NURN, SAURON, 5); }
    CO_END
}

/* ---- "No Event card without an effect" ----------------------------------------------------------------------
 * A card whose play could change nothing on the board is not offered (the ruling of the engines we are checked
 * against; the rulebook only covers effects that "cannot be fully applied"). The play is tried on a copy with every
 * die rolling its best; if the card then asks questions, each answer is tried too. A card draw printed on the card
 * counts; the Guide's or the Palantir's bonus draw and the die spent do not. The trial stops when the turn's dice run out. */
static int board_same(const State *s, const State *t, Move m) {
    uint8_t d[2][NFACE];
    memcpy(d, s->dice, sizeof d); d[s->dplayer][m.die & 0x7F]--;
    return (!t->winner || t->winner == 3) && t->card_where[m.a] != W_TABLE
        && !memcmp(t->reg, s->reg, sizeof s->reg) && !memcmp(t->eli, s->eli, sizeof s->eli) && !memcmp(t->ldr, s->ldr, sizeof s->ldr)
        && !memcmp(t->naz, s->naz, sizeof s->naz) && !memcmp(t->ctrl, s->ctrl, sizeof s->ctrl) && !memcmp(t->siege, s->siege, sizeof s->siege)
        && !memcmp(t->comp_loc, s->comp_loc, sizeof s->comp_loc) && !memcmp(t->minion_loc, s->minion_loc, sizeof s->minion_loc)
        && !memcmp(t->pol, s->pol, sizeof s->pol) && !memcmp(t->active, s->active, sizeof s->active)
        && !memcmp(t->pool_reg, s->pool_reg, sizeof s->pool_reg) && !memcmp(t->pool_eli, s->pool_eli, sizeof s->pool_eli)
        && !memcmp(t->tile_pool, s->tile_pool, sizeof s->tile_pool) && !memcmp(t->special_st, s->special_st, sizeof s->special_st)
        && t->fsp_loc == s->fsp_loc && t->progress == s->progress && t->revealed == s->revealed && t->corruption == s->corruption && t->mordor == s->mordor
        && t->rings[0] == s->rings[0] && t->rings[1] == s->rings[1] && t->gandalf_white == s->gandalf_white && t->aragorn == s->aragorn
        && !memcmp(t->dice, d, sizeof d) && t->hunt_sh == s->hunt_sh;
}
static int effect_search(const State *s, const State *t, Move m, int depth, int *budget) {
    if (!board_same(s, t, m) || (t->rng_max & 2)) return 1;       /* bit 2: the card drew a card */
    if (t->winner || t->sp <= s->sp || depth >= 3) return 0;      /* still inside the card's resolution? */
    Move sub[MAXMOVES]; int k = wotr_moves(t, sub);
    for (int j = 0; j < k && *budget > 0; j++) {
        State u = *t; (*budget)--;
        wotr_apply(&u, sub[j]);
        if (effect_search(s, &u, m, depth + 1, budget)) return 1;
    }
    return 0;
}
int card_has_effect(const State *s, Move m) {
    if (m.a == CARD_WORMTONGUE && s->active[ROHAN]) return 0;      /* would sit on the table doing nothing */
    if (CARD[m.a].table || m.a == CARD_THE_LIDLESS_EYE || m.a == CARD_CHALLENGE_OF_THE_KING) return 1;     /* (the Challenge's tile draws: best is not "highest") */
    State t = *s; int budget = 300;
    t.rng_max = 1;
    wotr_apply(&t, m);
    return effect_search(s, &t, m, 0, &budget);
}
