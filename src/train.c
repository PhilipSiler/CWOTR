/* train.c -- self-play training, evaluation matches, and a move-by-move game printer
 *   wotr train <outdir> [rounds=120] [games/round=2000] [threads=9] [fade=480]
 *   wotr eval <netA|prior> <netB|prior> [games] [threads]  A plays Free Peoples, B plays Shadow ("prior" = untrained)
 *   wotr show <net|prior> <seed> [v] [shadow net]
 *
 * Three things happen in training, and nothing else (see whattodo.txt):
 *   1. The hand-written strategy formula is the starting point, and fades slowly: a position's score is
 *      alpha * formula + (1 - alpha) * network, with alpha falling in a straight line from 1 to 0 over "fade" rounds.
 *   2. The network learns from the games it plays -- the real result, plus small partial rewards for progress made in
 *      the next few turns (see play_game) -- and so takes over as the formula fades.
 *   3. Earlier versions of the network, and the untrained formula, are mixed in as opponents.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include "agent.h"

/* WOTR_RECORD="<SH|FP>,<file prefix>": in readings (eval), write that side's action decisions as lesson records (the true
   State, then the Move played: what WOTR_AWLES_* reads), one file per thread <prefix>.<thread>. For keeping a strong
   player's own look-ahead choices in a later run's fit of the move suggester (2026-11-21). */
static const char *g_rec; static int g_rec_side;
static const char *g_falls;        /* WOTR_FALLS=<file prefix>: dump settlement pictures and what became of them, for fitting the fall forecast on our own games */
static float g_floor = 0.0f;       /* WOTR_FLOOR=<x>: the strategy formula's share never falls below this (below about 0.3 Shadow's conquest play slid in every run so far) */
/* Starting positions from human games (WOTR_STARTS=<file written by tools/humanlogs/starts.py>, WOTR_START_SHARE=<x>):
   a share of the training games begin from the board a human game had reached, so both sides meet positions they
   never create themselves (a ten-unit army beside Lórien on turn 3). Evaluation games always start from turn 1. */
static State *g_starts; static int g_nstarts; static float g_start_share = 0.5f;
static int g_eval_starts;      /* `wotr eval` with WOTR_STATES set: every test game starts from one of those boards (e.g. AlphaWOTR's Shadow already massed on Gondor) */
static int starts_load(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[256]; State s; int bad = 0, cap = 0;
    wotr_blank(&s);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "end", 3)) { wotr_set(&s, line); continue; }
        char err[200];
        if (wotr_begin_turn(&s, 12345 + (uint64_t)g_nstarts, err, sizeof err)) bad++;
        else {
            if (g_nstarts == cap) { cap = cap ? cap * 2 : 4096; g_starts = realloc(g_starts, (size_t)cap * sizeof(State)); }
            g_starts[g_nstarts++] = s;
        }
        wotr_blank(&s);
    }
    fclose(f);
    printf("starting positions: %d loaded from %s, %d rejected by the position checker\n", g_nstarts, path, bad);
    return 0;
}
/* WOTR_BALANCE=<x> (58): when either side has lately won more than x per cent of the training games, only the other
   side improves, until the first is down to 50 per cent or less. The seat of the side that is ahead is taken, in every training game,
   by a frozen copy of the player as it stood at that moment; the player in training plays the side that is behind.
   (Left to themselves the Free Peoples learn faster, and Shadow gives up conquest before it has learned it.) */
static int g_topk[2] = {-1, -1};    /* WOTR_TOPK_FP / WOTR_TOPK_SH: that side scores only the suggesting network's first so many moves (0: all; unset: 8 in training, all in eval) */
static float g_full = 0.1f;         /* WOTR_FULL=<x>: share of the larger decisions at which every move is scored (they teach the suggesting network) */
static int g_exact[2];              /* WOTR_EXACT="fp,sh" (0/1 each): 1 = exactly the first k moves are scored (ties broken at random) */
static int g_exempt[2] = {1, 1};    /* WOTR_EXEMPT="fp,sh" (0/1 each): 0 = attacks, card plays and the like are no longer scored regardless of the suggesting network */            /* WOTR_EXEMPT=0: attacks, card plays and the like are no longer scored regardless of the suggesting network */
static int g_deep[2][6];            /* WOTR_DEEP_FP / WOTR_DEEP_SH="depth,width,replies": that side's player looks ahead (eval and training games alike) */
/* Teaching the network what the look-ahead sees (WOTR_TEACH="depth,width,replies,share[,part]"): at the action decisions
   that are sampled for training, the move is chosen with the look-ahead (deep() in agent.c), and the position's
   target becomes  (1 - share) x the usual one (result + partial rewards)  +  share x the look-ahead's estimate.
   The games are otherwise played by the quick one-action search, so the cost is a few look-aheads a game. */
static int g_teach[3]; static float g_teach_share = 0, g_teach_part = 1; static int g_teach_pol;     /* part: the fraction of sampled action decisions that get it (a fifth number in WOTR_TEACH), to keep a round short */
/* Specialist networks (WOTR_SPEC=1; see spec_group in agent.c). Each family's pair starts as a copy of the main pair
   and is then trained only on positions that follow one of its decisions, toward the search's estimate at the first
   decision of the following turn (the real result if the game ends first). Their formula share, which also scales
   the hand-written nudges for those decisions, falls in a straight line to nothing over WOTR_SPEC_FADE rounds (480)
   from the round they were switched on (kept in <dir>/spec_start). Files: <player>.s<family><f|s>. */
static int g_spec = 0; static float g_spec_fade = 480;
/* The eight outcome modifiers (Philip, 2026-11-20), the ONLY reward switch: WOTR_REW="FPMV,FPRV,FPML,FPRL,SPMV,SPRV,SPML,SPRL".
   A victory is worth +modifier and a loss -modifier to the side concerned, on a scale where a plain win is +1 and a plain
   loss -1 (all eight at 1 = the plain game). FPMV/FPRV: the Free Peoples win by military victory / by the Ring; FPML/FPRL:
   they lose to Shadow's conquest / to corruption. SPMV/SPRV: Shadow wins by conquest / by corruption; SPML/SPRL: Shadow
   loses to a Free Peoples military victory / to the Ring. Each side has its own network, so the two halves need not mirror
   each other. The same values score a finished game inside the player's move choice (agent_out). Kept per side as the
   chance-like number the networks use, (1 + value) / 2: [side][its main win, its other win, ...]. The earlier switches
   (WOTR_RINGWIN, CORRWIN, SPEED, RINGFIRST, SOON, OUT_FP/OUT_SH) are gone: each is a special case of these eight. */
static float g_out[2][4] = {{1, 1, 0, 0}, {1, 1, 0, 0}}; static int g_out_on;
static float g_rteach[5] = {0, 12, 0.25f, 0.5f, SH};
static float g_roll[2][5] = {{0, 0, 0.15f, 12, 0}, {0, 0, 0.15f, 12, 0}};   /* WOTR_ROLL_FP / WOTR_ROLL_SH="ms,rollouts,tau,shortlist,further turns": the rollout chooser for that side in play (not in training games) */
/* WOTR_STYLE="share,spread": in that share of training games Shadow plays the whole game in a random style: each term
   of the strategy formula (bar the constant) is multiplied by exp(spread x a normal draw), fixed for the game. */
static float g_style = 0, g_style_s = 0.7f;
static float g_balance = 0;
/* WOTR_ABSORB=<x>: the network's target becomes (1 - x) x the usual one + x x the value the one-step search gave the
   position, which includes the strategy formula's share. The network alone cannot conquer (played with no formula,
   Shadow's conquest wins fall from 60% to 3%): trained only on results, it never picked up the formula's small
   step-by-step preferences. This lets it absorb them, so that the formula's share can then really be faded out. */
static float g_absorb = 0; static float g_lambda = 0;
static int g_dopen = 0; static float g_dgap = 0;      /* WOTR_DEEP_GAP=<x>: with WOTR_DEEP_OPEN, look ahead only where the two best one-step scores are within x log-odds */
static float g_open = 0;            /* WOTR_OPEN=<x>: Shadow's exploration setting on turns 1-3 of training games (0: no different from later turns) */
static float g_temp_sh = 0;         /* WOTR_TEMP_SH=<x>: Shadow's own exploration setting in training games (0: the same as the Free Peoples') */
/* WOTR_STATES=<file>: starting positions saved raw by the referee in CWOTR/proto (one State per record, taken at the
   first decision of a turn in games against AlphaWOTR). WOTR_STATES_TURNS="a,b" keeps turns a..b only (default 2,6).
   They join the positions of WOTR_STARTS, if any; WOTR_START_SHARE is the share of training games that begin from one. */
static int states_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    int lo = 2, hi = 6, n = 0, bad = 0, cap = g_nstarts; State s; char err[200];
    if (getenv("WOTR_STATES_TURNS")) sscanf(getenv("WOTR_STATES_TURNS"), "%d,%d", &lo, &hi);
    while (fread(&s, sizeof s, 1, f) == 1) {
        if (s.winner || s.turn < lo || s.turn > hi) continue;
        if (wotr_check(&s, err, sizeof err)) { bad++; continue; }
        if (g_nstarts == cap) { cap = cap ? cap * 2 : 4096; g_starts = realloc(g_starts, (size_t)cap * sizeof(State)); }
        g_starts[g_nstarts++] = s; n++;
    }
    fclose(f);
    printf("starting positions: %d from %s (turns %d-%d), %d rejected by the position checker\n", n, path, lo, hi, bad);
    return 0;
}
#define g_td feat_td               /* WOTR_TD=<x>: size of the partial rewards against their design (0 = plain win/loss targets) */
typedef struct { Net fp, sh; Policy pol[2]; Net spec[NSPEC][2]; } Player;      /* value nets for "FP to decide" / "Shadow to decide" positions, and each side's move policy */

static int player_load(Player *p, const char *path) {
    char b[512];
    pol_init(&p->pol[FP]); pol_init(&p->pol[SH]);
    if (!strcmp(path, "prior")) { nn_init(&p->fp, 1); nn_init(&p->sh, 2); for (int g = 0; g < NSPEC; g++) { memcpy(&p->spec[g][FP], &p->fp, sizeof(Net)); memcpy(&p->spec[g][SH], &p->sh, sizeof(Net)); } return 0; }
    snprintf(b, sizeof b, "%s.pfp", path); pol_load(&p->pol[FP], b);
    snprintf(b, sizeof b, "%s.psh", path); pol_load(&p->pol[SH], b);
    snprintf(b, sizeof b, "%s.fp", path); if (nn_load(&p->fp, b)) return -1;
    snprintf(b, sizeof b, "%s.sh", path); if (nn_load(&p->sh, b)) return -1;
    for (int g = 0; g < NSPEC; g++) for (int k = 0; k < 2; k++) {       /* a player saved without specialists gets copies of its main pair */
        snprintf(b, sizeof b, "%s.s%d%c", path, g, k == FP ? 'f' : 's');
        if (nn_load(&p->spec[g][k], b)) { memcpy(&p->spec[g][k], k == FP ? &p->fp : &p->sh, sizeof(Net)); p->spec[g][k].m = p->spec[g][k].v = p->spec[g][k].g = 0; }
    }
    return 0;
}
static void player_save(const Player *p, const char *path) {
    char b[512];
    snprintf(b, sizeof b, "%s.fp", path); nn_save(&p->fp, b);
    snprintf(b, sizeof b, "%s.sh", path); nn_save(&p->sh, b);
    snprintf(b, sizeof b, "%s.pfp", path); pol_save(&p->pol[FP], b);
    snprintf(b, sizeof b, "%s.psh", path); pol_save(&p->pol[SH], b);
    if (g_spec) for (int g = 0; g < NSPEC; g++) for (int k = 0; k < 2; k++) { snprintf(b, sizeof b, "%s.s%d%c", path, g, k == FP ? 'f' : 's'); nn_save(&p->spec[g][k], b); }
}

/* ---- training samples (variable length, stored in a per-thread arena) ---- */
typedef struct { uint16_t n; uint8_t net; float tgt[NOUT]; float prior[NPRIOR]; } SHdr;
typedef struct { uint8_t *buf; size_t len, cap; long count; } Arena;
static void arena_push(Arena *a, const Feat *f, int net, const float *tgt) {
    size_t need = sizeof(SHdr) + (size_t)f->n * (sizeof(uint16_t) + sizeof(float));
    if (a->len + need > a->cap) { a->cap = a->cap ? a->cap * 2 : (1 << 22); while (a->len + need > a->cap) a->cap *= 2; a->buf = realloc(a->buf, a->cap); }
    SHdr h; h.n = (uint16_t)f->n; h.net = (uint8_t)net; memcpy(h.tgt, tgt, sizeof h.tgt); memcpy(h.prior, f->prior, sizeof h.prior);
    memcpy(a->buf + a->len, &h, sizeof h); a->len += sizeof h;
    memcpy(a->buf + a->len, f->idx, (size_t)f->n * sizeof(uint16_t)); a->len += (size_t)f->n * sizeof(uint16_t);
    memcpy(a->buf + a->len, f->val, (size_t)f->n * sizeof(float)); a->len += (size_t)f->n * sizeof(float);
    a->count++;
}
static size_t sample_read(const uint8_t *p, Feat *f, int *net, float *tgt) {
    SHdr h; memcpy(&h, p, sizeof h); p += sizeof h;
    f->n = h.n; *net = h.net; memcpy(tgt, h.tgt, sizeof h.tgt); memcpy(f->prior, h.prior, sizeof h.prior);
    memcpy(f->idx, p, (size_t)h.n * sizeof(uint16_t)); p += (size_t)h.n * sizeof(uint16_t);
    memcpy(f->val, p, (size_t)h.n * sizeof(float));
    return sizeof h + (size_t)h.n * (sizeof(uint16_t) + sizeof(float));
}

/* policy samples: root features, the move list and the search's target distribution */
typedef struct { uint16_t nf, nm; uint8_t net, dkind; } PHdr;
static void parena_push(Arena *a, const Feat *f, int net, int dkind, const Move *mv, const float *tgt, int nm) {
    size_t need = sizeof(PHdr) + (size_t)f->n * 6 + sizeof(f->prior) + (size_t)nm * (sizeof(Move) + sizeof(float));
    if (a->len + need > a->cap) { a->cap = a->cap ? a->cap * 2 : (1 << 22); while (a->len + need > a->cap) a->cap *= 2; a->buf = realloc(a->buf, a->cap); }
    PHdr h = {(uint16_t)f->n, (uint16_t)nm, (uint8_t)net, (uint8_t)dkind};
    uint8_t *p = a->buf + a->len;
    memcpy(p, &h, sizeof h); p += sizeof h;
    memcpy(p, f->idx, (size_t)f->n * 2); p += (size_t)f->n * 2;
    memcpy(p, f->val, (size_t)f->n * 4); p += (size_t)f->n * 4;
    memcpy(p, mv, (size_t)nm * sizeof(Move)); p += (size_t)nm * sizeof(Move);
    memcpy(p, tgt, (size_t)nm * sizeof(float));
    a->len += need - sizeof(f->prior); a->count++;
}

typedef struct {
    long games, wins[3], wt[5], wturn[5], turns, decisions, fsp_moves, battles, evals;      /* wturn: sum of final turns per way of ending */
    double corr, vps, vpf;
    long evk[ND], dk[ND];
    long pol_full, pol_hit; double pol_regret;
    /* big-picture report */
    double lt[2][8];         /* Shadow's conquest wins before turn 15 / on turn 15 or later: games, turns, Fellowship steps left, corruption, on the Mordor track, turn it entered, its moves, Shadow points after turn 10 */
    long took_mil[NREG], held_mil[NREG], n_mil;      /* in Shadow's conquest wins: captured at some point; held by Shadow at the end */
    long took[NREG];                            /* games in which the settlement was captured at some point */
    long took_turn[NREG], took_by4[NREG];       /* sum of the turns it was first captured on; games in which that was turn 4 or earlier */
    long war_n[NNAT]; double war_turn[NNAT];    /* nation reached At War; turn it happened */
    long chr_n[5]; double chr_turn[5];          /* Saruman, Witch-king, Mouth, Gandalf the White, Aragorn entered; turn */
    double h_vp[5], h_mor[5], h_big[5], h_sg[5], h_front[5], h_walls[5], h_in[5], h_bes[5]; long h_n[5]; long alloc_n, alloc_sum, alloc_hist[8];
    long sh_act[3][10], sh_turns[3], fp_act[3][10];
    double fl_left[11], fl_rev[11], fl_corr[11], fl_in[11], fl_comp[11]; long fl_n[11];
    double me_corr, me_comp; long me_n, me_win[5]; double me_corr_w[5];      /* on entering Mordor: corruption, companions; by how the game ended */     /* Fellowship at the start of turns 2..10: steps left, revealed */     /* what Shadow spent its action dice on, in turns 1-3, 4-6 and 7-10 */      /* Shadow tempo after turns 2, 4, 6, 8, 10, for comparison with human games */
    long comp_sep[2], comp_lost[2], comp_games;      /* companions leaving the Fellowship before Mordor, turns 1-4 and later: sent away, lost to the Hunt or a card */
    long mv_n[3], mv_dmg[3], mv_eyes[3];     /* Fellowship moves outside Mordor by first/second/third+ of the turn: how many, damage taken, Shadow dice in the Hunt box */
    long in_eyes, in_corr, in_lost;      /* inside Mordor: Shadow dice in the Hunt box at each move, corruption gained, companions lost */
    long taught;             /* sampled decisions made with the look-ahead */
    long dlook, dask;        /* opening look-ahead (WOTR_DEEP_OPEN): decisions where it was really used / where it was asked for (WOTR_DEEP_GAP skips the clear ones) */
    long from_start;         /* games begun from a human position */
    long styled;             /* games Shadow played in a random style */
    long sr_n, sr_in, sr_out;        /* Free Peoples attacked in an own stronghold region: how often asked, how often they went inside */
    long gate_n, in_turns, in_steps, in_moves; double gate_turn, gate_wait;      /* reaching the gate, the wait there, and the pace inside */
    long fsp_at[NREG];      /* games in which the Fellowship was declared or revealed in each region */
    long mordor_n; double mordor_turn, left5, left10, left_end; long n5, n10;   /* Fellowship: steps still to walk */
    /* learning diagnostics */
    long tier_ev[4], atier[4], atier_ev[4], atier_nc[4];      /* positions scored by search tier; the same for action decisions only */
    long tier[4], searched, explored; double explore_cost;      /* decisions by search tier; non-greedy moves */
    long pol_top1;
    long cal_n[5]; double cal_p[5], cal_z[5];                   /* calibration: predicted P(FP) bucket vs real result */
    long vs_n; double vs_prior, vs_net, vs_err;                 /* |prior logit|, |network correction|, |target - prediction| */
    long len[4];                                                /* game length: <=10, 11-15, 16-20, >20 turns */
    long end_by[4][16];                                         /* games by ending (0 Ring, 1 corruption, 2 Free Peoples conquest, 3 Shadow conquest) and turn (index 15 = turn 15 or later) */
    long mil_n, mil_turn, mil_by[4];                            /* Shadow military wins: count, sum of turns, by turn <=7 / 8-10 / 11-14 / 15+ */
    long vp_walk[2], vp_fight[2];                               /* VP taken by walking in / by fighting, per side */
    double vpf_err, vps_err, vpf_abs, vps_abs;                  /* three-turn VP forecast: mean abs error and mean abs target */
    double own_p[NOWN], own_z[NOWN], own_bs[NOWN];              /* ownership forecast: mean prediction, actual rate, squared error */
    double fc_n[NOUT], fc_t[NOUT], fc_t2[NOUT], fc_e2[NOUT];    /* every head: count, sum target, sum target^2, sum squared error */
} Stats;

/* victory-point settlements (cities and strongholds), for the big-picture report */
static uint8_t g_vps[NREG]; static int g_nvps;
static void vps_init(void) {
    for (int side = 0; side < 2; side++) for (int r = 0; r < NREG; r++) {
        int st = REGION[r].settle;
        if ((st == S_CITY || st == S_STRONGHOLD) && orig_side(r) == side) g_vps[g_nvps++] = (uint8_t)r;
    }
}

typedef struct {
    const Player *pl[2]; const Player *diag;       /* who plays FP, who plays Shadow */
    int games, samples; float temp, keep;
    uint64_t seed;
    Arena *arena;              /* NULL: evaluation only */
    Arena *parena;             /* policy samples */
    int topk, nrand; float full_frac, pkeep;
    float fast_frac; int fastk;
    Stats st;
} Job;

/* Free Peoples strongholds currently under siege */
static int fp_sieges(const State *s) { int n = 0; for (int k = 0; k < g_nvps; k++) { int r = g_vps[k]; n += orig_side(r) == FP && s->siege[r] && s->ctrl[r] == FP; } return n; }

