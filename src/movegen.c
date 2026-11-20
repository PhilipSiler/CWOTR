/* movegen.c -- legal move generation for the structured decisions */
#include "wotr.h"

int wotr_prune = 1;
int wotr_roads = 1;

#define ANYDIE NFACE
static int locked(const State *s, int r) { return (int)((s->lock[r >> 6] >> (r & 63)) & 1); }

/* Which die pays for an action category. More specific dice are always spent first
 * (a Muster/Army die can do anything an Army die can; Will of the West can do anything). */
static int payer(const State *s, int side, int cat) {
    const uint8_t *d = s->dice[side];
    switch (cat) {
    case F_CHAR: if (d[F_CHAR]) return F_CHAR; break;
    case F_ARMY: if (d[F_ARMY]) return F_ARMY; if (d[F_MA]) return F_MA; break;
    case F_MUSTER: if (d[F_MUSTER]) return F_MUSTER; if (d[F_MA]) return F_MA; break;
    case F_EVENT: if (d[F_EVENT]) return F_EVENT; break;
    default: {
        static const uint8_t ord[] = {F_MUSTER, F_ARMY, F_EVENT, F_MA, F_CHAR, F_SPECIAL};
        for (int i = 0; i < 6; i++) if (d[ord[i]]) return ord[i];
        return -1; }
    }
    if (side == FP && d[F_SPECIAL]) return F_SPECIAL;
    if (side == SH && cat == F_ARMY && d[F_MUSTER] && !s->mouth_used && s->minion_loc[M_MOUTH] < NREG) return F_MUSTER | DIE_MOUTH;
    return -1;
}

static int add_comp(Move *out, int n, int max, Move m, int nr, int ne, int L, int cap, int first) {
    if (nr + ne == 0 || nr + ne > cap || n >= max) return n;
    for (int i = first; i < n; i++) if (out[i].c == nr && out[i].d == ne && out[i].e == L) return n;
    m.c = (uint8_t)nr; m.d = (uint8_t)ne; m.e = (uint8_t)L;
    out[n++] = m;
    return n;
}

/* ---- objective distance fields: an army move has a purpose if it shortens the way to something ---- */
static void bfs_multi(const uint8_t *src, uint8_t *d) {
    uint8_t q[NREG]; int qh = 0, qt = 0;
    memset(d, 0xFF, NREG);
    for (int r = 0; r < NREG; r++) if (src[r]) { d[r] = 0; q[qt++] = (uint8_t)r; }
    while (qh < qt) {
        int u = q[qh++];
        for (int i = 0; i < REGION[u].nadj; i++) { int v = REGION[u].adj[i]; if (d[v] == 0xFF) { d[v] = d[u] + 1; q[qt++] = (uint8_t)v; } }
    }
}
typedef struct { uint8_t off[NREG], def[NREG], enemy3[NREG]; } Fields;
/* off: distance to the nearest victory-point settlement the enemy holds; def: to the nearest own settlement
 * (or besieged friendly garrison) with enemy units within two regions; enemy3: enemy units within three */
static void objective_fields(const State *s, int side, Fields *F) {
    uint8_t so[NREG], sd[NREG], near[NREG], tmp[NREG];
    for (int r = 0; r < NREG; r++) near[r] = units(s, 1 - side, r) > 0;
    for (int step = 0; step < 3; step++) {                 /* dilate the enemy's presence along the region graph */
        memcpy(tmp, near, NREG);
        for (int r = 0; r < NREG; r++) if (tmp[r]) for (int i = 0; i < REGION[r].nadj; i++) near[REGION[r].adj[i]] = 1;
        if (step == 1) for (int r = 0; r < NREG; r++) sd[r] = is_settlement(r) && s->ctrl[r] == side && (near[r] || s->siege[r]);
    }
    memcpy(F->enemy3, near, NREG);
    for (int r = 0; r < NREG; r++) { int t = REGION[r].settle; so[r] = (t == S_CITY || t == S_STRONGHOLD) && s->ctrl[r] != side; }
    bfs_multi(so, F->off); bfs_multi(sd, F->def);
}

