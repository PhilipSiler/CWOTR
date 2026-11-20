/* bridge.c -- connects the engine to the WotR Bot Protocol (wotr-protocol/SPEC.md, draft 0.2, base game).
 *
 * Parts, in order: a small JSON reader; the name tables (protocol names <-> engine numbers); their tests; a local
 * referee: the engine runs the game and speaks the protocol, as a referee, to one outside bot on stdin/stdout, while
 * our own player decides for the other side directly.
 *   bridge names <vocabulary.json>   check that every base-game protocol name maps to exactly one of ours and back,
 *                                    and that the two maps (borders, nations, settlements) and card lists agree.
 *   bridge referee <player|prior> <fp|sh> <seed> [clock ms] [sims]
 *                                    one game; the outside bot plays the side named; the log goes to stderr.
 *   bridge mock <player|prior> <games> <seed>
 *                                    the same game loop with the outside bot replaced by random answers (a test).
 * Build: make -C proto   (does not touch src/ or bin/).
 */
#include "wotr.h"
#include "agent.h"
#include "features.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================= JSON reader ================= */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct J { int type; double num; const char *str; const char *key; struct J *child, *next; } J;
typedef struct { const char *p; char *mem; size_t used, cap; int err; } JP;

static void *j_alloc(JP *jp, size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (jp->used + n > jp->cap) { jp->err = 1; return NULL; }
    void *r = jp->mem + jp->used; jp->used += n; memset(r, 0, n); return r;
}
static void j_ws(JP *jp) { while (*jp->p == ' ' || *jp->p == '\n' || *jp->p == '\t' || *jp->p == '\r') jp->p++; }
static int j_hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v * 16 + d;
    }
    return v;
}
static const char *j_string(JP *jp) {           /* at the opening quote; returns the decoded text (UTF-8) */
    const char *q = jp->p + 1; size_t len = 0;
    while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; len++; }
    if (*q != '"') { jp->err = 1; return NULL; }
    char *out = j_alloc(jp, len * 3 + 1), *o = out;   /* a \uXXXX escape (6 bytes in) gives at most 3 bytes out */
    if (!out) return NULL;
    for (q = jp->p + 1; *q != '"'; q++) {
        if (*q != '\\') { *o++ = *q; continue; }
        q++;
        switch (*q) {
        case 'n': *o++ = '\n'; break; case 't': *o++ = '\t'; break; case 'r': *o++ = '\r'; break;
        case 'b': *o++ = '\b'; break; case 'f': *o++ = '\f'; break;
        case 'u': {
            int cp = j_hex4(q + 1);
            if (cp < 0) { jp->err = 1; return NULL; }
            q += 4;
            if (cp < 0x80) *o++ = (char)cp;
            else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
            else { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
            break; }
        default: *o++ = *q;
        }
    }
    *o = 0; jp->p = q + 1;
    return out;
}
static J *j_value(JP *jp, int depth) {
    J *v = j_alloc(jp, sizeof(J));
    if (!v || depth > 64) { jp->err = 1; return NULL; }
    j_ws(jp);
    char c = *jp->p;
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']'; J **tail = &v->child;
        v->type = c == '{' ? J_OBJ : J_ARR; jp->p++; j_ws(jp);
        if (*jp->p == close) { jp->p++; return v; }
        for (;;) {
            const char *key = NULL;
            j_ws(jp);
            if (v->type == J_OBJ) {
                if (*jp->p != '"') { jp->err = 1; return NULL; }
                key = j_string(jp); j_ws(jp);
                if (jp->err || *jp->p != ':') { jp->err = 1; return NULL; }
                jp->p++;
            }
            J *e = j_value(jp, depth + 1);
            if (jp->err) return NULL;
            e->key = key; *tail = e; tail = &e->next; j_ws(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == close) { jp->p++; return v; }
            jp->err = 1; return NULL;
        }
    }
    if (c == '"') { v->type = J_STR; v->str = j_string(jp); return v; }
    if (!strncmp(jp->p, "true", 4)) { v->type = J_BOOL; v->num = 1; jp->p += 4; return v; }
    if (!strncmp(jp->p, "false", 5)) { v->type = J_BOOL; jp->p += 5; return v; }
    if (!strncmp(jp->p, "null", 4)) { v->type = J_NULL; jp->p += 4; return v; }
    char *end; v->type = J_NUM; v->num = strtod(jp->p, &end);
    if (end == jp->p) { jp->err = 1; return NULL; }
    jp->p = end;
    return v;
}
/* Parses text into nodes held in one block (free it with free(*mem)). NULL if the text is not JSON. */
static J *j_parse(const char *text, char **mem) {
    JP jp = { text, NULL, 0, strlen(text) * 6 + 4096, 0 };
    jp.mem = malloc(jp.cap); *mem = jp.mem;
    if (!jp.mem) return NULL;
    J *v = j_value(&jp, 0);
    if (!jp.err) { j_ws(&jp); if (*jp.p) jp.err = 1; }
    if (jp.err) { free(jp.mem); *mem = NULL; return NULL; }
    return v;
}
static const J *j_get(const J *o, const char *key) {
    if (!o || o->type != J_OBJ) return NULL;
    for (const J *e = o->child; e; e = e->next) if (!strcmp(e->key, key)) return e;
    return NULL;
}
static const char *j_str(const J *o, const char *key) { const J *e = j_get(o, key); return e && e->type == J_STR ? e->str : NULL; }
static int j_len(const J *a) { int n = 0; if (a) for (const J *e = a->child; e; e = e->next) n++; return n; }

/* ================= name tables ================= */
/* A name as the protocol writes its keys: lower case, accents dropped, apostrophes dropped, anything else a hyphen. */
static void slug(const char *name, char *out, int n) {
    int o = 0, gap = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && o < n - 2; p++) {
        int c = *p;
        if (c == 0xC3 && p[1]) {                    /* accented Latin letter: keep its base letter */
            int x = *++p | 0x20;                    /* fold upper case onto lower (0x80-0x9E -> 0xA0-0xBE) */
            c = x <= 0xA5 ? 'a' : x == 0xA7 ? 'c' : x >= 0xA8 && x <= 0xAB ? 'e' : x >= 0xAC && x <= 0xAF ? 'i'
              : x == 0xB1 ? 'n' : x >= 0xB2 && x <= 0xB6 ? 'o' : x >= 0xB9 && x <= 0xBC ? 'u' : '-';
        }
        if (c == 0xE2 && p[1] == 0x80 && p[2] == 0x99) { p += 2; continue; }   /* typographic apostrophe */
        if (c == '\'') continue;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { if (gap && o) out[o++] = '-'; gap = 0; out[o++] = (char)c; }
        else gap = 1;
    }
    out[o] = 0;
}

static const char *const SIDE_KEY[2] = { "FREE_PEOPLES", "SHADOW" };
static const char *const NATION_KEY[NNAT] = { "DWARVES", "ELVES", "GONDOR", "NORTH", "ROHAN", "ISENGARD", "SAURON", "SOUTHRONS_EASTERLINGS" };
static const char *const SETTLE_KEY[5] = { NULL, "TOWN", "CITY", "STRONGHOLD", NULL };   /* the protocol does not list fortifications */
static const char *const FACE_KEY[NFACE] = { "CHARACTER", "ARMY", "MUSTER", "EVENT", "ARMY_MUSTER", NULL };   /* F_SPECIAL: by side */
static const char *const SPECIAL_FACE_KEY[2] = { "WILL_OF_THE_WEST", "EYE" };
static const char *const DECK_KEY[2] = { "character", "strategy" };
/* characters: the engine keeps companions, Gollum (as guide), the two upgrades and the minions in separate fields */
enum { CH_COMP0 = 0, CH_GOLLUM = 7, CH_GANDALF_WHITE, CH_ARAGORN, CH_MINION0, CH_RING_BEARERS = CH_MINION0 + NMINION, NCH };
static const char *const CHAR_KEY[NCH] = { "gandalf-the-grey", "strider", "boromir", "legolas", "gimli", "meriadoc", "peregrin",
    "gollum", "gandalf-the-white", "aragorn", "witch-king", "saruman", "mouth-of-sauron", "ring-bearers" };
/* Hunt tiles: the protocol names each tile, the engine counts tiles of each kind */
static const struct { const char *prefix; int kind, count; } TILE_KEY[NTILE] = {
    { "eye-", T_EYE, 4 }, { "3-", T_3, 3 }, { "2-", T_2, 2 }, { "2r", T_2R, 1 }, { "1-", T_1, 2 }, { "1r-", T_1R, 2 }, { "0r-", T_0R, 2 },
    { "elven-cloaks", T_CLOAKS, 1 }, { "elven-rope", T_ROPE, 1 }, { "phial-of-galadriel", T_PHIAL, 1 },
    { "smeagol-helps-nice-master", T_SMEAGOL, 1 }, { "shelobs-lair", T_SHELOB, 1 }, { "the-ring-is-mine", T_RING_MINE, 1 },
    { "on-on-they-went", T_ON_ON, 1 }, { "give-it-to-uss", T_GIVE_USS, 1 } };
/* the 26 questions a base game asks (SPEC section 5) */
enum { K_FELLOWSHIP_PHASE, K_HUNT_ALLOCATION, K_ACTION, K_ARMY_MOVE, K_ARMY_SELECTION, K_CHOOSE_ATTACK, K_FIELD_OR_SIEGE,
       K_COMBAT_CARD, K_CASUALTY, K_REMOVE_EXCESS_UNIT, K_CEASE_ATTACK, K_RETREAT, K_EXTEND_SIEGE, K_ADVANCE, K_RECRUIT,
       K_MOVE_COMPANIONS, K_JOIN_GROUP, K_SEPARATE_COMPANIONS, K_MOVE_MINIONS, K_CHOOSE_GUIDE, K_GUIDE_ABILITY, K_HUNT_CASUALTY,
       K_REVEAL_FELLOWSHIP, K_WHITE_RIDER, K_DISCARD_EXCESS_CARD, K_CARD_CHOICE, NKIND };
static const char *const KIND_KEY[NKIND] = { "FELLOWSHIP_PHASE", "HUNT_ALLOCATION", "ACTION", "ARMY_MOVE", "ARMY_SELECTION",
    "CHOOSE_ATTACK", "FIELD_OR_SIEGE", "COMBAT_CARD", "CASUALTY", "REMOVE_EXCESS_UNIT", "CEASE_ATTACK", "RETREAT", "EXTEND_SIEGE",
    "ADVANCE", "RECRUIT", "MOVE_COMPANIONS", "JOIN_GROUP", "SEPARATE_COMPANIONS", "MOVE_MINIONS", "CHOOSE_GUIDE", "GUIDE_ABILITY",
    "HUNT_CASUALTY", "REVEAL_FELLOWSHIP", "WHITE_RIDER", "DISCARD_EXCESS_CARD", "CARD_CHOICE" };
/* what a die can be used for (use_die's "action") */
enum { A_DRAW_CARD, A_MOVE_ARMIES, A_MOVE_ARMY_WITH_LEADER, A_ATTACK, A_ATTACK_WITH_LEADER, A_RECRUIT, A_DIPLOMACY,
       A_MOVE_COMPANIONS, A_SEPARATE_COMPANIONS, A_MOVE_MINIONS, A_MOVE_FELLOWSHIP, A_HIDE_FELLOWSHIP, A_HIDE_FELLOWSHIP_STRIDER,
       A_PLAY_COMPANION, A_PLAY_MINION, A_COMPANION_POLITICS, A_VOICE_OF_SARUMAN, A_PLAY_CARD, A_FORCE_DISCARD, NACTION };
static const char *const ACTION_KEY[NACTION] = { "draw_card", "move_armies", "move_army_with_leader", "attack", "attack_with_leader",
    "recruit", "diplomacy", "move_companions", "separate_companions", "move_minions", "move_fellowship", "hide_fellowship",
    "hide_fellowship_strider", "play_companion", "play_minion", "companion_politics", "voice_of_saruman", "play_card", "force_discard" };

static char REGION_KEY[NREG][32];
static char CARD_KEY[NCARD][16];

/* The protocol numbers three Shadow character cards differently from the engine's list:
 * ours 9 The Nazgul Strike!, 10 Nazgul Search, 11 Cruel Weather; theirs 09 Nazgul Search, 10 Cruel Weather, 11 The Nazgul Strike!. */
