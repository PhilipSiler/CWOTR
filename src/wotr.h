/* War of the Ring (2nd ed., base game) rules engine -- public + internal definitions.
 *
 * The engine is a resumable state machine. A State always sits either at a decision
 * (s->dkind != D_NONE, s->dplayer to move), or at game end (s->winner != 0).
 *   wotr_init(&s, seed);  n = wotr_moves(&s, mv);  wotr_apply(&s, mv[i]);
 * Chance (dice, tile and card draws) is resolved inside wotr_apply with the RNG
 * embedded in the State, so copying a State and reseeding samples a new future.
 */
#ifndef WOTR_H
#define WOTR_H
#include <stdint.h>
#include <string.h>
#include "data.h"

enum { FP = 0, SH = 1 };
enum { DWARVES, ELVES, GONDOR, NORTH, ROHAN, ISENGARD, SAURON, SOUTHRON, NNAT, NO_NATION = 8 };
#define NFPNAT 5
#define NAT_SIDE(n) ((n) >= ISENGARD ? SH : FP)
enum { S_NONE, S_TOWN, S_CITY, S_STRONGHOLD, S_FORT };
/* action die faces; F_SPECIAL is Will of the West (FP) or Eye (Shadow) */
enum { F_CHAR, F_ARMY, F_MUSTER, F_EVENT, F_MA, F_SPECIAL, NFACE };
enum { C_GANDALF, C_STRIDER, C_BOROMIR, C_LEGOLAS, C_GIMLI, C_MERRY, C_PIPPIN, NCOMP, GOLLUM = 7 };
enum { M_WK, M_SARUMAN, M_MOUTH, NMINION };
enum { LOC_FSP = 200, LOC_DEAD = 201, LOC_NONE = 202 }; /* else a region index */
enum { W_DECK, W_HAND, W_TABLE, W_DISCARD };
/* hunt tiles: 7 standard kinds then 8 specials */
enum { T_EYE, T_3, T_2, T_2R, T_1, T_1R, T_0R,
       T_CLOAKS, T_ROPE, T_PHIAL, T_SMEAGOL, T_SHELOB, T_RING_MINE, T_ON_ON, T_GIVE_USS, NTILE };
#define NSTD_TILE 7
enum { WIN_NONE, WIN_RING, WIN_CORRUPTION, WIN_FP_MIL, WIN_SH_MIL };

typedef struct {
    const char *name;
    uint8_t nation, settle, coastal, nadj, adj[8];
    uint8_t setup_nation, setup_reg, setup_eli, setup_ldr, setup_naz;
} RegionInfo;
typedef struct { const char *name; uint8_t type, combat, table; } CardInfo;
extern const RegionInfo REGION[NREG];
extern const CardInfo CARD[NCARD];
extern const char *const CC_NAME[NCC];
extern uint8_t DIST[NREG][NREG];           /* region graph distance */
#define CARD_SIDE(c) ((c) >= 48 ? SH : FP)
#define CARD_DECK(c) ((c) / 24)            /* 0 FP char, 1 FP strat, 2 SH char, 3 SH strat */
#define CARD_IS_CHAR(c) ((CARD_DECK(c) & 1) == 0)

/* ---- decisions ---- */
enum {
    D_NONE, D_ACTION, D_DISCARD, D_FSP_DECLARE, D_GUIDE, D_ENTER_MORDOR, D_HUNT_ALLOC,
    D_ARMY_MOVE2, D_MUSTER2, D_COMP_MOVE, D_NAZ_MOVE, D_SEP_DEST,
    D_BATTLE_CARD, D_CASUALTY, D_CONTINUE, D_RETREAT, D_SIEGE_RETREAT, D_ADVANCE, D_SIEGE_EXTEND,
    D_SELF_HITS, D_HUNT_RESPONSE, D_REVEAL_DEST, D_MITHRIL, D_STAFF,
    D_CARD_REGION, D_CARD_OPTION, D_CARD_NATION, D_CARD_CARD, D_CARD_COMP, D_CARD_ARMY, ND
};
/* the questions of D_CARD_OPTION added to ask what the official texts leave to a player (State.qtag) */
enum { Q_NONE, Q_WORDS_OF_POWER, Q_BLADE, Q_FATEFUL_STRIKE, Q_HEROIC_DEATH, Q_BLACK_BREATH, Q_WORN_WITH_SORROW, Q_NAZGUL_STRIKE, Q_BCC_NAZGUL, Q_BCC_ARMY, Q_BCC_ATTACK, Q_LURE, Q_EXCESS, Q_FLOCKS, Q_BALROG, Q_WHITE_RIDER };
/* D_FSP_DECLARE: one question for the whole Fellowship phase, in any order (RB p. 16): 255 ends it, a region declares
   the Fellowship there, FSP_GUIDE + c makes Companion c the Guide, FSP_MORDOR enters Mordor */