int gen_army_moves(const State *s, int side, int need_leader, Move proto, Move *out, int max) {
    int n = 0;
    uint8_t mr[NNAT], me[NNAT];
    Fields F;
    if (wotr_prune) objective_fields(s, side, &F);
    for (int src = 0; src < NREG; src++) {
        if (!units(s, side, src) || locked(s, src)) continue;
        if (s->siege[src] && s->ctrl[src] == side) continue;
        if (need_leader && !has_army_leader(s, side, src)) continue;
        /* Saruman never leaves Orthanc, so he cannot lead a move (he can lead an attack, whose units stay put) */
        if (need_leader && side == SH && src == R_ORTHANC && !s->naz[src] && s->minion_loc[M_WK] != src && s->minion_loc[M_MOUTH] != src && !shadow_leadership_units(s, src)) continue;
        int garrison = (is_settlement(src) && s->ctrl[src] == side) || (s->siege[src] && s->ctrl[src] != side);
        for (int i = 0; i < REGION[src].nadj; i++) {
            int dst = REGION[src].adj[i];
            if (!can_enter_army(s, side, src, dst)) continue;
            if (!movable_units(s, side, src, dst, 0, mr, me)) continue;
            int R = 0, E = 0;
            for (int k = 0; k < NNAT; k++) { R += mr[k]; E += me[k]; }
            int cap = 10 - units(s, side, dst), tot = R + E, first = n;
            if (cap <= 0) continue;
            Move m = proto; m.a = (uint8_t)src; m.b = (uint8_t)dst;
            if (wotr_prune) {
                int merge = units(s, side, dst) > 0, settle = is_settlement(dst) || REGION[dst].settle == S_FORT;
                int closer = F.def[dst] < F.def[src] || (F.off[dst] < F.off[src] && (side == SH || tot >= 4));
                /* Shadow may also march toward ANY city or stronghold the enemy holds within four regions, not only the
                   nearest one: humans' commonest unoffered move was Dagorlad > Noman-Lands, the Mordor army turning north
                   for Dale (2026-11-16, from the human game logs; wotr_roads = 0 gives the old rule). */
                if (!closer && side == SH && wotr_roads)
                    for (int t = 0; t < NREG && !closer; t++) { int ty = REGION[t].settle; closer = (ty == S_CITY || ty == S_STRONGHOLD) && s->ctrl[t] != side && DIST[dst][t] < DIST[src][t] && DIST[dst][t] <= 4; }
                /* ... and it may go ROUND a blocked road (2026-11-18): a march that gets no nearer a target is offered when
                   every region that does lead nearer from here holds an enemy army and the new region has an open way on.
                   AlphaWOTR takes Minas Tirith on turn 3 like this: North Ithilien > Dead Marshes > Druadan Forest, round
                   the Free Peoples in Osgiliath, onto a thin garrison. Our Shadow was only ever offered the frontal road. */
                if (!closer && side == SH && wotr_roads)
                    for (int t = 0; t < NREG && !closer; t++) {
                        int ty = REGION[t].settle;
                        if (!(ty == S_CITY || ty == S_STRONGHOLD) || s->ctrl[t] == side || DIST[dst][t] != DIST[src][t] || DIST[src][t] > 3 || DIST[src][t] < 2) continue;
                        int blocked = 1, open = 0;
                        for (int j = 0; j < REGION[src].nadj && blocked; j++) { int y = REGION[src].adj[j]; if (DIST[y][t] < DIST[src][t] && !units(s, 1 - side, y)) blocked = 0; }
                        for (int j = 0; j < REGION[dst].nadj && !open; j++) { int z = REGION[dst].adj[j]; open = DIST[z][t] < DIST[dst][t] && (z == t || !units(s, 1 - side, z)); }
                        closer = blocked && open;
                    }
                if (!merge && !settle && !closer) continue;                    /* walking away from everything */
                if (tot <= cap) n = add_comp(out, n, max, m, R, E, 1, cap, first);
                else { int e = E < cap ? E : cap; n = add_comp(out, n, max, m, cap - e < R ? cap - e : R, e, 1, cap, first); }
                if (tot >= 2 && garrison) n = R ? add_comp(out, n, max, m, R - 1, E, 1, cap, first) : add_comp(out, n, max, m, R, E - 1, 1, cap, first);
                if (!need_leader && tot >= 3 && is_settlement(dst) && !merge) n = add_comp(out, n, max, m, R ? 1 : 0, R ? 0 : 1, 0, cap, first);   /* errand: take or hold an empty settlement */
                continue;
            }
            if (tot <= cap) n = add_comp(out, n, max, m, R, E, 1, cap, first);
            else { int e = E < cap ? E : cap; n = add_comp(out, n, max, m, cap - e < R ? cap - e : R, e, 1, cap, first); }
            if (tot >= 2) n = R ? add_comp(out, n, max, m, R - 1, E, 1, cap, first) : add_comp(out, n, max, m, R, E - 1, 1, cap, first);
            if (!need_leader && tot >= 3) n = add_comp(out, n, max, m, R ? 1 : 0, R ? 0 : 1, 0, cap, first);
            if (tot >= 4) n = add_comp(out, n, max, m, (R + 1) / 2, E / 2, 1, cap, first);
        }
    }
    return n;
}