#define MAXREC 4096
static void play_game(Job *j, uint64_t seed) {
    static _Thread_local Feat feats[MAXREC];
    static _Thread_local uint8_t nets[MAXREC], keep[MAXREC];
    static _Thread_local uint16_t turn_at[MAXREC];
    static _Thread_local uint8_t vpf_at[MAXREC], vps_at[MAXREC];
    uint8_t vp_hist[40][2]; memset(vp_hist, 0, sizeof vp_hist);    /* VP at the start of each turn */
    uint8_t corr_hist[40], left_hist[40];                          /* corruption and Fellowship steps left, likewise */
    static _Thread_local uint8_t corr_at[MAXREC], left_at[MAXREC];
    static _Thread_local uint8_t sr_at[MAXREC], ch[42][NREG]; int prev_reg = 255;      /* the settlement fought over in the battle decision that led to each position; who held every region at the start of each turn */
    static _Thread_local int8_t sg_at[MAXREC]; float tv[42]; for (int q = 0; q < 42; q++) tv[q] = -1; int prev_grp = -1;     /* the specialist family whose decision led to each position; the search's estimate at the first decision of each turn */
    static _Thread_local float rt_at[MAXREC];      /* the rollout chooser's own estimate where it made the decision (WOTR_ROLL_TEACH) */
    static _Thread_local float sv_at[MAXREC];      /* WOTR_LAMBDA: the deciding side's plain estimate of the position */
    static _Thread_local float ab_at[MAXREC];      /* the value the search itself gave the position (formula share included), for WOTR_ABSORB */
    static _Thread_local float phi_at[MAXREC], dv_at[MAXREC];     /* progress score; the look-ahead's estimate where one was made (else -1) */
    uint16_t hits_hist[40][2]; uint8_t tiles_hist[40], siege_hist[40];   /* cumulative hits and tiles, FP strongholds besieged */
    static _Thread_local uint16_t hits_at[MAXREC][2]; static _Thread_local uint8_t tiles_at[MAXREC];
    int mordor_comp = 0, mordor_corr = 0;
    int hist_turn = 0;
    State s; wotr_init(&s, seed);
    if ((j->arena || g_eval_starts) && g_nstarts && (g_eval_starts || (double)((seed * 0x9E3779B97F4A7C15ULL) >> 40) / 16777216.0 < g_start_share)) {
        s = g_starts[((seed * 0xD1342543DE82EF95ULL) >> 33) % (uint64_t)g_nstarts]; s.rng = seed * 0x2545F4914F6CDD1DULL + 1; j->st.from_start++;
    }
    static _Thread_local Agent ag[2];
    memset(ag, 0, sizeof ag);
    for (int k = 0; k < 2; k++) { ag[k].net[FP] = &j->pl[k]->fp; ag[k].net[SH] = &j->pl[k]->sh; ag[k].samples = j->samples; if (g_spec) for (int g = 0; g < NSPEC; g++) { ag[k].spec[g][FP] = &j->pl[k]->spec[g][FP]; ag[k].spec[g][SH] = &j->pl[k]->spec[g][SH]; } ag[k].depth = g_deep[k][0]; ag[k].width = g_deep[k][1]; ag[k].replies = g_deep[k][2]; ag[k].narrow = g_deep[k][3]; ag[k].taper = g_deep[k][4]; ag[k].rnd = g_deep[k][5]; ag[k].dpol[FP] = &j->pl[k]->pol[FP]; ag[k].dpol[SH] = &j->pl[k]->pol[SH]; ag[k].roll_ms = j->arena ? 0 : g_roll[k][0]; ag[k].roll_n = j->arena ? 0 : (int)g_roll[k][1]; ag[k].roll_tau = g_roll[k][2]; ag[k].roll_k = (int)g_roll[k][3]; ag[k].roll_turns = (int)g_roll[k][4]; ag[k].temp = k == SH && g_temp_sh > 0 && j->arena ? g_temp_sh : j->temp; ag[k].rng = seed * 31 + (uint64_t)k * 7919 + 1; ag[k].evals = 0; ag[k].has_root = 0; { int tk = g_topk[k] >= 0 ? g_topk[k] : j->topk; ag[k].exempt = g_exempt[k]; ag[k].exact = g_exact[k]; ag[k].pol[FP] = tk ? &j->pl[k]->pol[FP] : 0; ag[k].pol[SH] = tk ? &j->pl[k]->pol[SH] : 0; ag[k].topk = tk; } ag[k].nrand = j->nrand; ag[k].full_frac = j->full_frac; ag[k].fast_frac = j->fast_frac; ag[k].fastk = j->fastk; memset(ag[k].ev_kind, 0, sizeof ag[k].ev_kind); memset(ag[k].dec_kind, 0, sizeof ag[k].dec_kind); }
    uint64_t r = seed * 0x2545F4914F6CDD1DULL + 11;
    int T = 0;
    nn_style_p = NULL;
    if (j->arena && g_style > 0) {
        uint64_t q = seed * 0xA0761D6478BD642FULL + 5;
        #define U01 (q ^= q << 13, q ^= q >> 7, q ^= q << 17, (float)((q >> 40) / 16777216.0))
        if (U01 < g_style) {
            for (int k = 0; k < NPRIOR; k++) { float g = -3.0f; for (int u = 0; u < 6; u++) g += U01; nn_style_w[k] = k == 11 ? 1.0f : expf(g_style_s * g * 1.414f); }
            nn_style_p = &j->pl[SH]->sh.p; j->st.styled++;
        }
        #undef U01
    }
    uint8_t took[NREG] = {0}, war_t[NNAT] = {0}, chr_t[5] = {0}, fsp_seen[NREG] = {0}, mordor_t = 0, gate_t = 0, seen5 = 0, seen10 = 0, hseen = 0; int last_turn = 0, fl_last = 0; static _Thread_local int fl[40][NVPS][8];
    while (!s.winner) {
        float bv; int n;
        while (hist_turn < s.turn && hist_turn < 39) {
            hist_turn++; vp_hist[hist_turn][FP] = (uint8_t)wotr_vp(&s, FP); vp_hist[hist_turn][SH] = (uint8_t)wotr_vp(&s, SH);
            corr_hist[hist_turn] = s.corruption; left_hist[hist_turn] = (uint8_t)(fsp_steps_to_mordor(&s) + (s.mordor < 0 ? 5 : 5 - s.mordor));
            if (hist_turn >= 2 && hist_turn <= 10) { j->st.fl_left[hist_turn] += left_hist[hist_turn]; j->st.fl_rev[hist_turn] += s.revealed; j->st.fl_corr[hist_turn] += s.corruption; j->st.fl_in[hist_turn] += s.mordor >= 0; j->st.fl_comp[hist_turn] += __builtin_popcount((unsigned)fsp_comps(&s)); j->st.fl_n[hist_turn]++; }
            hits_hist[hist_turn][FP] = s.stat_hits[FP]; hits_hist[hist_turn][SH] = s.stat_hits[SH]; tiles_hist[hist_turn] = s.stat_tiles; siege_hist[hist_turn] = (uint8_t)fp_sieges(&s);
        }
        Agent *A = &ag[s.dplayer];
        if (g_open > 0 && j->arena && s.dplayer == SH) A->temp = s.turn <= 3 ? g_open : g_temp_sh > 0 ? g_temp_sh : j->temp;
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        int kp = j->arena && (double)(r >> 11) / 9007199254740992.0 < j->keep;      /* this decision is sampled for training */
        int taught = kp && g_teach[0] > 1 && s.dkind == D_ACTION && (double)((r * 0x9E3779B97F4A7C15ULL) >> 40) / 16777216.0 < g_teach_part;
        if (taught) { A->depth = g_teach[0]; A->width = g_teach[1]; A->replies = g_teach[2]; }
        /* WOTR_DEEP_OPEN=<n>: in training games Shadow chooses its actions of turns 1..n with the standard look-ahead
           (3 deep, 4 wide, 1 reply), so that openings whose pay-off is a few moves away get played and their results seen. */
        /* WOTR_ROLL_TEACH="rollouts,shortlist,part,share[,side]" (2026-11-17): a part of the sampled action decisions of one side
           (default Shadow) is made by the rollout chooser, and taught: the position's target leans `share` toward the
           rollouts' own estimate, and the move-suggesting network is taught the rollouts' preference among the finalists.
           The rollouts beat the look-ahead by about 20 points in play; this is how the networks get to learn from them. */
        int rtaught = !taught && kp && g_rteach[0] > 0 && s.dkind == D_ACTION && s.dplayer == (int)g_rteach[4] && (double)((r * 0xD1B54A32D192ED03ULL) >> 40) / 16777216.0 < g_rteach[2];
        if (rtaught) { A->roll_n = (int)g_rteach[0]; A->roll_k = (int)g_rteach[1]; A->roll_tau = 0.15f; A->roll_ms = 0; A->roll_turns = 0; }
        int dopen = !taught && !rtaught && g_dopen > 0 && j->arena && s.dplayer == SH && s.dkind == D_ACTION && s.turn <= g_dopen;
        if (dopen) { A->depth = 3; A->width = 4; A->replies = 1; A->narrow = 6; A->deep_gap = g_dgap; taught = 1; }
        long ev0 = A->evals;
        Move m = agent_choose(A, &s, &bv, &n);
        if (g_rec && !j->arena && n > 1 && s.dkind == D_ACTION && s.dplayer == g_rec_side) {
            static _Thread_local FILE *rf;
            if (!rf) { char fp[600]; snprintf(fp, sizeof fp, "%s.%p", g_rec, (void *)&rf); rf = fopen(fp, "wb"); }
            if (rf) { fwrite(&s, sizeof(State), 1, rf); fwrite(&m, sizeof(Move), 1, rf); }
        }
        if (n > 1) { j->st.tier_ev[A->last_tier] += A->evals - ev0; if (s.dkind == D_ACTION) { j->st.atier[A->last_tier]++; j->st.atier_ev[A->last_tier] += A->evals - ev0; j->st.atier_nc[A->last_tier] += A->last_ncand; } }
        if (rtaught) {
            A->roll_n = 0; j->st.taught += n > 1;
            if (n > 1 && A->last_deep && j->parena) {       /* the rollouts' preference among their finalists, for the move-suggesting network */
                float tg[MAXMOVES], z = 0;
                for (int i = 0; i < n; i++) { tg[i] = !A->last_followed[i] ? 0 : expf((s.dplayer == FP ? A->last_val[i] - bv : bv - A->last_val[i]) / 0.08f); z += tg[i]; }
                if (z > 0) { for (int i = 0; i < n; i++) tg[i] /= z; parena_push(j->parena, &A->rc.f, s.dplayer, s.dkind, A->last_mv, tg, n); }
            }
        }
        if (dopen) { A->deep_gap = 0; j->st.dlook += A->last_deep; j->st.dask++; }
        /* WOTR_TEACH_POL=1 (2026-11-19): a decision made by the teaching look-ahead also teaches the move-suggesting network the
           look-ahead's preference among the moves it followed (the scores of those are comparable with one another). */
        if (taught && g_teach_pol && n > 1 && A->last_deep && j->parena) {
            float tg[MAXMOVES], z = 0;
            for (int i = 0; i < n; i++) { tg[i] = !A->last_followed[i] ? 0 : expf((s.dplayer == FP ? A->last_val[i] - bv : bv - A->last_val[i]) / 0.08f); z += tg[i]; }
            if (z > 0) { for (int i = 0; i < n; i++) tg[i] /= z; parena_push(j->parena, &A->rc.f, s.dplayer, s.dkind, A->last_mv, tg, n); }
        }
        if (taught) { A->depth = g_deep[s.dplayer][0]; A->width = g_deep[s.dplayer][1]; A->replies = g_deep[s.dplayer][2]; A->narrow = g_deep[s.dplayer][3]; j->st.taught += n > 1; }
        if (n > 1) { j->st.tier[A->last_tier]++; j->st.searched++; j->st.explored += A->last_explored; j->st.explore_cost += A->last_explore_cost; }
        if (A->last_full && !taught) {      /* (a look-ahead decision mixes two kinds of score, so it gives no move-suggestion target) */
            j->st.pol_full++; j->st.pol_hit += A->last_hit; j->st.pol_regret += A->last_regret; j->st.pol_top1 += A->last_top1;
            r ^= r << 13; r ^= r >> 7; r ^= r << 17;
            if (j->parena && (double)(r >> 11) / 9007199254740992.0 < j->pkeep) {      /* policy target: sharp softmax of the search values */
                float tg[MAXMOVES], z = 0;
                /* after a look-ahead only the moves it followed carry comparable scores: the target is spread over those */
                for (int i = 0; i < n; i++) { tg[i] = A->last_deep && !A->last_followed[i] ? 0 : expf((s.dplayer == FP ? A->last_val[i] - bv : bv - A->last_val[i]) / 0.08f); z += tg[i]; }
                for (int i = 0; i < n; i++) tg[i] /= z;
                parena_push(j->parena, &A->rc.f, s.dplayer, s.dkind, A->last_mv, tg, n);
            }
        }
        if (n > 1 && T < MAXREC) {
            keep[T] = (uint8_t)kp; dv_at[T] = taught && !dopen ? 1.0f / (1.0f + expf(-bv)) : -1.0f; rt_at[T] = rtaught && n > 1 && A->last_deep ? 1.0f / (1.0f + expf(-bv)) : -1.0f; ab_at[T] = n > 1 ? 1.0f / (1.0f + expf(-bv)) : -1.0f;
            sg_at[T] = (int8_t)prev_grp; sr_at[T] = (uint8_t)prev_reg; prev_grp = spec_group(s.dkind);
            prev_reg = prev_grp == SP_BATTLE && s.b.dst < NREG && is_settlement(s.b.dst) ? s.b.dst : 255;
            if (s.turn < 42 && tv[s.turn] < 0) { tv[s.turn] = 1.0f / (1.0f + expf(-bv)); memcpy(ch[s.turn], s.ctrl, NREG); }
            if (keep[T]) feat_encode(&s, &feats[T]);
            if (g_lambda > 0) { static _Thread_local Feat sf; float so[NOUT]; feat_encode(&s, &sf); nn_forward(A->net[s.dplayer], &sf, so); sv_at[T] = nn_prob(so[O_WIN]); }   /* the deciding side's own plain estimate of this position (no move bonuses, no best-move optimism) */
            phi_at[T] = feat_progress(&s, feat_rw);
            vpf_at[T] = (uint8_t)wotr_vp(&s, FP); vps_at[T] = (uint8_t)wotr_vp(&s, SH); corr_at[T] = s.corruption; left_at[T] = (uint8_t)(fsp_steps_to_mordor(&s) + (s.mordor < 0 ? 5 : 5 - s.mordor));
            if (keep[T]) { hits_at[T][FP] = s.stat_hits[FP]; hits_at[T][SH] = s.stat_hits[SH]; tiles_at[T] = s.stat_tiles; }
            (void)bv; nets[T] = s.dplayer; turn_at[T] = s.turn; T++;
        }
        if (s.turn != last_turn) {
            last_turn = s.turn; if (s.turn >= 1 && s.turn <= 10) j->st.sh_turns[s.turn <= 3 ? 0 : s.turn <= 6 ? 1 : 2]++;
            if (g_falls && s.turn < 40) {       /* the local picture around every settlement at the start of each turn */
                for (int k = 0; k < NVPS; k++) { int *q = fl[s.turn][k]; q[0] = vps_local(&s, k, &q[1], &q[2], &q[3], &q[4], &q[5], &q[6], &q[7]); }
                fl_last = s.turn;
            }
        }
        if (s.dkind == D_ACTION && s.dplayer == SH && s.turn <= 10) {
            int c = m.type == M_ARMY ? 0 : m.type == M_ATTACK ? (m.a == m.b ? 2 : 1) : m.type == M_PLAY ? 3 : m.type == M_DRAW ? 4 : m.type == M_DIPLO ? 5
                  : m.type == M_MUSTER || m.type == M_MUSTER_ELITE || m.type == M_VOICE ? 6 : m.type == M_MINION ? 7 : m.type == M_NAZGUL ? 8 : m.type == M_SKIP ? 9 : -1;
            if (c >= 0) j->st.sh_act[s.turn <= 3 ? 0 : s.turn <= 6 ? 1 : 2][c]++;
        }
        if (s.dkind == D_ACTION && s.dplayer == FP && s.turn <= 10) {
            int c = m.type == M_FSP_MOVE || m.type == M_FSP_HIDE ? 0 : m.type == M_ARMY ? 1 : m.type == M_ATTACK ? 2 : m.type == M_PLAY ? 3 : m.type == M_DRAW ? 4
                  : m.type == M_DIPLO || m.type == M_COMP_POLITICS ? 5 : m.type == M_MUSTER || m.type == M_MUSTER_ELITE ? 6 : m.type == M_SEPARATE || m.type == M_COMPS ? 7
                  : m.type == M_GANDALF_WHITE || m.type == M_ARAGORN ? 8 : m.type == M_SKIP ? 9 : -1;      /* a pass spends no die */
            if (c >= 0) j->st.fp_act[s.turn <= 3 ? 0 : s.turn <= 6 ? 1 : 2][c]++;
        }
        if (s.dkind == D_HUNT_ALLOC) { j->st.alloc_n++; j->st.alloc_sum += m.a; j->st.alloc_hist[m.a < 7 ? m.a : 7]++; }
        static _Thread_local int pend; if (!T && !j->st.decisions) pend = -1;
        int k0 = s.corruption;
        if (m.type == M_FSP_MOVE && s.mordor < 0 && s.dkind == D_ACTION) { pend = s.hunt_fp > 2 ? 2 : s.hunt_fp; j->st.mv_n[pend]++; j->st.mv_eyes[pend] += s.hunt_sh; }
        if (s.dkind == D_SIEGE_RETREAT && s.dplayer == FP) { j->st.sr_n++; j->st.sr_in += m.a == 1; }
        if (s.dkind == D_RETREAT && s.dplayer == FP && m.a != 255 && m.a != s.b.dst && REGION[s.b.dst].settle == S_STRONGHOLD && s.ctrl[s.b.dst] == FP) j->st.sr_out++;
        if (m.type == M_FSP_MOVE && s.mordor >= 0 && s.dkind == D_ACTION) { j->st.in_moves++; j->st.in_eyes += s.hunt_sh; }
        int c0 = s.mordor < 0 ? __builtin_popcount((unsigned)fsp_comps(&s)) : -1, sepm = m.type == M_SEPARATE || s.dkind == D_SEP_DEST, t0 = s.turn > 4;
        wotr_apply(&s, m);
        if (c0 >= 0 && s.mordor < 0) { int d = c0 - __builtin_popcount((unsigned)fsp_comps(&s)); if (d > 0) (sepm ? j->st.comp_sep : j->st.comp_lost)[t0] += d; if (pend >= 0 && d > 0 && !sepm) j->st.mv_dmg[pend] += d; }
        if (pend >= 0 && s.corruption > k0) j->st.mv_dmg[pend] += s.corruption - k0;
        if (s.dkind == D_ACTION || s.winner) pend = -1;
        j->st.decisions++;
        /* big-picture tracking (a few dozen byte compares per decision) */
        for (int k = 0; k < g_nvps; k++) if (s.ctrl[g_vps[k]] != orig_side(g_vps[k]) && !took[g_vps[k]]) { took[g_vps[k]] = 1; j->st.took_turn[g_vps[k]] += s.turn; j->st.took_by4[g_vps[k]] += s.turn <= 4; }
        for (int n = 0; n < NNAT; n++) if (!war_t[n] && s.pol[n] == 0) war_t[n] = (uint8_t)s.turn;
        for (int mi = 0; mi < NMINION; mi++) if (!chr_t[mi] && s.minion_loc[mi] < NREG) chr_t[mi] = (uint8_t)s.turn;
        if (!chr_t[3] && s.gandalf_white) chr_t[3] = (uint8_t)s.turn;
        if (!chr_t[4] && s.aragorn) chr_t[4] = (uint8_t)s.turn;
        if (s.mordor < 0 && s.fsp_loc < NREG) fsp_seen[s.fsp_loc] = 1;
        if (!gate_t && (s.mordor >= 0 || fsp_steps_to_mordor(&s) == 0)) gate_t = (uint8_t)s.turn;
        if (!mordor_t && s.mordor >= 0) { mordor_t = (uint8_t)s.turn; mordor_comp = __builtin_popcount((unsigned)fsp_comps(&s)); mordor_corr = s.corruption; }
        while (hseen < 5 && s.turn > 2 * (hseen + 1)) {
            int mor = 0, big = 0, sg = 0;
            for (int r = 0; r < NREG; r++) {
                int u = 0;
                for (int n = NFPNAT; n < NNAT; n++) u += s.reg[r][n] + s.eli[r][n];
                if (r == R_BARAD_DUR || r == R_GORGOROTH || r == R_NURN || r == R_MINAS_MORGUL || r == R_MORANNON) mor += u;
                big += u >= 6; sg += s.siege[r] && s.ctrl[r] == FP;
            }
            j->st.h_vp[hseen] += wotr_vp(&s, SH); j->st.h_mor[hseen] += mor; j->st.h_big[hseen] += big; j->st.h_sg[hseen] += sg; { int mo, fr, wa, in, be; shadow_deploy(&s, &mo, &fr, &wa, &in, &be); j->st.h_front[hseen] += fr; j->st.h_walls[hseen] += wa; j->st.h_in[hseen] += in; j->st.h_bes[hseen] += be; } j->st.h_n[hseen]++; hseen++;
        }
        if (!seen5 && s.turn > 5) { seen5 = 1; j->st.left5 += fsp_steps_to_mordor(&s) + (s.mordor < 0 ? 5 : 5 - s.mordor); j->st.n5++; }
        if (!seen10 && s.turn > 10) { seen10 = 1; j->st.left10 += fsp_steps_to_mordor(&s) + (s.mordor < 0 ? 5 : 5 - s.mordor); j->st.n10++; }
    }
    if (g_falls && fl_last > 2) {             /* WOTR_FALLS=<file>: one line per settlement and turn, with whether it changed hands within two turns */
        static _Thread_local FILE *ff;
        if (!ff) { char fp[600]; snprintf(fp, sizeof fp, "%s.%p", g_falls, (void *)&ff); ff = fopen(fp, "w"); }
        if (ff) for (int t = 1; t + 2 <= fl_last; t++) for (int k = 0; k < NVPS; k++) {
            int *q = fl[t][k], r = q[0];
            if (q[1] != orig_side(r)) continue;                                   /* only settlements still in their owner's hands */
            fprintf(ff, "%d %d %d %d %d %d %d %d %d %d\n", orig_side(r), fl[t + 2][k][1] != q[1], q[2], q[3], q[4], q[5], q[6], q[7], REGION[r].settle == S_STRONGHOLD, t);
        }
    }
    float z = s.win_type == WIN_NONE ? 0.5f : s.winner == 1 ? 1.0f : 0.0f;     /* turn cap = draw */
    float zs[2] = {z, z};                       /* the result as each side's own network is taught it (FP's view) */
    if (s.win_type == WIN_NONE && agent_stall > 0) zs[SH] = (1 + agent_stall) / 2;      /* the turn limit: time is on the Free Peoples' side, so a stalled game is the Shadow's failure */
    if (g_out_on && s.win_type != WIN_NONE) {
        int w = s.win_type;
        zs[FP] = g_out[FP][w == WIN_RING ? 0 : w == WIN_FP_MIL ? 1 : w == WIN_CORRUPTION ? 2 : 3];
        zs[SH] = 1 - g_out[SH][w == WIN_SH_MIL ? 0 : w == WIN_CORRUPTION ? 1 : w == WIN_RING ? 2 : 3];
    }
    if (j->arena) {
        float G = z, tgt[NOUT];
        int left_end = fsp_steps_to_mordor(&s) + (s.mordor < 0 ? 5 : 5 - s.mordor), tiles_fp = 0, tiles_sh = 0, fpwar = 0, dead = 0;
        /* Targets. A position is trained toward the real result of its game (1 = the Free Peoples won, 0 = Shadow
           won, 0.5 = turn limit), plus a little for progress made soon after it -- ordinary reward shaping, carried
           back with a discount so that it reaches about two turns:
             the change in progress_score (above): steps walked, corruption, victory points, sieges, units lost,
             nations at war, minions and crowned characters, companions dead
           each multiplied by 0.6 for every turn between the position and the event (and by WOTR_TD). The whole game's
           worth of these is small beside the result itself -- with the discount, what is still to come seldom adds up to more than 0.2
           at any moment -- so winning always matters most; what they add is a reason, visible within a
           turn or two, to take the stronghold or walk the next step. */
        static _Thread_local float shaped[MAXREC];
        {
            float run = 0, nphi = feat_progress(&s, feat_rw);      /* discounted partial rewards still to come, seen from decision t */
            int ntu = s.turn;
            for (int t = T - 1; t >= 0; t--) {
                for (int q = turn_at[t]; q < ntu; q++) run *= 0.6f;
                run += g_td * (nphi - phi_at[t]);
                shaped[t] = run;
                nphi = phi_at[t]; ntu = turn_at[t];
            }
        }
        for (int k = T_CLOAKS; k <= T_SMEAGOL; k++) tiles_fp += s.special_st[k] != 0;
        for (int k = T_SHELOB; k <= T_GIVE_USS; k++) tiles_sh += s.special_st[k] != 0;
        for (int n = 0; n < NFPNAT; n++) fpwar += s.pol[n] == 0;
        for (int c = 0; c < NCOMP; c++) dead += s.comp_loc[c] == LOC_DEAD;
        tgt[O_CORR] = s.corruption / 12.0f; tgt[O_SHVP] = wotr_vp(&s, SH) / 10.0f; tgt[O_FPVP] = wotr_vp(&s, FP) / 4.0f;
        tgt[O_RING] = (s.win_type == WIN_RING || s.win_type == WIN_CORRUPTION) ? 1.0f : 0.0f;
        for (int k = 0; k < NOWN; k++) tgt[O_OWN + k] = s.ctrl[g_vps[k]] != orig_side(g_vps[k]) ? 1.0f : 0.0f;   /* who holds each VP settlement at the end */
        tgt[O_STEPS_END] = left_end / 15.0f; tgt[O_MORDOR_P] = mordor_t ? 1.0f : 0.0f;
        for (int k = 0; k < 5; k++) tgt[O_CHR_P + k] = chr_t[k] ? 1.0f : 0.0f;
        tgt[O_END + 0] = s.win_type == WIN_RING; tgt[O_END + 1] = s.win_type == WIN_CORRUPTION; tgt[O_END + 2] = s.win_type == WIN_SH_MIL; tgt[O_END + 3] = s.win_type == WIN_FP_MIL;
        tgt[O_MORDOR_COMP] = mordor_t ? mordor_comp / 7.0f : -100.0f;
        tgt[O_TILES_FP] = tiles_fp / 4.0f; tgt[O_TILES_SH] = tiles_sh / 4.0f; tgt[O_FPWAR] = fpwar / 5.0f; tgt[O_COMPDEAD] = dead / 7.0f;
        /* WOTR_LAMBDA=<l> (2026-11-19, the TD-Gammon way): a position is taught not only who won but what the player thought of
           the NEXT position: target(t) = (1 - l) x estimate at that side's next decision + l x that decision's target, the last one's target being
           the result. l = 1 is the plain result (as without the switch); smaller l trusts the next estimate more. */
        static _Thread_local float lam_at[MAXREC];
        if (g_lambda > 0) {      /* the next estimate is taken from the SAME side's next decision (the other side may be a different, frozen player) */
            float g[2] = {zs[0], zs[1]}, nx[2] = {-1, -1};
            for (int t = T - 1; t >= 0; t--) { int k = nets[t] & 1; if (nx[k] >= 0) g[k] = (1 - g_lambda) * nx[k] + g_lambda * g[k]; lam_at[t] = g[k]; nx[k] = sv_at[t]; }
        }
        for (int t = T - 1; t >= 0; t--) {
            if (!keep[t]) continue;
            G = zs[nets[t] & 1] + shaped[t]; G = G < 0 ? 0 : G > 1 ? 1 : G;
            if (g_lambda > 0) G = lam_at[t];
            if (rt_at[t] >= 0) G = (1 - g_rteach[3]) * G + g_rteach[3] * rt_at[t];
            else if (dv_at[t] >= 0) G = (1 - g_teach_share) * G + g_teach_share * dv_at[t];
            else if (g_absorb > 0 && ab_at[t] >= 0) G = (1 - g_absorb) * G + g_absorb * ab_at[t];
            tgt[O_WIN] = G; tgt[O_TURNS] = (s.turn - turn_at[t]) / 30.0f;
            {   /* what really happened over the next three turns, and how long until the big events */
                int now_t = turn_at[t], h = now_t + 3, end = h > hist_turn;
                tgt[O_FPVP3] = ((end ? wotr_vp(&s, FP) : vp_hist[h][FP]) - vpf_at[t]) / 4.0f;
                tgt[O_SHVP3] = ((end ? wotr_vp(&s, SH) : vp_hist[h][SH]) - vps_at[t]) / 10.0f;
                tgt[O_CORR3] = ((end ? s.corruption : corr_hist[h]) - corr_at[t]) / 12.0f;
                tgt[O_STEPS3] = (left_at[t] - (end ? left_end : left_hist[h])) / 15.0f;
                tgt[O_MORDOR_T] = mordor_t ? (mordor_t > now_t ? mordor_t - now_t : 0) / 15.0f : -100.0f;
                for (int k = 0; k < 5; k++) tgt[O_CHR_T + k] = chr_t[k] ? (chr_t[k] > now_t ? chr_t[k] - now_t : 0) / 10.0f : -100.0f;
                for (int n = 0; n < NNAT; n++) { float w = war_t[n] ? (war_t[n] > now_t ? war_t[n] - now_t : 0) / 10.0f : (s.turn - now_t + 5) / 10.0f; tgt[O_WAR_T + n] = w > 3 ? 3 : w; }
                for (int k = 0; k < 2; k++) tgt[O_LOST3 + k] = ((end ? s.stat_hits[k] : hits_hist[h][k]) - hits_at[t][k]) / 10.0f;
                tgt[O_SIEGE3] = (end ? fp_sieges(&s) : siege_hist[h]) / 4.0f;
                tgt[O_TILES3] = ((end ? s.stat_tiles : tiles_hist[h]) - tiles_at[t]) / 6.0f;
            }
            {   /* diagnostics on the positions kept for training */
                /* measured on the version being trained (as it stood after the last round's fit), not on whichever
                   version happened to play this game: two thirds of the games include an earlier or untrained version,
                   and their predictions made the learner look worse than guessing */
                const Net *vn = j->diag ? (nets[t] == FP ? &j->diag->fp : &j->diag->sh) : (nets[t] == FP ? &j->pl[nets[t]]->fp : &j->pl[nets[t]]->sh);
                float o[NOUT], pr = 0, pv; int b;
                nn_forward(vn, &feats[t], o);
                pv = nn_prob(o[0]); b = (int)(pv * 5); if (b > 4) b = 4;     /* the net's own prediction, without tuning offsets */
                pr = vn->p.alpha * nn_formula(feats[t].prior);
                j->st.vs_n++; j->st.vs_prior += fabsf(pr); j->st.vs_net += fabsf(o[0] - pr); j->st.vs_err += fabsf(G - pv);
                j->st.cal_n[b]++; j->st.cal_p[b] += pv; j->st.cal_z[b] += z;
                for (int k = 0; k < NOWN; k++) { float pk = nn_prob(o[O_OWN + k]); j->st.own_p[k] += pk; j->st.own_z[k] += tgt[O_OWN + k]; j->st.own_bs[k] += (pk - tgt[O_OWN + k]) * (pk - tgt[O_OWN + k]); }
                for (int k = 0; k < NOUT; k++) if (tgt[k] > -50) { double y = k == O_WIN ? z : tgt[k], pk = nn_is_prob(k) ? nn_prob(o[k]) : o[k]; j->st.fc_n[k]++; j->st.fc_t[k] += y; j->st.fc_t2[k] += y * y; j->st.fc_e2[k] += (pk - y) * (pk - y); }

            }
            arena_push(j->arena, &feats[t], nets[t], tgt);
            if (g_spec && sg_at[t] >= 0) {      /* the same position for the specialist whose decision led to it: one target, the next turn's estimate */
                float st[NOUT]; int nx = turn_at[t] + 1;
                for (int k = 0; k < NOUT; k++) st[k] = -100.0f;
                st[O_WIN] = nx < 42 && tv[nx] >= 0 ? tv[nx] : z;
                /* after a battle decision over a settlement the lesson is the plain fact: do the Free Peoples hold it at the
                   end of the following turn (1) or Shadow (0)? The overall estimate comes from a network that never
                   understood sieges, and a specialist taught by it learned nothing (trial p1). */
                if (sg_at[t] == SP_BATTLE && sr_at[t] < NREG) st[O_WIN] = (nx + 1 < 42 && tv[nx + 1] >= 0 ? ch[nx + 1][sr_at[t]] : s.ctrl[sr_at[t]]) == FP ? 1.0f : 0.0f;
                arena_push(j->arena, &feats[t], 2 + sg_at[t] * 2 + nets[t], st);
            }
        }
    }
    for (int k = 0; k < g_nvps; k++) j->st.took[g_vps[k]] += took[g_vps[k]];
    if (s.win_type == WIN_SH_MIL) { double *q = j->st.lt[s.turn >= 15]; q[0]++; q[1] += s.turn; q[2] += fsp_steps_to_mordor(&s) + (s.mordor < 0 ? 5 : 5 - s.mordor); q[3] += s.corruption; q[4] += s.mordor >= 0; q[5] += mordor_t; q[6] += s.stat_fsp_moves; q[7] += vp_hist[10][SH]; }
    if (s.win_type == WIN_SH_MIL) { j->st.n_mil++; for (int k = 0; k < g_nvps; k++) { int r = g_vps[k]; if (orig_side(r) == FP) { j->st.took_mil[r] += took[r]; j->st.held_mil[r] += s.ctrl[r] == SH; } } }
    for (int n = 0; n < NNAT; n++) if (war_t[n]) { j->st.war_n[n]++; j->st.war_turn[n] += war_t[n]; }
    for (int k = 0; k < 5; k++) if (chr_t[k]) { j->st.chr_n[k]++; j->st.chr_turn[k] += chr_t[k]; }
    for (int r = 0; r < NREG; r++) j->st.fsp_at[r] += fsp_seen[r];
    if (mordor_t) { j->st.gate_n++; j->st.gate_turn += gate_t; j->st.gate_wait += mordor_t - gate_t; j->st.in_turns += s.turn - mordor_t + 1; j->st.in_steps += s.mordor > 5 ? 5 : s.mordor; j->st.in_corr += s.corruption - mordor_corr; j->st.in_lost += mordor_comp - __builtin_popcount((unsigned)fsp_comps(&s)); }
    if (mordor_t) { j->st.mordor_n++; j->st.mordor_turn += mordor_t; j->st.me_n++; j->st.me_corr += mordor_corr; j->st.me_comp += mordor_comp; j->st.me_win[s.win_type]++; j->st.me_corr_w[s.win_type] += mordor_corr; }
    j->st.left_end += fsp_steps_to_mordor(&s) + (s.mordor < 0 ? 5 : 5 - s.mordor);
    for (int k = 0; k < 2; k++) { j->st.vp_walk[k] += s.stat_vp_walk[k]; j->st.vp_fight[k] += s.stat_vp_fight[k]; }
    { int e = s.win_type == WIN_RING ? 0 : s.win_type == WIN_CORRUPTION ? 1 : s.win_type == WIN_FP_MIL ? 2 : s.win_type == WIN_SH_MIL ? 3 : -1; if (e >= 0) j->st.end_by[e][s.turn < 15 ? s.turn : 15]++; }
    if (s.win_type == WIN_SH_MIL) { j->st.mil_n++; j->st.mil_turn += s.turn; j->st.mil_by[s.turn <= 7 ? 0 : s.turn <= 10 ? 1 : s.turn <= 14 ? 2 : 3]++; }
    j->st.len[s.turn <= 10 ? 0 : s.turn <= 15 ? 1 : s.turn <= 20 ? 2 : 3]++;
    j->st.games++; j->st.wins[s.winner]++; j->st.wt[s.win_type]++; j->st.wturn[s.win_type] += s.turn; j->st.turns += s.turn;
    j->st.fsp_moves += s.stat_fsp_moves; j->st.battles += s.stat_battles; j->st.corr += s.corruption;
    for (int k = 0; k < ND; k++) { j->st.evk[k] += ag[0].ev_kind[k] + ag[1].ev_kind[k]; j->st.dk[k] += ag[0].dec_kind[k] + ag[1].dec_kind[k]; }
    j->st.vps += wotr_vp(&s, SH); j->st.vpf += wotr_vp(&s, FP); j->st.evals += ag[0].evals + ag[1].evals;
}
static void *worker(void *arg) {
    Job *j = arg;
    for (int g = 0; g < j->games; g++) play_game(j, j->seed + (uint64_t)g);
    return 0;
}
static pthread_t g_th[64];
static void start_jobs(Job *jobs, int nt) {
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 64u << 20);
    for (int i = 0; i < nt; i++) pthread_create(&g_th[i], &at, worker, &jobs[i]);
}
static void join_jobs(int nt) { for (int i = 0; i < nt; i++) pthread_join(g_th[i], 0); }
static void run_jobs(Job *jobs, int nt) { start_jobs(jobs, nt); join_jobs(nt); }
static void stats_add(Stats *a, const Stats *b) {
    a->games += b->games; a->turns += b->turns; a->decisions += b->decisions; a->fsp_moves += b->fsp_moves; a->battles += b->battles;
    a->evals += b->evals; a->corr += b->corr; a->vps += b->vps; a->vpf += b->vpf;
    for (int k = 0; k < 3; k++) a->wins[k] += b->wins[k];
    for (int k = 0; k < 5; k++) { a->wt[k] += b->wt[k]; a->wturn[k] += b->wturn[k]; }
    for (int k = 0; k < ND; k++) { a->evk[k] += b->evk[k]; a->dk[k] += b->dk[k]; }
    a->pol_full += b->pol_full; a->pol_hit += b->pol_hit; a->pol_regret += b->pol_regret;
    for (int r = 0; r < NREG; r++) { a->took_turn[r] += b->took_turn[r]; a->took_by4[r] += b->took_by4[r]; a->took[r] += b->took[r]; a->took_mil[r] += b->took_mil[r]; a->held_mil[r] += b->held_mil[r]; }
    a->n_mil += b->n_mil; for (int q = 0; q < 2; q++) for (int k = 0; k < 8; k++) a->lt[q][k] += b->lt[q][k];
    for (int n = 0; n < NNAT; n++) { a->war_n[n] += b->war_n[n]; a->war_turn[n] += b->war_turn[n]; }
    for (int k = 0; k < 5; k++) { a->chr_n[k] += b->chr_n[k]; a->chr_turn[k] += b->chr_turn[k]; }
    for (int k = 0; k < 4; k++) { a->tier[k] += b->tier[k]; a->len[k] += b->len[k]; a->mil_by[k] += b->mil_by[k]; }
    for (int e = 0; e < 4; e++) for (int t = 0; t < 16; t++) a->end_by[e][t] += b->end_by[e][t];
    a->mil_n += b->mil_n; a->mil_turn += b->mil_turn; a->dlook += b->dlook; a->dask += b->dask;
    for (int q = 0; q < 4; q++) { a->tier_ev[q] += b->tier_ev[q]; a->atier[q] += b->atier[q]; a->atier_ev[q] += b->atier_ev[q]; a->atier_nc[q] += b->atier_nc[q]; }
    a->searched += b->searched; a->explored += b->explored; a->explore_cost += b->explore_cost; a->pol_top1 += b->pol_top1;
    for (int k = 0; k < 5; k++) { a->cal_n[k] += b->cal_n[k]; a->cal_p[k] += b->cal_p[k]; a->cal_z[k] += b->cal_z[k]; }
    for (int k = 0; k < 2; k++) { a->vp_walk[k] += b->vp_walk[k]; a->vp_fight[k] += b->vp_fight[k]; }
    for (int k = 0; k < NOWN; k++) { a->own_p[k] += b->own_p[k]; a->own_z[k] += b->own_z[k]; a->own_bs[k] += b->own_bs[k]; }
    for (int k = 0; k < NOUT; k++) { a->fc_n[k] += b->fc_n[k]; a->fc_t[k] += b->fc_t[k]; a->fc_t2[k] += b->fc_t2[k]; a->fc_e2[k] += b->fc_e2[k]; }
    a->vpf_err += b->vpf_err; a->vps_err += b->vps_err; a->vpf_abs += b->vpf_abs; a->vps_abs += b->vps_abs;
    a->vs_n += b->vs_n; a->vs_prior += b->vs_prior; a->vs_net += b->vs_net; a->vs_err += b->vs_err;
    for (int k = 0; k < 5; k++) { a->h_vp[k] += b->h_vp[k]; a->h_mor[k] += b->h_mor[k]; a->h_big[k] += b->h_big[k]; a->h_sg[k] += b->h_sg[k]; a->h_front[k] += b->h_front[k]; a->h_walls[k] += b->h_walls[k]; a->h_in[k] += b->h_in[k]; a->h_bes[k] += b->h_bes[k]; a->h_n[k] += b->h_n[k]; }
    a->me_n += b->me_n; a->me_corr += b->me_corr; a->me_comp += b->me_comp; for (int q = 0; q < 5; q++) { a->me_win[q] += b->me_win[q]; a->me_corr_w[q] += b->me_corr_w[q]; }
    for (int q = 0; q < 11; q++) { a->fl_left[q] += b->fl_left[q]; a->fl_rev[q] += b->fl_rev[q]; a->fl_corr[q] += b->fl_corr[q]; a->fl_in[q] += b->fl_in[q]; a->fl_comp[q] += b->fl_comp[q]; a->fl_n[q] += b->fl_n[q]; }
    for (int q = 0; q < 3; q++) { a->sh_turns[q] += b->sh_turns[q]; for (int c = 0; c < 10; c++) { a->sh_act[q][c] += b->sh_act[q][c]; a->fp_act[q][c] += b->fp_act[q][c]; } }
    a->alloc_n += b->alloc_n; a->alloc_sum += b->alloc_sum; for (int k = 0; k < 8; k++) a->alloc_hist[k] += b->alloc_hist[k];
    for (int q = 0; q < 2; q++) { a->comp_sep[q] += b->comp_sep[q]; a->comp_lost[q] += b->comp_lost[q]; }
    for (int q = 0; q < 3; q++) { a->mv_n[q] += b->mv_n[q]; a->mv_dmg[q] += b->mv_dmg[q]; a->mv_eyes[q] += b->mv_eyes[q]; }
    for (int r = 0; r < NREG; r++) a->fsp_at[r] += b->fsp_at[r];
    a->taught += b->taught; a->from_start += b->from_start; a->styled += b->styled; a->sr_n += b->sr_n; a->sr_in += b->sr_in; a->sr_out += b->sr_out;
    a->gate_n += b->gate_n; a->gate_turn += b->gate_turn; a->gate_wait += b->gate_wait; a->in_turns += b->in_turns; a->in_steps += b->in_steps; a->in_moves += b->in_moves; a->in_eyes += b->in_eyes; a->in_corr += b->in_corr; a->in_lost += b->in_lost;
    a->mordor_n += b->mordor_n; a->mordor_turn += b->mordor_turn; a->left5 += b->left5; a->left10 += b->left10; a->left_end += b->left_end; a->n5 += b->n5; a->n10 += b->n10;
}
static void stats_print(const Stats *s, const char *tag) {
    double g = (double)s->games;
    printf("%s: %ld games | Shadow wins %.1f%% | FP ring %.1f%% FP military %.1f%% | SH corruption %.1f%% SH military %.1f%% | draws (turn cap) %.1f%% | turns %.1f fsp moves %.1f battles %.1f corr %.1f SHvp %.1f FPvp %.1f\n",
           tag, s->games, 100 * (s->wins[2] - s->wt[WIN_NONE]) / g, 100 * s->wt[WIN_RING] / g, 100 * s->wt[WIN_FP_MIL] / g, 100 * s->wt[WIN_CORRUPTION] / g,
           100 * s->wt[WIN_SH_MIL] / g, 100 * s->wt[WIN_NONE] / g, s->turns / g, s->fsp_moves / g, s->battles / g, s->corr / g, s->vps / g, s->vpf / g);
}