#define FSP_GUIDE 200
#define FSP_MORDOR 199
#define Q_HAND 254      /* option: a random card from the hand */
#define Q_HUNT 253      /* option: roll for the Hunt */
#define Q_LEADER 100    /* option: a Leader of nation (Q_LEADER + nation) */
#define Q_MINION 20     /* option: a Minion (Q_MINION + minion) */
#define Q_NAZGUL 50     /* option: a Nazgul */
/* move types */
enum {
    M_CHOICE,       /* a = option value (generic decisions) */
    M_PASS, M_SKIP, /* die = face */
    M_RING,         /* a = from face, b = to face */
    M_FSP_MOVE, M_FSP_HIDE,
    M_SEPARATE,     /* a = companion mask */
    M_COMPS,        /* start companion movement chain */
    M_ARMY,         /* a=src b=dst c=#regulars d=#elites e=leaders-follow f=1 if army action (2 moves) */
    M_ATTACK,       /* a=src b=dst c=#regulars d=#elites (attacking part; leaders always join) */
    M_PLAY,         /* a = card */
    M_DRAW,         /* a = 0 character deck / 1 strategy deck */
    M_DIPLO,        /* a = nation */
    M_MUSTER_ELITE, /* a = region */
    M_MUSTER,       /* a = region, b = 0 regular / 1 leader-or-nazgul ; first (or second) of two */
    M_GANDALF_WHITE,/* a = region */
    M_ARAGORN,
    M_COMP_POLITICS,/* a = nation (Boromir/Legolas/Gimli) */
    M_DISCARD_TABLE,/* a = card, b = 1 if an elven ring is also spent */
    M_NAZGUL,       /* start nazgul/minion movement chain */
    M_MINION,       /* a = minion, b = region */
    M_VOICE,        /* a = 0 regulars in settlements / 1 two elites in Orthanc */
    M_COMP_MOVE,    /* a = companion mask, b = dst             (chain step) */
    M_NAZ_MOVE,     /* a = src, b = dst, c = count, d = 1 witch-king, 2 mouth (chain step) */
    M_DONE,
    M_NAZ_PLAN,     /* a = main army region (255 none), b = second army (255 none), c = 1: one Nazgul hunts the Fellowship */
    NMOVETYPE
};
typedef struct { uint8_t type, die, a, b, c, d, e, f; } Move;
#define DIE_MOUTH 0x80   /* Move.die flag: muster die used as army die (Mouth of Sauron) */
#define MAXMOVES 1024

typedef struct { uint8_t kind; uint8_t v[9]; uint16_t step; } Frame;
#define MAXSTACK 24
#define MAXOPT 128

typedef struct {
    uint8_t active, att, src, dst, siege, sortie, round, max_rounds;
    uint8_t no_cease, help_unlooked, nocard_r1, white_rider, relief;
    uint8_t card[2];                /* this round's combat cards (255 none), by side */
    uint8_t ended;                  /* 1 attacker ceased, 2 defender retreated/eliminated, 3 attacker eliminated, 4 into siege */
    uint8_t rear_reg[NNAT], rear_eli[NNAT], rear_ldr[NFPNAT], rear_naz, rear_comp, rear_min;
    int8_t  hits[2];                /* hits scored BY side this round */
    uint8_t pre_fp_hits;            /* FP pre-combat hits this round (Mumakil) */
    uint8_t forfeit[2];             /* leadership forfeited this round */
    uint8_t wop_comp;               /* Words of Power target companion or 255 */
    uint8_t reroll_hits[2];
    /* rules as the official texts have them (see the pull request for the texts) */
    uint8_t sorc;                   /* the Witch-king's Sorcerer draw due (deck + 1), drawn when round 1 is over */
    uint8_t dd;                     /* Daring Defiance cancelled the Shadow card: every Companion forfeits his Leadership */
    uint8_t pre_sh_hits;            /* Shadow pre-combat hits this round (Durin's Bane), for Mumakil's count */
} Battle;