int gen_attacks(const State *s, int side, int need_leader, Move proto, Move *out) {
    int n = 0;
    uint8_t mr[NNAT], me[NNAT];
    for (int src = 0; src < NREG; src++) {
        if (!units(s, side, src)) continue;
        if (need_leader && !has_army_leader(s, side, src)) continue;
        if (!movable_units(s, side, src, src, 1, mr, me)) continue;
        int R = 0, E = 0;
        for (int k = 0; k < NNAT; k++) { R += mr[k]; E += me[k]; }
        Move m = proto; m.a = (uint8_t)src; m.c = (uint8_t)R; m.d = (uint8_t)E;
        if (s->siege[src]) {                       /* siege assault or sortie: only target is here */
            if (side == SH && protected_region(s, src)) continue;
            m.b = (uint8_t)src; out[n++] = m;
            if (s->ctrl[src] == side) continue;    /* besieged: no other targets */
        }
        for (int i = 0; i < REGION[src].nadj; i++) {
            int dst = REGION[src].adj[i];
            if (!enemy_field_units(s, side, dst)) continue;
            if (side == SH && protected_region(s, dst)) continue;
            m.b = (uint8_t)dst; m.c = (uint8_t)R; m.d = (uint8_t)E; out[n++] = m;
            if (R + E >= 2 && is_settlement(src) && !s->siege[src]) {   /* leave a one-unit rearguard */
                if (R) m.c--; else m.d--;
                out[n++] = m;
            }
        }
    }
    return n;
}

/* Muster-die recruitment of one regular (b=0) or one leader/Nazgul (b=1) */
int gen_muster(const State *s, int side, Move proto, int exclude, Move *out) {
    int n = 0;
    for (int r = 0; r < NREG; r++) {
        int nat = REGION[r].nation;
        if (!is_settlement(r) || nat == NO_NATION || NAT_SIDE(nat) != side || !at_war(s, nat)) continue;
        if (s->ctrl[r] != side || s->siege[r] || units(s, 1 - side, r) || r == exclude) continue;
        Move m = proto; m.a = (uint8_t)r;
        if (s->pool_reg[nat]) { m.b = 0; out[n++] = m; }    /* even into a full region (RB p. 26-27) */
        if (side == FP && s->pool_ldr[nat] && units(s, FP, r)) { m.b = 1; out[n++] = m; }
        if (nat == SAURON && REGION[r].settle == S_STRONGHOLD && s->pool_naz) { m.b = 1; out[n++] = m; }
    }
    return n;
}

static int comp_range(const State *s, int mask) {
    int best = 0, cnt = 0, hob = 0;
    for (int c = 0; c < NCOMP; c++) if ((mask >> c) & 1) {
        cnt++; if (c >= C_MERRY) hob++;
        if (comp_level(s, c) > best) best = comp_level(s, c);
    }
    if ((mask & 1) && s->gandalf_white && (cnt == 1 || (cnt == 2 && hob == 1))) return 4;
    return best;
}

/* Destinations worth a companion's walk: armies that may fight, settlements he can rouse, the places his
 * own card abilities need -- or, failing all that, the reachable region closest to one of them. */
int comp_dests(const State *s, int mask, const uint8_t *reach, int from, uint8_t *out) {
    int n = 0;
    if (!wotr_prune) { for (int r = 0; r < NREG; r++) if (reach[r] && r != from) out[n++] = (uint8_t)r; return n; }
    uint8_t goal[NREG], near[NREG];                               /* near: Shadow units within two regions */
    int any_reach = 0, org = from < NREG ? from : s->fsp_loc;     /* from = 255: separating from the Fellowship, staying put is allowed */
    memset(near, 0, sizeof near);
    for (int q = 0; q < NREG; q++) if (units(s, SH, q)) for (int r = 0; r < NREG; r++) near[r] |= DIST[q][r] <= 2;
    for (int r = 0; r < NREG; r++) {
        int t = REGION[r].settle, nat = REGION[r].nation, g = 0;
        int haven = (t == S_CITY || t == S_STRONGHOLD) && nat < NFPNAT && s->ctrl[r] == FP;
        if ((haven || s->siege[r] || near[r]) && units(s, FP, r)) g = 1;      /* an army that could see battle */
        if (haven && !at_war(s, nat)) {
            if (mask & ((1 << C_GANDALF) | (1 << C_MERRY) | (1 << C_PIPPIN))) g = 1;
            if ((mask & (1 << C_STRIDER)) && (s->aragorn || nat == NORTH)) g = 1;
            if ((mask & (1 << C_BOROMIR)) && nat == GONDOR) g = 1;
            if ((mask & (1 << C_LEGOLAS)) && nat == ELVES) g = 1;
            if ((mask & (1 << C_GIMLI)) && nat == DWARVES) g = 1;
        }
        if ((mask & (1 << C_STRIDER)) && !s->aragorn && (r == R_MINAS_TIRITH || r == R_DOL_AMROTH || r == R_PELARGIR) && s->ctrl[r] == FP) g = 1;
        if (r == R_FANGORN && s->gandalf_white) g = 1;
        if (s->siege[r] && s->ctrl[r] == FP && !reach[r]) g = 0;   /* cannot walk into a siege */
        goal[r] = (uint8_t)g;
        if (g && reach[r] && r != from) { out[n++] = (uint8_t)r; any_reach = 1; }
    }
    /* approach: the reachable region nearest to a goal that is out of range (Strider's crowning cities first) */
    for (int pass = 0; pass < 2; pass++) {
        int best = -1, bd = 99;
        if (pass == 0 && !((mask & (1 << C_STRIDER)) && !s->aragorn)) continue;
        if (pass == 1 && any_reach) continue;
        for (int r = 0; r < NREG; r++) {
            if (!reach[r] || r == from) continue;
            for (int q = 0; q < NREG; q++) {
                if (!goal[q]) continue;
                if (pass == 0 && q != R_MINAS_TIRITH && q != R_DOL_AMROTH && q != R_PELARGIR) continue;
                if (DIST[r][q] < bd && DIST[r][q] < DIST[org][q]) { bd = DIST[r][q]; best = r; }
            }
        }
        if (best >= 0) { int dup = 0; for (int i = 0; i < n; i++) dup |= out[i] == best; if (!dup) out[n++] = (uint8_t)best; }
    }
    return n;
}

