/* hambot.c -- Hambot as a bot of the WotR Bot Protocol (wotr-protocol/SPEC.md, 0.2): it joins a referee that runs
 * the game (the BehindMedia site: its sandbox, online bots, tournaments; or `cli referee` locally) and answers its
 * questions with this repository's player. bridge.c is the other way round: the engine is the referee.
 *
 * It keeps its own copy of the game in the engine and follows the referee's events (SPEC section 8) in it:
 *  - every `choice` (both sides) is matched to the engine's moves, written as protocol steps by bridge.c
 *    (build_leaves): the steps that agree with the answers so far stay; when one move is complete it is applied;
 *  - every `random_*` outcome is queued, and the engine's random draws (wotr_rnd_hook, tagged by what is drawn)
 *    are answered from the queues, so the copy rolls the referee's dice and draws its cards and tiles;
 *  - the opponent's hidden cards are stand-ins (any card of the deck), exchanged for the real one when it shows.
 * At each of its own Action decisions it compares its copy with the referee's view and, if they differ (or it lost
 * track somewhere), rebuilds the position from the view (as the position loader does). It then asks the player (the
 * release's settings: the value network with the move network; the Shadow with a short look-ahead) for a move and
 * answers the referee's questions step by step along that move. A question it cannot place gets a legal answer
 * anyway (the first option, logged as FALLBACK).
 *
 *   hambot <player>            the protocol on stdin/stdout (one JSON object per line); a log on stderr
 *   HAMBOT_VERBOSE=1           every decision and every step it follows on stderr
 *   HAMBOT_CHECK=1             compare the copy with the view between moves; log where they part (DIVERGED, LOST)
 *   HAMBOT_TRACE=<region>      that region's units and both sides' dice after every event
 *   HAMBOT_TIME=1 (or a share; 1 means 0.8)  use the clock: each Action decision gives the rollout chooser a share of it (off: the
 *                              release's settings, an answer at once)
 *
 * Over a WebSocket: ws_stdio.py (see README.md, "Playing on the BehindMedia site"). Offline: replay.py feeds a
 * recorded game back in and checks the copy at both sides' decisions. Build: make -C proto hambot. */
#define main cwotr_bridge_main
#include "bridge.c"
#undef main
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>


#define VERSION "0.1.0"
static int verbose, checking, foreign, trace_r = -1, following;
static void logf_(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); fflush(stderr);
}
#define LOG(...) logf_(__VA_ARGS__)
#define VLOG(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)

/* ================= the follower's game ================= */
static Ref R;                   /* R.s: the engine's copy of the game; R.dice: numbered dice for build_leaves */
static Agent ag;
static Player *player;
static int me = -1;             /* our side */
static int started;             /* the engine's game has been set up */
static int lost;                /* the copy is out of step until the next resync */
#define MAXDEF 16
static char *defer[MAXDEF]; static int ndefer;   /* events the engine is not ready for yet */
static char lost_why[200];      /* why (for the log: each fallback answer names it) */
static char last_note[160];     /* the last thing followed loosely (an event ignored, a draw not from the referee) */
#define LOSE(...) do { lost = 1; snprintf(lost_why, sizeof lost_why, __VA_ARGS__); \
    if (last_note[0]) { size_t n_ = strlen(lost_why); snprintf(lost_why + n_, sizeof lost_why - n_, " | before: %s", last_note); } \
    if (checking) logf_("LOST %s: %s", game_id, lost_why); } while (0)
static char game_id[80];
static struct { long decisions, planned, fallback, resyncs, diffs, ignored, forced, soft, asked_single, late; } st;

/* the random outcomes, in the referee's order */
enum { RQ_DICE, RQ_TILE, RQ_DRAW, RQ_HAND, RQ_COMP, RQ_D6 };
typedef struct { uint8_t type, side, deck, used; int16_t val; uint8_t roll; } Rand;   /* roll: 0 other, 1 combat, 2 leader_reroll */
#define MAXRAND 4096
static Rand rq[MAXRAND]; static int nrq;
/* The official rules ask the Leader re-roll after the Combat roll, so its dice come later than the roll's: a move that
 * needed one before it came (reroll_missing) waits (reroll_wait) and is played again from its snapshot once they come. */
static int reroll_missing, reroll_wait, snap_nrq;
/* The referee went on without a re-roll for a side (by its count it had none): that side's re-roll dice all miss here. */
static int reroll_misses;
/* The referee asks the passes on a hidden hand (new_game: official_rules, hidden_passes not false): so does the copy. */
static int hidden_passes;
/* The region a card's Army move of ours goes to (the referee's answer): its "how many go" is every unit, as CWOTR moves them. */
static int card_move_to = -1;
/* Following one of our own card's answers or selections: the engine may have run ahead of the referee (it plays the card's
 * moves at once), so a phase it reached is not one the referee skipped. */
static int following_own_card;
/* Where the referee is (turn_started, phase_started; each decision's view): a Fellowship phase the copy reached is one the
 * referee skipped only once the referee is there too (the engine runs ahead, into the next turn, after an advance say). */
static int ref_turn, ref_phase;
static int guide_skip_pending;  /* the Guide's ability declined before the engine asks it (guide_skip_settle) */
static int route_pending;       /* a card's Army move "from>to" just followed: its "Which way?" may come next */
static int phase_index(const char *p) {
    static const char *const ph[] = { "RECOVER", "FELLOWSHIP", "HUNT_ALLOCATION", "ACTION_ROLL", "ACTION_RESOLUTION", "VICTORY_CHECK" };
    for (int i = 0; p && i < 6; i++) if (!strcmp(p, ph[i])) return i;
    return -1;
}
static int referee_at_fsp_phase(void) { return ref_turn > R.s.turn || (ref_turn == R.s.turn && ref_phase >= 1); }
static void rq_clear(void) { nrq = 0; }
static void rq_add(Rand r) { if (nrq < MAXRAND) rq[nrq++] = r; }

/* the pending events (since the last decision) */
#define MAXEV 2048
static char *ev[MAXEV]; static int nev;

/* the move being followed: the engine's moves as protocol steps */
static Leaf *L; static int nl, depth = -1; static uint8_t *alive;
static int target = -1;         /* the leaf we play, at our own decision */

static uint64_t own_rng = 0x9E3779B97F4A7C15ULL;
static uint32_t own_rnd(uint32_t n) {
    uint64_t z = (own_rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; z ^= z >> 31;
    return n ? (uint32_t)(((z >> 32) * n) >> 32) : 0;
}

static void leaves_reset(void);
/* ---- the opponent's hidden cards ---- */
static uint8_t known[NCARD];    /* 1: this card's place is known for certain (not a stand-in) */
/* Puts card c where `where` says for its side, exchanging it with a stand-in of the same deck. */
static int ensure_card(State *s, int c, int where) {
    if (c < 0 || c >= NCARD) return 0;
    if (s->card_where[c] == where) { known[c] = 1; return 1; }
    int deck = CARD_DECK(c), x = -1;
    for (int k = deck * 24; k < deck * 24 + 24 && x < 0; k++) if (k != c && s->card_where[k] == where && !known[k]) x = k;
    int wc = s->card_where[c];
    if (wc == W_TABLE || (wc == W_DISCARD && known[c])) { VLOG("  %s is %s, not a stand-in", CARD_KEY[c], wc == W_TABLE ? "on the table" : "discarded"); return 0; }
    /* none of that deck: the opponent's hand held one card too few of it (the counts by deck are guesses between our
       decisions): a stand-in of the other deck goes back, or, with none at all, the hand grows by the real card */
    if (x < 0 && where == W_HAND && CARD_SIDE(c) != me) {
        int other = (deck ^ 1) * 24;
        for (int k = other; k < other + 24 && x < 0; k++) if (s->card_where[k] == W_HAND && !known[k]) x = k;
        if (x >= 0) { s->card_where[x] = W_DECK; s->card_where[c] = W_HAND; known[c] = 1; VLOG("  stand-in %s (other deck) exchanged for %s", CARD_KEY[x], CARD_KEY[c]); }
        else { s->card_where[c] = W_HAND; s->hand_n[CARD_SIDE(c)]++; known[c] = 1; VLOG("  %s added to the opponent's hand", CARD_KEY[c]); }
        if (s->dkind == D_BATTLE_CARD && s->dplayer == CARD_SIDE(c) && s->nopt < MAXOPT) {
            int has = 0; for (int i = 0; i < s->nopt; i++) { if (s->opt[i] == x && x >= 0) s->opt[i] = (uint8_t)c; has |= s->opt[i] == c; }
            if (!has) s->opt[s->nopt++] = (uint8_t)c;
        }
        if (depth == 0) leaves_reset();
        return 1;
    }
    if (x < 0) { VLOG("  no stand-in for %s", CARD_KEY[c]); return 0; }
    s->card_where[x] = (uint8_t)wc; s->card_where[c] = (uint8_t)where; known[c] = 1;
    if (s->b.active) for (int side = 0; side < 2; side++) if (s->b.card[side] == x) s->b.card[side] = (uint8_t)c;
    /* a question already asked lists the stand-in (or, for a Combat card, may lack the real one): ask it anew */
    if (s->dkind == D_BATTLE_CARD || s->dkind == D_DISCARD) {
        int at = -1, has = 0;
        for (int i = 0; i < s->nopt; i++) { if (s->opt[i] == x) at = i; if (s->opt[i] == c) has = 1; }
        if (!has) { if (at >= 0) s->opt[at] = (uint8_t)c; else if (where == W_HAND && s->dplayer == CARD_SIDE(c) && s->nopt < MAXOPT) s->opt[s->nopt++] = (uint8_t)c; }
        if (depth == 0) leaves_reset();
    } else if (depth == 0) leaves_reset();      /* the moves listed before held the stand-in: listed anew */
    VLOG("  stand-in %s exchanged for %s", CARD_KEY[x], CARD_KEY[c]);
    return 1;
}

/* ---- the engine's random draws, answered from the queues ---- */
static int rq_pop(int type, int side, int deck, int roll_any, int roll) {
    for (int i = 0; i < nrq; i++) {
        Rand *r = &rq[i];
        if (r->used || r->type != type) continue;
        if (side >= 0 && r->side != side) continue;
        if (deck >= 0 && r->deck != deck) continue;
        if (!roll_any && r->roll != roll) continue;
        r->used = 1; return i;
    }
    return -1;
}
static uint32_t soft(State *s, uint32_t n, const char *what) {
    st.soft++;
    snprintf(last_note, sizeof last_note, "no referee outcome for %s", what);
    VLOG("  no queued outcome for %s (of %u): the engine's own", what, n);
    return own_rnd(n);
}
/* A card the engine draws before the referee does (the referee asks which deck first, CWOTR draws at once): a
 * placeholder now, exchanged for the real card when the referee's draw arrives (resolve_late_draws). */
typedef struct { uint8_t deck, card; } Late;
static Late late[16]; static int nlate;
static uint32_t late_draw(State *s, uint32_t n, int deck) {
    int cand[24], m = 0, pick[24], p = 0;
    for (int k = deck * 24; k < deck * 24 + 24; k++) if (s->card_where[k] == W_DECK) cand[m++] = k;
    if ((uint32_t)m != n || nlate >= 16) return soft(s, n, "a card draw");
    for (int k = 0; k < m; k++) if (!known[cand[k]]) pick[p++] = k;
    int idx = p ? pick[own_rnd((uint32_t)p)] : (int)own_rnd(n);
    late[nlate].deck = (uint8_t)deck; late[nlate++].card = (uint8_t)cand[idx];
    st.late++;
    VLOG("  a card drawn before the referee's draw: %s for now", CARD_KEY[cand[idx]]);
    return (uint32_t)idx;
}
static void resolve_late_draws(void) {
    for (int k = 0; k < nlate; ) {
        int i = rq_pop(RQ_DRAW, -1, late[k].deck, 1, 0);
        if (i < 0) { k++; continue; }
        int c = rq[i].val, x = late[k].card;
        State *s = &R.s;
        if (c >= 0 && c != x && s->card_where[x] == W_HAND && s->card_where[c] == W_DECK) {
            s->card_where[x] = W_DECK; s->card_where[c] = W_HAND; known[x] = 0; known[c] = 1;
            if ((s->dkind == D_DISCARD || s->dkind == D_BATTLE_CARD) && depth == 0) {
                for (int o = 0; o < s->nopt; o++) if (s->opt[o] == x) s->opt[o] = (uint8_t)c;
                leaves_reset();
            }
            VLOG("  the referee's draw: %s instead of %s", CARD_KEY[c], CARD_KEY[x]);
        } else if (c >= 0) known[c] = 1;
        late[k] = late[--nlate];
    }
}
static uint32_t hook(State *s, uint32_t n, int kind) {
    static const uint8_t FPF[6] = {F_CHAR, F_CHAR, F_MA, F_MUSTER, F_EVENT, F_SPECIAL};
    static const uint8_t SHF[6] = {F_CHAR, F_ARMY, F_MUSTER, F_MA, F_EVENT, F_SPECIAL};
    int i;
    switch (kind) {
    case 1: case 2: {
        int side = kind == 1 ? FP : SH;
        if ((i = rq_pop(RQ_DICE, side, -1, 1, 0)) < 0) return soft(s, n, "an action die");
        for (int k = 0; k < 6; k++) if ((side == FP ? FPF : SHF)[k] == rq[i].val) return (uint32_t)k;
        return soft(s, n, "an action die's face");
    }
    case 3: {
        if ((i = rq_pop(RQ_TILE, -1, -1, 1, 0)) < 0) return soft(s, n, "a Hunt tile");
        int t = rq[i].val, k = 0;
        if (t < 0 || !s->tile_pool[t]) return soft(s, n, "a Hunt tile not in the pool");
        for (int u = 0; u < t; u++) k += s->tile_pool[u];
        return (uint32_t)k;
    }
    case 4: {
        int deck = wotr_draw_deck;
        if ((i = rq_pop(RQ_DRAW, -1, deck, 1, 0)) < 0) return late_draw(s, n, deck);
        int c = rq[i].val;
        if (c >= 0 && s->card_where[c] != W_DECK) ensure_card(s, c, W_DECK);   /* a stand-in took its place */
        int cand[24], m = 0;
        for (int k = deck * 24; k < deck * 24 + 24; k++) if (s->card_where[k] == W_DECK) cand[m++] = k;
        if ((uint32_t)m != n) return soft(s, n, "a card draw (deck size)");
        if (c < 0) {            /* the opponent's: any card it cannot be */
            int pick[24], p = 0;
            for (int k = 0; k < m; k++) if (!known[cand[k]]) pick[p++] = k;
            return p ? (uint32_t)pick[own_rnd((uint32_t)p)] : own_rnd(n);
        }
        for (int k = 0; k < m; k++) if (cand[k] == c) { known[c] = 1; return (uint32_t)k; }
        return soft(s, n, "a card draw (card)");
    }
    case 5: {
        if ((i = rq_pop(RQ_HAND, -1, -1, 1, 0)) < 0) return soft(s, n, "a card from the hand");
        int c = rq[i].val;
        ensure_card(s, c, W_HAND);
        int cand[24], m = 0;
        for (int k = 0; k < 24; k++) if (s->card_where[k] == W_HAND) cand[m++] = k;
        for (int k = 0; k < m; k++) if (cand[k] == c) return (uint32_t)k;
        return soft(s, n, "a card from the hand (card)");
    }
    case 6: {
        if ((i = rq_pop(RQ_COMP, -1, -1, 1, 0)) < 0) return soft(s, n, "a Companion");
        int c = rq[i].val, m = fsp_comps(s), k = 0;
        VLOG("  the referee's random Companion: %s (queue entry %d of %d)", c >= 0 && c < NCOMP ? CHAR_KEY[c] : "?", i, nrq);
        if (c < 0 || c >= NCOMP || !((m >> c) & 1)) return soft(s, n, "a Companion not in the Fellowship");
        for (int u = 0; u < c; u++) k += (m >> u) & 1;
        return (uint32_t)k;
    }
    default:
        if (n != 6) return soft(s, n, "an unknown draw");
        if (kind >= 7 && kind <= 10) {
            int side = (kind == 7 || kind == 9) ? FP : SH, roll = kind <= 8 ? 1 : 2;
            if ((i = rq_pop(RQ_D6, side, -1, 0, roll)) < 0) i = rq_pop(RQ_D6, side, -1, 1, 0);
        } else i = rq_pop(RQ_D6, -1, -1, 1, 0);
        if (i < 0 && (kind == 9 || kind == 10)) { if (reroll_misses) return 0; reroll_missing = 1; }
        if (i < 0) {
            char what[160];
            const char *kn = kind == 7 ? "FP combat" : kind == 8 ? "SH combat" : kind == 9 ? "FP re-roll" : kind == 10 ? "SH re-roll" : "a die";
            if (s->b.active) snprintf(what, sizeof what, "a die roll (%s; battle round %d, cards %s / %s; frame %d)", kn, s->b.round,
                                      s->b.card[FP] < NCARD ? CARD[s->b.card[FP]].name : "-", s->b.card[SH] < NCARD ? CARD[s->b.card[SH]].name : "-", s->st[s->sp - 1].kind);
            else { int c = card_asking(s); snprintf(what, sizeof what, "a die roll (%s; card %s; frame %d)", kn, c >= 0 && c < NCARD ? CARD[c].name : "-", s->st[s->sp - 1].kind); }
            if (checking) LOG("DICE %s turn %d: %s (battle %s->%s, units SH %d FP %d)", game_id, s->turn, what, s->b.active ? REGION_KEY[s->b.src] : "-", s->b.active ? REGION_KEY[s->b.dst] : "-",
                              s->b.active ? units(s, SH, s->b.att == SH ? s->b.src : s->b.dst) : 0, s->b.active ? units(s, FP, s->b.att == FP ? s->b.src : s->b.dst) : 0);
            return soft(s, n, what);
        }
        return (uint32_t)(rq[i].val - 1);
    }
}
static void stash_restore(void);
static int rec_used;            /* recruits of the engine already matched with the referee's answers */
/* A card's recruit as the referee's answer has it: CWOTR recruits an Elite where it can and never declines. */
static int recruit_answer(const char *answer) {
    State *s = &R.s;
    while (rec_used < s->rec_n && s->rec_eli[rec_used] == 2) rec_used++;      /* Regulars the card names: not asked */
    if (rec_used >= s->rec_n) return 0;
    int k = rec_used++, r = s->rec_r[k], n = s->rec_nat[k];
    if (!strcmp(answer, "REGULAR") && s->rec_eli[k] && s->eli[r][n] && s->pool_reg[n]) { s->eli[r][n]--; s->pool_eli[n]++; s->reg[r][n]++; s->pool_reg[n]--; }
    else if (!strcmp(answer, "ELITE") && !s->rec_eli[k] && s->reg[r][n] && s->pool_eli[n]) { s->reg[r][n]--; s->pool_reg[n]++; s->eli[r][n]++; s->pool_eli[n]--; }
    else if (!strcmp(answer, "skip")) { if (s->rec_eli[k] && s->eli[r][n]) { s->eli[r][n]--; s->pool_eli[n]++; } else if (!s->rec_eli[k] && s->reg[r][n]) { s->reg[r][n]--; s->pool_reg[n]++; } }
    VLOG("  a card's recruit in %s: %s", REGION_KEY[r], answer);
    return 1;
}
static void apply_followed(Move m) {
    /* every question the rules ask: the engine's pruning (wotr_prune) also settles some inside the rules (a reveal with
       one sensible place), which the referee asks */
    wotr_prune = 0;
    if (R.s.dkind == D_ACTION) { R.s.rec_n = 0; rec_used = 0; R.s.reloc_on = 0; guide_skip_pending = 0; }     /* a new action: the recruits and card moves logged anew */
    wotr_rnd_hook = hook;
    wotr_ask_every_card = hidden_passes;
    wotr_apply(&R.s, m);
    wotr_ask_every_card = 0;
    wotr_rnd_hook = NULL;
    wotr_prune = 1;
    stash_restore();
    wotr_rk_d6 = 0;
}

/* ---- the engine's moves as protocol steps ---- */
static void dice_from_state(void) {         /* numbered dice for build_leaves: one per die the engine counts */
    const State *s = &R.s;
    for (int side = 0; side < 2; side++) {
        int n = 0;
        for (int f = 0; f < NFACE; f++) for (int i = 0; i < s->dice[side][f] && n < MAXDICE; i++) { R.dice[side][n].face = (uint8_t)f; R.dice[side][n++].status = DS_AVAILABLE; }
        R.nd[side] = n;
    }
}
static int selmode;
static void leaves_reset(void) { depth = -1; nl = 0; target = -1; selmode = 0; }
/* An attack that leaves a rearguard: bridge.c stops at "all":false, the referee then asks unit by unit which attack
 * (as for an Army move). the engine always attacks with the leaders, Nazgul and characters and fills the attacking
 * regulars and elites nation by nation (push_battle): the counts come from its split on a copy. */
static void attack_steps(Leaf *l, const State *s) {
    if (l->mv[0].type != M_ATTACK || !l->nstep || !strstr(l->st[l->nstep - 1].text, "\"all\":false")) return;
    Move m = l->mv[0];
    int side = s->dplayer, src = m.a, lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT;
    State t = *s; wotr_apply(&t, m);
    if (!t.b.active || t.b.src != src) return;
    for (int n = lo; n < hi; n++) {
        if (s->reg[src][n]) step(l, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"nation\":\"%s\",\"unit\":\"regular\"}", s->reg[src][n] - t.b.rear_reg[n], NATION_KEY[n]);
        if (s->eli[src][n]) step(l, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"nation\":\"%s\",\"unit\":\"elite\"}", s->eli[src][n] - t.b.rear_eli[n], NATION_KEY[n]);
        if (n < NFPNAT && s->ldr[src][n]) step(l, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"nation\":\"%s\",\"unit\":\"leader\"}", s->ldr[src][n], NATION_KEY[n]);
    }
    if (side == SH && s->naz[src]) step(l, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"nation\":\"SAURON\",\"unit\":\"nazgul\"}", s->naz[src]);
    if (side == FP) { for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == src) step(l, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":1,\"character\":\"%s\"}", comp_key(s, c)); }
    else for (int k = 0; k < NMINION; k++) if (k != M_SARUMAN && s->minion_loc[k] == src) step(l, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":1,\"character\":\"%s\"}", minion_key(k));
}
/* Gwaihir the Windlord, We Prove the Swifter moving a group (option 0x80 | group): bridge.c writes one "move_companion"
 * of the group's first Companion; the referee lets any Companion of the region lead and then asks each other one there
 * whether it joins (JOIN_GROUP). The leaves are written so: one per leader of the group, with a join or stay each. */