typedef struct State {
    /* board */
    uint8_t reg[NREG][NNAT], eli[NREG][NNAT];
    uint8_t ldr[NREG][NFPNAT];
    uint8_t naz[NREG];
    uint8_t ctrl[NREG];             /* side controlling the settlement (if any) */
    uint8_t siege[NREG];            /* 1: units of ctrl[r] are besieged inside the stronghold */
    uint8_t comp_loc[NCOMP], minion_loc[NMINION];
    uint8_t gandalf_white, aragorn, minion_seen;
    /* reinforcements & casualties */
    uint8_t pool_reg[NNAT], pool_eli[NNAT], pool_ldr[NFPNAT], pool_naz;
    uint8_t cas_reg[NFPNAT];
    /* politics */
    uint8_t pol[NNAT], active[NNAT]; /* pol = steps from At War (0 = at war) */
    /* fellowship */
    uint8_t fsp_loc, progress, revealed, corruption, guide;
    int8_t  mordor;                 /* -1 not on track, else step 0..5 */
    uint8_t fsp_acted;              /* moved or hid this turn (Mordor rule) */
    /* hunt */
    uint8_t hunt_sh, hunt_fp, hunt_fp_prev;
    uint8_t tile_pool[NTILE], tile_out[NTILE], special_st[NTILE]; /* special_st: 0 no, 1 waiting, 2 entered */
    /* dice, rings */
    uint8_t dice[2][NFACE];
    uint8_t rings[2], ring_used[2], mouth_used;
    /* cards */
    uint8_t card_where[NCARD];
    uint8_t hand_n[2];
    /* flow */
    uint16_t turn;
    uint8_t winner, win_type;       /* winner: 0 none, 1 FP, 2 Shadow */
    uint8_t dkind, dplayer;
    uint8_t nopt, opt[MAXOPT];      /* options of generic decisions */
    Move mv;                        /* last move applied */
    uint8_t ret;                    /* return value from a child frame */
    uint8_t sp;
    Frame st[MAXSTACK];
    uint64_t lock[2];               /* regions locked as sources within the current action */
    uint8_t comp_moved;             /* companions already moved within the current action */
    uint8_t act_moves_left;
    Battle b;
    uint64_t rng;
    uint32_t nrand;                 /* number of random draws so far (chance detection) */
    uint8_t rng_max;                /* 1: every random draw returns its highest value (used to ask whether a card could do anything) */
    /* stats for analysis / shaping */
    uint8_t stat_fsp_moves, stat_battles;
    uint16_t stat_hits[2];           /* combat hits absorbed by each side's units so far (regular 1, elite reduced 1, elite removed 2) */
    uint8_t stat_tiles;              /* Hunt tiles drawn so far */
    uint8_t stat_vp_walk[2], stat_vp_fight[2];   /* enemy VP taken by walking into an empty settlement / by battle or siege (never decremented) */
    uint8_t mithril_tile, mithril_frame;   /* Mithril Coat and Sting: the first tile (+1) held out of the pool until the Hunt of frame mithril_frame ends */
    uint8_t hunt_drawn;              /* the tile a HF_DRAW_ONLY Hunt drew (255: none, e.g. Wizard's Staff) */
    uint8_t naz_moved[NREG];         /* Nazgul that have moved into each region in this action: each Nazgul moves once, the others may still go */
    uint8_t naz_only;               /* a card moves "the Nazgul" (the Witch-king among them): not the Mouth of Sauron */
    uint8_t qreg;                    /* the region a Q_EXCESS question is about */
    uint8_t qtag;                    /* which question a generic D_CARD_OPTION is (Q_*: written for the protocol by bridge.c) */
} State;