/* what each side actually did, in the strategy guide's terms */
static void report(const Stats *s) {
    static const char *NAT[NNAT] = {"Dwarves", "Elves", "Gondor", "North", "Rohan", "Isengard", "Sauron", "Southrons"};
    static const char *CHR[5] = {"Saruman", "Witch-king", "Mouth", "Gandalf the White", "Aragorn"};
    static const int CI[5] = {M_SARUMAN, M_WK, M_MOUTH, 3, 4};
    double g = (double)s->games;
    for (int side = 0; side < 2; side++) {
        printf("  %s captured:", side == FP ? "Shadow" : "Free Peoples");
        for (int k = 0; k < g_nvps; k++) { int r = g_vps[k]; if (orig_side(r) == side && s->took[r]) printf(" %s %.1f%%", REGION[r].name, 100 * s->took[r] / g); }
        printf("\n");
    }
    for (int side = 0; side < 2; side++) {
        long w = s->vp_walk[side], f = s->vp_fight[side];
        printf("  %s victory points taken: %.2f per game, %.0f%% by walking into an empty settlement, %.0f%% by battle or siege\n",
               side == FP ? "Free Peoples" : "Shadow", (w + f) / g, w + f ? 100.0 * w / (w + f) : 0, w + f ? 100.0 * f / (w + f) : 0);
    }
    printf("  average turns to conclusion: all games %.1f", s->turns / g);
    { static const char *WN[5] = {"turn limit", "Ring destroyed", "corruption", "Free Peoples conquest", "Shadow conquest"};
      for (int k = 1; k < 5; k++) if (s->wt[k]) printf("; %s %.1f", WN[k], (double)s->wturn[k] / s->wt[k]);
      printf("\n"); }
    for (int q = 0; q < 2; q++) if (s->lt[q][0] > 0) { const double *x = s->lt[q]; printf("  Shadow conquest wins %s (%.0f%% of games): turn %.1f; Fellowship then %.1f steps from Mount Doom, corruption %.1f, on the Mordor track in %.0f%% (entered turn %.1f), %.2f moves a turn; Shadow had %.1f points after turn 10\n", q ? "on turn 15 or later" : "before turn 15", 100 * x[0] / g, x[1] / x[0], x[2] / x[0], x[3] / x[0], 100 * x[4] / x[0], x[4] > 0 ? x[5] / x[4] : 0, x[6] / x[1], x[7] / x[0]); }
    if (s->n_mil) { printf("  in Shadow's conquest wins, held by Shadow at the end:"); for (int q = 0; q < 12; q++) { int b = -1; for (int k = 0; k < g_nvps; k++) { int r = g_vps[k]; if (s->held_mil[r] > 0 && (b < 0 || s->held_mil[r] > s->held_mil[b])) b = r; } if (b < 0) break; printf(" %s %.0f%%", REGION[b].name, 100.0 * s->held_mil[b] / s->n_mil); ((Stats *)s)->held_mil[b] = -s->held_mil[b]; } for (int r = 0; r < NREG; r++) if (s->held_mil[r] < 0) ((Stats *)s)->held_mil[r] = -s->held_mil[r]; printf("\n"); }
    {   /* where each kind of ending falls, turn by turn, as a share of ALL games (Philip: the cluster of Ring victories should be seen moving down) */
        static const char *EN[4] = {"Ring destroyed", "corruption", "Free Peoples conquest", "Shadow conquest"};
        for (int e = 0; e < 4; e++) {
            long tot = 0; for (int t = 0; t < 16; t++) tot += s->end_by[e][t];
            if (!tot) continue;
            printf("  %s by turn, %% of all games:", EN[e]);
            long early = 0; for (int t = 0; t <= 6; t++) early += s->end_by[e][t];
            printf(" <=6: %.1f", 100.0 * early / s->games);
            for (int t = 7; t <= 14; t++) printf("  %d: %.1f", t, 100.0 * s->end_by[e][t] / s->games);
            printf("  15+: %.1f\n", 100.0 * s->end_by[e][15] / s->games);
        }
    }
    {   /* Gondor: AlphaWOTR's Shadow takes Minas Tirith by turn 3-4 in nearly every game against us (humans lose it in about a fifth of games, turn 7) */
        static const int G[3] = {R_MINAS_TIRITH, R_PELARGIR, R_DOL_AMROTH};
        printf("  Gondor falls:");
        for (int k = 0; k < 3; k++) { int r = G[k]; printf(" %s in %.0f%% of games (turn %.1f; by turn 4 in %.0f%%)%s", REGION[r].name, 100.0 * s->took[r] / s->games, s->took[r] ? (double)s->took_turn[r] / s->took[r] : 0.0, 100.0 * s->took_by4[r] / s->games, k < 2 ? ";" : "\n"); }
    }
    if (s->dask) printf("  look-ahead in training: asked for at %.1f Shadow decisions a game, really used at %.1f (the rest were clear choices)\n", (double)s->dask / s->games, (double)s->dlook / s->games);
    if (s->mil_n) printf("  Shadow military victories: average turn %.1f; by turn 7: %.0f%%, turns 8-10: %.0f%%, 11-14: %.0f%%, 15+: %.0f%% (of those wins)\n", (double)s->mil_turn / s->mil_n,
                         100.0 * s->mil_by[0] / s->mil_n, 100.0 * s->mil_by[1] / s->mil_n, 100.0 * s->mil_by[2] / s->mil_n, 100.0 * s->mil_by[3] / s->mil_n);
    printf("  at war (%% of games, avg turn):");
    for (int n = 0; n < NNAT; n++) printf(" %s %.0f%%/t%.1f", NAT[n], 100 * s->war_n[n] / g, s->war_n[n] ? s->war_turn[n] / s->war_n[n] : 0);
    printf("\n  characters (%% of games, avg turn):");
    for (int k = 0; k < 5; k++) { int c = CI[k]; printf(" %s %.0f%%/t%.1f", CHR[k], 100 * s->chr_n[c] / g, s->chr_n[c] ? s->chr_turn[c] / s->chr_n[c] : 0); }
    {
        static const float HV[5] = {0.2f, 1.7f, 4.0f, 5.8f, 6.5f}, HM[5] = {12.4f, 9.2f, 7.5f, 6.5f, 5.7f}, HB[5] = {2.7f, 3.3f, 3.3f, 3.0f, 2.8f}, HS[5] = {0.2f, 0.9f, 1.2f, 1.2f, 1.2f};   /* 1,508 ladder games of 2022 */
        printf("\n  Shadow tempo after turns 2/4/6/8/10, ours (1,508 human games):");
        printf(" victory points"); for (int k = 0; k < 5; k++) printf(" %.1f (%.1f)", s->h_n[k] ? s->h_vp[k] / s->h_n[k] : 0, HV[k]);
        printf("; units in Mordor"); for (int k = 0; k < 5; k++) printf(" %.1f (%.1f)", s->h_n[k] ? s->h_mor[k] / s->h_n[k] : 0, HM[k]);
        printf("; armies of 6+"); for (int k = 0; k < 5; k++) printf(" %.1f (%.1f)", s->h_n[k] ? s->h_big[k] / s->h_n[k] : 0, HB[k]);
        printf("; Free Peoples strongholds besieged"); for (int k = 0; k < 5; k++) printf(" %.2f (%.1f)", s->h_n[k] ? s->h_sg[k] / s->h_n[k] : 0, HS[k]);
        { static const float HF[5] = {29.1f, 37.3f, 39.6f, 39.2f, 38.9f}, HB[5] = {0.8f, 3.2f, 4.1f, 4.3f, 4.2f};
          printf("; units within two regions of a Free Peoples settlement"); for (int k = 0; k < 5; k++) printf(" %.1f (%.1f)", s->h_n[k] ? s->h_front[k] / s->h_n[k] : 0, HF[k]);
          { static const float HW[5] = {7.0f, 17.1f, 23.6f, 26.5f, 27.3f}; printf("; units in or next to one"); for (int k = 0; k < 5; k++) printf(" %.1f (%.1f)", s->h_n[k] ? s->h_walls[k] / s->h_n[k] : 0, HW[k]); }
          { static const float HI[5] = {2.6f, 10.5f, 15.4f, 18.1f, 18.5f}; printf("; units in one (besieging or holding)"); for (int k = 0; k < 5; k++) printf(" %.1f (%.1f)", s->h_n[k] ? s->h_in[k] / s->h_n[k] : 0, HI[k]); }
          printf("; Free Peoples units besieged"); for (int k = 0; k < 5; k++) printf(" %.1f (%.1f)", s->h_n[k] ? s->h_bes[k] / s->h_n[k] : 0, HB[k]); }
        {   /* humans: 1,508 ladder games; their logs do not tell a march from an attack, and show no discarded dice */
            static const char *AN[10] = {"march", "attack", "assault a siege", "play a card", "draw a card", "advance a nation", "recruit", "bring in a minion", "move Nazgul", "discard the die"};
            static const float HU[3][10] = {{1.94f, -1, -1, 0.75f, 0.37f, 0.77f, 0.22f, 0.44f, 0.07f, 0}, {2.08f, -1, -1, 1.43f, 0.38f, 0.33f, 0.91f, 0.16f, 0.22f, 0}, {1.98f, -1, -1, 1.40f, 0.53f, 0.04f, 0.96f, 0.18f, 0.27f, 0}};
            static const char *BN[3] = {"turns 1-3", "turns 4-6", "turns 7-10"};
            for (int q = 0; q < 3; q++) {
                double n = s->sh_turns[q] ? (double)s->sh_turns[q] : 1;
                printf("\n  Shadow action dice per turn, %s, ours (humans): march + attack + assault %.2f + %.2f + %.2f = %.2f (%.2f);", BN[q], s->sh_act[q][0] / n, s->sh_act[q][1] / n, s->sh_act[q][2] / n, (s->sh_act[q][0] + s->sh_act[q][1] + s->sh_act[q][2]) / n, HU[q][0]);
                for (int c = 3; c < 10; c++) printf(" %s %.2f (%.2f);", AN[c], s->sh_act[q][c] / n, HU[q][c]);
            }
        }
        {   /* the Free Peoples' dice the same way; the human logs lump separating companions, Gandalf the White and Aragorn under "other" */
            static const char *FN[10] = {"move the Fellowship", "march", "attack", "play a card", "draw a card", "advance a nation", "recruit", "move or separate companions", "Gandalf the White or Aragorn", "discard the die"};
            static const float HU[3][10] = {{1.50f, 0.36f, -1, 0.78f, 0.10f, 0.51f, 0.23f, 0.08f, -1, 0}, {1.48f, 0.42f, -1, 0.97f, 0.10f, 0.30f, 0.69f, 0.07f, -1, 0}, {1.22f, 0.64f, -1, 0.92f, 0.28f, 0.15f, 0.84f, 0.06f, -1, 0}};
            static const char *BN[3] = {"turns 1-3", "turns 4-6", "turns 7-10"};
            for (int q = 0; q < 3; q++) {
                double n = s->sh_turns[q] ? (double)s->sh_turns[q] : 1;
                printf("\n  Free Peoples action dice per turn, %s, ours (humans): %s %.2f (%.2f); march + attack %.2f + %.2f = %.2f (%.2f);", BN[q], FN[0], s->fp_act[q][0] / n, HU[q][0], s->fp_act[q][1] / n, s->fp_act[q][2] / n, (s->fp_act[q][1] + s->fp_act[q][2]) / n, HU[q][1]);
                for (int c = 3; c < 10; c++) { printf(" %s %.2f", FN[c], s->fp_act[q][c] / n); if (HU[q][c] >= 0) printf(" (%.2f)", HU[q][c]); printf(";"); }
            }
        }
        printf("\n  Shadow dice set aside for the Hunt before rolling: %.2f a turn (humans 0.8); share of turns with 0/1/2/3/4/5+ dice:", s->alloc_n ? (double)s->alloc_sum / s->alloc_n : 0);
        for (int k = 0; k < 5; k++) printf(" %.0f%%", s->alloc_n ? 100.0 * s->alloc_hist[k] / s->alloc_n : 0);
        printf(" %.0f%%", s->alloc_n ? 100.0 * (s->alloc_hist[5] + s->alloc_hist[6] + s->alloc_hist[7]) / s->alloc_n : 0);
    }
    {
        static const float HL[11] = {0, 0, 13.3f, 11.9f, 10.5f, 9.1f, 7.6f, 6.1f, 4.7f, 3.3f, 2.0f};       /* humans, from their Fellowship moves per turn */
        printf("\n  Fellowship steps left after turns 1-9, ours (humans):");
        for (int q = 2; q <= 10; q++) printf(" %.1f (%.1f)", s->fl_n[q] ? s->fl_left[q] / s->fl_n[q] : 0, HL[q]);
        printf("; revealed at the start of turns 2-10:");
        for (int q = 2; q <= 10; q++) printf(" %.0f%%", s->fl_n[q] ? 100 * s->fl_rev[q] / s->fl_n[q] : 0);
        static const float HC[11] = {0, 0, 0.3f, 0.7f, 1.4f, 2.0f, 2.7f, 3.4f, 4.2f, 4.9f, 5.7f}, HI[11] = {0, 0, 0, 0, 0, 0, 20, 40, 60, 80, 80};
        printf("\n  corruption after turns 1-9, ours (humans):");
        for (int q = 2; q <= 10; q++) printf(" %.1f (%.1f)", s->fl_n[q] ? s->fl_corr[q] / s->fl_n[q] : 0, HC[q]);
        printf("; inside Mordor:");
        for (int q = 5; q <= 10; q++) printf(" %.0f%% (%.0f%%)", s->fl_n[q] ? 100 * s->fl_in[q] / s->fl_n[q] : 0, HI[q]);
        printf("; companions:");
        for (int q = 2; q <= 10; q++) printf(" %.1f", s->fl_n[q] ? s->fl_comp[q] / s->fl_n[q] : 0);
    }
    if (s->me_n) printf("\n  on entering Mordor, ours (human Ring wins / corruption losses): corruption %.1f (2.6 / 3.6), companions %.1f (4.1 / 3.1); of those that entered: Ring destroyed %.0f%% (entered with corruption %.1f), corruption %.0f%% (%.1f), Shadow conquest first %.0f%%",
        s->me_corr / s->me_n, s->me_comp / s->me_n, 100.0 * s->me_win[WIN_RING] / s->me_n, s->me_win[WIN_RING] ? s->me_corr_w[WIN_RING] / s->me_win[WIN_RING] : 0,
        100.0 * s->me_win[WIN_CORRUPTION] / s->me_n, s->me_win[WIN_CORRUPTION] ? s->me_corr_w[WIN_CORRUPTION] / s->me_win[WIN_CORRUPTION] : 0, 100.0 * s->me_win[WIN_SH_MIL] / s->me_n);
    printf("\n  companions leaving the Fellowship before Mordor, per game: sent away %.2f in turns 1-4 and %.2f later; lost to the Hunt or a card %.2f in turns 1-4 and %.2f later", s->comp_sep[0] / g, s->comp_sep[1] / g, s->comp_lost[0] / g, s->comp_lost[1] / g);
    printf("\n  Fellowship moves outside Mordor, first / second / third or later of the turn: per game %.1f / %.1f / %.1f; damage per move (corruption + companions) %.2f / %.2f / %.2f; Shadow dice in the Hunt box %.1f / %.1f / %.1f",
        s->mv_n[0] / g, s->mv_n[1] / g, s->mv_n[2] / g, (double)s->mv_dmg[0] / (s->mv_n[0] + !s->mv_n[0]), (double)s->mv_dmg[1] / (s->mv_n[1] + !s->mv_n[1]), (double)s->mv_dmg[2] / (s->mv_n[2] + !s->mv_n[2]), (double)s->mv_eyes[0] / (s->mv_n[0] + !s->mv_n[0]), (double)s->mv_eyes[1] / (s->mv_n[1] + !s->mv_n[1]), (double)s->mv_eyes[2] / (s->mv_n[2] + !s->mv_n[2]));
    if (s->taught) printf("\n  %.1f decisions a game were made with the look-ahead and taught to the network", s->taught / g);
    if (s->styled) printf("\n  Shadow played %.0f%% of these games in a random style", 100 * s->styled / g);
    if (s->from_start) printf("\n  %.0f%% of these games began from a human position (their turn counts include the human part)", 100 * s->from_start / g);
    printf("\n  Free Peoples attacked in an own stronghold region: %.1f times a game, went inside the walls %.0f%% of the time, walked out of the region %.2f times a game", s->sr_n / g, 100.0 * s->sr_in / (s->sr_n + !s->sr_n), s->sr_out / g);
    printf("\n  where the Fellowship was declared or revealed (share of games):");
    for (int k = 0; k < 14; k++) { int b = -1; for (int r = 0; r < NREG; r++) if (s->fsp_at[r] > 0 && (b < 0 || s->fsp_at[r] > s->fsp_at[b])) b = r; if (b < 0) break; printf(" %s %.0f%%", REGION[b].name, 100 * s->fsp_at[b] / g); ((Stats *)s)->fsp_at[b] = -s->fsp_at[b]; }
    for (int r = 0; r < NREG; r++) if (s->fsp_at[r] < 0) ((Stats *)s)->fsp_at[r] = -s->fsp_at[r];
    if (s->gate_n) printf("\n  at the gate of Mordor: reached on turn %.1f, entered %.2f turns later; inside: %.2f steps per turn (humans 1.1), %.1f turns inside per game; %.1f move attempts for %.1f steps; per step: corruption %.2f (humans 0.97), companions lost %.2f; Shadow dice in the Hunt box at a move %.1f",
        s->gate_turn / s->gate_n, s->gate_wait / s->gate_n, (double)s->in_steps / s->in_turns, (double)s->in_turns / s->gate_n, (double)s->in_moves / s->gate_n, (double)s->in_steps / s->gate_n, (double)s->in_corr / (s->in_steps + !s->in_steps), (double)s->in_lost / (s->in_steps + !s->in_steps), (double)s->in_eyes / (s->in_moves + !s->in_moves));
    printf("\n  Fellowship steps left to Mount Doom (15 at start): after turn 5 %.1f, after turn 10 %.1f, at game end %.1f; entered Mordor in %.0f%% of games (avg turn %.1f)\n",
           s->n5 ? s->left5 / s->n5 : 0, s->n10 ? s->left10 / s->n10 : 0, s->left_end / g, 100 * s->mordor_n / g, s->mordor_n ? s->mordor_turn / s->mordor_n : 0);
}