static void comp_move_leads(void);
static void card_group_steps(void) {
    const State *s = &R.s;
    if (s->dkind == D_COMP_MOVE) { comp_move_leads(); return; }
    if (s->dkind != D_CARD_COMP) return;
    int n0 = nl;
    for (int i = 0; i < n0; i++) {
        Move m = L[i].mv[0];
        if (!(m.a & 0x80) || m.a == 255 || L[i].nstep != 1 || !strstr(L[i].st[0].text, "\"kind\":\"move_companion\"")) continue;
        int g = m.a & 0x7F, k0 = __builtin_ctz((unsigned)g), from = s->comp_loc[k0];
        if (from >= NREG) continue;
        char to[64] = ""; const char *tp = strstr(L[i].st[0].text, "\"to\":\"");
        if (tp) { tp += 6; size_t k = 0; while (tp[k] && tp[k] != '"' && k < sizeof to - 1) { to[k] = tp[k]; k++; } to[k] = 0; }
        Leaf base = L[i];
        for (int lead = 0; lead < NCOMP; lead++) {
            if (!((g >> lead) & 1)) continue;
            Leaf *l = lead == k0 ? &L[i] : (nl < MAXLEAF ? &L[nl++] : NULL);
            if (!l) break;
            *l = base; l->nstep = 0;
            step(l, K_CARD_CHOICE, "\"kind\":\"move_companion\",\"character\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}", CHAR_KEY[lead], REGION_KEY[from], to);
            int others = comps_at(s, from) & ~(1 << lead);
            for (int c = 0; c < NCOMP; c++) if ((others >> c) & 1)
                step(l, K_JOIN_GROUP, "\"kind\":\"%s\",\"character\":\"%s\",\"to\":\"%s\"}", (g >> c) & 1 ? "join" : "stay", comp_key(s, c), to);
        }
    }
}
/* A Companions' move (a Character die): the engine's list of moves stops at MAXMOVES, which every group of six or seven
 * Companions in one region (127 groups, each to several regions) exceeds. The moves are written here as bridge.c writes
 * them: every group, every destination, its first Companion leading, the others of the region joining or staying
 * (comp_lead: the referee's player may name another Companion of the group first). */
static int group_range(const State *s, int mask) {      /* movegen.c's comp_range: the group's highest Level, Gandalf the White's 4 */
    int best = 0, cnt = 0, hob = 0;
    for (int c = 0; c < NCOMP; c++) if ((mask >> c) & 1) { cnt++; if (c >= C_MERRY) hob++; if (comp_level(s, c) > best) best = comp_level(s, c); }
    if ((mask & 1) && s->gandalf_white && (cnt == 1 || (cnt == 2 && hob == 1))) return 4;
    return best;
}
static void comp_move_leads(void) {
    const State *s = &R.s;
    int w = 0;
    for (int i = 0; i < nl; i++) if (L[i].mv[0].type != M_COMP_MOVE) L[w++] = L[i];
    nl = w;
    int seen = 0;
    for (int c0 = 0; c0 < NCOMP; c0++) {
        int from = s->comp_loc[c0];
        if (from >= NREG || ((seen >> c0) & 1)) continue;
        int all = comps_at(s, from) & ~s->comp_moved;
        seen |= comps_at(s, from);
        for (int g = 1; g < 128; g++) {
            if (g & ~all) continue;
            uint8_t reach[NREG], dst[NREG];
            comp_reach(s, from, group_range(s, g), 0, reach);
            int nd = comp_dests(s, g, reach, from, dst);
            for (int d = 0; d < nd; d++) {
                int lead = __builtin_ctz((unsigned)g);
                Move m = {M_COMP_MOVE, 0, (uint8_t)g, dst[d], 0, 0, 0, 0};
                Leaf *l = leaf_new(L, &nl, m);
                if (!l) return;
                step(l, K_MOVE_COMPANIONS, "\"kind\":\"move_companion\",\"character\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}", comp_key(s, lead), REGION_KEY[from], REGION_KEY[dst[d]]);
                int others = all & ~(1 << lead);
                for (int c = 0; c < NCOMP; c++) if ((others >> c) & 1)
                    step(l, K_JOIN_GROUP, "\"kind\":\"%s\",\"character\":\"%s\",\"to\":\"%s\"}", (g >> c) & 1 ? "join" : "stay", comp_key(s, c), REGION_KEY[dst[d]]);
            }
        }
    }
}
/* The Companion the referee's player names first for a group's move: in the leaves where he joins the group, he leads
 * and the one written first joins in his place. */
static void comp_lead(const char *character) {
    if (!character || depth != 0) return;
    char want[80], joins[100]; snprintf(want, sizeof want, "\"character\":\"%s\"", character);
    snprintf(joins, sizeof joins, "\"kind\":\"join\",\"character\":\"%s\"", character);
    for (int i = 0; i < nl; i++) {
        if (!alive[i] || !L[i].nstep || !strstr(L[i].st[0].text, "\"kind\":\"move_companion\"") || strstr(L[i].st[0].text, want)) continue;
        for (int j = 1; j < L[i].nstep; j++) {
            if (!strstr(L[i].st[j].text, joins)) continue;
            const char *c0 = strstr(L[i].st[0].text, "\"character\":\""), *f0 = strstr(L[i].st[0].text, "\",\"from\":");
            if (!c0 || !f0) break;
            char lead[40]; size_t n = (size_t)(f0 - (c0 + 13)); if (n >= sizeof lead) break;
            memcpy(lead, c0 + 13, n); lead[n] = 0;
            char t0[200], tj[200];
            snprintf(t0, sizeof t0, "\"kind\":\"move_companion\",\"character\":\"%s\"%s", character, f0 + 1);
            const char *to = strstr(L[i].st[j].text, "\",\"to\":");
            snprintf(tj, sizeof tj, "\"kind\":\"join\",\"character\":\"%s\"%s", lead, to ? to + 1 : "}");
            snprintf(L[i].st[0].text, sizeof L[i].st[0].text, "%s", t0);
            snprintf(L[i].st[j].text, sizeof L[i].st[j].text, "%s", tj);
            break;
        }
    }
}
/* The Breaking of the Fellowship: CWOTR asks the Companions that separate as one set ("answer":"<mask>"), the referee
 * one at a time ("Which Companion separates?"): one step per Companion of the set. */
static void breaking_steps(void) {
    const State *s = &R.s;
    if (s->dkind != D_CARD_COMP || card_asking(s) != CARD_THE_BREAKING_OF_THE_FELLOWSHIP) return;
    const char *ck = CARD_KEY[CARD_THE_BREAKING_OF_THE_FELLOWSHIP];
    for (int i = 0; i < nl; i++) {
        int m = L[i].mv[0].a;
        if (L[i].nstep != 1 || m == 255 || !strstr(L[i].st[0].text, "\"count\":")) continue;
        L[i].nstep = 0;
        for (int c = 0; c < NCOMP; c++) if ((m >> c) & 1)
            step(&L[i], K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"character\":\"%s\"}", ck, comp_key(s, c), comp_key(s, c));
    }
}
/* A set asked one member at a time in the referee's order ("join or stay", the Companions a card separates): the
 * engine's steps (in its own order) reordered so that the one about this Companion comes next (a group is a set).
 * same: what the steps of the set share. */
static int leaf_done(int i);
static int in_set(const char *t, const char *same) {
    if (same) return strstr(t, same) != NULL;
    return strstr(t, "\"kind\":\"join\"") || strstr(t, "\"kind\":\"stay\"");
}
static void set_order(const char *character, const char *same) {
    if (!character || depth < 0) return;
    char want[80]; snprintf(want, sizeof want, "\"character\":\"%s\"", character);
    for (int i = 0; i < nl; i++) {
        if (!alive[i] || leaf_done(i)) continue;
        const char *t = L[i].st[depth].text;
        if (!in_set(t, same) || strstr(t, want)) continue;
        for (int j = depth + 1; j < L[i].nstep; j++) {
            const char *u = L[i].st[j].text;
            if (!in_set(u, same)) break;
            if (strstr(u, want)) { Step x = L[i].st[depth]; L[i].st[depth] = L[i].st[j]; L[i].st[j] = x; break; }
        }
    }
}
static void join_order(const char *character) { set_order(character, NULL); }
static int leaves_ensure(void) {
    if (depth >= 0) return nl;
    if (R.s.winner) return 0;
    dice_from_state();
    wotr_prune = 0;                 /* every legal move: the referee's are */
    nl = build_leaves(&R, L);
    card_group_steps();
    breaking_steps();
    wotr_prune = 1;
    memset(alive, 1, (size_t)nl);
    depth = 0; target = -1;
    return nl;
}
static int leaf_done(int i) { return L[i].nstep <= depth; }
static int all_done(void) { for (int i = 0; i < nl; i++) if (alive[i] && !leaf_done(i)) return 0; return 1; }
/* The state before the last move followed, to replay it when an opponent's card shows that the copy's stand-ins
 * could not have played (the engine does not even ask for a Combat card from a hand without one). */
static State snap; static Leaf snap_leaf; static uint8_t snap_used[MAXRAND], snap_known[NCARD]; static int snap_ok;
/* the referee's split applied after that move (sel_fixup): applied again when the move is played again */
static int snap_fix; static State snap_fix_before; static Move snap_fix_m; static int snap_fix_want[NNAT][4], snap_fix_char[NCOMP + NMINION];
static void replay_fixup(void);
/* the last moves applied (HAMBOT_CHECK: where the copy and the referee part) */
static char recent[8][160]; static int nrecent, diverged;
/* The face of the die the referee's last Action used: its player picks the die (an Army/Muster or a Will of the West
 * for an Army move, a Muster die as an Army die with the Mouth of Sauron, an Event die for any card); the engine
 * always pays with one particular die. The move followed is paid with the referee's. */
/* Voice of Saruman with too few Isengard Regulars for every region: the engine fills its regions in its own order,
   the referee in another (the rules leave it to the player): the referee's recruits (figure_recruited) are applied */
static int voice_pending, voice_seen[NREG];
static int act_face = -1, act_ring_to = -1;    /* act_ring_to: the face an Elven Ring gave (a plain Muster too) */
static void apply_leaf(int i) {
    char buf[200];
    if (act_ring_to >= 0 && R.s.dkind == D_ACTION && L[i].nmv > 0 && L[i].mv[0].type == M_RING && L[i].mv[0].b != act_ring_to) L[i].mv[0].b = (uint8_t)act_ring_to;
    if (act_face >= 0 && R.s.dkind == D_ACTION && L[i].nmv > 0) {
        Move *m = &L[i].mv[0];
        int f = m->die & 0x7F, side = R.s.dplayer;
        static const uint8_t paid[] = { M_ARMY, M_ATTACK, M_PLAY, M_FSP_MOVE, M_FSP_HIDE, M_SEPARATE, M_COMPS, M_NAZGUL, M_DIPLO, M_MUSTER, M_MUSTER_ELITE, M_MINION, M_VOICE, M_DRAW, M_COMP_POLITICS };
        int is_paid = 0; for (size_t k = 0; k < sizeof paid; k++) is_paid |= m->type == paid[k];
        if (is_paid && f < NFACE && act_face != f && R.s.dice[side][act_face] > 0) {
            int mouth = side == SH && act_face == F_MUSTER && (m->type == M_ARMY || m->type == M_ATTACK || (m->type == M_PLAY && CARD[m->a].type == F_ARMY));
            VLOG("  paid with the referee's die: %s for %s", face_key(side, act_face), face_key(side, f));
            m->die = (uint8_t)(act_face | (mouth ? DIE_MOUTH : 0));
        }
    }
    snap = R.s; snap_leaf = L[i]; memcpy(snap_known, known, sizeof known);
    for (int k = 0; k < nrq; k++) snap_used[k] = rq[k].used;
    snap_nrq = nrq;
    snap_ok = 1; snap_fix = 0;
    wotr_move_str(&R.s, L[i].mv[0], buf, sizeof buf);
    VLOG("  %s plays %s", SIDE_KEY[R.s.dplayer], buf);
    snprintf(recent[nrecent++ % 8], sizeof recent[0], "T%d %s %s", R.s.turn, R.s.dplayer ? "SH" : "FP", buf);
    Leaf lf = L[i];
    if (lf.nmv && lf.mv[0].type == M_VOICE && lf.mv[0].a == 0) { voice_pending = 1; memset(voice_seen, 0, sizeof voice_seen); }
    reroll_missing = 0;
    for (int k = 0; k < lf.nmv; k++) apply_followed(lf.mv[k]);
    if (reroll_missing) { reroll_wait = 1; VLOG("  the Leader re-roll's dice are not here yet: the move is played again when they come"); }
    leaves_reset();
}
static void replay_for_reroll(void);
/* Mighty Attack's Companion (Axe and Bow, Horn of Gondor): the official rules ask which one forfeits his Leadership,
 * before the Leader re-roll's dice; CWOTR's engine takes the one of the lowest Leadership unless told (wotr_ma_pick).
 * The question does not end a wait for the re-roll's dice: the waiting move is played again with the Companion. */
static int ma_question_card(const char *card) { int c = card_of(card); return c == CARD_AXE_AND_BOW || c == CARD_HORN_OF_GONDOR; }
static int ma_comp_of(const char *answer) {
    int c = answer ? char_of(answer) : -1;
    if (c == 9) return C_STRIDER;           /* aragorn */
    if (c == 8) return C_GANDALF;           /* gandalf-the-white */
    return c >= 0 && c < NCOMP ? c : -1;
}
/* The referee has gone past the Leader re-roll (another question): the waiting move played again with the dice it sent,
 * and a re-roll it did not make (none for that side by its count) as dice that miss: no hits, as there. */
static void settle_reroll(void) {
    if (!reroll_wait) return;
    replay_for_reroll();
    if (!reroll_wait) return;
    reroll_misses = 1;
    replay_for_reroll();
    reroll_misses = 0;
    if (reroll_wait) { reroll_wait = 0; VLOG("  the Leader re-roll could not be played again"); }
    else VLOG("  the referee made no Leader re-roll here: none (its dice miss)");
    wotr_ma_pick = -1;
}
/* The move that waited for the Leader re-roll's dice (reroll_wait), played again from its snapshot now that they may be
 * queued; it waits on if they are still missing (the other side has not answered yet). */
static void replay_for_reroll(void) {
    if (!reroll_wait || !snap_ok) return;
    reroll_wait = 0;
    R.s = snap; memcpy(known, snap_known, sizeof known);
    for (int k = 0; k < nrq; k++) rq[k].used = k < snap_nrq ? snap_used[k] : 0;     /* queued since: unused */
    Leaf lf = snap_leaf;
    leaves_reset();
    reroll_missing = 0;
    for (int k = 0; k < lf.nmv; k++) apply_followed(lf.mv[k]);
    replay_fixup();
    if (reroll_missing) reroll_wait = 1;
    else { VLOG("  played again with the Leader re-roll's dice"); wotr_ma_pick = -1; }
    leaves_reset();
}
static void voice_settle(void) {
    if (!voice_pending) return;
    voice_pending = 0;
    State *s = &R.s;
    int copy[NREG] = {0}, any = 0;
    for (int k = 0; k < s->rec_n; k++) if (s->rec_nat[k] == ISENGARD) copy[s->rec_r[k]]++;
    for (int r = 0; r < NREG; r++) any |= voice_seen[r];
    if (!any) return;
    for (int pass = 0; pass < 2; pass++)            /* back to the reinforcements first (they ran short), then placed */
        for (int r = 0; r < NREG; r++) {
            int d = voice_seen[r] - copy[r];
            if (pass == 0) while (d < 0 && s->reg[r][ISENGARD]) { s->reg[r][ISENGARD]--; s->pool_reg[ISENGARD]++; d++; }
            else {
                while (d > 0 && s->pool_reg[ISENGARD]) { s->reg[r][ISENGARD]++; s->pool_reg[ISENGARD]--; d--; }
                if (voice_seen[r] != copy[r]) VLOG("  Voice of Saruman: %d Regular(s) in %s, as the referee placed them", voice_seen[r], REGION_KEY[r]);
            }
        }
}
/* Replays the last move with card c (or, c < 0, a card of the opponent's deck that has a Combat text) in the hand. */
static int replay_with_card(int side, int c, int deck) {
    if (!snap_ok) { VLOG("  no move to play again with the opponent's Combat card"); return 0; }
    snap_ok = 0;
    /* a hidden card: one the engine would offer in this battle (its card question is open now), else any with a Combat
       text; one not known for certain (a hand or deck stand-in first, else a discard face down) */
    int d = side * 2 + deck, x = -1;
    if (c < 0 && R.s.b.active)
        for (int pass = 0; pass < 2 && x < 0; pass++)
            for (int k = d * 24; k < d * 24 + 24 && x < 0; k++)
                if (R.s.card_where[k] != W_TABLE && (pass || R.s.card_where[k] != W_DISCARD) && !known[k] && CARD[k].combat && cc_playable(&R.s, side, k)) x = k;
    R.s = snap; memcpy(known, snap_known, sizeof known);
    for (int k = 0; k < nrq; k++) rq[k].used = snap_used[k];
    if (c >= 0) { if (!ensure_card(&R.s, c, W_HAND)) return 0; }
    else {
        if (x >= 0 && R.s.card_where[x] == W_TABLE) x = -1;
        for (int k = d * 24; k < d * 24 + 24 && x < 0; k++) if (R.s.card_where[k] == W_DECK && !known[k] && CARD[k].combat) x = k;
        if (x < 0 || !ensure_card(&R.s, x, W_HAND)) { VLOG("  no card of that deck to stand in for the opponent's Combat card"); return 0; }
        known[x] = 0;               /* a guess: the real card replaces it when it shows */
    }
    Leaf lf = snap_leaf;
    leaves_reset();
    for (int k = 0; k < lf.nmv; k++) apply_followed(lf.mv[k]);
    replay_fixup();
    VLOG("  replayed the last move with a Combat card in the opponent's hand");
    return 1;
}
static int first_alive(void) { for (int i = 0; i < nl; i++) if (alive[i]) return i; return -1; }

/* ---- an option (JSON) against a step (the engine's text) ---- */
static int j_equal(const J *a, const J *b) {
    if (!a || !b) return 0;
    if (a->type == J_NUM && b->type == J_NUM) return a->num == b->num;
    if (a->type != b->type) return 0;
    if (a->type == J_STR) return !strcmp(a->str, b->str);
    if (a->type == J_BOOL) return a->num == b->num;
    if (a->type == J_NULL) return 1;
    if (a->type == J_ARR) {
        const J *x = a->child, *y = b->child;
        for (; x && y; x = x->next, y = y->next) if (!j_equal(x, y)) return 0;
        return !x && !y;
    }
    return 0;       /* objects never appear in an option */
}
/* A card's question written differently by the two (0: the same answer, -1: not): yes/no against "the one answer or
 * skip"; a region, a character or a card named by both (a destination as "to", a move as "from>to"). */
static int loose_match(const J *sj, const J *opt) {
    const J *yes = j_get(sj, "yes"), *skip = j_get(opt, "skip");
    /* Scouts into the Stronghold: the referee's "siege", the engine's retreat into the battle's own region */
    const char *ans = j_str(opt, "answer"), *sk = j_str(sj, "kind");
    if (ans && !strcmp(ans, "siege") && sk && !strcmp(sk, "retreat") && R.s.b.active) return region_of(j_str(sj, "region")) == R.s.b.dst ? 0 : -1;
    int oskip = skip && skip->type == J_BOOL && skip->num;
    if (yes && yes->type == J_BOOL) return (yes->num != 0) == !oskip ? 0 : -1;
    const J *sskip = j_get(sj, "skip");
    if (sskip && sskip->type == J_BOOL && sskip->num) return oskip ? 0 : -1;
    static const char *const keys[] = { "region", "character", "target_card", "to" };
    for (int k = 0; k < 4; k++) {
        const char *sv = j_str(sj, keys[k]);
        if (!sv) continue;
        static const char *const okeys[] = { "region", "to", "character", "target_card", "answer" };
        for (int q = 0; q < 5; q++) { const char *ov = j_str(opt, okeys[q]); if (ov && !strcmp(ov, sv)) return 0; }
        return -1;
    }
    return -1;
}
/* score: how many fields agree (-1: one disagrees). The die's number is ours, not the referee's: the face counts. */
static int step_match(const Step *sp, const J *opt) {
    static char buf[300]; char *mem;
    snprintf(buf, sizeof buf, "{%s", sp->text);
    const J *sj = j_parse(buf, &mem);
    if (!sj) return -1;
    int score = 1, bad = 0;      /* 1: the kinds agree (a loose match below scores 0) */
    const char *sk = j_str(sj, "kind"), *ok = j_str(opt, "kind");
    if (sk && ok && !strcmp(sk, "guide_ability") && !strcmp(ok, "use")) { free(mem); return 1; }   /* our GUIDE_ABILITY: use */
    if (!sk || !ok || strcmp(sk, ok)) bad = 1;
    for (const J *f = sj->child; f && !bad; f = f->next) {
        if (!strcmp(f->key, "die") || !strcmp(f->key, "as")) continue;     /* "as": CWOTR writes EVENT for an Event die, the referee the card's type */
        const J *o = j_get(opt, f->key);
        if (!o) continue;
        if (j_equal(f, o)) score++;
        /* an advance with part of the Army: CWOTR advances all; the referee's split is applied after (sel_fixup) */
        else if (!strcmp(f->key, "all") && sk && !strcmp(sk, "advance")) ;
        /* an attack with part of the Army where the engine lists only the whole one (a Character die's attack): the
           referee's unit counts follow and are applied (selection mode) */
        else if (!strcmp(f->key, "all") && sk && (!strcmp(sk, "attack") || (!strcmp(sk, "army_move") && following)) && f->type == J_BOOL && f->num && o->type == J_BOOL && !o->num) ;
        /* a rearguard split CWOTR cannot make (it fills nation by nation): the count may differ */
        else if (!strcmp(f->key, "count") && strstr(sp->text, "\"unit\"") && R.s.dkind == D_ACTION && depth > 0) ;
        /* the die the referee's player picked, where the engine pays with another (apply_leaf pays with the referee's) */
        else if (!strcmp(f->key, "face") && sk && !strcmp(sk, "use_die") && o->type == J_STR && R.s.dkind == D_ACTION && face_of(o->str) >= 0
                 && R.s.dice[R.s.dplayer][face_of(o->str)] > 0) ;
        /* CWOTR never turns a die into a plain Muster (an Army/Muster result does all a Muster does) */
        else if ((!strcmp(f->key, "face") || !strcmp(f->key, "to")) && f->type == J_STR && o->type == J_STR
                 && !strcmp(f->str, "ARMY_MUSTER") && !strcmp(o->str, "MUSTER")) ;
        /* an attack or Army move with a Character die is "..._with_leader" to the referee, whatever the engine's flags say */
        else if (!strcmp(f->key, "action") && f->type == J_STR && o->type == J_STR
                 && ((!strcmp(f->str, "attack") && !strcmp(o->str, "attack_with_leader")) || (!strcmp(f->str, "attack_with_leader") && !strcmp(o->str, "attack"))
                     || (!strcmp(f->str, "move_armies") && !strcmp(o->str, "move_army_with_leader")) || (!strcmp(f->str, "move_army_with_leader") && !strcmp(o->str, "move_armies")))) ;
        else bad = 1;
    }
    if (bad) score = (sk && ok && strcmp(sk, ok)) || (sk && !strcmp(sk, "card_choice")) ? loose_match(sj, opt) : -1;
    free(mem);
    return score;
}
/* A hidden choice (a card discarded face down, the attacker's Combat card before it shows): its kind and deck. */
static int hidden_match(const Step *sp, const J *opt) {
    const char *kind = j_str(opt, "kind"), *deck = j_str(opt, "deck");
    char want[40];
    if (!kind) return -1;
    snprintf(want, sizeof want, "\"kind\":\"%s\"", kind);
    if (!strstr(sp->text, want)) return -1;
    if (deck && !strstr(sp->text, !strcmp(deck, "character") ? "-char-" : "-strat-")) return -1;
    return 0;
}
/* The cards an option names (to put them in the opponent's hand first). */
static void option_cards(const J *opt, int side) {
    const char *kind = j_str(opt, "kind");
    if (!kind) return;
    int card = card_of(j_str(opt, "card"));
    if (card >= 0 && CARD_SIDE(card) == side && (!strcmp(kind, "use_die") || !strcmp(kind, "combat_card") || !strcmp(kind, "discard"))) {
        const char *action = j_str(opt, "action");
        if (!action || !strcmp(action, "play_card")) ensure_card(&R.s, card, W_HAND);
    }
    const J *pay = j_get(opt, "pay");
    if (pay && pay->type == J_ARR) for (const J *p = pay->child; p; p = p->next) if (p->type == J_STR) { int c = card_of(p->str); if (c >= 0) ensure_card(&R.s, c, W_HAND); }
    /* the Shadow forcing A Power too Great / The Power of Tom Bombadil off the table pays with a Character card and an
       Army (Strategy) card, hidden: the stand-ins in its hand are made such cards */
    const char *action = j_str(opt, "action");
    if (side == SH && side != me && action && !strcmp(action, "force_discard")) {
        State *s = &R.s; int hc = 0, hs = 0;
        for (int c = 48; c < 72; c++) hc |= s->card_where[c] == W_HAND;
        for (int c = 72; c < 96; c++) hs |= s->card_where[c] == W_HAND && CARD[c].type == F_ARMY;
        for (int c = 48; c < 72 && !hc; c++) if (s->card_where[c] == W_DECK && !known[c] && ensure_card(s, c, W_HAND)) { known[c] = 0; hc = 1; }
        for (int c = 72; c < 96 && !hs; c++) if (s->card_where[c] == W_DECK && !known[c] && CARD[c].type == F_ARMY && ensure_card(s, c, W_HAND)) { known[c] = 0; hs = 1; }
    }
}

