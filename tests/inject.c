/* inject.c -- loads positions written by tools/oracle/compare.py (taken from an independent engine) into a State
 * resting at an action decision and prints every legal move of the full generator, so the two engines' legal
 * sets can be compared. Input: one command per line; "go <side>" prints the moves and resets for the next one. */
#include "wotr.h"
#include "features.h"
#include "nn.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Same board before and after a card play: units, control, characters, politics, reinforcements, the Fellowship and
 * the Hunt pool; within the same turn also the dice (less the one spent). Card draws and a new turn's roll are ignored. */
static int board_same(const State *s, const State *t, Move m) {
    State e = *s;
    e.dice[e.dplayer][m.die & 0x7F]--;
    return !t->winner && t->card_where[m.a] != W_TABLE
        && !memcmp(t->reg, e.reg, sizeof e.reg) && !memcmp(t->eli, e.eli, sizeof e.eli) && !memcmp(t->ldr, e.ldr, sizeof e.ldr)
        && !memcmp(t->naz, e.naz, sizeof e.naz) && !memcmp(t->ctrl, e.ctrl, sizeof e.ctrl) && !memcmp(t->siege, e.siege, sizeof e.siege)
        && !memcmp(t->comp_loc, e.comp_loc, sizeof e.comp_loc) && !memcmp(t->minion_loc, e.minion_loc, sizeof e.minion_loc)
        && !memcmp(t->pol, e.pol, sizeof e.pol) && !memcmp(t->active, e.active, sizeof e.active)
        && !memcmp(t->pool_reg, e.pool_reg, sizeof e.pool_reg) && !memcmp(t->pool_eli, e.pool_eli, sizeof e.pool_eli)
        && !memcmp(t->tile_pool, e.tile_pool, sizeof e.tile_pool) && !memcmp(t->special_st, e.special_st, sizeof e.special_st)
        && t->fsp_loc == e.fsp_loc && t->progress == e.progress && t->revealed == e.revealed && t->corruption == e.corruption && t->mordor == e.mordor
        && t->rings[0] == e.rings[0] && t->rings[1] == e.rings[1] && t->gandalf_white == e.gandalf_white && t->aragorn == e.aragorn
        && (t->turn != s->turn || (!memcmp(t->dice, e.dice, sizeof e.dice) && t->hunt_sh == e.hunt_sh));
}

/* Does the play change the board, now or after any answers to the questions the card asks (searched three deep)? */
static int has_effect(const State *s, const State *t, Move m, int depth) {
    if (!board_same(s, t, m)) return 1;
    if (t->winner || t->dkind < D_CARD_REGION || t->turn != s->turn || depth >= 3) return 0;
    Move sub[MAXMOVES]; int k = wotr_moves(t, sub);
    for (int j = 0; j < k; j++) { State u = *t; wotr_apply(&u, sub[j]); if (has_effect(s, &u, m, depth + 1)) return 1; }
    return 0;
}

static void print_moves(const State *s, const char *indent) {
    Move mv[MAXMOVES]; int n = wotr_moves(s, mv);
    for (int i = 0; i < n; i++) {
        Move m = mv[i];
        int noeffect = 0;
        if (!indent[0] && m.type == M_PLAY) {       /* does playing this card change anything on the board? */
            State t = *s;
            wotr_apply(&t, m);
            noeffect = !has_effect(s, &t, m, 0);
        }
        if (!indent[0] && m.type == M_PLAY && getenv("INJECT_DEBUG")) {
            State t = *s; wotr_apply(&t, m);
            fprintf(stderr, "play %s: noeffect %d, then decision %d by %d, card now %d, units %s, politics %s, dice %s, corruption %d->%d\n", CARD[m.a].name, noeffect,
                    t.dkind, t.dplayer, t.card_where[m.a], memcmp(t.reg, s->reg, sizeof t.reg) || memcmp(t.eli, s->eli, sizeof t.eli) ? "changed" : "same",
                    memcmp(t.pol, s->pol, sizeof t.pol) || memcmp(t.active, s->active, sizeof t.active) ? "changed" : "same", memcmp(t.dice, s->dice, sizeof t.dice) ? "changed" : "same", s->corruption, t.corruption);
        }
        printf("%s%d %d %d %d %d %d %d %d%s\n", indent, m.type, m.die, m.a, m.b, m.c, m.d, m.e, m.f, noeffect ? " noeffect" : "");
        if (!indent[0] && (m.type == M_COMPS || m.type == M_NAZGUL)) {      /* chain starters: list the first chain step */
            State t = *s; wotr_apply(&t, m);
            if (!t.winner && (t.dkind == D_COMP_MOVE || t.dkind == D_NAZ_MOVE)) print_moves(&t, "  ");
        }
    }
}

