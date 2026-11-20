/* agent.c -- 1-ply expectimax with sampled chance, own sub-choice expansion, hidden-hand determinization */
#include <math.h>
#include "agent.h"
#include <stdio.h>
#include <stdlib.h>

static uint64_t next(uint64_t *s) { *s += 0x9E3779B97F4A7C15ULL; uint64_t z = *s; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; return z ^ (z >> 31); }

/* Specialist networks: three families of tactical decision are scored by networks of their own (Agent.spec), each
   trained on how the position stood at the start of the following turn instead of on who won the game. */
int spec_group(int dkind) {
    switch (dkind) {
    case D_HUNT_ALLOC: return SP_HUNT;
    case D_SIEGE_RETREAT: case D_RETREAT: case D_CONTINUE: case D_SIEGE_EXTEND: case D_ADVANCE: case D_CASUALTY: case D_SELF_HITS: case D_BATTLE_CARD: return SP_BATTLE;
    case D_HUNT_RESPONSE: case D_GUIDE: case D_FSP_DECLARE: case D_ENTER_MORDOR: case D_REVEAL_DEST: return SP_FSP;
    default: return -1;
    }
}
float agent_value(Agent *a, const State *s) {
    if (s->winner) return terminal_value(s);
    float win; static _Thread_local FeatDelta d; static _Thread_local Feat f; static _Thread_local float o[NOUT];
    const Net *vn = a->vnet[s->dplayer] ? a->vnet[s->dplayer] : a->net[s->dplayer];
    if (a->has_root) { feat_delta(a->root, &a->rc, s, &d); win = nn_win_list(vn, a->acc[s->dplayer], &d); }     /* encode only what differs from the root */
    else { feat_encode(s, &f); nn_forward(vn, &f, o); win = o[O_WIN]; }
    a->evals++; a->ev_kind[a->cur_kind]++;
    return win > WIN_LOGIT ? WIN_LOGIT : win < -WIN_LOGIT ? -WIN_LOGIT : win;
}

/* a follow-up choice by the same player that completes the action just started */
static int is_sub_choice(int dk) {
    switch (dk) {
    /* only choices whose intermediate state means nothing by itself; movement chains and the second
       muster are evaluated as ordinary positions */
    case D_SEP_DEST: case D_CARD_REGION: case D_CARD_OPTION: case D_CARD_NATION: case D_CARD_CARD: case D_CARD_COMP:
    case D_CARD_ARMY: return 1;
    default: return 0;
    }
}

/* Battle lookahead. A battle is judged by how it ends, not by its first round, so a position inside a battle
 * is played out to the battle's end with plain default tactics for both sides before it is evaluated:
 * no combat cards, press on while not outnumbered, withdraw into a stronghold or retreat when outnumbered. */
static void finish_battle(State *t) {
    Move mv[MAXMOVES];
    for (int guard = 0; guard < 200 && !t->winner && t->b.active; guard++) {
        int n = wotr_moves(t, mv), pick = 0;
        const Battle *b = &t->b;
        int au = units(t, b->att, b->src), du = units(t, 1 - b->att, b->dst);
        switch (t->dkind) {
        case D_CONTINUE: pick = au >= du ? 1 : 0; break;                    /* options: 0 cease, 1 continue */
        case D_SIEGE_RETREAT: pick = du <= au ? 1 : 0; break;               /* 1 = into the stronghold */
        case D_RETREAT: pick = (du < au && n > 1) ? 1 : 0; break;           /* 0 = stand, others = regions */
        case D_ADVANCE: pick = n > 1 ? 1 : 0; break;
        default: pick = 0; break;                                           /* no card, no siege extension, first casualty */
        }
        wotr_apply(t, mv[pick < n ? pick : 0]);
    }
}

float agent_hum[2], agent_probe, agent_probe_siege;
int agent_flank;
/* Shadow's march m gets no nearer some enemy city or stronghold, the regions that do lead nearer all hold enemy armies, and the new region has an open way on (the same test as in movegen.c) */
static int goes_round(const State *s, Move m) {
    int src = m.a, dst = m.b;
    for (int t = 0; t < NREG; t++) {
        int ty = REGION[t].settle;
        if (!(ty == S_CITY || ty == S_STRONGHOLD) || s->ctrl[t] == SH || DIST[dst][t] != DIST[src][t] || DIST[src][t] > 3 || DIST[src][t] < 2) continue;
        int blocked = 1, open = 0;
        for (int j = 0; j < REGION[src].nadj && blocked; j++) { int y = REGION[src].adj[j]; if (DIST[y][t] < DIST[src][t] && !units(s, FP, y)) blocked = 0; }
        for (int j = 0; j < REGION[dst].nadj && !open; j++) { int z = REGION[dst].adj[j]; open = DIST[z][t] < DIST[dst][t] && (z == t || !units(s, FP, z)); }
        if (blocked && open) return 1;
    }
    return 0;
}
int agent_bsamp;
static float eval_move(Agent *a, const State *s, Move m, int pov, int depth, uint64_t seed, int samples) {
    float sum = 0; int k = 0;
    if ((m.type == M_ATTACK || s->b.active) && samples < 3) samples = 3;     /* battles are dice: look at a few */
    /* agent_bsamp (WOTR_BSAMP=<n>): look at n rolls of every attack. With three, the best of many attacks is often just
       the luckiest three rolls, which leans the choice toward attacking (against AlphaWOTR Shadow kept assaulting
       besieged strongholds with no elite in the army). */
    if ((m.type == M_ATTACK || s->b.active) && samples < agent_bsamp) samples = agent_bsamp;
    for (; k < samples; k++) {
        State t = *s;
        t.rng = seed + (uint64_t)k * 0x632BE59BD9B4E019ULL;
        uint32_t nr = t.nrand;
        wotr_apply(&t, m);
        int chance = t.nrand != nr;
        float v;
        if (t.winner) v = terminal_value(&t);
        else if (!chance && depth < 1 && t.dplayer == pov && is_sub_choice(t.dkind)) {
            Move sub[MAXMOVES]; int n = wotr_moves(&t, sub);
            if (n > 24) v = agent_value(a, &t);
            else {
                v = pov == FP ? -1e9f : 1e9f;
                for (int i = 0; i < n; i++) {
                    float x = eval_move(a, &t, sub[i], pov, depth + 1, seed ^ 0x5851F42D4C957F2DULL, 1);
                    if (pov == FP ? x > v : x < v) v = x;
                }
            }
        } else if (t.dkind == D_ADVANCE && t.dplayer != pov && !getenv("WOTR_NOADV")) {
            /* the move leaves the other side choosing whether to advance into the region just fought over: judge it by
               what they will do. Without this, walking out of an attacked stronghold looked safe, because the position
               was scored before the attacker stepped into the empty stronghold (against AlphaWOTR: the Woodland Realm,
               Lórien, Helm's Deep and Minas Tirith all went that way by turn 5). */
            Move sub[MAXMOVES]; int n = wotr_moves(&t, sub);
            v = pov == FP ? 1e9f : -1e9f;
            for (int i = 0; i < n; i++) {
                State u = t; wotr_apply(&u, sub[i]);
                if (u.b.active) finish_battle(&u);
                float x = u.winner ? terminal_value(&u) : agent_value(a, &u);
                if (pov == FP ? x < v : x > v) v = x;
            }
} else {
            if (t.b.active) finish_battle(&t);
            v = t.winner ? terminal_value(&t) : agent_value(a, &t);
        }
        sum += v;
        if (!chance) { k++; break; }
    }
    return sum / (float)k;
}