/* Questions the referee asks that the engine settles by itself: the new Guide (the strongest Companion), the
 * unit a card recruits (an Elite if there is one). */
static int settled_by_engine(const char *decision, const J *opt) {
    if (decision && !strcmp(decision, "CHOOSE_GUIDE")) return 1;
    /* a bonus draw the engine makes by itself (Gandalf the Grey's); the Palantir of Orthanc's deck is the engine's question */
    if (decision && !strcmp(decision, "CARD_CHOICE") && opt && j_str(opt, "kind") && !strcmp(j_str(opt, "kind"), "draw")
        && !(R.s.dkind == D_CARD_OPTION && R.s.qtag == Q_DRAW_DECK)) return 1;
    const char *a = opt ? j_str(opt, "answer") : NULL;
    return decision && !strcmp(decision, "CARD_CHOICE") && a && (!strcmp(a, "ELITE") || !strcmp(a, "REGULAR"));
}
/* "done" in the Fellowship phase: CWOTR ends the phase after a change of Guide or a declaration; the referee asks again. */
static int phase_done(const char *decision, const J *opt) {
    const char *k = opt ? j_str(opt, "kind") : NULL;
    return decision && !strcmp(decision, "FELLOWSHIP_PHASE") && k && !strcmp(k, "done");
}
static int engine_in_fsp_phase(void) {
    return R.s.dplayer == FP && (R.s.dkind == D_FSP_DECLARE || R.s.dkind == D_GUIDE || R.s.dkind == D_ENTER_MORDOR);
}
/* The answer the engine already gave to such a question, or -1. */
static int settled_answer(const J *decision) {
    const char *kind = j_str(decision, "kind");
    const J *opts = j_get(decision, "options");
    for (const J *o = opts ? opts->child : NULL; o; o = o->next) {
        const char *a = j_str(o, "answer"), *c = j_str(o, "character");
        if (kind && !strcmp(kind, "CHOOSE_GUIDE") && c && R.s.guide <= GOLLUM && !strcmp(c, R.s.guide == GOLLUM ? "gollum" : comp_key(&R.s, R.s.guide))) return (int)j_get(o, "o")->num;
        if (a && !strcmp(a, "ELITE")) return (int)j_get(o, "o")->num;
        if (a && !strcmp(a, "REGULAR") && (!o->next || !j_str(o->next, "answer") || strcmp(j_str(o->next, "answer"), "ELITE"))) {
            int elite = 0;      /* "REGULAR / skip": CWOTR recruits */
            for (const J *q = opts->child; q; q = q->next) if (j_str(q, "answer") && !strcmp(j_str(q, "answer"), "ELITE")) elite = 1;
            if (!elite) return (int)j_get(o, "o")->num;
        }
        const char *k = j_str(o, "kind"), *u = j_str(o, "unit"), *d = j_str(o, "deck");
        /* the deck the engine drew from (a card drawn before the referee's draw) */
        if (k && !strcmp(k, "draw") && d) for (int i = 0; i < nlate; i++) if ((late[i].deck & 1) == (strcmp(d, "strategy") == 0)) return (int)j_get(o, "o")->num;
    }
    return -1;
}
/* A safe answer while the copy is out of step: no declaring the Fellowship, few Hunt dice. */
/* The answer when the copy cannot place the question (it lost track of the game): chosen from the options with rules
   of thumb and the referee's view, never the engine's move. A battle's strengths: Regulars 1, Elites 2, as in
   casualties. */
static int view_int(const J *o, const char *key);
static const J *battle_units(const J *view, int side) {     /* the attackers, or the defenders' region's units */
    const J *b = view ? j_get(view, "battle") : NULL;
    if (!b || b->type != J_OBJ) return NULL;
    if (side == side_of(j_str(b, "attacker"))) return j_get(b, "attackers");
    const J *regs = j_get(view, "regions"), *r = regs ? j_get(regs, j_str(b, "region")) : NULL;
    return r ? j_get(r, "units") : NULL;
}
static int battle_strength(const J *view, int side) {
    const J *b = view ? j_get(view, "battle") : NULL;
    if (!b || b->type != J_OBJ) return -1;
    int st = 0;
    const J *units = battle_units(view, side);
    for (const J *u = units ? units->child : NULL; u; u = u->next) {
        int n = nation_of(j_str(u, "nation")); const char *k = j_str(u, "unit");
        if (n < 0 || !k || NAT_SIDE(n) != side) continue;
        int c = view_int(u, "n");
        if (!strcmp(k, "regular")) st += c; else if (!strcmp(k, "elite")) st += 2 * c;
    }
    return st;
}
static int opt_of(const J *opts, const char *key, const char *val) {
    for (const J *o = opts ? opts->child : NULL; o; o = o->next) { const char *v = j_str(o, key); if (v && !strcmp(v, val)) return (int)j_get(o, "o")->num; }
    return -1;
}
static int fallback_answer(const J *decision, const J *view) {
    const char *kind = j_str(decision, "kind");
    const J *opts = j_get(decision, "options");
    int s0 = settled_answer(decision), o;
    if (s0 >= 0) return s0;
    if (!kind || !opts) return 0;
    int ours = battle_strength(view, me), theirs = battle_strength(view, 1 - me);
    if (!strcmp(kind, "FELLOWSHIP_PHASE") && (o = opt_of(opts, "kind", "done")) >= 0) return o;
    if (!strcmp(kind, "HUNT_ALLOCATION")) for (const J *x = opts->child; x; x = x->next) if (j_get(x, "count") && (int)j_get(x, "count")->num == 1) return (int)j_get(x, "o")->num;
    if (!strcmp(kind, "COMBAT_CARD") && (o = opt_of(opts, "kind", "none")) >= 0) return o;       /* the card stays for later */
    if (!strcmp(kind, "CASUALTY")) {        /* a Regular of the largest group first, then an Elite reduced, then removed */
        int best = -1, bn = -1;
        for (const J *x = opts->child; x; x = x->next) {
            const char *u = j_str(x, "unit"), *e = j_str(x, "effect");
            if (!u || !e || strcmp(u, "regular") || strcmp(e, "remove")) continue;
            int n = 0; const J *list = battle_units(view, me);
            for (const J *y = list ? list->child : NULL; y; y = y->next) if (j_str(y, "nation") && j_str(x, "nation") && !strcmp(j_str(y, "nation"), j_str(x, "nation")) && j_str(y, "unit") && !strcmp(j_str(y, "unit"), "regular")) n += view_int(y, "n");
            if (n > bn) { bn = n; best = (int)j_get(x, "o")->num; }
        }
        if (best >= 0) return best;
        if ((o = opt_of(opts, "effect", "reduce")) >= 0) return o;
    }
    if (!strcmp(kind, "CEASE_ATTACK")) return (o = opt_of(opts, "kind", ours >= 0 && theirs >= 0 && ours < theirs ? "cease" : "continue")) >= 0 ? o : 0;
    if (!strcmp(kind, "RETREAT")) {         /* stand unless clearly outnumbered */
        if (ours >= 0 && theirs >= 0 && 2 * ours < theirs) { for (const J *x = opts->child; x; x = x->next) if (j_str(x, "kind") && !strcmp(j_str(x, "kind"), "retreat")) return (int)j_get(x, "o")->num; }
        if ((o = opt_of(opts, "kind", "stand")) >= 0) return o;
    }
    if (!strcmp(kind, "FIELD_OR_SIEGE")) return (o = opt_of(opts, "kind", ours >= 0 && theirs >= 0 && ours < theirs ? "siege" : "field")) >= 0 ? o : 0;
    if (!strcmp(kind, "ADVANCE")) for (const J *x = opts->child; x; x = x->next) if (j_str(x, "kind") && !strcmp(j_str(x, "kind"), "advance") && j_get(x, "all") && j_get(x, "all")->num) return (int)j_get(x, "o")->num;
    if (!strcmp(kind, "EXTEND_SIEGE") && (o = opt_of(opts, "kind", "stop")) >= 0) return o;
    if (!strcmp(kind, "WHITE_RIDER") && (o = opt_of(opts, "kind", "use")) >= 0) return o;
    if (!strcmp(kind, "ARMY_SELECTION")) {  /* every unit goes */
        int best = -1, bc = -1;
        for (const J *x = opts->child; x; x = x->next) { const J *c = j_get(x, "count"); if (c && c->num > bc) { bc = (int)c->num; best = (int)j_get(x, "o")->num; } }
        if (best >= 0) return best;
    }
    if (!strcmp(kind, "CHOOSE_ATTACK")) for (const J *x = opts->child; x; x = x->next) if (j_get(x, "all") && j_get(x, "all")->num) return (int)j_get(x, "o")->num;
    if (!strcmp(kind, "CARD_CHOICE") && (o = opt_of(opts, "answer", "skip")) >= 0) return o;   /* a card's "you may": not */
    return 0;
}
/* Which units go (ARMY_SELECTION): the referee asks unit by unit, about its own set of units in its own order; the engine's
 * moves differ only in how many regulars (c) and elites (d) go. The counts are added up and, when the selection ends,
 * the move with the nearest totals is applied. */
static int sel_reg, sel_eli, sel_battle;
static State adv_before; static int adv_on, adv_src, adv_dst;     /* a partial advance waiting for its split */
static int sel_want[NNAT][4];           /* per nation: regulars, elites, leaders, Nazgul that go (-1: not asked) */
static int sel_char[NCOMP + NMINION];   /* per Companion / Minion: 1 goes, 0 stays, -1 not asked */
/* units an attack left behind that the engine cannot leave behind (leaders, Nazgul): back after the battle */
static int stash_on, stash_src, stash_ldr[NFPNAT], stash_naz;
static void sel_begin(void) {
    selmode = 1; sel_reg = sel_eli = 0; sel_battle = 0;
    if (!adv_on) memset(&adv_before, 0, 0);
    for (int n = 0; n < NNAT; n++) for (int k = 0; k < 4; k++) sel_want[n][k] = -1;
    for (int c = 0; c < NCOMP + NMINION; c++) sel_char[c] = -1;
}
static void stash_restore(void) {
    if (!stash_on || R.s.b.active) return;
    for (int n = 0; n < NFPNAT; n++) R.s.ldr[stash_src][n] += (uint8_t)stash_ldr[n];
    R.s.naz[stash_src] += (uint8_t)stash_naz;
    stash_on = 0; memset(stash_ldr, 0, sizeof stash_ldr); stash_naz = 0;
}
/* After the engine's nearest move: the units moved (or attacking) set to what the referee's answers chose. */
static void sel_fixup(const State *before, Move m) {
    State *s = &R.s;
    int src = m.a, dst = m.b;
    if (s->winner) return;
    if (m.type == M_ATTACK) {
        if (!s->b.active || s->b.src != src) return;
        for (int n = 0; n < NNAT; n++) {
            if (sel_want[n][0] >= 0) { int tot = s->reg[src][n] + s->b.rear_reg[n], w = sel_want[n][0] > tot ? tot : sel_want[n][0]; s->reg[src][n] = (uint8_t)w; s->b.rear_reg[n] = (uint8_t)(tot - w); }
            if (sel_want[n][1] >= 0) { int tot = s->eli[src][n] + s->b.rear_eli[n], w = sel_want[n][1] > tot ? tot : sel_want[n][1]; s->eli[src][n] = (uint8_t)w; s->b.rear_eli[n] = (uint8_t)(tot - w); }
            if (n < NFPNAT && sel_want[n][2] >= 0 && sel_want[n][2] < s->ldr[src][n]) {
                int k = s->ldr[src][n] - sel_want[n][2]; s->ldr[src][n] = (uint8_t)sel_want[n][2]; stash_ldr[n] += k; stash_on = 1; stash_src = src;
            }
        }
        if (sel_want[SAURON][3] >= 0 && sel_want[SAURON][3] < s->naz[src]) { stash_naz += s->naz[src] - sel_want[SAURON][3]; s->naz[src] = (uint8_t)sel_want[SAURON][3]; stash_on = 1; stash_src = src; }
        return;
    }
    if (m.type != M_ARMY || src == dst || dst >= NREG) return;
    for (int n = 0; n < NNAT; n++) {
        uint8_t *a[3] = { s->reg[src], s->eli[src], n < NFPNAT ? s->ldr[src] : NULL }, *b[3] = { s->reg[dst], s->eli[dst], n < NFPNAT ? s->ldr[dst] : NULL };
        const uint8_t *a0[3] = { before->reg[src], before->eli[src], n < NFPNAT ? before->ldr[src] : NULL };
        for (int k = 0; k < 3; k++) {
            if (!a[k] || sel_want[n][k] < 0) continue;
            int moved = a0[k][n] - a[k][n], d = sel_want[n][k] - moved;
            if (d > a[k][n]) d = a[k][n];
            if (-d > b[k][n]) d = -b[k][n];
            a[k][n] = (uint8_t)(a[k][n] - d); b[k][n] = (uint8_t)(b[k][n] + d);
        }
    }
    if (sel_want[SAURON][3] >= 0) { int moved = before->naz[src] - s->naz[src], d = sel_want[SAURON][3] - moved; if (d > s->naz[src]) d = s->naz[src]; if (-d > s->naz[dst]) d = -s->naz[dst]; s->naz[src] = (uint8_t)(s->naz[src] - d); s->naz[dst] = (uint8_t)(s->naz[dst] + d); }
    for (int c = 0; c < NCOMP; c++) if (sel_char[c] >= 0 && before->comp_loc[c] == src) s->comp_loc[c] = (uint8_t)(sel_char[c] ? dst : src);
    for (int k = 0; k < NMINION; k++) if (sel_char[NCOMP + k] >= 0 && before->minion_loc[k] == src) s->minion_loc[k] = (uint8_t)(sel_char[NCOMP + k] ? dst : src);
}
/* The White Rider asked by the engine for an attack that (with the referee's split) brings no Nazgul and not the
 * Witch-king: the referee does not ask it (it changes nothing): not used. */
static void white_rider_unasked(void) {
    const State *s = &R.s;
    if (s->dkind != D_CARD_OPTION || s->qtag != Q_WHITE_RIDER || !s->b.active) return;
    int sr = s->b.att == SH ? s->b.src : s->b.dst;
    if (s->naz[sr] || s->minion_loc[M_WK] == sr) return;
    if (!leaves_ensure() || depth != 0) return;
    for (int i = 0; i < nl; i++) if (alive[i] && L[i].mv[0].a == 255) { VLOG("  The White Rider: no Nazgul in this battle, not asked"); apply_leaf(i); return; }
}
static void sel_finalize(void) {
    if (!selmode) return;
    selmode = 0;
    /* a partial advance (applied already): its split, whatever moves the engine lists now (the next player's) */
    if (adv_on) {
        Move m; memset(&m, 0, sizeof m); m.type = M_ARMY; m.a = (uint8_t)adv_src; m.b = (uint8_t)adv_dst;
        VLOG("  the advance with the referee's split");
        sel_fixup(&adv_before, m);
        adv_on = 0;
        return;
    }
    int best = -1, bc = 1 << 30;
    for (int i = 0; i < nl; i++) {
        if (!alive[i]) continue;
        Move m = L[i].mv[0];
        if (m.type != M_ARMY && m.type != M_ATTACK) continue;
        int cost = abs(m.c - sel_reg) + abs(m.d - sel_eli);
        if (cost < bc) { bc = cost; best = i; }
    }
    if (best < 0 && sel_battle && R.s.b.active) {
        Move m; memset(&m, 0, sizeof m); m.type = M_ATTACK; m.a = R.s.b.src; m.b = R.s.b.dst;
        VLOG("  the attackers of a card's battle chosen by the referee");
        sel_fixup(&R.s, m);
        return;
    }
    if (best < 0) { snprintf(last_note, sizeof last_note, "a selection without a move to match"); return; }
    VLOG("  the selection: %d regulars, %d elites (off by %d)", sel_reg, sel_eli, bc);
    State before = R.s; Move m = L[best].mv[0];
    apply_leaf(best);
    sel_fixup(&before, m);
    snap_fix = 1; snap_fix_before = before; snap_fix_m = m; memcpy(snap_fix_want, sel_want, sizeof sel_want); memcpy(snap_fix_char, sel_char, sizeof sel_char);
    white_rider_unasked();
}
static void replay_fixup(void) {
    if (!snap_fix) return;
    memcpy(sel_want, snap_fix_want, sizeof sel_want); memcpy(sel_char, snap_fix_char, sizeof sel_char);
    stash_on = 0; memset(stash_ldr, 0, sizeof stash_ldr); stash_naz = 0;
    sel_fixup(&snap_fix_before, snap_fix_m);
    white_rider_unasked();
}
/* The count the engine's move (leaf i) gives the unit an ARMY_SELECTION option names: its own split, on a copy. */
static int sel_count(int i, const J *o) {
    const State *s = &R.s;
    Move m = L[i].mv[0];
    if (m.type != M_ARMY && m.type != M_ATTACK) return -1;
    int src = m.a;
    State t = *s; wotr_apply(&t, m);
    const char *nation = j_str(o, "nation"), *unit = j_str(o, "unit"), *ch = j_str(o, "character");
    if (ch) {
        if (m.type == M_ATTACK) return 1;
        for (int c = 0; c < NCOMP; c++) if (!strcmp(ch, comp_key(s, c)) || !strcmp(ch, CHAR_KEY[c])) return s->comp_loc[c] == src && t.comp_loc[c] != src;
        for (int k = 0; k < NMINION; k++) if (!strcmp(ch, minion_key(k))) return s->minion_loc[k] == src && t.minion_loc[k] != src;
        return 1;
    }
    int n = nation_of(nation);
    if (n < 0 || !unit) return -1;
    if (!strcmp(unit, "nazgul")) return m.type == M_ATTACK ? s->naz[src] : (s->naz[src] > t.naz[src] ? s->naz[src] - t.naz[src] : 0);
    if (!strcmp(unit, "leader")) return n >= NFPNAT ? 0 : m.type == M_ATTACK ? s->ldr[src][n] : s->ldr[src][n] - t.ldr[src][n];
    int el = !strcmp(unit, "elite");
    if (m.type == M_ATTACK) {
        if (!t.b.active || t.b.src != src) return -1;
        return el ? s->eli[src][n] - t.b.rear_eli[n] : s->reg[src][n] - t.b.rear_reg[n];
    }
    return el ? s->eli[src][n] - t.eli[src][n] : s->reg[src][n] - t.reg[src][n];
}
/* The engine asks the opponent for a Combat card that the referee never asked (the stand-ins in the opponent's hand
 * could play one, its real hand could not): no card. 1 if applied. */
static int opponent_no_card(void) {
    if (!leaves_ensure() || depth != 0) return 0;
    /* a Fellowship phase the referee did not ask (nothing to do there, by its rules): done, for either side; not while
       our own card's answers are followed (the engine ran ahead: the referee has not got there yet) */
    if ((R.s.dkind == D_FSP_DECLARE || R.s.dkind == D_GUIDE || R.s.dkind == D_ENTER_MORDOR) && (following_own_card || !referee_at_fsp_phase())) return 0;
    if (R.s.dkind == D_FSP_DECLARE || R.s.dkind == D_GUIDE || R.s.dkind == D_ENTER_MORDOR) {
        for (int i = 0; i < nl; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"done\"")) { VLOG("  a Fellowship phase the referee did not ask: done"); apply_leaf(i); return 1; }
        return 0;
    }
    if (R.s.dplayer == me) return 0;
    /* the opponent's Companions' move, over at the referee: the rest stay */
    if (R.s.dkind == D_COMP_MOVE) {
        for (int i = 0; i < nl; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"stay\"")) { VLOG("  the rest of the opponent's Companions stay"); apply_leaf(i); return 1; }
        for (int i = 0; i < nl; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"end\"")) { VLOG("  the opponent's Companions' move ends"); apply_leaf(i); return 1; }
        return 0;
    }
    /* a Combat card: none; a casualty the referee took by itself (no choice: everything goes): a removal */
    const char *want = R.s.dkind == D_BATTLE_CARD ? "\"kind\":\"none\"" : R.s.dkind == D_CASUALTY ? "\"effect\":\"remove\"" : NULL;
    if (!want) return 0;
    for (int i = 0; i < nl; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, want)) { VLOG("  the opponent's %s, not asked by the referee", R.s.dkind == D_BATTLE_CARD ? "Combat card (none)" : "casualty"); apply_leaf(i); return 1; }
    return 0;
}
/* A hand-limit discard straight into the copy: the card named, or (hidden) a stand-in of the deck. */
static void discard_direct(int side, const J *opt) {
    State *s = &R.s;
    int c = card_of(j_str(opt, "card")), deck = deck_of(j_str(opt, "deck"));
    if (c < 0 && deck >= 0) for (int k = (side * 2 + deck) * 24; k < (side * 2 + deck) * 24 + 24 && c < 0; k++) if (s->card_where[k] == W_HAND && !known[k]) c = k;
    if (c < 0 && deck >= 0) for (int k = (side * 2 + deck) * 24; k < (side * 2 + deck) * 24 + 24 && c < 0; k++) if (s->card_where[k] == W_HAND) c = k;
    if (c < 0 || s->card_where[c] != W_HAND) return;
    s->card_where[c] = W_DISCARD; s->hand_n[side]--;
    VLOG("  a discard the copy did not expect: %s", CARD_KEY[c]);
}
/* Many Kings to the Service of Mordor, Pits of Mordor: CWOTR asks the next region with the Regulars run out (it recruits
 * none there); the referee ends the card. Such a question is answered with any region (it changes nothing). 1 if applied. */