int gen_comp_moves(const State *s, Move *out) {
    int n = 0, seen = 0;
    uint8_t reach[NREG], dst[NREG];
    for (int c = 0; c < NCOMP; c++) {
        int from = s->comp_loc[c];
        if (from >= NREG || ((seen >> c) & 1)) continue;
        int all = comps_at(s, from) & ~s->comp_moved;
        seen |= comps_at(s, from);
        if (!all) continue;
        int groups[NCOMP + 1], ng = 0;
        for (int k = 0; k < NCOMP; k++) if ((all >> k) & 1) groups[ng++] = 1 << k;
        if (ng > 1) groups[ng++] = all;
        for (int g = 0; g < ng; g++) {
            comp_reach(s, from, comp_range(s, groups[g]), 0, reach);
            int nd = comp_dests(s, groups[g], reach, from, dst);
            for (int i = 0; i < nd && n < MAXMOVES - 2; i++) {
                Move m = {M_COMP_MOVE, 0, (uint8_t)groups[g], dst[i], 0, 0, 0, 0};
                out[n++] = m;
            }
        }
    }
    return n;
}

/* Nazgul may go anywhere, but only frontline armies and the Fellowship are worth a visit. */
static int naz_dest_ok(const State *s, int r) {
    if (REGION[r].settle == S_STRONGHOLD && s->ctrl[r] == FP && !s->siege[r]) return 0;
    if (s->mordor < 0 && r == s->fsp_loc) return 1;
    if (!units(s, SH, r)) return 0;
    if (s->siege[r]) return 1;
    for (int i = 0; i < REGION[r].nadj; i++) {
        int v = REGION[r].adj[i];
        if (units(s, FP, v) || (is_settlement(v) && s->ctrl[v] == FP)) return 1;
    }
    return 0;
}
/* How much a Shadow army in r wants leadership: armies that are besieging or facing the enemy. */
static int front_score(const State *s, int r) {
    int u = units(s, SH, r), sc;
    if (!u || !naz_dest_ok(s, r)) return 0;
    sc = u + (s->siege[r] ? 6 : 0);
    for (int i = 0; i < REGION[r].nadj; i++) {
        int v = REGION[r].adj[i], t = REGION[v].settle;
        if (units(s, FP, v)) sc += 2;
        if ((t == S_CITY || t == S_STRONGHOLD) && s->ctrl[v] == FP) sc += 3;
    }
    return sc;
}
static int top_fronts(const State *s, uint8_t *out, int k) {
    int sc[NREG], n = 0;
    for (int r = 0; r < NREG; r++) sc[r] = front_score(s, r);
    for (; n < k; n++) {
        int best = -1;
        for (int r = 0; r < NREG; r++) if (sc[r] > 0 && (best < 0 || sc[r] > sc[best])) best = r;
        if (best < 0) break;
        out[n] = (uint8_t)best; sc[best] = 0;
    }
    return n;
}
/* leadership an army can still use: re-rolls are capped by the dice it rolls */
static int lead_need(const State *s, int r) {
    int u = units(s, SH, r), have = shadow_leadership_units(s, r);
    if (s->minion_loc[M_WK] == r) have += 2;
    if (s->minion_loc[M_MOUTH] == r) have += 2;
    if (s->minion_loc[M_SARUMAN] == r) have += 1;
    u = u > 5 ? 5 : u;
    return u > have ? u - have : 0;
}
/* Nazgul movement as an assignment: the Witch-king, the Mouth (if in range) and as many Nazgul as the main
 * army can use go to T1, then T2; optionally one Nazgul shadows the Fellowship; the rest stay where they are. */