static int card_number(int c) {                 /* the protocol's number (1..24) of engine card c */
    int n = c % 24 + 1;
    if (CARD_DECK(c) == 2) { if (n == 9) return 11; if (n == 10) return 9; if (n == 11) return 10; }
    return n;
}
static void names_init(void) {
    static const char *const deck[4] = { "fp-char", "fp-strat", "sh-char", "sh-strat" };
    for (int r = 0; r < NREG; r++) {
        slug(REGION[r].name, REGION_KEY[r], sizeof REGION_KEY[r]);
        char *h = strstr(REGION_KEY[r], "rhovanion");       /* the protocol spells it "rovanion" */
        if (h) memmove(h + 1, h + 2, strlen(h + 2) + 1);
    }
    for (int c = 0; c < NCARD; c++) snprintf(CARD_KEY[c], sizeof CARD_KEY[c], "%s-%02d", deck[CARD_DECK(c)], card_number(c));
}
static int find_key(const char *key, const char *const *table, int n) {
    if (key) for (int i = 0; i < n; i++) if (table[i] && !strcmp(key, table[i])) return i;
    return -1;
}
static int region_of(const char *key) { if (key) for (int r = 0; r < NREG; r++) if (!strcmp(key, REGION_KEY[r])) return r; return -1; }
static int card_of(const char *key)   { if (key) for (int c = 0; c < NCARD; c++) if (!strcmp(key, CARD_KEY[c])) return c; return -1; }
static int nation_of(const char *key) { return find_key(key, NATION_KEY, NNAT); }
static int side_of(const char *key)   { return find_key(key, SIDE_KEY, 2); }
static int char_of(const char *key)   { return find_key(key, CHAR_KEY, NCH); }
static int kind_of(const char *key)   { return find_key(key, KIND_KEY, NKIND); }
static int action_of(const char *key) { return find_key(key, ACTION_KEY, NACTION); }
static int deck_of(const char *key)   { return find_key(key, DECK_KEY, 2); }
static int face_of(const char *key) {           /* engine face; Will of the West and Eye are both F_SPECIAL */
    int f = find_key(key, FACE_KEY, NFACE);
    return f >= 0 ? f : find_key(key, SPECIAL_FACE_KEY, 2) >= 0 ? F_SPECIAL : -1;
}
static const char *face_key(int side, int face) { return face == F_SPECIAL ? SPECIAL_FACE_KEY[side] : FACE_KEY[face]; }
static int tile_of(const char *key) {           /* engine tile kind of a protocol tile key */
    if (!key) return -1;
    for (int i = 0; i < NTILE; i++) {
        size_t n = strlen(TILE_KEY[i].prefix);
        if (strncmp(key, TILE_KEY[i].prefix, n)) continue;
        if (TILE_KEY[i].kind >= NSTD_TILE || TILE_KEY[i].kind == T_2R) { if (!key[n]) return TILE_KEY[i].kind; continue; }
        if (key[n] >= '1' && key[n] < '1' + TILE_KEY[i].count && !key[n + 1]) return TILE_KEY[i].kind;   /* numbered copy */
    }
    return -1;
}

/* ================= tests ================= */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    if (buf) buf[n] = 0;
    fclose(f);
    return buf;
}
static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int base_game(const J *e) { return !j_get(e, "expansion") && !j_get(e, "variant"); }

/* Every base-game name of the vocabulary maps to exactly one of ours and back; maps and card lists agree. */
static int test_names(const char *vocab_path) {
    char *text = read_file(vocab_path), *mem;
    if (!text) { printf("cannot read %s\n", vocab_path); return 1; }
    const J *v = j_parse(text, &mem);
    if (!v) { printf("%s is not JSON\n", vocab_path); return 1; }
    int n, seen[256];

    /* regions: each key exactly once, same nation, settlement and borders */
    memset(seen, 0, sizeof seen); n = 0;
    for (const J *e = j_get(v, "regions")->child; e; e = e->next, n++) {
        const char *key = j_str(e, "key"); int r = region_of(key);
        CHECK(r >= 0, "region %s: no engine region", key);
        if (r < 0) continue;
        CHECK(!seen[r]++, "region %s: maps to an engine region already used", key);
        for (int q = 0; q < NREG; q++) CHECK(q == r || strcmp(REGION_KEY[q], key), "region %s: two engine regions have this key", key);
        const char *nat = j_str(e, "nation"), *set = j_str(e, "settlement");
        CHECK(nation_of(nat) == (REGION[r].nation == NO_NATION ? -1 : REGION[r].nation), "region %s: nation %s, ours %d", key, nat ? nat : "none", REGION[r].nation);
        const char *ours = SETTLE_KEY[REGION[r].settle];
        CHECK((!set && !ours) || (set && ours && !strcmp(set, ours)), "region %s: settlement %s, ours %s", key, set ? set : "none", ours ? ours : "none");
        const J *nb = j_get(e, "neighbours");
        CHECK(j_len(nb) == REGION[r].nadj, "region %s: %d neighbours, ours %d", key, j_len(nb), REGION[r].nadj);
        for (const J *x = nb ? nb->child : NULL; x; x = x->next) {
            int q = region_of(x->str), found = 0;
            for (int i = 0; i < REGION[r].nadj; i++) found |= REGION[r].adj[i] == q;
            CHECK(found, "region %s: border with %s is not in the engine's map", key, x->str);
        }
    }
    CHECK(n == NREG, "%d regions in the vocabulary, %d in the engine", n, NREG);
    for (int r = 0; r < NREG; r++) CHECK(seen[r] == 1, "engine region %s (%s) is not in the vocabulary", REGION[r].name, REGION_KEY[r]);

    /* cards: each base key exactly once, same side, deck and title (three known spelling differences aside) */
    static const char *const spelling[][2] = { { "fear-fear-foes", "fear-fire-foes" }, { "swords-in-eriador", "swords-of-eriador" },
                                               { "hordes-from-the-east", "horde-from-the-east" } };
    memset(seen, 0, sizeof seen); n = 0;
    for (const J *e = j_get(v, "cards")->child; e; e = e->next) {
        if (!base_game(e)) continue;
        const char *key = j_str(e, "key"); int c = card_of(key); n++;
        CHECK(c >= 0, "card %s: no engine card", key);
        if (c < 0) continue;
        CHECK(!seen[c]++, "card %s: maps to an engine card already used", key);
        CHECK(side_of(j_str(e, "side")) == CARD_SIDE(c), "card %s: side", key);
        CHECK(!strcmp(j_str(e, "deck"), CARD_IS_CHAR(c) ? "CHARACTER" : "STRATEGY"), "card %s: deck", key);
        char a[80], b[80]; slug(CARD[c].name, a, sizeof a); slug(j_str(e, "title"), b, sizeof b);
        for (int i = 0; i < 3; i++) if (!strcmp(a, spelling[i][0]) && !strcmp(b, spelling[i][1])) strcpy(a, b);
        CHECK(!strcmp(a, b), "card %s: title \"%s\", ours \"%s\"", key, j_str(e, "title"), CARD[c].name);
    }
    CHECK(n == NCARD, "%d base cards in the vocabulary, %d in the engine", n, NCARD);
    for (int c = 0; c < NCARD; c++) CHECK(seen[c] == 1, "engine card %s (%s) is not in the vocabulary", CARD[c].name, CARD_KEY[c]);

    /* plain lists: sides, nations, characters, decision kinds, die actions */
    #define LIST(field, is_obj, lookup, count, what) do { \
        memset(seen, 0, sizeof seen); n = 0; \
        for (const J *e = j_get(v, field)->child; e; e = e->next) { \
            if (is_obj && !base_game(e)) continue; \
            const char *key = is_obj ? j_str(e, "key") : e->str; int i = lookup(key); n++; \
            CHECK(i >= 0, what " %s: no engine counterpart", key); \
            if (i >= 0) CHECK(!seen[i]++, what " %s: listed twice", key); \
        } \
        CHECK(n == count, "%d " what "s in the vocabulary, %d here", n, count); } while (0)
    LIST("sides", 0, side_of, 2, "side");
    LIST("nations", 1, nation_of, NNAT, "nation");
    LIST("characters", 1, char_of, NCH, "character");
    LIST("decision_kinds", 1, kind_of, NKIND, "decision kind");
    LIST("die_actions", 1, action_of, NACTION, "die action");
    for (const J *e = j_get(v, "nations")->child; e; e = e->next)
        CHECK(side_of(j_str(e, "side")) == NAT_SIDE(nation_of(j_str(e, "key"))), "nation %s: side", j_str(e, "key"));

    /* die faces: the seven base faces (the last two in the file are the expansion's) */
    { int count[NFACE] = {0}; n = 0;
      for (const J *e = j_get(v, "die_faces")->child; e; e = e->next) {
          if (!strcmp(e->str, "CARD_DRAW") || !strcmp(e->str, "USE_BALROG")) continue;
          int f = face_of(e->str); n++;
          CHECK(f >= 0, "die face %s: no engine face", e->str);
          if (f >= 0) count[f]++;
      }
      for (int f = 0; f < NFACE; f++) CHECK(count[f] == (f == F_SPECIAL ? 2 : 1), "engine die face %d: named %d times", f, count[f]);
      for (int side = 0; side < 2; side++) for (int f = 0; f < NFACE; f++) CHECK(face_of(face_key(side, f)) == f, "die face %d does not map back", f);
    }

    /* Hunt tiles: every base tile has a kind, and the numbers of each kind are those of the engine at the start */
    { int count[NTILE] = {0}; State s; wotr_init(&s, 1);
      for (const J *e = j_get(v, "hunt_tiles")->child; e; e = e->next) {
          if (!strncmp(e->str, "smeagol-", 8) && strcmp(e->str, "smeagol-helps-nice-master")) continue;   /* the expansion's tiles */
          int t = tile_of(e->str);
          CHECK(t >= 0, "Hunt tile %s: no engine tile", e->str);
          if (t >= 0) count[t]++;
      }
      for (int i = 0; i < NTILE; i++) {
          int t = TILE_KEY[i].kind;
          CHECK(count[t] == TILE_KEY[i].count, "Hunt tile kind %s: %d in the vocabulary, %d expected", TILE_KEY[i].prefix, count[t], TILE_KEY[i].count);
          if (t < NSTD_TILE) CHECK(s.tile_pool[t] == count[t], "Hunt tile kind %s: %d in the engine's starting pool, %d in the vocabulary", TILE_KEY[i].prefix, s.tile_pool[t], count[t]);
      }
    }

    /* units and decks */
    { static const char *const unit[4] = { "regular", "elite", "leader", "nazgul" }; n = 0;
      for (const J *e = j_get(v, "units")->child; e; e = e->next, n++) CHECK(find_key(e->str, unit, 4) >= 0, "unit %s unknown", e->str);
      CHECK(n == 4, "%d unit kinds", n);
      CHECK(deck_of("character") == 0 && deck_of("strategy") == 1, "decks"); }

    free(mem); free(text);
    printf("names: %s\n", fails ? "FAILED" : "ok (105 regions, 96 cards, 8 nations, 14 characters, 7 die faces, 24 Hunt tiles, 26 decision kinds, 19 die actions)");
    return fails != 0;
}

/* The JSON reader on awkward input. */
static int test_json(void) {
    char *mem;
    const J *v = j_parse("{\"a\":[1,-2.5e1,true,null,\"x\\u00fby\\n\\\"\"],\"b\":{},\"c\":[]}", &mem);
    CHECK(v && v->type == J_OBJ, "json: object");
    if (v) {
        const J *a = j_get(v, "a");
        CHECK(j_len(a) == 5 && a->child->num == 1 && a->child->next->num == -25 && a->child->next->next->type == J_BOOL, "json: array values");
        const J *s = a->child->next->next->next->next;
        CHECK(s->type == J_STR && !strcmp(s->str, "x\xC3\xBBy\n\""), "json: string escapes");
        CHECK(j_get(v, "b")->type == J_OBJ && j_len(j_get(v, "c")) == 0 && !j_get(v, "zz"), "json: empty containers");
        free(mem);
    }
    static const char *const bad[] = { "", "{", "[1,]", "{\"a\"}", "{\"a\":1,}", "\"abc", "[1] x", "{\"a\":\"\\u12\"}", "nul" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { CHECK(!j_parse(bad[i], &mem), "json: accepted bad input %s", bad[i]); }
    char sl[64]; slug("Barad-d\xC3\xBBr", sl, sizeof sl); CHECK(!strcmp(sl, "barad-dur"), "slug: %s", sl);
    slug("Goblin's Gate", sl, sizeof sl); CHECK(!strcmp(sl, "goblins-gate"), "slug: %s", sl);
    slug("The Nazg\xC3\xBBl Strike!", sl, sizeof sl); CHECK(!strcmp(sl, "the-nazgul-strike"), "slug: %s", sl);
    return fails != 0;
}


/* ================= the player ================= */
typedef struct { Net fp, sh; Policy pol[2]; } Player;      /* as in src/train.c */
static int player_load(Player *p, const char *path) {
    char b[512];
    pol_init(&p->pol[FP]); pol_init(&p->pol[SH]);
    if (!strcmp(path, "prior")) { nn_init(&p->fp, 1); nn_init(&p->sh, 2); return 0; }
    snprintf(b, sizeof b, "%s.pfp", path); pol_load(&p->pol[FP], b);
    snprintf(b, sizeof b, "%s.psh", path); pol_load(&p->pol[SH], b);
    snprintf(b, sizeof b, "%s.fp", path); if (nn_load(&p->fp, b)) return -1;
    snprintf(b, sizeof b, "%s.sh", path); if (nn_load(&p->sh, b)) return -1;
    return 0;
}

/* ================= text building ================= */
typedef struct { char *p; size_t n, cap; } Buf;
static void bput(Buf *b, const char *fmt, ...) {
    va_list ap;
    if (!b->p) { b->cap = 1024; b->n = 0; b->p = malloc(b->cap); if (!b->p) { fprintf(stderr, "out of memory\n"); exit(1); } }
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k >= 0 && (size_t)k < b->cap - b->n) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + (size_t)(k > 0 ? k : 0) + 256;
        b->p = realloc(b->p, b->cap);
        if (!b->p) { fprintf(stderr, "out of memory\n"); exit(1); }
    }
}
static void bcomma(Buf *b) { char c = b->n ? b->p[b->n - 1] : 0; if (c != '[' && c != '{' && c != ',' && c != ':') bput(b, ","); }
static uint64_t fnv1a(uint64_t h, const char *t) { for (; *t; t++) { h ^= (unsigned char)*t; h *= 0x100000001b3ULL; } return h; }

/* ================= the referee's own bookkeeping ================= */
/* The engine counts dice per face; the protocol shows each die with a number and a status. */
enum { DS_UNROLLED, DS_AVAILABLE, DS_USED, DS_HUNT };
static const char *const DS_KEY[4] = { "unrolled", "available", "used", "hunt_box" };
typedef struct { uint8_t face, status; } Die;       /* face NFACE: not rolled */
#define MAXDICE 16
typedef struct {
    State s;
    Die dice[2][MAXDICE]; int nd[2];
    int ext;                    /* the side the outside bot plays (-1: none) */
    int mock;                   /* the outside bot is replaced by random answers */
    uint64_t rng;               /* for mock answers and for filling in what our player cannot see */
    int next_id, seq, clock_ms;
    char game[40];
    long asked, illegal;
} Ref;