/* Move types the search always looks at, whatever the policy thinks: there are only a few of each per decision
 * and whole plans hinge on them (going to war, attacking, minions and characters, the Fellowship, cards).
 * The policy prunes the bulk: army moves, regular musters, separations. */
static int essential(int type) {
    switch (type) {
    case M_ATTACK: case M_DIPLO: case M_MINION: case M_VOICE: case M_FSP_MOVE: case M_FSP_HIDE: case M_GANDALF_WHITE: case M_ARAGORN:
    case M_PLAY: case M_DRAW: case M_MUSTER_ELITE: case M_NAZGUL: case M_COMP_POLITICS: case M_DISCARD_TABLE: return 1;
    default: return 0;
    }
}

static double unif(uint64_t *s) { return (double)(next(s) >> 11) / 9007199254740992.0; }

/* Lane guards: small nudges on a few choices, in log-odds, added to a move's value when moves are compared (never to
   the value that is learned from, and never removing a move). They are part of the hand-written starting guidance
   and fade with the strategy formula (the caller multiplies them by the network's alpha). They mark the three places where
   self-play Shadow strays furthest from what 1,508 human games show:
     - throwing an action die away unused: humans practically never do; ours did it 0.2 to 0.5 times a turn
     - Hunt dice: humans set aside 0.8 a turn; ours up to 1.7, a fifth of the time three or more
     - attacking: humans have 10 units inside Free Peoples settlements after turn 4; ours under 1
   WOTR_LANE=0 switches them off, for control runs. */
/* g_imagined: set while the look-ahead is ranking the OTHER side's replies. The Free Peoples' Fellowship nudge is then
   the rule as it stood before 18:14 on 2026-11-15: with the stronger one, Shadow's look-ahead expected a Fellowship that
   our own Free Peoples run but AlphaWOTR's do not, and r7's Shadow fell from 28 wins in 40 to 19-20 (restored to 27 by
   WOTR_RUSH=old, 31 of 40 games identical move for move). */
static _Thread_local int g_imagined;
/* The nudges are scaled by the formula's share, so they fade with it. WOTR_NUDGE=<x> holds them at a fixed strength
   instead (0.3 = as they were at the share every player so far was trained with): fading the formula and taking off
   the nudges are then two separate steps. (At share 0 with the nudges gone too, Shadow lost 4 games in 10 to raids
   on its strongholds.) */
static float nudge_share(float al) { static float x = -2; if (x < -1) { const char *v = getenv("WOTR_NUDGE"); x = v ? (float)atof(v) : -1; } return x >= 0 ? x : al; }
/* what an own city or stronghold with a Free Peoples army beside it is worth guarding, when `left` Shadow units would
   stand in it (0: not one, not threatened, or held well enough). Held well enough: at least one unit - or, with
   WOTR_HOME_THIN=1 (2026-11-20), at least as many units as the largest such Free Peoples army, five at most (what fits
   inside the walls). Against the raiding Free Peoples of run slow2, Orthanc was lost with one regular in it and four
   Rohan units next door, Moria with three against four Elves. */
