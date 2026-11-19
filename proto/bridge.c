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
#include <time.h>

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
    double ms_sum[2], ms_max[2]; long ms_n[2];      /* thinking time per decision: [0] our player, [1] the outside bot */
} Ref;
static double ref_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }
static void ref_time(Ref *R, int who, double t0) { double ms = ref_now() - t0; R->ms_sum[who] += ms; R->ms_n[who]++; if (ms > R->ms_max[who]) R->ms_max[who] = ms; }

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
    case M_FSP_HIDE: step(L, K_ACTION, "%s\"%s\"}", head, f == F_CHAR || f == F_SPECIAL ? "hide_fellowship" : "hide_fellowship_strider"); break;
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
        case D_FSP_DECLARE: if (a == 255) step(l, K_FELLOWSHIP_PHASE, "\"kind\":\"done\"}"); else step(l, K_FELLOWSHIP_PHASE, "\"kind\":\"declare\",\"region\":\"%s\"}", REGION_KEY[a]); break;
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
            int c = card_asking(s);
            const char *ck = c >= 0 && c < NCARD ? CARD_KEY[c] : "";
            if (a == 255) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"none\",\"skip\":true}", ck);
            else if ((s->dkind == D_CARD_REGION || s->dkind == D_CARD_ARMY) && a < NREG) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"region\":\"%s\"}", ck, REGION_KEY[a], REGION_KEY[a]);
            else if (s->dkind == D_CARD_NATION && a < NNAT) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"nation\":\"%s\"}", ck, NATION_KEY[a], NATION_KEY[a]);
            else if (s->dkind == D_CARD_CARD && a < NCARD) step(l, K_CARD_CHOICE, "\"kind\":\"card_choice\",\"card\":\"%s\",\"answer\":\"%s\",\"target_card\":\"%s\"}", ck, CARD_KEY[a], CARD_KEY[a]);
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
    double t0 = ref_now();
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
        if (choice >= 0) { ref_time(R, 1, t0); return choice; }
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

/* WOTR_AGREE=1 (a test of the bot mode): at every question answered by the outside bot, what would our own player have
   chosen in the true position? Counted per engine question; our player asked twice shows how much it varies by itself. */
static Agent *g_agx; static long g_agree[ND][3];
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
            if (g_agx && nl > 1) {
                Move d[2]; float bv; int n;
                for (int q = 0; q < 2; q++) { State c = *s; wotr_prune = 1; wotr_determinize(&c, side, R->rng += 0x1234567); d[q] = agent_choose(g_agx, &c, &bv, &n); if (d[q].type == M_NAZ_MOVE && d[q].d == 0 && d[q].c > 1) d[q].c = 1; }
                wotr_prune = 0;
                g_agree[s->dkind][0]++; g_agree[s->dkind][1] += !memcmp(&d[0], &first, sizeof first); g_agree[s->dkind][2] += !memcmp(&d[0], &d[1], sizeof first);
            }
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
            double t0 = ref_now();
            first = agent_choose(ag, &c, &bv, &n);
            if (n > 1) ref_time(R, 0, t0);
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
    ag.dpol[FP] = &p->pol[FP]; ag.dpol[SH] = &p->pol[SH];     /* always: the lean toward the move-suggester (WOTR_HUMAN_FP/SH) needs it. Until 2026-11-19 it was
                                                                 set only with look-ahead or rollouts, so a player without them played WITHOUT its lean. */
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
    static Agent agx;
    if (getenv("WOTR_AGREE")) { agx = ag; agx.rng = seed * 131 + 9; if (R.ext == SH) { agx.depth = 3; agx.width = 4; agx.replies = 1; agx.narrow = 6; agx.taper = 0; } else agx.depth = 0; g_agx = &agx; }
    if (ag.roll_ms > 0 || ag.roll_n > 0) fprintf(stderr, "rollouts on for our side: %.0f ms, at most %d rollouts, shortlist %d\n", ag.roll_ms, ag.roll_n, ag.roll_k);
    int bad = play_game(&R, &ag, seed, 1);
    roll_report();
    const State *s = &R.s;
    if (s->win_type == WIN_NONE) snprintf(msg, sizeof msg, "{\"type\":\"game_over\",\"game\":\"%s\",\"winner\":null,\"how\":\"TURN_CAP\",\"turn\":%d}", R.game, s->turn);
    else snprintf(msg, sizeof msg, "{\"type\":\"game_over\",\"game\":\"%s\",\"winner\":\"%s\",\"how\":\"%s\",\"turn\":%d}", R.game, SIDE_KEY[s->winner - 1], how_key(s), s->turn);
    send_line(msg);
    send_line("{\"type\":\"quit\"}");
    if (g_agx) for (int k = 0; k < ND; k++) if (g_agree[k][0]) fprintf(stderr, "AGREE %s asked=%ld bot_same=%ld self_same=%ld\n", wotr_dkind_name(k), g_agree[k][0], g_agree[k][1], g_agree[k][2]);
    fprintf(stderr, "RESULT game=%s outside_bot=%s outside_side=%s winner=%s how=%s turn=%d questions=%ld illegal_answers=%ld failed_checks=%d fp_vp=%d sh_vp=%d corruption=%d our_ms_avg=%.0f our_ms_max=%.0f bot_ms_avg=%.0f bot_ms_max=%.0f\n",
            R.game, botname, SIDE_KEY[R.ext], s->win_type == WIN_NONE ? "none" : SIDE_KEY[s->winner - 1], how_key(s), s->turn, R.asked, R.illegal, bad, wotr_vp(s, FP), wotr_vp(s, SH), s->corruption,
            R.ms_n[0] ? R.ms_sum[0] / R.ms_n[0] : 0, R.ms_max[0], R.ms_n[1] ? R.ms_sum[1] / R.ms_n[1] : 0, R.ms_max[1]);
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


/* ================= playing as a bot: the other end of the protocol (2026-11-19) =================
 * bridge bot <player>: our player as a protocol bot on stdin/stdout. A referee (any rules engine) holds the game; we
 * only ever answer with one of the options it lists, so an illegal move is impossible.
 *  - At an ACTION question the position is rebuilt from the view (what our side cannot see is dealt at random), our
 *    player chooses a whole move with its own engine, and the move is given as the small answers the protocol asks for.
 *  - The questions that continue our own action (the next Army, the next recruit, Companions, Nazgul) are answered
 *    from the position we then hold, as long as nothing random has happened in between.
 *  - Everything else (battles, the Hunt, the Fellowship phase, cards' questions) is answered by short written rules
 *    for now: the engine cannot yet be set down in the middle of a battle from a view. */
typedef struct {
    Ref R;                      /* R.s: our picture of the position; R.dice: the numbered dice as the view gives them */
    int me, valid;              /* valid: R.s is in step with the game */
    Step pend[MAXSTEP]; int npend, ipend; Move pmv[2]; int npmv;    /* the answers still to give for the chosen move; the engine moves they amount to */
    long n_dec, n_engine, n_chain, n_rule, n_zero, n_rebuild_fail, n_nomatch, n_fp_same, n_fp_seen, n_battle, n_battle_miss, n_hunt, n_turn, n_rule_kind[NKIND + 1];
    State prev; Move prevmv[2]; int nprev, prev_ok;     /* the position before our last move, and the move: the Hunt it sets off is played again from here */
    int naz_plan; uint8_t naz_tgt[NREG], wk_tgt, mouth_tgt;       /* where our player's Nazgul plan puts them: carried out one figure at a time */
    int box_turn, box_now, box_prev;        /* Free Peoples dice seen in the Hunt box this turn and the turn before (the view at the start of a turn no longer shows them) */
    uint64_t rng;
} Bot;