/* how the learning itself is going */
static void diagnostics(const Stats *s, const Player *learner) {
    double g = (double)s->games, d = (double)(s->searched ? s->searched : 1);
    printf("  game length: <=10 turns %.0f%%, 11-15 %.0f%%, 16-20 %.0f%%, >20 %.0f%%; %.0f decisions with a choice per game, %.1f positions evaluated per decision\n",
           100 * s->len[0] / g, 100 * s->len[1] / g, 100 * s->len[2] / g, 100 * s->len[3] / g, d / g, (double)s->evals / d);
    printf("  search tiers: all moves (few options) %.0f%%, full width %.0f%%, policy top-8 %.0f%%, quick top-3 %.0f%%\n",
           100 * s->tier[0] / d, 100 * s->tier[1] / d, 100 * s->tier[2] / d, 100 * s->tier[3] / d);
    printf("  exploration: %.1f%% of moves were not the search's best, giving up %.3f log-odds on average when they weren't; policy's first choice = search's best in %.0f%% of full-width decisions\n",
           100 * s->explored / d, s->explored ? s->explore_cost / (double)s->explored : 0, s->pol_full ? 100.0 * s->pol_top1 / (double)s->pol_full : 0);
    if (s->vs_n) {
        printf("  value = prior + network: mean |prior| %.2f, mean |network correction| %.2f log-odds; mean |target - prediction| %.3f\n",
               s->vs_prior / (double)s->vs_n, s->vs_net / (double)s->vs_n, s->vs_err / (double)s->vs_n);
        {   /* every auxiliary prediction: skill = 1 - (squared error) / (variance of what happened); 0 = no better than the average */
            static const struct { int k; const char *name; } F[] = {
                {O_WIN, "who wins"}, {O_CORR, "final corruption"}, {O_CORR3, "corruption next 3 turns"}, {O_STEPS_END, "Fellowship steps left at end"},
                {O_STEPS3, "Fellowship steps next 3 turns"}, {O_MORDOR_P, "enters Mordor"}, {O_MORDOR_T, "turns to Mordor"}, {O_SHVP, "final Shadow VP"},
                {O_SHVP3, "Shadow VP next 3 turns"}, {O_FPVP, "final FP VP"}, {O_FPVP3, "FP VP next 3 turns"}, {O_RING, "decided by the Ring"}, {O_TURNS, "turns remaining"},
                {O_CHR_P + 0, "Witch-king enters"}, {O_CHR_T + 0, "turns to Witch-king"}, {O_CHR_P + 1, "Saruman enters"}, {O_CHR_T + 1, "turns to Saruman"},
                {O_CHR_P + 2, "Mouth enters"}, {O_CHR_T + 2, "turns to Mouth"}, {O_CHR_P + 3, "Gandalf the White enters"}, {O_CHR_T + 3, "turns to Gandalf the White"},
                {O_CHR_P + 4, "Aragorn crowned"}, {O_CHR_T + 4, "turns to Aragorn"}, {O_TILES_FP, "FP special tiles"}, {O_TILES_SH, "Shadow special tiles"},
                {O_FPWAR, "FP nations at war at end"}, {O_COMPDEAD, "companions dead at end"},
                {O_END + 0, "ends: Ring destroyed"}, {O_END + 1, "ends: corruption"}, {O_END + 2, "ends: Shadow military"}, {O_END + 3, "ends: FP military"},
                {O_WAR_T + 0, "turns to war: Dwarves"}, {O_WAR_T + 1, "Elves"}, {O_WAR_T + 2, "Gondor"}, {O_WAR_T + 3, "North"}, {O_WAR_T + 4, "Rohan"},
                {O_WAR_T + 5, "Isengard"}, {O_WAR_T + 6, "Sauron"}, {O_WAR_T + 7, "Southrons"},
                {O_LOST3 + 0, "FP losses next 3 turns"}, {O_LOST3 + 1, "Shadow losses next 3 turns"}, {O_SIEGE3, "FP strongholds besieged in 3 turns"},
                {O_MORDOR_COMP, "companions at Mordor entry"}, {O_TILES3, "Hunt tiles next 3 turns"}};
            printf("  prediction skill (0 = no better than the average, 1 = perfect):");
            for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
                int k = F[i].k; double n = s->fc_n[k];
                if (n < 50) { printf(" %s n/a;", F[i].name); continue; }
                double var = s->fc_t2[k] / n - (s->fc_t[k] / n) * (s->fc_t[k] / n);
                printf(" %s %.2f;", F[i].name, var > 1e-9 ? 1 - (s->fc_e2[k] / n) / var : 0);
            }
            printf("\n");
        }
        {   /* end-of-game ownership forecast. Skill = how much better than always guessing the base rate (0 = no better, 1 = perfect). */
            double n = (double)s->vs_n, bs = 0, ref = 0;
            for (int k = 0; k < NOWN; k++) { double z = s->own_z[k] / n; bs += s->own_bs[k] / n; ref += z * (1 - z); }
            printf("  ownership forecast (enemy holds it at game end), skill %.2f; predicted -> actual:", ref > 0 ? 1 - bs / ref : 0);
            for (int k = 0; k < NOWN; k++) if (s->own_z[k] / n >= 0.03) printf(" %s %.0f%%->%.0f%%;", REGION[g_vps[k]].name, 100 * s->own_p[k] / n, 100 * s->own_z[k] / n);
            printf("\n");
        }
        printf("  calibration (predicted P(FP wins) -> actual):");
        for (int k = 0; k < 5; k++) if (s->cal_n[k]) printf(" %.0f%%->%.0f%% (%.0f%% of positions)", 100 * s->cal_p[k] / (double)s->cal_n[k], 100 * s->cal_z[k] / (double)s->cal_n[k], 100.0 * s->cal_n[k] / (double)s->vs_n);
        printf("\n");
    }
    printf("  strategy formula still at %.0f%% of its starting strength\n", 100 * learner->fp.p.alpha);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }

/* one pass of Adam over all samples of the window, in shuffled order */
static void fit(Player *p, Arena *arenas, int na, float lr, uint64_t seed, double *loss_out, int only) {
    long total = 0;
    for (int i = 0; i < na; i++) total += arenas[i].count;
    const uint8_t **ptr = malloc(sizeof(*ptr) * (size_t)(total ? total : 1));
    long k = 0;
    for (int i = 0; i < na; i++) {
        size_t off = 0;
        while (off < arenas[i].len) { SHdr h; memcpy(&h, arenas[i].buf + off, sizeof h); ptr[k++] = arenas[i].buf + off; off += sizeof h + (size_t)h.n * 6; }
    }
    uint64_t r = seed | 1;
    for (long i = total - 1; i > 0; i--) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; long q = (long)(r % (uint64_t)(i + 1)); const uint8_t *t = ptr[i]; ptr[i] = ptr[q]; ptr[q] = t; }
    int cnt[2 + 2 * NSPEC] = {0}; double loss = 0; long used = 0;
    Feat f; float tgt[NOUT]; int net;
    for (long i = 0; i < total; i++) {
        { SHdr h; memcpy(&h, ptr[i], sizeof h); if (only == 2 ? h.net < 2 : h.net != only) continue; }      /* only: FP or SH (a main network), or 2 (all the specialists) */
        sample_read(ptr[i], &f, &net, tgt);
        Net *n = net == FP ? &p->fp : net == SH ? &p->sh : &p->spec[(net - 2) / 2][(net - 2) & 1];
        loss += nn_backward(n, &f, tgt, 1.0f); used++;
        if (++cnt[net] == 128) { nn_step(n, lr, 128); cnt[net] = 0; }
    }
    for (int q = 0; q < 2 + 2 * NSPEC; q++) if (cnt[q]) nn_step(q == FP ? &p->fp : q == SH ? &p->sh : &p->spec[(q - 2) / 2][(q - 2) & 1], lr, cnt[q]);
    free(ptr);
    *loss_out = used ? loss / (double)used : 0;
}

static void fit_policy(Player *p, Arena *arenas, int na, float lr, double *loss_out) {
    static _Thread_local Feat f; static _Thread_local Move mv[MAXMOVES]; static _Thread_local float tg[MAXMOVES];
    float acc[H1]; int cnt[2] = {0, 0}; long total = 0; double loss = 0;
    for (int i = 0; i < na; i++) {
        size_t off = 0;
        while (off < arenas[i].len) {
            PHdr h; const uint8_t *q = arenas[i].buf + off;
            memcpy(&h, q, sizeof h); q += sizeof h;
            f.n = h.nf; memcpy(f.idx, q, (size_t)h.nf * 2); q += (size_t)h.nf * 2; memcpy(f.val, q, (size_t)h.nf * 4); q += (size_t)h.nf * 4;
            memcpy(mv, q, (size_t)h.nm * sizeof(Move)); q += (size_t)h.nm * sizeof(Move); memcpy(tg, q, (size_t)h.nm * sizeof(float)); q += (size_t)h.nm * sizeof(float);
            off = (size_t)(q - arenas[i].buf);
            nn_root(h.net == FP ? &p->fp : &p->sh, &f, acc);
            for (int k = 0; k < H1; k++) if (acc[k] < 0) acc[k] = 0;
            loss += pol_backward(&p->pol[h.net], acc, h.dkind, mv, h.nm, tg); total++;
            if (++cnt[h.net] == 32) { pol_step(&p->pol[h.net], lr, 32); cnt[h.net] = 0; }
        }
    }
    for (int k = 0; k < 2; k++) if (cnt[k]) pol_step(&p->pol[k], lr, cnt[k]);
    *loss_out = total ? loss / (double)total : 0;
}

static Player *cur;                 /* the one version being trained */
static Player g_frozen;             /* WOTR_BALANCE: the side that is ahead, as it stood when it was frozen */
static Player *g_frz[6]; static int g_nfrz;      /* WOTR_FREEZE_*="a,b,c": several frozen opponents, dealt out among the threads and rotated every round (a mix, so that what is learned against one is not unlearned against the next) */
static Player g_snap;               /* a frozen copy of it: plays this round's games while the next fit runs */
static Player *hist[4]; static int nhist;       /* its last four earlier selves, kept as opponents */

/* the two value nets are independent, so each is fitted on its own thread */
typedef struct { Player *p; Arena *arenas; int na; float lr; uint64_t seed; double loss; int only; } FitJob;
static void *fit_worker(void *arg) { FitJob *j = arg; fit(j->p, j->arenas, j->na, j->lr, j->seed, &j->loss, j->only); return 0; }
static void strip_optimizer(Player *p) {
    p->fp.m = p->fp.v = p->fp.g = 0; p->sh.m = p->sh.v = p->sh.g = 0;
    for (int g = 0; g < NSPEC; g++) for (int k = 0; k < 2; k++) p->spec[g][k].m = p->spec[g][k].v = p->spec[g][k].g = 0;
    for (int k = 0; k < 2; k++) p->pol[k].m = p->pol[k].v = p->pol[k].g = 0;
}
static void hist_push(const Player *p) {
    if (nhist < 4) hist[nhist++] = malloc(sizeof(Player));
    else { Player *q = hist[0]; memmove(hist, hist + 1, 3 * sizeof(Player *)); hist[3] = q; }
    memcpy(hist[nhist - 1], p, sizeof(Player)); strip_optimizer(hist[nhist - 1]);
}

/* Each round: the frozen copy plays the games (a third of the threads against itself, the rest one side of it against
 * one of its earlier selves or the untrained formula) while the version in training is fitted on the previous rounds'
 * games; then the copy is refreshed. Every OLD rounds the version is saved and joins the earlier selves. */
/* Human boards with who went on to win (tools/humanlogs/starts.py). Used by `wotr lessons` (a one-off fit) and by
   WOTR_LESSONS="<file>,<share>" in training: every round that share of the fit's samples are human boards, so that
   self-play keeps one foot on how winning positions look in human games (left to itself Shadow drifted to
   corruption play within a stretch, even from a lessoned start). */
static int lessons_load(const char *path, State **stp, uint8_t **labp) {
    FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "cannot read %s\n", path); return 0; }
    State *st = NULL, s; uint8_t *lab = NULL; int n = 0, cap = 0, bad = 0, nolabel = 0; char line[256];
    wotr_blank(&s);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "end", 3)) { wotr_set(&s, line); continue; }
        char id[100], w = '?', err[200]; int a, b;
        sscanf(line, "end %99s %d %d %c", id, &a, &b, &w);
        if (w != 'F' && w != 'S') nolabel++;
        else if (wotr_begin_turn(&s, 777 + (uint64_t)n, err, sizeof err)) bad++;
        else {
            if (n == cap) { cap = cap ? cap * 2 : 4096; st = realloc(st, (size_t)cap * sizeof(State)); lab = realloc(lab, (size_t)cap); }
            st[n] = s; lab[n++] = w == 'F';
        }
        wotr_blank(&s);
    }
    fclose(f);
    printf("human lessons: %d boards from %s (%d without a result, %d refused by the checker)\n", n, path, nolabel, bad);
    *stp = st; *labp = lab; return n;
}
static State *g_les; static uint8_t *g_les_lab; static int g_nles; static float g_les_share; static Arena g_les_arena;
/* WOTR_AWLES_SH / WOTR_AWLES_FP="<share>,<decisions file>[:<file>...]": the other bot's decisions (State + Move records,
   see cmd_awlearn) stay in every round's fit of that side's move-suggesting network - `share` as many lessons as the
   round's own samples. Without this, self-play fitting washes the lessons out within a run (41.5% -> 23% agreement). */
static State *g_aw_st[2]; static Move *g_aw_mv[2]; static long g_aw_n[2]; static float g_aw_share[2]; static Arena g_aw_arena;
static void awles_load(int side, const char *spec) {
    char buf[4000]; snprintf(buf, sizeof buf, "%s", spec);
    char *c = strchr(buf, ','); if (!c) return;
    *c = 0; g_aw_share[side] = (float)atof(buf);
    long cap = 0;
    for (char *t = strtok(c + 1, ":"); t; t = strtok(NULL, ":")) {
        FILE *f = fopen(t, "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", t); continue; }
        State st; Move m;
        while (fread(&st, sizeof st, 1, f) == 1 && fread(&m, sizeof m, 1, f) == 1) {
            if (st.dplayer != side) continue;
            if (g_aw_n[side] == cap) { cap = cap ? cap * 2 : 8192; g_aw_st[side] = realloc(g_aw_st[side], (size_t)cap * sizeof(State)); g_aw_mv[side] = realloc(g_aw_mv[side], (size_t)cap * sizeof(Move)); }
            g_aw_st[side][g_aw_n[side]] = st; g_aw_mv[side][g_aw_n[side]++] = m;
        }
        fclose(f);
    }
    printf("lessons kept in every round for the %s move-suggester: %ld decisions, share %.2f\n", side ? "Shadow" : "Free Peoples", g_aw_n[side], g_aw_share[side]);
}
static void awles_fit(Player *p, long nps, uint64_t seed) {
    static Feat ft; static Move mv[MAXMOVES]; static float tg[MAXMOVES]; double dummy;
    g_aw_arena.len = 0; g_aw_arena.count = 0;
    for (int side = 0; side < 2; side++) {
        long want = (long)(g_aw_share[side] * (float)nps);
        for (long q = 0; q < want && g_aw_n[side]; q++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            long x = (long)(seed % (uint64_t)g_aw_n[side]); const State *st = &g_aw_st[side][x]; Move am = g_aw_mv[side][x];
            int k = wotr_moves(st, mv), ai = -1, exact = 0;
            if (k < 2) continue;
            for (int i = 0; i < k; i++) if (mv[i].type == am.type && mv[i].a == am.a && mv[i].b == am.b) { int ex = mv[i].c == am.c && mv[i].d == am.d; if (ai < 0 || (ex && !exact)) { ai = i; exact = ex; } }
            if (ai < 0) continue;
            for (int i = 0; i < k; i++) tg[i] = i == ai;
            feat_encode(st, &ft);
            parena_push(&g_aw_arena, &ft, side, st->dkind, mv, tg, k);
        }
    }
    if (g_aw_arena.count) fit_policy(p, &g_aw_arena, 1, 2e-3f, &dummy);
}

