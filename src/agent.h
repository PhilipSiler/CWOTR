/* agent.h -- move selection: shallow expectimax over legal moves with a value network */
#ifndef WOTR_AGENT_H
#define WOTR_AGENT_H
#include "policy.h"

enum { SP_HUNT, SP_BATTLE, SP_FSP, NSPEC };    /* families of tactical decision with networks of their own (spec_group) */
typedef struct { Feat root; float acc[2][H1]; int age; } AccCache;      /* the previous decision's root and first-layer sums, per pair of networks */
typedef struct {
    const Net *net[2];      /* indexed by the side to decide at the evaluated state */
    int samples;            /* dice samples per move whose outcome involves chance */
    const Policy *dpol[2];  /* the move-suggesting networks for `narrow` (kept apart from pol[], which switches on pruning at the first step) */
    int narrow;             /* inside the look-ahead, score only the move-suggesting network's first `narrow` moves at each action decision (0: all) */
    int taper;              /* the number of moves followed shrinks by this much with each further own action (0: stays at width) */
    int rnd;                /* wide look-ahead (Philip, 2026-11-16): besides the `width` best moves, this many picked at random from the
                               rest are followed too, for BOTH sides; both numbers shrink by one per action looked ahead; after
                               `depth` actions each line is played on, one move a side, to the end of the turn */
    int depth, width, replies;   /* looking ahead: own actions looked ahead (0 or 1: only the next), moves followed at each of them, replies considered for the other side (see deep() in agent.c) */
    float temp;             /* softmax temperature on win probability (0 = greedy) */
    uint64_t rng;
    long evals;             /* statistics */
    long ev_kind[ND], dec_kind[ND];   /* evaluations / decisions by decision kind (profiling) */
    int cur_kind;
    float acc[2][H1];                 /* first-layer sums of the root, per network */
    int has_root;
    const Net *spec[NSPEC][2];        /* specialist networks, [family][side to decide at the scored position]; 0 = the main networks score everything */
    const Net *vnet[2];               /* the pair scoring the decision in hand */
    AccCache cache[1 + NSPEC];        /* previous decision's root per pair: the next one is built from it */
    /* policy pruning: search only the policy's top-k moves plus a few random ones, except on
       "full" decisions (probability full_frac), which search everything and yield policy targets */
    const Policy *pol[2];
    int topk, nrand; float full_frac;
    int exact;                        /* 1: ties in the policy's logits are broken at random, so exactly topk moves are scored */
    int exempt;                       /* 1: attacks, card plays and the like are always scored, whatever the policy says (the old rule); 0: the policy's first topk only */
    int last_tier;                    /* 0 everything searched (few moves or no policy), 1 full width, 2 top-k, 3 quick */
    int last_explored; float last_explore_cost;   /* the move played was not the search's best; log-odds given up */
    int last_top1;                    /* full decisions: the policy's first choice was the search's best */
    float last_regret;                /* full decisions: value given up by restricting to the policy's top-k */
    int last_deep; uint8_t last_followed[MAXMOVES];   /* last decision: was the look-ahead used, and which moves it followed */
    int last_ncand;                   /* last decision: moves scored at the first step */
    int last_full, last_n, last_hit;  /* last decision: searched full width? move count; was the best move in the policy's top-k? */
    Move last_mv[MAXMOVES]; float last_val[MAXMOVES];
    FeatCache rc;                     /* the root position's encoding (rc.f) with its block structure */
    const State *root;
    /* the rollout chooser (see roll_choose in agent.c): time budget in ms on this thread, or a rollout budget; softmax temperature inside rollouts; root moves shortlisted */
    float roll_ms; int roll_n; float roll_tau; int roll_k; int roll_turns;
    float deep_gap;                   /* look ahead only when the two best one-step scores are closer than this (0: always) */
    /* quick decisions: with probability fast_frac a non-full decision searches only the policy's top fastk moves */
    float fast_frac; int fastk;
} Agent;

int   spec_group(int dkind);                                       /* SP_... or -1 */
float agent_value(Agent *a, const State *s);                       /* log-odds that FP wins (+-WIN_LOGIT at game end) */
#define WIN_LOGIT 30.0f
/* log-odds of a finished game: a game stopped by the turn cap is a draw, so nobody can win by waiting */
static inline float terminal_value(const State *s) { return s->win_type == WIN_NONE ? 0.0f : s->winner == 1 ? WIN_LOGIT : -WIN_LOGIT; }
/* picks a move for s->dplayer; *best_v = search value (log-odds that FP wins) of the best move; last_val[] likewise */
Move  agent_choose(Agent *a, const State *s, float *best_v, int *n_moves);
extern long agent_roll_count, agent_roll_decisions; extern double agent_roll_ms_max, agent_roll_ms_sum;
extern int agent_bsamp, agent_flank;
extern float agent_probe, agent_probe_siege;
extern float agent_hum[2];   /* weight of the move-suggesting network's opinion in the choice at action decisions, per side (0: none) */
#endif