static void bot_dice(Bot *B, const J *v) {
    State *s = &B->R.s;
    memset(s->dice, 0, sizeof s->dice);
    for (int side = 0; side < 2; side++) {
        const J *arr = j_get(j_get(v, "dice"), SIDE_KEY[side]); int pos = 0;
        B->R.nd[side] = 0;
        for (int i = 0; i < MAXDICE; i++) { B->R.dice[side][i].face = NFACE; B->R.dice[side][i].status = DS_USED; }
        for (const J *d = arr ? arr->child : NULL; d; d = d->next, pos++) {
            const J *ix = j_get(d, "die"); int i = ix && ix->type == J_NUM ? (int)ix->num : pos, f = face_of(j_str(d, "face"));
            const char *st = j_str(d, "status"); int ds = find_key(st, DS_KEY, 4);
            if (i < 0 || i >= MAXDICE) continue;
            if (ds < 0) { const J *u = j_get(d, "used"); ds = u && u->num ? DS_USED : f >= 0 ? DS_AVAILABLE : DS_UNROLLED; }
            B->R.dice[side][i].face = (uint8_t)(f < 0 ? NFACE : f); B->R.dice[side][i].status = (uint8_t)ds;
            if (i + 1 > B->R.nd[side]) B->R.nd[side] = i + 1;
            if (ds == DS_AVAILABLE && f >= 0) s->dice[side][f]++;
        }
    }
}
static int j_int(const J *o, const char *key, int dflt) { const J *e = j_get(o, key); return e && (e->type == J_NUM || e->type == J_BOOL) ? (int)e->num : dflt; }
static void bot_cards(State *s, const J *list, int where) {
    for (const J *e = list ? list->child : NULL; e; e = e->next) { int c = e->type == J_STR ? card_of(e->str) : -1; if (c >= 0) s->card_where[c] = (uint8_t)where; }
}
/* The position of a view, resting at an action decision of our side. Returns 0, or 1 if it cannot be built. */
static int bot_view(Bot *B, const J *v, int who, char *err, int en) {      /* who: the side resting at its action decision */
    State *s = &B->R.s; int me = B->me, opp = 1 - me;
    const J *regs = j_get(v, "regions"), *f = j_get(v, "fellowship"), *h = j_get(v, "hunt"), *cd = j_get(v, "cards");
    if (!regs || !f || !h || !cd) { snprintf(err, (size_t)en, "the view lacks regions, fellowship, hunt or cards"); return 1; }
    wotr_blank(s);
    for (int c = 0; c < NCOMP; c++) s->comp_loc[c] = LOC_DEAD;
    for (int m = 0; m < NMINION; m++) s->minion_loc[m] = LOC_NONE;
    s->gandalf_white = s->aragorn = s->minion_seen = s->mouth_used = 0;
    memset(s->pool_reg, 0, sizeof s->pool_reg); memset(s->pool_eli, 0, sizeof s->pool_eli); memset(s->pool_ldr, 0, sizeof s->pool_ldr); s->pool_naz = 0;
    memset(s->cas_reg, 0, sizeof s->cas_reg);
    for (const J *e = regs->child; e; e = e->next) {
        int r = region_of(e->key); if (r < 0) continue;
        int cs = side_of(j_str(e, "control"));
        if (is_settlement(r) && cs >= 0) s->ctrl[r] = (uint8_t)cs;
        s->siege[r] = (uint8_t)(j_int(e, "siege", 0) != 0);
        const J *us = j_get(e, "units");
        for (const J *u = us ? us->child : NULL; u; u = u->next) {
            int nat = nation_of(j_str(u, "nation")), cnt = j_int(u, "n", 1); const char *k = j_str(u, "unit");
            if (!k || cnt <= 0) continue;
            if (!strcmp(k, "nazgul")) s->naz[r] = (uint8_t)(s->naz[r] + cnt);
            else if (nat < 0) continue;
            else if (!strcmp(k, "regular")) s->reg[r][nat] = (uint8_t)(s->reg[r][nat] + cnt);
            else if (!strcmp(k, "elite")) s->eli[r][nat] = (uint8_t)(s->eli[r][nat] + cnt);
            else if (!strcmp(k, "leader") && nat < NFPNAT) s->ldr[r][nat] = (uint8_t)(s->ldr[r][nat] + cnt);
        }
    }
    /* characters: the plain ones first, then the two upgrades over them */
    const J *chs = j_get(v, "characters");
    for (int pass = 0; pass < 2; pass++) for (const J *e = chs ? chs->child : NULL; e; e = e->next) {
        int ch = char_of(e->key); const char *w = j_str(e, "where"); if (ch < 0 || !w) continue;
        int loc = !strcmp(w, "fellowship") ? LOC_FSP : !strcmp(w, "eliminated") ? LOC_DEAD : !strcmp(w, "region") ? region_of(j_str(e, "region")) : !strcmp(w, "replaced") ? -2 : LOC_NONE;
        if (loc == -1 || loc == -2) continue;
        if (!pass && ch < NCOMP) { if (loc != LOC_NONE) s->comp_loc[ch] = (uint8_t)loc; }
        else if (!pass && ch >= CH_MINION0 && ch < CH_MINION0 + NMINION) { s->minion_loc[ch - CH_MINION0] = (uint8_t)loc; if (loc != LOC_NONE) s->minion_seen = 1; }
        else if (pass && ch == CH_GANDALF_WHITE && loc != LOC_NONE) { s->gandalf_white = 1; s->comp_loc[C_GANDALF] = (uint8_t)loc; }
        else if (pass && ch == CH_ARAGORN && loc != LOC_NONE) { s->aragorn = 1; s->comp_loc[C_STRIDER] = (uint8_t)loc; }
    }
    const J *pol = j_get(v, "politics"), *rf = j_get(v, "reinforcements"), *cas = j_get(v, "casualties");
    for (int n = 0; n < NNAT; n++) {
        const J *e = j_get(pol, NATION_KEY[n]); if (e) { s->pol[n] = (uint8_t)j_int(e, "step", s->pol[n]); s->active[n] = (uint8_t)(j_int(e, "active", s->active[n]) != 0); }
        e = j_get(rf, NATION_KEY[n]);
        if (e) { s->pool_reg[n] = (uint8_t)j_int(e, "regular", 0); s->pool_eli[n] = (uint8_t)j_int(e, "elite", 0); if (n < NFPNAT) s->pool_ldr[n] = (uint8_t)j_int(e, "leader", 0); if (n == SAURON) s->pool_naz = (uint8_t)j_int(e, "nazgul", 0); }
        e = j_get(cas, NATION_KEY[n]); if (e && n < NFPNAT) s->cas_reg[n] = (uint8_t)j_int(e, "regular", 0);
    }
    { int r = region_of(j_str(f, "region")); if (r >= 0) s->fsp_loc = (uint8_t)r; }
    s->progress = (uint8_t)j_int(f, "progress", 0); s->corruption = (uint8_t)j_int(f, "corruption", 0); s->revealed = (uint8_t)!j_int(f, "hidden", 1);
    { const J *m = j_get(f, "mordor"); s->mordor = (int8_t)(m && m->type == J_NUM ? (int)m->num : -1); }
    { const J *cl = j_get(f, "companions"); for (const J *e = cl ? cl->child : NULL; e; e = e->next) { int ch = e->type == J_STR ? char_of(e->str) : -1; if (ch == CH_GANDALF_WHITE) ch = C_GANDALF; if (ch == CH_ARAGORN) ch = C_STRIDER; if (ch >= 0 && ch < NCOMP) s->comp_loc[ch] = LOC_FSP; } }
    { const char *g = j_str(f, "guide"); int ch = char_of(g); s->guide = (uint8_t)(g && !strcmp(g, "gollum") ? GOLLUM : ch == CH_GANDALF_WHITE ? C_GANDALF : ch == CH_ARAGORN ? C_STRIDER : ch >= 0 && ch < NCOMP ? ch : GOLLUM); }
    bot_dice(B, v);
    { const J *box = j_get(h, "box"); s->hunt_fp = (uint8_t)j_int(box, "FREE_PEOPLES", 0); s->hunt_sh = (uint8_t)j_int(box, "SHADOW", 0); }
    s->hunt_fp_prev = (uint8_t)(j_int(h, "shadow_must_hunt", 0) != 0); s->fsp_acted = (uint8_t)(s->hunt_fp > 0);
    memset(s->tile_pool, 0, sizeof s->tile_pool); memset(s->tile_out, 0, sizeof s->tile_out); memset(s->special_st, 0, sizeof s->special_st);
    static const char *const place[5] = { "pool", "ready", "drawn", "removed", "reserve" };
    for (int w = 0; w < 5; w++) { const J *l = j_get(h, place[w]);
        for (const J *e = l ? l->child : NULL; e; e = e->next) {
            int t = e->type == J_STR ? tile_of(e->str) : -1; if (t < 0) continue;
            if (w == 0) { s->tile_pool[t]++; if (t >= NSTD_TILE) s->special_st[t] = 2; }
            else if (w == 1) { if (t >= NSTD_TILE) s->special_st[t] = 1; }
            else if (w == 2 || w == 3) { if (t < NSTD_TILE) s->tile_out[t]++; else s->special_st[t] = 2; }
        } }
    for (int c = 0; c < NCARD; c++) s->card_where[c] = W_DECK;
    bot_cards(s, j_get(j_get(cd, "discards"), "FREE_PEOPLES"), W_DISCARD); bot_cards(s, j_get(j_get(cd, "discards"), "SHADOW"), W_DISCARD);
    bot_cards(s, j_get(j_get(cd, "on_table"), "FREE_PEOPLES"), W_TABLE); bot_cards(s, j_get(j_get(cd, "on_table"), "SHADOW"), W_TABLE);
    bot_cards(s, j_get(cd, "discarded_face_down"), W_DISCARD); bot_cards(s, j_get(cd, "hand"), W_HAND);
    {   /* the opponent's cards: so many in hand per deck, so many left in each deck, the rest discarded unseen; which ones is dealt at random below */
        const J *bd = j_get(cd, "opponent_hand_by_deck"), *dk = j_get(j_get(cd, "decks"), SIDE_KEY[opp]);
        int tot = j_int(cd, "opponent_hand", 0);
        for (int d = 0; d < 2; d++) {
            int hand = bd ? j_int(bd, DECK_KEY[d], 0) : d ? tot / 2 : tot - tot / 2, left = dk ? j_int(dk, DECK_KEY[d], -1) : -1, unk = 0, deck = opp * 2 + d;
            for (int c = deck * 24; c < deck * 24 + 24; c++) unk += s->card_where[c] == W_DECK;
            int gone = left >= 0 ? unk - hand - left : 0;
            for (int c = deck * 24; c < deck * 24 + 24; c++) { if (s->card_where[c] != W_DECK) continue; if (hand > 0) { s->card_where[c] = W_HAND; hand--; } else if (gone > 0) { s->card_where[c] = W_DISCARD; gone--; } }
        }
    }
    s->hand_n[0] = s->hand_n[1] = 0;
    for (int c = 0; c < NCARD; c++) if (s->card_where[c] == W_HAND) s->hand_n[CARD_SIDE(c)]++;
    { const J *er = j_get(v, "elven_rings"), *eu = j_get(v, "elven_ring_used");
      for (int k = 0; k < 2; k++) { s->rings[k] = (uint8_t)j_int(er, SIDE_KEY[k], s->rings[k]); s->ring_used[k] = (uint8_t)(j_int(eu, SIDE_KEY[k], 0) != 0); } }
    s->turn = (uint16_t)j_int(v, "turn", 1);
    s->winner = 0; s->win_type = WIN_NONE; s->act_moves_left = 0; s->lock[0] = s->lock[1] = 0; s->comp_moved = 0; memset(&s->b, 0, sizeof s->b);
    s->dplayer = (uint8_t)who; s->st[s->sp - 1].v[0] = (uint8_t)who;
    wotr_determinize(s, me, B->rng += 0x1234567);
    if (wotr_check(s, err, en)) return strstr(err, "hand over limit") ? 0 : 1;      /* (a hand over the limit is what a discard question looks like) */
    return 0;
}