static int spent_recruit(void) {
    if (!leaves_ensure() || depth != 0 || R.s.dkind != D_CARD_REGION) return 0;
    int c = card_asking(&R.s), n = c == CARD_PITS_OF_MORDOR ? SAURON : c == CARD_MANY_KINGS_TO_THE_SERVICE_OF_MORDOR ? SOUTHRON : -1;
    if (n < 0 || R.s.pool_reg[n] > 0 || first_alive() < 0) return 0;
    VLOG("  %s: no Regulars left, the region asked changes nothing", CARD[c].name);
    apply_leaf(first_alive());
    return 1;
}
/* The Guide's ability declined before the engine asks it: the referee asks the rest of a Hunt's damage before the
 * separation of a Hobbit Guide taken as its casualty, CWOTR after it. Once the engine gets there: no ability (the damage
 * is Corruption, the one answer left). 1 if applied. */
static int guide_skip_settle(void) {
    if (!guide_skip_pending || !leaves_ensure() || depth != 0 || R.s.dkind != D_HUNT_RESPONSE) return 0;
    guide_skip_pending = 0;
    int pick = -1;
    for (int i = 0; i < nl; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"guide_ability\"")) alive[i] = 0;
    for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"corruption\"")) pick = i;
    for (int i = 0; i < nl; i++) if (alive[i] && i != pick) return 0;          /* another answer left: the referee's choice comes */
    if (pick < 0) return 0;
    VLOG("  the Guide's ability declined before: the Hunt's damage is Corruption");
    apply_leaf(pick);
    return 1;
}
/* A second Army move, recruit or Nazgul flight the engine asks but the referee does not (by its rules nothing more may
 * move: The Ringwraiths Are Abroad's second Army), and the referee is on to another question: the move ends there.
 * 1 if applied. */
static int unasked_end(const char *decision) {
    static const char *const same[] = { "ARMY_MOVE", "ARMY_SELECTION", "RECRUIT", "MOVE_MINIONS", "LEADER_REROLL" };
    if (!decision) return 0;
    for (size_t k = 0; k < sizeof same / sizeof *same; k++) if (!strcmp(decision, same[k])) return 0;
    if (!leaves_ensure() || depth != 0 || !(R.s.dkind == D_ARMY_MOVE2 || R.s.dkind == D_MUSTER2 || R.s.dkind == D_NAZ_MOVE)) return 0;
    for (int i = 0; i < nl; i++) if (alive[i] && L[i].nstep == 1 && strstr(L[i].st[0].text, "\"kind\":\"end\"")) {
        VLOG("  %s's move the referee did not ask (it is at %s): ended", SIDE_KEY[R.s.dplayer], decision);
        apply_leaf(i); return 1;
    }
    return 0;
}
/* Follows one answered decision. look: the events after it (for a hidden Combat card shown later). */
/* a question that comes after the round's Combat cards (the referee is past them) */
static int after_cards(const char *k) {
    static const char *const after[] = { "CASUALTY", "CEASE_ATTACK", "RETREAT", "ADVANCE", "EXTEND_SIEGE", "ACTION", "HUNT_ALLOCATION",
                                         "FELLOWSHIP_PHASE", "DISCARD_EXCESS_CARD", "MOVE_COMPANIONS", "MOVE_MINIONS", "ARMY_MOVE", "RECRUIT" };
    for (size_t i = 0; k && i < sizeof after / sizeof *after; i++) if (!strcmp(k, after[i])) return 1;
    return 0;
}
/* A card's "Eliminate which figure?" (Stormcrow: the Free Peoples choose, "region:TYPE"): CWOTR eliminates one itself
 * (a Leader first, else a Regular of the largest Army); the referee's figure replaces the engine's (snap: before the card). */
static int is_figure_answer(const J *opt) {
    const char *a = j_str(opt, "answer");
    return a && strchr(a, ':') && j_str(opt, "region") && j_str(opt, "unit") && !j_str(opt, "nation");
}
static uint8_t *figure_cell(State *s, int r, int n, const char *u) {
    return !strcmp(u, "regular") ? &s->reg[r][n] : !strcmp(u, "elite") ? &s->eli[r][n] : !strcmp(u, "leader") && n < NFPNAT ? &s->ldr[r][n] : NULL;
}
static void card_figure_settle(const J *opt) {
    if (!snap_ok) return;
    State *s = &R.s;
    int r = region_of(j_str(opt, "region")); const char *u = j_str(opt, "unit");
    if (r < 0) return;
    static const char *const kinds[3] = { "regular", "elite", "leader" };
    for (int n = 0; n < NFPNAT; n++) for (int r0 = 0; r0 < NREG; r0++) for (int k = 0; k < 3; k++) {
        uint8_t *now = figure_cell(s, r0, n, kinds[k]), *was = figure_cell(&snap, r0, n, kinds[k]);
        if (!now || !was || *now >= *was) continue;
        if (r0 == r && !strcmp(kinds[k], u)) return;               /* the same figure */
        uint8_t *to = figure_cell(s, r, n, u);
        if (!to || !*to) continue;
        (*now)++; (*to)--;
        if (k < 2) { if (k == 0) s->pool_reg[n]--; else s->pool_eli[n]--; }
        if (!strcmp(u, "regular")) s->pool_reg[n]++; else if (!strcmp(u, "elite")) s->pool_eli[n]++;
        VLOG("  a card's figure eliminated as the referee did: %s %s in %s (the engine's: %s in %s)", NATION_KEY[n], u, REGION_KEY[r], kinds[k], REGION_KEY[r0]);
        return;
    }
}
static int hunt_sep_hold, hunt_sep_dest = -1;
/* The referee's Action dice by its numbering (its `die`), face only: rolled (action_dice_rolled), changed by an Elven
 * Ring or The Lidless Eye. The Lidless Eye's question names a die by that number, CWOTR's engine a face. */
static int8_t ref_face[2][16]; static int ref_nd[2];     /* a Hunt answer waiting for a separation's destination; that destination */
static int follow_choice_(const J *e, int look);
static int follow_choice(const J *e, int look) { following = 1; int r = follow_choice_(e, look); following = 0; return r; }
static int follow_choice_(const J *e, int look) {
    int deferred = 0;
    if (lost || !started) return 0;
    const char *side_key = j_str(e, "side"), *decision = j_str(e, "decision");
    const J *opt = j_get(e, "option");
    int side = side_of(side_key);
    if (!opt || side < 0) return 0;
    following_own_card = side == me && decision && (!strcmp(decision, "CARD_CHOICE") || !strcmp(decision, "ARMY_SELECTION"));
    if (hunt_sep_hold && decision && !strcmp(decision, "SEPARATE_COMPANIONS") && j_str(opt, "kind") && !strcmp(j_str(opt, "kind"), "separate_to"))
        hunt_sep_dest = region_of(j_str(opt, "to"));
    int ma = decision && !strcmp(decision, "CARD_CHOICE") && ma_question_card(j_str(opt, "card")) && ma_comp_of(j_str(opt, "answer")) >= 0;
    if (ma) {
        wotr_ma_pick = ma_comp_of(j_str(opt, "answer"));
        VLOG("  Mighty Attack: %s forfeits his Leadership", j_str(opt, "answer"));
        if (reroll_wait) replay_for_reroll();
        return 0;
    }
    if (!(decision && !strcmp(decision, "LEADER_REROLL"))) settle_reroll();
    /* The Lidless Eye: the referee's die (its number) as the face CWOTR's engine asks */
    if (decision && !strcmp(decision, "CARD_CHOICE") && j_str(opt, "card") && card_of(j_str(opt, "card")) == CARD_THE_LIDLESS_EYE &&
        R.s.dkind == D_CARD_OPTION && R.s.dplayer == SH && card_asking(&R.s) == CARD_THE_LIDLESS_EYE) {
        const J *sk = j_get(opt, "skip"); const char *ans = j_str(opt, "answer");
        int d = ans ? atoi(ans) : -1, face = sk && sk->type == J_BOOL && sk->num ? 255 : d >= 0 && d < ref_nd[SH] ? ref_face[SH][d] : -1;
        Move mv[MAXMOVES]; int n = wotr_moves(&R.s, mv), k = -1;
        for (int i = 0; i < n; i++) if (mv[i].type == M_CHOICE && mv[i].a == face) k = i;
        if (k >= 0) {
            VLOG("  The Lidless Eye: the referee's die %d (%s)", d, face == 255 ? "no more" : FACE_KEY[face]);
            if (face != 255) ref_face[SH][d] = F_SPECIAL;
            leaves_reset(); apply_followed(mv[k]); leaves_reset();
            return 0;
        }
    }
    int ev_face = -1, ev_ring_to = -1;
    if (decision && !strcmp(decision, "ACTION")) {
        const char *fk = j_str(opt, "face"), *kk = j_str(opt, "kind"), *tk = j_str(opt, "to");
        ev_face = fk && kk && !strcmp(kk, "use_die") ? face_of(fk) : -1;
        ev_ring_to = tk && kk && !strcmp(kk, "elven_ring") ? face_of(tk) : -1;
        { const J *dj = j_get(opt, "die"); int d = dj && dj->type == J_NUM ? (int)dj->num : -1;
          if (ev_ring_to >= 0 && d >= 0 && d < ref_nd[side]) ref_face[side][d] = (int8_t)ev_ring_to; }
    }
    const J *hidden = j_get(opt, "hidden");
    /* the attacker's hidden Combat card: the card itself comes with combat_cards_revealed */
    J *real_opt = NULL; char *real_mem = NULL;
    if (hidden && hidden->type == J_BOOL && hidden->num && !strcmp(j_str(opt, "kind") ? j_str(opt, "kind") : "", "combat_card")) {
        for (int k = look; k < nev; k++) {
            char *mem; const J *m = j_parse(ev[k], &mem);
            const J *x = m ? j_get(m, "event") : NULL;
            const char *kk = x ? j_str(x, "kind") : NULL;
            if (kk && !strcmp(kk, "combat_cards_revealed")) {
                const char *role = R.s.b.active && R.s.b.att == side ? "attacker_card" : "defender_card";
                const char *card = j_str(x, role);
                if (!card) card = j_str(x, side == SH ? "shadow_card" : "free_peoples_card");
                if (card) {
                    char buf[200]; snprintf(buf, sizeof buf, "{\"kind\":\"combat_card\",\"card\":\"%s\"}", card);
                    real_opt = (J *)j_parse(buf, &real_mem);
                }
                free(mem); break;
            }
            if (m) free(mem);
        }
        if (real_opt) opt = real_opt;
    }
    /* "The Ents Awake" (Treebeard, Huorns, Entmoot): a further Character card declined where CWOTR asks none (its card
       ends there) */
    int ents = card_of(j_str(opt, "card"));
    ents = ents == CARD_THE_ENTS_AWAKE_TREEBEARD || ents == CARD_THE_ENTS_AWAKE_HUORNS || ents == CARD_THE_ENTS_AWAKE_ENTMOOT;
    if (decision && !strcmp(decision, "CARD_CHOICE") && ents &&
        j_get(opt, "skip") && !(leaves_ensure() && R.s.dplayer == side && R.s.dkind != D_ACTION && depth >= 0 && first_alive() >= 0 &&
                                !leaf_done(first_alive()) && strstr(L[first_alive()].st[depth].text, "card_choice"))) goto done;
    /* a card's Army move of ours: where it goes, for the units' question after it */
    if (decision && !strcmp(decision, "CARD_CHOICE") && side == me) {
        const char *to = j_str(opt, "to") ? j_str(opt, "to") : j_str(opt, "region");
        if (to && region_of(to) >= 0) card_move_to = region_of(to);
    }
    if (decision && !strcmp(decision, "ACTION") && side == me) card_move_to = -1;
    /* a card's "How many Regulars in <region>?" (the referee's only): CWOTR fills the regions itself, no step */
    if (decision && !strcmp(decision, "CARD_CHOICE") && j_get(opt, "count") && !j_get(opt, "region") && j_str(opt, "card") &&
        (!strcmp(j_str(opt, "card"), "sh-strat-17") || !strcmp(j_str(opt, "card"), "sh-strat-24"))) goto done;
    /* the official rules' Leader re-roll ("both players can re-roll"): CWOTR always re-rolls, without a question; the
     * referee's dice that follow are the re-roll's (a side that declines rolls none: the engine's copy differs then) */
    if (decision && !strcmp(decision, "LEADER_REROLL")) {
        const char *answer = j_str(opt, "answer");
        if (answer && strcmp(answer, "reroll")) LOSE("%s declined its Leader re-roll (CWOTR always re-rolls)", side_key);
        else replay_for_reroll();
        goto done;
    }
    /* "continue" where the engine does not ask (a card's battle that may not be ceased: Corsairs of Umbar): nothing to follow */
    if (decision && !strcmp(decision, "CEASE_ATTACK") && j_str(opt, "kind") && !strcmp(j_str(opt, "kind"), "continue") &&
        !(leaves_ensure() && R.s.dplayer == side && R.s.dkind == D_CONTINUE)) { VLOG("  the battle goes on (the engine asks no cease here)"); goto done; }
    /* the new Guide on a tie: CWOTR picks one itself; the referee's answer is the one played */
    if (decision && !strcmp(decision, "CHOOSE_GUIDE")) {
        int c = char_of(j_str(opt, "character"));
        if (c == CH_GANDALF_WHITE) c = C_GANDALF; else if (c == CH_ARAGORN) c = C_STRIDER; else if (c == CH_GOLLUM) c = GOLLUM;
        if (c >= 0 && c <= GOLLUM && (c == GOLLUM || R.s.comp_loc[c] == LOC_FSP)) { R.s.guide = (uint8_t)c; leaves_reset(); }
        goto done;
    }
    /* a card's Nation that CWOTR chose itself (it offers only the Nations the card changes: Wisdom of Elrond with one
       Nation not at war), the referee offering every one: settled; another Nation than the engine's is played instead */
    if (decision && !strcmp(decision, "CARD_CHOICE") && j_str(opt, "nation") && !j_str(opt, "region") &&
        !(leaves_ensure() && R.s.dplayer == side && R.s.dkind == D_CARD_NATION)) {
        int want = nation_of(j_str(opt, "nation")), c = card_of(j_str(opt, "card")), k = -1;
        if (snap_ok && c == CARD_WISDOM_OF_ELROND && want >= 0 && want < NFPNAT) {
            for (int n = 0; n < NFPNAT && k < 0; n++) if (snap.pol[n] != R.s.pol[n] || snap.active[n] != R.s.active[n]) k = n;
            if (k >= 0 && k != want) {
                R.s.pol[k] = snap.pol[k]; R.s.active[k] = snap.active[k];
                activate(&R.s, want); advance_pol(&R.s, want, 0);
                VLOG("  %s: %s as the referee chose (the engine's: %s)", CARD[c].name, NATION_KEY[want], NATION_KEY[k]);
            }
        }
        VLOG("  a card's Nation the engine chose itself: settled"); goto done;
    }
    if (decision && !strcmp(decision, "CARD_CHOICE") && is_figure_answer(opt) &&
        !(leaves_ensure() && R.s.dplayer == side && R.s.dkind >= D_CARD_REGION)) { card_figure_settle(opt); goto done; }
    /* the way a card's Army move just made goes ("Which way?", asked right after the move: its path, "via>to" or the
       destination alone): CWOTR moves it at once */
    int was_route_pending = route_pending; route_pending = 0;
    if (decision && !strcmp(decision, "CARD_CHOICE") && R.s.reloc_on && was_route_pending && j_str(opt, "answer")) {
        const char *a = j_str(opt, "answer"), *last = strrchr(a, '>');
        if (region_of(last ? last + 1 : a) == R.s.reloc_to) { VLOG("  the way of a card's move: settled"); goto done; }
    }
    /* a card's Army move, "from>to" in one answer: CWOTR asks the Army (its region), then where it goes */
    if (decision && !strcmp(decision, "CARD_CHOICE") && j_str(opt, "from") && j_str(opt, "to") && strcmp(j_str(opt, "kind") ? j_str(opt, "kind") : "", "move_companion")) {
        int from = region_of(j_str(opt, "from")), to = region_of(j_str(opt, "to")), used = 0;
        for (int stage = 0; stage < 2; stage++) {
            if (!leaves_ensure() || R.s.dplayer != side || depth != 0) break;
            int want = R.s.dkind == D_CARD_ARMY ? from : R.s.dkind == D_CARD_REGION ? to : -1, pick = -1;
            if (want < 0) break;
            char key[64]; snprintf(key, sizeof key, "\"region\":\"%s\"", REGION_KEY[want]);
            for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, key)) pick = i;
            if (pick < 0) break;
            apply_leaf(pick); used = 1;
            if (want == to) break;
        }
        if (!used) VLOG("  a card's route or move the engine made already: %s>%s", j_str(opt, "from"), j_str(opt, "to"));
        route_pending = 1;
        goto done;
    }
    /* a card's question about a move the engine made already (the route of a card move: "Which way?") */
    const char *route = j_str(opt, "answer");
    int asks_it = 0;            /* the engine asks this very answer (Scouts' retreat, say): not a route */
    if (leaves_ensure() && R.s.dplayer == side) for (int i = 0; i < nl && !asks_it; i++) asks_it = alive[i] && !leaf_done(i) && step_match(&L[i].st[depth], opt) > 0;
    if (decision && !strcmp(decision, "CARD_CHOICE") && (j_str(opt, "region") || (route && strchr(route, '>') && !j_str(opt, "from"))) && R.s.reloc_on && !asks_it && !(leaves_ensure() && R.s.dplayer == side && (R.s.dkind == D_CARD_REGION || R.s.dkind == D_CARD_ARMY))) {
        VLOG("  the route of a card's move: settled"); goto done;
    }
    {
        const char *ans = j_str(opt, "answer");
        if (decision && !strcmp(decision, "CARD_CHOICE") && ans && (!strcmp(ans, "REGULAR") || !strcmp(ans, "ELITE"))) { recruit_answer(ans); goto done; }
        /* "skip" for a recruit (a Shadow card's "Recruit which ... figure?"): not when the engine asks this card's question */
        /* (Regulars the card names, asked only when the reinforcements run short: CWOTR recruited as many, in the same
           region, which the referee's Regular-or-skip answers only spread differently over the card's items) */
        if (decision && !strcmp(decision, "CARD_CHOICE") && ans && !strcmp(ans, "skip") && R.s.rec_n > 0 &&
            !(leaves_ensure() && R.s.dplayer == side && depth == 0 && R.s.dkind >= D_SELF_HITS)) { recruit_answer("skip"); goto done; }
    }
    if (settled_by_engine(decision, opt)) { VLOG("  settled by the engine: %s", decision); goto done; }
    /* the army of a card's battle: the engine started that battle already */
    if (decision && !strcmp(decision, "CHOOSE_ATTACK") && R.s.b.active && depth <= 0 && region_of(j_str(opt, "to")) == R.s.b.dst) { VLOG("  the card's battle has begun in the engine"); goto done; }
    if (selmode && !(decision && !strcmp(decision, "ARMY_SELECTION"))) sel_finalize();
    if (decision && !strcmp(decision, "ARMY_SELECTION") && j_str(opt, "kind") && !strcmp(j_str(opt, "kind"), "count")) {
        /* the units of a card's whole-Army move (CWOTR moved them all, the engine may be at the next Action): the
           referee's army_moved that follows says what went */
        if (!selmode && R.s.reloc_on) { VLOG("  a card's Army move: its units come with army_moved"); goto done; }
        /* a partial advance whose selection the copy dropped (a move applied since): army_moved settles it too */
        if (!selmode && adv_on) { VLOG("  the advance's units come with army_moved"); goto done; }
        if (!selmode) {
            int any = 0;
            if (leaves_ensure() && R.s.dplayer == side) for (int i = 0; i < nl; i++) if (alive[i] && (L[i].mv[0].type == M_ARMY || L[i].mv[0].type == M_ATTACK)) any = 1;
            /* a battle a card started (Corsairs of Umbar, The Black Captain Commands): the attackers chosen likewise */
            int card_battle = !any && R.s.b.active && R.s.b.att == side && R.s.b.round <= 1;
            if (!any && !card_battle) { snprintf(last_note, sizeof last_note, "a selection where the engine has no Army move"); st.ignored++; goto done; }
            sel_begin(); sel_battle = card_battle;
        }
        const char *unit = j_str(opt, "unit"); const J *cnt = j_get(opt, "count");
        int k = cnt && cnt->type == J_NUM ? (int)cnt->num : 0;
        if (unit && !strcmp(unit, "regular")) sel_reg += k; else if (unit && !strcmp(unit, "elite")) sel_eli += k;
        int nn = nation_of(j_str(opt, "nation")), uk = !unit ? -1 : !strcmp(unit, "regular") ? 0 : !strcmp(unit, "elite") ? 1 : !strcmp(unit, "leader") ? 2 : !strcmp(unit, "nazgul") ? 3 : -1;
        if (nn >= 0 && uk >= 0) sel_want[nn][uk] = k;
        const char *ch = j_str(opt, "character");
        if (ch) {
            int c = char_of(ch);
            if (c == CH_GANDALF_WHITE) c = C_GANDALF; else if (c == CH_ARAGORN) c = C_STRIDER;
            if (c >= 0 && c < NCOMP) sel_char[c] = k > 0; else if (c >= CH_MINION0 && c < CH_MINION0 + NMINION) sel_char[NCOMP + c - CH_MINION0] = k > 0;
        }
        goto done;
    }
    /* GUIDE_ABILITY "skip": CWOTR asks the ability with the Hunt's damage; the moves without it stay */
    if (decision && !strcmp(decision, "GUIDE_ABILITY") && leaves_ensure()) {
        const char *k = j_str(opt, "kind");
        if (k && !strcmp(k, "skip")) {
            if (!(R.s.dkind == D_HUNT_RESPONSE && R.s.dplayer == side)) guide_skip_pending = 1;
            for (int i = 0; i < nl; i++) if (alive[i] && !leaf_done(i) && strstr(L[i].st[depth].text, "guide_ability")) alive[i] = 0;
            goto done;
        }
    }
    if (side != me) option_cards(opt, side);
    /* an excess unit the referee asks about where the engine removed it by itself (one choice) or not yet */
    if (decision && !strcmp(decision, "REMOVE_EXCESS_UNIT") && !(leaves_ensure() && R.s.dplayer == side && R.s.dkind == D_CARD_OPTION && R.s.qtag == Q_EXCESS)) {
        int r = region_of(j_str(opt, "region")), n = nation_of(j_str(opt, "nation")); const char *u = j_str(opt, "unit");
        if (r >= 0 && n >= 0 && u) {
            int limit = R.s.siege[r] && R.s.ctrl[r] == side ? 5 : 10;
            if (units(&R.s, side, r) > limit) {
                if (!strcmp(u, "elite") && R.s.eli[r][n]) { R.s.eli[r][n]--; R.s.pool_eli[n]++; }
                else if (R.s.reg[r][n]) { R.s.reg[r][n]--; R.s.pool_reg[n]++; }
                VLOG("  an excess unit removed as the referee did: %s %s in %s", NATION_KEY[n], u, REGION_KEY[r]);
            } else VLOG("  an excess unit the engine removed already");
        }
        goto done;
    }
    /* the end of a Companions' move: the groups CWOTR still asks about stay */
    if (decision && !strcmp(decision, "MOVE_COMPANIONS") && j_str(opt, "kind") && !strcmp(j_str(opt, "kind"), "end")) {
        for (int g = 0; g < 8 && leaves_ensure() && R.s.dplayer == side && R.s.dkind == D_COMP_MOVE && depth == 0; g++) {
            int pick = -1;
            for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"stay\"")) pick = i;
            for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"end\"")) pick = i;
            if (pick < 0) break;
            VLOG("  the rest of the Companions stay");
            apply_leaf(pick);
        }
        if (!(leaves_ensure() && R.s.dplayer == side && R.s.dkind == D_COMP_MOVE)) goto done;
    }
    /* a separation CWOTR made without asking (one way to do it: I Will Go Alone with one Companion left), which the
       referee asks anyway: the engine is past it */
    if (decision && !strcmp(decision, "SEPARATE_COMPANIONS") && leaves_ensure() && (R.s.dplayer != side || (R.s.dkind == D_ACTION && depth == 0)))
        { VLOG("  a separation the engine made already"); goto done; }
    /* the Companions of a separation, one at a time in the player's order: the engine's steps (in its own order) are
       reordered to the referee's (a group is a set) */
    if (decision && !strcmp(decision, "SEPARATE_COMPANIONS") && j_str(opt, "character") && leaves_ensure() && R.s.dplayer == side && depth >= 0) {
        char want[80]; snprintf(want, sizeof want, "\"character\":\"%s\"", j_str(opt, "character"));
        for (int i = 0; i < nl; i++) {
            if (!alive[i] || leaf_done(i) || !strstr(L[i].st[depth].text, "\"kind\":\"separate\"") || strstr(L[i].st[depth].text, want)) continue;
            for (int j = depth + 1; j < L[i].nstep && strstr(L[i].st[j].text, "\"kind\":\"separate\""); j++)
                if (strstr(L[i].st[j].text, want)) { Step t = L[i].st[depth]; L[i].st[depth] = L[i].st[j]; L[i].st[j] = t; break; }
        }
    }
    if (decision && !strcmp(decision, "JOIN_GROUP") && leaves_ensure() && R.s.dplayer == side) join_order(j_str(opt, "character"));
    if (decision && !strcmp(decision, "MOVE_COMPANIONS") && leaves_ensure() && R.s.dplayer == side && R.s.dkind == D_COMP_MOVE) comp_lead(j_str(opt, "character"));
    if (decision && !strcmp(decision, "CARD_CHOICE") && card_of(j_str(opt, "card")) == CARD_THE_BREAKING_OF_THE_FELLOWSHIP && leaves_ensure() && R.s.dplayer == side) {
        char same[64]; snprintf(same, sizeof same, "\"card\":\"%s\"", CARD_KEY[CARD_THE_BREAKING_OF_THE_FELLOWSHIP]);
        set_order(j_str(opt, "character"), same);
    }
    int forced = 0;
    for (int guard = 0; guard < 40; guard++) {
        if (!leaves_ensure()) { VLOG("  no decision in the engine for %s %s", side_key, decision); st.ignored++; break; }
        if (spent_recruit() || guide_skip_settle() || unasked_end(decision)) continue;
        /* the engine asks a Combat card the referee never asked (by its reading the side had none to play), and the
           referee is already further (any other question): none */
        if (R.s.dkind == D_BATTLE_CARD && depth <= 0 && after_cards(decision) && forced < 4) {
            int pick = -1;
            for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"none\"")) pick = i;
            if (pick >= 0) { forced++; VLOG("  %s's Combat card, not asked by the referee: none", SIDE_KEY[R.s.dplayer]); apply_leaf(pick); continue; }
        }
        if (R.s.dplayer != side) {
            if (phase_done(decision, opt)) { VLOG("  the engine ended the Fellowship phase already"); break; }
            /* the Companions' move is over at the referee (no group left to it): the engine's remaining groups stay */
            if (R.s.dkind == D_COMP_MOVE && depth == 0 && forced++ < 8) {
                int pick = -1;
                for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"stay\"")) pick = i;
                for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"end\"")) pick = i;
                if (pick >= 0) { VLOG("  the rest of the Companions stay"); apply_leaf(pick); continue; }
            }
            if (opponent_no_card()) continue;
            /* a Combat card question the referee skipped (by its reading, that side had no card to play): none, our
               own side too (the other side's choice comes first) */
            if (R.s.dkind == D_BATTLE_CARD && depth <= 0 && (after_cards(decision) || (decision && !strcmp(decision, "COMBAT_CARD"))) && forced++ < 3) {
                int pick = -1;
                for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"none\"")) pick = i;
                if (pick >= 0) {
                    if (verbose) { char b[400] = ""; size_t bn = 0; for (int i = 0; i < R.s.nopt && bn < 300; i++) if (R.s.opt[i] < NCARD) bn += (size_t)snprintf(b + bn, sizeof b - bn, " %s", CARD_KEY[R.s.opt[i]]);
                                   VLOG("  %s's Combat card, not asked by the referee: none (the engine offered:%s)", SIDE_KEY[R.s.dplayer], b); }
                    apply_leaf(pick); continue;
                }
            }
            /* a card's question that the referee asks before a separation's destination, CWOTR after it (Worn with
               Sorrow and Toil after a Hobbit lost to the Hunt): it waits for the separation */
            if (decision && !strcmp(decision, "CARD_CHOICE") && R.s.dkind == D_SEP_DEST) { VLOG("  %s's card answer waits for the separation", side_key); deferred = 1; break; }
            /* the rest of a Hunt's damage after a Hobbit Guide is separated (the next Hobbit's Guide ability, a
               casualty): the referee asks it first and separates every Companion of that Hunt with one destination,
               CWOTR asks each one's destination at once: it waits, and the destination is given to each in turn */
            if (decision && (!strcmp(decision, "GUIDE_ABILITY") || !strcmp(decision, "HUNT_CASUALTY")) && R.s.dkind == D_SEP_DEST) {
                VLOG("  %s's Hunt answer waits for the separation", side_key); hunt_sep_hold = 1; deferred = 1; break;
            }
            /* both sides over the hand limit: the referee asks the Shadow first, CWOTR the Free Peoples: wait for it */
            if (decision && !strcmp(decision, "DISCARD_EXCESS_CARD") && R.s.dkind == D_DISCARD) { VLOG("  %s's discard waits for the engine", side_key); deferred = 1; break; }
            /* a discard the copy did not expect (its count of that hand was off): made in the copy as it is */
            if (decision && !strcmp(decision, "DISCARD_EXCESS_CARD")) { discard_direct(side, opt); break; }
            /* the engine asks the other side: a question the referee settled by itself (one option) is applied */
            int i = first_alive(), distinct = 1;
            for (int k = 0; k < nl; k++) if (alive[k] && !leaf_done(k) && strcmp(L[k].st[depth].text, L[i].st[depth].text)) distinct = 0;
            if (distinct && i >= 0 && R.s.dkind != D_ACTION && forced++ < 3) { st.forced++; if (leaf_done(i)) apply_leaf(i); else { depth++; if (all_done()) apply_leaf(first_alive()); } continue; }
            if (decision && !strcmp(decision, "COMBAT_CARD") && side != me && replay_with_card(side, card_of(j_str(opt, "card")), deck_of(j_str(opt, "deck")) < 0 ? 1 : deck_of(j_str(opt, "deck")))) continue;
            VLOG("  out of step: the referee's %s %s, the engine asks %s (%s)", side_key, decision, SIDE_KEY[R.s.dplayer], recent[(nrecent + 7) % 8]);
            LOSE("followed %s %s %s, the engine asks %s (%d) [%s %s]", side_key, decision, j_str(opt, "kind"), SIDE_KEY[R.s.dplayer], R.s.dkind,
                 j_str(opt, "card") ? j_str(opt, "card") : "", j_str(opt, "answer") ? j_str(opt, "answer") : ""); break;
        }
        int best = -1;
        /* (a die action with hidden parts, the cards paying a forced discard: matched like any action) */
        int hid = hidden && hidden->type == J_BOOL && hidden->num && opt != real_opt && !(j_str(opt, "kind") && !strcmp(j_str(opt, "kind"), "use_die"));
        for (int i = 0; i < nl; i++) {
            if (!alive[i] || leaf_done(i)) continue;
            int sc = hid ? hidden_match(&L[i].st[depth], opt) : step_match(&L[i].st[depth], opt);
            if (sc > best) best = sc;
        }
        if (best >= 0) {
            const char *chosen = NULL;
            for (int i = 0; i < nl; i++) {
                if (!alive[i]) continue;
                int sc = leaf_done(i) ? -1 : hid ? hidden_match(&L[i].st[depth], opt) : step_match(&L[i].st[depth], opt);
                /* our own planned move, answered as far as the referee's options allow (an attack with "all":false
                   offered only): it stays the move followed */
                if (i == target && side == me && sc >= 0 && sc < best) continue;
                if (sc != best) { if (i == target) VLOG("  target dropped at step %d: score %d < %d: %s", depth, sc, best, leaf_done(i) ? "-" : L[i].st[depth].text); alive[i] = 0; continue; }
                if (hid) { if (!chosen) chosen = L[i].st[depth].text; else if (strcmp(chosen, L[i].st[depth].text)) alive[i] = 0; }
            }
            if (depth == 0 && R.s.dkind == D_ACTION) { act_face = ev_face; act_ring_to = ev_ring_to; }     /* the die of the move now followed */
            depth++;
            if (j_get(opt, "all") && j_get(opt, "all")->type == J_BOOL && !j_get(opt, "all")->num) {
                const char *ok2 = j_str(opt, "kind");
                if (ok2 && !strcmp(ok2, "advance") && R.s.b.active) {        /* a partial advance: advance all, then the split */
                    State before = R.s; int src = R.s.b.src, dst = R.s.b.dst;
                    if (all_done()) apply_leaf(first_alive());
                    adv_before = before; adv_src = src; adv_dst = dst; adv_on = 1;
                    sel_begin();
                    break;
                }
                sel_begin(); break;     /* units asked next */
            }
            if (all_done()) apply_leaf(first_alive());
            break;
        }
        /* a unit the referee does not ask about (or asks later): the engine's later selection steps */
        if (j_str(opt, "kind") && !strcmp(j_str(opt, "kind"), "count")) {
            int skip = 0;
            for (int k = 1; k <= 4 && !skip; k++)
                for (int i2 = 0; i2 < nl && !skip; i2++)
                    if (alive[i2] && L[i2].nstep > depth + k && step_match(&L[i2].st[depth + k], opt) >= 0) skip = k;
            if (skip) { VLOG("  %d selection step(s) skipped", skip); depth += skip; continue; }
        }
        /* a card's question the referee did not ask (no such choice by its rules): the "nothing" answer */
        if (depth == 0 && (R.s.dkind == D_CARD_OPTION || R.s.dkind == D_CARD_REGION || R.s.dkind == D_CARD_ARMY) && forced++ < 3) {
            int pick = -1;
            for (int k = 0; k < nl && pick < 0; k++)
                if (alive[k] && L[k].nstep && (strstr(L[k].st[0].text, "\"skip\":true") || strstr(L[k].st[0].text, "\"answer\":\"0\"") || strstr(L[k].st[0].text, "\"answer\":\"skip\""))) pick = k;
            if (pick >= 0) { VLOG("  a card's question the referee did not ask: nothing"); apply_leaf(pick); continue; }
        }
        /* a "join or stay" the referee did not ask (it went on): that Companion stays */
        if (depth > 0 && !(decision && !strcmp(decision, "JOIN_GROUP"))) {
            int any = 0, stays = 0;
            for (int i = 0; i < nl; i++) if (alive[i] && !leaf_done(i)) {
                const char *t = L[i].st[depth].text;
                if (strstr(t, "\"kind\":\"stay\"") || strstr(t, "\"kind\":\"join\"")) { any++; stays += strstr(t, "\"kind\":\"stay\"") != NULL; }
                else { any = -1000; break; }
            }
            if (any > 0 && stays > 0) {
                for (int i = 0; i < nl; i++) if (alive[i] && !leaf_done(i) && strstr(L[i].st[depth].text, "\"kind\":\"join\"")) alive[i] = 0;
                VLOG("  a join-or-stay the referee did not ask: stays");
                depth++;
                if (all_done()) apply_leaf(first_alive());
                continue;
            }
        }
        /* a Hunt answer after a Hobbit Guide is separated, the engine asking his destination first: it waits (above) */
        if (decision && (!strcmp(decision, "GUIDE_ABILITY") || !strcmp(decision, "HUNT_CASUALTY")) && R.s.dkind == D_SEP_DEST) {
            VLOG("  %s's Hunt answer waits for the separation", side_key); hunt_sep_hold = 1; deferred = 1; break;
        }
        /* no step agrees: a single-answer step the referee skipped, or a question the engine does not ask */
        int i = first_alive(), distinct = 1;
        for (int k = 0; k < nl; k++) if (alive[k] && !leaf_done(k) && strcmp(L[k].st[depth].text, L[i].st[depth].text)) distinct = 0;
        if (distinct && i >= 0 && !leaf_done(i) && depth > 0) { st.forced++; depth++; if (all_done()) apply_leaf(first_alive()); continue; }
        if (decision && !strcmp(decision, "COMBAT_CARD") && side != me && replay_with_card(side, card_of(j_str(opt, "card")), deck_of(j_str(opt, "deck")) < 0 ? 1 : deck_of(j_str(opt, "deck")))) continue;
        if (phase_done(decision, opt)) { VLOG("  the engine ended the Fellowship phase already"); break; }
        if (verbose) {
            char *o = NULL; Buf b = {0};
            for (const J *f = opt->child; f; f = f->next) { bcomma(&b); if (f->type == J_STR) bput(&b, "%s=%s", f->key, f->str); else if (f->type == J_NUM) bput(&b, "%s=%g", f->key, f->num); else bput(&b, "%s", f->key); }
            o = b.p;
            VLOG("  ignored: %s %s {%s} (the engine: %s)", side_key, decision, o ? o : "", i >= 0 && !leaf_done(i) ? L[i].st[depth].text : "-");
            free(o);
        }
        st.ignored++;
        snprintf(last_note, sizeof last_note, "ignored %s %s %s", side_key, decision, j_str(opt, "kind"));
        break;
    }