static int cmd_train(int argc, char **argv) {
    const char *dir = argv[2];
    int iters = argc > 3 ? atoi(argv[3]) : 120, gpi = argc > 4 ? atoi(argv[4]) : 2000, nt = argc > 5 ? atoi(argv[5]) : 9;
    float half = argc > 6 ? (float)atof(argv[6]) : 480.0f;     /* rounds over which the strategy formula's share falls, in a straight line, from all to nothing */
    enum { WINDOW = 3, OLD = 20 };
    double sloss = 0; long spec_start = 0;      /* specialists: their loss last round; the round they were switched on */
    double sh_lately = -1;      /* Shadow's share of wins over the last few rounds (a running average) */
    int catchup = -1;           /* WOTR_BALANCE: the side that is behind and alone in learning (-1: neither) */
    float lr = 3e-4f, temp = 0.03f, keep = gpi >= 6000 ? 0.03f : 0.08f, full_frac = 1.0f;
    char path[512], rp[600];
    long done = 0;                      /* rounds trained in this folder so far, across restarts: the fade depends on it */
    if (getenv("WOTR_TD")) g_td = (float)atof(getenv("WOTR_TD"));
    if (getenv("WOTR_START_SHARE")) g_start_share = (float)atof(getenv("WOTR_START_SHARE"));
    if (getenv("WOTR_STARTS") && starts_load(getenv("WOTR_STARTS"))) { fprintf(stderr, "cannot read %s\n", getenv("WOTR_STARTS")); return 1; }
    if (getenv("WOTR_STATES") && states_load(getenv("WOTR_STATES"))) { fprintf(stderr, "cannot read %s\n", getenv("WOTR_STATES")); return 1; }
    if (getenv("WOTR_FLOOR")) g_floor = (float)atof(getenv("WOTR_FLOOR"));
    if (getenv("WOTR_TEACH_POL")) g_teach_pol = atoi(getenv("WOTR_TEACH_POL"));
    if (getenv("WOTR_TEACH")) sscanf(getenv("WOTR_TEACH"), "%d,%d,%d,%f,%f", &g_teach[0], &g_teach[1], &g_teach[2], &g_teach_share, &g_teach_part);
    if (getenv("WOTR_SPEC_FADE")) g_spec_fade = (float)atof(getenv("WOTR_SPEC_FADE"));
    if (getenv("WOTR_ROLL_TEACH")) sscanf(getenv("WOTR_ROLL_TEACH"), "%f,%f,%f,%f,%f", &g_rteach[0], &g_rteach[1], &g_rteach[2], &g_rteach[3], &g_rteach[4]);
    if (getenv("WOTR_STYLE")) sscanf(getenv("WOTR_STYLE"), "%f,%f", &g_style, &g_style_s);
    if (getenv("WOTR_AWLES_SH")) awles_load(SH, getenv("WOTR_AWLES_SH"));
    if (getenv("WOTR_AWLES_FP")) awles_load(FP, getenv("WOTR_AWLES_FP"));
    if (getenv("WOTR_LESSONS")) { char pth[600]; if (sscanf(getenv("WOTR_LESSONS"), "%599[^,],%f", pth, &g_les_share) == 2) g_nles = lessons_load(pth, &g_les, &g_les_lab); }
    if (getenv("WOTR_LAMBDA")) g_lambda = (float)atof(getenv("WOTR_LAMBDA"));
    if (getenv("WOTR_ABSORB")) g_absorb = (float)atof(getenv("WOTR_ABSORB"));
    if (getenv("WOTR_DEEP_OPEN")) g_dopen = atoi(getenv("WOTR_DEEP_OPEN"));
    if (getenv("WOTR_DEEP_GAP")) g_dgap = (float)atof(getenv("WOTR_DEEP_GAP"));
    if (getenv("WOTR_OPEN")) g_open = (float)atof(getenv("WOTR_OPEN"));
    if (getenv("WOTR_TEMP_SH")) g_temp_sh = (float)atof(getenv("WOTR_TEMP_SH"));
    if (getenv("WOTR_BALANCE")) g_balance = (float)atof(getenv("WOTR_BALANCE"));
    /* WOTR_FREEZE_SH / WOTR_FREEZE_FP=<player>: that side's seat is taken, in every training game of the run, by the
       given player unchanged; the player in training plays only the other side. For training the two sides as
       separate players: a side that is not being changed cannot drift. */
    { const char *fs = getenv("WOTR_FREEZE_SH"), *ff = getenv("WOTR_FREEZE_FP"); const char *fz = fs ? fs : ff;
      if (fz) {
          char list[1000]; snprintf(list, sizeof list, "%s", fz);
          for (char *t = strtok(list, ","); t && g_nfrz < 6; t = strtok(NULL, ",")) { g_frz[g_nfrz] = malloc(sizeof(Player)); if (player_load(g_frz[g_nfrz], t)) { fprintf(stderr, "cannot load %s\n", t); return 1; } g_nfrz++; }
          if (!g_nfrz) { fprintf(stderr, "no player in %s\n", fz); return 1; }
          memcpy(&g_frozen, g_frz[0], sizeof(Player));
          catchup = fs ? FP : SH; printf("%s is played throughout by %s; only the other side is trained\n", fs ? "Shadow" : "the Free Peoples", fz); } }
    if (getenv("WOTR_REW")) { const char *v = getenv("WOTR_REW"); int k; float x; while (*v && sscanf(v, "%d:%f", &k, &x) == 2) { if (k >= 0 && k < NRW) feat_rw[k] = x; v = strchr(v, ','); if (!v) break; v++; } }
    printf("partial rewards:"); for (int k = 0; k < NRW; k++) printf(" %d:%g", k, feat_rw[k]); printf("\n");
    if (getenv("WOTR_LR")) lr = (float)atof(getenv("WOTR_LR"));             /* learning rate (3e-4) */
    if (getenv("WOTR_TEMP")) temp = (float)atof(getenv("WOTR_TEMP"));       /* exploration: how readily a slightly worse move is tried (0.03) */
    printf("settings: learning rate %g, exploration %g (Shadow %g), partial rewards x%g, formula share never below %g\n", lr, temp, g_temp_sh > 0 ? g_temp_sh : temp, g_td, g_floor);
    g_falls = getenv("WOTR_FALLS");
    mkdir(dir, 0755);
    cur = malloc(sizeof(Player));
    Player *anchor = malloc(sizeof(Player));       /* the untrained formula: a fixed opponent */
    player_load(anchor, "prior");
    snprintf(path, sizeof path, "%s/latest", dir); snprintf(rp, sizeof rp, "%s/rounds", dir);
    if (player_load(cur, path)) { player_load(cur, "prior"); printf("starting from the untrained strategy formula\n"); }
    else {
        FILE *f = fopen(rp, "r"); if (f) { if (fscanf(f, "%ld", &done) != 1) done = 0; fclose(f); }
        printf("resuming from %s after %ld rounds\n", path, done); full_frac = g_full;
        if (g_spec) {       /* the round the specialists were switched on: their fade is counted from it */
            char sp[600]; snprintf(sp, sizeof sp, "%s/spec_start", dir); FILE *sf = fopen(sp, "r");
            if (sf) { if (fscanf(sf, "%ld", &spec_start) != 1) spec_start = done; fclose(sf); }
            else { spec_start = done; sf = fopen(sp, "w"); if (sf) { fprintf(sf, "%ld\n", spec_start); fclose(sf); } }
            printf("specialist networks on since round %ld; their formula share and nudges fade to nothing over %.0f rounds\n", spec_start, g_spec_fade);
        }
        for (long c = done / OLD - 3; c <= done / OLD; c++) if (c > 0) {      /* its saved earlier selves */
            char cp[600]; Player *p = malloc(sizeof(Player)); snprintf(cp, sizeof cp, "%s/ckpt%04ld", dir, c * OLD);
            if (!player_load(p, cp)) hist_push(p);
            free(p);
        }
    }
    printf("strategy formula fades in a straight line to nothing over %.0f rounds; partial rewards at %.0f%% of their design; %d earlier versions loaded as opponents\n", half, 100 * g_td, nhist);
    static Arena win[WINDOW][64], pwin[2][64], fitset[WINDOW * 64];
    double t0 = now(), loss = 0, ploss = 0;
    long total_games = 0;
    for (int it = 0; it < iters; it++, done++) {
        static Job jobs[64]; memset(jobs, 0, sizeof jobs);
        int slot = it % WINDOW;
        cur->fp.p.alpha = cur->sh.p.alpha = done < half ? 1.0f - (float)done / half : 0.0f;
        if (cur->fp.p.alpha < g_floor) cur->fp.p.alpha = cur->sh.p.alpha = g_floor;
        if (g_spec) { float sa = cur->fp.p.alpha * (1.0f - (float)(done - spec_start) / g_spec_fade); if (sa < 0) sa = 0; for (int g = 0; g < NSPEC; g++) cur->spec[g][FP].p.alpha = cur->spec[g][SH].p.alpha = sa; }
        memcpy(&g_snap, cur, sizeof(Player)); strip_optimizer(&g_snap);
        if (g_balance > 0 && sh_lately >= 0) {
            int was = catchup;
            if (catchup < 0) catchup = sh_lately < 100 - g_balance ? SH : sh_lately > g_balance ? FP : -1;
            else if (catchup == SH ? sh_lately >= 50 : sh_lately <= 50) catchup = -1;
            if (catchup >= 0 && was < 0) memcpy(&g_frozen, &g_snap, sizeof(Player));
            if (catchup != was) sh_lately = -1;         /* the share is measured afresh under the new arrangement */
            printf("balance (a side winning more than %.0f%% is frozen until it is down to 50%%): %s\n", g_balance, catchup < 0 ? "both sides learning" : catchup == SH ? "Shadow is behind: it learns against the Free Peoples as they stood when this began" : "the Free Peoples are behind: they learn against Shadow as it stood when this began");
        }
        for (int i = 0; i < nt; i++) {
            Arena *a = &win[slot][i], *pa = &pwin[it & 1][i];
            a->len = 0; a->count = 0; pa->len = 0; pa->count = 0;
            jobs[i].pl[FP] = jobs[i].pl[SH] = &g_snap; jobs[i].diag = &g_snap;
            if (i % 3) {        /* two thirds of the threads: one side (alternating) meets an earlier self or the untrained formula */
                int o = (it + i) & 1, q = (it * 7 + i) % (nhist + 1);
                jobs[i].pl[o] = q == nhist ? anchor : hist[q];
            }
            if (catchup >= 0) { jobs[i].pl[catchup] = &g_snap; jobs[i].pl[1 - catchup] = g_nfrz > 1 ? g_frz[(it + i) % g_nfrz] : &g_frozen; }
            jobs[i].games = gpi / nt; jobs[i].samples = 1; jobs[i].temp = temp; jobs[i].keep = keep;
            jobs[i].topk = 8; jobs[i].nrand = 2; jobs[i].full_frac = full_frac;
            jobs[i].fast_frac = it < 3 ? 0 : 0.4f; jobs[i].fastk = 3;     /* most decisions get a quick look once the policy has some shape */
            jobs[i].pkeep = 0.05f / full_frac > 1 ? 1 : 0.05f / full_frac;
            jobs[i].arena = a; jobs[i].parena = pa;
            jobs[i].seed = (uint64_t)time(0) * 1000003ULL + (uint64_t)it * 100000ULL + (uint64_t)i * 10000ULL;
        }
        double t1 = now();
        start_jobs(jobs, nt);
        long ns = 0, nps = 0;
        if (it > 0) {                                   /* fit on the finished rounds while the new one is played */
            int n = 0;
            for (int w = 0; w < WINDOW; w++) if (w != slot) for (int i = 0; i < 64; i++) if (win[w][i].count) { fitset[n++] = win[w][i]; ns += win[w][i].count; }
            if (g_nles && ns > 0) {                     /* human boards, a fresh sample each round, for both networks */
                static Feat lf; float tg[NOUT]; uint64_t q = (uint64_t)it * 0x9E3779B97F4A7C15ULL + 77;
                long want = (long)(g_les_share * (float)ns / 2);
                g_les_arena.len = 0; g_les_arena.count = 0;
                for (long i = 0; i < want; i++) {
                    q ^= q << 13; q ^= q >> 7; q ^= q << 17;
                    int k = (int)(q % (uint64_t)g_nles);
                    feat_encode(&g_les[k], &lf);
                    for (int o = 0; o < NOUT; o++) tg[o] = -100;
                    tg[O_WIN] = g_les_lab[k];
                    arena_push(&g_les_arena, &lf, 0, tg); arena_push(&g_les_arena, &lf, 1, tg);
                }
                fitset[n++] = g_les_arena; ns += g_les_arena.count;
            }
            FitJob fj[3]; pthread_t ft[3];
            nn_train_begin(&cur->fp); nn_train_begin(&cur->sh);
            if (g_spec) for (int g = 0; g < NSPEC; g++) { nn_train_begin(&cur->spec[g][FP]); nn_train_begin(&cur->spec[g][SH]); }   /* before the threads start: each thread used to allocate both nets' buffers, and one could find the other's half done */
            for (int k = 0; k < 2; k++) {
                FitJob x = {cur, fitset, n, lr, (uint64_t)it * 977 + 13 + (uint64_t)k, 0, k}; fj[k] = x;
                pthread_create(&ft[k], 0, fit_worker, &fj[k]);
            }
            if (g_spec) { FitJob x = {cur, fitset, n, lr, (uint64_t)it * 977 + 15, 0, 2}; fj[2] = x; pthread_create(&ft[2], 0, fit_worker, &fj[2]); }
            for (int k = 0; k < 2; k++) pthread_join(ft[k], 0);
            if (g_spec) { pthread_join(ft[2], 0); sloss = fj[2].loss; }
            loss = 0.5 * (fj[0].loss + fj[1].loss);
            /* the policy reads the value nets' hidden layer, so it is fitted after them */
            for (int i = 0; i < 64; i++) nps += pwin[(it - 1) & 1][i].count;
            fit_policy(cur, pwin[(it - 1) & 1], 64, 2e-3f, &ploss);
            if (g_aw_n[0] || g_aw_n[1]) awles_fit(cur, nps, (uint64_t)it * 7919 + 3);
        }
        double t2 = now();
        join_jobs(nt);
        double t3 = now();
        Stats st; memset(&st, 0, sizeof st);
        for (int i = 0; i < nt; i++) stats_add(&st, &jobs[i].st);
        total_games += st.games;
        double recall = st.pol_full ? (double)st.pol_hit / (double)st.pol_full : 0;
        printf("it %d  %.1fs wall: %.0f games/s (fit %.1fs on %ldk value + %ldk policy samples, value loss %.4f, policy loss %.3f)  %ldk games, %.0f min\n",
               it, t3 - t1, st.games / (t3 - t1), t2 - t1, ns / 1000, nps / 1000, loss, ploss, total_games / 1000, (now() - t0) / 60);
        stats_print(&st, "  self-play");
        if (g_spec) printf("  specialist networks: loss %.4f, formula share and nudges at %.0f%% for their decisions\n", sloss, 100 * cur->spec[0][0].p.alpha);
        { double sh = 100.0 * (st.wins[2] - st.wt[WIN_NONE]) / (double)st.games; sh_lately = sh_lately < 0 ? sh : 0.8 * sh_lately + 0.2 * sh; }
        report(&st);
        diagnostics(&st, &g_snap);
        { const char *wotr_dkind_name(int); double te = 0; for (int k = 1; k < ND; k++) te += st.evk[k]; printf("  positions scored per game: %.0f;", te / st.games); for (int k = 1; k < ND; k++) if (st.evk[k] > 0.02 * te) printf(" %s %.0f decisions/%.0f", wotr_dkind_name(k), (double)st.dk[k] / st.games, (double)st.evk[k] / st.games); printf("\n    action decisions per game x positions scored at each, by tier (few options / full width / top-k / quick):"); for (int q = 0; q < 4; q++) printf(" %.0f x %.0f (%.0f moves)", (double)st.atier[q] / st.games, st.atier[q] ? (double)st.atier_ev[q] / st.atier[q] : 0, st.atier[q] ? (double)st.atier_nc[q] / st.atier[q] : 0); printf("\n"); }
        printf("  policy: best move in top-8 %.1f%%, log-odds given up by top-8 pruning %.3f (%ldk full-width decisions, full-width fraction %.2f)\n",
               100 * recall, st.pol_full ? st.pol_regret / (double)st.pol_full : 0, st.pol_full / 1000, full_frac);
        fflush(stdout);
        full_frac = it == 0 ? 0.5f : it == 1 ? 0.25f : g_full;
        player_save(cur, path);
        { FILE *f = fopen(rp, "w"); if (f) { fprintf(f, "%ld\n", done + 1); fclose(f); } }
        if ((done + 1) % OLD == 0) {
            char cp[600]; snprintf(cp, sizeof cp, "%s/ckpt%04ld", dir, done + 1); player_save(cur, cp);
            hist_push(cur);
        }
    }
    return 0;
}

static int cmd_eval(int argc, char **argv) {
    int games = argc > 4 ? atoi(argv[4]) : 200, nt = argc > 5 ? atoi(argv[5]) : 8, topk = argc > 6 ? atoi(argv[6]) : 0;   /* topk > 0: policy-pruned search */
    Player *a = malloc(sizeof(Player)), *b = malloc(sizeof(Player));
    if (player_load(a, argv[2]) || player_load(b, argv[3])) { fprintf(stderr, "cannot load nets\n"); return 1; }
    /* WOTR_ALPHA_FP / WOTR_ALPHA_SH=<x>: play that seat's player with this share of the strategy formula instead of the one
       it was saved with (a measurement of how much of its play is the formula and how much the network) */
    if (getenv("WOTR_STATES")) { if (states_load(getenv("WOTR_STATES"))) { fprintf(stderr, "cannot read %s\n", getenv("WOTR_STATES")); return 1; } g_eval_starts = 1; }
    if (getenv("WOTR_STARTS")) { if (starts_load(getenv("WOTR_STARTS"))) { fprintf(stderr, "cannot read %s\n", getenv("WOTR_STARTS")); return 1; } g_eval_starts = 1; }   /* test games from human boards (the backward sweep is judged from its own stage's boards) */
    if (getenv("WOTR_ALPHA_FP")) a->fp.p.alpha = a->sh.p.alpha = (float)atof(getenv("WOTR_ALPHA_FP"));
    if (getenv("WOTR_ALPHA_SH")) b->fp.p.alpha = b->sh.p.alpha = (float)atof(getenv("WOTR_ALPHA_SH"));
    if (getenv("WOTR_HUMAN_FP")) agent_hum[FP] = (float)atof(getenv("WOTR_HUMAN_FP"));
    if (getenv("WOTR_HUMAN_SH")) agent_hum[SH] = (float)atof(getenv("WOTR_HUMAN_SH"));
    if (getenv("WOTR_PROBE_ELITE")) agent_probe = (float)atof(getenv("WOTR_PROBE_ELITE"));
    if (getenv("WOTR_PROBE_SIEGE")) agent_probe_siege = (float)atof(getenv("WOTR_PROBE_SIEGE"));
    Job jobs[64]; memset(jobs, 0, sizeof jobs);
    for (int i = 0; i < nt; i++) { jobs[i].pl[FP] = a; jobs[i].pl[SH] = b; jobs[i].games = games / nt; jobs[i].samples = 3; jobs[i].temp = 0.02f; jobs[i].topk = topk; jobs[i].nrand = 0; jobs[i].full_frac = 0; jobs[i].seed = 777000ULL + (uint64_t)i * 10000ULL; }
    double t = now();
    run_jobs(jobs, nt);
    Stats st; memset(&st, 0, sizeof st);
    for (int i = 0; i < nt; i++) stats_add(&st, &jobs[i].st);
    char tag[600]; snprintf(tag, sizeof tag, "FP=%s vs SH=%s", argv[2], argv[3]);
    stats_print(&st, tag);
    report(&st);
    { const char *wotr_dkind_name(int); printf("  per game:"); for (int k = 1; k < ND; k++) if (st.dk[k]) printf(" %s %.0f dec/%.0fk ev", wotr_dkind_name(k), (double)st.dk[k] / st.games, st.evk[k] / 1e3 / st.games); printf("\n"); }
    if (agent_roll_decisions) printf("  rollout chooser: %ld decisions, %.0f rollouts each on average, %.1f ms each on average, longest %.1f ms\n", agent_roll_decisions, (double)agent_roll_count / agent_roll_decisions, agent_roll_ms_sum / agent_roll_decisions, agent_roll_ms_max);
    printf("  %.1f games/s, %.2f ms per decision\n", st.games / (now() - t), 1e3 * (now() - t) * nt / (double)st.decisions);
    return 0;
}

/* print one game move by move */
static int cmd_show(int argc, char **argv) {
    Player *a = malloc(sizeof(Player));
    if (player_load(a, argv[2])) { fprintf(stderr, "cannot load nets\n"); return 1; }
    uint64_t seed = argc > 3 ? strtoull(argv[3], 0, 10) : 1;
    State s; wotr_init(&s, seed);
    /* optional 5th argument: a different player for Shadow */
    Player *b = a;
    if (argc > 5) { b = malloc(sizeof(Player)); if (player_load(b, argv[5])) { fprintf(stderr, "cannot load nets\n"); return 1; } }
    static Agent ags[2]; memset(ags, 0, sizeof ags);
    for (int k = 0; k < 2; k++) { Player *pl = k == FP ? a : b; ags[k].net[FP] = &pl->fp; ags[k].net[SH] = &pl->sh; ags[k].samples = 3; ags[k].temp = 0.02f; ags[k].rng = seed * 31 + 1 + (uint64_t)k; }
    char buf[200]; int turn = 0;
    while (!s.winner) {
        float bv; int n;
        if (s.turn != turn && s.dkind == D_ACTION) { turn = s.turn; wotr_print(&s); }
        Agent *agp = &ags[s.dplayer];
#define ag (*agp)
        Move m = agent_choose(&ag, &s, &bv, &n);
        wotr_move_str(&s, m, buf, sizeof buf);
        if (n > 1) printf("%s %-60s (%d options, P(FP)=%.2f)\n", s.dplayer ? "  SH" : "FP  ", buf, n, nn_prob(bv));
        if (n > 1 && argc > 4 && (s.dkind == D_ADVANCE || s.dkind == D_ENTER_MORDOR || s.dkind == D_HUNT_ALLOC)) for (int i = 0; i < n; i++) {
            State u = s; Feat fu; wotr_apply(&u, ag.last_mv[i]); feat_encode(&u, &fu);
            printf("        option %d: %+.3f | next decision %d by %d | prior terms:", ag.last_mv[i].a, ag.last_val[i], u.dkind, u.dplayer);
            for (int k = 0; k < NPRIOR; k++) printf(" %.2f", fu.prior[k]);
            printf("\n");
        }
        if (n > 1 && argc > 4 && s.dkind == D_ACTION) {     /* verbose: the three best alternatives and the Fellowship move */
            for (int k = 0; k < 3; k++) {
                int bi = -1;
                for (int i = 0; i < n; i++) if (ag.last_val[i] > -900 && (bi < 0 || (s.dplayer == FP ? ag.last_val[i] > ag.last_val[bi] : ag.last_val[i] < ag.last_val[bi]))) bi = i;
                if (bi < 0) break;
                char b2[200]; wotr_move_str(&s, ag.last_mv[bi], b2, sizeof b2); printf("        %+.3f %s\n", ag.last_val[bi], b2); ag.last_val[bi] = -999;
            }
            for (int i = 0; i < n; i++) if (ag.last_mv[i].type == M_FSP_MOVE || ag.last_mv[i].type == M_FSP_HIDE || ag.last_mv[i].type == M_ATTACK) {
                char b2[200]; wotr_move_str(&s, ag.last_mv[i], b2, sizeof b2); if (ag.last_val[i] > -900) printf("        %+.3f   also: %s\n", ag.last_val[i], b2);
            }
        }
        if (n > 1 && getenv("WOTR_SHOWALL") && s.dkind == D_ACTION && s.dplayer == SH && s.turn <= atoi(getenv("WOTR_SHOWALL")))      /* every option with its score, for diagnosis */
            for (int i = 0; i < n; i++) { char b2[200]; wotr_move_str(&s, ag.last_mv[i], b2, sizeof b2); printf("        all: %+.3f %s\n", ag.last_val[i], b2); }
        wotr_apply(&s, m);
#undef ag
    }
    wotr_print(&s);
    printf("winner: %s (type %d) on turn %d\n", s.winner == 1 ? "Free Peoples" : "Shadow", s.win_type, s.turn);
    return 0;
}

/* wotr lessons <player in> <player out> <positions file> [epochs=2] [learning rate=1e-4]
   Lessons from human games: every end-of-turn board of the decoded human games (tools/humanlogs/starts.py) with who
   went on to win. Both of the player's value networks are fitted a little toward "who won from here" on those boards
   (their own estimate, like all training; the formula share is untouched). It is a start for self-play to build on:
   what a winning Shadow position looks like on turn 3 or 4 is hard to find by playing oneself. */