/* Is the option the answer a step stands for? Every field of the step must be in the option with the same value.
 * Returns the number of other fields the option has ("o" and "text" aside), or -1 if it is not the answer. */
static int opt_fits(const J *opt, const J *want) {
    int extra = 0;
    for (const J *w = want->child; w; w = w->next) {
        const J *x = j_get(opt, w->key);
        if (!x || x->type != w->type) return -1;
        if (w->type == J_STR ? strcmp(x->str, w->str) != 0 : w->type == J_NUM || w->type == J_BOOL ? x->num != w->num : 0) return -1;
    }
    for (const J *x = opt->child; x; x = x->next) if (strcmp(x->key, "o") && strcmp(x->key, "text") && !j_get(want, x->key)) extra++;
    return extra;
}
static int opt_number(const J *opt, int pos) { const J *o = j_get(opt, "o"); return o && o->type == J_NUM ? (int)o->num : pos; }
static int find_option(const J *opts, const char *steptext) {      /* the option's number, or -1 */
    char buf[260], *mem; snprintf(buf, sizeof buf, "{%s", steptext);
    const J *want = j_parse(buf, &mem); if (!want) return -1;
    int best = -1, be = 1 << 30, pos = 0;
    for (const J *o = opts ? opts->child : NULL; o; o = o->next, pos++) { int e = opt_fits(o, want); if (e >= 0 && e < be) { be = e; best = opt_number(o, pos); } }
    free(mem);
    return best;
}
static int find_kind(const J *opts, const char *kind) {             /* the first option of that kind, or -1 */
    int pos = 0;
    for (const J *o = opts ? opts->child : NULL; o; o = o->next, pos++) { const char *k = j_str(o, "kind"); if (k && !strcmp(k, kind)) return opt_number(o, pos); }
    return -1;
}
static int find_kv(const J *opts, const char *kind, const char *key, const char *val) {
    int pos = 0;
    for (const J *o = opts ? opts->child : NULL; o; o = o->next, pos++) { const char *k = j_str(o, "kind"), *x = j_str(o, key); if (k && !strcmp(k, kind) && x && !strcmp(x, val)) return opt_number(o, pos); }
    return -1;
}
/* the fighting units of each side in a region of the view */
static void view_count(const J *v, const char *region, int cnt[2]) {
    cnt[0] = cnt[1] = 0;
    const J *us = j_get(j_get(j_get(v, "regions"), region ? region : ""), "units");
    for (const J *u = us ? us->child : NULL; u; u = u->next) {
        int nat = nation_of(j_str(u, "nation")); const char *k = j_str(u, "unit");
        if (nat >= 0 && k && (!strcmp(k, "regular") || !strcmp(k, "elite"))) cnt[NAT_SIDE(nat)] += j_int(u, "n", 1);
    }
}
/* The written rules for the questions the engine does not yet answer. Returns an option number (0 if nothing better). */
static int bot_rule(const Bot *B, int kind, const J *dec, const J *v) {
    const J *opts = j_get(dec, "options"), *bt = j_get(v, "battle");
    int o = -1, att = 0, def = 0, pos = 0;
    if (bt && bt->type == J_OBJ) {
        int as = side_of(j_str(bt, "attacker")), c[2]; const char *from = j_str(bt, "from"), *reg = j_str(bt, "region");
        view_count(v, reg, c); def = as >= 0 ? c[1 - as] : 0;
        const J *al = j_get(bt, "attackers");
        for (const J *u = al ? al->child : NULL; u; u = u->next) att += j_int(u, "n", 1);
        if (!att && as >= 0) { view_count(v, from && reg && strcmp(from, reg) ? from : reg, c); att = c[as]; }
    }
    switch (kind) {
    case K_FELLOWSHIP_PHASE: if ((o = find_kind(opts, "enter_mordor")) < 0) o = find_kind(opts, "done"); break;
    case K_HUNT_ALLOCATION: {       /* two dice, three once the Fellowship is in Mordor; the nearest count on offer */
        const J *m = j_get(j_get(v, "fellowship"), "mordor"); int want = m && m->type == J_NUM ? 3 : 2, bd = 99;
        for (const J *x = opts ? opts->child : NULL; x; x = x->next, pos++) { int c = j_int(x, "count", -9), d = c > want ? c - want : want - c; if (c >= 0 && d < bd) { bd = d; o = opt_number(x, pos); } }
        break; }
    case K_FIELD_OR_SIEGE: o = find_kind(opts, "siege"); break;
    case K_COMBAT_CARD: o = find_kind(opts, "none"); break;
    case K_CASUALTY: if ((o = find_kv(opts, "casualty", "unit", "regular")) < 0) o = find_kv(opts, "casualty", "effect", "reduce"); break;
    case K_REMOVE_EXCESS_UNIT: o = find_kv(opts, "remove_unit", "unit", "regular"); break;
    case K_CEASE_ATTACK: o = find_kind(opts, att >= def ? "continue" : "cease"); break;
    case K_RETREAT: o = def >= att ? find_kind(opts, "stand") : find_kind(opts, "retreat"); break;
    case K_EXTEND_SIEGE: o = att >= 2 * def + 2 ? find_kind(opts, "extend") : find_kind(opts, "stop"); break;
    case K_ADVANCE: o = find_kind(opts, "advance"); break;
    case K_HUNT_CASUALTY: {
        const J *m = j_get(j_get(v, "fellowship"), "mordor"); int dmg = j_int(j_get(dec, "context"), "damage", 1);
        if ((m && m->type == J_NUM) || dmg >= 2) { if ((o = find_kind(opts, "guide")) < 0) o = find_kind(opts, "random"); }
        if (o < 0) o = find_kind(opts, "corruption");
        break; }
    case K_REVEAL_FELLOWSHIP: {     /* an own settlement if there is one, else a region without the enemy */
        int best = -1;
        for (const J *x = opts ? opts->child : NULL; x; x = x->next, pos++) {
            const char *rk = j_str(x, "region"); int r = region_of(rk), c[2], sc; if (r < 0) continue;
            view_count(v, rk, c);
            const char *ctl = j_str(j_get(j_get(v, "regions"), rk), "control");
            sc = (c[SH] ? -4 : 0) + (ctl && !strcmp(ctl, "SHADOW") ? -3 : 0) + (ctl && !strcmp(ctl, "FREE_PEOPLES") ? 2 : 0) + (c[FP] ? 1 : 0);
            if (sc > best || o < 0) { best = sc; o = opt_number(x, pos); }
        }
        break; }
    case K_WHITE_RIDER: case K_GUIDE_ABILITY: o = find_kind(opts, "use"); break;
    case K_ARMY_MOVE: case K_RECRUIT: case K_MOVE_COMPANIONS: case K_MOVE_MINIONS: o = find_kind(opts, "end"); break;
    case K_ARMY_SELECTION: { int bc = -1; for (const J *x = opts ? opts->child : NULL; x; x = x->next, pos++) { int c = j_int(x, "count", -1); if (c > bc) { bc = c; o = opt_number(x, pos); } } break; }
    case K_JOIN_GROUP: o = find_kind(opts, "join"); break;
    case K_ACTION: if ((o = find_kind(opts, "pass")) < 0) o = find_kv(opts, "use_die", "action", "draw_card"); break;
    default: break;
    }
    (void)B;
    return o < 0 ? 0 : o;
}