void apply_naz_plan(State *s, Move m) {
    uint8_t old[NREG];
    int N = 0, t1 = m.a, t2 = m.b;
    memcpy(old, s->naz, NREG);
    for (int r = 0; r < NREG; r++) { N += s->naz[r]; s->naz[r] = 0; }
    if (m.c && N) { s->naz[s->fsp_loc]++; N--; if (old[s->fsp_loc]) old[s->fsp_loc]--; }
    if (t1 < NREG) {
        if (s->minion_loc[M_WK] < NREG) s->minion_loc[M_WK] = (uint8_t)t1;
        int mo = s->minion_loc[M_MOUTH];
        if (mo < NREG && DIST[mo][t1] <= 3 && !(s->siege[mo] && s->ctrl[mo] == SH)) s->minion_loc[M_MOUTH] = (uint8_t)t1;
    }
    for (int k = 0; k < 2; k++) {
        int t = k ? t2 : t1;
        if (t >= NREG) continue;
        int need = lead_need(s, t); if (need > N) need = N;
        s->naz[t] += (uint8_t)need; N -= need;
        if (old[t] > need) old[t] -= (uint8_t)need; else old[t] = 0;
    }
    for (int r = 0; r < NREG && N; r++) { int k = old[r] < N ? old[r] : N; s->naz[r] += (uint8_t)k; N -= k; }
    if (N) s->naz[t1 < NREG ? t1 : R_BARAD_DUR] += (uint8_t)N;
}
static int gen_naz_chain(const State *s, Move *out);
int gen_naz_moves(const State *s, Move *out) {
    if (!wotr_prune) return gen_naz_chain(s, out);
    uint8_t fr[4];
    int nf = top_fronts(s, fr, 4), n = 0, f = s->fsp_loc;
    int hunter = s->mordor < 0 && !(REGION[f].settle == S_STRONGHOLD && s->ctrl[f] == FP && !s->siege[f]) && !s->naz[f] && s->minion_loc[M_WK] != f;
    for (int h = 0; h <= hunter; h++) {
        for (int i = 0; i < nf; i++) { Move m = {M_NAZ_PLAN, 0, fr[i], 255, (uint8_t)h, 0, 0, 0}; out[n++] = m; }
        if (nf >= 2) { Move m = {M_NAZ_PLAN, 0, fr[0], fr[1], (uint8_t)h, 0, 0, 0}; out[n++] = m; m.a = fr[1]; m.b = fr[0]; out[n++] = m; }
    }
    if (hunter) { Move m = {M_NAZ_PLAN, 0, 255, 255, 1, 0, 0, 0}; out[n++] = m; }
    /* drop plans that leave everything where it is */
    int k = 0;
    for (int i = 0; i < n; i++) {
        State t = *s; apply_naz_plan(&t, out[i]);
        if (memcmp(t.naz, s->naz, NREG) || memcmp(t.minion_loc, s->minion_loc, NMINION)) out[k++] = out[i];
    }
    return k;
}
static int gen_naz_chain(const State *s, Move *out) {
    int n = 0, nd = 0;
    uint8_t dest[NREG];
    /* the full generator offers every region the rules allow: anywhere but a Stronghold the Free Peoples control
       that no Shadow army besieges (rulebook p.24) */
    for (int r = 0; r < NREG; r++) if (!(REGION[r].settle == S_STRONGHOLD && s->ctrl[r] == FP && !s->siege[r])) dest[nd++] = (uint8_t)r;
    for (int src = 0; src < NREG; src++) {
        int avail = s->naz[src] - s->naz_moved[src];          /* each Nazgul moves once */
        if (avail <= 0) continue;
        for (int i = 0; i < nd && n < MAXMOVES - 8; i++) {
            if (dest[i] == src) continue;
            Move m = {M_NAZ_MOVE, 0, (uint8_t)src, dest[i], 1, 0, 0, 0};
            out[n++] = m;
            if (avail > 1) { m.c = (uint8_t)avail; out[n++] = m; }
        }
    }
    for (int k = 0; k < 2; k++) {
        int mi = k ? M_MOUTH : M_WK, src = s->minion_loc[mi];
        if (src >= NREG || ((s->comp_moved >> k) & 1)) continue;
        if (mi == M_MOUTH && s->siege[src] && s->ctrl[src] == SH) continue;
        uint8_t reach[NREG];        /* the Mouth walks: up to three regions, never through a Stronghold he may not enter */
        memset(reach, 99, sizeof reach); reach[src] = 0;
        if (mi == M_MOUTH) for (int step = 0; step < 3; step++) for (int r = 0; r < NREG; r++) if (reach[r] == step)
            for (int j = 0; j < REGION[r].nadj; j++) {
                int v = REGION[r].adj[j];
                if (reach[v] != 99 || (REGION[v].settle == S_STRONGHOLD && (s->ctrl[v] == FP ? !s->siege[v] : s->siege[v]))) continue;
                reach[v] = (uint8_t)(step + 1);
            }
        for (int i = 0; i < nd && n < MAXMOVES - 8; i++) {
            if (dest[i] == src) continue;
            if (mi == M_MOUTH && reach[dest[i]] > 3) continue;
            Move m = {M_NAZ_MOVE, 0, (uint8_t)src, dest[i], 0, (uint8_t)(k + 1), 0, 0};
            out[n++] = m;
        }
    }
    return n;
}

static int can_diplo(const State *s, int n) {
    if (s->pol[n] == 0) return 0;
    if (n < NFPNAT && !s->active[n]) {
        if (s->pol[n] == 1) return 0;
        if (s->card_where[CARD_THREATS_AND_PROMISES] == W_TABLE) return 0;
    }
    return 1;
}
static int can_comp_pol(const State *s, int n) { return s->pol[n] > 0 && (s->active[n] || s->pol[n] > 1); }