static float home_cost(const State *s, int r, int left) {
    int t = REGION[r].settle, vp = t == S_STRONGHOLD ? 2 : t == S_CITY ? 1 : 0, threat = 0;
    if (!vp || orig_side(r) != SH || s->ctrl[r] != SH || s->siege[r]) return 0;
    static int reach = -1, thin = -1;
    if (reach < 0) { const char *v = getenv("WOTR_HOME_REACH"); reach = v ? atoi(v) : 1; v = getenv("WOTR_HOME_THIN"); thin = v ? atoi(v) : 0; }      /* reach: how near the Free Peoples army must be (regions) */
    for (int q = 0; q < NREG; q++)
        if (DIST[r][q] <= reach && !(s->siege[q] && s->ctrl[q] == FP)) { int u = units(s, FP, q); if (u > threat) threat = u; }
    if (!threat || left >= (thin ? (threat < 5 ? threat : 5) : 1)) return 0;
    return wotr_vp(s, FP) + vp >= 4 ? 10.0f : 2.0f * (float)vp;
}
static float lane_guard(const State *s, Move m, int n) {
    static int on = -1;
    if (on < 0) { const char *v = getenv("WOTR_LANE"); on = !(v && v[0] == '0'); }
    if (!on) return 0;
    if (s->dplayer == FP) {
        /* The Free Peoples' side of it (WOTR_LANE_FP=0 switches it off): once Shadow played at the human pace they
           gave up the Ring -- the Fellowship moved 0.0 times a turn against 1.5 for humans -- and threw away up to
           a die a turn. */
        static int fp = -1;
        if (fp < 0) { const char *v = getenv("WOTR_LANE_FP"); fp = !(v && v[0] == '0'); }
        if (!fp) return 0;
        /* "If you end a turn with the Fellowship revealed it might be a good idea to replace Gandalf as Guide with
           Strider ... to make sure that you can hide the Fellowship immediately" (Strategy 101, The Companions): with
           Strider guiding, any die hides it and the Character dice stay free for moving. */
        if (s->dkind == D_GUIDE) return s->revealed && m.a == C_STRIDER ? 0.3f : 0;
        /* A Hunt hit outside Mordor: "When faced with a 2 or a 3 tile it might be wise to sacrifice the Guide ... but
           don't sacrifice a randomly selected Companion as a casualty when your opponent draws a 1 tile" (Strategy
           101, The Quest of the Fellowship). Human Ring wins reach Mordor with four companions; the formula alone
           gave one up to a random casualty two and a half times as often as it took the corruption. */
        if (s->dkind == D_HUNT_RESPONSE && s->mordor < 0 && s->st[s->sp - 1].v[4] <= 1 && (m.a == 4 || m.a == 5)) return -0.4f;
        /* and inside: "Once in Mordor you should take the Companions as casualties as soon as possible in order to
           limit the number of Hunt dice the Shadow can place in the Hunt box" (Strategy 101, Into Mordor) */
        if (s->dkind == D_HUNT_RESPONSE && s->mordor >= 0 && (m.a == 4 || m.a == 5)) return 0.3f;
        /* attacked in an own stronghold region: go inside the walls. "Defending in siege is by far the most effective
           way to avoid losing your hard acquired troops"; the formula alone stayed out for a field round half the
           time, and against AlphaWOTR gave up Minas Tirith on turn 2 that way. */
        if (s->dkind == D_SIEGE_RETREAT) { static float w = -1; if (w < 0) { const char *v = getenv("WOTR_WALLS"); w = v ? (float)atof(v) : 3.0f; } return m.a == 1 ? w : 0; }
        /* ... and do not walk out of it after a round in the field: the attacker then advances into an empty
           stronghold. Against AlphaWOTR the Woodland Realm went that way on turn 3 ("retreat -> Dale"). */
        if (s->dkind == D_RETREAT && m.a != 255 && REGION[s->b.dst].settle == S_STRONGHOLD && s->ctrl[s->b.dst] == FP && m.a != s->b.dst) { static float w = -1; if (w < 0) { const char *v = getenv("WOTR_STAY"); w = v ? (float)atof(v) : 3.0f; } return -w; }
        if (s->dkind != D_ACTION) return 0;
        if (m.type == M_SKIP && n > 1) return -0.3f;
        if (m.type == M_FSP_MOVE) {
            /* Keep the Ring moving. With the trained network at a 30% formula share, a nudge of 1.0 (0.3 in effect)
               takes the Free Peoples from 56.5% to 62.6% against our own Shadow (0.5: 60.3%; 2.0: 57.8%). Against
               AlphaWOTR our Free Peoples were losing to conquest on turns 6-11 with the Fellowship at corruption 1-5,
               its Hunt budget unspent, having moved it 1.3 times a turn where AlphaWOTR's own moves 2. (With the bare
               formula a nudge outside Mordor had changed nothing; WOTR_RUSH sets another strength.) */
            static float w = -1; static int oldrule = 0;      /* WOTR_RUSH=old: the rule as it was before 18:14 on 2026-11-15 (0.5 inside Mordor, nothing outside), for exact comparisons */
            if (w < 0) { const char *v = getenv("WOTR_RUSH"); oldrule = v && !strcmp(v, "old"); w = v && !oldrule ? (float)atof(v) : 1.0f; }
            return oldrule || g_imagined ? (s->mordor >= 0 ? 0.5f : 0.0f) : w;
        }
        /* "If there are only one or two dice [in the Hunt box], use an Elven ring (if possible) to hasten the movement
           of the Fellowship" (Strategy 101, The Quest of the Fellowship). In human games the three rings are used
           2.1 times a game, evenly from turn 1; the formula alone used 1.2, late, and half of those for armies. */
        if (m.type == M_RING && m.b == F_CHAR && s->hunt_sh <= 2) return 0.25f;
        /* Character and Will of the West dice are the only ones that move or hide the Fellowship: "Will of the West
           dice not needed for Gandalf the White or Aragorn should almost always be spent for moving/hiding the
           fellowship". The formula alone spent a third of them on marching and attacking. */
        if ((m.type == M_ATTACK || m.type == M_ARMY) && ((m.die & 0x7F) == F_CHAR || (m.die & 0x7F) == F_SPECIAL)) return -0.3f;
        /* nor on recruiting (against AlphaWOTR they went on elites in the Shire and Ered Luin while Shadow took 8 points by turn 8) */
        if ((m.type == M_MUSTER || m.type == M_MUSTER_ELITE) && (m.die & 0x7F) == F_SPECIAL) { static float w = -1; if (w < 0) { const char *v = getenv("WOTR_WILLM"); w = v ? (float)atof(v) : 0.3f; } return -w; }
        /* attacking out of one's own besieged stronghold gives up the walls: "Defending in siege is by far the most
           effective way to avoid losing your hard acquired troops" (the formula alone did it in most games) */
        if (m.type == M_ATTACK && m.a == m.b && s->siege[m.a] && s->ctrl[m.a] == FP) return -0.4f;
        return 0;
    }
    /* Hunt dice: match the Fellowship's pace. A die beyond the first is worth setting aside up to the number of
       moves the Fellowship made last turn (against a Free Peoples that spends everything on the Ring, one die a turn
       let it walk in by turn 6-9), and is discouraged beyond that (a Shadow that only hunts gives up conquest). */
    if (s->dkind == D_HUNT_ALLOC) {
        /* strength 1.0 since 2026-11-15: against AlphaWOTR, whose Fellowship wins carrying 6-11 corruption, full matching
           took our Shadow from about 3.5 wins in 10 to 11 in 20 (it costs about 2.5 points against our own, slower,
           Free Peoples). WOTR_HUNTX=<n>: match the Fellowship's moves plus n. */
        static float w = -1; static int extra = 0;
        if (w < 0) { const char *v = getenv("WOTR_HUNTN"); w = v ? (float)atof(v) : 1.0f; v = getenv("WOTR_HUNTX"); extra = v ? atoi(v) : 0; }
        float z = 0; for (int k = 2; k <= m.a; k++) z += k <= s->hunt_fp_prev + extra ? -w : 0.15f;
        return z;
    }
    /* Shadow's own strongholds, the mirror of the Free Peoples' rules above: go inside the walls when attacked, do not
       walk out of the region, do not sally from a siege. Against AlphaWOTR 6 of our Shadow's 80 games were lost to a
       Free Peoples military victory: three to six units took Minas Morgul, Dol Guldur, Morannon, Moria and Umbar from
       garrisons that fought in the open first (alpha-619: "siege-retreat 0", then a sally of five regulars). */
    {
        static float w = -1; if (w < 0) { const char *v = getenv("WOTR_SHWALLS"); w = v ? (float)atof(v) : 3.0f; }
        if (s->dkind == D_SIEGE_RETREAT) return m.a == 1 ? -w : 0;
        if (s->dkind == D_RETREAT && m.a != 255 && REGION[s->b.dst].settle == S_STRONGHOLD && s->ctrl[s->b.dst] == SH && m.a != s->b.dst) return w;
        if (s->dkind == D_ACTION && m.type == M_ATTACK && m.a == m.b && s->siege[m.a] && s->ctrl[m.a] == SH) return w * 0.5f;
    }
    /* Home guard (2026-11-17/18; WOTR_HOME=<strength>, default 1; WOTR_HOME_REACH=<regions>, default 1 = an ADJACENT Free Peoples army. At reach 2 it cut stronghold losses by a fifth but slowed the attack by 5 points; at reach 1 it cuts them by two thirds - games lost to raids 10.3% -> 3.4% against a raider - with conquest unchanged): do not march the last unit out of an own city or stronghold
       while a Free Peoples army stands within two regions of it, and put a unit back into one that stands empty under
       such a threat. The new AlphaWOTR won 5 of 40 games by conquest, two of them on turn 6, by walking three regulars
       into an empty Morannon or Minas Morgul and storming a thin Dol Guldur. Worth 2 per victory point (10 if taking
       it would give the Free Peoples their four points), before the nudge share. A training wheel, like the rest. */
    if ((s->dkind == D_ACTION || s->dkind == D_ARMY_MOVE2) && (m.type == M_ARMY || m.type == M_MUSTER || m.type == M_MUSTER_ELITE)) {
        static float w = -1; if (w < 0) { const char *v = getenv("WOTR_HOME"); w = v ? (float)atof(v) : 1.0f; }
        float z = 0; int dst = m.type == M_ARMY ? m.b : m.a;
        if (w > 0 && m.type == M_ARMY) z += w * home_cost(s, m.a, units(s, SH, m.a) - m.c - m.d);
        if (w > 0) z -= w * home_cost(s, dst, units(s, SH, dst));
        if (z != 0) return z;
    }
    /* ... and do not advance out of one after a battle, leaving it short (only the units held back from the attack stay) */
    if (s->dkind == D_ADVANCE && m.a == 1 && s->b.src != s->b.dst) {
        static float w = -1; if (w < 0) { const char *v = getenv("WOTR_HOME"); w = v ? (float)atof(v) : 1.0f; }
        int rear = 0; for (int i = NFPNAT; i < NNAT; i++) rear += s->b.rear_reg[i] + s->b.rear_eli[i];
        return w * home_cost(s, s->b.src, rear);
    }
    if (s->dkind != D_ACTION) return 0;
    if (m.type == M_SKIP && n > 1) return 0.3f;
    /* an attack on a Free Peoples city or stronghold region, and a little more for an assault on a besieged one (a
       siege that is not pressed wins nothing). Not any attack: with a nudge on all of them Shadow threw the Minas
       Morgul garrison at the Gondor army in Ithilien and lost the stronghold to what was left of it. */
    if (m.type == M_ATTACK && feat_front(m.b) == 3) return m.a == m.b ? -0.2f : -0.1f;
    return 0;
}