/* Sets our engine down inside the battle the view shows, at the question asked (the engine question `target`, ours):
 * the board of the view, the same attack started on it, and the dice thrown again until the question comes up as the
 * referee describes it (for a casualty: with that many hits to take; for the others: after a round without losses, so
 * that the board stays the view's). Returns 1 with the position in B->R.s, or 0. */
static int bot_battle(Bot *B, const J *v, const J *dec, int target) {
    const J *bt = j_get(v, "battle"); if (!bt || bt->type != J_OBJ) return 0;
    int att = side_of(j_str(bt, "attacker")), from = region_of(j_str(bt, "from")), to = region_of(j_str(bt, "region"));
    if (att < 0 || to < 0) return 0;
    if (from < 0) from = to;
    char err[200];
    if (bot_view(B, v, att, err, sizeof err)) return 0;
    State pre = B->R.s;
    pre.dice[att][F_ARMY]++;                                    /* the die that started the battle is spent in the view */
    int areg = 0, aeli = 0, round = j_int(bt, "round", 1), hits = j_int(j_get(dec, "context"), "hits", -1), card[2] = {-1, -1};
    { const J *al = j_get(bt, "attackers");
      for (const J *u = al ? al->child : NULL; u; u = u->next) { const char *k = j_str(u, "unit"); if (k && !strcmp(k, "regular")) areg += j_int(u, "n", 1); else if (k && !strcmp(k, "elite")) aeli += j_int(u, "n", 1); } }
    card[att] = card_of(j_str(bt, "attacker_card")); card[1 - att] = card_of(j_str(bt, "defender_card"));
    for (int k = 0; k < 2; k++) if (card[k] >= 0 && pre.card_where[card[k]] != W_HAND && pre.card_where[card[k]] != W_TABLE) { pre.card_where[card[k]] = W_HAND; pre.hand_n[CARD_SIDE(card[k])]++; }
    static Move mv[MAXMOVES], o[MAXMOVES];
    wotr_prune = 0;
    int n = wotr_moves(&pre, mv), pick = -1, best = -1;
    for (int i = 0; i < n; i++) if (mv[i].type == M_ATTACK && mv[i].a == from && mv[i].b == to) { int sc = mv[i].c == areg && mv[i].d == aeli ? 1000 : mv[i].c + mv[i].d; if (sc > best) { best = sc; pick = i; } }
    if (pick < 0) return 0;
    int tot0 = units(&pre, FP, from) + units(&pre, SH, from) + (to != from ? units(&pre, FP, to) + units(&pre, SH, to) : 0);
    for (uint64_t seed = 1; seed <= 3000; seed++) {
        State t = pre; t.rng = seed * 0x9E3779B97F4A7C15ULL + B->rng;
        wotr_apply(&t, mv[pick]);
        if (t.b.active && round > 1) { t.b.round = (uint8_t)round; if (t.b.siege && t.b.max_rounds < round) t.b.max_rounds = (uint8_t)round; }
        for (int stp = 0; stp < 24 && !t.winner && t.b.active; stp++) {
            int mine = t.dplayer == B->me, ch = -1, fail = 0;
            int tot = units(&t, FP, from) + units(&t, SH, from) + (to != from ? units(&t, FP, to) + units(&t, SH, to) : 0);
            if (mine && t.dkind == target) {
                const Frame *f = &t.st[t.sp - 1];
                int ok = target == D_CASUALTY ? f->kind == FR_CASUALTY && (hits < 0 || f->v[2] == hits) : tot == tot0;
                if (ok) { B->R.s = t; return 1; }
                break;
            }
            int k = wotr_moves(&t, o);
            if (k <= 0) break;
            switch (t.dkind) {
            case D_SIEGE_RETREAT: for (int j = 0; j < k; j++) if (o[j].a == 0) ch = j; break;                       /* the battle goes on in the field */
            case D_BATTLE_CARD: { int want = card[t.dplayer] >= 0 ? card[t.dplayer] : 255; for (int j = 0; j < k; j++) if (o[j].a == want) ch = j; if (ch < 0) for (int j = 0; j < k; j++) if (o[j].a == 255) ch = j; break; }
            case D_CONTINUE: for (int j = 0; j < k; j++) if (o[j].a == 1) ch = j; break;
            case D_RETREAT: for (int j = 0; j < k; j++) if (o[j].a == 255) ch = j; break;
            case D_CASUALTY: if (target == D_CASUALTY && !mine) ch = 0; else fail = 1; break;
            case D_SIEGE_EXTEND: case D_ADVANCE: fail = 1; break;
            default: ch = 0; break;
            }
            if (fail) break;
            wotr_apply(&t, o[ch < 0 ? 0 : ch]);
        }
    }
    return 0;
}
/* Our player's move in the position we hold, as one of the leaves; -1 if it is not among them. */
static int bot_pick(Bot *B, Agent *ag, const Leaf *L, int nl) {
    for (int full = 0; full < 2; full++) {       /* first as our player normally plays (purposeful moves only), then from the full list */
        State c = B->R.s; float bv; int n, have2 = 0; Move m2; memset(&m2, 0, sizeof m2);
        wotr_prune = !full;
        Move m = agent_choose(ag, &c, &bv, &n);
        if (m.type == M_NAZ_MOVE && m.d == 0 && m.c > 1) m.c = 1;        /* the protocol moves one Nazgul at a time: the others follow in the next questions */
        for (int i = 0; i < nl; i++) {
            if (memcmp(&L[i].mv[0], &m, sizeof m)) continue;
            if (L[i].nmv == 1) { wotr_prune = 0; return i; }
            if (!have2) { State t = B->R.s; wotr_apply(&t, m); m2 = agent_choose(ag, &t, &bv, &n); have2 = 1; }
            if (L[i].mv[1].type == m2.type && L[i].mv[1].a == m2.a) { wotr_prune = 0; return i; }
        }
    }
    wotr_prune = 0;
    return -1;
}
static int chain_dkind(int dk) { return dk == D_ARMY_MOVE2 || dk == D_MUSTER2 || dk == D_COMP_MOVE || dk == D_NAZ_MOVE || dk == D_SEP_DEST || (dk >= D_CARD_REGION && dk < ND); }
static void bot_applied(Bot *B) {       /* every answer of the chosen move is given: play it in our own picture too */
    State *s = &B->R.s; State before = *s; uint32_t nr = s->nrand;
    B->prev = before; memcpy(B->prevmv, B->pmv, sizeof B->prevmv); B->nprev = B->npmv; B->prev_ok = 1;
    for (int k = 0; k < B->npmv; k++) if (!s->winner) wotr_apply(s, B->pmv[k]);
    B->valid = !s->winner && s->dplayer == B->me && s->nrand == nr && chain_dkind(s->dkind);
    if (B->valid) dice_sync(&B->R, &before, B->me, -1);
    B->npend = B->ipend = 0; B->npmv = 0;
}