static int gen_cards(const State *s, int side, Move *out, int n) {
    int pe = s->dice[side][F_EVENT] ? F_EVENT : -1;
    for (int c = side * 48; c < side * 48 + 48; c++) {
        if (s->card_where[c] != W_HAND || !card_playable(s, c)) continue;
        int p = payer(s, side, CARD[c].type);
        Move m = {M_PLAY, 0, (uint8_t)c, 0, 0, 0, 0, 0};
        int n0 = n;
        if (p >= 0) { m.die = (uint8_t)p; out[n++] = m; }
        if (pe >= 0 && pe != p) { m.die = (uint8_t)pe; out[n++] = m; }
        else if (p < 0 && pe < 0 && side == FP && s->dice[FP][F_SPECIAL]) { m.die = F_SPECIAL; out[n++] = m; }
        if (s->dkind == D_ACTION && !s->rng_max) {          /* no Event card without an effect (it can depend on the die spent) */
            int k = n0;
            for (int i = n0; i < n; i++) if (card_has_effect(s, out[i])) out[k++] = out[i];
            /* the cheapest die may be the very die the card needs (Mirror of Galadriel played with the only Character
               die): then any other die that can pay is tried */
            for (int f = 0; f < NFACE && k == n0; f++) {
                int t = CARD[c].type, pays = f == t || (f == F_MA && (t == F_ARMY || t == F_MUSTER)) || (f == F_SPECIAL && side == FP) || f == F_EVENT;
                if (!pays || !s->dice[side][f]) continue;
                m.die = (uint8_t)f;
                if (card_has_effect(s, m)) out[k++] = m;
            }
            n = k;
        }
    }
    return n;
}