static const char *comp_key(const State *s, int c) {
    if (c == C_GANDALF && s->gandalf_white) return CHAR_KEY[CH_GANDALF_WHITE];
    if (c == C_STRIDER && s->aragorn) return CHAR_KEY[CH_ARAGORN];
    return CHAR_KEY[c];
}
static const char *minion_key(int m) { return CHAR_KEY[CH_MINION0 + m]; }
static int pool_dice(const State *s, int side) {     /* as pool_size() in src/flow.c */
    if (side == FP) return 4 + (s->aragorn && s->comp_loc[C_STRIDER] < NREG) + (s->gandalf_white && s->comp_loc[C_GANDALF] < NREG);
    int n = 7;
    for (int m = 0; m < NMINION; m++) n += s->minion_loc[m] < NREG;
    return n;
}
static void dice_unrolled(Ref *R) {
    for (int side = 0; side < 2; side++) {
        R->nd[side] = pool_dice(&R->s, side);
        for (int i = 0; i < R->nd[side]; i++) { R->dice[side][i].face = NFACE; R->dice[side][i].status = DS_UNROLLED; }
    }
}
static void dice_rolled(Ref *R) {
    const State *s = &R->s;
    for (int side = 0; side < 2; side++) {
        int n = 0;
        if (side == SH) for (int i = 0; i < s->hunt_sh && n < MAXDICE; i++) { R->dice[SH][n].face = F_SPECIAL; R->dice[SH][n++].status = DS_HUNT; }
        for (int f = 0; f < NFACE; f++) for (int i = 0; i < s->dice[side][f] && n < MAXDICE; i++) { R->dice[side][n].face = (uint8_t)f; R->dice[side][n++].status = DS_AVAILABLE; }
        R->nd[side] = n;
    }
}
/* Brings the numbered dice in line with the engine's counts after a move; pref is the die the mover named (or -1). */
static void dice_sync(Ref *R, const State *before, int mover, int pref) {
    const State *s = &R->s;
    if (s->turn != before->turn) { dice_unrolled(R); return; }
    if (before->dkind == D_HUNT_ALLOC) { dice_rolled(R); return; }
    for (int side = 0; side < 2; side++) {
        int freed[MAXDICE], nf = 0;
        for (int f = 0; f < NFACE; f++) for (int k = before->dice[side][f] - s->dice[side][f]; k > 0; k--) {
            int pick = -1;
            if (side == mover && pref >= 0 && pref < R->nd[side] && R->dice[side][pref].status == DS_AVAILABLE && R->dice[side][pref].face == f) pick = pref;
            for (int i = 0; i < R->nd[side] && pick < 0; i++) if (R->dice[side][i].status == DS_AVAILABLE && R->dice[side][i].face == f) pick = i;
            if (pick < 0) continue;
            R->dice[side][pick].status = DS_USED; freed[nf++] = pick;
        }
        for (int f = 0; f < NFACE; f++) for (int k = s->dice[side][f] - before->dice[side][f]; k > 0; k--) {   /* an Elven Ring, or a card's extra die */
            int i = nf ? freed[--nf] : R->nd[side] < MAXDICE ? R->nd[side]++ : -1;
            if (i < 0) continue;
            R->dice[side][i].face = (uint8_t)f; R->dice[side][i].status = DS_AVAILABLE;
        }
        int inbox = 0, want = side == SH ? s->hunt_sh : s->hunt_fp;
        for (int i = 0; i < R->nd[side]; i++) inbox += R->dice[side][i].status == DS_HUNT;
        while (inbox < want && nf) { int i = freed[--nf]; R->dice[side][i].status = DS_HUNT; if (side == SH) R->dice[side][i].face = F_SPECIAL; inbox++; }
    }
}
/* The numbered dice and the engine's counts must tell the same story. */
static int dice_check(const Ref *R, char *err, int n) {
    for (int side = 0; side < 2; side++) for (int f = 0; f < NFACE; f++) {
        int k = 0;
        for (int i = 0; i < R->nd[side]; i++) k += R->dice[side][i].status == DS_AVAILABLE && R->dice[side][i].face == f;
        if (k != R->s.dice[side][f]) { snprintf(err, (size_t)n, "dice: side %d face %d: %d numbered, %d in the engine", side, f, k, R->s.dice[side][f]); return 1; }
    }
    return 0;
}

/* ================= the view (SPEC section 7) ================= */
static void view_units(Buf *b, const State *s, int r) {
    const Battle *bt = &s->b;
    int rear = bt->active && bt->src == r;        /* the engine sets a rearguard aside during a battle: it is still in the region */
    bput(b, "\"units\":[");
    for (int n = 0; n < NNAT; n++) {
        int reg = s->reg[r][n] + (rear ? bt->rear_reg[n] : 0), eli = s->eli[r][n] + (rear ? bt->rear_eli[n] : 0);
        if (reg) { bcomma(b); bput(b, "{\"nation\":\"%s\",\"unit\":\"regular\",\"n\":%d}", NATION_KEY[n], reg); }
        if (eli) { bcomma(b); bput(b, "{\"nation\":\"%s\",\"unit\":\"elite\",\"n\":%d}", NATION_KEY[n], eli); }
        if (n < NFPNAT && s->ldr[r][n]) { bcomma(b); bput(b, "{\"nation\":\"%s\",\"unit\":\"leader\",\"n\":%d}", NATION_KEY[n], s->ldr[r][n]); }
    }
    if (s->naz[r]) { bcomma(b); bput(b, "{\"nation\":\"SAURON\",\"unit\":\"nazgul\",\"n\":%d}", s->naz[r]); }
    bput(b, "],\"characters\":[");
    for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == r) { bcomma(b); bput(b, "\"%s\"", comp_key(s, c)); }
    for (int m = 0; m < NMINION; m++) if (s->minion_loc[m] == r) { bcomma(b); bput(b, "\"%s\"", minion_key(m)); }
    bput(b, "]");
}
static void view_where(Buf *b, const char *key, int loc) {
    bcomma(b);
    if (loc < NREG) bput(b, "\"%s\":{\"where\":\"region\",\"region\":\"%s\"}", key, REGION_KEY[loc]);
    else bput(b, "\"%s\":{\"where\":\"%s\"}", key, loc == LOC_FSP ? "fellowship" : loc == LOC_DEAD ? "eliminated" : "not_in_play");
}
static void view_tiles(Buf *b, const State *s, int which) {     /* 0 pool, 1 reserve, 2 ready, 3 drawn */
    bput(b, "[");
    for (int t = 0; t < NTILE; t++) {
        if (t < NSTD_TILE) {
            int lo = which == 0 ? 0 : s->tile_pool[t], n = which == 0 ? s->tile_pool[t] : which == 3 ? s->tile_out[t] : 0;
            for (int i = lo; i < lo + n; i++) { bcomma(b); if (t == T_2R) bput(b, "\"2r\""); else bput(b, "\"%s%d\"", TILE_KEY[t].prefix, i + 1); }
        } else {
            int st = s->special_st[t], in = which == 0 ? s->tile_pool[t] > 0 : which == 1 ? st == 0 : which == 2 ? st == 1 : st == 2 && !s->tile_pool[t];
            if (in) { bcomma(b); bput(b, "\"%s\"", TILE_KEY[t].prefix); }
        }
    }
    bput(b, "]");
}
static void view_cards(Buf *b, const State *s, int side, int where, int skip1, int skip2) {
    bput(b, "[");
    for (int c = side * 48; c < side * 48 + 48; c++) if (s->card_where[c] == where && c != skip1 && c != skip2) { bcomma(b); bput(b, "\"%s\"", CARD_KEY[c]); }
    bput(b, "]");
}
static void view_json(Buf *b, const Ref *R, int pov) {
    const State *s = &R->s;
    const Battle *bt = &s->b;
    int opp = 1 - pov;
    /* a Combat card the opponent has chosen and not yet shown is already in the engine's discard pile: keep it out of sight */
    int hidden = bt->active && s->dkind == D_BATTLE_CARD && bt->card[opp] != 255 ? bt->card[opp] : -1;
    bput(b, "{\"viewer\":\"%s\",\"turn\":%d,\"phase\":\"%s\",\"to_act\":\"%s\",\"active_side\":\"%s\",\"regions\":{", SIDE_KEY[pov], s->turn,
         s->dkind == D_HUNT_ALLOC ? "HUNT_ALLOCATION" : s->dkind == D_FSP_DECLARE || s->dkind == D_GUIDE || s->dkind == D_ENTER_MORDOR ? "FELLOWSHIP" : "ACTION_RESOLUTION",
         SIDE_KEY[s->dplayer], SIDE_KEY[s->dplayer]);
    for (int r = 0; r < NREG; r++) {
        bcomma(b);
        bput(b, "\"%s\":{\"control\":", REGION_KEY[r]);
        if (is_settlement(r) && s->ctrl[r] < 2) bput(b, "\"%s\",", SIDE_KEY[s->ctrl[r]]); else bput(b, "null,");
        view_units(b, s, r);
        bput(b, ",\"siege\":%s}", s->siege[r] ? "true" : "false");
    }
    bput(b, "},\"politics\":{");
    for (int n = 0; n < NNAT; n++) { bcomma(b); bput(b, "\"%s\":{\"step\":%d,\"active\":%s}", NATION_KEY[n], s->pol[n], s->active[n] ? "true" : "false"); }
    bput(b, "},\"reinforcements\":{");
    for (int n = 0; n < NNAT; n++) { bcomma(b); bput(b, "\"%s\":{\"regular\":%d,\"elite\":%d,\"leader\":%d,\"nazgul\":%d}", NATION_KEY[n], s->pool_reg[n], s->pool_eli[n], n < NFPNAT ? s->pool_ldr[n] : 0, n == SAURON ? s->pool_naz : 0); }
    bput(b, "},\"casualties\":{");
    for (int n = 0; n < NFPNAT; n++) { bcomma(b); bput(b, "\"%s\":{\"regular\":%d,\"elite\":0,\"leader\":0}", NATION_KEY[n], s->cas_reg[n]); }
    bput(b, "},\"fellowship\":{\"region\":\"%s\",\"progress\":%d,\"corruption\":%d,\"hidden\":%s,\"mordor\":", REGION_KEY[s->fsp_loc], s->progress, s->corruption, s->revealed ? "false" : "true");
    if (s->mordor < 0) bput(b, "null"); else bput(b, "%d", s->mordor);
    bput(b, ",\"guide\":\"%s\",\"companions\":[", s->guide == GOLLUM ? "gollum" : comp_key(s, s->guide));
    for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == LOC_FSP) { bcomma(b); bput(b, "\"%s\"", CHAR_KEY[c]); }
    bput(b, "]},\"characters\":{");
    for (int c = 0; c < NCOMP; c++) {
        int up = (c == C_GANDALF && s->gandalf_white) || (c == C_STRIDER && s->aragorn);
        if (up) { bcomma(b); bput(b, "\"%s\":{\"where\":\"replaced\"}", CHAR_KEY[c]); view_where(b, comp_key(s, c), s->comp_loc[c]); }
        else view_where(b, CHAR_KEY[c], s->comp_loc[c]);
    }
    if (!s->gandalf_white) view_where(b, CHAR_KEY[CH_GANDALF_WHITE], LOC_NONE);
    if (!s->aragorn) view_where(b, CHAR_KEY[CH_ARAGORN], LOC_NONE);
    view_where(b, "gollum", s->guide == GOLLUM ? LOC_FSP : LOC_NONE);
    for (int m = 0; m < NMINION; m++) view_where(b, minion_key(m), s->minion_loc[m]);
    bput(b, "},\"dice\":{");
    for (int side = 0; side < 2; side++) {
        bcomma(b); bput(b, "\"%s\":[", SIDE_KEY[side]);
        for (int i = 0; i < R->nd[side]; i++) {
            Die d = R->dice[side][i];
            bcomma(b); bput(b, "{\"die\":%d,\"face\":", i);
            if (d.face >= NFACE) bput(b, "null"); else bput(b, "\"%s\"", face_key(side, d.face));
            bput(b, ",\"status\":\"%s\",\"used\":%s}", DS_KEY[d.status], d.status == DS_USED || d.status == DS_HUNT ? "true" : "false");
        }
        bput(b, "]");
    }
    bput(b, "},\"hunt\":{\"box\":{\"FREE_PEOPLES\":%d,\"SHADOW\":%d},\"pool\":", s->hunt_fp, s->hunt_sh);
    view_tiles(b, s, 0); bput(b, ",\"reserve\":"); view_tiles(b, s, 1); bput(b, ",\"ready\":"); view_tiles(b, s, 2);
    bput(b, ",\"drawn\":"); view_tiles(b, s, 3);
    bput(b, ",\"removed\":[],\"shadow_must_hunt\":%s},\"cards\":{\"hand\":", s->hunt_fp_prev ? "true" : "false");
    view_cards(b, s, pov, W_HAND, -1, -1);
    {
        int oh[2] = {0, 0}, deck[4] = {0, 0, 0, 0};
        for (int c = 0; c < NCARD; c++) {
            if (s->card_where[c] == W_DECK) deck[CARD_DECK(c)]++;
            if (CARD_SIDE(c) == opp && s->card_where[c] == W_HAND) oh[CARD_DECK(c) & 1]++;
        }
        if (hidden >= 0) oh[CARD_DECK(hidden) & 1]++;       /* the chosen Combat card counts as in the hand until it is shown */
        bput(b, ",\"discarded_face_down\":[],\"opponent_hand\":%d,\"opponent_hand_by_deck\":{\"character\":%d,\"strategy\":%d},\"opponent_discarded_face_down\":%d",
             oh[0] + oh[1], oh[0], oh[1], 0);
        bput(b, ",\"decks\":{\"FREE_PEOPLES\":{\"character\":%d,\"strategy\":%d},\"SHADOW\":{\"character\":%d,\"strategy\":%d}}", deck[0], deck[1], deck[2], deck[3]);
    }
    bput(b, ",\"discards\":{\"FREE_PEOPLES\":"); view_cards(b, s, FP, W_DISCARD, hidden, -1);
    bput(b, ",\"SHADOW\":"); view_cards(b, s, SH, W_DISCARD, hidden, -1);
    bput(b, "},\"on_table\":{\"FREE_PEOPLES\":"); view_cards(b, s, FP, W_TABLE, -1, -1);
    bput(b, ",\"SHADOW\":"); view_cards(b, s, SH, W_TABLE, -1, -1);
    bput(b, "}},\"victory_points\":{\"FREE_PEOPLES\":%d,\"SHADOW\":%d},\"elven_rings\":{\"FREE_PEOPLES\":%d,\"SHADOW\":%d},\"elven_ring_used\":{\"FREE_PEOPLES\":%s,\"SHADOW\":%s},\"battle\":",
         wotr_vp(s, FP), wotr_vp(s, SH), s->rings[FP], s->rings[SH], s->ring_used[FP] ? "true" : "false", s->ring_used[SH] ? "true" : "false");
    if (!bt->active) bput(b, "null");
    else {
        bput(b, "{\"attacker\":\"%s\",\"battle\":\"%s\",\"from\":\"%s\",\"region\":\"%s\",\"round\":%d,\"white_rider\":%s,\"attackers\":[", SIDE_KEY[bt->att],
             bt->siege ? "siege" : bt->sortie ? "sortie" : "field", REGION_KEY[bt->src], REGION_KEY[bt->dst], bt->round ? bt->round : 1, bt->white_rider ? "true" : "false");
        for (int n = 0; n < NNAT; n++) {
            if (NAT_SIDE(n) != bt->att) continue;
            if (s->reg[bt->src][n]) { bcomma(b); bput(b, "{\"nation\":\"%s\",\"unit\":\"regular\",\"n\":%d}", NATION_KEY[n], s->reg[bt->src][n]); }
            if (s->eli[bt->src][n]) { bcomma(b); bput(b, "{\"nation\":\"%s\",\"unit\":\"elite\",\"n\":%d}", NATION_KEY[n], s->eli[bt->src][n]); }
        }
        bput(b, "]");
        for (int side = 0; side < 2; side++) {
            const char *role = side == bt->att ? "attacker" : "defender";
            if (bt->card[side] == 255) continue;
            if (bt->card[side] == hidden) bput(b, ",\"%s_card_back\":\"%s\"", role, DECK_KEY[CARD_DECK(hidden) & 1]);
            else bput(b, ",\"%s_card\":\"%s\"", role, CARD_KEY[bt->card[side]]);
        }
        bput(b, "}");
    }
    bput(b, ",\"result\":null}");
}