/* 1 (default): structure-aware generation that drops moves with no purpose (see movegen.c).
 * 0: every legal move the engine models -- used by rules tests and log replay. */
extern int wotr_prune;

/* ---- public API ---- */
void wotr_global_init(void);
void wotr_init(State *s, uint64_t seed);
int  wotr_moves(const State *s, Move *out);
void wotr_apply(State *s, Move m);
int  wotr_vp(const State *s, int side);
void wotr_move_str(const State *s, Move m, char *buf, int n);
void wotr_print(const State *s);
int  wotr_check(const State *s, char *err, int n);   /* invariant checker; 0 if ok */
void wotr_blank(State *s);                           /* an empty board resting at an action decision, to be filled with wotr_set */
int  wotr_set(State *s, const char *line);           /* one line of the position language; 1 if understood */
int  wotr_begin_turn(State *s, uint64_t seed, char *err, int n);
int  wotr_mid_turn(State *s, int side, uint64_t seed, char *err, int en);   /* end-of-turn board -> first decision of the next turn; 0 if playable, else err says why not */
void wotr_determinize(State *s, int pov, uint64_t seed); /* resample info hidden from pov */

/* ---- internal (shared across engine translation units) ---- */
enum {
    FR_GAME, FR_ACTIONS, FR_DISCARD, FR_FSP_PHASE, FR_HUNT, FR_REVEAL, FR_SEPARATE, FR_ARMY2, FR_MUSTER2,
    FR_COMPMOVE, FR_NAZMOVE, FR_BATTLE, FR_ROUND, FR_CASUALTY, FR_CARD, FR_FSP_MOVE, FR_CARDFX, NFRAME
};
typedef void (*FrameFn)(State *, Frame *);
Frame *push(State *s, int kind);
void pop(State *s);
#define CO_BEGIN switch (f->step) { case 0:
#define CO_END } pop(s); return;
/* __COUNTER__ gives every suspension point in a translation unit a unique resume label */
#define ASK_(k, p, id) do { f->step = (id); s->dkind = (k); s->dplayer = (p); return; case (id):; } while (0)
#define ASK(k, p) ASK_(k, p, __COUNTER__ + 1)
#define CALL_(expr, id) do { f->step = (id); expr; return; case (id):; } while (0)
#define CALL(expr) CALL_(expr, __COUNTER__ + 1)