int gen_action(const State *s, Move *out) {
    int side = s->dplayer, n = 0;
    const uint8_t *d = s->dice[side];
    int mine = 0, theirs = 0;
    for (int f = 0; f < NFACE; f++) { mine += d[f]; theirs += s->dice[1 - side][f]; }
    Move z; memset(&z, 0, sizeof z);
    Move m;
    if (mine < theirs) { m = z; m.type = M_PASS; out[n++] = m; }
    int pc = payer(s, side, F_CHAR), pa = payer(s, side, F_ARMY), pm = payer(s, side, F_MUSTER), pe = payer(s, side, F_EVENT);
    int pany = payer(s, side, ANYDIE);
    if (wotr_prune) { if (pany >= 0) { m = z; m.type = M_SKIP; m.die = (uint8_t)pany; out[n++] = m; } }
    else for (int f = 0; f < NFACE; f++) if (d[f]) { m = z; m.type = M_SKIP; m.die = (uint8_t)f; out[n++] = m; }
    /* elven rings */
    if (s->rings[side] && !s->ring_used[side]) {
        static const uint8_t to[] = {F_CHAR, F_MA, F_EVENT, F_SPECIAL};
        for (int f = 0; f < NFACE; f++) {
            if (!d[f] || f == F_SPECIAL) continue;
            if (wotr_prune && f != pany) continue;                 /* spend the least useful die... */
            for (int t = 0; t < 4; t++) {
                if (to[t] == f || (to[t] == F_SPECIAL && side == FP)) continue;
                if (wotr_prune) {                                  /* ...and only to gain an action that no die offers */
                    if (to[t] == F_SPECIAL) continue;
                    if (to[t] == F_CHAR && pc >= 0) continue;
                    if (to[t] == F_EVENT && pe >= 0) continue;
                    if (to[t] == F_MA && pa >= 0 && pm >= 0) continue;
                }
                m = z; m.type = M_RING; m.a = (uint8_t)f; m.b = to[t]; out[n++] = m;
            }
        }
    }
    /* armies */
    if (pa >= 0) {
        m = z; m.type = M_ARMY; m.die = (uint8_t)pa; m.f = 1;
        n += gen_army_moves(s, side, 0, m, out + n, MAXMOVES - 64 - n);
        m = z; m.type = M_ATTACK; m.die = (uint8_t)pa;
        n += gen_attacks(s, side, 0, m, out + n);
    }
    if (pc >= 0 && pc != pa) {
        m = z; m.type = M_ARMY; m.die = (uint8_t)pc;
        n += gen_army_moves(s, side, 1, m, out + n, MAXMOVES - 64 - n);
        m = z; m.type = M_ATTACK; m.die = (uint8_t)pc;
        n += gen_attacks(s, side, 1, m, out + n);
    }
    /* muster */
    if (pm >= 0) {
        for (int k = 0; k < NFPNAT + 0; k++) {
            int nat = side == FP ? k : NFPNAT + k;
            if (nat >= NNAT || NAT_SIDE(nat) != side) continue;
            if (can_diplo(s, nat)) { m = z; m.type = M_DIPLO; m.die = (uint8_t)pm; m.a = (uint8_t)nat; out[n++] = m; }
        }
        m = z; m.type = M_MUSTER; m.die = (uint8_t)pm;
        int k = gen_muster(s, side, m, 255, out + n);
        n += k;
        for (int r = 0; r < NREG; r++) {
            int nat = REGION[r].nation;
            if (!is_settlement(r) || nat == NO_NATION || NAT_SIDE(nat) != side || !at_war(s, nat)) continue;
            if (s->ctrl[r] != side || s->siege[r] || units(s, 1 - side, r) || !s->pool_eli[nat]) continue;
            m = z; m.type = M_MUSTER_ELITE; m.die = (uint8_t)pm; m.a = (uint8_t)r; out[n++] = m;
        }
    }
    /* event die: draw */
    if (pe >= 0) for (int k = 0; k < 2; k++) {
        int any = 0;
        for (int c = (side * 2 + k) * 24; c < (side * 2 + k) * 24 + 24; c++) if (s->card_where[c] == W_DECK) { any = 1; break; }
        if (any) { m = z; m.type = M_DRAW; m.die = (uint8_t)pe; m.a = (uint8_t)k; out[n++] = m; }
    }
    n = gen_cards(s, side, out, n);

    if (side == FP) {
        int fc = fsp_comps(s);
        if (pc >= 0) {
            if (!s->revealed && (s->mordor >= 0 || s->progress < 12)) { m = z; m.type = M_FSP_MOVE; m.die = (uint8_t)pc; out[n++] = m; }
            if (fc && s->mordor < 0) {
                if (!wotr_prune) {      /* any group of the Companions may separate (RB p. 34): every one, when following a player */
                    for (unsigned g = 1; g < 256 && n < MAXMOVES - 64; g++) if (!(g & ~(unsigned)fc)) { m = z; m.type = M_SEPARATE; m.die = (uint8_t)pc; m.a = (uint8_t)g; out[n++] = m; }
                } else {
                    for (int a = 0; a < NCOMP; a++) {
                        if (!((fc >> a) & 1)) continue;
                        m = z; m.type = M_SEPARATE; m.die = (uint8_t)pc; m.a = (uint8_t)(1 << a); out[n++] = m;
                    }
                    if (__builtin_popcount((unsigned)fc) > 2) { m = z; m.type = M_SEPARATE; m.die = (uint8_t)pc; m.a = (uint8_t)fc; out[n++] = m; }
                }
            }
            for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] < NREG) { m = z; m.type = M_COMPS; m.die = (uint8_t)pc; out[n++] = m; break; }
        }
        if (s->revealed) {
            int p = s->guide == C_STRIDER ? pany : pc;
            if (p >= 0) { m = z; m.type = M_FSP_HIDE; m.die = (uint8_t)p; out[n++] = m; }
        }
        if (d[F_SPECIAL]) {
            int g = s->comp_loc[C_GANDALF];
            if (!s->gandalf_white && g != LOC_FSP && s->minion_seen) {
                m = z; m.type = M_GANDALF_WHITE; m.die = F_SPECIAL;
                if (g < NREG) { m.a = (uint8_t)g; out[n++] = m; }
                else {
                    static const uint8_t el[] = {R_FANGORN, R_LORIEN, R_RIVENDELL, R_GREY_HAVENS, R_WOODLAND_REALM};
                    for (int i = 0; i < 5; i++) if (i == 0 || s->ctrl[el[i]] == FP) { m.a = el[i]; out[n++] = m; }
                }
            }
            int st = s->comp_loc[C_STRIDER];
            if (!s->aragorn && (st == R_MINAS_TIRITH || st == R_DOL_AMROTH || st == R_PELARGIR) && s->ctrl[st] == FP) {
                m = z; m.type = M_ARAGORN; m.die = F_SPECIAL; out[n++] = m;
            }
            if (s->card_where[CARD_DENETHORS_FOLLY] == W_TABLE) { m = z; m.type = M_DISCARD_TABLE; m.die = F_SPECIAL; m.a = CARD_DENETHORS_FOLLY; out[n++] = m; }
            if (s->card_where[CARD_THE_PALANTIR_OF_ORTHANC] == W_TABLE) { m = z; m.type = M_DISCARD_TABLE; m.die = F_SPECIAL; m.a = CARD_THE_PALANTIR_OF_ORTHANC; out[n++] = m; }
        }
        if (pany >= 0 && pany != F_SPECIAL) {
            if (s->card_where[CARD_DENETHORS_FOLLY] == W_TABLE &&
                (s->comp_loc[C_GANDALF] == R_MINAS_TIRITH || (s->aragorn && s->comp_loc[C_STRIDER] == R_MINAS_TIRITH))) {
                m = z; m.type = M_DISCARD_TABLE; m.die = (uint8_t)pany; m.a = CARD_DENETHORS_FOLLY; out[n++] = m;
            }
            if (s->card_where[CARD_THE_PALANTIR_OF_ORTHANC] == W_TABLE && s->rings[FP] && !s->ring_used[FP]) {
                m = z; m.type = M_DISCARD_TABLE; m.die = (uint8_t)pany; m.a = CARD_THE_PALANTIR_OF_ORTHANC; m.b = 1; out[n++] = m;
            }
        }
        if (pany >= 0) {
            int b = s->comp_loc[C_BOROMIR], l = s->comp_loc[C_LEGOLAS];
            m = z; m.type = M_COMP_POLITICS; m.die = (uint8_t)pany;
            if (b < NREG && REGION[b].nation == GONDOR && (REGION[b].settle == S_CITY || REGION[b].settle == S_STRONGHOLD) && s->ctrl[b] == FP && can_comp_pol(s, GONDOR)) { m.a = GONDOR; out[n++] = m; }
            if (l < NREG && REGION[l].nation == ELVES && REGION[l].settle == S_STRONGHOLD && s->ctrl[l] == FP && can_comp_pol(s, ELVES)) { m.a = ELVES; out[n++] = m; }
            if (s->comp_loc[C_GIMLI] == R_EREBOR && s->ctrl[R_EREBOR] == FP && can_comp_pol(s, DWARVES)) { m.a = DWARVES; out[n++] = m; }
        }
    } else {
        if (pc >= 0) {
            int any = s->minion_loc[M_WK] < NREG || s->minion_loc[M_MOUTH] < NREG;
            for (int r = 0; r < NREG && !any; r++) any = s->naz[r] > 0;
            if (any) { m = z; m.type = M_NAZGUL; m.die = (uint8_t)pc; out[n++] = m; }
        }
        if (pm >= 0) {
            m = z; m.type = M_MINION; m.die = (uint8_t)pm;
            if (s->minion_loc[M_SARUMAN] == LOC_NONE && at_war(s, ISENGARD) && s->ctrl[R_ORTHANC] == SH) { m.a = M_SARUMAN; m.b = R_ORTHANC; out[n++] = m; }
            if (s->minion_loc[M_WK] == LOC_NONE && at_war(s, SAURON)) {
                int fpw = 0;
                for (int k = 0; k < NFPNAT; k++) fpw |= at_war(s, k);
                if (fpw && wotr_prune) {                           /* the Black Captain joins a front-line army */
                    uint8_t fr[NREG]; int nf = top_fronts(s, fr, NREG), k = 0;
                    for (int i = 0; i < nf && k < 4; i++) if (s->reg[fr[i]][SAURON] + s->eli[fr[i]][SAURON]) { m.a = M_WK; m.b = fr[i]; out[n++] = m; k++; }
                    if (!k) for (int r = 0; r < NREG && !k; r++) if (s->reg[r][SAURON] + s->eli[r][SAURON]) { m.a = M_WK; m.b = (uint8_t)r; out[n++] = m; k++; }
                } else if (fpw) for (int r = 0; r < NREG; r++) if (s->reg[r][SAURON] + s->eli[r][SAURON]) { m.a = M_WK; m.b = (uint8_t)r; out[n++] = m; }
            }
            if (s->minion_loc[M_MOUTH] == LOC_NONE && (s->mordor >= 0 || all_at_war(s, FP))) {
                for (int r = 0; r < NREG; r++)
                    if (REGION[r].nation == SAURON && REGION[r].settle == S_STRONGHOLD && s->ctrl[r] == SH) { m.a = M_MOUTH; m.b = (uint8_t)r; out[n++] = m; }
            }
            if (s->minion_loc[M_SARUMAN] < NREG && s->ctrl[R_ORTHANC] == SH && !s->siege[R_ORTHANC]) {
                m = z; m.type = M_VOICE; m.die = (uint8_t)pm;
                {   /* recruiting needs a settlement that can take a unit; with none it is the same as discarding the die */
                    static const uint8_t st[3] = {R_ORTHANC, R_NORTH_DUNLAND, R_SOUTH_DUNLAND};
                    int room = 0;
                    for (int i = 0; i < 3; i++) room |= s->ctrl[st[i]] == SH && !units(s, FP, st[i]) && units(s, SH, st[i]) < 10;
                    if (s->pool_reg[ISENGARD] && room) { m.a = 0; out[n++] = m; }
                }
                if (s->reg[R_ORTHANC][ISENGARD] && s->pool_eli[ISENGARD]) { m.a = 1; out[n++] = m; }
            }
        }
        if (pany >= 0) {
            int hs = 0, hc = 0;
            for (int c = 48; c < 72; c++) hc |= s->card_where[c] == W_HAND;
            for (int c = 72; c < 96; c++) hs |= s->card_where[c] == W_HAND && CARD[c].type == F_ARMY;   /* the card asks for a Strategy (Army) card: a Muster one does not pay */
            if (hs && hc) {
                m = z; m.type = M_DISCARD_TABLE; m.die = (uint8_t)pany;
                if (s->card_where[CARD_A_POWER_TOO_GREAT] == W_TABLE) { m.a = CARD_A_POWER_TOO_GREAT; out[n++] = m; }
                if (s->card_where[CARD_THE_POWER_OF_TOM_BOMBADIL] == W_TABLE) { m.a = CARD_THE_POWER_OF_TOM_BOMBADIL; out[n++] = m; }
            }
        }
    }
    return n;
}