done:
    if (real_mem) free(real_mem);
    return deferred;
}

/* ---- the random outcomes ---- */
static void queue_random(const J *x) {
    const char *k = j_str(x, "kind");
    Rand r = {0};
    int side = side_of(j_str(x, "side"));
    r.side = (uint8_t)(side < 0 ? 0 : side);
    if (!strcmp(k, "random_action_dice")) {
        const J *faces = j_get(x, "faces");
        for (const J *f = faces ? faces->child : NULL; f; f = f->next) if (f->type == J_STR) { r.type = RQ_DICE; r.val = (int16_t)face_of(f->str); rq_add(r); }
    } else if (!strcmp(k, "random_card_draw")) {
        int deck = deck_of(j_str(x, "deck"));
        r.type = RQ_DRAW; r.deck = (uint8_t)(r.side * 2 + (deck < 0 ? 0 : deck)); r.val = (int16_t)card_of(j_str(x, "card")); rq_add(r);
    } else if (!strcmp(k, "random_hunt_tile")) {
        r.type = RQ_TILE; r.val = (int16_t)tile_of(j_str(x, "tile")); rq_add(r);
    } else if (!strcmp(k, "random_companion")) {
        r.type = RQ_COMP; r.val = (int16_t)char_of(j_str(x, "character")); rq_add(r);
    } else if (!strcmp(k, "random_hand_card")) {
        r.type = RQ_HAND; r.val = (int16_t)card_of(j_str(x, "card")); rq_add(r);
    } else if (!strcmp(k, "random_roll")) {
        const char *roll = j_str(x, "roll");
        r.type = RQ_D6; r.roll = roll && !strcmp(roll, "combat") ? 1 : roll && !strcmp(roll, "leader_reroll") ? 2 : 0;
        if (side < 0) r.side = 255;
        const J *dice = j_get(x, "dice");
        for (const J *d = dice ? dice->child : NULL; d; d = d->next) if (d->type == J_NUM) { r.val = (int16_t)d->num; rq_add(r); }
    }
}

static int view_int(const J *o, const char *key);
/* A card moved a whole Army in the engine (relocate_army); the referee's move says what went: the rest goes back. */
static void army_moved(const J *x) {
    State *s = &R.s;
    int from = region_of(j_str(x, "from")), to = region_of(j_str(x, "to"));
    /* a partial advance (applied whole, its split pending): the pieces that went are the split */
    if (adv_on && from == adv_src && to == adv_dst && side_of(j_str(x, "side")) >= 0) {
        int side = side_of(j_str(x, "side")), lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT;
        for (int n = lo; n < hi; n++) { sel_want[n][0] = sel_want[n][1] = 0; if (n < NFPNAT) sel_want[n][2] = 0; }
        if (side == SH) sel_want[SAURON][3] = 0;
        const J *pieces = j_get(x, "pieces");
        for (const J *p = pieces ? pieces->child : NULL; p; p = p->next) {
            int n = nation_of(j_str(p, "nation")); const char *u = j_str(p, "unit"); int k = view_int(p, "count");
            int uk = !u ? -1 : !strcmp(u, "regular") ? 0 : !strcmp(u, "elite") ? 1 : !strcmp(u, "leader") ? 2 : !strcmp(u, "nazgul") ? 3 : -1;
            if (n >= 0 && uk >= 0) sel_want[n][uk] = k;
        }
        Move m; memset(&m, 0, sizeof m); m.type = M_ARMY; m.a = (uint8_t)adv_src; m.b = (uint8_t)adv_dst;
        sel_fixup(&adv_before, m);
        adv_on = 0; selmode = 0;
        VLOG("  the advance as the referee made it: %s -> %s", REGION_KEY[from], REGION_KEY[to]);
        return;
    }
    /* an advance the referee made without asking (Corsairs of Umbar: the Army must move in): the engine's advance, all */
    if (!lost && leaves_ensure() && s->dkind == D_ADVANCE && s->b.active && s->b.src == from && s->b.dst == to && depth == 0) {
        for (int i = 0; i < nl; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"advance\",\"all\":true")) {
            VLOG("  an advance the referee made by itself: the engine's");
            apply_leaf(i); return;
        }
    }
    if (!s->reloc_on || from != s->reloc_from || to != s->reloc_to) return;
    s->reloc_on = 0;
    int reg[NNAT] = {0}, eli[NNAT] = {0}, ldr[NFPNAT] = {0}, naz = 0;
    const J *pieces = j_get(x, "pieces");
    for (const J *p = pieces ? pieces->child : NULL; p; p = p->next) {
        int n = nation_of(j_str(p, "nation")); const char *u = j_str(p, "unit"); int k = view_int(p, "count");
        if (n < 0 || !u) continue;
        if (!strcmp(u, "regular")) reg[n] += k; else if (!strcmp(u, "elite")) eli[n] += k;
        else if (!strcmp(u, "leader") && n < NFPNAT) ldr[n] += k; else if (!strcmp(u, "nazgul")) naz += k;
    }
    for (int n = 0; n < NNAT; n++) {
        /* (more than CWOTR moved: the rest comes along) */
        int back = s->reloc_reg[n] - reg[n]; if (back > 0 && s->reg[to][n] >= back) { s->reg[to][n] -= (uint8_t)back; s->reg[from][n] += (uint8_t)back; }
        else if (back < 0 && s->reg[from][n] >= -back) { s->reg[from][n] -= (uint8_t)-back; s->reg[to][n] += (uint8_t)-back; }
        back = s->reloc_eli[n] - eli[n]; if (back > 0 && s->eli[to][n] >= back) { s->eli[to][n] -= (uint8_t)back; s->eli[from][n] += (uint8_t)back; }
        else if (back < 0 && s->eli[from][n] >= -back) { s->eli[from][n] -= (uint8_t)-back; s->eli[to][n] += (uint8_t)-back; }
        if (n < NFPNAT) { back = s->reloc_ldr[n] - ldr[n]; if (back > 0 && s->ldr[to][n] >= back) { s->ldr[to][n] -= (uint8_t)back; s->ldr[from][n] += (uint8_t)back; } }
    }
    int back = s->reloc_naz - naz; if (back > 0 && s->naz[to] >= back) { s->naz[to] -= (uint8_t)back; s->naz[from] += (uint8_t)back; }
    VLOG("  a card's move as the referee made it: %s -> %s", REGION_KEY[from], REGION_KEY[to]);
}
/* The battle is over at the referee while the engine asks the besieger to extend the siege battle (the referee does not
 * offer to reduce the Army's last Elite with no Regular to replace it): the siege battle ends. */