int main(void) {
    State s; char line[256]; int bside = 0;
    wotr_prune = 0;
    feat_init();
    #define RESET() wotr_blank(&s)
    RESET();
    while (fgets(line, sizeof line, stdin)) {
        int a = 0, b = 0, c = 0, d = 0; char k[16];
        if (sscanf(line, "%15s %d %d %d %d", k, &a, &b, &c, &d) < 1) continue;
        if (wotr_set(&s, line)) continue;
        if (!strcmp(k, "side")) bside = a;
        else if (!strcmp(k, "battle")) {            /* battle <from> <to> <defender withdraws into the siege 0/1> <trials>: fight it with fixed simple choices */
            long n = d, attw = 0, defw = 0, cap = 0, sieged = 0; double al = 0, dl = 0;
            s.dplayer = (uint8_t)bside; s.st[s.sp - 1].v[0] = (uint8_t)bside;
            Move mv[MAXMOVES], att; int nm = wotr_moves(&s, mv), found = 0;
            for (int i = 0; i < nm; i++) if (mv[i].type == M_ATTACK && mv[i].a == a && mv[i].b == b && (!found || mv[i].c + mv[i].d > att.c + att.d)) { att = mv[i]; found = 1; }
            if (!found) { printf("noattack\nend\n"); fflush(stdout); RESET(); continue; }
            int a0 = units(&s, bside, a) - (a == b ? 0 : 0), d0 = units(&s, 1 - bside, b);
            for (long i = 0; i < n; i++) {
                State t = s; { uint64_t z = (uint64_t)(i + 1) * 0xD1342543DE82EF95ULL; z ^= z >> 32; z *= 0xBF58476D1CE4E5B9ULL; z ^= z >> 29; t.rng = z; }   /* well-separated streams */
                wotr_apply(&t, att);
                while (!t.winner && t.b.active) {
                    Move o[MAXMOVES]; int k2 = wotr_moves(&t, o), pick = 0;
                    switch (t.dkind) {
                    case D_SIEGE_RETREAT: for (int j = 0; j < k2; j++) if (o[j].a == c) pick = j; break;
                    case D_BATTLE_CARD: case D_RETREAT: case D_SIEGE_EXTEND: for (int j = 0; j < k2; j++) if (o[j].a == 255) pick = j; break;
                    case D_CASUALTY: for (int j = 1; j < k2; j++) if (o[j].a < o[pick].a) pick = j; break;      /* regulars first, then reduce an elite */
                    case D_CONTINUE: case D_ADVANCE: for (int j = 0; j < k2; j++) if (o[j].a == 1) pick = j; break;
                    default: break;
                    }
                    wotr_apply(&t, o[pick]);
                }
                int au = units(&t, bside, a) + (a == b ? 0 : units(&t, bside, b)), du = units(&t, 1 - bside, b);
                attw += au == 0; defw += du == 0; al += a0 - au; dl += d0 - du; cap += t.ctrl[b] != s.ctrl[b]; sieged += t.siege[b] && !s.siege[b];
            }
            printf("battle %ld %.4f %.4f %.4f %.4f %.4f %.4f\nend\n", n, (double)attw / n, (double)defw / n, al / n, dl / n, (double)cap / n, (double)sieged / n);
            fflush(stdout); RESET();
        }
        else if (!strcmp(k, "prior")) {             /* the strategy formula's terms for this position (tools/humanlogs/positions.py) */
            Feat f; feat_encode(&s, &f);
            printf("prior");
            for (int i = 0; i < NPRIOR; i++) printf(" %.4f", f.prior[i]);
            printf(" %d %d", wotr_vp(&s, SH), wotr_vp(&s, FP));
            { int mo, fr, wa, in, be; shadow_deploy(&s, &mo, &fr, &wa, &in, &be); printf(" %d %d %d %d %d", mo, fr, wa, in, be); }
            {   /* INJECT_NET=<value net file>: also the trained network's win logit for the Free Peoples */
                static Net *net; static int tried;
                if (!tried) { tried = 1; const char *np = getenv("INJECT_NET"); if (np) { net = malloc(sizeof(Net)); if (nn_load(net, np)) { free(net); net = 0; } } }
                if (net) { float out[NOUT]; s.dplayer = 0; nn_forward(net, &f, out); printf(" %.4f", out[0]); }
            }
            printf("\n");
            if (getenv("INJECT_LOCAL")) {       /* one line per victory-point settlement: region, original owner, holder, garrison, enemies next door, two away, siege, leadership */
                for (int k = 0; k < NVPS; k++) { int h, mi, e1, e2, sg, la, ld, r = vps_local(&s, k, &h, &mi, &e1, &e2, &sg, &la, &ld);
                    printf("local %d %d %d %d %d %d %d %d %d %d\n", r, orig_side(r), h, mi, e1, e2, sg, la, ld, REGION[r].settle == S_STRONGHOLD); }
            }
            fflush(stdout);
            {   /* INJECT_SAMPLES=<file>: append the position's network inputs, once as "Free Peoples to decide" and once as
                   "Shadow to decide": int32 count, uint16 index[count], float value[count], float terms[NPRIOR] */
                static FILE *sf; static int tried;
                if (!tried) { tried = 1; const char *sp = getenv("INJECT_SAMPLES"); if (sp) sf = fopen(sp, "wb"); }
                if (sf) for (int side = 0; side < 2; side++) {
                    s.dplayer = (uint8_t)side; s.st[s.sp - 1].v[0] = (uint8_t)side; feat_encode(&s, &f);
                    int32_t n = f.n; fwrite(&n, sizeof n, 1, sf); fwrite(f.idx, sizeof(uint16_t), (size_t)n, sf); fwrite(f.val, sizeof(float), (size_t)n, sf); fwrite(f.prior, sizeof(float), NPRIOR, sf);
                }
                if (sf) fflush(sf);
            }
            RESET();
        }
        else if (!strcmp(k, "go")) {
            s.dplayer = (uint8_t)a; s.st[s.sp - 1].v[0] = (uint8_t)a;
            print_moves(&s, "");
            printf("end\n"); fflush(stdout);
            RESET();
        }
    }
    return 0;
}