/* ---- looking further ahead (Agent.depth > 1) ----
   The one-action search above ranks every move by the position it leads to. deep() follows a move further: the other
   side answers with the move it would rank first (or the best of its first `replies`), the side that is choosing then
   considers its `width` best next actions, and so on until it has looked `depth` of its own actions ahead or the turn
   ends; the position reached is scored as usual. The dice of the turn are already on the table, so within a turn this
   is plain planning ("march, join, attack"); battles on the way are fought once, with the same dice for every line.
   Everything between two action decisions (second army moves, casualties, card choices) is settled by its owner's
   first choice. `q` is each option's one-step score from its owner's side, nudges included. */
static float deep(Agent *a, const State *from, int pov, int d, int turn0, int own);
static float pass_d = -2;
static _Thread_local int in_rollout; static int roll_lean = -1;   /* WOTR_ROLL_LEAN=1: inside a rollout both sides' choices get their lean toward the move-suggester too */
static int rank_moves(Agent *a, const State *t, const Move *mv, int n, float *q, int pov, int nar) {      /* returns the index of the best option for the side to decide */
    int who = t->dplayer, bi = -1; float al = a->net[who]->p.alpha;
    /* Agent.narrow: at an action decision inside the look-ahead, score only the moves the move-suggesting network puts
       in its first `narrow` (and the kinds of move it must never drop); the rest are left out of this line of play.
       Scoring all hundred or so at every step is what made looking ahead fifty times dearer than not. */
    static _Thread_local float logit[MAXMOVES]; float kth = -1e30f, lmx = 0; int narrowed = 0, lean = 0;
    if (nar && a->narrow > 0 && t->dkind == D_ACTION && a->dpol[who] && n > a->narrow + 2) {
        static _Thread_local Feat ft; float acc[H1], h[H1];
        feat_encode(t, &ft);
        nn_acc_delta(a->net[who], &a->rc.f, a->acc[who], &ft, acc);
        for (int j = 0; j < H1; j++) h[j] = acc[j] > 0 ? acc[j] : 0;
        pol_logits(a->dpol[who], h, t->dkind, mv, n, logit);
        float top[32]; int k = a->narrow < 32 ? a->narrow : 32, m = 0;
        for (int i = 0; i < n; i++) {
            float x = logit[i]; int j;
            if (m < k) j = m++; else if (x > top[k - 1]) j = k - 1; else continue;
            while (j > 0 && top[j - 1] < x) { top[j] = top[j - 1]; j--; }
            top[j] = x;
        }
        kth = top[m - 1]; narrowed = 1;
        if (roll_lean < 0) roll_lean = getenv("WOTR_ROLL_LEAN") ? atoi(getenv("WOTR_ROLL_LEAN")) : 0;
        if (in_rollout && roll_lean && agent_hum[who] > 0) {
            float mx = -1e30f, z = 0;
            for (int i = 0; i < n; i++) if (logit[i] > mx) mx = logit[i];
            for (int i = 0; i < n; i++) z += expf(logit[i] - mx);
            z = logf(z); lean = 1; lmx = mx + z;
        }
    }
    for (int i = 0; i < n; i++) {
        if (narrowed && logit[i] < kth && !(a->exempt && essential(mv[i].type))) { q[i] = who == FP ? -2e9f : 2e9f; continue; }
        State u = *t; wotr_apply(&u, mv[i]);
        if (u.b.active) finish_battle(&u);
        g_imagined = t->dplayer != pov;
        q[i] = (u.winner ? terminal_value(&u) : agent_value(a, &u)) + nudge_share(al) * lane_guard(t, mv[i], n);
        g_imagined = 0;
        if (lean) { float lp = logit[i] - lmx; if (lp < -6) lp = -6; q[i] += (who == FP ? 1 : -1) * agent_hum[who] * lp; }
        if (bi < 0 || (who == FP ? q[i] > q[bi] : q[i] < q[bi])) bi = i;
    }
    return bi < 0 ? 0 : bi;
}
/* own: how many of pov's own actions lie between the decision being made and this position (1 after its first).
   With Agent.taper the number of moves followed shrinks by that much per action, down to one: wide at first, then a
   single line played out, which is what makes looking all the way to the end of the turn affordable. */
static float deep(Agent *a, const State *from, int pov, int d, int turn0, int own) {
    static _Thread_local Move mvs[8][MAXMOVES]; static _Thread_local float qs[8][MAXMOVES]; static _Thread_local int level;
    State t = *from;
    if (level >= 8) return t.winner ? terminal_value(&t) : agent_value(a, &t);
    Move *mv = mvs[level]; float *q = qs[level];
    for (int guard = 0; guard < 400; guard++) {
        if (t.winner) return terminal_value(&t);
        if (t.b.active) { finish_battle(&t); continue; }
        if (t.turn != turn0) return agent_value(a, &t);                 /* the turn is over: the next one's dice are not known */
        int n = wotr_moves(&t, mv);
        if (n == 1) { wotr_apply(&t, mv[0]); continue; }
        int wide = a->rnd > 0, rn = 0;
        if (t.dkind == D_ACTION && d <= 0 && !wide) return agent_value(a, &t);
        int who = t.dplayer, bi = rank_moves(a, &t, mv, n, q, pov, !wide || d <= 0 || t.dkind != D_ACTION);
        int w = t.dkind != D_ACTION ? 1 : who == pov ? a->width - a->taper * own : a->replies;
        if (wide) {                                                     /* both sides: best + random, one fewer of each per action; then a single line to the end of the turn */
            int L = a->depth - d;
            if (t.dkind != D_ACTION || d <= 0) w = 1;
            else { w = a->width - L; if (w < 1) w = 1; rn = a->rnd - L; if (rn < 0) rn = 0; }
        }
        /* the action that uses the turn's last die runs on into the next turn's roll: give every line the same roll */
        if (t.dkind == D_ACTION) { int left = 0; for (int f = 0; f < NFACE; f++) left += t.dice[FP][f] + t.dice[SH][f]; if (left <= 1) t.rng = 0x51ED270B9F3C7A11ULL ^ (uint64_t)turn0; }
        if (w + rn <= 1) { wotr_apply(&t, mv[bi]); continue; }          /* one line only: follow it */
        float best = who == FP ? -1e9f : 1e9f;
        for (int k = 0; k < w + rn && k < n; k++) {                     /* the w best by one-step score (then rn at random from the rest), each followed further */
            int b = -1;
            if (k < w) { for (int i = 0; i < n; i++) if (q[i] > -1e8f && q[i] < 1e8f && (b < 0 || (who == FP ? q[i] > q[b] : q[i] < q[b]))) b = i; }
            else {
                int left = 0; for (int i = 0; i < n; i++) left += q[i] > -1e8f && q[i] < 1e8f;
                if (!left) break;
                a->rng ^= a->rng << 13; a->rng ^= a->rng >> 7; a->rng ^= a->rng << 17;
                int pick = (int)((a->rng >> 20) % (uint64_t)left);
                for (int i = 0; i < n; i++) if (q[i] > -1e8f && q[i] < 1e8f && pick-- == 0) { b = i; break; }
            }
            if (b < 0) break;
            State u = t; wotr_apply(&u, mv[b]);
            q[b] = who == FP ? -2e9f : 2e9f;                            /* taken */
            level++;
            float v = deep(a, &u, pov, wide || who == pov ? d - 1 : d, turn0, who == pov ? own + 1 : own);
            level--;
            if (who == FP ? v > best : v < best) best = v;
        }
        return best;
    }
    return agent_value(a, &t);
}