/* The Hunt our own move set off: the move is played again from the position before it, the dice and tiles falling anew,
 * until the engine asks the question the referee asks (for the Hunt's damage: with that much damage). */
static int bot_hunt(Bot *B, const J *dec, int target) {
    if (!B->prev_ok || B->nprev < 1) return 0;
    int dmg = j_int(j_get(dec, "context"), "damage", -1);
    static Move o[MAXMOVES];
    wotr_prune = 0;
    for (uint64_t seed = 1; seed <= 2000; seed++) {
        State t = B->prev; t.rng = seed * 0xD1342543DE82EF95ULL + B->rng;
        for (int k = 0; k < B->nprev && !t.winner; k++) wotr_apply(&t, B->prevmv[k]);
        for (int stp = 0; stp < 12 && !t.winner && t.dkind != D_ACTION; stp++) {
            if (t.dplayer == B->me && t.dkind == target) {
                const Frame *f = &t.st[t.sp - 1];
                if (target != D_HUNT_RESPONSE || dmg < 0 || (f->kind == FR_HUNT && f->v[4] == dmg)) { B->R.s = t; return 1; }
                break;
            }
            int k = wotr_moves(&t, o); if (k <= 0) break;
            wotr_apply(&t, o[0]);
        }
    }
    return 0;
}
/* The first question of the turn the view is in: the board of the view, the turn before ended in our engine, the cards
 * put back as the view has them. Returns 1 with the position in B->R.s. */
static int bot_turnstart(Bot *B, const J *v) {
    char err[200];
    if (bot_view(B, v, FP, err, sizeof err)) return 0;
    State *s = &B->R.s; uint8_t cw[NCARD]; memcpy(cw, s->card_where, NCARD);
    int turn = s->turn, must = s->hunt_fp_prev;
    if (B->box_turn == turn && B->box_prev > must) must = B->box_prev;
    for (int c = 0; c < NCARD; c++) if (s->card_where[c] == W_HAND) s->card_where[c] = W_DECK;      /* empty hands: the turn's draw then asks for no discard */
    s->hand_n[0] = s->hand_n[1] = 0;
    s->turn = (uint16_t)(turn > 0 ? turn - 1 : 0); s->hunt_fp = (uint8_t)must; s->hunt_sh = 0; s->fsp_acted = 1;
    memset(s->dice, 0, sizeof s->dice); s->dice[FP][F_MUSTER] = 1;
    static Move mv[MAXMOVES]; wotr_prune = 0;
    int n = wotr_moves(s, mv), k = -1;
    for (int i = 0; i < n; i++) if (mv[i].type == M_SKIP) k = i;
    if (k < 0) return 0;
    wotr_apply(s, mv[k]);
    if (s->winner || s->turn != turn) return 0;
    memcpy(s->card_where, cw, NCARD); s->hand_n[0] = s->hand_n[1] = 0;
    for (int c = 0; c < NCARD; c++) if (s->card_where[c] == W_HAND) s->hand_n[CARD_SIDE(c)]++;
    return 1;
}
static int fsp_dkind(int dk) { return dk == D_FSP_DECLARE || dk == D_GUIDE || dk == D_ENTER_MORDOR; }

/* Do our leaves ask the same question as the referee: every option of the referee is the first answer of a leaf? */
static int same_question(const Leaf *L, int nl, int kind, const J *opts) {
    int n = 0;
    for (const J *x = opts ? opts->child : NULL; x; x = x->next, n++) {
        int hit = 0;
        for (int i = 0; i < nl && !hit; i++) {
            if (L[i].nstep <= 0 || L[i].st[0].kind != kind) continue;
            char buf[260], *mem; snprintf(buf, sizeof buf, "{%s", L[i].st[0].text);
            const J *want = j_parse(buf, &mem); if (!want) continue;
            hit = opt_fits(x, want) >= 0; free(mem);
        }
        if (!hit) return 0;
    }
    return n > 0;
}
/* A question that follows our own move after something random (a card's question, a new Guide): the move is played again
 * from the position before it until the engine asks us the same question with the same options. */