static void battle_ended(void) {
    if (!leaves_ensure() || depth != 0 || R.s.dkind != D_SIEGE_EXTEND) return;
    for (int i = 0; i < nl; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"stop\"")) {
        VLOG("  the siege battle ends (the referee did not ask to extend it)");
        apply_leaf(i); return;
    }
}
/* ---- the pending events, at a decision or the game's end ---- */
static void start_game(void) {
    memset(known, 0, sizeof known);
    wotr_rnd_hook = hook;
    wotr_init(&R.s, 1);
    wotr_rnd_hook = NULL;
    for (int c = 0; c < NCARD; c++) if (CARD_SIDE(c) == me && R.s.card_where[c] == W_HAND) known[c] = 1;
    leaves_reset();
    started = 1;
}
/* events the engine is not ready for yet (an excess-card discard asked in the other order) */
static void retry_deferred(void) {
    for (int again = 1; again && ndefer; ) {
        again = 0;
        for (int d = 0; d < ndefer; d++) {
            char *mem; const J *m = j_parse(defer[d], &mem);
            const J *x = m ? j_get(m, "event") : NULL;
            int still = x ? follow_choice(x, nev) : 0;
            if (m) free(mem);
            if (!still) {
                free(defer[d]); memmove(&defer[d], &defer[d + 1], sizeof(char *) * (size_t)(ndefer - d - 1)); ndefer--; again = 1;
                /* a held Hunt answer separated another Companion: the referee's one destination for him too */
                if (hunt_sep_dest >= 0 && !lost && R.s.dkind == D_SEP_DEST && R.s.dplayer == FP) {
                    Move mv[MAXMOVES]; int n = wotr_moves(&R.s, mv), k = -1;
                    for (int i = 0; i < n; i++) if (mv[i].a == hunt_sep_dest) k = i;
                    if (k >= 0) { VLOG("  the separation's destination again: %s", REGION_KEY[hunt_sep_dest]); apply_followed(mv[k]); }
                }
                break;
            }
        }
    }
    if (!ndefer) { hunt_sep_hold = 0; hunt_sep_dest = -1; }
}
static void process_events(void) {
    for (int k = 0; k < nev; k++) {             /* every random outcome first: the engine takes them when it gets there */
        char *mem; const J *m = j_parse(ev[k], &mem);
        const J *x = m ? j_get(m, "event") : NULL;
        const char *kind = x ? j_str(x, "kind") : NULL;
        if (kind && !strncmp(kind, "random_", 7)) queue_random(x);
        if (m) free(mem);
    }
    if (!started) start_game();
    resolve_late_draws();
    for (int k = 0; k < nev && !lost; k++) {    /* the opponent's cards this batch shows were in its hand: no stand-ins for them */
        char *mem; const J *m = j_parse(ev[k], &mem);
        const J *x = m ? j_get(m, "event") : NULL;
        const char *kind = x ? j_str(x, "kind") : NULL;
        const char *keys[3] = { NULL, NULL, NULL };
        if (kind && !strcmp(kind, "combat_cards_revealed")) { keys[0] = j_str(x, "attacker_card"); keys[1] = j_str(x, "defender_card"); }
        else if (kind && !strcmp(kind, "choice")) { const J *o = j_get(x, "option"); const char *ok = o ? j_str(o, "kind") : NULL; if (ok && (!strcmp(ok, "combat_card") || !strcmp(ok, "use_die"))) keys[0] = j_str(o, "card"); }
        for (int i = 0; i < 3; i++) {
            int c = card_of(keys[i]);
            if (c >= 0 && CARD_SIDE(c) != me && R.s.card_where[c] == W_DECK && !known[c]) ensure_card(&R.s, c, W_HAND);
        }
        /* the opponent's hidden Combat card, shown only after ours was chosen (a later batch): the copy played a
           stand-in for it, which the real card replaces before the round is fought */
        if (kind && !strcmp(kind, "combat_cards_revealed") && R.s.b.active) {
            for (int i = 0; i < 2; i++) {
                int c = card_of(keys[i]), sd = c >= 0 ? CARD_SIDE(c) : -1, was = sd >= 0 ? R.s.b.card[sd] : 255;
                if (c < 0 || sd == me || was == 255 || was == c || CARD_SIDE(was) != sd) continue;
                uint8_t w = R.s.card_where[c]; R.s.card_where[c] = R.s.card_where[was]; R.s.card_where[was] = w;
                R.s.b.card[sd] = (uint8_t)c; known[c] = 1; known[was] = 0;
                VLOG("  the opponent's Combat card is %s, not the stand-in %s", CARD[c].name, CARD[was].name);
            }
        }
        if (m) free(mem);
    }
    for (int k = 0; k < nev; k++) {
        char *mem; const J *m = j_parse(ev[k], &mem);
        const J *x = m ? j_get(m, "event") : NULL;
        const char *kind = x ? j_str(x, "kind") : NULL;
        if (kind && voice_pending && strcmp(kind, "figure_recruited")) {      /* the Voice's recruits are over */
            int any = 0; for (int r = 0; r < NREG; r++) any |= voice_seen[r];
            if (any) voice_settle();
        }
        if (kind && !strcmp(kind, "turn_started")) { ref_turn = view_int(x, "turn"); ref_phase = -1; }
        if (kind && !strcmp(kind, "phase_started")) ref_phase = phase_index(j_str(x, "phase"));
        /* "Then, you may move or attack with an Army containing the Witch-king / a Nazgul" with no such Army: the
           referee does not ask (one option); CWOTR asks "skip" / "move" still: the card ends there */
        if (kind && !lost && started && (!strcmp(kind, "phase_started") || !strcmp(kind, "turn_started")) &&
            R.s.dkind == D_CARD_OPTION && R.s.dplayer == SH && R.s.qtag == Q_BCC_ARMY) {
            Move mv[MAXMOVES]; int n = wotr_moves(&R.s, mv), k0 = -1;
            for (int i = 0; i < n; i++) if (mv[i].type == M_CHOICE && mv[i].a == 0) k0 = i;
            if (k0 >= 0) { VLOG("  the card's Army move the referee did not ask (no such Army): none"); leaves_reset(); apply_followed(mv[k0]); leaves_reset(); }
        }
        if (kind && !strcmp(kind, "action_dice_rolled")) {
            int sd = side_of(j_str(x, "side")); const J *fl = j_get(x, "faces");
            if (sd >= 0) { ref_nd[sd] = 0; for (const J *f = fl ? fl->child : NULL; f && ref_nd[sd] < 16; f = f->next) ref_face[sd][ref_nd[sd]++] = (int8_t)(f->type == J_STR ? face_of(f->str) : -1); }
        }
        if (kind && !strcmp(kind, "choice")) {
            if (follow_choice(x, k + 1)) { if (ndefer < MAXDEF) defer[ndefer++] = strdup(ev[k]); }
            else retry_deferred();
        }
        else if (kind && !lost && !strcmp(kind, "army_moved")) army_moved(x);
        else if (kind && !lost && !strcmp(kind, "battle_ended")) battle_ended();
        else if (kind && voice_pending && !strcmp(kind, "figure_recruited") && nation_of(j_str(x, "nation")) == ISENGARD) {
            int r = region_of(j_str(x, "region")); if (r >= 0) voice_seen[r]++;
        }
        else if (kind && !lost && (!strcmp(kind, "card_played") || !strcmp(kind, "card_discarded"))) {
            /* a card of the opponent shown: it is the real one from now on (the follower places it with the choice) */
            int c = card_of(j_str(x, "card")), side = side_of(j_str(x, "side"));
            if (c >= 0 && side >= 0 && side != me) known[c] = 1;
        }
        if (trace_r >= 0 && kind) {          /* HAMBOT_TRACE=region: the copy's units there after each event (debugging) */
            char tb[160] = ""; size_t tn = 0;
            for (int n = 0; n < NNAT; n++) if (R.s.reg[trace_r][n] || R.s.eli[trace_r][n]) tn += (size_t)snprintf(tb + tn, sizeof tb - tn, " %s %dR%dE", NATION_KEY[n], R.s.reg[trace_r][n], R.s.eli[trace_r][n]);
            const J *o = j_get(x, "option");
            int dsh = 0, dfp = 0; for (int f = 0; f < NFACE; f++) { dsh += R.s.dice[SH][f]; dfp += R.s.dice[FP][f]; }
            LOG("TRACE %-22s %-16s %-12s | %s:%s | dice FP %d SH %d | copy at %s %d depth %d", kind, j_str(x, "decision") ? j_str(x, "decision") : "", o && j_str(o, "kind") ? j_str(o, "kind") : "", REGION_KEY[trace_r], tb, dfp, dsh, SIDE_KEY[R.s.dplayer], R.s.dkind, depth);
        }
        if (m) free(mem);
        free(ev[k]);
    }
    nev = 0;
    voice_settle();
    /* drop what has been used; not while a move waits for the Leader re-roll's dice: it is played again from its
       snapshot, with the queue as it was then */
    if (!reroll_wait) {
        int w = 0;
        for (int i = 0; i < nrq; i++) if (!rq[i].used) rq[w++] = rq[i];
        nrq = w;
        snap_ok = 0;
    }
}

/* ================= the view: compare, rebuild ================= */
static int hb_view_count(const J *units, const char *nation, const char *unit) {
    for (const J *u = units ? units->child : NULL; u; u = u->next) {
        const char *n = j_str(u, "nation"), *t = j_str(u, "unit"); const J *c = j_get(u, "n");
        if (n && t && c && !strcmp(n, nation) && !strcmp(t, unit)) return (int)c->num;
    }
    return 0;
}
static int view_bool(const J *o, const char *key) { const J *x = j_get(o, key); return x && x->type == J_BOOL && x->num; }
static int view_int(const J *o, const char *key) { const J *x = j_get(o, key); return x && x->type == J_NUM ? (int)x->num : 0; }
static int where_of(const J *chars, const char *key) {      /* a character's place: region, LOC_FSP, LOC_DEAD, LOC_NONE */
    const J *c = j_get(chars, key);
    const char *w = c ? j_str(c, "where") : NULL;
    if (!w) return -1;
    if (!strcmp(w, "region")) return region_of(j_str(c, "region"));
    if (!strcmp(w, "fellowship")) return LOC_FSP;
    if (!strcmp(w, "eliminated")) return LOC_DEAD;
    return LOC_NONE;
}
/* The board from the view into t (a copy of the engine's state whose hidden parts stay as they are). 0: understood. */
static int board_from_view(State *t, const J *v, int pov, char *err, int en) {
    const J *regions = j_get(v, "regions");
    for (int r = 0; r < NREG; r++) {
        const J *x = j_get(regions, REGION_KEY[r]);
        if (!x) {           /* the referee leaves out a region with nothing in it and no settlement */
            memset(t->reg[r], 0, NNAT); memset(t->eli[r], 0, NNAT); memset(t->ldr[r], 0, NFPNAT);
            t->naz[r] = 0; t->siege[r] = 0;
            if (is_settlement(r)) t->ctrl[r] = (uint8_t)orig_side(r);
            continue;
        }
        const J *units = j_get(x, "units");
        for (int n = 0; n < NNAT; n++) {
            t->reg[r][n] = (uint8_t)hb_view_count(units, NATION_KEY[n], "regular");
            t->eli[r][n] = (uint8_t)hb_view_count(units, NATION_KEY[n], "elite");
            if (n < NFPNAT) t->ldr[r][n] = (uint8_t)hb_view_count(units, NATION_KEY[n], "leader");
        }
        t->naz[r] = (uint8_t)hb_view_count(units, "SAURON", "nazgul");
        const char *ctl = j_str(x, "control");
        if (is_settlement(r)) t->ctrl[r] = ctl ? (uint8_t)(side_of(ctl) < 0 ? 2 : side_of(ctl)) : (uint8_t)orig_side(r);
        { const J *sg = j_get(x, "siege"); t->siege[r] = (uint8_t)(sg && ((sg->type == J_BOOL && sg->num) || sg->type == J_STR)); }   /* the referee names the besieged side */
    }
    const J *pol = j_get(v, "politics"), *rein = j_get(v, "reinforcements"), *cas = j_get(v, "casualties");
    for (int n = 0; n < NNAT; n++) {
        const J *p = j_get(pol, NATION_KEY[n]), *q = j_get(rein, NATION_KEY[n]);
        if (p) { t->pol[n] = (uint8_t)view_int(p, "step"); t->active[n] = (uint8_t)view_bool(p, "active"); }
        if (q) {
            t->pool_reg[n] = (uint8_t)view_int(q, "regular"); t->pool_eli[n] = (uint8_t)view_int(q, "elite");
            if (n < NFPNAT) t->pool_ldr[n] = (uint8_t)view_int(q, "leader");
            if (n == SAURON) t->pool_naz = (uint8_t)view_int(q, "nazgul");
        }
        if (n < NFPNAT) { const J *c = j_get(cas, NATION_KEY[n]); if (c) t->cas_reg[n] = (uint8_t)view_int(c, "regular"); }
    }
    const J *f = j_get(v, "fellowship"), *chars = j_get(v, "characters");
    if (f) {
        int r = region_of(j_str(f, "region"));
        if (r >= 0) t->fsp_loc = (uint8_t)r;
        t->progress = (uint8_t)view_int(f, "progress"); t->corruption = (uint8_t)view_int(f, "corruption");
        t->revealed = (uint8_t)!view_bool(f, "hidden");
        const J *mo = j_get(f, "mordor"); t->mordor = (int8_t)(mo && mo->type == J_NUM ? (int)mo->num : -1);
        const char *g = j_str(f, "guide");
        int gc = g ? char_of(g) : -1;
        if (gc == CH_GANDALF_WHITE) gc = C_GANDALF; else if (gc == CH_ARAGORN) gc = C_STRIDER; else if (gc == CH_GOLLUM) gc = GOLLUM;
        if (gc >= 0 && gc <= GOLLUM) t->guide = (uint8_t)gc;
    }
    t->gandalf_white = 0; t->aragorn = 0;
    for (int c = 0; c < NCOMP; c++) {
        int w = where_of(chars, CHAR_KEY[c]);
        if (c == C_GANDALF) { int w2 = where_of(chars, "gandalf-the-white"); if (w2 >= 0 && w2 != LOC_NONE) { w = w2; t->gandalf_white = 1; } }
        if (c == C_STRIDER) { int w2 = where_of(chars, "aragorn"); if (w2 >= 0 && w2 != LOC_NONE) { w = w2; t->aragorn = 1; } }
        if (w >= 0) t->comp_loc[c] = (uint8_t)w;
    }
    for (int m = 0; m < NMINION; m++) { int w = where_of(chars, CHAR_KEY[CH_MINION0 + m]); if (w >= 0) t->minion_loc[m] = (uint8_t)w; }
    const J *dice = j_get(v, "dice");
    for (int side = 0; side < 2; side++) {
        memset(t->dice[side], 0, sizeof t->dice[side]);
        const J *list = j_get(dice, SIDE_KEY[side]);
        for (const J *d = list ? list->child : NULL; d; d = d->next) {
            const char *status = j_str(d, "status"), *face = j_str(d, "face");
            if (status && !strcmp(status, "available") && face) { int fc = face_of(face); if (fc >= 0) t->dice[side][fc]++; }
        }
    }
    const J *hunt = j_get(v, "hunt");
    if (hunt) {
        const J *box = j_get(hunt, "box");
        t->hunt_fp = (uint8_t)view_int(box, "FREE_PEOPLES"); t->hunt_sh = (uint8_t)view_int(box, "SHADOW");
        t->hunt_fp_prev = (uint8_t)view_bool(hunt, "shadow_must_hunt");
        memset(t->tile_pool, 0, sizeof t->tile_pool); memset(t->tile_out, 0, sizeof t->tile_out);
        static const char *const place[4] = { "pool", "reserve", "ready", "drawn" };
        for (int t2 = NSTD_TILE; t2 < NTILE; t2++) t->special_st[t2] = 0;
        for (int p = 0; p < 4; p++) {
            const J *list = j_get(hunt, place[p]);
            for (const J *x = list ? list->child : NULL; x; x = x->next) {
                int k = x->type == J_STR ? tile_of(x->str) : -1;
                if (k < 0) continue;
                if (p == 0) { t->tile_pool[k]++; if (k >= NSTD_TILE) t->special_st[k] = 2; }
                else if (p == 2 && k >= NSTD_TILE) t->special_st[k] = 1;
                else if (p == 3) { if (k < NSTD_TILE) t->tile_out[k]++; else t->special_st[k] = 2; }
            }
        }
    }
    const J *rings = j_get(v, "elven_rings"), *used = j_get(v, "elven_ring_used");
    for (int side = 0; side < 2; side++) { t->rings[side] = (uint8_t)view_int(rings, SIDE_KEY[side]); t->ring_used[side] = (uint8_t)view_bool(used, SIDE_KEY[side]); }
    /* the cards: our hand, both discards and tables; the opponent's hand as stand-ins */
    const J *cards = j_get(v, "cards");
    if (cards) {
        uint8_t was[NCARD]; memcpy(was, t->card_where, sizeof was);
        for (int c = 0; c < NCARD; c++) t->card_where[c] = W_DECK;
        uint8_t sure[NCARD]; memset(sure, 0, sizeof sure);
        const J *hand = j_get(cards, "hand");
        for (const J *x = hand ? hand->child : NULL; x; x = x->next) { int c = x->type == J_STR ? card_of(x->str) : -1; if (c >= 0) { t->card_where[c] = W_HAND; sure[c] = 1; } }
        const J *down_own = j_get(cards, "discarded_face_down");      /* our own cards discarded face down */
        for (const J *x = down_own ? down_own->child : NULL; x; x = x->next) { int c = x->type == J_STR ? card_of(x->str) : -1; if (c >= 0) { t->card_where[c] = W_DISCARD; sure[c] = 1; } }
        static const char *const piles[2] = { "discards", "on_table" };
        for (int p = 0; p < 2; p++) {
            const J *o = j_get(cards, piles[p]);
            for (int side = 0; side < 2; side++) {
                const J *list = j_get(o, SIDE_KEY[side]);
                for (const J *x = list ? list->child : NULL; x; x = x->next) { int c = x->type == J_STR ? card_of(x->str) : -1; if (c >= 0) { t->card_where[c] = p ? W_TABLE : W_DISCARD; sure[c] = 1; } }
            }
        }
        const J *by = j_get(cards, "opponent_hand_by_deck");
        int opp = 1 - pov;
        for (int d = 0; d < 2; d++) {
            int want = view_int(by, DECK_KEY[d]), deck = opp * 2 + d;
            /* the cards the copy already held for the opponent first (they may be real), then any */
            for (int pass = 0; pass < 2 && want > 0; pass++)
                for (int c = deck * 24; c < deck * 24 + 24 && want > 0; c++)
                    if (!sure[c] && t->card_where[c] == W_DECK && (pass || was[c] == W_HAND)) { t->card_where[c] = W_HAND; want--; }
        }
        const J *decks = j_get(j_get(cards, "decks"), SIDE_KEY[opp]);
        if (decks) {            /* per deck: as many left in it as the view counts, the rest discarded face down */
            for (int d = 0; d < 2; d++) {
                int deck = opp * 2 + d, left = view_int(decks, DECK_KEY[d]), have = 0;
                for (int c = deck * 24; c < deck * 24 + 24; c++) have += !sure[c] && t->card_where[c] == W_DECK;
                for (int pass = 0; pass < 2 && have > left; pass++)
                    for (int c = deck * 24; c < deck * 24 + 24 && have > left; c++)
                        if (!sure[c] && t->card_where[c] == W_DECK && (pass || was[c] == W_DISCARD)) { t->card_where[c] = W_DISCARD; have--; }
            }
        } else {
            int down = view_int(cards, "opponent_discarded_face_down");
            for (int c = opp * 48; c < opp * 48 + 48 && down > 0; c++) if (!sure[c] && t->card_where[c] == W_DECK) { t->card_where[c] = W_DISCARD; down--; }
        }
        t->hand_n[0] = t->hand_n[1] = 0;
        for (int c = 0; c < NCARD; c++) if (t->card_where[c] == W_HAND) t->hand_n[CARD_SIDE(c)]++;
        for (int c = 0; c < NCARD; c++) known[c] = sure[c];
    }
    const J *turn = j_get(v, "turn");
    if (turn && turn->type == J_NUM) t->turn = (uint16_t)turn->num;
    return 0;
}
/* The first difference between the copy and the view (NULL: none that matters). */
static const char *differs(const J *v, int pov) {
    static char why[200];
    State t = R.s; char err[120];
    uint8_t keep[NCARD]; memcpy(keep, known, sizeof keep);
    if (board_from_view(&t, v, pov, err, sizeof err)) { memcpy(known, keep, sizeof keep); snprintf(why, sizeof why, "%s", err); return why; }
    memcpy(known, keep, sizeof keep);
    const State *s = &R.s;
    for (int r = 0; r < NREG; r++) {
        if (memcmp(s->reg[r], t.reg[r], NNAT) || memcmp(s->eli[r], t.eli[r], NNAT) || memcmp(s->ldr[r], t.ldr[r], NFPNAT) || s->naz[r] != t.naz[r]) {
            int n = 0; while (n < NNAT && s->reg[r][n] == t.reg[r][n] && s->eli[r][n] == t.eli[r][n] && (n >= NFPNAT || s->ldr[r][n] == t.ldr[r][n])) n++;
            if (n < NNAT) snprintf(why, sizeof why, "units in %s: %s copy %dR %dE %dL view %dR %dE %dL", REGION_KEY[r], NATION_KEY[n], s->reg[r][n], s->eli[r][n], n < NFPNAT ? s->ldr[r][n] : 0, t.reg[r][n], t.eli[r][n], n < NFPNAT ? t.ldr[r][n] : 0);
            else snprintf(why, sizeof why, "units in %s: Nazgul copy %d view %d", REGION_KEY[r], s->naz[r], t.naz[r]);
            return why; }
        if (is_settlement(r) && s->ctrl[r] != t.ctrl[r]) { snprintf(why, sizeof why, "control of %s", REGION_KEY[r]); return why; }
        if (s->siege[r] != t.siege[r]) { snprintf(why, sizeof why, "siege in %s", REGION_KEY[r]); return why; }
    }
    for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] != t.comp_loc[c]) { snprintf(why, sizeof why, "characters: %s copy %d view %d", CHAR_KEY[c], s->comp_loc[c], t.comp_loc[c]); return why; }
    for (int m = 0; m < NMINION; m++) if (s->minion_loc[m] != t.minion_loc[m]) { snprintf(why, sizeof why, "characters: %s copy %d view %d", minion_key(m), s->minion_loc[m], t.minion_loc[m]); return why; }
    if (s->gandalf_white != t.gandalf_white || s->aragorn != t.aragorn) return "characters: Gandalf the White / Aragorn";
    if (s->fsp_loc != t.fsp_loc || s->progress != t.progress || s->corruption != t.corruption || s->revealed != t.revealed || s->mordor != t.mordor || s->guide != t.guide) {
        snprintf(why, sizeof why, "the Fellowship: copy %s p%d c%d r%d m%d g%d, view %s p%d c%d r%d m%d g%d", REGION_KEY[s->fsp_loc], s->progress, s->corruption, s->revealed, s->mordor, s->guide,
                 REGION_KEY[t.fsp_loc], t.progress, t.corruption, t.revealed, t.mordor, t.guide);
        return why;
    }
    if (memcmp(s->pol, t.pol, NNAT) || memcmp(s->active, t.active, NNAT)) return "politics";
    if (memcmp(s->pool_reg, t.pool_reg, NNAT) || memcmp(s->pool_eli, t.pool_eli, NNAT) || memcmp(s->pool_ldr, t.pool_ldr, NFPNAT) || s->pool_naz != t.pool_naz) return "reinforcements";
    for (int sd = 0; sd < 2; sd++) for (int f = 0; f < NFACE; f++) if (s->dice[sd][f] != t.dice[sd][f]) { snprintf(why, sizeof why, "dice: %s %s copy %d view %d (dkind %d)", SIDE_KEY[sd], face_key(sd, f), s->dice[sd][f], t.dice[sd][f], s->dkind); return why; }
    if (s->hunt_sh != t.hunt_sh || (s->hunt_fp != t.hunt_fp && s->sp <= 2)) {   /* in a Hunt CWOTR adds the die when it ends, the referee at once */ snprintf(why, sizeof why, "hunt box: copy %d/%d view %d/%d", s->hunt_fp, s->hunt_sh, t.hunt_fp, t.hunt_sh); return why; }
    if (memcmp(s->tile_pool, t.tile_pool, NTILE)) return "the Hunt pool";
    if (memcmp(s->rings, t.rings, 2)) return "Elven Rings";
    if (s->turn != t.turn) return "the turn";
    for (int c = 0; c < NCARD; c++) {
        int ours = CARD_SIDE(c) == pov && !foreign;
        if ((ours || s->card_where[c] == W_TABLE || t.card_where[c] == W_TABLE) && s->card_where[c] != t.card_where[c]) { static const char *const W[4] = { "deck", "hand", "table", "discard" }; snprintf(why, sizeof why, "card %s (%s) copy %s view %s", CARD_KEY[c], CARD[c].name, W[s->card_where[c] & 3], W[t.card_where[c] & 3]); return why; }
    }
    if (s->hand_n[0] != t.hand_n[0] || s->hand_n[1] != t.hand_n[1]) { snprintf(why, sizeof why, "hand sizes: copy %d/%d view %d/%d", s->hand_n[0], s->hand_n[1], t.hand_n[0], t.hand_n[1]); return why; }
    for (int deck = 0; deck < 4; deck++) {
        int a = 0, b = 0;
        for (int c = deck * 24; c < deck * 24 + 24; c++) { a += s->card_where[c] == W_DECK; b += t.card_where[c] == W_DECK; }
        if (a != b && j_get(j_get(v, "cards"), "decks")) { snprintf(why, sizeof why, "deck %s %s: copy %d view %d", SIDE_KEY[deck / 2], DECK_KEY[deck % 2], a, b); return why; }
    }
    return NULL;
}
/* The cards from the view, between moves: our hand exactly, the opponent's hand by deck (its stand-ins exchanged with
 * unseen cards of the deck). The view is exact about both; the copy's bookkeeping of hidden cards is not. */