/* ---- the rollout chooser (Philip, 2026-11-17): "some sort of truncated rollout function that reliably runs in < 1s on
   a single performance core ... and goes to end of turn, sampling a variety of moves" ----
   One rollout: from a position, both sides play on to the end of the turn with real dice; at each choice the side to
   decide scores its options one step ahead (at action decisions only the move-suggesting network's first `narrow`)
   and picks among them at random, the better the likelier (softmax, temperature tau in log-odds), so that many rollouts
   of one move cover many different continuations. The board where the turn ends is scored by the value network.
   Returns the probability that the Free Peoples win. */
long agent_roll_count, agent_roll_decisions; double agent_roll_ms_max, agent_roll_ms_sum;
#include <time.h>
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static float rollout(Agent *a, const State *from, int pov, int turn0, float tau) {
    static _Thread_local Move mv[MAXMOVES]; static _Thread_local float q[MAXMOVES];
    State t = *from;
    for (int guard = 0; guard < 600; guard++) {
        if (t.winner) return nn_prob(terminal_value(&t));
        if (t.b.active && t.dkind == D_NONE) { finish_battle(&t); continue; }
        if (t.turn > turn0 + a->roll_turns) return nn_prob(agent_value(a, &t));        /* the turn is over (roll_turns: play that many further turns as well) */
        int n = wotr_moves(&t, mv);
        if (n == 1) { wotr_apply(&t, mv[0]); continue; }
        in_rollout = 1; int who = t.dplayer, bi = rank_moves(a, &t, mv, n, q, pov, 1), pick = bi; in_rollout = 0;
        if (tau > 0) {
            double tot = 0, w[MAXMOVES];
            for (int i = 0; i < n; i++) { float d = who == FP ? q[bi] - q[i] : q[i] - q[bi]; w[i] = d > 8 * tau ? 0 : exp(-(double)d / tau); tot += w[i]; }
            double x = unif(&a->rng) * tot;
            for (int i = 0; i < n; i++) { x -= w[i]; if (w[i] > 0 && x <= 0) { pick = i; break; } }
        }
        wotr_apply(&t, mv[pick]);
    }
    return nn_prob(agent_value(a, &t));
}
/* Chooses among the `nk` shortlisted root moves kept[] by rollouts under a time budget (a->roll_ms, on this thread) or a
   rollout budget (a->roll_n). Every candidate gets a batch of rollouts with the SAME dice seeds as the others; the worse
   half is dropped and the batch doubled, until one is left or the budget is spent. val[] of the finalists becomes the
   log-odds of their average; cand[] is set to the finalists. Returns the best. */
static int roll_choose(Agent *a, const State *det, const Move *mv, int n, int pov, uint64_t seed, int *kept, int nk, float *val, uint8_t *cand) {
    double t0 = now_ms(), sum[64]; int cnt[64], alive[64], na = nk < 64 ? nk : 64;
    for (int k = 0; k < na; k++) { sum[k] = 0; cnt[k] = 0; alive[k] = k; }
    int over = 0, saved_narrow = a->narrow, total = 0;
    if (a->narrow <= 0) a->narrow = 6;
    /* one rollout for candidate k; its j-th rollout has the same dice as every other candidate's j-th */
    #define ROLL1(k) do { State u = *det; u.rng = seed + (uint64_t)cnt[k] * 0x632BE59BD9B4E019ULL; wotr_apply(&u, mv[kept[k]]); \
                          sum[k] += rollout(a, &u, pov, det->turn, a->roll_tau); cnt[k]++; total++; \
                          over = (a->roll_ms > 0 && now_ms() - t0 >= a->roll_ms) || (a->roll_n > 0 && total >= a->roll_n); } while (0)
    /* The budget is planned, not just spent (2026-11-17: cut off in mid-comparison with a few rollouts a move, the
       chooser was worth 4 points; allowed to finish, 23). Two rollouts a candidate show what one costs here (early in a
       turn far more than late); from that, how many fit in the time; that number is split evenly over the halving
       rounds (drop the worse half after each), so the comparison always runs to its end. */
    for (int r = 0; r < 2 && !over; r++) for (int j = 0; j < na && !over; j++) ROLL1(alive[j]);
    long N = a->roll_n > 0 ? a->roll_n : 1000000;
    if (a->roll_ms > 0 && total > 0) { double per = (now_ms() - t0) / total; long fit = per > 0 ? (long)(a->roll_ms * 0.97 / per) : N; if (fit < N) N = fit; }
    int rounds = 0; for (int x = na; x > 1; x = (x + 1) / 2) rounds++;
    for (int rd = 0; rd < rounds && !over && na > 1; rd++) {
        long left = N - total, m = left / ((long)(rounds - rd) * na);
        if (rd == 0) m = m > 0 ? m : 0; else if (m < 1) m = 1;
        for (long r = 0; r < m && !over; r++) for (int j = 0; j < na && !over; j++) ROLL1(alive[j]);
        if (over && rd == 0 && cnt[alive[na - 1]] < 2) break;
        for (int i = 0; i < na; i++) for (int j = i + 1; j < na; j++) {          /* best first, by average so far */
            double mi = sum[alive[i]] / (cnt[alive[i]] ? cnt[alive[i]] : 1), mj = sum[alive[j]] / (cnt[alive[j]] ? cnt[alive[j]] : 1);
            if (pov == FP ? mj > mi : mj < mi) { int x = alive[i]; alive[i] = alive[j]; alive[j] = x; }
        }
        na = (na + 1) / 2;
    }
    #undef ROLL1
    a->narrow = saved_narrow;
    memset(cand, 0, (size_t)n);
    int bi = -1; float best = 0;
    for (int j = 0; j < na; j++) {
        int k = alive[j]; if (!cnt[k]) continue;
        double p = sum[k] / cnt[k]; if (p < 1e-4) p = 1e-4; if (p > 1 - 1e-4) p = 1 - 1e-4;
        int i = kept[k]; val[i] = (float)log(p / (1 - p)) + nudge_share(a->net[pov]->p.alpha) * lane_guard(det, mv[i], n); cand[i] = 1;
        /* only candidates that had as many rollouts as the leader are compared (a cut-off batch leaves some short) */
        if (bi < 0 || (pov == FP ? val[i] > best : val[i] < best)) { best = val[i]; bi = i; }
    }
    double ms = now_ms() - t0;
    __atomic_add_fetch(&agent_roll_count, total, __ATOMIC_RELAXED); __atomic_add_fetch(&agent_roll_decisions, 1, __ATOMIC_RELAXED);
    if (ms > agent_roll_ms_max) agent_roll_ms_max = ms;
    agent_roll_ms_sum += ms;
    return bi;
}

/* ---- Gumbel search at the root (Danihelka et al. 2022, "Policy improvement by planning with Gumbel") ----
   WOTR_GUM_FP / WOTR_GUM_SH = "n,k,scale,noise,prior": at an action decision of that side, n evaluations are spent on k
   moves. The k are the moves with the largest (log-probability the move-suggesting network gives them + noise * a Gumbel
   random number); the worse half is dropped after each of log2(k) equal rounds; moves are ranked, and the survivor
   chosen, by  prior + noise + (50 + most evaluations any move has had) * scale * (its average chance of winning).
   One evaluation plays the move with its own dice (the j-th of every move has the same dice) and scores what follows:
   by the look-ahead when Agent.depth > 1, else one step ahead. prior: 0 the self-play suggester, 1 the taught one.
   extra[] (nudges and the lean, log-odds, Free Peoples' view) is added to each evaluation. On return cand[] marks the
   k moves and val[] holds the log-odds of their averages; the index of the chosen move is returned. */