static int cmd_lessons(int argc, char **argv) {
    feat_init();
    Player *pl = malloc(sizeof(Player));
    if (player_load(pl, argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
    int epochs = argc > 5 ? atoi(argv[5]) : 2; float lr = argc > 6 ? (float)atof(argv[6]) : 1e-4f;
    State *st = NULL; uint8_t *lab = NULL; int n = lessons_load(argv[4], &st, &lab);
    if (!n) { fprintf(stderr, "no labelled positions in %s\n", argv[4]); return 1; }
    nn_train_begin(&pl->fp); nn_train_begin(&pl->sh);
    static Feat ft; float tgt[NOUT]; uint64_t r = 0x9E3779B97F4A7C15ULL;
    int *ord = malloc((size_t)n * sizeof(int)); for (int i = 0; i < n; i++) ord[i] = i;
    for (int e = 0; e < epochs; e++) {
        double loss = 0;
        for (int i = n - 1; i > 0; i--) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; int j = (int)(r % (uint64_t)(i + 1)), t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
        for (int i = 0; i < n; i++) {
            feat_encode(&st[ord[i]], &ft);
            for (int k = 0; k < NOUT; k++) tgt[k] = -100;
            tgt[O_WIN] = lab[ord[i]];
            loss += nn_backward(&pl->fp, &ft, tgt, 1.0f); nn_backward(&pl->sh, &ft, tgt, 1.0f);
            if (i % 64 == 63 || i == n - 1) { int bsz = i % 64 + 1; nn_step(&pl->fp, lr, bsz); nn_step(&pl->sh, lr, bsz); }
        }
        printf("lessons, pass %d: %d positions, loss %.4f\n", e + 1, n, loss / n);
    }
    player_save(pl, argv[3]);
    return 0;
}

/* ---- lessons from human MOVES (tools/humanlogs/moves.py): the board before each action die a human spent, and what they did with it ---- */
typedef struct { uint8_t kind, a, b, c, d, held, turn; } HAct;      /* kind: 0 army, 1 muster, 2 diplo, 3 draw, 4 fsp, 5 attack on region a */
static int hact_match(const HAct *h, const Move *mv, int n, float *tg) {      /* marks the engine's moves that are what the human did; returns how many */
    int k = 0, exact = 0;
    for (int pass = 0; pass < 2 && !k; pass++) {          /* first the exact move (unit counts too), else any move along the same road / to the same place */
        for (int i = 0; i < n; i++) {
            Move m = mv[i]; int ok = 0;
            switch (h->kind) {
            case 0: ok = (m.type == M_ARMY || m.type == M_ATTACK) && m.a == h->a && m.b == h->b && (pass || (m.c == h->c && m.d == h->d)); break;
            case 1: ok = h->b == 3 ? m.type == M_MINION && m.a == h->c && (pass || m.b == h->a)
                       : h->b == 2 ? m.type == M_MUSTER_ELITE && m.a == h->a
                       : m.type == M_MUSTER && m.a == h->a && (pass || m.b == h->b); break;
            case 2: ok = m.type == M_DIPLO && m.a == h->a; break;
            case 3: ok = m.type == M_DRAW && m.a == h->a; break;
            case 4: ok = m.type == M_FSP_MOVE; break;
            case 5: ok = m.type == M_ATTACK && m.b == h->a; break;
            }
            tg[i] = ok; k += ok;
        }
        exact = !pass;
    }
    (void)exact;
    for (int i = 0; i < n; i++) tg[i] = k ? tg[i] / (float)k : 0;
    return k;
}
static int hacts_load(const char *path, int side, State **stp, HAct **hp) {
    FILE *f = fopen(path, "r"); if (!f) { fprintf(stderr, "cannot read %s\n", path); return 0; }
    State *st = NULL, s; HAct *ha = NULL, cur = {0}; int n = 0, cap = 0, bad = 0, as = -1; char line[256];
    wotr_blank(&s);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "act", 3)) {
            char kd[16]; int a = 0, b = 0, c = 0, d = 0;
            sscanf(line, "act %d %15s %d %d %d %d", &as, kd, &a, &b, &c, &d);
            cur.kind = !strcmp(kd, "army") ? 0 : !strcmp(kd, "muster") ? 1 : !strcmp(kd, "diplo") ? 2 : !strcmp(kd, "draw") ? 3 : !strcmp(kd, "attack") ? 5 : 4;
            cur.a = (uint8_t)a; cur.b = (uint8_t)b; cur.c = (uint8_t)c; cur.d = (uint8_t)d;
            continue;
        }
        if (strncmp(line, "end", 3)) { wotr_set(&s, line); continue; }
        char id[100], w, err[200]; int turn = 0, turns; unsigned hsh = 5381;
        sscanf(line, "end %99s %d %d %c", id, &turn, &turns, &w);
        for (char *q = id; *q; q++) hsh = hsh * 33 + (unsigned char)*q;
        if (as != side) { }
        else if (wotr_mid_turn(&s, side, 999 + (uint64_t)n, err, sizeof err)) bad++;
        else {
            if (n == cap) { cap = cap ? cap * 2 : 4096; st = realloc(st, (size_t)cap * sizeof(State)); ha = realloc(ha, (size_t)cap * sizeof(HAct)); }
            cur.held = hsh % 5 == 0; cur.turn = (uint8_t)turn;
            st[n] = s; ha[n++] = cur;
        }
        wotr_blank(&s);
    }
    fclose(f);
    printf("human moves: %d %s actions from %s (%d refused by the checker)\n", n, side ? "Shadow" : "Free Peoples", path, bad);
    *stp = st; *hp = ha; return n;
}
/* how well the move-suggesting network agrees with the humans on the games kept apart (a fifth of them, never trained on) */
static void hacts_measure(Player *pl, int side, const State *st, const HAct *ha, int n, const char *when) {
    static Feat ft; static Move mv[MAXMOVES]; static float tg[MAXMOVES], lg[MAXMOVES]; float acc[H1];
    long tot = 0, found = 0, top1 = 0, top6 = 0, early = 0, e1 = 0, e6 = 0; double rank = 0, nm = 0;
    for (int i = 0; i < n; i++) {
        if (!ha[i].held) continue;
        int k = wotr_moves(&st[i], mv); tot++;
        if (!hact_match(&ha[i], mv, k, tg)) continue;
        found++; nm += k;
        feat_encode(&st[i], &ft); nn_root(side == FP ? &pl->fp : &pl->sh, &ft, acc);
        for (int j = 0; j < H1; j++) if (acc[j] < 0) acc[j] = 0;
        pol_logits(&pl->pol[side], acc, st[i].dkind, mv, k, lg);
        float best = -1e30f; for (int j = 0; j < k; j++) if (tg[j] > 0 && lg[j] > best) best = lg[j];
        int r = 0; for (int j = 0; j < k; j++) if (lg[j] > best) r++;
        rank += r + 1; top1 += r == 0; top6 += r < 6;
        if (ha[i].turn <= 3) { early++; e1 += r == 0; e6 += r < 6; }
    }
    printf("%s: %ld held-out actions, the engine offers the human's move in %.1f%% (%.0f moves to choose from); of those the network puts it first %.1f%%, in its first six %.1f%%, average place %.1f; turns 1-3: first %.1f%%, first six %.1f%%\n",
           when, tot, 100.0 * found / (tot ? tot : 1), nm / (found ? found : 1), 100.0 * top1 / (found ? found : 1), 100.0 * top6 / (found ? found : 1), rank / (found ? found : 1), 100.0 * e1 / (early ? early : 1), 100.0 * e6 / (early ? early : 1));
}
/* wotr imitate <player in> <player out> <moves file> [passes=3] [lr=1e-3]   (WOTR_IM_SIDE=FP teaches the Free Peoples' network; default Shadow) */
static int cmd_imitate(int argc, char **argv) {
    Player *pl = malloc(sizeof(Player));
    if (player_load(pl, argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
    int side = getenv("WOTR_IM_SIDE") && !strcmp(getenv("WOTR_IM_SIDE"), "FP") ? FP : SH;
    int epochs = argc > 5 ? atoi(argv[5]) : 3; float lr = argc > 6 ? (float)atof(argv[6]) : 1e-3f;
    State *st = NULL; HAct *ha = NULL; int n = hacts_load(argv[4], side, &st, &ha);
    if (!n) return 1;
    hacts_measure(pl, side, st, ha, n, "before");
    static Feat ft; static Move mv[MAXMOVES]; static float tg[MAXMOVES]; float acc[H1]; uint64_t r = 0x9E3779B97F4A7C15ULL;
    int *ord = malloc((size_t)n * sizeof(int)); for (int i = 0; i < n; i++) ord[i] = i;
    for (int e = 0; e < epochs; e++) {
        double loss = 0; long used = 0; int cnt = 0;
        for (int i = n - 1; i > 0; i--) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; int j = (int)(r % (uint64_t)(i + 1)), t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
        for (int i = 0; i < n; i++) {
            int x = ord[i]; if (ha[x].held) continue;
            int k = wotr_moves(&st[x], mv);
            if (!hact_match(&ha[x], mv, k, tg)) continue;
            feat_encode(&st[x], &ft); nn_root(side == FP ? &pl->fp : &pl->sh, &ft, acc);
            for (int j = 0; j < H1; j++) if (acc[j] < 0) acc[j] = 0;
            loss += pol_backward(&pl->pol[side], acc, st[x].dkind, mv, k, tg); used++;
            if (++cnt == 32) { pol_step(&pl->pol[side], lr, 32); cnt = 0; }
        }
        if (cnt) pol_step(&pl->pol[side], lr, cnt);
        char when[64]; snprintf(when, sizeof when, "after pass %d (%ld actions, loss %.3f)", e + 1, used, loss / (used ? used : 1));
        hacts_measure(pl, side, st, ha, n, when);
    }
    if (strcmp(argv[3], "-")) player_save(pl, argv[3]);
    return 0;
}

/* wotr agree <player> <moves file>: in the positions where a human spent a die, what would our player do? (one-step choice, no look-ahead unless WOTR_DEEP_* is set) */
static int kind_of(Move m) {       /* 0 army move, 1 attack, 2 muster, 3 elite, 4 minion, 5 diplomacy, 6 draw, 7 Fellowship, 8 other */
    switch (m.type) { case M_ARMY: return 0; case M_ATTACK: return 1; case M_MUSTER: return 2; case M_MUSTER_ELITE: return 3; case M_MINION: return 4;
                      case M_DIPLO: return 5; case M_DRAW: return 6; case M_FSP_MOVE: return 7; default: return 8; }
}
long g_dwhy[2][6];
static int cmd_agree(int argc, char **argv) {
    static const char *KN[9] = {"army move", "attack", "muster", "elite", "minion", "diplomacy", "draw", "Fellowship", "other"};
    Player *pl = malloc(sizeof(Player));
    if (player_load(pl, argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
    int side = getenv("WOTR_IM_SIDE") && !strcmp(getenv("WOTR_IM_SIDE"), "FP") ? FP : SH, every = argc > 4 ? atoi(argv[4]) : 1;
    if (getenv("WOTR_HUMAN_SH")) agent_hum[SH] = (float)atof(getenv("WOTR_HUMAN_SH"));
    State *st = NULL; HAct *ha = NULL; int n = hacts_load(argv[3], side, &st, &ha);
    static Agent ag; memset(&ag, 0, sizeof ag);
    ag.net[FP] = &pl->fp; ag.net[SH] = &pl->sh; ag.samples = 3; ag.temp = 0; ag.rng = 12345;
    ag.depth = g_deep[side][0]; ag.width = g_deep[side][1]; ag.replies = g_deep[side][2]; ag.narrow = g_deep[side][3]; ag.dpol[FP] = &pl->pol[FP]; ag.dpol[SH] = &pl->pol[SH];
    enum { NB = 4 }; static const char *BN[NB] = {"turns 1-2", "turns 3-4", "turns 5-7", "turns 8+"};
    static long tot[NB], same[NB], hk[NB][9], ok[NB][9], mat[NB][9][9]; static double gap[NB][9];
    static double fterm[NPRIOR], fgap; static long fn, ereg[NREG]; static Feat fh, fo; float nn_formula(const float *prior);
    static long hroad[NREG][NREG], oroad[NREG][NREG], hatt[NB][NREG], oatt[NB][NREG];
    static Move mv[MAXMOVES]; static float tg[MAXMOVES];
    int vsroll = getenv("WOTR_VS_ROLL") ? atoi(getenv("WOTR_VS_ROLL")) : 0; static Agent ag2; static long vr_n[4], vr_same[4], vr_mat[9][9], vr_k1[9], vr_k2[9]; int vr_shown = 0;
    if (vsroll) { ag2 = ag; ag2.depth = 0; ag2.width = 0; ag2.roll_n = vsroll; ag2.roll_k = 24; ag2.roll_tau = 0.15f; ag2.narrow = 6; }
    static long miss[6], seen[6], why[4]; static long mroad[NREG][NREG];
    for (int i = 0; i < n; i += every) {
        int k = wotr_moves(&st[i], mv);
        seen[ha[i].kind]++;
        if (!hact_match(&ha[i], mv, k, tg)) {
            miss[ha[i].kind]++;
            if (ha[i].kind == 2 || ha[i].kind == 3) {
                static long dwhy[2][6]; const State *q = &st[i]; int d = ha[i].kind - 2, nat = ha[i].a;
                int md = q->dice[side][F_MUSTER] + q->dice[side][F_MA], ev = q->dice[side][F_EVENT], nd = 0; for (int f = 0; f < NFACE; f++) nd += q->dice[side][f];
                if (d == 0) dwhy[0][!md ? 0 : q->pol[nat] == 0 ? 1 : q->pol[nat] == 1 && !q->active[nat] ? 2 : 3]++; else dwhy[1][!ev ? 0 : 1]++;
                dwhy[d][5] += nd; dwhy[d][4]++;
                if (i + every >= n || 1) { extern long g_dwhy[2][6]; memcpy(g_dwhy, dwhy, sizeof dwhy); }
            }
            if (ha[i].kind == 0) {      /* an army move the engine does not offer: no army die? nothing may leave that region? or just not that road? */
                int any = 0, from = 0; for (int j = 0; j < k; j++) if (mv[j].type == M_ARMY || mv[j].type == M_ATTACK) { any = 1; from |= mv[j].a == ha[i].a; }
                int mine = units(&st[i], side, ha[i].a);
                why[!any ? 0 : !mine ? 1 : !from ? 2 : 3]++; if (any && mine && from) mroad[ha[i].a][ha[i].b]++;
            }
            continue;
        }
        int hm = -1; for (int j = 0; j < k; j++) if (tg[j] > 0 && (hm < 0 || mv[j].type == M_ATTACK)) hm = j;
        float bv; int nn; Move m = agent_choose(&ag, &st[i], &bv, &nn);
        if (vsroll) {       /* WOTR_VS_ROLL=<rollouts>: where does the rollout chooser overrule the ordinary choice, and from what kind of move to what kind? */
            ag2.rng = ag.rng; Move m2 = agent_choose(&ag2, &st[i], &bv, &nn);
            int b2 = ha[i].turn <= 2 ? 0 : ha[i].turn <= 4 ? 1 : ha[i].turn <= 7 ? 2 : 3;
            vr_n[b2]++; vr_same[b2] += !memcmp(&m, &m2, sizeof m); vr_mat[kind_of(m)][kind_of(m2)]++; vr_k1[kind_of(m)]++; vr_k2[kind_of(m2)]++;
            if (memcmp(&m, &m2, sizeof m) && vr_shown < 40 && (i % 7) == 0) { char b1[200], b3[200]; wotr_move_str(&st[i], m, b1, sizeof b1); wotr_move_str(&st[i], m2, b3, sizeof b3); printf("  turn %d: ordinary %-52s rollouts %s\n", ha[i].turn, b1, b3); vr_shown++; }
            continue;
        }
        int ours = -1; for (int j = 0; j < k; j++) if (!memcmp(&mv[j], &m, sizeof m)) ours = j;
        int b = ha[i].turn <= 2 ? 0 : ha[i].turn <= 4 ? 1 : ha[i].turn <= 7 ? 2 : 3, a = kind_of(mv[hm]), o = kind_of(m);
        tot[b]++; same[b] += ours >= 0 && tg[ours] > 0; hk[b][a]++; ok[b][o]++; mat[b][a][o]++;
        if (ours >= 0) { float bh = 1e9f; for (int j = 0; j < k; j++) if (tg[j] > 0 && ag.last_val[j] > -900 && (side == SH ? ag.last_val[j] < bh : -ag.last_val[j] < bh)) bh = side == SH ? ag.last_val[j] : -ag.last_val[j];
                         gap[b][a] += bh - (side == SH ? ag.last_val[ours] : -ag.last_val[ours]); }
        if (a == 3 && o != 3 && ours >= 0) {          /* the human mustered an elite and we would not: which formula terms speak against it? */
            State u = st[i]; wotr_apply(&u, mv[hm]); feat_encode(&u, &fh); u = st[i]; wotr_apply(&u, m); feat_encode(&u, &fo);
            for (int q = 0; q < NPRIOR; q++) { float e[NPRIOR] = {0}; e[q] = 1; fterm[q] += pl->sh.p.alpha * nn_formula(e) * (fh.prior[q] - fo.prior[q]); }
            { float bh = 1e9f; for (int j = 0; j < k; j++) if (tg[j] > 0 && ag.last_val[j] > -900 && ag.last_val[j] < bh) bh = ag.last_val[j]; fgap += bh - ag.last_val[ours]; }
            fn++; ereg[mv[hm].a]++;
        }
        if (a <= 1 && ha[i].turn <= 3) hroad[mv[hm].a][mv[hm].b]++;
        if (o <= 1 && ha[i].turn <= 3) oroad[m.a][m.b]++;
        if (a == 1) hatt[b][mv[hm].b]++;
        if (o == 1) oatt[b][m.b]++;
    }
    if (vsroll) {
        for (int b = 0; b < NB; b++) if (vr_n[b]) printf("%s: %ld positions, rollouts keep the ordinary choice in %.0f%%\n", BN[b], vr_n[b], 100.0 * vr_same[b] / vr_n[b]);
        long tot = 0; for (int a = 0; a < 9; a++) tot += vr_k1[a];
        printf("kind of move chosen, ordinary / rollouts:"); for (int a = 0; a < 9; a++) if (vr_k1[a] + vr_k2[a]) printf("  %s %.0f%% / %.0f%%", KN[a], 100.0 * vr_k1[a] / tot, 100.0 * vr_k2[a] / tot); printf("\n");
        for (int a = 0; a < 9; a++) if (vr_k1[a] * 25 > tot) { printf("  ordinary choice %s -> rollouts choose:", KN[a]); for (int o = 0; o < 9; o++) if (vr_mat[a][o] * 20 > vr_k1[a]) printf(" %s %.0f%%", KN[o], 100.0 * vr_mat[a][o] / vr_k1[a]); printf("\n"); }
        return 0;
    }
    for (int b = 0; b < NB; b++) {
        printf("%s: %ld positions, our choice is the human's in %.1f%%\n  what the die was spent on, humans / ours:", BN[b], tot[b], 100.0 * same[b] / (tot[b] ? tot[b] : 1));
        for (int a = 0; a < 9; a++) if (hk[b][a] + ok[b][a]) printf("  %s %.0f%% / %.0f%%", KN[a], 100.0 * hk[b][a] / tot[b], 100.0 * ok[b][a] / tot[b]);
        printf("\n  how much worse we score the human's move than our own choice (log-odds x 100):");
        for (int a = 0; a < 9; a++) if (hk[b][a] * 20 > tot[b]) printf("  %s %.1f", KN[a], 100.0 * gap[b][a] / hk[b][a]);
        printf("\n");
        for (int a = 0; a < 9; a++) if (hk[b][a] * 20 > tot[b]) {
            printf("  when humans chose %s, we chose:", KN[a]);
            for (int o = 0; o < 9; o++) if (mat[b][a][o] * 20 > hk[b][a]) printf(" %s %.0f%%", KN[o], 100.0 * mat[b][a][o] / hk[b][a]);
            printf("\n");
        }
        for (int w = 0; w < 2; w++) {
            long (*t)[NREG] = w ? oatt : hatt; long sum = 0; for (int r = 0; r < NREG; r++) sum += t[b][r];
            printf("  %s attacks (%.1f%% of positions) on:", w ? "our" : "human", 100.0 * sum / (tot[b] ? tot[b] : 1));
            for (int q = 0; q < 8; q++) { int br = -1; for (int r = 0; r < NREG; r++) if (t[b][r] > 0 && (br < 0 || t[b][r] > t[b][br])) br = r; if (br < 0) break; printf(" %s %.0f%%", REGION[br].name, 100.0 * t[b][br] / sum); t[b][br] = -t[b][br]; }
            printf("\n");
        }
    }
    printf("human actions the engine does not offer: army %.1f%% of %ld, muster %.1f%% of %ld, diplomacy %.1f%% of %ld, draw %.1f%% of %ld\n", 100.0 * miss[0] / (seen[0] ? seen[0] : 1), seen[0], 100.0 * miss[1] / (seen[1] ? seen[1] : 1), seen[1], 100.0 * miss[2] / (seen[2] ? seen[2] : 1), seen[2], 100.0 * miss[3] / (seen[3] ? seen[3] : 1), seen[3]);
    printf("  army moves not offered (%ld): no army move possible at all %ld, no own units in the region the log names %ld, units there but none may move %ld, this road not offered %ld; such roads:", miss[0], why[0], why[1], why[2], why[3]);
    for (int q = 0; q < 12; q++) { int ba = -1, bb = -1; for (int a = 0; a < NREG; a++) for (int b = 0; b < NREG; b++) if (mroad[a][b] > 0 && (ba < 0 || mroad[a][b] > mroad[ba][bb])) { ba = a; bb = b; } if (ba < 0) break; printf(" %s>%s %ld", REGION[ba].name, REGION[bb].name, mroad[ba][bb]); mroad[ba][bb] = 0; }
    printf("\n");
    printf("  diplomacy not offered (%ld): no muster die in our dice %ld, nation already at war %ld, at the last step but not active %ld, other %ld; dice in hand then %.1f\n  draw not offered (%ld): no event die in our dice %ld, other %ld; dice in hand then %.1f\n", g_dwhy[0][4], g_dwhy[0][0], g_dwhy[0][1], g_dwhy[0][2], g_dwhy[0][3], (double)g_dwhy[0][5] / (g_dwhy[0][4] ? g_dwhy[0][4] : 1), g_dwhy[1][4], g_dwhy[1][0], g_dwhy[1][1], (double)g_dwhy[1][5] / (g_dwhy[1][4] ? g_dwhy[1][4] : 1));
    if (fn) {
        printf("human elite musters we would not make (%ld): our score for them is %.2f log-odds worse than for our choice; of that, by formula term (share-weighted):", fn, fgap / fn);
        double fs = 0; for (int q = 0; q < NPRIOR; q++) { fs += fterm[q]; if (fabs(fterm[q] / fn) > 0.005) printf(" term %d %+.3f", q, fterm[q] / fn); }
        printf("; formula in all %+.2f, the network the rest\n  where humans mustered them:", fs / fn);
        for (int q = 0; q < 10; q++) { int br = -1; for (int r = 0; r < NREG; r++) if (ereg[r] > 0 && (br < 0 || ereg[r] > ereg[br])) br = r; if (br < 0) break; printf(" %s %.0f%%", REGION[br].name, 100.0 * ereg[br] / fn); ereg[br] = 0; }
        printf("\n");
    }
    for (int w = 0; w < 2; w++) {
        long (*t)[NREG] = w ? oroad : hroad; long sum = 0; for (int a = 0; a < NREG; a++) for (int b = 0; b < NREG; b++) sum += t[a][b];
        printf("turns 1-3, %s army moves and attacks by road:", w ? "our" : "human");
        for (int q = 0; q < 14; q++) { int ba = -1, bb = -1; for (int a = 0; a < NREG; a++) for (int b = 0; b < NREG; b++) if (t[a][b] > 0 && (ba < 0 || t[a][b] > t[ba][bb])) { ba = a; bb = b; } if (ba < 0) break; printf(" %s>%s %.1f%%", REGION[ba].name, REGION[bb].name, 100.0 * t[ba][bb] / sum); t[ba][bb] = 0; }
        printf("\n");
    }
    return 0;
}

/* wotr awcmp <shadow player> <decisions file>...: AlphaWOTR's Shadow, decision by decision (records written by the referee's
   replay: the State before the decision, then the Move it chose), against what our Shadow would choose in the very same
   positions. Kinds of move, how many units and elites go into its attacks, and how badly we score its choices. */
static int kind2(Move m) {      /* 0 army move, 1 attack, 2 siege assault, 3 muster, 4 elite, 5 minion, 6 diplomacy, 7 draw, 8 play a card, 9 Nazgul, 10 other */
    switch (m.type) { case M_ARMY: return 0; case M_ATTACK: return m.a == m.b ? 2 : 1; case M_MUSTER: return 3; case M_MUSTER_ELITE: case M_VOICE: return 4; case M_MINION: return 5;
                      case M_DIPLO: return 6; case M_DRAW: return 7; case M_PLAY: return 8; case M_NAZGUL: case M_NAZ_PLAN: case M_NAZ_MOVE: return 9; default: return 10; }
}
static int cmd_awcmp(int argc, char **argv) {
    static const char *KN[11] = {"army move", "attack", "siege assault", "muster", "elite", "minion", "diplomacy", "draw", "play a card", "Nazgul", "other"};
    int side = getenv("WOTR_IM_SIDE") && !strcmp(getenv("WOTR_IM_SIDE"), "FP") ? FP : SH;      /* WOTR_IM_SIDE=FP: AlphaWOTR's Free Peoples against ours */
    if (side == FP) { KN[5] = "pass"; KN[9] = "Fellowship"; }
    enum { NB = 4 }; static const char *BN[NB] = {"turns 1-2", "turns 3-4", "turns 5-7", "turns 8+"};
    Player *pl = malloc(sizeof(Player));
    if (player_load(pl, argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
    static Agent ag; memset(&ag, 0, sizeof ag);
    ag.net[FP] = &pl->fp; ag.net[SH] = &pl->sh; ag.samples = 3; ag.temp = 0; ag.rng = 12345;
    ag.depth = g_deep[side][0]; ag.width = g_deep[side][1]; ag.replies = g_deep[side][2]; ag.narrow = g_deep[side][3]; ag.dpol[FP] = &pl->pol[FP]; ag.dpol[SH] = &pl->pol[SH];
    static long tot[NB], same[NB], ak[NB][11], ok[NB][11], inlist[NB], au[NB][3], ae[NB][3], an[NB][3], ou[NB][3], oe[NB][3], on[NB][3], ext[2], card[2], turns_seen[64], games = 0;
    static double gap[NB][11]; static long gapn[NB][11], atgt[NREG], otgt[NREG];
    static Move mv[MAXMOVES]; int prev_turn = 99, every = getenv("WOTR_EVERY") ? atoi(getenv("WOTR_EVERY")) : 1; long rec = 0;
    for (int fi = 3; fi < argc; fi++) {
        FILE *f = fopen(argv[fi], "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", argv[fi]); continue; }
        State st; Move am;
        while (fread(&st, sizeof st, 1, f) == 1 && fread(&am, sizeof am, 1, f) == 1) {
            if (st.turn < prev_turn - 3) games++;
            prev_turn = st.turn;
            if (st.dplayer != side) continue;
            if (st.dkind == D_SIEGE_EXTEND) { ext[0]++; ext[1] += am.a != 0; continue; }
            if (st.dkind == D_BATTLE_CARD) { card[0]++; card[1] += am.a < NCARD; continue; }
            if (st.dkind != D_ACTION) continue;
            if (st.turn < 64 && !turns_seen[st.turn]) turns_seen[st.turn] = 1;
            int b = st.turn <= 2 ? 0 : st.turn <= 4 ? 1 : st.turn <= 7 ? 2 : 3, a = kind2(am); if (side == FP) a = am.type == M_PASS ? 5 : am.type == M_FSP_MOVE || am.type == M_FSP_HIDE ? 9 : a;
            tot[b]++; ak[b][a]++;
            if (a == 1 || a == 2) { int x = a - 1; an[b][x]++; au[b][x] += am.c + am.d; ae[b][x] += am.d; if (st.turn <= 4) atgt[am.b]++; }
            if (rec++ % every) continue;
            int n = wotr_moves(&st, mv), ai = -1;
            for (int i = 0; i < n; i++) if (mv[i].type == am.type && mv[i].a == am.a && mv[i].b == am.b && (ai < 0 || (mv[i].c == am.c && mv[i].d == am.d))) ai = i;
            float bv; int nn; Move m = agent_choose(&ag, &st, &bv, &nn);
            int o = kind2(m), oi = -1; if (side == FP) o = m.type == M_PASS ? 5 : m.type == M_FSP_MOVE || m.type == M_FSP_HIDE ? 9 : o; for (int i = 0; i < n; i++) if (!memcmp(&mv[i], &m, sizeof m)) oi = i;
            ok[b][o]++; same[b] += m.type == am.type && m.a == am.a && m.b == am.b;
            if (o == 1 || o == 2) { int x = o - 1; on[b][x]++; ou[b][x] += m.c + m.d; oe[b][x] += m.d; if (st.turn <= 4) otgt[m.b]++; }
            if (ai >= 0) { inlist[b]++; if (oi >= 0 && ag.last_val[ai] > -900 && ag.last_val[ai] < 900) { gap[b][a] += side == SH ? ag.last_val[ai] - ag.last_val[oi] : ag.last_val[oi] - ag.last_val[ai]; gapn[b][a]++; } }
        }
        fclose(f);
    }
    printf("AlphaWOTR's %s: %ld games; ours, %s, asked in every %d%s of its action positions\n", side == FP ? "Free Peoples" : "Shadow", games, argv[2], every, every == 1 ? "st" : "th");
    for (int b = 0; b < NB; b++) {
        long os = 0; for (int k = 0; k < 11; k++) os += ok[b][k];
        if (!tot[b] || !os) continue;
        printf("%s: %ld AlphaWOTR action decisions; we would make the same move in %.0f%%; its move is among those we consider in %.0f%%\n  dice spent, AlphaWOTR / ours:", BN[b], tot[b], 100.0 * same[b] / os, 100.0 * inlist[b] / os);
        for (int k = 0; k < 11; k++) if (ak[b][k] * 50 > tot[b] || ok[b][k] * 50 > os) printf("  %s %.0f%% / %.0f%%", KN[k], 100.0 * ak[b][k] / tot[b], 100.0 * ok[b][k] / os);
        printf("\n  how far below our own choice we score its move (log-odds):");
        for (int k = 0; k < 11; k++) if (gapn[b][k] * 30 > os) printf("  %s %.2f", KN[k], gap[b][k] / gapn[b][k]);
        printf("\n  units (elites) committed: its field attacks %.1f (%.1f), ours %.1f (%.1f); its siege assaults %.1f (%.1f), ours %.1f (%.1f)\n",
               an[b][0] ? (double)au[b][0] / an[b][0] : 0, an[b][0] ? (double)ae[b][0] / an[b][0] : 0, on[b][0] ? (double)ou[b][0] / on[b][0] : 0, on[b][0] ? (double)oe[b][0] / on[b][0] : 0,
               an[b][1] ? (double)au[b][1] / an[b][1] : 0, an[b][1] ? (double)ae[b][1] / an[b][1] : 0, on[b][1] ? (double)ou[b][1] / on[b][1] : 0, on[b][1] ? (double)oe[b][1] / on[b][1] : 0);
    }
    for (int w = 0; w < 2; w++) {
        long *t = w ? otgt : atgt, sum = 0; for (int r = 0; r < NREG; r++) sum += t[r];
        printf("turns 1-4, %s attacks on:", w ? "our" : "AlphaWOTR's");
        for (int q = 0; q < 8 && sum; q++) { int br = -1; for (int r = 0; r < NREG; r++) if (t[r] > 0 && (br < 0 || t[r] > t[br])) br = r; if (br < 0) break; printf(" %s %.0f%%", REGION[br].name, 100.0 * t[br] / sum); t[br] = 0; }
        printf("\n");
    }
    printf("AlphaWOTR extends a siege round (reduces an elite) in %.0f%% of the %ld chances; plays a combat card in %.0f%% of %ld battle rounds\n", 100.0 * ext[1] / (ext[0] ? ext[0] : 1), ext[0], 100.0 * card[1] / (card[0] ? card[0] : 1), card[0]);
    return 0;
}

/* wotr awlearn <player in> <player out> <passes> <lr> <decisions file>...: teach Shadow's move-suggesting network the moves
   AlphaWOTR's Shadow chose (every kind of decision), keeping every fifth game apart to measure on. */
static int cmd_awlearn(int argc, char **argv) {
    Player *pl = malloc(sizeof(Player));
    if (player_load(pl, argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
    int passes = atoi(argv[4]); float lr = (float)atof(argv[5]);
    int side = getenv("WOTR_IM_SIDE") && !strcmp(getenv("WOTR_IM_SIDE"), "FP") ? FP : SH;      /* whose decisions the records hold, and whose network learns */
    Net *vn = side == FP ? &pl->fp : &pl->sh;
    State *st = NULL; Move *am = NULL; uint8_t *held = NULL; long n = 0, cap = 0; int games = 0, prev = 99;
    for (int fi = 6; fi < argc; fi++) {
        FILE *f = fopen(argv[fi], "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", argv[fi]); continue; }
        State s; Move m;
        while (fread(&s, sizeof s, 1, f) == 1 && fread(&m, sizeof m, 1, f) == 1) {
            if (s.turn < prev - 3) games++;
            prev = s.turn;
            if (s.dplayer != side) continue;
            if (n == cap) { cap = cap ? cap * 2 : 8192; st = realloc(st, (size_t)cap * sizeof(State)); am = realloc(am, (size_t)cap * sizeof(Move)); held = realloc(held, (size_t)cap); }
            st[n] = s; am[n] = m; held[n++] = games % 5 == 0;
        }
        fclose(f); prev = 99;
    }
    static Feat ft; static Move mv[MAXMOVES]; static float tg[MAXMOVES], lg[MAXMOVES]; float acc[H1]; uint64_t r = 0x9E3779B97F4A7C15ULL;
    long *ord = malloc((size_t)n * sizeof(long)); for (long i = 0; i < n; i++) ord[i] = i;
    for (int e = 0; e <= passes; e++) {
        long tot = 0, found = 0, top1 = 0, top6 = 0, used = 0; double loss = 0; int cnt = 0;
        if (e) for (long i = n - 1; i > 0; i--) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; long j = (long)(r % (uint64_t)(i + 1)), t = ord[i]; ord[i] = ord[j]; ord[j] = t; }
        for (long q = 0; q < n; q++) {
            long x = ord[q]; int k = wotr_moves(&st[x], mv), ai = -1, exact = 0;
            if (k < 2) continue;
            for (int i = 0; i < k; i++) if (mv[i].type == am[x].type && mv[i].a == am[x].a && mv[i].b == am[x].b) { int ex = mv[i].c == am[x].c && mv[i].d == am[x].d; if (ai < 0 || (ex && !exact)) { ai = i; exact = ex; } }
            if (e == 0 || held[x]) {         /* measure (before any pass: everything held apart; afterwards: the games kept apart) */
                if (!held[x]) continue;
                if (st[x].dkind == D_ACTION) { tot++; if (ai < 0) continue; found++;
                    feat_encode(&st[x], &ft); nn_root(vn, &ft, acc); for (int j = 0; j < H1; j++) if (acc[j] < 0) acc[j] = 0;
                    pol_logits(&pl->pol[side], acc, st[x].dkind, mv, k, lg);
                    int rk = 0; for (int j = 0; j < k; j++) if (lg[j] > lg[ai]) rk++;
                    top1 += rk == 0; top6 += rk < 6; }
                continue;
            }
            if (ai < 0) continue;
            for (int i = 0; i < k; i++) tg[i] = i == ai;
            feat_encode(&st[x], &ft); nn_root(vn, &ft, acc); for (int j = 0; j < H1; j++) if (acc[j] < 0) acc[j] = 0;
            loss += pol_backward(&pl->pol[side], acc, st[x].dkind, mv, k, tg); used++;
            if (++cnt == 32) { pol_step(&pl->pol[side], lr, 32); cnt = 0; }
        }
        if (cnt) pol_step(&pl->pol[side], lr, cnt);
        if (e == 0) printf("%ld %s decisions of AlphaWOTR from %d games; before: ", n, side ? "Shadow" : "Free Peoples", games); else printf("after pass %d (loss %.3f): ", e, loss / (used ? used : 1));
        /* the measuring loop above ran inside the same sweep for e > 0 on the held games */
        printf("held-out action decisions %ld, its move offered in %.0f%%; the network puts it first %.1f%%, in its first six %.1f%%\n", tot, 100.0 * found / (tot ? tot : 1), 100.0 * top1 / (found ? found : 1), 100.0 * top6 / (found ? found : 1));
    }
    player_save(pl, argv[3]);
    return 0;
}

/* WOTR_REW is read for every command, so that readings (eval) are played with the same values the players were trained with */
static void rew_init(void) {
    if (getenv("WOTR_STALL")) agent_stall = (float)atof(getenv("WOTR_STALL"));
    if (getenv("WOTR_RECORD") && strlen(getenv("WOTR_RECORD")) > 3) { g_rec_side = getenv("WOTR_RECORD")[0] == 'F' ? FP : SH; g_rec = getenv("WOTR_RECORD") + 3; }
    if (getenv("WOTR_REW")) {
        float m[8];
        if (sscanf(getenv("WOTR_REW"), "%f,%f,%f,%f,%f,%f,%f,%f", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5], &m[6], &m[7]) != 8) { fprintf(stderr, "WOTR_REW needs eight numbers: FPMV,FPRV,FPML,FPRL,SPMV,SPRV,SPML,SPRL\n"); exit(1); }
        g_out[FP][0] = (1 + m[1]) / 2; g_out[FP][1] = (1 + m[0]) / 2; g_out[FP][2] = (1 - m[3]) / 2; g_out[FP][3] = (1 - m[2]) / 2;   /* Ring win, military win, lost to corruption, lost to conquest */
        g_out[SH][0] = (1 + m[4]) / 2; g_out[SH][1] = (1 + m[5]) / 2; g_out[SH][2] = (1 - m[7]) / 2; g_out[SH][3] = (1 - m[6]) / 2;   /* conquest, corruption, lost to the Ring, lost to a military victory */
        g_out_on = 1; memcpy(agent_out, g_out, sizeof g_out); agent_out_on = 1;
        printf("outcome modifiers: FPMV %.2f FPRV %.2f FPML %.2f FPRL %.2f | SPMV %.2f SPRV %.2f SPML %.2f SPRL %.2f\n", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
    }
}

int main(int argc, char **argv) {
    rew_init();
    if (getenv("WOTR_SPEC")) g_spec = atoi(getenv("WOTR_SPEC"));
    if (getenv("WOTR_ROLL_FP")) sscanf(getenv("WOTR_ROLL_FP"), "%f,%f,%f,%f,%f", &g_roll[FP][0], &g_roll[FP][1], &g_roll[FP][2], &g_roll[FP][3], &g_roll[FP][4]);
    if (getenv("WOTR_ROLL_SH")) sscanf(getenv("WOTR_ROLL_SH"), "%f,%f,%f,%f,%f", &g_roll[SH][0], &g_roll[SH][1], &g_roll[SH][2], &g_roll[SH][3], &g_roll[SH][4]);
    if (getenv("WOTR_PROBE_FLANK")) agent_flank = atoi(getenv("WOTR_PROBE_FLANK"));
    if (getenv("WOTR_BSAMP")) agent_bsamp = atoi(getenv("WOTR_BSAMP"));
    if (getenv("WOTR_ROADS")) { extern int wotr_roads; wotr_roads = atoi(getenv("WOTR_ROADS")); }
    if (getenv("WOTR_FULL")) g_full = (float)atof(getenv("WOTR_FULL"));
    if (getenv("WOTR_EXACT")) { if (sscanf(getenv("WOTR_EXACT"), "%d,%d", &g_exact[FP], &g_exact[SH]) == 1) g_exact[SH] = g_exact[FP]; }
    if (getenv("WOTR_EXEMPT")) { if (sscanf(getenv("WOTR_EXEMPT"), "%d,%d", &g_exempt[FP], &g_exempt[SH]) == 1) g_exempt[SH] = g_exempt[FP]; }
    if (getenv("WOTR_TOPK_FP")) g_topk[FP] = atoi(getenv("WOTR_TOPK_FP"));
    if (getenv("WOTR_TOPK_SH")) g_topk[SH] = atoi(getenv("WOTR_TOPK_SH"));
    if (getenv("WOTR_DEEP_FP")) sscanf(getenv("WOTR_DEEP_FP"), "%d,%d,%d,%d,%d,%d", &g_deep[FP][0], &g_deep[FP][1], &g_deep[FP][2], &g_deep[FP][3], &g_deep[FP][4], &g_deep[FP][5]);
    if (getenv("WOTR_DEEP_SH")) sscanf(getenv("WOTR_DEEP_SH"), "%d,%d,%d,%d,%d,%d", &g_deep[SH][0], &g_deep[SH][1], &g_deep[SH][2], &g_deep[SH][3], &g_deep[SH][4], &g_deep[SH][5]);
    feat_init(); vps_init();
    if (argc >= 3 && !strcmp(argv[1], "train")) return cmd_train(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "eval")) return cmd_eval(argc, argv);
    if (argc >= 5 && !strcmp(argv[1], "lessons")) { feat_init(); return cmd_lessons(argc, argv); }
    if (argc >= 5 && !strcmp(argv[1], "imitate")) return cmd_imitate(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "agree")) return cmd_agree(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "awcmp")) return cmd_awcmp(argc, argv);
    if (argc >= 7 && !strcmp(argv[1], "awlearn")) return cmd_awlearn(argc, argv);
    if (argc >= 5 && !strcmp(argv[1], "share")) {      /* wotr share <player in> <player out> <x>: the same player saved with formula share x */
        Player *pl = malloc(sizeof(Player)); if (player_load(pl, argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
        pl->fp.p.alpha = pl->sh.p.alpha = (float)atof(argv[4]); player_save(pl, argv[3]); return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "show")) return cmd_show(argc, argv);
    fprintf(stderr, "usage: wotr train <outdir> [rounds=120] [games/round=2000] [threads=9] [fade=480]\n       wotr eval <netA|prior> <netB|prior> [games] [threads]\n       wotr show <net|prior> <seed> [v] [shadow net]\n");
    return 2;
}