static void cards_from_view(const J *v, int pov) {
    State *s = &R.s;
    const J *cards = j_get(v, "cards");
    if (!cards || s->b.active) return;
    uint8_t inview[NCARD]; memset(inview, 0, sizeof inview);
    const J *hand = j_get(cards, "hand");
    for (const J *x = hand ? hand->child : NULL; x; x = x->next) { int c = x->type == J_STR ? card_of(x->str) : -1; if (c >= 0) inview[c] = 1; }
    int fixed = 0;
    for (int c = pov * 48; c < pov * 48 + 48; c++) {
        if (inview[c] && s->card_where[c] != W_HAND) { if (s->card_where[c] == W_DECK) { s->card_where[c] = W_HAND; fixed++; } }
        else if (!inview[c] && s->card_where[c] == W_HAND) { s->card_where[c] = W_DECK; fixed++; }
    }
    const J *by = j_get(cards, "opponent_hand_by_deck");
    int opp = 1 - pov;
    const J *decks = j_get(j_get(cards, "decks"), SIDE_KEY[opp]);
    for (int d = 0; d < 2 && by; d++) {
        /* the opponent's cards whose place the copy only guesses (stand-ins): as many in its hand and in the deck as
         * the view counts, the rest discarded face down */
        int deck = opp * 2 + d, want[4] = { 0 }, have[4] = { 0 }, nu = 0;
        want[W_HAND] = view_int(by, DECK_KEY[d]);
        for (int c = deck * 24; c < deck * 24 + 24; c++) {
            if (known[c] || s->card_where[c] == W_TABLE) { if (s->card_where[c] == W_HAND) want[W_HAND]--; continue; }
            have[s->card_where[c] & 3]++; nu++;
        }
        if (want[W_HAND] < 0) want[W_HAND] = 0;
        if (decks) { want[W_DECK] = view_int(decks, DECK_KEY[d]); want[W_DISCARD] = nu - want[W_HAND] - want[W_DECK]; }
        else { want[W_DECK] = have[W_DECK] + have[W_HAND] - want[W_HAND]; want[W_DISCARD] = have[W_DISCARD]; }
        if (want[W_DECK] < 0 || want[W_DISCARD] < 0) continue;      /* the counts do not add up: left alone */
        for (int c = deck * 24; c < deck * 24 + 24; c++) {
            int w = s->card_where[c] & 3;
            if (known[c] || s->card_where[c] == W_TABLE || have[w] <= want[w]) continue;
            int to = have[W_HAND] < want[W_HAND] ? W_HAND : have[W_DECK] < want[W_DECK] ? W_DECK : W_DISCARD;
            if (to == w) continue;
            s->card_where[c] = (uint8_t)to; have[w]--; have[to]++; fixed++;
        }
    }
    if (!fixed) return;
    s->hand_n[0] = s->hand_n[1] = 0;
    for (int c = 0; c < NCARD; c++) if (s->card_where[c] == W_HAND) s->hand_n[CARD_SIDE(c)]++;
    if (depth == 0 && s->dkind == D_DISCARD) {        /* the question lists the hand: asked anew */
        s->nopt = 0;
        for (int c = s->dplayer * 48; c < s->dplayer * 48 + 48; c++) if (s->card_where[c] == W_HAND) s->opt[s->nopt++] = (uint8_t)c;
        leaves_reset();
    }
    VLOG("  %d card(s) set from the view", fixed);
}
/* Rebuilds the copy from the view at an Action decision of `pov`. 0: done. */
static int resync(const J *v, int pov) {
    char err[200];
    State old = R.s;
    State *s = &R.s;
    wotr_blank(s);
    /* what the view does not show is kept from the copy */
    if (started) {
        s->fsp_acted = old.fsp_acted; s->mouth_used = old.mouth_used; s->minion_seen = old.minion_seen;
        memcpy(s->special_st, old.special_st, sizeof s->special_st);
        memcpy(s->card_where, old.card_where, sizeof s->card_where);
        s->stat_fsp_moves = old.stat_fsp_moves; s->stat_battles = old.stat_battles; s->stat_tiles = old.stat_tiles;
    }
    if (board_from_view(s, v, pov, err, sizeof err)) { LOG("resync failed: %s", err); return 1; }
    s->lock[0] = s->lock[1] = 0; s->comp_moved = 0;
    s->dkind = D_ACTION; s->dplayer = (uint8_t)pov; s->st[s->sp - 1].v[0] = (uint8_t)pov;
    s->winner = 0;
    if (wotr_check(s, err, sizeof err)) { LOG("resync: the position fails the engine's check: %s", err); return 1; }
    leaves_reset(); rq_clear(); nlate = 0; adv_on = 0; stash_on = 0; memset(stash_ldr, 0, sizeof stash_ldr); stash_naz = 0;
    reroll_wait = reroll_missing = 0; snap_ok = snap_fix = 0;       /* a move waiting for re-roll dice: replaced by the view */
    while (ndefer) free(defer[--ndefer]);
    started = 1; lost = 0; st.resyncs++; last_note[0] = 0; diverged = 0;
    return 0;
}

/* ================= our decisions ================= */
/* Time (HAMBOT_TIME, off by default: the release plays at once). With it, each Action decision gives the engine's
 * rollout chooser (roll_choose: the one-step scores shortlist roll_k moves, rollouts with shared dice decide) a share
 * of the clock: under a per-decision clock HAMBOT_TIME of it less 400 ms (1 means 0.8, the most that is safe: answers
 * overshoot their budget by about 20 ms), under a game clock the time left spread
 * over the Actions expected to remain, plus most of the increment. Other questions are answered at once. */
static double time_share;           /* 0: off */
static int clock_game;              /* new_game's clock: 1 a game clock, 0 per decision */
static double clock_left, clock_incr; static int clock_known;     /* the decision's clock (decide's "clock") */
static double action_budget_ms(void) {
    if (time_share <= 0 || !clock_known) return 0;
    double margin = 400, b;
    if (!clock_game) b = clock_left * time_share - margin;
    else {
        int turns_left = 16 - R.s.turn; if (turns_left < 3) turns_left = 3;
        double actions_left = turns_left * 5.0;                   /* about five Actions a turn for a side */
        b = (clock_left - 4000) / actions_left + 0.8 * clock_incr;
        if (b > clock_left * 0.2) b = clock_left * 0.2;
    }
    return b > 20 ? b : 0;
}
static Move choose_move(void) {
    State c = R.s; float bv; int n;
    wotr_prune = 1;
    wotr_determinize(&c, me, own_rng += 0x1234567);   /* the player sees only what its side may know */
    double budget = R.s.dkind == D_ACTION ? action_budget_ms() : 0;
    ag.roll_ms = (float)budget; ag.roll_n = 0; ag.roll_tau = 0.15f; ag.roll_k = 12; ag.roll_turns = 0;
    Move m = agent_choose(&ag, &c, &bv, &n);
    ag.roll_ms = 0;
    char buf[200]; wotr_move_str(&R.s, m, buf, sizeof buf);
    VLOG("CWOTR: %s (chance the Free Peoples win %.0f%%%s)", buf, 100 * nn_prob(bv), budget > 0 ? ", rollouts" : "");
    return m;
}
/* the player moves the Nazgul as one plan (M_NAZ_PLAN: where they all end up); the referee asks one Nazgul or
 * Minion at a time: each step moves one toward the plan, then "end". */
void apply_naz_plan(State *s, Move m);
static int naz_plan_on; static uint8_t naz_target[NREG], min_target[NMINION];
static int naz_leaf_toward(void) {
    const State *s = &R.s;
    int end = -1;
    for (int i = 0; i < nl; i++) {
        if (!alive[i]) continue;
        Move m = L[i].mv[0];
        if (m.type != M_NAZ_MOVE) { if (end < 0) end = i; continue; }
        if (m.d == 0 && m.c == 1 && s->naz[m.a] > naz_target[m.a] && s->naz[m.b] < naz_target[m.b]) return i;
        if (m.d >= 1) { int mi = m.d == 1 ? M_WK : M_MOUTH; if (s->minion_loc[mi] != min_target[mi] && m.b == min_target[mi]) return i; }
    }
    return end >= 0 ? end : first_alive();
}
static int pick_target(void) {
    if (R.s.dkind == D_NAZ_MOVE && naz_plan_on) return naz_leaf_toward();
    naz_plan_on = 0;
    Move m = choose_move();
    if (m.type == M_NAZ_PLAN) {
        State t = R.s; apply_naz_plan(&t, m);
        memcpy(naz_target, t.naz, sizeof naz_target); memcpy(min_target, t.minion_loc, sizeof min_target);
        naz_plan_on = 1;
        return naz_leaf_toward();
    }
    for (int i = 0; i < nl; i++) if (alive[i] && !memcmp(&L[i].mv[0], &m, sizeof m)) return i;
    /* an attack is written with its leader flags cleared (bridge.c) */
    for (int i = 0; i < nl; i++) if (alive[i] && L[i].mv[0].type == m.type && L[i].mv[0].a == m.a && L[i].mv[0].b == m.b) return i;
    if (checking) { char buf[200]; wotr_move_str(&R.s, m, buf, sizeof buf); LOG("UNMAPPED move type %d at decision %d: %s", m.type, R.s.dkind, buf); }
    return first_alive();
}
/* Our excess card when the referee asks us before the opponent (CWOTR asks the Free Peoples first): the opponent's
 * discard stands in on a copy, CWOTR chooses ours there. */
static int discard_ahead(const J *opts) {
    State t = R.s; Move mv[MAXMOVES];
    for (int g = 0; g < 4 && !t.winner && t.dkind == D_DISCARD && t.dplayer != me; g++) { int n = wotr_moves(&t, mv); if (!n) return -1; wotr_apply(&t, mv[0]); }
    if (t.winner || t.dkind != D_DISCARD || t.dplayer != me) return -1;
    /* our hand is the options: a card the referee no longer lists was discarded already (our earlier discard, waiting
       in the copy for the opponent's) */
    {
        uint8_t listed[NCARD]; memset(listed, 0, sizeof listed);
        for (const J *o = opts->child; o; o = o->next) { int c = card_of(j_str(o, "card")); if (c >= 0) listed[c] = 1; }
        for (int c = me * 48; c < me * 48 + 48; c++) if (t.card_where[c] == W_HAND && !listed[c]) { t.card_where[c] = W_DISCARD; t.hand_n[me]--; }
        t.nopt = 0;
        for (int c = me * 48; c < me * 48 + 48; c++) if (t.card_where[c] == W_HAND) t.opt[t.nopt++] = (uint8_t)c;
        if (!t.nopt) return -1;
    }
    State c = t; float bv; int n;
    wotr_determinize(&c, me, own_rng += 0x1234567);
    Move m = agent_choose(&ag, &c, &bv, &n);
    if (m.a >= NCARD) return -1;
    for (const J *o = opts->child; o; o = o->next) { const char *card = j_str(o, "card"); if (card && !strcmp(card, CARD_KEY[m.a])) { VLOG("our discard planned ahead: %s", CARD_KEY[m.a]); return (int)j_get(o, "o")->num; } }
    return -1;
}
/* Our card question that the referee asks before the opponent's separation destination (CWOTR asks it after): the
 * separation stands in on a copy, CWOTR answers ours there. */