static float gum_cfg[2][5] = {{0, 8, 1, 0, 0}, {0, 8, 1, 0, 0}};
static void gum_init(void) {
    static int read; if (read) return;
    const char *v;
    if ((v = getenv("WOTR_GUM_FP"))) sscanf(v, "%f,%f,%f,%f,%f", &gum_cfg[FP][0], &gum_cfg[FP][1], &gum_cfg[FP][2], &gum_cfg[FP][3], &gum_cfg[FP][4]);
    if ((v = getenv("WOTR_GUM_SH"))) sscanf(v, "%f,%f,%f,%f,%f", &gum_cfg[SH][0], &gum_cfg[SH][1], &gum_cfg[SH][2], &gum_cfg[SH][3], &gum_cfg[SH][4]);
    read = 1;
}
long agent_gum_evals, agent_gum_decisions;
static int gumbel_choose(Agent *a, const State *det, const Move *mv, int n, int pov, uint64_t seed, const float *lp, const float *extra, float *val, uint8_t *cand) {
    const float *c = gum_cfg[pov];
    int N = (int)c[0], k = (int)c[1], idx[64], cnt[64], na = 0, total = 0; double sum[64]; float base[64];
    if (k > 64) k = 64;
    if (k < 2) k = 2;
    for (int i = 0; i < n; i++) {                                      /* the k largest of prior + noise, best first */
        if (!cand[i]) continue;
        double u = unif(&a->rng); if (u < 1e-12) u = 1e-12;
        float x = lp[i] + (c[3] > 0 ? c[3] * (float)-log(-log(u)) : 0); int j;
        if (na < k) j = na++; else if (x > base[k - 1]) j = k - 1; else continue;
        while (j > 0 && base[j - 1] < x) { base[j] = base[j - 1]; idx[j] = idx[j - 1]; j--; }
        base[j] = x; idx[j] = i;
    }
    int nk = na, rounds = 0; for (int x = na; x > 1; x = (x + 1) / 2) rounds++;
    for (int j = 0; j < nk; j++) { sum[j] = 0; cnt[j] = 0; }
    int alive[64]; for (int j = 0; j < nk; j++) alive[j] = j;
    for (int rd = 0; rd < rounds; rd++) {
        int m = N / (rounds * na); if (m < 1) m = 1;
        for (int r = 0; r < m; r++) for (int q = 0; q < na; q++) {
            int j = alive[q], i = idx[j]; float v; uint64_t sd = seed + (uint64_t)cnt[j] * 0x632BE59BD9B4E019ULL;
            if (a->depth > 1) { State u = *det; u.rng = sd; wotr_apply(&u, mv[i]); v = deep(a, &u, pov, a->depth - 1, det->turn, 1); }
            else v = eval_move(a, det, mv[i], pov, 0, sd, 1);
            double p = 1.0 / (1.0 + exp(-(double)(v + extra[i])));
            sum[j] += pov == FP ? p : 1 - p; cnt[j]++; total++;
        }
        int mc = 0; for (int q = 0; q < na; q++) if (cnt[alive[q]] > mc) mc = cnt[alive[q]];
        #define GSC(j) (base[j] + (50.0f + (float)mc) * c[2] * (float)(sum[j] / cnt[j]))
        for (int x = 0; x < na; x++) for (int y = x + 1; y < na; y++) if (GSC(alive[y]) > GSC(alive[x])) { int t = alive[x]; alive[x] = alive[y]; alive[y] = t; }
        #undef GSC
        na = (na + 1) / 2;
    }
    memset(cand, 0, (size_t)n);
    for (int j = 0; j < nk; j++) {
        double p = sum[j] / cnt[j]; if (pov != FP) p = 1 - p;
        if (p < 1e-4) p = 1e-4; if (p > 1 - 1e-4) p = 1 - 1e-4;
        val[idx[j]] = (float)log(p / (1 - p)); cand[idx[j]] = 1;
    }
    __atomic_add_fetch(&agent_gum_evals, total, __ATOMIC_RELAXED); __atomic_add_fetch(&agent_gum_decisions, 1, __ATOMIC_RELAXED);
    return idx[alive[0]];
}