static int bot_replay(Bot *B, Leaf *L, int kind, const J *opts) {
    if (!B->prev_ok || B->nprev < 1) return 0;
    static Move o[MAXMOVES];
    for (uint64_t seed = 1; seed <= 300; seed++) {
        State t = B->prev; t.rng = seed * 0xA0761D6478BD642FULL + B->rng;
        wotr_prune = 0;
        for (int k = 0; k < B->nprev && !t.winner; k++) wotr_apply(&t, B->prevmv[k]);
        for (int stp = 0; stp < 10 && !t.winner && t.dkind != D_ACTION; stp++) {
            if (t.dplayer == B->me) { B->R.s = t; int nl = build_leaves(&B->R, L); if (same_question(L, nl, kind, opts)) return 1; }
            int k = wotr_moves(&t, o); if (k <= 0) break;
            wotr_apply(&t, o[0]);
        }
    }
    return 0;
}

/* The Nazgul question. Our player thinks in whole plans (all Nazgul to an Army, one to the Fellowship); the protocol
 * moves one figure at a time. The plan is chosen once, and each question gets the next single flight toward it.
 * Returns the leaf, -1 if none fits, -2 if our player chose no plan (then the ordinary choice is made). */
static int naz_pick(Bot *B, Agent *ag, const Leaf *L, int nl) {
    const State *s = &B->R.s;
    if (!B->naz_plan) {
        State c = *s; float bv; int n;
        wotr_prune = 1; Move m = agent_choose(ag, &c, &bv, &n); wotr_prune = 0;
        if (m.type != M_NAZ_PLAN) return -2;
        State t = *s; wotr_apply(&t, m);
        memcpy(B->naz_tgt, t.naz, NREG); B->wk_tgt = t.minion_loc[M_WK]; B->mouth_tgt = t.minion_loc[M_MOUTH]; B->naz_plan = 1;
    }
    int from = -1, to = -1, d = 0;
    for (int r = 0; r < NREG; r++) { if (s->naz[r] > B->naz_tgt[r] && from < 0) from = r; if (s->naz[r] < B->naz_tgt[r] && to < 0) to = r; }
    if (from < 0 || to < 0) {
        from = to = -1;
        if (s->minion_loc[M_WK] < NREG && B->wk_tgt < NREG && s->minion_loc[M_WK] != B->wk_tgt) { from = s->minion_loc[M_WK]; to = B->wk_tgt; d = 1; }
        else if (s->minion_loc[M_MOUTH] < NREG && B->mouth_tgt < NREG && s->minion_loc[M_MOUTH] != B->mouth_tgt) { from = s->minion_loc[M_MOUTH]; to = B->mouth_tgt; d = 2; }
    }
    if (from >= 0) for (int i = 0; i < nl; i++) if (L[i].mv[0].type == M_NAZ_MOVE && L[i].mv[0].a == from && L[i].mv[0].b == to && L[i].mv[0].d == d && (d || L[i].mv[0].c == 1)) return i;
    for (int i = 0; i < nl; i++) if (L[i].mv[0].type != M_NAZ_MOVE) return i;        /* the plan is carried out (or its next flight is not on offer): end */
    return -1;
}

/* Takes leaf i as the move chosen: returns the option that is its first answer and keeps the rest to give; -1 if the
 * referee does not offer it (the same move paid with another die of that face is tried too). */