/* ================= the engine's questions as protocol questions ================= */
/* The protocol asks in small steps (the die and what to do with it, then which Army, then which units ...); the
 * engine's moves are whole. Every engine move is written as the list of small answers that amount to it; the referee
 * then asks step by step, keeping the moves that still agree with the answers so far, until one is left. */
#define MAXSTEP 20
#define MAXLEAF 8192
typedef struct { uint8_t kind; char text[200]; } Step;      /* text: the option without its number, from "kind" on */
typedef struct { Move mv[2]; uint8_t nmv, nstep; int8_t die; Step st[MAXSTEP]; } Leaf;

static void step(Leaf *L, int kind, const char *fmt, ...) {
    if (L->nstep >= MAXSTEP) return;
    Step *st = &L->st[L->nstep++];
    va_list ap; va_start(ap, fmt); vsnprintf(st->text, sizeof st->text, fmt, ap); va_end(ap);
    st->kind = (uint8_t)kind;
}
static int card_asking(const State *s) {
    for (int i = s->sp - 1; i >= 0; i--) if (s->st[i].kind == FR_CARDFX || s->st[i].kind == FR_CARD) return s->st[i].v[0];
    return -1;
}
/* which units an Army move takes: asked as "all", or kind by kind */
static void army_steps(Leaf *L, const State *s, Move m, int kind_first, const char *okind) {
    int side = s->dplayer, src = m.a, lo = side == FP ? 0 : NFPNAT, hi = side == FP ? NFPNAT : NNAT;
    State t = *s; wotr_apply(&t, m);
    int all = m.e;
    for (int n = lo; n < hi; n++) all &= t.reg[src][n] + t.eli[src][n] == 0 || src == m.b;
    if (okind[0] == 'a' && okind[1] == 'd') step(L, kind_first, "\"kind\":\"advance\",\"all\":%s}", all ? "true" : "false");
    else step(L, kind_first, "\"kind\":\"army_move\",\"from\":\"%s\",\"to\":\"%s\",\"all\":%s}", REGION_KEY[src], REGION_KEY[m.b], all ? "true" : "false");
    if (all) return;
    for (int n = lo; n < hi; n++) {
        if (s->reg[src][n]) step(L, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"nation\":\"%s\",\"unit\":\"regular\"}", s->reg[src][n] - t.reg[src][n], NATION_KEY[n]);
        if (s->eli[src][n]) step(L, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"nation\":\"%s\",\"unit\":\"elite\"}", s->eli[src][n] - t.eli[src][n], NATION_KEY[n]);
        if (n < NFPNAT && s->ldr[src][n]) step(L, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"nation\":\"%s\",\"unit\":\"leader\"}", s->ldr[src][n] - t.ldr[src][n], NATION_KEY[n]);
    }
    if (side == SH && s->naz[src]) step(L, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"nation\":\"SAURON\",\"unit\":\"nazgul\"}", s->naz[src] > t.naz[src] ? s->naz[src] - t.naz[src] : 0);
    if (side == FP) { for (int c = 0; c < NCOMP; c++) if (s->comp_loc[c] == src) step(L, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"character\":\"%s\"}", t.comp_loc[c] != src, comp_key(s, c)); }
    else for (int k = 0; k < NMINION; k++) if (k != M_SARUMAN && s->minion_loc[k] == src) step(L, K_ARMY_SELECTION, "\"kind\":\"count\",\"count\":%d,\"character\":\"%s\"}", t.minion_loc[k] != src, minion_key(k));
}
static void recruit_step(Leaf *L, const State *s, Move m) {
    int nat = REGION[m.a].nation;
    step(L, K_RECRUIT, "\"kind\":\"recruit\",\"region\":\"%s\",\"nation\":\"%s\",\"unit\":\"%s\"}", REGION_KEY[m.a], NATION_KEY[nat],
         m.type == M_MUSTER_ELITE ? "elite" : !m.b ? "regular" : NAT_SIDE(nat) == FP ? "leader" : "nazgul");
}
/* the tail of an action: everything after "this die, this action" */
static void action_tail(Leaf *L, const State *s, Move m) {
    if (m.type == M_ARMY) army_steps(L, s, m, K_ARMY_MOVE, "army_move");
    else if (m.type == M_ATTACK) step(L, K_CHOOSE_ATTACK, "\"kind\":\"attack\",\"from\":\"%s\",\"to\":\"%s\",\"all\":%s}", REGION_KEY[m.a], REGION_KEY[m.b], m.f ? "false" : "true");
    else if (m.type == M_MUSTER || m.type == M_MUSTER_ELITE) recruit_step(L, s, m);
    else if (m.type == M_SEPARATE)
        for (int c = 0; c < NCOMP; c++) if ((m.a >> c) & 1) step(L, K_SEPARATE_COMPANIONS, "\"kind\":\"separate\",\"character\":\"%s\"}", CHAR_KEY[c]);
}
/* "this die, this action": the first answer of an action. Returns 0 for a move that has none (pass). */
static void action_head(Leaf *L, const State *s, Move m, int die) {
    int side = s->dplayer, f = m.die & 0x7F;
    char head[80];
    snprintf(head, sizeof head, "\"kind\":\"use_die\",\"die\":%d,\"face\":\"%s\",\"action\":", die, face_key(side, f));
    switch (m.type) {
    case M_PASS: step(L, K_ACTION, "\"kind\":\"pass\"}"); break;
    case M_SKIP: step(L, K_ACTION, "\"kind\":\"skip_die\",\"die\":%d,\"face\":\"%s\"}", die, face_key(side, f)); break;
    case M_RING: step(L, K_ACTION, "\"kind\":\"elven_ring\",\"die\":%d,\"from\":\"%s\",\"to\":\"%s\"}", die, face_key(side, m.a), face_key(side, m.b)); break;
    case M_FSP_MOVE: step(L, K_ACTION, "%s\"move_fellowship\"}", head); break;
    /* with Strider as Guide the BehindMedia referee offers only his way (any die, a Character die too) */
    case M_FSP_HIDE: step(L, K_ACTION, "%s\"%s\"}", head, s->guide != C_STRIDER && (f == F_CHAR || f == F_SPECIAL) ? "hide_fellowship" : "hide_fellowship_strider"); break;
    case M_SEPARATE: step(L, K_ACTION, "%s\"separate_companions\"}", head); break;
    case M_COMPS: step(L, K_ACTION, "%s\"move_companions\"}", head); break;
    case M_ARMY: step(L, K_ACTION, "%s\"%s\"}", head, m.f ? "move_armies" : "move_army_with_leader"); break;
    case M_ATTACK: step(L, K_ACTION, "%s\"%s\"}", head, m.e ? "attack_with_leader" : "attack"); break;
    case M_PLAY: step(L, K_ACTION, "%s\"play_card\",\"card\":\"%s\",\"as\":\"%s\"}", head, CARD_KEY[m.a], f == F_EVENT ? "EVENT" : FACE_KEY[CARD[m.a].type]); break;
    case M_DRAW: step(L, K_ACTION, "%s\"draw_card\",\"deck\":\"%s\"}", head, DECK_KEY[m.a]); break;
    case M_DIPLO: step(L, K_ACTION, "%s\"diplomacy\",\"nation\":\"%s\"}", head, NATION_KEY[m.a]); break;
    case M_MUSTER: case M_MUSTER_ELITE: step(L, K_ACTION, "%s\"recruit\"}", head); break;
    case M_GANDALF_WHITE: step(L, K_ACTION, "%s\"play_companion\",\"character\":\"gandalf-the-white\",\"region\":\"%s\"}", head, REGION_KEY[m.a]); break;
    case M_ARAGORN: step(L, K_ACTION, "%s\"play_companion\",\"character\":\"aragorn\",\"region\":\"%s\"}", head, REGION_KEY[s->comp_loc[C_STRIDER]]); break;
    case M_COMP_POLITICS: step(L, K_ACTION, "%s\"companion_politics\",\"character\":\"%s\"}", head, CHAR_KEY[m.a == GONDOR ? C_BOROMIR : m.a == ELVES ? C_LEGOLAS : C_GIMLI]); break;
    case M_DISCARD_TABLE: step(L, K_ACTION, "%s\"force_discard\",\"card\":\"%s\",\"any_die\":%s,\"elven_ring\":%s}", head, CARD_KEY[m.a], f == F_SPECIAL && side == FP ? "false" : "true", m.b ? "true" : "false"); break;
    case M_NAZGUL: step(L, K_ACTION, "%s\"move_minions\"}", head); break;
    case M_MINION: step(L, K_ACTION, "%s\"play_minion\",\"character\":\"%s\",\"region\":\"%s\"}", head, minion_key(m.a), REGION_KEY[m.b]); break;
    case M_VOICE: step(L, K_ACTION, "%s\"voice_of_saruman\",\"upgrade\":%s}", head, m.a ? "true" : "false"); break;
    default: step(L, K_ACTION, "%s\"other\"}", head); break;
    }
}
/* The engine pays for an action with one particular die; the rules allow others (a Muster/Army die for a Muster
 * action, a Will of the West for anything). Adds those versions of the plain actions, so the outside bot may name
 * the die it likes. Army moves, attacks and cards are left with the engine's die. */
static int other_dice(const State *s, Move *mv, int n) {
    int side = s->dplayer, n0 = n;
    for (int i = 0; i < n0 && n < MAXMOVES - 8; i++) {
        Move m = mv[i]; int f = m.die & 0x7F, any = 0, cat;
        switch (m.type) {
        case M_FSP_MOVE: case M_SEPARATE: case M_COMPS: case M_NAZGUL: cat = F_CHAR; break;
        case M_FSP_HIDE: cat = F_CHAR; any = s->guide == C_STRIDER; break;
        case M_DIPLO: case M_MUSTER: case M_MUSTER_ELITE: case M_MINION: case M_VOICE: cat = F_MUSTER; break;
        case M_DRAW: cat = F_EVENT; break;
        case M_COMP_POLITICS: cat = F_CHAR; any = 1; break;
        default: continue;
        }
        if (m.die & DIE_MOUTH) continue;
        for (int g = 0; g < NFACE; g++) {
            if (g == f || !s->dice[side][g]) continue;
            if (!(any || g == cat || (g == F_MA && cat == F_MUSTER) || (g == F_SPECIAL && side == FP))) continue;
            Move v = m; v.die = (uint8_t)g;
            int dup = 0;
            for (int j = 0; j < n && !dup; j++) dup = !memcmp(&mv[j], &v, sizeof v);
            if (!dup && n < MAXMOVES - 8) mv[n++] = v;
        }
    }
    return n;
}
static Leaf *leaf_new(Leaf *L, int *n, Move m) {
    if (*n >= MAXLEAF) return NULL;
    Leaf *l = &L[(*n)++];
    l->nmv = 1; l->mv[0] = m; l->nstep = 0; l->die = -1;
    return l;
}
/* The questions added to ask what the official texts leave to a player (State.qtag), written as the BehindMedia referee writes
 * a card's question: "answer" plus its typed field (a character), a Leader as "LEADER:<nation>", "skip". */
