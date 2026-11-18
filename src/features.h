/* features.h -- sparse state encoding and the strategy-guide value prior */
#ifndef WOTR_FEATURES_H
#define WOTR_FEATURES_H
#include "wotr.h"

#define REG_F 11                       /* per-region features */
#define NVPS 22                        /* victory-point settlements (12 FP + 10 Shadow) */
#define VPS_F 5
#define F_REGION 0
#define F_VPS (F_REGION + NREG * REG_F)
#define F_GLOBAL (F_VPS + NVPS * VPS_F)
#define NGUIDE 64                       /* the guides as network inputs: strategy terms, fall forecasts, deployment counts */
#define NGLOBAL (424 + NGUIDE)
#define WATCH_F 6                       /* per settlement: can it be taken or held? (g_watch in features.c) */
#define NWATCH (NVPS * WATCH_F + 6 + NVPS)     /* new inputs are only ever added at the END of the list, so saved networks stay loadable */
#define F_WATCH (F_GLOBAL + NGLOBAL)
#define DANGER_F 7                      /* per settlement: how likely the enemy takes it soon, and what an assault on it costs (g_danger in features.c) */
#define NDANGER (NVPS * DANGER_F + 6)
#define F_DANGER (F_WATCH + NWATCH)
#define NFEAT (F_DANGER + NDANGER)
#define MAXNNZ 1100
#define NPRIOR 24                      /* hand-picked strategic terms; also the inputs of the prior */

typedef struct { int n; uint16_t idx[MAXNNZ]; float val[MAXNNZ]; float prior[NPRIOR]; } Feat;

/* cached root encoding and the change list of a position relative to it (see features.c) */
typedef struct { int8_t vps, vpf, sfp, ssh, thr; float dng, gap, sg, fsg; } FeatVC;      /* gap/sg/fsg: garrison gap and siege forecasts (see prior_terms) */
typedef struct {
    Feat f;
    uint16_t rs[NREG + 1], vs[NVPS + 1], gs[12];    /* where each block starts in f */
    uint8_t un[2][NREG], cl[NREG], ml[NREG];
    int fsp;
    FeatVC vc[NVPS];
} FeatCache;
typedef struct { int n; uint16_t idx[2 * MAXNNZ]; float dv[2 * MAXNNZ]; float prior[NPRIOR]; } FeatDelta;

void feat_init(void);
void feat_cache(const State *s, FeatCache *c);                                        /* full encode, keeping block structure */
void feat_delta(const State *root, const FeatCache *c, const State *s, FeatDelta *d);   /* s relative to the cached root */
void feat_encode(const State *s, Feat *f);
/* quantities used for reporting/auxiliary targets */
int  vps_local(const State *s, int k, int *holder, int *mine, int *e1, int *e2, int *siege, int *la, int *ld);   /* local picture around a victory-point settlement; returns its region */
int  feat_front(int r);                     /* 0, or 1/2/3: two regions from / next to / in a Free Peoples city or stronghold */
enum { RW_STEP, RW_CORR, RW_SHVP, RW_FPVP, RW_SIEGE, RW_FPUNIT, RW_SHUNIT, RW_WAR, RW_MINION, RW_CROWN, RW_DEAD, RW_THREAT, RW_LEAD, RW_GARRISON, RW_REVEALED, NRW };
extern float feat_rw[NRW], feat_td;       /* sizes of the partial rewards used in training, and a scale on all of them (0 = none) */
float feat_progress(const State *s, const float *rw);      /* the position's progress score under a table of sizes, from the Free Peoples' side, in win-probability units */
void shadow_deploy(const State *s, int *mordor, int *front, int *walls, int *inside, int *besieged);   /* Shadow units in Mordor / near a Free Peoples settlement, FP units besieged */
int  fsp_steps_to_mordor(const State *s);   /* regions still to walk before the Mordor track */
#endif