static int bot_take(Bot *B, const Leaf *L, int nl, int i, int kind, const J *opts) {
    int o;
    if (i < 0 || L[i].nstep <= 0 || L[i].st[0].kind != kind) return -1;
    if (find_option(opts, L[i].st[0].text) < 0)
        for (int j = 0; j < nl; j++) if (j != i && L[j].nmv == L[i].nmv && !memcmp(L[j].mv, L[i].mv, sizeof(Move) * L[i].nmv) && L[j].nstep > 0 && find_option(opts, L[j].st[0].text) >= 0) { i = j; break; }
    if ((o = find_option(opts, L[i].st[0].text)) < 0) return -1;
    memcpy(B->pend, L[i].st, sizeof(Step) * L[i].nstep); B->npend = L[i].nstep; B->ipend = 1;
    B->npmv = L[i].nmv; memcpy(B->pmv, L[i].mv, sizeof(Move) * L[i].nmv);
    if (B->ipend == B->npend) bot_applied(B);
    return o;
}
static int bot_decide(Bot *B, Agent *ag, const J *dec, const J *v) {
    static Leaf *L; if (!L) L = malloc(sizeof(Leaf) * MAXLEAF);
    const J *opts = j_get(dec, "options");
    int kind = kind_of(j_str(dec, "kind")), o;
    B->n_dec++;
    {   int t = j_int(v, "turn", 0), bx = j_int(j_get(j_get(v, "hunt"), "box"), "FREE_PEOPLES", 0);
        if (t != B->box_turn) { B->box_prev = t == B->box_turn + 1 ? B->box_now : -1; B->box_turn = t; B->box_now = 0; }
        if (bx > B->box_now) B->box_now = bx; }
    if (B->ipend < B->npend) {          /* an answer of the move already chosen */
        for (int k = B->ipend; k < B->npend; k++) {
            if (B->pend[k].kind != kind || (o = find_option(opts, B->pend[k].text)) < 0) continue;
            Step t = B->pend[k]; for (int j = k; j > B->ipend; j--) B->pend[j] = B->pend[j - 1]; B->pend[B->ipend++] = t;
            if (B->ipend == B->npend) bot_applied(B);
            B->n_chain++; return o;
        }
        /* the referee asks something else: if only the "which units" answers were left, the move is made as far as we are concerned */
        int only = B->ipend > 0;
        for (int k = B->ipend; k < B->npend; k++) only &= B->pend[k].kind == K_ARMY_SELECTION || B->pend[k].kind == K_JOIN_GROUP;
        if (only) bot_applied(B); else { fprintf(stderr, "bot: expected %s (%s), asked %s; plan dropped\n", KIND_KEY[B->pend[B->ipend].kind], B->pend[B->ipend].text, kind >= 0 ? KIND_KEY[kind] : "?"); B->npend = B->ipend = 0; B->valid = 0; B->n_nomatch++; }
    }
    if (kind == K_ACTION) {             /* a fresh picture from the view */
        char err[200];
        B->prev_ok = 0; B->naz_plan = 0;
        B->valid = !bot_view(B, v, B->me, err, sizeof err);
        if (!B->valid) { B->n_rebuild_fail++; fprintf(stderr, "bot: cannot rebuild the position (turn %d): %s\n", j_int(v, "turn", 0), err); }
    }
    if (B->valid && !B->R.s.winner && B->R.s.dplayer == B->me && (kind == K_ACTION || kind == K_ARMY_MOVE || kind == K_RECRUIT || kind == K_MOVE_COMPANIONS || kind == K_MOVE_MINIONS || kind == K_SEPARATE_COMPANIONS || kind == K_CARD_CHOICE)) {
        wotr_prune = 0;
        int nl = build_leaves(&B->R, L);
        if (kind == K_ACTION) {         /* do we list the same options as the referee? (its fingerprint tells) */
            static const char *seen[4096]; int ns = 0; uint64_t hsh = 0xcbf29ce484222325ULL; char tmp[260];
            for (int i = 0; i < nl && ns < 4096; i++) { int dup = 0; for (int j = 0; j < ns && !dup; j++) dup = !strcmp(seen[j], L[i].st[0].text); if (!dup) seen[ns++] = L[i].st[0].text; }
            snprintf(tmp, sizeof tmp, "%s\n%s\n%d\n", SIDE_KEY[B->me], KIND_KEY[kind], ns); hsh = fnv1a(hsh, tmp);
            for (int i = 0; i < ns; i++) { snprintf(tmp, sizeof tmp, "{\"o\":%d,%s\n", i, seen[i]); hsh = fnv1a(hsh, tmp); }
            snprintf(tmp, sizeof tmp, "%016llx", (unsigned long long)hsh);
            const char *fp = j_str(dec, "fingerprint"); B->n_fp_seen++; B->n_fp_same += fp && !strcmp(fp, tmp);
            if (fp && strcmp(fp, tmp) && getenv("WOTR_BOT_DEBUG")) fprintf(stderr, "bot: options differ at turn %d: we list %d, the referee %d\n", B->R.s.turn, ns, j_len(opts));
        }
        int i = !nl ? -1 : B->R.s.dkind == D_NAZ_MOVE ? naz_pick(B, ag, L, nl) : -2;
        if (i == -2) i = bot_pick(B, ag, L, nl);
        if ((o = bot_take(B, L, nl, i, kind, opts)) >= 0) { B->n_engine++; return o; }
        if (kind == K_ACTION && B->me == SH && !B->R.s.mouth_used) {       /* the Mouth of Sauron's die may be spent already this turn: the view does not say */
            B->R.s.mouth_used = 1; wotr_prune = 0;
            nl = build_leaves(&B->R, L); i = nl ? bot_pick(B, ag, L, nl) : -1;
            if ((o = bot_take(B, L, nl, i, kind, opts)) >= 0) { B->n_engine++; return o; }
        }
        B->n_nomatch++; B->valid = 0;
        if (i >= 0) fprintf(stderr, "bot: our move is not among the referee's options (%s: %s)\n", kind >= 0 ? KIND_KEY[kind] : "?", L[i].nstep ? L[i].st[0].text : "");
        else fprintf(stderr, "bot: our move is not among our own leaves (%s, engine question %d)\n", kind >= 0 ? KIND_KEY[kind] : "?", B->R.s.dkind);
    } else if (kind != K_ACTION) B->valid = 0;         /* something happened that our picture does not follow */
    {   /* a question inside a battle: set the engine down in it and let our player answer */
        int target = kind == K_COMBAT_CARD ? D_BATTLE_CARD : kind == K_CASUALTY ? D_CASUALTY : kind == K_FIELD_OR_SIEGE ? D_SIEGE_RETREAT : kind == K_CEASE_ATTACK ? D_CONTINUE
                   : kind == K_RETREAT ? D_RETREAT : kind == K_EXTEND_SIEGE ? D_SIEGE_EXTEND : -1;
        if (target >= 0 && j_len(opts) > 1 && !getenv("WOTR_BOT_NOBATTLE") && bot_battle(B, v, dec, target)) {
            wotr_prune = 0;
            int nl = build_leaves(&B->R, L), i = nl ? bot_pick(B, ag, L, nl) : -1;
            B->valid = 0;
            if (i >= 0 && L[i].nstep > 0 && (o = find_option(opts, L[i].st[0].text)) >= 0) { B->n_battle++; return o; }
            B->n_nomatch++;
            fprintf(stderr, "bot: battle question %s: our answer is not among the referee's options (%s)\n", KIND_KEY[kind], i >= 0 && L[i].nstep ? L[i].st[0].text : "none");
        } else if (target >= 0 && j_len(opts) > 1) B->n_battle_miss++;
    }
    if ((kind == K_HUNT_CASUALTY || kind == K_REVEAL_FELLOWSHIP) && j_len(opts) > 1 && bot_hunt(B, dec, kind == K_HUNT_CASUALTY ? D_HUNT_RESPONSE : D_REVEAL_DEST)) {
        wotr_prune = 0;
        int nl = build_leaves(&B->R, L), i = nl ? bot_pick(B, ag, L, nl) : -1;
        B->valid = 0;
        if (i >= 0 && L[i].nstep > 0 && (o = find_option(opts, L[i].st[0].text)) >= 0) { B->n_hunt++; return o; }
    }
    if ((kind == K_FELLOWSHIP_PHASE && B->me == FP) || (kind == K_HUNT_ALLOCATION && B->me == SH)) {
        static Move mv[MAXMOVES];
        /* First the engine step that asks exactly the referee's question (a referee that asks the phase step by step, as
           ours does); failing that, the first step where our player wants something the referee offers (one menu for the phase). */
        for (int strict = 1; strict >= 0 && j_len(opts) > 1; strict--) {
            if (!bot_turnstart(B, v)) break;
            State *s = &B->R.s;
            B->valid = 0;
            for (int stp = 0; stp < 5 && !s->winner; stp++) {
                wotr_prune = 0;
                if (s->dplayer == FP && fsp_dkind(s->dkind) && B->me == SH) {       /* the Free Peoples' phase is over in the view: pass it by */
                    int n = wotr_moves(s, mv), ch = 0;
                    for (int j = 0; j < n; j++) if (mv[j].a == (s->dkind == D_FSP_DECLARE ? 255 : s->dkind == D_GUIDE ? s->guide : 0)) ch = j;
                    wotr_apply(s, mv[ch]); continue;
                }
                if (!(s->dplayer == B->me && (B->me == FP ? fsp_dkind(s->dkind) : s->dkind == D_HUNT_ALLOC))) break;
                int nl = build_leaves(&B->R, L);
                if (strict && B->me == FP && !same_question(L, nl, kind, opts)) {     /* not this step: pass it by with "nothing" */
                    int dl = -1; for (int j = 0; j < nl; j++) if (strstr(L[j].st[0].text, "\"done\"")) dl = j;
                    if (dl < 0) break;
                    wotr_apply(s, L[dl].mv[0]); continue;
                }
                int i = nl ? bot_pick(B, ag, L, nl) : -1;
                if (i < 0 || L[i].nstep <= 0) break;
                if ((o = find_option(opts, L[i].st[0].text)) >= 0 && (strict || !strstr(L[i].st[0].text, "\"done\""))) { B->n_turn++; return o; }
                if (B->me == FP && strstr(L[i].st[0].text, "\"done\"")) { wotr_apply(s, L[i].mv[0]); continue; }     /* nothing to do at this step: on to the next */
                break;
            }
            if (B->me == SH) break;
        }
        if (B->me == FP && j_len(opts) > 1 && (o = find_kind(opts, "done")) >= 0) { B->n_turn++; return o; }
    }
    if ((kind == K_CARD_CHOICE || kind == K_CHOOSE_GUIDE || kind == K_SEPARATE_COMPANIONS || kind == K_GUIDE_ABILITY) && j_len(opts) > 1 && bot_replay(B, L, kind, opts)) {
        wotr_prune = 0;
        int nl = build_leaves(&B->R, L), i = nl ? bot_pick(B, ag, L, nl) : -1;
        B->valid = 0;
        if (i >= 0 && L[i].nstep > 0 && (o = find_option(opts, L[i].st[0].text)) >= 0) { B->n_hunt++; return o; }
    }
    if (kind == K_DISCARD_EXCESS_CARD && j_len(opts) > 1) {       /* the discard that leaves the position our network likes best */
        char err[200];
        if (!bot_view(B, v, B->me, err, sizeof err)) {
            int pos = 0, bo = -1; float bv = 0;
            B->valid = 0;
            for (const J *x = opts->child; x; x = x->next, pos++) {
                int c = card_of(j_str(x, "card")); if (c < 0 || B->R.s.card_where[c] != W_HAND) continue;
                State t = B->R.s; t.card_where[c] = W_DISCARD; t.hand_n[CARD_SIDE(c)]--;
                ag->has_root = 0;
                float val = agent_value(ag, &t); if (B->me == SH) val = -val;
                if (bo < 0 || val > bv) { bv = val; bo = opt_number(x, pos); }
            }
            if (bo >= 0) { B->n_turn++; return bo; }
        }
    }
    o = bot_rule(B, kind, dec, v);
    B->n_rule++; B->n_rule_kind[kind >= 0 && kind < NKIND ? kind : NKIND] += j_len(opts) > 1;
    return o;
}
static int cmd_bot(int argc, char **argv) {
    static Bot B; static Agent ag;
    Player *p = malloc(sizeof(Player));
    if (!p || player_load(p, argv[2])) { fprintf(stderr, "cannot load the player %s\n", argv[2]); return 1; }
    setenv("WOTR_W", "12:0,18:0", 0); setenv("WOTR_NUDGE", "0.3", 0); setenv("WOTR_HUMAN_SH", "0.1", 0); setenv("WOTR_HUMAN_FP", "0.1", 0);
    memset(&ag, 0, sizeof ag); memset(&B, 0, sizeof B);
    ag.net[FP] = &p->fp; ag.net[SH] = &p->sh; ag.samples = 3; ag.temp = 0.02f; ag.dpol[FP] = &p->pol[FP]; ag.dpol[SH] = &p->pol[SH];
    B.R.ext = -1; B.rng = 0x9E3779B97F4A7C15ULL; ag.rng = 12345;
    char *line, *mem, out[400];
    while ((line = read_line())) {
        const J *m = j_parse(line, &mem);
        if (!m) continue;
        const char *type = j_str(m, "type"); int id = j_int(m, "id", 0);
        if (!type) { free(mem); continue; }
        if (!strcmp(type, "hello")) {
            const char *tok = getenv("WOTR_BOT_TOKEN");
            if (tok) snprintf(out, sizeof out, "{\"type\":\"hello\",\"id\":%d,\"protocol\":\"wotr-bot/0.2\",\"name\":\"CWOTR\",\"version\":\"0.2\",\"events\":false,\"rulesets\":[\"base\"],\"token\":\"%s\"}", id, tok);
            else snprintf(out, sizeof out, "{\"type\":\"hello\",\"id\":%d,\"protocol\":\"wotr-bot/0.2\",\"name\":\"CWOTR\",\"version\":\"0.2\",\"events\":false,\"rulesets\":[\"base\"]}", id);
            send_line(out);
        } else if (!strcmp(type, "new_game")) {
            B.me = side_of(j_str(m, "side")); if (B.me < 0) B.me = FP;
            B.valid = 0; B.npend = B.ipend = 0;
            uint64_t sb = (uint64_t)j_int(m, "seed_bot", 1); ag.rng = sb * 31 + 7; B.rng = sb * 77 + 5;
            if (B.me == SH && !getenv("WOTR_BOT_NOLOOK")) { ag.depth = 3; ag.width = 4; ag.replies = 1; ag.narrow = 6; ag.taper = 0; } else ag.depth = 0;    /* our Shadow looks ahead; our Free Peoples play one step ahead */
            fprintf(stderr, "bot: new game %s, we play %s\n", j_str(m, "game") ? j_str(m, "game") : "?", B.me == FP ? "the Free Peoples" : "the Shadow");
            snprintf(out, sizeof out, "{\"type\":\"ready\",\"id\":%d}", id); send_line(out);
        } else if (!strcmp(type, "decide")) {
            const J *dec = j_get(m, "decision"), *v = j_get(m, "view");
            int o = dec ? bot_decide(&B, &ag, dec, v) : 0;
            const char *fp = j_str(dec, "fingerprint");
            if (fp && strlen(fp) < 40) snprintf(out, sizeof out, "{\"type\":\"choice\",\"id\":%d,\"o\":%d,\"fingerprint\":\"%s\"}", id, o, fp);
            else snprintf(out, sizeof out, "{\"type\":\"choice\",\"id\":%d,\"o\":%d}", id, o);
            send_line(out);
        } else if (!strcmp(type, "game_over")) {
            fprintf(stderr, "bot: game over, winner %s by %s on turn %d\n", j_str(m, "winner") ? j_str(m, "winner") : "none", j_str(m, "how") ? j_str(m, "how") : "?", j_int(m, "turn", 0));
            fprintf(stderr, "BOT decisions=%ld by_engine=%ld continuing_a_move=%ld in_battle_by_engine=%ld battle_not_reached=%ld hunt_by_engine=%ld turn_start_by_engine=%ld by_rule=%ld not_matched=%ld rebuild_failed=%ld action_questions=%ld same_options_as_referee=%ld\n",
                    B.n_dec, B.n_engine, B.n_chain, B.n_battle, B.n_battle_miss, B.n_hunt, B.n_turn, B.n_rule, B.n_nomatch, B.n_rebuild_fail, B.n_fp_seen, B.n_fp_same);
            fprintf(stderr, "RULES"); for (int k = 0; k <= NKIND; k++) if (B.n_rule_kind[k]) fprintf(stderr, " %s=%ld", k < NKIND ? KIND_KEY[k] : "unknown", B.n_rule_kind[k]); fprintf(stderr, "\n");
        } else if (!strcmp(type, "error") || !strcmp(type, "timeout")) {
            fprintf(stderr, "bot: the referee says %s\n", line);
        } else if (!strcmp(type, "quit")) { free(mem); break; }
        free(mem);
    }
    return 0;
}