static void qtag_step(Leaf *L, const State *s, int a) {
    int side = s->qtag == Q_WORDS_OF_POWER || s->qtag == Q_BLACK_BREATH ? SH : FP;
    if (s->qtag == Q_NAZGUL_STRIKE || s->qtag == Q_BCC_NAZGUL || s->qtag == Q_BCC_ARMY || s->qtag == Q_BCC_ATTACK || s->qtag == Q_FLOCKS || s->qtag == Q_BALROG || s->qtag == Q_DRAW_DECK) side = SH;
    if (s->qtag == Q_DRAW_DECK) { step(L, K_CARD_CHOICE, "\"kind\":\"draw\",\"deck\":\"%s\"}", a ? "strategy" : "character"); return; }
    if (s->qtag == Q_WHITE_RIDER) { step(L, K_WHITE_RIDER, "\"kind\":\"%s\"}", a == 1 ? "use" : "skip"); return; }
    if (s->qtag == Q_FLOCKS || s->qtag == Q_BALROG) {      /* "Discard ...?": the BehindMedia referee's "yes" / "skip" */
        step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\"%s}", CARD_KEY[s->qtag == Q_FLOCKS ? CARD_FLOCKS_OF_CREBAIN : CARD_BALROG_OF_MORIA],
             a == 1 ? "yes" : "skip", a == 1 ? "" : ",\"skip\":true");
        return;
    }
    if (s->qtag == Q_BCC_ATTACK) {        /* The Black Captain Commands' attack: the BehindMedia referee's Choose attack */
        int from = s->minion_loc[M_WK] < NREG ? s->minion_loc[M_WK] : 0;
        if (card_asking(s) == CARD_THE_RINGWRAITHS_ARE_ABROAD && a < NREG)      /* an Army with a Nazgul next to the target */
            for (int r = 0; r < NREG; r++) if (s->naz[r] && units(s, SH, r) && (r == a || DIST[r][a] == 1)) { from = r; break; }
        step(L, K_CHOOSE_ATTACK, "\"kind\":\"attack\",\"from\":\"%s\",\"to\":\"%s\",\"all\":true}", REGION_KEY[from], REGION_KEY[a < NREG ? a : 0]);
        return;
    }
    if (s->qtag == Q_EXCESS) {      /* the BehindMedia referee's Remove excess unit */
        step(L, K_REMOVE_EXCESS_UNIT, "\"kind\":\"remove_unit\",\"nation\":\"%s\",\"unit\":\"%s\",\"region\":\"%s\"}", NATION_KEY[a & 7], a >= 8 ? "elite" : "regular", REGION_KEY[s->qreg]);
        return;
    }
    if (s->qtag == Q_LURE) {
        step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\"}", CARD_KEY[CARD_LURE_OF_THE_RING], a ? "eliminate" : "corruption");
        return;
    }
    if (s->qtag == Q_BCC_NAZGUL || s->qtag == Q_BCC_ARMY) {
        static const char *const first[2] = { "recruit", "move" }, *const then[3] = { "skip", "move", "attack" };
        const char *ans = a == 255 ? "skip" : s->qtag == Q_BCC_NAZGUL ? first[a & 1] : then[a < 3 ? a : 0];
        int ck = s->qtag == Q_BCC_ARMY && card_asking(s) == CARD_THE_RINGWRAITHS_ARE_ABROAD ? CARD_THE_RINGWRAITHS_ARE_ABROAD : CARD_THE_BLACK_CAPTAIN_COMMANDS;
        step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\"%s}", CARD_KEY[ck], ans, !strcmp(ans, "skip") ? ",\"skip\":true" : "");
        return;
    }
    int c = s->qtag == Q_WORN_WITH_SORROW ? CARD_WORN_WITH_SORROW_AND_TOIL : s->qtag == Q_NAZGUL_STRIKE ? CARD_THE_NAZGUL_STRIKE : s->b.card[side];
    const char *ck = c < NCARD ? CARD_KEY[c] : "";
    if (a == 255) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"skip\",\"skip\":true}", ck);
    else if (s->qtag == Q_NAZGUL_STRIKE && a == Q_HUNT) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"hunt\"}", ck);
    else if ((s->qtag == Q_WORN_WITH_SORROW || s->qtag == Q_NAZGUL_STRIKE) && a < NCARD) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"target_card\":\"%s\"}", ck, CARD_KEY[a], CARD_KEY[a]);
    else if (s->qtag == Q_WORN_WITH_SORROW && a == Q_HAND) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"hand\"}", ck);
    else if (s->qtag == Q_WORN_WITH_SORROW) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"target_card\":\"%s\"}", ck, CARD_KEY[a], CARD_KEY[a]);
    else if (a >= Q_LEADER && a < Q_LEADER + NFPNAT) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"LEADER:%s\"}", ck, NATION_KEY[a - Q_LEADER]);
    else if (a == Q_NAZGUL) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"nazgul\"}", ck);
    else if (a >= Q_MINION && a < Q_MINION + NMINION) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"character\":\"%s\"}", ck, minion_key(a - Q_MINION), minion_key(a - Q_MINION));
    else if (a < NCOMP) step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"character\":\"%s\"}", ck, comp_key(s, a), comp_key(s, a));
    else step(L, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%d\"}", ck, a);
}
/* A card's separation as the BehindMedia referee asks it: the Companions one at a time, then where to (a leaf per destination
   when the engine asks it next; with one way only, or on the Mordor Track where the Companions are removed, that one).
   l holds the steps so far; returns the new leaf count. */
