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
    if (R.s.dkind == D_ACTION) { R.s.rec_n = 0; rec_used = 0; R.s.reloc_on = 0; }     /* a new action: the recruits and card moves logged anew */
    wotr_rnd_hook = hook;
    wotr_apply(&R.s, m);
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
static int leaves_ensure(void) {
    if (depth >= 0) return nl;
    if (R.s.winner) return 0;
    dice_from_state();
    wotr_prune = 0;                 /* every legal move: the referee's are */
    nl = build_leaves(&R, L);
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
    snap_ok = 1;
    wotr_move_str(&R.s, L[i].mv[0], buf, sizeof buf);
    VLOG("  %s plays %s", SIDE_KEY[R.s.dplayer], buf);
    snprintf(recent[nrecent++ % 8], sizeof recent[0], "T%d %s %s", R.s.turn, R.s.dplayer ? "SH" : "FP", buf);
    Leaf lf = L[i];
    if (lf.nmv && lf.mv[0].type == M_VOICE && lf.mv[0].a == 0) { voice_pending = 1; memset(voice_seen, 0, sizeof voice_seen); }
    for (int k = 0; k < lf.nmv; k++) apply_followed(lf.mv[k]);
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
    if (!snap_ok) return 0;
    snap_ok = 0;
    R.s = snap; memcpy(known, snap_known, sizeof known);
    for (int k = 0; k < nrq; k++) rq[k].used = snap_used[k];
    if (c >= 0) { if (!ensure_card(&R.s, c, W_HAND)) return 0; }
    else {
        int d = side * 2 + deck, x = -1;
        for (int k = d * 24; k < d * 24 + 24 && x < 0; k++) if (R.s.card_where[k] == W_DECK && !known[k] && CARD[k].combat) x = k;
        if (x < 0 || !ensure_card(&R.s, x, W_HAND)) return 0;
        known[x] = 0;               /* a guess: the real card replaces it when it shows */
    }
    Leaf lf = snap_leaf;
    leaves_reset();
    for (int k = 0; k < lf.nmv; k++) apply_followed(lf.mv[k]);
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
static int fallback_answer(const J *decision) {
    const char *kind = j_str(decision, "kind");
    const J *opts = j_get(decision, "options");
    int s0 = settled_answer(decision);
    if (s0 >= 0) return s0;
    for (const J *o = opts ? opts->child : NULL; o; o = o->next) {
        const char *k = j_str(o, "kind");
        if (kind && !strcmp(kind, "FELLOWSHIP_PHASE") && k && !strcmp(k, "done")) return (int)j_get(o, "o")->num;
        if (kind && !strcmp(kind, "HUNT_ALLOCATION") && k && !strcmp(k, "hunt_dice") && j_get(o, "count") && (int)j_get(o, "count")->num == 1) return (int)j_get(o, "o")->num;
    }
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
static void sel_finalize(void) {
    if (!selmode) return;
    selmode = 0;
    int best = -1, bc = 1 << 30;
    for (int i = 0; i < nl; i++) {
        if (!alive[i]) continue;
        Move m = L[i].mv[0];
        if (m.type != M_ARMY && m.type != M_ATTACK) continue;
        int cost = abs(m.c - sel_reg) + abs(m.d - sel_eli);
        if (cost < bc) { bc = cost; best = i; }
    }
    if (best < 0 && adv_on) {
        Move m; memset(&m, 0, sizeof m); m.type = M_ARMY; m.a = (uint8_t)adv_src; m.b = (uint8_t)adv_dst;
        VLOG("  the advance with the referee's split");
        sel_fixup(&adv_before, m);
        adv_on = 0;
        return;
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
    /* a Fellowship phase the referee did not ask (nothing to do there, by its rules): done, for either side */
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
/* Follows one answered decision. look: the events after it (for a hidden Combat card shown later). */
static int follow_choice_(const J *e, int look);
static int follow_choice(const J *e, int look) { following = 1; int r = follow_choice_(e, look); following = 0; return r; }
static int follow_choice_(const J *e, int look) {
    int deferred = 0;
    if (lost || !started) return 0;
    const char *side_key = j_str(e, "side"), *decision = j_str(e, "decision");
    const J *opt = j_get(e, "option");
    int side = side_of(side_key);
    if (!opt || side < 0) return 0;
    int ev_face = -1, ev_ring_to = -1;
    if (decision && !strcmp(decision, "ACTION")) {
        const char *fk = j_str(opt, "face"), *kk = j_str(opt, "kind"), *tk = j_str(opt, "to");
        ev_face = fk && kk && !strcmp(kk, "use_die") ? face_of(fk) : -1;
        ev_ring_to = tk && kk && !strcmp(kk, "elven_ring") ? face_of(tk) : -1;
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
    /* the new Guide on a tie: CWOTR picks one itself; the referee's answer is the one played */
    if (decision && !strcmp(decision, "CHOOSE_GUIDE")) {
        int c = char_of(j_str(opt, "character"));
        if (c == CH_GANDALF_WHITE) c = C_GANDALF; else if (c == CH_ARAGORN) c = C_STRIDER; else if (c == CH_GOLLUM) c = GOLLUM;
        if (c >= 0 && c <= GOLLUM && (c == GOLLUM || R.s.comp_loc[c] == LOC_FSP)) { R.s.guide = (uint8_t)c; leaves_reset(); }
        goto done;
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
    int forced = 0;
    for (int guard = 0; guard < 40; guard++) {
        if (!leaves_ensure()) { VLOG("  no decision in the engine for %s %s", side_key, decision); st.ignored++; break; }
        /* the engine asks a Combat card the referee never asked (by its reading the side had none to play), and the
           referee is already further (any other question): none */
        if (R.s.dkind == D_BATTLE_CARD && depth <= 0 && decision && strcmp(decision, "COMBAT_CARD") && forced < 4) {
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
            if (R.s.dkind == D_BATTLE_CARD && depth <= 0 && forced++ < 3) {
                int pick = -1;
                for (int i = 0; i < nl && pick < 0; i++) if (alive[i] && L[i].nstep && strstr(L[i].st[0].text, "\"kind\":\"none\"")) pick = i;
                if (pick >= 0) {
                    if (verbose) { char b[400] = ""; size_t bn = 0; for (int i = 0; i < R.s.nopt && bn < 300; i++) if (R.s.opt[i] < NCARD) bn += (size_t)snprintf(b + bn, sizeof b - bn, " %s", CARD_KEY[R.s.opt[i]]);
                                   VLOG("  %s's Combat card, not asked by the referee: none (the engine offered:%s)", SIDE_KEY[R.s.dplayer], b); }
                    apply_leaf(pick); continue;
                }
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
            if (!still) { free(defer[d]); memmove(&defer[d], &defer[d + 1], sizeof(char *) * (size_t)(ndefer - d - 1)); ndefer--; again = 1; break; }
        }
    }
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
        if (kind && !strcmp(kind, "choice")) {
            if (follow_choice(x, k + 1)) { if (ndefer < MAXDEF) defer[ndefer++] = strdup(ev[k]); }
            else retry_deferred();
        }
        else if (kind && !lost && !strcmp(kind, "army_moved")) army_moved(x);
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
    /* drop what has been used */
    int w = 0;
    for (int i = 0; i < nrq; i++) if (!rq[i].used) rq[w++] = rq[i];
    nrq = w;
    snap_ok = 0;
}

/* ================= the view: compare, rebuild ================= */
static int view_count(const J *units, const char *nation, const char *unit) {
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
            t->reg[r][n] = (uint8_t)view_count(units, NATION_KEY[n], "regular");
            t->eli[r][n] = (uint8_t)view_count(units, NATION_KEY[n], "elite");
            if (n < NFPNAT) t->ldr[r][n] = (uint8_t)view_count(units, NATION_KEY[n], "leader");
        }
        t->naz[r] = (uint8_t)view_count(units, "SAURON", "nazgul");
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
    while (ndefer) free(defer[--ndefer]);
    started = 1; lost = 0; st.resyncs++; last_note[0] = 0; diverged = 0;
    return 0;
}

/* ================= our decisions ================= */
static Move choose_move(void) {
    State c = R.s; float bv; int n;
    wotr_prune = 1;
    wotr_determinize(&c, me, own_rng += 0x1234567);   /* the player sees only what its side may know */
    Move m = agent_choose(&ag, &c, &bv, &n);
    char buf[200]; wotr_move_str(&R.s, m, buf, sizeof buf);
    VLOG("CWOTR: %s (chance the Free Peoples win %.0f%%)", buf, 100 * nn_prob(bv));
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
/* The option to answer, or -1. */
static int answer(const J *decision, const J *view) {
    const J *opts = j_get(decision, "options");
    int nopt = j_len(opts);
    const char *kind = j_str(decision, "kind");
    st.decisions++;
    if (selmode && !(kind && (!strcmp(kind, "ARMY_SELECTION") || !strcmp(kind, "SEPARATE_COMPANIONS")))) sel_finalize();
    if (nopt <= 1) { st.asked_single++; return 0; }
    if (!started) start_game();
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
    for (int g = 0; g < 2 && kind && strcmp(kind, "COMBAT_CARD") && leaves_ensure() && R.s.dplayer == me && R.s.dkind == D_BATTLE_CARD && depth == 0; g++) {
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
    if (kind && !strcmp(kind, "ARMY_SELECTION") && !selmode && (!leaves_ensure() || R.s.dplayer != me || R.s.dkind != D_ACTION)) {
        /* a card's Army move: the engine moved the units itself, all of them */
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
static void new_game(const J *m) {
    const char *side = j_str(m, "side"), *g = j_str(m, "game");
    me = side_of(side);
    snprintf(game_id, sizeof game_id, "%s", g ? g : "?");
    const J *sb = j_get(m, "seed_bot");
    own_rng = sb && sb->type == J_NUM ? (uint64_t)sb->num : (uint64_t)time(NULL);
    ag.rng = own_rng * 31 + 7;
    /* CWOTR 0.1's settings: the Free Peoples one step ahead, the Shadow with a short look-ahead (3 deep, 4 wide) */
    if (me == SH) { ag.depth = 3; ag.width = 4; ag.replies = 1; ag.narrow = 6; ag.taper = 0; ag.dpol[FP] = &player->pol[FP]; ag.dpol[SH] = &player->pol[SH]; }
    else { ag.depth = 0; ag.width = 0; ag.dpol[FP] = ag.dpol[SH] = NULL; }
    memset(&R, 0, sizeof R); R.ext = -1;
    started = 0; lost = 0; nev = 0; rq_clear(); leaves_reset(); nlate = 0; adv_on = 0; stash_on = 0; memset(stash_ldr, 0, sizeof stash_ldr); stash_naz = 0; diverged = 0; nrecent = 0;
    while (ndefer) free(defer[--ndefer]);
    memset(&st, 0, sizeof st);
    LOG("%s: Hambot plays %s", game_id, side ? side : "?");
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
            int o = d ? answer(d, j_get(m, "view")) : -1;
            if (o < 0) { o = d ? fallback_answer(d) : 0; st.fallback++; LOG("FALLBACK %s: %s", d ? j_str(d, "kind") : "?", lost_why); }
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