static int answer_ahead(const J *opts) {
    if (!leaves_ensure() || R.s.dkind != D_SEP_DEST || R.s.dplayer == me) return -1;
    State t = R.s; Move mv[MAXMOVES];
    wotr_prune = 0; int n = wotr_moves(&t, mv); if (n) wotr_apply(&t, mv[0]); wotr_prune = 1;
    if (!n || t.winner || t.dplayer != me || t.dkind != D_CARD_OPTION || !t.qtag) return -1;
    State c = t; float bv; int k;
    wotr_determinize(&c, me, own_rng += 0x1234567);
    Move m = agent_choose(&ag, &c, &bv, &k);
    Leaf tmp; memset(&tmp, 0, sizeof tmp);
    qtag_step(&tmp, &t, m.a);
    int best = -1, bo = -1;
    for (const J *o = opts->child; o && tmp.nstep; o = o->next) { int sc = step_match(&tmp.st[0], o); if (sc > best) { best = sc; bo = (int)j_get(o, "o")->num; } }
    if (bo >= 0) VLOG("our card answer planned ahead of the opponent's separation: %s", tmp.st[0].text);
    return bo;
}
/* The option to answer, or -1. */
/* A card's "How many Regulars in <region>?" (its options counts): asked by the referee only, CWOTR fills the regions. */
static int recruit_count_question(const J *decision) {
    const J *ctx = j_get(decision, "context");
    const char *q = ctx ? j_str(ctx, "question") : NULL;
    return q && !strncmp(q, "How many Regulars in ", 21);
}
static int answer(const J *decision, const J *view) {
    const J *opts = j_get(decision, "options");
    int nopt = j_len(opts);
    const char *kind = j_str(decision, "kind");
    st.decisions++;
    if (selmode && !(kind && (!strcmp(kind, "ARMY_SELECTION") || !strcmp(kind, "SEPARATE_COMPANIONS")))) sel_finalize();
    /* Mighty Attack's Companion: the one CWOTR's engine takes (the lowest Leadership), before the re-roll's dice */
    if (kind && !strcmp(kind, "CARD_CHOICE") && opts && opts->child && ma_question_card(j_str(opts->child, "card"))) {
        int best = -1, bl = 99;
        for (const J *o = opts->child; o; o = o->next) {
            int c = ma_comp_of(j_str(o, "answer"));
            int l = c >= 0 ? comp_leadership(&R.s, c) : 99;
            if (c >= 0 && l < bl) { bl = l; best = (int)j_get(o, "o")->num; }
        }
        if (best >= 0) { VLOG("  Mighty Attack: the Companion of the lowest Leadership"); return best; }
    }
    if (!(kind && !strcmp(kind, "LEADER_REROLL"))) settle_reroll();
    if (view && view_int(view, "turn") > 0) { ref_turn = view_int(view, "turn"); ref_phase = phase_index(j_str(view, "phase")); }
    if (nopt <= 1) { st.asked_single++; return 0; }
    if (!started) start_game();
    /* "How many Regulars in <region>?" (Many Kings to the Service of Mordor, Pits of Mordor: asked when the Regulars
       run short, so that the most are recruited): CWOTR recruits two in each region while it can, without asking */
    if (kind && !strcmp(kind, "CARD_CHOICE") && recruit_count_question(decision)) {
        int best = -1, bc = -1;
        for (const J *o = opts->child; o; o = o->next) { const J *c = j_get(o, "count"); if (c && c->num > bc) { bc = (int)c->num; best = (int)j_get(o, "o")->num; } }
        if (best >= 0) { VLOG("  a card's recruit: as many Regulars as offered (%d)", bc); return best; }
    }
    /* the way a card's Army move goes ("Which way?"): CWOTR made the move already, its way changes nothing there */
    if (kind && !strcmp(kind, "CARD_CHOICE")) {
        const J *c = j_get(decision, "context");
        const char *q = c ? j_str(c, "question") : NULL;
        if (q && !strcmp(q, "Which way?") && R.s.reloc_on) { VLOG("  the way of a card's move: any"); return (int)j_get(opts->child, "o")->num; }
    }
    /* "The Ents Awake: Huorns": another Character card may be played at once; CWOTR offers no such play (its card ends):
       declined, as the engine goes on */
    if (kind && !strcmp(kind, "CARD_CHOICE")) {
        const J *c = j_get(decision, "context");
        const char *q = c ? j_str(c, "question") : NULL;
        if (q && !strcmp(q, "Play another Character card?") && !(leaves_ensure() && R.s.dplayer == me && R.s.dkind != D_ACTION))
            for (const J *o = opts->child; o; o = o->next) { const J *sk = j_get(o, "skip"); if (sk && sk->type == J_BOOL && sk->num) { VLOG("  Huorns: no further Character card (CWOTR offers none)"); return (int)j_get(o, "o")->num; } }
    }
    /* a card's Nation that CWOTR chose itself (it offered one): the Nation whose Political step the copy changed and the view not yet */
    if (kind && !strcmp(kind, "CARD_CHOICE") && view && opts->child && j_str(opts->child, "nation") && !j_str(opts->child, "region") &&
        !(leaves_ensure() && R.s.dplayer == me && R.s.dkind == D_CARD_NATION)) {
        const J *pol = j_get(view, "politics");
        for (const J *o = opts->child; o; o = o->next) {
            int n = nation_of(j_str(o, "nation")); const J *p = pol ? j_get(pol, j_str(o, "nation")) : NULL;
            if (n < 0 || !p) continue;
            if (view_int(p, "step") != R.s.pol[n] || (n < NFPNAT && view_bool(p, "active") != (R.s.active[n] != 0))) { VLOG("  a card's Nation: the engine's"); return (int)j_get(o, "o")->num; }
        }
        VLOG("  a card's Nation: none changes in the engine, any");
        return (int)j_get(opts->child, "o")->num;
    }
    /* a card's "Eliminate which figure?" that CWOTR answered itself: the figure the view has and the copy no longer has */
    if (kind && !strcmp(kind, "CARD_CHOICE") && view && opts->child && is_figure_answer(opts->child) && !(leaves_ensure() && R.s.dplayer == me && R.s.dkind >= D_CARD_REGION)) {
        int best = -1, bd = 0;
        for (const J *o = opts->child; o; o = o->next) {
            int r = region_of(j_str(o, "region")); const char *u = j_str(o, "unit");
            if (r < 0 || !u) continue;
            const J *regs = j_get(view, "regions"), *vr = regs ? j_get(regs, REGION_KEY[r]) : NULL;
            int d = 0;
            for (int n = 0; n < NFPNAT; n++) { uint8_t *c = figure_cell(&R.s, r, n, u); if (c) d += hb_view_count(vr ? j_get(vr, "units") : NULL, NATION_KEY[n], u) - *c; }
            if (d > bd) { bd = d; best = (int)j_get(o, "o")->num; }
        }
        if (best >= 0) { VLOG("  a card's figure: the one CWOTR eliminated"); return best; }
    }
    /* a unit over the stacking limit (a card's Army move, say): CWOTR removed the excess itself (enforce_stacking) and
       asks nothing; the units the view has and the copy no longer has are the ones it removed: one of them */
    if (kind && !strcmp(kind, "REMOVE_EXCESS_UNIT") && view && !(leaves_ensure() && R.s.dplayer == me && R.s.dkind == D_CARD_OPTION && R.s.qtag == Q_EXCESS)) {
        int best = -1, bd = 0;
        for (const J *o = opts->child; o; o = o->next) {
            int r = region_of(j_str(o, "region")), n = nation_of(j_str(o, "nation")); const char *u = j_str(o, "unit");
            if (r < 0 || n < 0 || !u) continue;
            const J *regs = j_get(view, "regions"), *vr = regs ? j_get(regs, REGION_KEY[r]) : NULL;
            int el = !strcmp(u, "elite"), d = hb_view_count(vr ? j_get(vr, "units") : NULL, NATION_KEY[n], u) - (el ? R.s.eli[r][n] : R.s.reg[r][n]);
            if (d > bd) { bd = d; best = (int)j_get(o, "o")->num; }
        }
        if (best >= 0) { VLOG("  an excess unit: the one CWOTR removed"); return best; }
    }
    /* the official rules' Leader re-roll: taken, as CWOTR always does (a re-roll can only add hits) */
    if (kind && !strcmp(kind, "LEADER_REROLL")) {
        for (const J *o = opts->child; o; o = o->next) if (j_str(o, "answer") && !strcmp(j_str(o, "answer"), "reroll")) return (int)j_get(o, "o")->num;
    }
    if (kind && !strcmp(kind, "ACTION") && view && (depth <= 0 || lost)) {
        const char *why = lost ? "lost track" : (R.s.dkind != D_ACTION || R.s.dplayer != me || depth > 0) ? "the engine is elsewhere" : differs(view, me);
        if (why) {
            st.diffs++;
            LOG("%s: rebuilt from the view at turn %d (%s)", game_id, view_int(view, "turn"), why);
            if (resync(view, me)) LOSE("the rebuild failed");
        }
    }
    if (settled_answer(decision) >= 0 && (kind && (!strcmp(kind, "CHOOSE_GUIDE") || !strcmp(kind, "CARD_CHOICE")))) {
        int o = settled_answer(decision);
        if (!(leaves_ensure() && R.s.dplayer == me && first_alive() >= 0 && !leaf_done(first_alive()) && strstr(L[first_alive()].st[depth].text, "card_choice"))) return o;
    }
    const J *ctx = j_get(decision, "context");
    if (checking && !lost && !diverged && !R.s.reloc_on && !(kind && (!strcmp(kind, "ARMY_SELECTION") || !strcmp(kind, "SEPARATE_COMPANIONS"))) && !(ctx && j_str(ctx, "card")) && depth <= 0 && !selmode && !ndefer && view && !R.s.b.active && (!j_get(view, "battle") || j_get(view, "battle")->type == J_NULL)) {
        const char *why = differs(view, me);
        if (why) {
            diverged = 1;
            char moves[1400] = ""; size_t n = 0;
            for (int k = nrecent > 8 ? nrecent - 8 : 0; k < nrecent; k++) n += (size_t)snprintf(moves + n, sizeof moves - n, " / %s", recent[k % 8]);
            int dfp = 0, dsh = 0; for (int fc = 0; fc < NFACE; fc++) { dfp += R.s.dice[FP][fc]; dsh += R.s.dice[SH][fc]; }
            LOG("DIVERGED at %s: %s; last moves:%s; note: %s; copy at %s %d, dice FP %d SH %d, turn %d", kind, why, moves, last_note, SIDE_KEY[R.s.dplayer], R.s.dkind, dfp, dsh, R.s.turn);
        }
    }
    if (checking && view && !lost && !R.s.b.active && !ndefer && !(kind && !strcmp(kind, "DISCARD_EXCESS_CARD")) && j_get(view, "cards")) {     /* where the hand sizes part (diagnostics) */
        static int was_diff;
        const J *cards = j_get(view, "cards");
        int own = j_len(j_get(cards, "hand")), opp = view_int(cards, "opponent_hand");
        int diff = own != R.s.hand_n[me] || opp != R.s.hand_n[1 - me];
        if (diff && !was_diff) {
            char moves[1400] = ""; size_t n = 0;
            for (int k = nrecent > 4 ? nrecent - 4 : 0; k < nrecent; k++) n += (size_t)snprintf(moves + n, sizeof moves - n, " / %s", recent[k % 8]);
            LOG("HANDS at %s turn %d: copy %d/%d view %d/%d (ours/theirs); last moves:%s; note: %s", kind, R.s.turn, R.s.hand_n[me], R.s.hand_n[1 - me], own, opp, moves, last_note);
        }
        was_diff = diff;
    }
    if (lost) return -1;
    if (view && depth <= 0 && !selmode && !R.s.reloc_on && !nlate && !ndefer && R.s.dkind != D_DISCARD && !(kind && !strcmp(kind, "DISCARD_EXCESS_CARD"))) cards_from_view(view, me);
    if (kind && !strcmp(kind, "FELLOWSHIP_PHASE") && !engine_in_fsp_phase()) {
        for (const J *o = opts->child; o; o = o->next) { const char *k = j_str(o, "kind"); if (k && !strcmp(k, "done")) { VLOG("the engine ended the Fellowship phase already"); return (int)j_get(o, "o")->num; } }
    }
    /* our own Combat card question the referee skipped (by its reading we had no card to play): none */
    for (int g = 0; g < 2 && after_cards(kind) && leaves_ensure() && R.s.dplayer == me && R.s.dkind == D_BATTLE_CARD && depth == 0; g++) {
        int pick = -1;
        for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"none\"")) pick = i;
        if (pick < 0) break;
        VLOG("our Combat card, not asked by the referee: none");
        apply_leaf(pick);
    }
    /* the opponent's questions the referee did not ask (no card, an unasked casualty, the rest of a Companions' move),
       and questions with one answer the referee settled by itself: applied until our own question comes (one may
       lead to the other) */
    for (int round = 0; round < 4; round++) {
        int moved = 0;
        while (spent_recruit() || guide_skip_settle() || unasked_end(kind)) moved = 1;
        for (int g = 0; g < 6 && !(kind && !strcmp(kind, "FELLOWSHIP_PHASE")) && opponent_no_card(); g++) moved = 1;
        for (int guard = 0; guard < 10 && leaves_ensure() && R.s.dplayer != me; guard++) {
            int i = first_alive(), distinct = 1;
            for (int k = 0; k < nl; k++) if (alive[k] && !leaf_done(k) && strcmp(L[k].st[depth].text, L[i].st[depth].text)) distinct = 0;
            if (!distinct) break;
            st.forced++; moved = 1;
            if (leaf_done(i)) apply_leaf(i); else { depth++; if (all_done()) apply_leaf(first_alive()); }
        }
        if (!moved) break;
    }
    if (kind && !strcmp(kind, "DISCARD_EXCESS_CARD") && leaves_ensure() && R.s.dkind == D_DISCARD && R.s.dplayer != me) {
        /* the copy asks the opponent to discard first, but the view counts no excess in its hand: the copy held a
           stand-in too many, which goes (then our own discard is the engine's question) */
        const J *vc = view ? j_get(view, "cards") : NULL;
        for (int g = 0; g < 3 && vc && leaves_ensure() && R.s.dkind == D_DISCARD && R.s.dplayer != me && depth == 0
                        && R.s.hand_n[1 - me] > view_int(vc, "opponent_hand"); g++) {
            int pick = -1;
            for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].mv[0].a < NCARD && !known[L[i].mv[0].a]) pick = i;
            if (pick < 0) break;
            VLOG("  the opponent's discard the referee did not ask: a stand-in goes");
            apply_leaf(pick);
        }
        int o = discard_ahead(opts);
        if (o >= 0) return o;
    }
    /* a card's Army move asked as "from>to": the engine's Army, then (on a copy) where it goes */
    if (kind && !strcmp(kind, "CARD_CHOICE") && opts->child && j_str(opts->child, "from") && leaves_ensure() && R.s.dplayer == me && R.s.dkind == D_CARD_ARMY && depth == 0) {
        Move m = choose_move();
        int from = m.a, to = -1;
        State t = R.s; wotr_prune = 0; wotr_apply(&t, m); wotr_prune = 1;
        if (!t.winner && t.dkind == D_CARD_REGION && t.dplayer == me) {
            State c = t; float bv; int n;
            wotr_determinize(&c, me, own_rng += 0x1234567);
            to = agent_choose(&ag, &c, &bv, &n).a;
        }
        int pick = -1;
        for (const J *o = opts->child; o; o = o->next) {
            if (region_of(j_str(o, "from")) != from) continue;
            if (pick < 0 || region_of(j_str(o, "to")) == to) pick = (int)j_get(o, "o")->num;
            if (region_of(j_str(o, "to")) == to) break;
        }
        if (pick >= 0) return pick;
    }
    /* the Army of a card's attack (Grond, The Fighting Uruk-hai, ...): the engine started that battle already */
    if (kind && !strcmp(kind, "CHOOSE_ATTACK") && R.s.b.active && depth <= 0) {
        int pick = -1;
        for (const J *o = opts->child; o && pick < 0; o = o->next)
            if (region_of(j_str(o, "to")) == R.s.b.dst && (region_of(j_str(o, "from")) == R.s.b.src || R.s.b.src == R.s.b.dst)) pick = (int)j_get(o, "o")->num;
        for (const J *o = opts->child; o && pick < 0; o = o->next) if (region_of(j_str(o, "to")) == R.s.b.dst) pick = (int)j_get(o, "o")->num;
        if (pick >= 0) { VLOG("the card's battle has begun in the engine"); return pick; }
    }
    /* the route of a card's move CWOTR made already ("Which way?"): any */
    if (kind && !strcmp(kind, "CARD_CHOICE") && R.s.reloc_on && opts->child && (j_str(opts->child, "region") || j_str(opts->child, "from")
            || (j_str(opts->child, "answer") && strchr(j_str(opts->child, "answer"), '>'))) &&
        !(leaves_ensure() && R.s.dplayer == me && (R.s.dkind == D_CARD_ARMY || R.s.dkind == D_CARD_REGION))) return (int)j_get(opts->child, "o")->num;
    /* the units of a card's Army move CWOTR made itself (relocate_army, Rage of the Dunlendings): as many as it moved */
    if (kind && !strcmp(kind, "ARMY_SELECTION") && !selmode && R.s.reloc_on && ctx && region_of(j_str(ctx, "from")) == R.s.reloc_from && region_of(j_str(ctx, "to")) == R.s.reloc_to) {
        int best = -1, bd = 1 << 30;
        for (const J *o = opts->child; o; o = o->next) {
            int n = nation_of(j_str(o, "nation")); const char *u = j_str(o, "unit"); const J *c = j_get(o, "count");
            if (!c) continue;
            int want = 99;          /* a character: it goes */
            if (n >= 0 && u) want = !strcmp(u, "regular") ? R.s.reloc_reg[n] : !strcmp(u, "elite") ? R.s.reloc_eli[n] : !strcmp(u, "leader") && n < NFPNAT ? R.s.reloc_ldr[n] : !strcmp(u, "nazgul") ? R.s.reloc_naz : 99;
            int d = abs((int)c->num - want);
            if (d < bd) { bd = d; best = (int)j_get(o, "o")->num; }
        }
        if (best >= 0) { VLOG("a card's Army move: as many as CWOTR moved"); return best; }
    }
    if (kind && !strcmp(kind, "ARMY_SELECTION") && (!selmode || sel_battle) && (!leaves_ensure() || R.s.dplayer != me || R.s.dkind != D_ACTION ||
                                                                 (card_move_to >= 0 && ctx && region_of(j_str(ctx, "to")) == card_move_to))) {
        /* a card's Army move (or a card's battle: its attackers): the engine moved the units itself, all of them */
        int best = -1, bc = -1;
        for (const J *o = opts->child; o; o = o->next) { const J *c = j_get(o, "count"); if (c && c->num > bc) { bc = (int)c->num; best = (int)j_get(o, "o")->num; } }
        if (best >= 0) { VLOG("a card's Army move: every unit goes"); return best; }
    }
    if (kind && !strcmp(kind, "ARMY_SELECTION") && selmode && adv_on && ctx && region_of(j_str(ctx, "to")) == adv_dst) {
        /* our advance where the referee offers only "with part of the Army" (from a Stronghold): CWOTR advanced all */
        int best = -1, bc = -1;
        for (const J *o = opts->child; o; o = o->next) { const J *c = j_get(o, "count"); if (c && c->num > bc) { bc = (int)c->num; best = (int)j_get(o, "o")->num; } }
        if (best >= 0) { VLOG("our advance: every unit goes"); return best; }
    }
    if (kind && !strcmp(kind, "CARD_CHOICE")) { int o = answer_ahead(opts); if (o >= 0) return o; }
    if (!leaves_ensure() || R.s.dplayer != me) { VLOG("the engine has no decision of ours here"); LOSE("asked %s, the engine asks %s (%d) [%.60s]", kind, R.s.winner ? "nobody (over)" : SIDE_KEY[R.s.dplayer], R.s.dkind, j_str(decision, "prompt") ? j_str(decision, "prompt") : ""); return -1; }
    if (kind && !strcmp(kind, "ARMY_SELECTION") && target >= 0 && alive[target]) {
        int best = -1, bd = 1 << 30;
        for (const J *o = opts->child; o; o = o->next) {
            int want = sel_count(target, o); const J *c = j_get(o, "count");
            if (want < 0 || !c) continue;
            int d = abs((int)c->num - want);
            if (d < bd) { bd = d; best = (int)j_get(o, "o")->num; }
        }
        if (best >= 0) return best;
    }
    if (kind && !strcmp(kind, "ARMY_SELECTION")) VLOG("selection: target %d alive %d type %d depth %d selmode %d", target, target >= 0 ? alive[target] : -1, target >= 0 ? L[target].mv[0].type : -1, depth, selmode);
    if (selmode) sel_finalize();
    if (target < 0 || !alive[target] || leaf_done(target)) { target = pick_target(); st.planned++; }
    if (kind && !strcmp(kind, "GUIDE_ABILITY")) {      /* CWOTR asks it with the Hunt's damage */
        int use = target >= 0 && !leaf_done(target) && strstr(L[target].st[depth].text, "guide_ability") != NULL;
        for (const J *o = opts->child; o; o = o->next) { const char *k = j_str(o, "kind"); if (k && !strcmp(k, use ? "use" : "skip")) return (int)j_get(o, "o")->num; }
    }
    if (kind && !strcmp(kind, "JOIN_GROUP") && ctx) join_order(j_str(ctx, "character"));
    int planned = target;
    for (int tries = 0; tries < 2; tries++) {
        int best = -1, bo = -1;
        if (target >= 0 && !leaf_done(target))
            for (const J *o = opts->child; o; o = o->next) { int sc = step_match(&L[target].st[depth], o); if (sc > best) { best = sc; bo = (int)j_get(o, "o")->num; } }
        if (bo >= 0) return bo;
        /* a selection step the referee does not ask: the planned move's next one that it does */
        if (target >= 0 && kind && (!strcmp(kind, "ARMY_SELECTION") || !strcmp(kind, "ARMY_MOVE")))
            for (int k = 1; k <= 4 && bo < 0 && L[target].nstep > depth + k; k++)
                for (const J *o = opts->child; o; o = o->next) if (step_match(&L[target].st[depth + k], o) > 0) { bo = (int)j_get(o, "o")->num; depth += k; break; }
        if (bo >= 0) { VLOG("selection steps skipped"); return bo; }
        /* the planned step is not offered: any move still possible */
        for (int i = 0; i < nl && bo < 0; i++) {
            if (!alive[i] || leaf_done(i)) continue;
            for (const J *o = opts->child; o; o = o->next) if (step_match(&L[i].st[depth], o) >= 0) { target = i; bo = (int)j_get(o, "o")->num; break; }
        }
        if (bo >= 0) { VLOG("the planned move is not offered (%s); another of the engine's", planned >= 0 && !leaf_done(planned) ? L[planned].st[depth].text : "-"); return bo; }
    }
    if (verbose) {
        Buf b = {0}; int k = 0;
        for (const J *o = opts->child; o && k < 4; o = o->next, k++) { bput(&b, " |"); for (const J *f = o->child; f; f = f->next) if (strcmp(f->key, "text") && strcmp(f->key, "o")) { if (f->type == J_STR) bput(&b, " %s=%s", f->key, f->str); else if (f->type == J_NUM) bput(&b, " %s=%g", f->key, f->num); else bput(&b, " %s", f->key); } }
        VLOG("no option agrees with the engine's moves (%s, step %d): %s; the options:%s", kind, depth, target >= 0 && !leaf_done(target) ? L[target].st[depth].text : "-", b.p ? b.p : "");
        free(b.p);
    }
    LOSE("asked %s, no option agrees with the engine's (%.60s)", kind, target >= 0 && !leaf_done(target) ? L[target].st[depth].text : "-");
    return -1;
}

/* ================= the session ================= */
static void send_choice(int id, int o, const char *fp) {
    char msg[200];
    if (fp) snprintf(msg, sizeof msg, "{\"type\":\"choice\",\"id\":%d,\"o\":%d,\"fingerprint\":\"%s\"}", id, o, fp);
    else snprintf(msg, sizeof msg, "{\"type\":\"choice\",\"id\":%d,\"o\":%d}", id, o);
    send_line(msg);
}
static int start_at_phase(const J *sv, int ph);
static void new_game(const J *m) {
    const char *side = j_str(m, "side"), *g = j_str(m, "game");
    me = side_of(side);
    snprintf(game_id, sizeof game_id, "%s", g ? g : "?");
    const J *ck = j_get(m, "clock"); const char *kind = ck ? j_str(ck, "kind") : NULL;
    clock_game = kind && !strcmp(kind, "game");
    { const J *of = j_get(m, "official_rules"), *hp = j_get(m, "hidden_passes");
      hidden_passes = of && of->type == J_BOOL && of->num && !(hp && hp->type == J_BOOL && !hp->num); }
    const J *sb = j_get(m, "seed_bot");
    own_rng = sb && sb->type == J_NUM ? (uint64_t)sb->num : (uint64_t)time(NULL);
    ag.rng = own_rng * 31 + 7;
    /* CWOTR 0.1's settings: the Free Peoples one step ahead, the Shadow with a short look-ahead (3 deep, 4 wide) */
    if (me == SH) { ag.depth = 3; ag.width = 4; ag.replies = 1; ag.narrow = 6; ag.taper = 0; ag.dpol[FP] = &player->pol[FP]; ag.dpol[SH] = &player->pol[SH]; }
    else { ag.depth = 0; ag.width = 0; ag.dpol[FP] = ag.dpol[SH] = NULL; }
    if (time_share > 0) { ag.dpol[FP] = &player->pol[FP]; ag.dpol[SH] = &player->pol[SH]; }   /* the rollouts follow the move networks' first moves */
    memset(&R, 0, sizeof R); R.ext = -1;
    reroll_wait = reroll_missing = reroll_misses = 0; wotr_ma_pick = -1; last_note[0] = 0; hunt_sep_hold = 0; hunt_sep_dest = -1; ref_nd[0] = ref_nd[1] = 0; snap_ok = snap_fix = 0; card_move_to = -1; route_pending = 0; voice_pending = 0;   /* nothing of the last game */
    started = 0; lost = 0; nev = 0; guide_skip_pending = 0; ref_turn = 0; ref_phase = -1; rq_clear(); leaves_reset(); nlate = 0; adv_on = 0; stash_on = 0; memset(stash_ldr, 0, sizeof stash_ldr); stash_naz = 0; diverged = 0; nrecent = 0;
    while (ndefer) free(defer[--ndefer]);
    memset(&st, 0, sizeof st);
    LOG("%s: Hambot plays %s", game_id, side ? side : "?");
    /* A game from a position (a certification scenario, a suite entry): no setup to follow; new_game carries this
       side's view of it, an Action of the side to act: the copy is built from it */
    const J *sv = j_get(m, "view");
    if (sv) {
        int act = side_of(j_str(sv, "to_act"));
        if (act < 0) act = me;
        int ph = phase_index(j_str(sv, "phase"));
        if (resync(sv, me)) LOG("%s: the start position could not be read", game_id);   /* our view: our cards known */
        else if (ph == 1 || ph == 2) {
            if (start_at_phase(sv, ph)) LOG("%s: the start position's %s could not be reached", game_id, j_str(sv, "phase"));
            else { st.resyncs = 0; LOG("%s: from a position (new_game's view), at the %s", game_id, j_str(sv, "phase")); }
        } else {
            R.s.dplayer = (uint8_t)act; R.s.st[R.s.sp - 1].v[0] = (uint8_t)act;          /* the side to act's Action */
            st.resyncs = 0; LOG("%s: from a position (new_game's view), %s to act", game_id, SIDE_KEY[act]);
        }
    }
}
/* A start position in the Fellowship phase (ph 1) or at the Hunt allocation (2): the copy (resync's, at an Action) is
 * taken back to the end of the turn before and the turn begun in the engine (its own draws: the referee sends none,
 * the view has them), the Fellowship phase's question answered "end" for the Hunt allocation; then the view's board,
 * cards and Fellowship laid over it, the engine's questions kept. 0: the copy is there. */
static int start_at_phase(const J *sv, int ph) {
    char err[200];
    State t = R.s;
    if (t.turn < 2) wotr_init(&t, own_rng ^ 0x5eed);       /* turn 1: the game's own start (its Fellowship phase next) */
    else if (wotr_begin_turn(&t, own_rng ^ 0x5eed, err, sizeof err)) { LOG("start: %s", err); return 1; }
    int target = ph == 1 ? D_FSP_DECLARE : D_HUNT_ALLOC;
    for (int guard = 0; guard < 16 && t.dkind != target && !t.winner; guard++) {
        Move mv[MAXMOVES]; int n = wotr_moves(&t, mv), k = n > 0 ? 0 : -1;
        if (t.dkind == D_FSP_DECLARE) { k = -1; for (int i = 0; i < n; i++) if (mv[i].a == 255) k = i; }
        else if (t.dkind != D_DISCARD) { LOG("start: the engine asks %d before the phase", t.dkind); return 1; }
        if (k < 0) { LOG("start: no move to go on"); return 1; }
        wotr_apply(&t, mv[k]);
    }
    if (t.dkind != target) { LOG("start: the phase not reached (%d)", t.dkind); return 1; }
    if (board_from_view(&t, sv, me, err, sizeof err)) { LOG("start: %s", err); return 1; }
    if (wotr_check(&t, err, sizeof err)) { LOG("start: the position fails the engine's check: %s", err); return 1; }
    R.s = t;
    return 0;
}
static void game_over(const J *m) {
    const char *w = j_str(m, "winner"), *how = j_str(m, "how");
    LOG("%s: over, winner %s by %s; %ld decisions (%ld planned, %ld with one option), %ld fallback answers, %ld rebuilt from the view, %ld events ignored, %ld forced steps, %ld draws not from the referee",
        game_id, w ? w : "none", how ? how : "?", st.decisions, st.planned, st.asked_single, st.fallback, st.resyncs, st.ignored, st.forced, st.soft);
    for (int k = 0; k < nev; k++) free(ev[k]);
    nev = 0; started = 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: hambot <player: the path without .fp/.sh/.pfp/.psh>\n"); return 2; }
    verbose = getenv("HAMBOT_VERBOSE") && *getenv("HAMBOT_VERBOSE") == '1';
    checking = getenv("HAMBOT_CHECK") && *getenv("HAMBOT_CHECK") == '1';
    if (getenv("HAMBOT_TIME")) { time_share = atof(getenv("HAMBOT_TIME")); if (time_share > 0.95) time_share = 0.8; if (time_share < 0) time_share = 0; }
    wotr_global_init(); feat_init(); names_init();
    if (getenv("HAMBOT_TRACE")) trace_r = region_of(getenv("HAMBOT_TRACE"));
    player = malloc(sizeof(Player));
    if (!player || player_load(player, argv[1])) { fprintf(stderr, "cannot load the player %s\n", argv[1]); return 1; }
    memset(&ag, 0, sizeof ag);
    ag.net[FP] = &player->fp; ag.net[SH] = &player->sh; ag.samples = 3; ag.temp = 0.02f;
    L = malloc(sizeof(Leaf) * MAXLEAF); alive = malloc(MAXLEAF);
    if (!L || !alive) return 1;
    LOG("Hambot %s (player %s) ready", VERSION, argv[1]);
    for (;;) {
        char *line = read_line(), *mem;
        if (!line) break;
        const J *m = j_parse(line, &mem);
        if (!m) { LOG("not JSON: %.120s", line); continue; }
        const char *type = j_str(m, "type");
        const J *id = j_get(m, "id");
        int iid = id && id->type == J_NUM ? (int)id->num : 0;
        if (!type) { free(mem); continue; }
        if (!strcmp(type, "hello")) {
            char msg[600];
            snprintf(msg, sizeof msg, "{\"type\":\"hello\",\"id\":%d,\"protocol\":\"wotr-bot/0.2\",\"name\":\"Hambot\",\"version\":\"%s (CWOTR 0.1)\","
                     "\"author\":\"Philip Siler (CWOTR); protocol client by BehindMedia\",\"events\":true,\"rulesets\":[\"base\"]}", iid, VERSION);
            send_line(msg);
        } else if (!strcmp(type, "new_game")) {
            new_game(m);
            char msg[80]; snprintf(msg, sizeof msg, "{\"type\":\"ready\",\"id\":%d}", iid); send_line(msg);
        } else if (!strcmp(type, "event")) {
            if (nev < MAXEV) ev[nev++] = strdup(line);
            else LOG("too many events between decisions");
        } else if (!strcmp(type, "decide")) {
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            process_events();
            const J *d = j_get(m, "decision");
            const char *fp = d ? j_str(d, "fingerprint") : NULL;
            { const J *ck = j_get(m, "clock"), *lm = ck ? j_get(ck, "left_ms") : NULL, *im = ck ? j_get(ck, "increment_ms") : NULL;
              clock_known = lm && lm->type == J_NUM; clock_left = clock_known ? lm->num : 0; clock_incr = im && im->type == J_NUM ? im->num : 0; }
            int o = d ? answer(d, j_get(m, "view")) : -1;
            if (o < 0) { o = d ? fallback_answer(d, j_get(m, "view")) : 0; st.fallback++; LOG("FALLBACK %s: %s", d ? j_str(d, "kind") : "?", lost_why); }
            send_choice(iid, o, fp);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            VLOG("answered %s with %d in %.1f ms", d ? j_str(d, "kind") : "?", o, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
        } else if (!strcmp(type, "check")) {       /* the offline replay (replay.py): the copy against another view, in a child */
            fflush(stderr);
            pid_t pid = fork();
            if (pid == 0) {
                process_events();
                const J *v = j_get(m, "view"); const char *at = j_str(m, "at");
                int pov = v ? side_of(j_str(v, "viewer")) : -1;
                if (v && pov >= 0 && !lost && !diverged && depth <= 0 && !selmode && !ndefer && !R.s.reloc_on && !R.s.b.active && (!j_get(v, "battle") || j_get(v, "battle")->type == J_NULL)) {
                    foreign = pov != me;
                    const char *why = differs(v, pov);
                    if (why) {
                        char moves[1400] = ""; size_t n = 0;
                        for (int k = nrecent > 8 ? nrecent - 8 : 0; k < nrecent; k++) n += (size_t)snprintf(moves + n, sizeof moves - n, " / %s", recent[k % 8]);
                        LOG("CHECK %s at %s: %s; last moves:%s; note: %s", game_id, at ? at : "?", why, moves, last_note);
                    }
                }
                fflush(stderr); _exit(0);
            }
            if (pid > 0) waitpid(pid, NULL, 0);
        } else if (!strcmp(type, "game_over")) {
            game_over(m);
        } else if (!strcmp(type, "quit")) {
            free(mem); break;
        } else if (!strcmp(type, "error") || !strcmp(type, "timeout")) {
            LOG("%s: %s", game_id, line);
        }
        free(mem);
    }
    return 0;
}