static int separation_leaves(Leaf *L, int nl, Leaf *l, const State *s, Move m, int a) {
    for (int k = 0; k < NCOMP; k++) if ((a >> k) & 1) step(l, K_SEPARATE_COMPANIONS, "\"kind\":\"separate\",\"character\":\"%s\"}", CHAR_KEY[k]);
    State t = *s; wotr_apply(&t, m);
    if (!t.winner && t.dkind == D_SEP_DEST && t.dplayer == FP && t.nopt) {
        Leaf base = *l;
        for (int d = 0; d < t.nopt; d++) {
            Leaf *lx = d == 0 ? l : leaf_new(L, &nl, m);
            if (!lx) break;
            if (d) *lx = base;
            step(lx, K_SEPARATE_COMPANIONS, "\"kind\":\"separate_to\",\"to\":\"%s\"}", REGION_KEY[t.opt[d]]);
            lx->nmv = 2; memset(&lx->mv[1], 0, sizeof(Move)); lx->mv[1].type = M_CHOICE; lx->mv[1].a = t.opt[d];
        }
    } else if (a) {
        int c0 = __builtin_ctz((unsigned)a), to = t.comp_loc[c0] < NREG ? t.comp_loc[c0] : s->fsp_loc;
        step(l, K_SEPARATE_COMPANIONS, "\"kind\":\"separate_to\",\"to\":\"%s\"}", REGION_KEY[to]);
    }
    return nl;
}
/* Every legal move at the current question, as lists of small answers. */
static int build_leaves(const Ref *R, Leaf *L) {
    const State *s = &R->s;
    static Move mv[MAXMOVES];
    int side = s->dplayer, n = wotr_moves(s, mv), nl = 0;
    Leaf *l;
    if (s->dkind == D_ACTION) {
        n = other_dice(s, mv, n);
        /* attacks: the engine lists the whole Army first, then the same attack with a rearguard left behind */
        for (int i = 0; i < n; i++) if (mv[i].type == M_ATTACK) {
            mv[i].e = mv[i].f = 0;
            for (int j = 0; j < i; j++) if (mv[j].type == M_ATTACK && mv[j].die == mv[i].die && mv[j].a == mv[i].a && mv[j].b == mv[i].b) mv[i].f = 1;
        }
        int pa = -1;
        for (int i = 0; i < n; i++) if (mv[i].type == M_ARMY && mv[i].f) pa = mv[i].die;
        for (int i = 0; i < n; i++) if (mv[i].type == M_ATTACK && pa >= 0 && mv[i].die != pa) mv[i].e = 1;     /* a Character die: an attack with a Leader */
        for (int i = 0; i < n; i++) {
            Move m = mv[i], real = m;
            if (m.type == M_ATTACK) real.e = real.f = 0;
            int f = (m.type == M_RING ? m.a : m.die) & 0x7F;
            Leaf tail; tail.nstep = 0;
            int ndest = 0; uint8_t dest[MAXOPT];
            if (m.type == M_SEPARATE) {         /* the companions one by one, then where they go (the engine's next question) */
                State t = *s; wotr_apply(&t, real);
                if (!t.winner && t.dkind == D_SEP_DEST && t.dplayer == side) { ndest = t.nopt; memcpy(dest, t.opt, t.nopt); }
                else { int c = __builtin_ctz((unsigned)m.a); dest[0] = t.comp_loc[c] < NREG ? t.comp_loc[c] : s->fsp_loc; ndest = -1; }
            }
            action_tail(&tail, s, m);
            for (int d = 0; d < (m.type == M_PASS ? 1 : R->nd[side]); d++) {
                if (m.type != M_PASS && !(R->dice[side][d].status == DS_AVAILABLE && R->dice[side][d].face == f)) continue;
                for (int k = 0; k < (ndest > 0 ? ndest : 1); k++) {
                    if (!(l = leaf_new(L, &nl, real))) return nl;
                    l->die = (int8_t)(m.type == M_PASS ? -1 : d);
                    action_head(l, s, m, d);
                    for (int j = 0; j < tail.nstep && l->nstep < MAXSTEP; j++) l->st[l->nstep++] = tail.st[j];
                    if (m.type == M_SEPARATE) {
                        step(l, K_SEPARATE_COMPANIONS, "\"kind\":\"separate_to\",\"to\":\"%s\"}", REGION_KEY[dest[k]]);
                        if (ndest > 0) { l->nmv = 2; memset(&l->mv[1], 0, sizeof(Move)); l->mv[1].type = M_CHOICE; l->mv[1].a = dest[k]; }
                    }
                }
            }
        }
        return nl;
    }
    for (int i = 0; i < n; i++) {
        Move m = mv[i]; int a = m.a;
        if (s->dkind == D_NAZ_MOVE && m.type == M_NAZ_MOVE && m.d == 0 && m.c > 1) continue;    /* the protocol moves one Nazgul at a time */
        if (!(l = leaf_new(L, &nl, m))) return nl;
        switch (s->dkind) {
        case D_ARMY_MOVE2: if (m.type == M_ARMY) army_steps(l, s, m, K_ARMY_MOVE, "army_move"); else step(l, K_ARMY_MOVE, "\"kind\":\"end\"}"); break;
        case D_MUSTER2: if (m.type == M_MUSTER) recruit_step(l, s, m); else step(l, K_RECRUIT, "\"kind\":\"end\"}"); break;
        case D_COMP_MOVE:
            if (m.type != M_COMP_MOVE) { step(l, K_MOVE_COMPANIONS, "\"kind\":\"end\"}"); break; }
            {
                int lead = __builtin_ctz((unsigned)m.a), from = s->comp_loc[lead], others = comps_at(s, from) & ~s->comp_moved & ~(1 << lead);
                step(l, K_MOVE_COMPANIONS, "\"kind\":\"move_companion\",\"character\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}", comp_key(s, lead), REGION_KEY[from], REGION_KEY[m.b]);
                for (int c = 0; c < NCOMP; c++) if ((others >> c) & 1)
                    step(l, K_JOIN_GROUP, "\"kind\":\"%s\",\"character\":\"%s\",\"to\":\"%s\"}", (m.a >> c) & 1 ? "join" : "stay", comp_key(s, c), REGION_KEY[m.b]);
            }
            break;
        case D_NAZ_MOVE:
            if (m.type != M_NAZ_MOVE) { step(l, K_MOVE_MINIONS, "\"kind\":\"end\"}"); break; }
            if (m.d == 0) {
                step(l, K_MOVE_MINIONS, "\"kind\":\"select_nazgul\",\"from\":\"%s\"}", REGION_KEY[m.a]);
                step(l, K_MOVE_MINIONS, "\"kind\":\"move_nazgul\",\"from\":\"%s\",\"to\":\"%s\"}", REGION_KEY[m.a], REGION_KEY[m.b]);
            } else {
                const char *who = minion_key(m.d == 1 ? M_WK : M_MOUTH);
                step(l, K_MOVE_MINIONS, "\"kind\":\"select_minion\",\"character\":\"%s\",\"from\":\"%s\"}", who, REGION_KEY[m.a]);
                step(l, K_MOVE_MINIONS, "\"kind\":\"move_minion\",\"character\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}", who, REGION_KEY[m.a], REGION_KEY[m.b]);
            }
            break;
        case D_DISCARD: step(l, K_DISCARD_EXCESS_CARD, "\"kind\":\"discard\",\"card\":\"%s\"}", CARD_KEY[a]); break;
        case D_FSP_DECLARE:         /* the phase's one question (fr_fsp_phase): done, declare, a Guide (200 + c), enter Mordor (199) */
            if (a == 255) step(l, K_FELLOWSHIP_PHASE, "\"kind\":\"done\"}");
            else if (a >= 200 && a < 200 + NCOMP) step(l, K_FELLOWSHIP_PHASE, "\"kind\":\"guide\",\"character\":\"%s\"}", comp_key(s, a - 200));
            else if (a == 199) step(l, K_FELLOWSHIP_PHASE, "\"kind\":\"enter_mordor\"}");
            else step(l, K_FELLOWSHIP_PHASE, "\"kind\":\"declare\",\"region\":\"%s\"}", REGION_KEY[a]);
            break;
        case D_GUIDE:               /* in the Fellowship phase keeping the present Guide is "done", as the protocol's menu has it */
            if (s->st[s->sp - 1].kind == FR_FSP_PHASE && a == s->guide) step(l, K_FELLOWSHIP_PHASE, "\"kind\":\"done\"}");
            else step(l, s->st[s->sp - 1].kind == FR_FSP_PHASE ? K_FELLOWSHIP_PHASE : K_CHOOSE_GUIDE, "\"kind\":\"guide\",\"character\":\"%s\"}", CHAR_KEY[a]);
            break;
        case D_ENTER_MORDOR: step(l, K_FELLOWSHIP_PHASE, a ? "\"kind\":\"enter_mordor\"}" : "\"kind\":\"done\"}"); break;
        case D_HUNT_ALLOC: step(l, K_HUNT_ALLOCATION, "\"kind\":\"hunt_dice\",\"count\":%d}", a); break;
        case D_SEP_DEST: step(l, K_SEPARATE_COMPANIONS, "\"kind\":\"separate_to\",\"to\":\"%s\"}", REGION_KEY[a]); break;
        case D_BATTLE_CARD: if (a == 255) step(l, K_COMBAT_CARD, "\"kind\":\"none\"}"); else step(l, K_COMBAT_CARD, "\"kind\":\"combat_card\",\"card\":\"%s\"}", CARD_KEY[a]); break;
        case D_CASUALTY: step(l, K_CASUALTY, "\"kind\":\"casualty\",\"nation\":\"%s\",\"unit\":\"%s\",\"effect\":\"%s\"}", NATION_KEY[a & 7], a >> 3 ? "elite" : "regular", (a >> 3) == 1 ? "reduce" : "remove"); break;
        case D_CONTINUE: step(l, K_CEASE_ATTACK, a ? "\"kind\":\"continue\"}" : "\"kind\":\"cease\"}"); break;
        case D_RETREAT: if (a == 255) step(l, K_RETREAT, "\"kind\":\"stand\"}"); else step(l, K_RETREAT, "\"kind\":\"retreat\",\"region\":\"%s\"}", REGION_KEY[a]); break;
        case D_SIEGE_RETREAT: step(l, K_FIELD_OR_SIEGE, a ? "\"kind\":\"siege\"}" : "\"kind\":\"field\"}"); break;
        case D_ADVANCE: step(l, K_ADVANCE, a ? "\"kind\":\"advance\",\"all\":true}" : "\"kind\":\"stay\"}"); break;
        case D_SIEGE_EXTEND: if (a == 255) step(l, K_EXTEND_SIEGE, "\"kind\":\"stop\"}"); else step(l, K_EXTEND_SIEGE, "\"kind\":\"extend\",\"nation\":\"%s\"}", NATION_KEY[a]); break;
        case D_HUNT_RESPONSE:       /* the engine asks the Hunt's damage, the cards against it and the Guide's ability as one question */
            if (a == 0) step(l, K_HUNT_CASUALTY, "\"kind\":\"corruption\"}");
            else if (a == 4) step(l, K_HUNT_CASUALTY, "\"kind\":\"guide\"}");
            else if (a == 5) step(l, K_HUNT_CASUALTY, "\"kind\":\"random\"}");
            else if (a == 3) step(l, K_HUNT_CASUALTY, "\"kind\":\"guide_ability\"}");
            else step(l, K_HUNT_CASUALTY, "\"kind\":\"card_choice\",\"target_card\":\"%s\"}", CARD_KEY[a == 1 ? CARD_AXE_AND_BOW : CARD_HORN_OF_GONDOR]);
            break;
        case D_REVEAL_DEST: step(l, K_REVEAL_FELLOWSHIP, "\"kind\":\"reveal\",\"region\":\"%s\"}", REGION_KEY[a]); break;
        case D_MITHRIL: case D_STAFF:
            step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"yes\":%s}", CARD_KEY[s->dkind == D_MITHRIL ? CARD_MITHRIL_COAT_AND_STING : CARD_WIZARDS_STAFF], a ? "yes" : "no", a ? "true" : "false");
            break;
        case D_SELF_HITS:
            step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%d\",\"count\":%d}", s->b.card[SH] < NCARD ? CARD_KEY[s->b.card[SH]] : "", a, a);
            break;
        default: {                  /* a card's own question */
            if (s->qtag) { qtag_step(l, s, a); break; }
            int c = card_asking(s);
            const char *ck = c >= 0 && c < NCARD ? CARD_KEY[c] : "";
            if (a == 255) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"none\",\"skip\":true}", ck);
            /* There is Another Way with Gollum as Guide: the BehindMedia referee's "hide" / "move" / "skip" */
            else if (c == CARD_THERE_IS_ANOTHER_WAY && s->dkind == D_CARD_OPTION && a <= 2)
                step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\"%s}", ck, a == 1 ? "hide" : a == 2 ? "move" : "skip", a ? "" : ",\"skip\":true");
            else if ((s->dkind == D_CARD_REGION || s->dkind == D_CARD_ARMY) && a < NREG) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"region\":\"%s\"}", ck, REGION_KEY[a], REGION_KEY[a]);
            else if (s->dkind == D_CARD_NATION && a < NNAT) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"nation\":\"%s\"}", ck, NATION_KEY[a], NATION_KEY[a]);
            else if (s->dkind == D_CARD_CARD && a < NCARD) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"target_card\":\"%s\"}", ck, CARD_KEY[a], CARD_KEY[a]);
            /* Gwaihir the Windlord, We Prove the Swifter: the BehindMedia referee's "move_companion" (a Companion, from, to: one answer),
               or "separate" and then the separation */
            else if (s->dkind == D_CARD_COMP && (c == CARD_GWAIHIR_THE_WINDLORD || c == CARD_WE_PROVE_THE_SWIFTER) && (a & 0x80)) {
                int k0 = __builtin_ctz((unsigned)(a & 0x7F)), from = s->comp_loc[k0];
                State t = *s; wotr_apply(&t, m);
                int made = 0;
                if (!t.winner && t.dkind == D_CARD_REGION && t.dplayer == FP)
                    for (int d = 0; d < t.nopt; d++) {
                        if (t.opt[d] == 255) continue;
                        Leaf *lx = made ? leaf_new(L, &nl, m) : l;
                        if (!lx) break;
                        step(lx, K_CARD_CHOICE, "\"kind\":\"move_companion\",\"character\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}", CHAR_KEY[k0], REGION_KEY[from < NREG ? from : 0], REGION_KEY[t.opt[d]]);
                        lx->nmv = 2; memset(&lx->mv[1], 0, sizeof(Move)); lx->mv[1].type = M_CHOICE; lx->mv[1].a = t.opt[d];
                        made = 1;
                    }
                if (!made) step(l, K_CARD_CHOICE, "\"kind\":\"move_companion\",\"character\":\"%s\",\"from\":\"%s\",\"to\":\"%s\"}", CHAR_KEY[k0], REGION_KEY[from < NREG ? from : 0], REGION_KEY[from < NREG ? from : 0]);
            }
            else if (s->dkind == D_CARD_COMP && (c == CARD_GWAIHIR_THE_WINDLORD || c == CARD_WE_PROVE_THE_SWIFTER) && a < 0x80) {
                step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"separate\"}", ck);
                nl = separation_leaves(L, nl, l, s, m, a);
            }
            /* I Will Go Alone, There and Back Again: the BehindMedia referee's separation, the Companions one at a time, then where to */
            else if (s->dkind == D_CARD_COMP && (c == CARD_I_WILL_GO_ALONE || c == CARD_THERE_AND_BACK_AGAIN) && a < 0x80) {
                nl = separation_leaves(L, nl, l, s, m, a);
            }
            else if (s->dkind == D_CARD_COMP && __builtin_popcount((unsigned)(a & 0x7F)) == 1) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"character\":\"%s\"}", ck, CHAR_KEY[__builtin_ctz((unsigned)a)], CHAR_KEY[__builtin_ctz((unsigned)a)]);
            else step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%d\",\"count\":%d}", ck, a, a);
            break; }
        }
    }
    return nl;
}