/* rng */
static inline uint32_t rnd(State *s, uint32_t n) {
    if (s->rng_max) { s->nrand++; return n - 1; }
    uint64_t z = (s->rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; z ^= z >> 31;
    s->nrand++;
    return (uint32_t)(((z >> 32) * n) >> 32);
}
static inline int d6(State *s) { return (int)rnd(s, 6) + 1; }

/* core.c */
static inline int orig_side(int r) { return REGION[r].nation == NO_NATION ? 2 : NAT_SIDE(REGION[r].nation); }
static inline int is_settlement(int r) { int t = REGION[r].settle; return t == S_TOWN || t == S_CITY || t == S_STRONGHOLD; }
int  units(const State *s, int side, int r);
int  elites(const State *s, int side, int r);
int  fp_leaders(const State *s, int r);
int  comps_at(const State *s, int r);            /* mask of companions on the map in r */
int  minions_at(const State *s, int r);
int  comp_level(const State *s, int c);
int  comp_leadership(const State *s, int c);
int  enemy_field_units(const State *s, int side, int r); /* enemy units not besieged by side */
int  besieged_side(const State *s, int r);       /* side inside, or -1 */
int  at_war(const State *s, int n);
int  all_at_war(const State *s, int side);
void activate(State *s, int n);
int  advance_pol(State *s, int n, int cause);    /* cause: 0 die/card, 1 attack, 2 companion ability, 3 capture */
void set_ctrl(State *s, int r, int side);
void army_entered(State *s, int side, int r);    /* capture + activation after units arrive */
void kill_unit(State *s, int r, int n, int elite);
int  downgrade_elite(State *s, int r, int n);
void wipe_leaders(State *s, int side, int r, int kill_chars);
void kill_comp(State *s, int c);
void kill_minion(State *s, int m);
int  recruit(State *s, int r, int n, int elite); /* 1 if placed */
int  recruit_leader(State *s, int r, int n);
int  recruit_naz(State *s, int r);
void enforce_stacking(State *s, int side, int r);
void add_corruption(State *s, int n);
void heal(State *s, int n);
int  draw_card(State *s, int deck);              /* returns card or -1 */
void discard_card(State *s, int c);
void table_upkeep(State *s);
int  fsp_comps(const State *s);                  /* mask of companions in the fellowship */
void fix_guide(State *s);
int  free_for_recruit(const State *s, int side, int r);
int  shadow_leadership_units(const State *s, int r);
int  card_has_effect(const State *s, Move m);
int  can_enter_army(const State *s, int side, int src, int dst);
int  movable_units(const State *s, int side, int src, int dst, int attack, uint8_t *mr, uint8_t *me);
void move_army(State *s, int side, int src, int dst, int nr, int ne, int leaders, int attack_pick);
int  comp_reach(const State *s, int from, int range, int siege_end, uint8_t *out);
void place_comps(State *s, int mask, int r);
int  has_army_leader(const State *s, int side, int r);
int  protected_region(const State *s, int r);    /* A Power too Great / Tom Bombadil */
void relocate_army(State *s, int side, int from, int to); /* whole army incl. leaders */
void fix_siege(State *s, int r);
/* movegen.c */
int  gen_action(const State *s, Move *out);
int  gen_army_moves(const State *s, int side, int need_leader, Move proto, Move *out, int max);
int  gen_attacks(const State *s, int side, int need_leader, Move proto, Move *out);
int  gen_muster(const State *s, int side, Move proto, int exclude_region, Move *out);
int  gen_comp_moves(const State *s, Move *out);
int  gen_naz_moves(const State *s, Move *out);
int  card_playable(const State *s, int c);
int  comp_dests(const State *s, int mask, const uint8_t *reach, int from, uint8_t *out);   /* purposeful destinations among reach[] */
void apply_naz_plan(State *s, Move m);
/* flow.c */
void fr_game(State *, Frame *); void fr_actions(State *, Frame *); void fr_discard(State *, Frame *);
void fr_army2(State *, Frame *); void fr_muster2(State *, Frame *);
void fr_compmove(State *, Frame *); void fr_nazmove(State *, Frame *);
void do_naz_move(State *s, Move m);
/* fellowship.c */
void fr_fsp_phase(State *, Frame *); void fr_hunt(State *, Frame *); void fr_reveal(State *, Frame *);
void fr_separate(State *, Frame *); void fr_fsp_move(State *, Frame *);
void push_hunt(State *s, int mode, int flags);
void push_separate(State *s, int mask, int bonus, int siege_end, int fixed_level);
void push_reveal(State *s);
void enter_special_tile(State *s, int t);
int  draw_tile(State *s);
enum { HM_ROLL, HM_MORDOR, HM_CARD, HM_STRONGHOLD };
enum { HF_NO_REDUCE = 1, HF_RANDOM_FIRST = 2, HF_IGNORE_REVEAL = 4, HF_EYE_DISCARD = 8, HF_DRAW_ONLY = 16 };   /* DRAW_ONLY: the tile only, in hunt_drawn (Wizard's Staff and Mithril offered) */
/* battle.c */
void fr_battle(State *, Frame *); void fr_round(State *, Frame *); void fr_casualty(State *, Frame *);
void push_battle(State *s, int side, int src, int dst, int nr, int ne);
void attacked_politics(State *s, int side, int r);
void push_casualty(State *s, int side, int r, int hits, int kill_chars);
int  cc_playable(const State *s, int side, int c);
int  army_leadership(const State *s, int side, int r);
int  nazgul_leadership(const State *s, int r);
/* cards.c */
void fr_card(State *, Frame *); void fr_cardfx(State *, Frame *);
void push_card(State *s, int c, int by_event_die);
#endif