Move agent_choose(Agent *a, const State *s, float *best_v, int *n_moves) {
    Move *mv = a->last_mv; float *val = a->last_val;
    int n = wotr_moves(s, mv), pov = s->dplayer;
    if (n_moves) *n_moves = n;
    a->last_deep = 0;
    a->last_n = n; a->last_full = 0; a->last_hit = 0; a->last_tier = 0; a->last_explored = 0; a->last_explore_cost = 0; a->last_top1 = 0;
    if (n == 1) { if (best_v) *best_v = 0.0f; return mv[0]; }
    a->cur_kind = s->dkind; a->dec_kind[s->dkind]++;
    State det = *s;
    wotr_determinize(&det, pov, next(&a->rng));
    uint64_t seed = next(&a->rng);
    feat_cache(&det, &a->rc); a->root = &det;
    /* first-layer sums: update the previous decision's instead of rebuilding (rebuilt every 64 to stop drift) */
    int sg = spec_group(s->dkind), set = sg >= 0 && a->spec[sg][FP] ? 1 + sg : 0;       /* which networks score this decision: 0 the main pair, else a specialist pair */
    a->vnet[FP] = set ? a->spec[sg][FP] : a->net[FP]; a->vnet[SH] = set ? a->spec[sg][SH] : a->net[SH];
    AccCache *pc = &a->cache[set];
    for (int k = 0; k < 2; k++) {
        if (pc->age > 0 && pc->age < 64) nn_acc_delta(a->vnet[k], &pc->root, pc->acc[k], &a->rc.f, a->acc[k]);
        else nn_root(a->vnet[k], &a->rc.f, a->acc[k]);
        memcpy(pc->acc[k], a->acc[k], sizeof a->acc[k]);
    }
    pc->age = pc->age >= 64 ? 1 : pc->age + 1;
    pc->root.n = a->rc.f.n;
    memcpy(pc->root.idx, a->rc.f.idx, sizeof(uint16_t) * (size_t)a->rc.f.n);
    memcpy(pc->root.val, a->rc.f.val, sizeof(float) * (size_t)a->rc.f.n);
    a->has_root = 1;
    /* candidate selection */
    uint8_t cand[MAXMOVES];
    int topk = a->topk, nrand = a->nrand;
    int use_pol = a->pol[pov] && !set && n > topk + nrand, full = !use_pol || unif(&a->rng) < a->full_frac;
    a->last_tier = !use_pol ? 0 : full ? 1 : 2;
    if (use_pol && !full && a->fastk && unif(&a->rng) < a->fast_frac) { topk = a->fastk; nrand = 1; a->last_tier = 3; }
    float logit[MAXMOVES], kth = -1e30f;
    if (use_pol) {
        float h[H1];
        for (int j = 0; j < H1; j++) h[j] = a->acc[pov][j] > 0 ? a->acc[pov][j] : 0;
        pol_logits(a->pol[pov], h, s->dkind, mv, n, logit);
        /* many moves get exactly the same logit (the same march with different numbers of units, say), and "every move
           at least as good as the k-th" then let about thirty through where eight were asked for. A tiny random amount
           breaks the ties, so that k means k. */
        if (a->exact) for (int i = 0; i < n; i++) logit[i] += 1e-4f * (float)unif(&a->rng);
        float top[64]; int k = topk < 64 ? topk : 64, m = 0;       /* k largest logits, kept sorted */
        for (int i = 0; i < n; i++) {
            float x = logit[i]; int j;
            if (m < k) j = m++; else if (x > top[k - 1]) j = k - 1; else continue;
            while (j > 0 && top[j - 1] < x) { top[j] = top[j - 1]; j--; }
            top[j] = x;
        }
        kth = top[m - 1];
    }
    if (full) memset(cand, 1, (size_t)n);
    else {
        for (int i = 0; i < n; i++) cand[i] = logit[i] >= kth || (a->exempt && s->dkind == D_ACTION && essential(mv[i].type));
        for (int r = 0; r < nrand; r++) cand[(int)(unif(&a->rng) * n)] = 1;
    }
    /* agent_hum[side] (WOTR_HUMAN_FP/SH=<w>): at an action decision every move's score gets w times the log of
       the probability the move-suggesting network gives it (floored at -6): with a network taught from human moves
       (`wotr imitate`) this leans the choice toward what human players do, a learned stand-in for the strategy formula. */
    static _Thread_local float hum[MAXMOVES]; int use_hum = 0;
    static float hum_early; static int hum_early_turns; static float hum_early_fp; static int hum_early_fp_turns;
    {   /* WOTR_HUMAN_FP / WOTR_HUMAN_SH are read here, once, so that training games and the referee's bridge get the lean too */
        static int read; if (!read) { const char *v; if ((v = getenv("WOTR_HUMAN_FP")) && agent_hum[FP] == 0) agent_hum[FP] = (float)atof(v); if ((v = getenv("WOTR_HUMAN_SH")) && agent_hum[SH] == 0) agent_hum[SH] = (float)atof(v);
            if ((v = getenv("WOTR_HUMAN_EARLY"))) sscanf(v, "%f,%d", &hum_early, &hum_early_turns);
            if ((v = getenv("WOTR_HUMAN_EARLY_FP"))) sscanf(v, "%f,%d", &hum_early_fp, &hum_early_fp_turns);   /* the same for the Free Peoples */   /* Shadow's lean is <w> instead on turns up to <turns> */
            read = 1; }
    }
    if (agent_hum[pov] > 0 && s->dkind == D_ACTION && a->dpol[pov] && !set) {
        float h[H1], mx = -1e30f, z = 0;
        for (int j = 0; j < H1; j++) h[j] = a->acc[pov][j] > 0 ? a->acc[pov][j] : 0;
        pol_logits(a->dpol[pov], h, s->dkind, mv, n, hum);
        for (int i = 0; i < n; i++) if (hum[i] > mx) mx = hum[i];
        for (int i = 0; i < n; i++) z += expf(hum[i] - mx);
        z = logf(z);
        float w = pov == SH && s->turn <= hum_early_turns ? hum_early : pov == FP && s->turn <= hum_early_fp_turns ? hum_early_fp : agent_hum[pov];
        for (int i = 0; i < n; i++) { float lp = hum[i] - mx - z; if (lp < -6) lp = -6; hum[i] = (pov == FP ? 1 : -1) * w * lp; }
        use_hum = 1;
    }
    /* agent_probe (WOTR_PROBE_ELITE=<x>, a measuring probe, not a keeper): Shadow's elite musters score x log-odds better.
       Humans spend 15-19% of Shadow's dice on them from turn 5, we 3-4%; this asks whether more of them would win more. */
    if (agent_probe > 0 && pov == SH && s->dkind == D_ACTION && !set) {
        if (!use_hum) memset(hum, 0, sizeof(float) * (size_t)n);
        for (int i = 0; i < n; i++) if (mv[i].type == M_MUSTER_ELITE || (mv[i].type == M_VOICE && mv[i].a == 1)) hum[i] -= agent_probe;
        use_hum = 1;
    }
    /* agent_probe_siege (WOTR_PROBE_SIEGE=<x>, a measuring probe): Shadow's assault on a besieged stronghold with no elite
       in the army (one combat round, hitting on sixes against fives) against three or more defenders scores x worse. */
    if (agent_probe_siege > 0 && pov == SH && s->dkind == D_ACTION && !set) {
        if (!use_hum) memset(hum, 0, sizeof(float) * (size_t)n);
        for (int i = 0; i < n; i++) if (mv[i].type == M_ATTACK && mv[i].a == mv[i].b && s->siege[mv[i].a] && s->ctrl[mv[i].a] == FP
                                        && !(s->eli[mv[i].a][ISENGARD] + s->eli[mv[i].a][SAURON] + s->eli[mv[i].a][SOUTHRON]) && units(s, FP, mv[i].a) >= 3) hum[i] += agent_probe_siege;
        use_hum = 1;
    }
    float best = pov == FP ? -1e9f : 1e9f; int bi = -1;
    a->last_ncand = 0;
    for (int i = 0; i < n; i++) {
        if (!cand[i]) continue;
        a->last_ncand++;
        val[i] = eval_move(a, &det, mv[i], pov, 0, seed, a->samples) + nudge_share(a->vnet[pov]->p.alpha) * lane_guard(s, mv[i], n);
        if (use_hum) val[i] += hum[i];
        if (bi < 0 || (pov == FP ? val[i] > best : val[i] < best)) { best = val[i]; bi = i; }
    }
    /* WOTR_PASS=<d> (2026-11-18): a pass keeps the die for later in the turn, which a one-step score cannot see - it scores
       the pass as if the action were given up (0.34 log-odds below our choice, where AlphaWOTR's Free Peoples pass a fifth
       of their early decisions). With this set, a pass is worth what the best other move is worth, less d. */
    if (pass_d < -1) pass_d = getenv("WOTR_PASS") ? (float)atof(getenv("WOTR_PASS")) : -1;
    if (pass_d >= 0 && s->dkind == D_ACTION && !set) {
        int pi = -1, bo = -1;
        for (int i = 0; i < n; i++) { if (!cand[i]) continue; if (mv[i].type == M_PASS) pi = i; else if (bo < 0 || (pov == FP ? val[i] - (use_hum ? hum[i] : 0) > val[bo] - (use_hum ? hum[bo] : 0) : val[i] - (use_hum ? hum[i] : 0) < val[bo] - (use_hum ? hum[bo] : 0))) bo = i; }
        if (pi >= 0 && bo >= 0) {
            val[pi] = val[bo] - (use_hum ? hum[bo] : 0) + (pov == FP ? -pass_d : pass_d) + (use_hum ? hum[pi] : 0);
            bi = -1; for (int i = 0; i < n; i++) if (cand[i] && (bi < 0 || (pov == FP ? val[i] > val[bi] : val[i] < val[bi]))) bi = i;
            best = val[bi];
        }
    }
    int rolled = 0;
    gum_init();
    if (gum_cfg[pov][0] > 0 && s->dkind == D_ACTION && !set && a->last_ncand > 1) {
        static _Thread_local float lp[MAXMOVES], extra[MAXMOVES];
        const Policy *P = gum_cfg[pov][4] > 0 ? (a->dpol[pov] ? a->dpol[pov] : a->pol[pov]) : (a->pol[pov] ? a->pol[pov] : a->dpol[pov]);
        if (P) {
            float h[H1], mx = -1e30f, z = 0;
            for (int j = 0; j < H1; j++) h[j] = a->acc[pov][j] > 0 ? a->acc[pov][j] : 0;
            pol_logits(P, h, s->dkind, mv, n, lp);
            for (int i = 0; i < n; i++) if (lp[i] > mx) mx = lp[i];
            for (int i = 0; i < n; i++) z += expf(lp[i] - mx);
            z = mx + logf(z);
            for (int i = 0; i < n; i++) lp[i] -= z;
        } else memset(lp, 0, sizeof(float) * (size_t)n);
        for (int i = 0; i < n; i++) extra[i] = nudge_share(a->vnet[pov]->p.alpha) * lane_guard(s, mv[i], n) + (use_hum ? hum[i] : 0);
        bi = gumbel_choose(a, &det, mv, n, pov, seed, lp, extra, val, cand); best = val[bi];
        rolled = 1; a->last_deep = 1; memcpy(a->last_followed, cand, (size_t)n);
    }
    if (!rolled && (a->roll_ms > 0 || a->roll_n > 0) && s->dkind == D_ACTION && !set) {
        /* the rollout chooser: shortlist the best `roll_k` moves by the one-step scores, then let rollouts decide */
        int kept[64], nk = 0, w = a->roll_k > 0 && a->roll_k <= 64 ? a->roll_k : 12;
        uint8_t used[MAXMOVES]; memset(used, 0, (size_t)n);
        for (; nk < w; nk++) {
            int b = -1;
            for (int i = 0; i < n; i++) if (cand[i] && !used[i] && (b < 0 || (pov == FP ? val[i] > val[b] : val[i] < val[b]))) b = i;
            if (b < 0) break;
            used[b] = 1; kept[nk] = b;
        }
        if (nk > 1) {
            bi = roll_choose(a, &det, mv, n, pov, seed, kept, nk, val, cand); rolled = 1; a->last_deep = 1; memcpy(a->last_followed, cand, (size_t)n);
            /* the lean toward the taught move-suggester counts among the rollouts' finalists too (without it the rollouts
               undid the AlphaWOTR lessons: Minas Tirith fell on turn 7.8 again) */
            if (use_hum) { for (int i = 0; i < n; i++) if (cand[i]) { val[i] += hum[i]; if (pov == FP ? val[i] > val[bi] : val[i] < val[bi]) bi = i; } }
            best = val[bi];
        }
    }
    /* Agent.deep_gap (WOTR_DEEP_GAP in training): look ahead only when the choice is close - when the best move by the
       one-step scores is ahead of the second best by less than this (log-odds). A clear choice is taken as it is, which
       is what makes look-ahead through the whole of a training game affordable. */
    int clear = 0;
    if (a->deep_gap > 0 && a->depth > 1 && s->dkind == D_ACTION) {
        float second = pov == FP ? -1e9f : 1e9f;
        for (int i = 0; i < n; i++) if (cand[i] && i != bi && (pov == FP ? val[i] > second : val[i] < second)) second = val[i];
        clear = (pov == FP ? best - second : second - best) > a->deep_gap;
    }
    if (!rolled && !clear && a->depth > 1 && s->dkind == D_ACTION && a->width > 0) {
        /* the `width` best moves by the one-action search are each followed further and the choice is made among them */
        float al = a->net[pov]->p.alpha; int kept[32], nk = 0, w = a->width < 32 ? a->width : 32;
        uint8_t used[MAXMOVES]; memset(used, 0, (size_t)n);
        for (; nk < w; nk++) {
            int b = -1;
            for (int i = 0; i < n; i++) if (cand[i] && !used[i] && (b < 0 || (pov == FP ? val[i] > val[b] : val[i] < val[b]))) b = i;
            if (b < 0) break;
            used[b] = 1; kept[nk] = b;
        }
        /* agent_flank (WOTR_PROBE_FLANK=<k>, a probe): also follow the k best marches that go ROUND a blocked road. The
           look-ahead follows only the best four moves by the one-step score, and the first step of a flank looks like
           walking away, so the three-step way AlphaWOTR takes Minas Tirith was never looked at. */
        for (int k = 0; k < agent_flank && pov == SH && nk < 32; k++) {
            int b = -1;
            for (int i = 0; i < n; i++) if (cand[i] && !used[i] && mv[i].type == M_ARMY && goes_round(s, mv[i]) && (b < 0 || val[i] < val[b])) b = i;
            if (b < 0) break;
            used[b] = 1; kept[nk++] = b;
        }
        for (int k = 0; k < a->rnd && nk < 32; k++) {                   /* wide look-ahead: some picked at random from the rest */
            int left = 0; for (int i = 0; i < n; i++) left += cand[i] && !used[i];
            if (!left) break;
            if (!a->rng) a->rng = seed | 1;
            a->rng ^= a->rng << 13; a->rng ^= a->rng >> 7; a->rng ^= a->rng << 17;
            int pick = (int)((a->rng >> 20) % (uint64_t)left);
            for (int i = 0; i < n; i++) if (cand[i] && !used[i] && pick-- == 0) { used[i] = 1; kept[nk++] = i; break; }
        }
        best = pov == FP ? -1e9f : 1e9f; bi = -1;
        for (int k = 0; k < nk; k++) {
            int i = kept[k]; float sum = 0; int m = 0, ns = mv[i].type == M_ATTACK && a->samples < 3 ? 3 : a->samples; if (mv[i].type == M_ATTACK && ns < agent_bsamp) ns = agent_bsamp;
            for (; m < ns; m++) {
                State u = det; u.rng = seed + (uint64_t)m * 0x632BE59BD9B4E019ULL;
                uint32_t nr = u.nrand;
                wotr_apply(&u, mv[i]);
                sum += deep(a, &u, pov, a->depth - 1, det.turn, 1);
                if (u.nrand == nr) { m++; break; }                      /* no dice in this move: once is enough */
            }
            val[i] = sum / (float)m + nudge_share(al) * lane_guard(s, mv[i], n);
            if (use_hum) val[i] += hum[i];
            if (bi < 0 || (pov == FP ? val[i] > best : val[i] < best)) { best = val[i]; bi = i; }
        }
        for (int i = 0; i < n; i++) cand[i] = used[i];                  /* exploration, too, chooses among the moves that were followed */
        a->last_deep = 1; memcpy(a->last_followed, used, (size_t)n);
    }
    a->has_root = 0;
    a->last_full = full && use_pol; a->last_hit = 0; a->last_regret = 0;
    if (a->last_full) {
        int ge = 0; float bk = pov == FP ? -1e9f : 1e9f;
        for (int i = 0; i < n; i++) {
            int in = logit[i] >= kth || (a->exempt && s->dkind == D_ACTION && essential(mv[i].type));
            ge += logit[i] >= logit[bi]; if (in && (pov == FP ? val[i] > bk : val[i] < bk)) bk = val[i];
        }
        a->last_hit = ge <= a->topk;                           /* ties (an untrained policy) do not count as hits */
        a->last_top1 = ge == 1;
        a->last_regret = pov == FP ? best - bk : bk - best;
    }
    if (best_v) *best_v = best - nudge_share(a->vnet[pov]->p.alpha) * lane_guard(s, mv[bi], n) - (use_hum ? hum[bi] : 0);       /* what is learned from stays free of the nudge and of the lean */
    if (a->temp > 0) {
        double tot = 0, w[MAXMOVES];
        for (int i = 0; i < n; i++) { w[i] = cand[i] ? exp((pov == FP ? val[i] - best : best - val[i]) / a->temp) : 0; tot += w[i]; }
        double x = unif(&a->rng) * tot;
        for (int i = 0; i < n; i++) { x -= w[i]; if (cand[i] && x <= 0) { a->last_explored = i != bi; a->last_explore_cost = pov == FP ? best - val[i] : val[i] - best; return mv[i]; } }
    }
    return mv[bi];
}