/* ================= talking to the outside bot ================= */
static char *read_line(void) {
    static char *line; static size_t cap;
    size_t n = 0;
    for (;;) {
        int c = getchar();
        if (c == EOF) { if (!n) return NULL; break; }
        if (n + 2 > cap) { cap = cap * 2 + 4096; line = realloc(line, cap); if (!line) return NULL; }
        if (c == '\n') break;
        line[n++] = (char)c;
    }
    if (!line) { cap = 16; line = malloc(cap); }
    line[n] = 0;
    return line;
}
static void send_line(const char *text) { fputs(text, stdout); fputc('\n', stdout); fflush(stdout); }
static void send_event(Ref *R, const char *fmt, ...) {
    if (R->ext < 0 || R->mock) return;
    char ev[256], msg[400];
    va_list ap; va_start(ap, fmt); vsnprintf(ev, sizeof ev, fmt, ap); va_end(ap);
    snprintf(msg, sizeof msg, "{\"type\":\"event\",\"game\":\"%s\",\"seq\":%d,\"event\":%s}", R->game, ++R->seq, ev);
    send_line(msg);
}
static uint32_t ref_rnd(Ref *R, uint32_t n) {
    uint64_t z = (R->rng += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL; z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL; z ^= z >> 31;
    return (uint32_t)(((z >> 32) * n) >> 32);
}
/* What the question is about (SPEC section 4, "context"). */
static void context_json(Buf *b, const State *s, int kind) {
    const Frame *top = &s->st[s->sp - 1];
    if (kind == K_CASUALTY && top->kind == FR_CASUALTY)
        bput(b, ",\"context\":{\"region\":\"%s\",\"round\":%d,\"hits\":%d,\"attacker\":\"%s\"}", REGION_KEY[top->v[1]], s->b.round ? s->b.round : 1, top->v[2], SIDE_KEY[s->b.active ? s->b.att : 1 - s->dplayer]);
    else if (kind == K_HUNT_CASUALTY && top->kind == FR_HUNT) bput(b, ",\"context\":{\"damage\":%d}", top->v[4]);
    else if (kind == K_CARD_CHOICE) {
        int c = s->dkind == D_MITHRIL ? CARD_MITHRIL_COAT_AND_STING : s->dkind == D_STAFF ? CARD_WIZARDS_STAFF : s->dkind == D_SELF_HITS ? s->b.card[SH] : card_asking(s);
        if (c >= 0 && c < NCARD) bput(b, ",\"context\":{\"card\":\"%s\",\"question\":\"%s\"}", CARD_KEY[c], CARD[c].name);
    }
}
/* Asks the outside bot one protocol question; returns the number of the option it chose. */
static int ask(Ref *R, int kind, char **text, int n) {
    R->asked++;
    if (R->mock) {      /* a test: build the full message, check it is JSON, answer at random */
        Buf b = {0};
        bput(&b, "{\"view\":"); view_json(&b, R, R->s.dplayer); bput(&b, ",\"options\":[");
        for (int i = 0; i < n; i++) { bcomma(&b); bput(&b, "{\"o\":%d,%s", i, text[i]); }
        bput(&b, "]"); context_json(&b, &R->s, kind); bput(&b, "}");
        char *mem; if (!j_parse(b.p, &mem)) { fprintf(stderr, "MOCK: message is not JSON: ...%s\n", strstr(b.p, "\"options\"") ? strstr(b.p, "\"options\"") : b.p); exit(1); }
        free(mem); free(b.p);
        return (int)ref_rnd(R, (uint32_t)n);
    }
    static Buf b;
    int side = R->s.dplayer, id = ++R->next_id;
    uint64_t h = 0xcbf29ce484222325ULL;
    char tmp[260];
    snprintf(tmp, sizeof tmp, "%s\n%s\n%d\n", SIDE_KEY[side], KIND_KEY[kind], n); h = fnv1a(h, tmp);
    b.n = 0;
    bput(&b, "{\"type\":\"decide\",\"id\":%d,\"game\":\"%s\",\"decision\":{\"kind\":\"%s\",\"side\":\"%s\",\"prompt\":\"\",\"options\":[", id, R->game, KIND_KEY[kind], SIDE_KEY[side]);
    for (int i = 0; i < n; i++) {
        snprintf(tmp, sizeof tmp, "{\"o\":%d,%s\n", i, text[i]); h = fnv1a(h, tmp);
        bcomma(&b); bput(&b, "{\"o\":%d,%s", i, text[i]);
    }
    bput(&b, "],\"fingerprint\":\"%016llx\"", (unsigned long long)h);
    context_json(&b, &R->s, kind);
    bput(&b, "},\"view\":"); view_json(&b, R, side);
    bput(&b, ",\"clock\":{\"left_ms\":%d}}", R->clock_ms);
    send_line(b.p);
    for (;;) {
        char *line = read_line(), *mem;
        if (!line) { fprintf(stderr, "the outside bot closed the connection\n"); exit(3); }
        const J *m = j_parse(line, &mem);
        if (!m) continue;
        const char *type = j_str(m, "type");
        const J *mid = j_get(m, "id"), *o = j_get(m, "o");
        int choice = -2;
        if (type && !strcmp(type, "get_view")) {
            Buf v = {0}; bput(&v, "{\"type\":\"view\",\"id\":%d,\"view\":", mid ? (int)mid->num : 0); view_json(&v, R, side); bput(&v, "}");
            send_line(v.p); free(v.p);
        } else if (type && !strcmp(type, "choice") && mid && (int)mid->num == id) {
            choice = o && o->type == J_NUM && o->num >= 0 && o->num < n ? (int)o->num : -1;
        }
        free(mem);
        if (choice >= 0) return choice;
        if (choice == -1) {         /* not one of the options: the first option is played and the slip is counted */
            R->illegal++;
            fprintf(stderr, "ILLEGAL answer from the outside bot to %s; option 0 played\n", KIND_KEY[kind]);
            return 0;
        }
    }
}

static const char *how_key(const State *s) {
    return s->win_type == WIN_RING ? "RING_DESTROYED" : s->win_type == WIN_CORRUPTION ? "CORRUPTION" : s->win_type == WIN_FP_MIL ? "FREE_PEOPLES_MILITARY"
         : s->win_type == WIN_SH_MIL ? "SHADOW_MILITARY" : "TURN_CAP";
}

/* WOTR_DECISIONS_FILE=<file>: every decision of the game, both sides, as fixed records of the true State just before
 * it (sizeof(State) bytes) followed by the Move played (sizeof(Move) = 8), with one line per record in <file>.txt.
 * Off unless the variable is set. "bridge replay" writes the same records from a saved game. */
const char *wotr_dkind_name(int k);
static void decision_dump(const char *file, const State *s, Move m, uint64_t seed, int outside) {
    char ix[700], words[200];
    FILE *f = fopen(file, "ab");
    if (!f) return;
    fwrite(s, sizeof(State), 1, f); fwrite(&m, sizeof(Move), 1, f); fclose(f);
    wotr_move_str(s, m, words, sizeof words);
    snprintf(ix, sizeof ix, "%s.txt", file);
    if ((f = fopen(ix, "a"))) {
        fprintf(f, "seed=%llu turn=%d side=%s player=%s kind=%s move=%s\n", (unsigned long long)seed, s->turn, SIDE_KEY[s->dplayer],
                outside ? "outside" : "ours", wotr_dkind_name(s->dkind), words);
        fclose(f);
    }
}

/* One game. Returns 0, or 1 if a check failed on the way. */
static int play_game(Ref *R, Agent *ag, uint64_t seed, int verbose) {
    static Leaf *L; static char **text; static uint8_t *alive;
    if (!L) { L = malloc(sizeof(Leaf) * MAXLEAF); text = malloc(sizeof(char *) * MAXLEAF); alive = malloc(MAXLEAF); }
    State *s = &R->s;
    char buf[200], err[200];
    int bad = 0, turn = 0;
    wotr_init(s, seed);
    R->rng = seed * 77 + 5; R->seq = 0;
    dice_unrolled(R);
    while (!s->winner) {
        State before = *s;
        int side = s->dplayer, pref = -1;
        Move first;
        if (s->turn != turn) {
            turn = s->turn;
            send_event(R, "{\"kind\":\"turn_started\",\"turn\":%d}", turn);
            const char *sf = getenv("WOTR_STATES_FILE");    /* the true position at the first decision of each turn, raw, for training starts */
            if (sf) {
                char ix[600]; FILE *f = fopen(sf, "ab");
                if (f) { fwrite(s, sizeof(State), 1, f); fclose(f); }
                snprintf(ix, sizeof ix, "%s.txt", sf); f = fopen(ix, "a");
                if (f) { fprintf(f, "seed=%llu turn=%d outside_side=%s to_decide=%s state_bytes=%zu\n", (unsigned long long)seed, turn, R->ext < 0 ? "none" : SIDE_KEY[R->ext], SIDE_KEY[side], sizeof(State)); fclose(f); }
            }
            if (verbose) fprintf(stderr, "--- turn %d: victory points FP %d, Shadow %d; corruption %d; Fellowship %s progress %d%s\n", turn, wotr_vp(s, FP), wotr_vp(s, SH), s->corruption,
                                 s->mordor >= 0 ? "on the Mordor track, step" : REGION[s->fsp_loc].name, s->mordor >= 0 ? s->mordor : s->progress, s->revealed ? " (revealed)" : "");
        }
        if (side == R->ext) {
            wotr_prune = 0;
            int nl = build_leaves(R, L), depth = 0, left = nl;
            if (!nl) { fprintf(stderr, "ERROR: no legal move for the outside bot at question %d\n", s->dkind); return 1; }
            memset(alive, 1, (size_t)nl);
            for (;;) {
                int n = 0, kind = -1, ended = 0;
                for (int i = 0; i < nl; i++) {
                    if (!alive[i]) continue;
                    if (L[i].nstep <= depth) { ended++; continue; }
                    int dup = 0;
                    for (int j = 0; j < n && !dup; j++) dup = !strcmp(text[j], L[i].st[depth].text);
                    if (!dup) { text[n++] = L[i].st[depth].text; if (kind < 0) kind = L[i].st[depth].kind; }
                }
                if (!n) break;                              /* every answer is in: the moves left are the same move */
                if (ended) { fprintf(stderr, "ERROR: a move ends where others go on (question %d, step %d)\n", s->dkind, depth); bad = 1; }
                int o = ask(R, kind, text, n);
                const char *chosen = text[o];
                for (int i = 0; i < nl; i++) if (alive[i] && (L[i].nstep <= depth || strcmp(L[i].st[depth].text, chosen))) { alive[i] = 0; left--; }
                depth++;
            }
            int pick = 0;
            while (pick < nl && !alive[pick]) pick++;
            if (pick >= nl) { fprintf(stderr, "ERROR: no move left after the answers\n"); return 1; }
            (void)left;
            pref = L[pick].die; first = L[pick].mv[0];
            wotr_move_str(s, first, buf, sizeof buf);
            if (verbose) fprintf(stderr, "%s* %s\n", side ? "  SH" : "FP  ", buf);
            for (int k = 0; k < L[pick].nmv; k++) {
                if (getenv("WOTR_DECISIONS_FILE")) decision_dump(getenv("WOTR_DECISIONS_FILE"), s, L[pick].mv[k], seed, 1);
                wotr_apply(s, L[pick].mv[k]);
            }
        } else {
            wotr_prune = 1;
            State c = *s; float bv; int n;
            wotr_determinize(&c, side, R->rng += 0x1234567);       /* our player sees only what its side may know */
            first = agent_choose(ag, &c, &bv, &n);
            wotr_move_str(s, first, buf, sizeof buf);
            if (verbose && n > 1) fprintf(stderr, "%s  %s   (chance the Free Peoples win: %.0f%%)\n", side ? "  SH" : "FP  ", buf, 100 * nn_prob(bv));
            if (getenv("WOTR_DECISIONS_FILE")) decision_dump(getenv("WOTR_DECISIONS_FILE"), s, first, seed, 0);
            wotr_apply(s, first);
        }
        dice_sync(R, &before, side, pref);
        if (first.type == M_FSP_MOVE && before.dkind == D_ACTION) send_event(R, "{\"kind\":\"fellowship_moved\"}");
        if (first.type == M_FSP_HIDE && before.dkind == D_ACTION) send_event(R, "{\"kind\":\"fellowship_hidden\"}");
        if (before.mordor < 0 && s->mordor >= 0) send_event(R, "{\"kind\":\"fellowship_entered_mordor\"}");
        if (before.dkind == D_ACTION && side == SH && (first.die & DIE_MOUTH) && (first.type == M_ARMY || first.type == M_ATTACK))
            send_event(R, "{\"kind\":\"die_used\",\"side\":\"SHADOW\",\"face\":\"MUSTER\",\"action\":\"%s\"}", first.type == M_ARMY ? "move_armies" : "attack");
        if (wotr_check(s, err, sizeof err) || dice_check(R, err, sizeof err)) { fprintf(stderr, "CHECK FAILED after \"%s\" (turn %d): %s\n", buf, s->turn, err); bad = 1; dice_rolled(R); }
    }
    wotr_prune = 1;
    return bad;
}

/* WOTR_ROLL_SH / WOTR_ROLL_FP="ms,rollouts,tau,shortlist,further turns" (the same variables as bin/wotr): our player
   chooses its actions by rollouts to the end of the turn when it plays that side (roll_choose in src/agent.c). */
static void roll_setup(Agent *ag, Player *p, int side) {
    float v[5] = {0, 0, 0.15f, 12, 0};
    const char *e = getenv(side == SH ? "WOTR_ROLL_SH" : "WOTR_ROLL_FP");
    if (e) sscanf(e, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
    ag->roll_ms = v[0]; ag->roll_n = (int)v[1]; ag->roll_tau = v[2]; ag->roll_k = (int)v[3]; ag->roll_turns = (int)v[4];
    if (e) { ag->dpol[FP] = &p->pol[FP]; ag->dpol[SH] = &p->pol[SH]; }
}
static void roll_report(void) {
    if (agent_roll_decisions) fprintf(stderr, "rollouts: %ld decisions, %.0f rollouts each on average, %.0f ms on average, %.0f ms the longest\n",
        agent_roll_decisions, (double)agent_roll_count / agent_roll_decisions, agent_roll_ms_sum / agent_roll_decisions, agent_roll_ms_max);
}

static int cmd_referee(int argc, char **argv, int mock) {
    static Ref R; static Agent ag;
    Player *p = malloc(sizeof(Player));
    if (!p || player_load(p, argv[2])) { fprintf(stderr, "cannot load the player %s\n", argv[2]); return 1; }
    memset(&ag, 0, sizeof ag);
    ag.net[FP] = &p->fp; ag.net[SH] = &p->sh; ag.samples = 3; ag.temp = 0.02f;
    if (getenv("WOTR_LOOKAHEAD")) {   /* the player's own look-ahead, set as the training session measures it */
        ag.depth = 3; ag.width = 4; ag.replies = 1; ag.narrow = 6; ag.taper = 0;
        ag.dpol[FP] = &p->pol[FP]; ag.dpol[SH] = &p->pol[SH];
        fprintf(stderr, "look-ahead on: depth 3, width 4, replies 1, narrow 6\n");
    }
    memset(&R, 0, sizeof R);
    if (mock) {
        int games = atoi(argv[3]), bad = 0; uint64_t seed = strtoull(argv[4], 0, 10);
        long wins[2] = {0, 0};
        R.mock = 1;
        for (int g = 0; g < games; g++) {
            R.ext = g & 1; ag.rng = seed + (uint64_t)g + 1; roll_setup(&ag, p, !R.ext);
            bad += play_game(&R, &ag, seed + (uint64_t)g, 0);
            if (R.s.winner) wins[R.s.winner - 1]++;
        }
        roll_report();
        printf("mock: %d games, %ld questions asked, %d games with a failed check; winners FP %ld, Shadow %ld\n", games, R.asked, bad, wins[0], wins[1]);
        return bad != 0;
    }
    uint64_t seed = strtoull(argv[4], 0, 10);
    R.ext = !strcmp(argv[3], "sh") ? SH : FP;
    R.clock_ms = argc > 5 ? atoi(argv[5]) : 10000;
    int sims = argc > 6 ? atoi(argv[6]) : 0;
    ag.rng = seed * 31 + 7;
    snprintf(R.game, sizeof R.game, "g-local-%llu", (unsigned long long)seed);
    /* the session (SPEC section 3) */
    char msg[400], *mem, botname[80] = "?";
    send_line("{\"type\":\"hello\",\"protocol\":\"wotr-bot/0.2\",\"referee\":\"cwotr-local 0.1\",\"id\":1}");
    for (;;) {
        char *line = read_line();
        if (!line) { fprintf(stderr, "the outside bot never said hello\n"); return 3; }
        const J *m = j_parse(line, &mem);
        if (!m) continue;
        const char *type = j_str(m, "type");
        if (type && !strcmp(type, "hello")) {
            if (j_str(m, "name")) snprintf(botname, sizeof botname, "%s %s", j_str(m, "name"), j_str(m, "version") ? j_str(m, "version") : "");
            const J *opts = j_get(m, "options");
            Buf b = {0}; bput(&b, "{\"type\":\"options\",\"values\":{");
            for (const J *o = opts ? opts->child : NULL; o; o = o->next) {
                const char *name = j_str(o, "name"); const J *def = j_get(o, "default");
                if (!name || !def || def->type != J_NUM) continue;
                bcomma(&b); bput(&b, "\"%s\":%d", name, sims && !strcmp(name, "sims") ? sims : (int)def->num);
            }
            bput(&b, "}}"); send_line(b.p); free(b.p); free(mem);
            break;
        }
        free(mem);
    }
    snprintf(msg, sizeof msg, "{\"type\":\"new_game\",\"id\":2,\"game\":\"%s\",\"side\":\"%s\",\"rules\":\"base-2e\",\"variant\":[],\"seed_bot\":%llu,\"clock\":{\"kind\":\"per_decision\",\"ms\":%d}}",
             R.game, SIDE_KEY[R.ext], (unsigned long long)(seed * 2654435761ULL % 1000000007ULL), R.clock_ms);
    send_line(msg);
    for (;;) {
        char *line = read_line();
        if (!line) { fprintf(stderr, "the outside bot left before the game\n"); return 3; }
        const J *m = j_parse(line, &mem);
        int ready = m && j_str(m, "type") && !strcmp(j_str(m, "type"), "ready");
        if (m) free(mem);
        if (ready) break;
    }
    R.next_id = 2;
    fprintf(stderr, "game %s: %s plays %s, our player (%s) plays %s\n", R.game, botname, R.ext == FP ? "the Free Peoples" : "the Shadow", argv[2], R.ext == FP ? "the Shadow" : "the Free Peoples");
    roll_setup(&ag, p, !R.ext);
    if (ag.roll_ms > 0 || ag.roll_n > 0) fprintf(stderr, "rollouts on for our side: %.0f ms, at most %d rollouts, shortlist %d\n", ag.roll_ms, ag.roll_n, ag.roll_k);
    int bad = play_game(&R, &ag, seed, 1);
    roll_report();
    const State *s = &R.s;
    if (s->win_type == WIN_NONE) snprintf(msg, sizeof msg, "{\"type\":\"game_over\",\"game\":\"%s\",\"winner\":null,\"how\":\"TURN_CAP\",\"turn\":%d}", R.game, s->turn);
    else snprintf(msg, sizeof msg, "{\"type\":\"game_over\",\"game\":\"%s\",\"winner\":\"%s\",\"how\":\"%s\",\"turn\":%d}", R.game, SIDE_KEY[s->winner - 1], how_key(s), s->turn);
    send_line(msg);
    send_line("{\"type\":\"quit\"}");
    fprintf(stderr, "RESULT game=%s outside_bot=%s outside_side=%s winner=%s how=%s turn=%d questions=%ld illegal_answers=%ld failed_checks=%d fp_vp=%d sh_vp=%d corruption=%d\n",
            R.game, botname, SIDE_KEY[R.ext], s->win_type == WIN_NONE ? "none" : SIDE_KEY[s->winner - 1], how_key(s), s->turn, R.asked, R.illegal, bad, wotr_vp(s, FP), wotr_vp(s, SH), s->corruption);
    return 0;
}

/* ================= replaying a saved game ================= */
/* bridge replay <referee log> <out file> [outside|ours|all]: plays the game of a referee log again from its seed, as the
 * log records it move by move (the dice come from the State's own generator, so the same moves bring the same dice),
 * and writes the chosen player's decisions as WOTR_DECISIONS_FILE does. The log leaves out our player's decisions with
 * a single choice and the outside bot's destination after separating Companions: those are found by search. When
 * <log minus .log>.states is there, every turn's start must equal its saved State byte for byte. */
#define RP_MAXD 8000
#define RP_SILENT 8                 /* an unlogged decision of ours is searched only when it has at most this many moves */
typedef struct { int side, ext; char text[200]; } RpEntry;
static RpEntry *rp_e; static int rp_ne, rp_ext, rp_depth, rp_far;
static State *rp_states, *rp_path; static int rp_turn_at[100];
static Move *rp_move; static uint8_t *rp_out;
static long rp_budget, rp_turns_checked;
static State *rp_farst; static Move rp_farmv[RP_MAXD]; static int rp_fard;      /* the position where the replay got furthest in the log, for the report when it fails */

static int rp_candidates(const State *s, Move *mv) {
    static Move pm[MAXMOVES];
    wotr_prune = 0;
    int n = wotr_moves(s, mv);
    if (s->dplayer != rp_ext) {             /* our player chose from its own (pruned) generator, whose moves can differ */
        wotr_prune = 1;
        int np = wotr_moves(s, pm);
        for (int k = 0; k < np && n < MAXMOVES; k++) {
            int dup = 0;
            for (int j = 0; j < n && !dup; j++) dup = !memcmp(&mv[j], &pm[k], sizeof(Move));
            if (!dup) mv[n++] = pm[k];
        }
        return n;
    }
    if (s->dkind != D_ACTION) return n;
    n = other_dice(s, mv, n);
    for (int i = 0, n0 = n; i < n0 && n < MAXMOVES; i++) if (mv[i].type == M_ATTACK && (mv[i].e || mv[i].f)) {
        Move v = mv[i]; v.e = v.f = 0;                  /* the referee plays the outside bot's attacks this way */
        int dup = 0;
        for (int j = 0; j < n && !dup; j++) dup = !memcmp(&mv[j], &v, sizeof v);
        if (!dup) mv[n++] = v;
    }
    return n;
}
/* a move applied as play_game applies it: the engine's own follow-ups differ with wotr_prune, which play_game sets to 0
   for the outside bot's moves and to 1 for ours */
static void rp_apply(State *t, Move m) { wotr_prune = t->dplayer != rp_ext; wotr_apply(t, m); }
static int rp_go(const State *s, int i, int d, int turn) {
    if (--rp_budget < 0 || d >= RP_MAXD) return 0;
    if (i > rp_far) { rp_far = i; *rp_farst = *s; rp_fard = d; memcpy(rp_farmv, rp_move, sizeof(Move) * (size_t)d); }
    if (s->winner) { rp_depth = d; return i == rp_ne; }
    if (s->turn != turn) {
        turn = s->turn;
        if (turn < 100 && rp_turn_at[turn] >= 0) { if (memcmp(s, &rp_states[rp_turn_at[turn]], sizeof(State))) return 0; }
    }
    struct { Move mv[MAXMOVES], pm[MAXMOVES]; State t; char w[200]; } *F = malloc(sizeof *F);
    int n = rp_candidates(s, F->mv), side = s->dplayer, out = side == rp_ext, ok = 0;
    rp_path[d] = *s; rp_out[d] = (uint8_t)out;
    if (i < rp_ne && rp_e[i].side == side && rp_e[i].ext == out)
        for (int k = 0; k < n && !ok; k++) {
            wotr_move_str(s, F->mv[k], F->w, sizeof F->w);
            if (strcmp(F->w, rp_e[i].text)) continue;
            F->t = *s; rp_apply(&F->t, F->mv[k]); rp_move[d] = F->mv[k];
            ok = rp_go(&F->t, i + 1, d + 1, turn);
        }
    if (!ok && !out) {                                  /* our player's only move under its own (pruned) generator */
        wotr_prune = 1;
        int np = wotr_moves(s, F->pm);
        if (np == 1) { F->t = *s; rp_apply(&F->t, F->pm[0]); rp_move[d] = F->pm[0]; ok = rp_go(&F->t, i, d + 1, turn); }
    }
    if (!ok && (out ? s->dkind == D_SEP_DEST : n <= RP_SILENT)) {
        for (int k = 0; k < n && !ok; k++) {
            F->t = *s; rp_apply(&F->t, F->mv[k]); rp_move[d] = F->mv[k];
            ok = rp_go(&F->t, i, d + 1, turn);
        }
    }
    free(F);
    return ok;
}
static int cmd_replay(int argc, char **argv) {
    const char *who = argc > 4 ? argv[4] : "outside";
    FILE *f = fopen(argv[2], "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }
    char line[4096], res[600] = "";
    unsigned long long seed = 0; int have = 0, cap = 4096;
    rp_e = malloc(sizeof(RpEntry) * (size_t)cap); rp_ne = 0; rp_ext = -1;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "game g-local-%llu", &seed) == 1) { have = 1; rp_ext = strstr(line, "plays the Free Peoples, our player") ? FP : SH; continue; }
        if (!strncmp(line, "RESULT", 6)) { snprintf(res, sizeof res, "%s", line); continue; }
        char *p = line;
        while (*p == ' ') p++;
        int side = !strncmp(p, "FP", 2) ? FP : !strncmp(p, "SH", 2) ? SH : -1;
        if (side < 0 || (p[2] != ' ' && p[2] != '*')) continue;
        /* play_game writes "FP  * move", "  SH* move", "FP    move", "  SH  move"; some moves' words start with spaces */
        p += 2;
        int ext = side == FP ? !strncmp(p, "  * ", 4) : !strncmp(p, "* ", 2);
        if (strncmp(p, side == FP ? (ext ? "  * " : "    ") : (ext ? "* " : "  "), side == FP ? 4 : 2)) continue;
        p += side == FP ? 4 : 2;
        char *cut = strstr(p, "   (chance the Free Peoples win");
        if (cut) *cut = 0;
        size_t k = strlen(p); while (k && (p[k - 1] == '\n' || p[k - 1] == ' ')) p[--k] = 0;
        if (rp_ne == cap) rp_e = realloc(rp_e, sizeof(RpEntry) * (size_t)(cap *= 2));
        rp_e[rp_ne].side = side; rp_e[rp_ne].ext = ext; snprintf(rp_e[rp_ne].text, sizeof rp_e[rp_ne].text, "%s", p); rp_ne++;
    }
    fclose(f);
    if (!have) { fprintf(stderr, "%s: no \"game g-local-<seed>\" line\n", argv[2]); return 1; }
    /* the saved State at the start of each turn, if the game kept them */
    for (int t = 0; t < 100; t++) rp_turn_at[t] = -1;
    char sp[1100]; snprintf(sp, sizeof sp, "%s", argv[2]);
    char *dot = strstr(sp, ".log"); int nst = 0;
    if (dot) {
        strcpy(dot, ".states");
        FILE *fs = fopen(sp, "rb");
        if (fs) {
            fseek(fs, 0, SEEK_END); long bytes = ftell(fs); fseek(fs, 0, SEEK_SET);
            nst = (int)(bytes / (long)sizeof(State));
            rp_states = malloc(sizeof(State) * (size_t)(nst + 1));
            if (bytes % (long)sizeof(State) || fread(rp_states, sizeof(State), (size_t)nst, fs) != (size_t)nst) { fprintf(stderr, "%s: not whole States of %zu bytes\n", sp, sizeof(State)); return 1; }
            fclose(fs);
            for (int j = 0; j < nst; j++) if (rp_states[j].turn < 100) rp_turn_at[rp_states[j].turn] = j;
        }
    }
    rp_path = malloc(sizeof(State) * RP_MAXD); rp_move = malloc(sizeof(Move) * RP_MAXD); rp_out = malloc(RP_MAXD);
    State *s = malloc(sizeof(State));
    wotr_init(s, seed);
    rp_budget = 2000000; rp_far = 0; rp_depth = 0; rp_farst = malloc(sizeof(State)); *rp_farst = *s;
    int ok = rp_go(s, 0, 0, 0);
    wotr_prune = 1;
    if (!ok) {
        fprintf(stderr, "REPLAY FAILED %s seed=%llu: got as far as log move %d of %d (%s%s %s)%s\n", argv[2], seed, rp_far, rp_ne,
                rp_far < rp_ne ? SIDE_KEY[rp_e[rp_far].side] : "", rp_far < rp_ne && rp_e[rp_far].ext ? "*" : "", rp_far < rp_ne ? rp_e[rp_far].text : "", rp_budget < 0 ? "; search budget spent" : "");
        static Move mv[MAXMOVES]; char w[200];
        int n = rp_candidates(rp_farst, mv);
        fprintf(stderr, "  there: turn %d, %s to decide %s; its moves:", rp_farst->turn, SIDE_KEY[rp_farst->dplayer], wotr_dkind_name(rp_farst->dkind));
        for (int k = 0; k < n && k < 60; k++) { wotr_move_str(rp_farst, mv[k], w, sizeof w); fprintf(stderr, " [%s]", w); }
        if (getenv("WOTR_REPLAY_TRACE")) {          /* the moves of the furthest attempt, from the start */
            State *t = malloc(sizeof(State)); wotr_init(t, seed);
            for (int d = 0; d < rp_fard; d++) { wotr_move_str(t, rp_farmv[d], w, sizeof w); fprintf(stderr, "  T%d %s %s: %s\n", t->turn, SIDE_KEY[t->dplayer], wotr_dkind_name(t->dkind), w); rp_apply(t, rp_farmv[d]); }
            free(t);
        }
        wotr_prune = 1; n = wotr_moves(rp_farst, mv);
        fprintf(stderr, "\n  under our player's own generator:");
        for (int k = 0; k < n && k < 60; k++) { wotr_move_str(rp_farst, mv[k], w, sizeof w); fprintf(stderr, " [%s]", w); }
        fprintf(stderr, "\n");
        return 1;
    }
    const State *e = &rp_path[rp_depth - 1];
    State last = *e; rp_apply(&last, rp_move[rp_depth - 1]); wotr_prune = 1;
    char want[200]; snprintf(want, sizeof want, "winner=%s how=%s turn=%d ", last.win_type == WIN_NONE ? "none" : SIDE_KEY[last.winner - 1], how_key(&last), last.turn);
    if (res[0] && !strstr(res, want)) { fprintf(stderr, "REPLAY FAILED %s: the replay ends %s, the log says %s", argv[2], want, res); return 1; }
    int written = 0, silent = rp_depth - rp_ne;
    for (int d = 0; d < rp_depth; d++) {
        if (!strcmp(who, "outside") && !rp_out[d]) continue;
        if (!strcmp(who, "ours") && rp_out[d]) continue;
        decision_dump(argv[3], &rp_path[d], rp_move[d], seed, rp_out[d]);
        written++;
    }
    for (int t = 0; t < 100; t++) rp_turns_checked += rp_turn_at[t] >= 0 && t <= last.turn;
    printf("replay %s seed=%llu: ok, %d decisions (%d not in the log), %swritten %d; turn starts checked against saved States: %d of %d\n",
           argv[2], seed, rp_depth, silent, want, written, (int)rp_turns_checked, last.turn);
    return 0;
}

int main(int argc, char **argv) {
    wotr_global_init();
    feat_init();
    names_init();
    if (argc >= 3 && !strcmp(argv[1], "names")) return test_json() | test_names(argv[2]);
    if (argc >= 5 && !strcmp(argv[1], "referee")) return cmd_referee(argc, argv, 0);
    if (argc >= 5 && !strcmp(argv[1], "mock")) return cmd_referee(argc, argv, 1);
    if (argc >= 4 && !strcmp(argv[1], "replay")) return cmd_replay(argc, argv);
    fprintf(stderr, "usage: bridge names <vocabulary.json>\n       bridge referee <player|prior> <fp|sh: the outside bot's side> <seed> [clock ms=10000] [sims]\n       bridge mock <player|prior> <games> <seed>\n       bridge replay <referee log> <out file> [outside|ours|all]\n");
    return 2;
}
