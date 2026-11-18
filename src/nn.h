/* nn.h -- small MLP value network with a linear strategy-guide prior on the win logit */
#ifndef WOTR_NN_H
#define WOTR_NN_H
#include <math.h>
#include "features.h"

#define H1 256
#define H2 32
/* Outputs. Only O_WIN is used by the search; the rest are auxiliary predictions of big-picture facts about how
 * the game unfolds. They give the shared layers many concrete things to explain per game instead of one bit. */
#define NOWN 22     /* victory-point settlements: 12 Free Peoples, then 10 Shadow (the order of features.c's VPS list) */
enum {
    O_WIN,          /* logit of P(Free Peoples win) */
    O_CORR,         /* final corruption /12 */
    O_SHVP,         /* final Shadow VP /10 */
    O_RING,         /* P(game decided by the Ring: destroyed or corrupted) */
    O_TURNS,        /* turns remaining /30 */
    O_FPVP3,        /* VP the Free Peoples gain over the next three turns /4 */
    O_SHVP3,        /* VP Shadow gains over the next three turns /10 */
    O_OWN,          /* NOWN x P(settlement is in enemy hands at game end) */
    O_CORR3 = O_OWN + NOWN,   /* corruption gained over the next three turns /12 */
    O_STEPS_END,    /* Fellowship steps left to Mount Doom at game end /15 */
    O_STEPS3,       /* steps the Fellowship advances over the next three turns /15 */
    O_FPVP,         /* final Free Peoples VP /4 */
    O_MORDOR_P,     /* P(the Fellowship enters Mordor) */
    O_MORDOR_T,     /* turns until it does /15 (trained only on games where it happens) */
    O_CHR_P,        /* 5 x P(character enters play): Witch-king, Saruman, Mouth, Gandalf the White, Aragorn */
    O_CHR_T = O_CHR_P + 5,    /* 5 x turns until that character enters /10 (only where it happens) */
    O_TILES_FP = O_CHR_T + 5, /* Free Peoples special Hunt tiles brought into play /4 */
    O_TILES_SH,     /* Shadow special Hunt tiles brought into play /4 */
    O_FPWAR,        /* Free Peoples nations At War at game end /5 */
    O_COMPDEAD,     /* companions dead at game end /7 */
    O_END,          /* 4 x P(the game ends this way): Ring destroyed, corruption, Shadow military, Free Peoples military */
    O_WAR_T = O_END + 4,      /* 8 x turns until the nation is At War /10 (if never: turns past the end of the game) */
    O_LOST3 = O_WAR_T + 8,    /* 2 x combat hits the side's units absorb over the next three turns /10 (FP, Shadow) */
    O_SIEGE3 = O_LOST3 + 2,   /* Free Peoples strongholds under siege three turns from now /4 */
    O_MORDOR_COMP,  /* companions still in the Fellowship when it enters Mordor /7 (only where it does) */
    O_TILES3,       /* Hunt tiles drawn over the next three turns /6 */
    NOUT
};
static inline int nn_is_prob(int k) { return k == O_WIN || k == O_RING || (k >= O_OWN && k < O_OWN + NOWN) || k == O_MORDOR_P || (k >= O_CHR_P && k < O_CHR_P + 5) || (k >= O_END && k < O_END + 4); }

typedef struct {
    float W1[NFEAT][H1], b1[H1];
    float W2[H1][H2], b2[H2];
    float W3[H2][NOUT], b3[NOUT];
    float alpha;           /* the hand-written strategy formula's share of the win logit used for decisions (the network has the rest): 1 at the start, fading toward 0 */
} Params;
#define NPARAM (sizeof(Params) / sizeof(float))

typedef struct {
    Params p;
    Params *m, *v, *g;     /* Adam moments and gradient accumulator (allocated only for training) */
    long step;
} Net;

void  nn_init(Net *n, uint64_t seed);
/* out[0] is the LOGIT of P(FP wins). The search compares moves in log-odds on purpose: a probability
 * saturates to exactly 0 or 1 in a won or lost position, every move then ties, and the side that is
 * ahead would play at random. */
void  nn_forward(const Net *n, const Feat *f, float *out);
static inline float nn_prob(float logit) { return 1.0f / (1.0f + expf(-logit)); }
/* incremental evaluation: positions searched from one root differ from it in few features. Features are
 * index-sorted, so the difference between two positions is a merge of their sparse lists. */
void  nn_root(const Net *n, const Feat *root, float *acc);                 /* acc[H1]: first-layer sums of root */
void  nn_acc_delta(const Net *n, const Feat *from, const float *acc_from, const Feat *to, float *acc_to);
void  nn_forward_delta(const Net *n, const Feat *root, const float *acc, const Feat *f, float *out);
void  nn_forward_list(const Net *n, const float *acc, const FeatDelta *d, float *out);   /* from a change list */
float nn_win_list(const Net *n, const float *acc, const FeatDelta *d);                    /* the same, win logit only */
extern _Thread_local const Params *nn_style_p; extern _Thread_local float nn_style_w[NPRIOR];      /* one game's style, see nn.c */
float nn_formula(const float *prior);      /* the hand-written strategy formula: fixed weights times the terms */
void  nn_train_begin(Net *n);
/* accumulate gradient of one sample; target[k] < -50 means "no target for this head"; returns win-head loss */
float nn_backward(Net *n, const Feat *f, const float *target, float weight);
void  nn_step(Net *n, float lr, int batch);
int   nn_save(const Net *n, const char *path);
int   nn_load(Net *n, const char *path);
#endif
