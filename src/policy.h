/* policy.h -- move-scoring policy used to prune the search.
 * logit(move) = bias[type] + h . (sum of embedding rows selected by the move's fields),
 * where h is the value network's first hidden layer at the root position. Scoring a move
 * costs a few dot products and does not need the move to be applied. */
#ifndef WOTR_POLICY_H
#define WOTR_POLICY_H
#include "nn.h"

#define NAUX 96
typedef struct {
    float type[NMOVETYPE][H1], die[8][H1], src[NREG + 1][H1], dst[NREG + 1][H1], card[NCARD + 1][H1];
    float kind[ND][H1], aux[NAUX][H1], cnt[2][H1];
    float bias[NMOVETYPE];
} PolParams;
typedef struct { PolParams p; PolParams *m, *v, *g; long step; } Policy;

void  pol_init(Policy *p);
void  pol_logits(const Policy *p, const float *h, int dkind, const Move *mv, int n, float *logit);
/* cross-entropy step toward target probabilities; returns loss */
float pol_backward(Policy *p, const float *h, int dkind, const Move *mv, int n, const float *target);
void  pol_step(Policy *p, float lr, int batch);
int   pol_save(const Policy *p, const char *path);
int   pol_load(Policy *p, const char *path);
#endif