int main(int argc, char **argv) {
    wotr_global_init();
    feat_init();
    names_init();
    if (argc >= 3 && !strcmp(argv[1], "names")) return test_json() | test_names(argv[2]);
    if (argc >= 5 && !strcmp(argv[1], "referee")) return cmd_referee(argc, argv, 0);
    if (argc >= 5 && !strcmp(argv[1], "play")) {       /* the referee with the settings our players were measured at (anything already set in the environment stays) */
        setenv("WOTR_W", "12:0,18:0", 0); setenv("WOTR_NUDGE", "0.3", 0); setenv("WOTR_HUMAN_SH", "0.1", 0); setenv("WOTR_HUMAN_FP", "0.1", 0);
        if (!strcmp(argv[3], "fp")) setenv("WOTR_LOOKAHEAD", "1", 0);      /* our Shadow looks ahead; our Free Peoples play one step ahead */
        return cmd_referee(argc, argv, 0);
    }
    if (argc >= 5 && !strcmp(argv[1], "mock")) return cmd_referee(argc, argv, 1);
    if (argc >= 4 && !strcmp(argv[1], "replay")) return cmd_replay(argc, argv);
    if (argc >= 3 && !strcmp(argv[1], "bot")) return cmd_bot(argc, argv);
    fprintf(stderr, "usage: bridge names <vocabulary.json>\n       bridge bot <player>   (our player as a protocol bot on stdin/stdout)\n       bridge play <player> <fp|sh: the outside bot's side> <seed> [clock ms=10000]   (referee, with the measured settings)\n       bridge referee <player|prior> <fp|sh: the outside bot's side> <seed> [clock ms=10000] [sims]\n       bridge mock <player|prior> <games> <seed>\n       bridge replay <referee log> <out file> [outside|ours|all]\n");
    return 2;
}
