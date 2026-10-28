#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <stddef.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "defines.h"
#include "utils.h"
#include "i18n.h"
#include "nekobit.h"

// A session shorter than this was a launch that failed or was backed out of
#define NB_MIN_SESSION 10
// and anything past this is a device left running, not play
#define NB_MAX_SESSION (12 * 3600)
// Unlock dates and file times within this many seconds of the session count
#define NB_WINDOW_SLACK 5
#define NB_MAX_FRESH 64 // unlocks credited per game per session

static NB_Save S;

static NB_Event events[16];
static int event_head = 0;
static int event_count = 0;

///////////////////////////////////////
// small helpers

static uint32_t rng = 0x9e3779b9;

void NB_seed(uint32_t seed) {
    rng = seed ? seed : 0x9e3779b9;
}
static uint32_t nb_rand(void) { // xorshift32
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static int nb_range(int lo, int hi) { // inclusive
    return lo + (int)(nb_rand() % (uint32_t)(hi - lo + 1));
}

// strncpy that always terminates and never cuts a UTF-8 sequence in half
static void nb_copy(char* dst, const char* src, size_t size) {
    size_t n = strlen(src);
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

// Paths are kept relative to SDCARD_PATH, like recent.txt
static const char* nb_rel(const char* path) {
    size_t len = strlen(SDCARD_PATH);
    if (!strncmp(path, SDCARD_PATH, len) && path[len] == '/') return path + len;
    return path;
}
static void nb_abs(const char* rel, char* out, size_t size) {
    snprintf(out, size, "%s%s", SDCARD_PATH, rel);
}

// path is parent itself or something inside it
static int nb_pathUnder(const char* parent, const char* path) {
    size_t len = strlen(parent);
    return len && !strncmp(parent, path, len) && (path[len] == '\0' || path[len] == '/');
}

// minui collates "Name (TAG)" folders with siblings that share "Name (" (see
// getEntries), so a system is everything that shares that prefix.
static int nb_inSystem(const char* system, const char* path) {
    char collated[NB_PATH_LEN];
    nb_copy(collated, system, sizeof(collated));
    char* folder = strrchr(collated, '/');
    char* paren = folder ? strrchr(folder, '(') : NULL;
    if (!paren) return nb_pathUnder(system, path);
    paren[1] = '\0';
    return !strncasecmp(collated, path, strlen(collated));
}

static void nb_displayName(const char* path, char* out, size_t size) {
    char name[MAX_PATH];
    getDisplayName(path, name);
    char* trimmed = name;
    trimSortingMeta(&trimmed);
    nb_copy(out, trimmed, size);
}

static void nb_tagOf(const char* path, char* out, size_t size) {
    char tag[MAX_PATH];
    getEmuName(path, tag);
    nb_copy(out, tag, size);
}

static char* nb_readAll(const char* path) { // malloc'd, NUL terminated
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > 8 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    char* buf = malloc(size + 1);
    if (buf) {
        size_t got = fread(buf, 1, size, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static void nb_event(int kind, int a, int b, const char* text) {
    if (kind == NB_EV_LEVEL) { // a run of level-ups is one banner, with the level it ended on
        for (int i = 0; i < event_count; i++) {
            NB_Event* ev = &events[(event_head + i) % (sizeof(events) / sizeof(events[0]))];
            if (ev->kind != NB_EV_LEVEL) continue;
            ev->a = a;
            return;
        }
    }
    if (event_count == (int)(sizeof(events) / sizeof(events[0]))) return; // the player won't read more
    NB_Event* ev = &events[(event_head + event_count++) % (sizeof(events) / sizeof(events[0]))];
    ev->kind = kind;
    ev->a = a;
    ev->b = b;
    nb_copy(ev->text, text ? text : "", sizeof(ev->text));
}

int NB_popEvent(NB_Event* out) {
    if (!event_count) return 0;
    *out = events[event_head];
    event_head = (event_head + 1) % (sizeof(events) / sizeof(events[0]));
    event_count--;
    return 1;
}

// "Oct 27 2026" -> epoch: no clock this side of the build can be right
uint32_t NB_buildEpoch(void) {
    static uint32_t epoch = 0;
    if (epoch) return epoch;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* date = __DATE__;
    char mon[4] = {date[0], date[1], date[2], 0};
    const char* hit = strstr(months, mon);
    int m = hit ? (int)(hit - months) / 3 + 1 : 1;
    int d = atoi(date + 4);
    int y = atoi(date + 7);
    // days from civil (Howard Hinnant)
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    epoch = (uint32_t)((era * 146097 + doe - 719468) * 86400u);
    return epoch;
}

// The wall clock, which tests can set (NB_TEST builds only)
#ifdef NB_TEST
static uint32_t test_clock = 0;
void NB_setClock(uint32_t now) {
    test_clock = now;
}
#endif
static uint32_t nb_now(void) {
#ifdef NB_TEST
    if (test_clock) return test_clock;
#endif
    return (uint32_t)time(NULL);
}

static int nb_wallOk(uint32_t now) {
    return now >= NB_buildEpoch();
}

///////////////////////////////////////
// state

static void nb_fresh(void) {
    memset(&S, 0, sizeof(S));
    S.level = 1;
    S.xp_next = NB_xpToNext(1);
    S.mood = NB_MOOD_HAPPY;
    S.shown_level = S.level;
}

#define NB_CRC_OFFSET (offsetof(NB_Save, crc) + sizeof(uint32_t))

// `dirty` = the card is now behind. Where NekoBit happens to be sitting isn't
// worth a card write (it's rolled again next boot), so that alone is quiet.
static void nb_storeAs(int dirty) {
    S.magic = NB_SAVE_MAGIC;
    S.version = NB_SAVE_VERSION;
    S.size = sizeof(S);
    S.crc = NB_crc32((uint8_t*)&S + NB_CRC_OFFSET, sizeof(S) - NB_CRC_OFFSET);
    mkdir(NB_TMP_DIR, 0755);
    NB_writeFile(NB_TMP_STATE, &S, sizeof(S), 0); // tmpfs: RAM, never the card
    if (!dirty) return;
    int fd = open(NB_TMP_DIRTY, O_WRONLY | O_CREAT, 0644);
    if (fd >= 0) close(fd);
}
static void nb_store(void) {
    nb_storeAs(1);
}

static int nb_load(const char* path) {
    static NB_Save tmp;
    if (NB_readFile(path, &tmp, sizeof(tmp)) != (int)sizeof(tmp)) return 0;
    if (tmp.magic != NB_SAVE_MAGIC || tmp.version != NB_SAVE_VERSION || tmp.size != sizeof(tmp)) return 0;
    if (tmp.crc != NB_crc32((uint8_t*)&tmp + NB_CRC_OFFSET, sizeof(tmp) - NB_CRC_OFFSET)) return 0;
    S = tmp;
    return 1;
}

const NB_Save* NB_state(void) {
    return &S;
}

const char* NB_face(int mood) {
    static const char* faces[NB_MOOD_COUNT] = {
        "(^-^)", // happy
        "(^o^)", // excited
        "(-_-)", // neutral
        "(;_;)", // sad
        "(>_<)", // angry
        "(O_O)", // surprised
    };
    return (mood >= 0 && mood < NB_MOOD_COUNT) ? faces[mood] : faces[0];
}

const char* NB_sleepyFace(void) {
    return "(-.-)";
}

int NB_moodString(int mood) {
    static const int ids[NB_MOOD_COUNT] = {
        STR_NB_MOOD_HAPPY, STR_NB_MOOD_EXCITED, STR_NB_MOOD_NEUTRAL,
        STR_NB_MOOD_SAD, STR_NB_MOOD_ANGRY, STR_NB_MOOD_SURPRISED,
    };
    return ids[(mood >= 0 && mood < NB_MOOD_COUNT) ? mood : 0];
}

///////////////////////////////////////
// mood and xp

// A good thing happened (BitPal's finalize_mission): moods climb one step
static void nb_moodUp(int allow_excited) {
    switch (S.mood) {
    case NB_MOOD_SAD:
    case NB_MOOD_ANGRY: S.mood = NB_MOOD_NEUTRAL; break;
    case NB_MOOD_NEUTRAL:
    case NB_MOOD_SURPRISED: S.mood = NB_MOOD_HAPPY; break;
    case NB_MOOD_HAPPY:
        if (allow_excited && nb_range(0, 99) < 40) S.mood = NB_MOOD_EXCITED;
        break;
    }
}

static int leveled = 0; // a level went up since NB_init: worth a card write

// The level curve, with no cap: leaving level 1 costs 100 XP, and each level
// after costs 8% more than the one before up to level 30, then 16% more for
// ever - compound growth, so the late levels are a lifelong wall. Worked in
// thousandths of an XP so the rounding doesn't drift, shown rounded to tens,
// and held at NB_XP_MAX so it can't overflow (level ~150 and up).
#define NB_XP_MAX 2000000000u
uint32_t NB_xpToNext(uint32_t level) {
    uint64_t milli = 100 * 1000;
    for (uint32_t l = 2; l <= level && milli < NB_XP_MAX * 1000ull; l++)
        milli = milli * (l <= 30 ? 108 : 116) / 100;
    uint64_t xp = (milli / 1000 + 5) / 10 * 10;
    return xp < NB_XP_MAX ? (uint32_t)xp : NB_XP_MAX;
}

// Rewards grow with the player so the late levels stay in reach: +4% a level,
// x2.5 at most (level 38 on). Play missions, achievements and their missions
// alike, so hunting achievements never falls behind just playing.
int NB_levelScale(uint32_t level) {
    return level >= 38 ? 250 : 100 + 4 * (int)(level - 1);
}
static int nb_scaled(int xp) {
    return (int)((int64_t)xp * NB_levelScale(S.level) / 100);
}

// A mastery (100% of an RA set). The Golden Guarantee: exactly one whole level
// for any of the first 6 sets mastered, while under level 20. After either,
// for good: 3 standard missions at the player's level, so a short easy set
// can't be farmed for levels. `mastered` = sets mastered before this one.
int NB_masteryXp(uint32_t level, uint32_t mastered, uint32_t xp_to_next) {
    if (mastered < 6 && level < 20) return (int)xp_to_next;
    return 3 * NB_MISSION_XP * NB_levelScale(level) / 100;
}

static void nb_addXp(int xp) {
    if (xp <= 0) return;
    S.xp += xp;
    while (S.xp >= S.xp_next) {
        S.xp -= S.xp_next;
        S.level++;
        S.xp_next = NB_xpToNext(S.level);
        S.mood = NB_MOOD_EXCITED;
        nb_event(NB_EV_LEVEL, S.level, 0, NULL);
        leveled = 1;
    }
}

// The first achievements in a set are easy and the last ones take mastery,
// so an unlock is worth more the further into its set it is: x1 for the
// first, rising to x3 for the last. RA points (the designer's own difficulty
// call, 0-100, capped at 50) set the base, and rarity (the % of players who
// have it, from the downloaded set) adds up to 50% for the rare ones.
int NB_xpForUnlock(int points, int rarity_x100, int before, int total) {
    if (points < 0) points = 0;
    if (points > 50) points = 50;
    int base = 10 + 2 * points;
    int progress = 100;
    if (total > 0) {
        if (before < 0) before = 0;
        if (before > total - 1) before = total - 1;
        progress = 100 + 200 * before / (total > 1 ? total - 1 : 1);
    }
    int rare = 100;
    if (rarity_x100 > 0) {
        if (rarity_x100 < 500) rare = 150;        // under 5% of players
        else if (rarity_x100 < 1500) rare = 130;
        else if (rarity_x100 < 3000) rare = 115;
    }
    return base * progress / 100 * rare / 100;
}

// The marks of a set crossed going from `before` to `after` unlocks: bit 0
// = 25% .. bit 3 = 100%. 25/50/75% pay NB_MILESTONE_XP (at level 1), 100%
// is a mastery (NB_masteryXp).
static const int NB_MILESTONE_XP[3] = {50, 100, 200};
int NB_milestonesCrossed(int before, int after, int total) {
    if (total <= 0 || after <= before) return 0;
    int crossed = 0;
    for (int i = 0; i < 4; i++) {
        int pct = 25 * (i + 1);
        if (before * 100 < pct * total && after * 100 >= pct * total) crossed |= 1 << i;
    }
    return crossed;
}

///////////////////////////////////////
// the paid ledger
//
// Every reward paid, ever: each achievement by its RA id (unique across all
// of RetroAchievements, so another dump of the same game shares it) and each
// 25/50/75/100% of a set by the set's RA game id. Wiping a game's progress in
// minarch, or deleting its file, makes minarch hand out the same unlocks
// again; this is how NekoBit knows it has paid for them.
//
// A Bloom filter, so it never grows: NB_PAID_BITS bits, NB_PAID_K of them
// set per key. It can only err one way - taking something new for paid
// (~0.02% of the time at 2,000 unlocks, ~0.5% at 5,000) - so it never pays
// twice.
#define NB_PAID_K 4
#define NB_KEY_ACH(id)        ((uint64_t)(id))
#define NB_KEY_SET(game, pct) ((1ull << 63) | (((game) & ((1ull << 55) - 1)) << 8) | (uint64_t)(pct))

static uint64_t nb_mix(uint64_t x) { // splitmix64
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

int NB_paidOnce(uint64_t key) {
    uint64_t h = nb_mix(key);
    uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32) | 1;
    int seen = 1;
    for (uint32_t i = 0; i < NB_PAID_K; i++) {
        uint32_t bit = (h1 + i * h2) % NB_PAID_BITS;
        uint8_t mask = 1u << (bit & 7);
        if (!(S.paid[bit >> 3] & mask)) {
            seen = 0;
            S.paid[bit >> 3] |= mask;
        }
    }
    return seen;
}

///////////////////////////////////////
// missions

static void nb_describeParts(int type, int flavor, int target, const char* title, char* out, size_t size) {
    switch (type) {
    case NB_MIS_PLAY_GAME:
        if (flavor == NB_FLAVOR_REDISCOVER) snprintf(out, size, T(STR_NB_M_REDISCOVER), title, target);
        else if (flavor == NB_FLAVOR_PLAYMORE) snprintf(out, size, T(STR_NB_M_PLAYMORE), title, target);
        else snprintf(out, size, T(STR_NB_M_GAME), title, target);
        break;
    case NB_MIS_PLAY_SYSTEM: snprintf(out, size, T(STR_NB_M_SYSTEM), title, target); break;
    case NB_MIS_PLAY_SURPRISE: snprintf(out, size, T(STR_NB_M_SURPRISE), target); break;
    case NB_MIS_PLAY_ANY: snprintf(out, size, T(STR_NB_M_ANY), target); break;
    case NB_MIS_ACH_ANY:
        if (target == 1) snprintf(out, size, "%s", T(STR_NB_M_ACH_ANY_1));
        else snprintf(out, size, T(STR_NB_M_ACH_ANY_N), target);
        break;
    case NB_MIS_ACH_GAME:
        if (flavor == NB_FLAVOR_MASTERY) {
            if (target == 1) snprintf(out, size, T(STR_NB_M_MASTERY_1), title);
            else snprintf(out, size, T(STR_NB_M_MASTERY_N), target, title);
        }
        else {
            if (target == 1) snprintf(out, size, T(STR_NB_M_ACH_GAME_1), title);
            else snprintf(out, size, T(STR_NB_M_ACH_GAME_N), target, title);
        }
        break;
    case NB_MIS_ACH_SYSTEM: snprintf(out, size, T(STR_NB_M_ACH_SYSTEM), title); break;
    default: out[0] = '\0'; break;
    }
}

void NB_describe(const NB_Mission* m, char* out, size_t size) {
    nb_describeParts(m->type, m->flavor, m->target, m->title, out, size);
}

void NB_label(const NB_Mission* m, char* out, size_t size) {
    switch (m->type) {
    case NB_MIS_PLAY_GAME:
    case NB_MIS_PLAY_SYSTEM:
    case NB_MIS_ACH_GAME:
    case NB_MIS_ACH_SYSTEM: snprintf(out, size, "%s", m->title); break;
    case NB_MIS_PLAY_ANY:
    case NB_MIS_ACH_ANY: snprintf(out, size, "%s", T(STR_NB_L_ANY)); break;
    case NB_MIS_PLAY_SURPRISE: snprintf(out, size, "%s", T(STR_NB_L_SURPRISE)); break;
    default: out[0] = '\0'; break;
    }
}

void NB_formatTime(uint32_t secs, char* out, size_t size) {
    if (secs >= 3600) snprintf(out, size, T(STR_NB_TIME_HM), (int)(secs / 3600), (int)(secs % 3600 / 60));
    else if (secs >= 60) snprintf(out, size, T(STR_NB_TIME_MS), (int)(secs / 60), (int)(secs % 60));
    else snprintf(out, size, T(STR_NB_TIME_S), (int)secs);
}

static int nb_isPlay(int type) {
    return type >= NB_MIS_PLAY_GAME && type <= NB_MIS_PLAY_ANY;
}

static void nb_complete(int slot) {
    NB_Mission* m = &S.missions[slot];
    NB_History* h = &S.history[S.history_count % NB_MAX_HISTORY];
    memset(h, 0, sizeof(*h));
    h->type = m->type;
    h->flavor = m->flavor;
    h->target = m->target;
    h->xp = m->xp;
    h->done_at = S.play_clock;
    nb_copy(h->title, m->title, sizeof(h->title));
    S.history_count++;
    S.missions_completed++;

    int xp = m->xp;
    memset(m, 0, sizeof(*m));
    nb_event(NB_EV_MISSION, xp, 0, NULL);
    nb_moodUp(1);
    nb_addXp(xp);
}

static void nb_checkDone(int slot) {
    NB_Mission* m = &S.missions[slot];
    uint32_t goal = nb_isPlay(m->type) ? (uint32_t)m->target * 60 : m->target;
    if (m->type != NB_MIS_NONE && m->progress >= goal) nb_complete(slot);
}

int NB_freeSlots(void) {
    int n = 0;
    for (int i = 0; i < NB_MAX_MISSIONS; i++) n += S.missions[i].type == NB_MIS_NONE;
    return n;
}

static int nb_hasMission(int type, const char* path, const char* hash) {
    for (int i = 0; i < NB_MAX_MISSIONS; i++) {
        const NB_Mission* m = &S.missions[i];
        if (m->type != type) continue;
        if (hash && hash[0] && !strcmp(m->hash, hash)) return 1;
        if (path && path[0] && !strcmp(m->path, path)) return 1;
        if ((!path || !path[0]) && (!hash || !hash[0])) return 1;
    }
    return 0;
}

///////////////////////////////////////
// games NekoBit has seen

static NB_Game* nb_findGame(const char* path) {
    for (int i = 0; i < NB_MAX_GAMES; i++)
        if (S.games[i].path[0] && !strcmp(S.games[i].path, path)) return &S.games[i];
    return NULL;
}

static NB_Game* nb_game(const char* path) {
    NB_Game* g = nb_findGame(path);
    if (g) return g;
    g = &S.games[0];
    for (int i = 0; i < NB_MAX_GAMES; i++) { // a free slot, else the least recent
        if (!S.games[i].path[0]) {
            g = &S.games[i];
            break;
        }
        if (S.games[i].last_session < g->last_session) g = &S.games[i];
    }
    memset(g, 0, sizeof(*g));
    nb_copy(g->path, path, sizeof(g->path));
    return g;
}

///////////////////////////////////////
// achievements (minarch's ra_offline.c files)
//
//   progress/<TAG>/<md5>_progress.json
//     {"hash":"..","set":"..","total":23,"unlocked":[{"id":N,"date":N},..],"check":".."}
//   data/<TAG>/<md5>.json (or data/<md5>.json): the RA "patch" response, for
//     the game title and each achievement's Points and Rarity

typedef struct NB_Unlocks {
    char hash[33];
    char tag[32];
    char path[NB_PATH_LEN]; // minarch's record of where it's played from, "" = none
    int total;
    int unlocked;
    int fresh; // unlocked inside the session
    uint32_t ids[NB_MAX_FRESH];
} NB_Unlocks;

static const char* nb_jsonKey(const char* from, const char* to, const char* key) {
    const char* at = strstr(from, key);
    if (!at || (to && at >= to)) return NULL;
    at = strchr(at + strlen(key), ':');
    if (!at || (to && at >= to)) return NULL;
    at++;
    while (*at == ' ') at++;
    return at;
}

static int nb_jsonString(const char* at, char* out, size_t size) {
    if (!at || *at != '"') return 0;
    at++;
    size_t n = 0;
    while (*at && *at != '"' && n + 1 < size) {
        if (*at == '\\' && at[1]) {
            at++;
            if (*at == 'u') { // \uXXXX, BMP only
                unsigned cp = 0;
                for (int i = 1; i <= 4 && isxdigit((unsigned char)at[i]); i++)
                    cp = cp * 16 + (isdigit((unsigned char)at[i]) ? at[i] - '0' : (tolower((unsigned char)at[i]) - 'a' + 10));
                at += 5;
                if (n + 4 >= size) break; // no room for the sequence
                if (cp < 0x80) out[n++] = cp;
                else if (cp < 0x800) {
                    out[n++] = 0xc0 | (cp >> 6);
                    out[n++] = 0x80 | (cp & 0x3f);
                }
                else {
                    out[n++] = 0xe0 | (cp >> 12);
                    out[n++] = 0x80 | ((cp >> 6) & 0x3f);
                    out[n++] = 0x80 | (cp & 0x3f);
                }
                continue;
            }
            out[n++] = *at == 'n' ? ' ' : *at; // \" \\ \/ as themselves
            at++;
            continue;
        }
        out[n++] = *at++;
    }
    if (*at && *at != '"') { // cut short: don't leave half a UTF-8 sequence
        size_t keep = n;
        while (keep > 0 && ((unsigned char)out[keep - 1] & 0xc0) == 0x80) keep--;
        if (keep > 0 && ((unsigned char)out[keep - 1] & 0xc0) == 0xc0) n = keep - 1;
    }
    out[n] = '\0';
    return 1;
}

// minarch's tamper check (ra_progressChecksum): FNV-1a over the hash, the
// set's fingerprint, the count, and every {id, date} record in file order
static uint32_t nb_fnv(uint32_t sum, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    while (size--) sum = (sum ^ *p++) * 16777619u;
    return sum;
}

// the {"id":N,"date":N} record at or after `at`, inside the "unlocked" list:
// past it, or NULL when the list is done
static const char* nb_nextUnlock(const char* at, uint32_t* id, uint32_t* date) {
    while (at && *at && *at != ']') {
        if (*at != '{') {
            at++;
            continue;
        }
        const char* close = strchr(at, '}');
        if (!close) return NULL;
        const char* i = nb_jsonKey(at, close, "\"id\"");
        const char* d = nb_jsonKey(at, close, "\"date\"");
        if (i && isdigit((unsigned char)*i)) {
            *id = (uint32_t)strtoul(i, NULL, 10);
            *date = d ? (uint32_t)strtoul(d, NULL, 10) : 0;
            return close + 1;
        }
        at = close + 1;
    }
    return NULL;
}

// 0 for a file that isn't minarch's as it wrote it: unreadable, or edited by
// hand (its checksum fails), so none of it is believed
static int nb_parseProgress(const char* json, uint32_t lo, uint32_t hi, NB_Unlocks* u) {
    memset(u, 0, sizeof(*u));
    char field[16];
    if (!nb_jsonString(nb_jsonKey(json, NULL, "\"hash\""), u->hash, sizeof(u->hash))) return 0;
    if (!nb_jsonString(nb_jsonKey(json, NULL, "\"set\""), field, sizeof(field))) return 0;
    uint32_t set = (uint32_t)strtoul(field, NULL, 16);
    if (!nb_jsonString(nb_jsonKey(json, NULL, "\"check\""), field, sizeof(field))) return 0;
    uint32_t check = (uint32_t)strtoul(field, NULL, 16);
    if (!nb_jsonString(nb_jsonKey(json, NULL, "\"path\""), u->path, sizeof(u->path))) u->path[0] = '\0';
    const char* at = nb_jsonKey(json, NULL, "\"total\"");
    u->total = at ? atoi(at) : 0;
    const char* list = strstr(json, "\"unlocked\"");
    if (list) list = strchr(list, '[');

    uint32_t id, date, count = 0;
    for (at = list; (at = nb_nextUnlock(at, &id, &date));) count++; // the count goes in first
    uint32_t sum = nb_fnv(2166136261u, u->hash, strlen(u->hash));
    sum = nb_fnv(sum, &set, sizeof(set));
    sum = nb_fnv(sum, &count, sizeof(count));
    for (at = list; (at = nb_nextUnlock(at, &id, &date));) {
        uint32_t record[2] = {id, date};
        sum = nb_fnv(sum, record, sizeof(record));
        if (date && date >= lo && date <= hi && u->fresh < NB_MAX_FRESH) u->ids[u->fresh++] = id;
    }
    if (sum != check) return 0;
    u->unlocked = (int)count;
    return 1;
}

static char* nb_readSet(const char* tag, const char* hash) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/data/%s/%s.json", RETROACHIVEMENTS_PATH, tag, hash);
    char* json = nb_readAll(path);
    if (json) return json;
    snprintf(path, sizeof(path), "%s/data/%s.json", RETROACHIVEMENTS_PATH, hash);
    return nb_readAll(path);
}

// The set's RA game id (PatchData's "ID", the same for every dump of the
// game), or a key from the hash if the set can't say
static uint64_t nb_gameKey(const char* set, const char* hash) {
    const char* at = set ? nb_jsonKey(set, NULL, "\"ID\"") : NULL;
    if (at && isdigit((unsigned char)*at)) return strtoull(at, NULL, 10);
    return (1ull << 54) | nb_fnv(2166136261u, hash, strlen(hash));
}

// the game's title is PatchData's first "Title", before the achievements'
static int nb_setTitle(const char* json, char* out, size_t size) {
    return json && nb_jsonString(nb_jsonKey(json, NULL, "\"Title\""), out, size) && out[0];
}

static void nb_achInfo(const char* json, uint32_t id, int* points, int* rarity_x100) {
    *points = 5; // the most common value, if the set can't tell us
    *rarity_x100 = 0;
    if (!json) return;
    char needle[32];
    int len = snprintf(needle, sizeof(needle), "\"ID\":%u", id);
    const char* at = json;
    while ((at = strstr(at, needle))) {
        if (!isdigit((unsigned char)at[len])) break;
        at += len;
    }
    if (!at) return;
    const char* end = strstr(at + len, "\"ID\":"); // the next achievement
    const char* p = nb_jsonKey(at, end, "\"Points\"");
    if (p) *points = atoi(p);
    const char* r = nb_jsonKey(at, end, "\"Rarity\"");
    if (r) *rarity_x100 = (int)(strtod(r, NULL) * 100 + 0.5);
}

// Which RA sets got new unlocks during the session. minarch rewrites the
// progress file on every unlock (ra_saveProgress), which also changes the
// folder's mtime, so an unchanged folder costs one stat. The session's window
// on the wall clock is anchored on its end and sized by the monotonic
// duration: it is right even if the clock read 1970 the whole time, as long
// as nobody changed it mid-game.
static int nb_findUnlocks(const NB_Session* s, uint32_t elapsed, uint32_t wall_end, NB_Unlocks* out, int max) {
    if (!s->tag[0]) return 0;
    char dir_path[MAX_PATH];
    snprintf(dir_path, sizeof(dir_path), "%s/progress/%s", RETROACHIVEMENTS_PATH, s->tag);
    struct stat st;
    if (stat(dir_path, &st) || (uint32_t)st.st_mtime == s->ra_dir_mtime) return 0;

    uint32_t start = wall_end > elapsed ? wall_end - elapsed : 0;
    uint32_t lo = start > NB_WINDOW_SLACK ? start - NB_WINDOW_SLACK : 0;
    uint32_t hi = wall_end + NB_WINDOW_SLACK;

    int count = 0;
    DIR* dh = opendir(dir_path);
    if (!dh) return 0;
    struct dirent* dp;
    while ((dp = readdir(dh)) && count < max) {
        if (!suffixMatch("_progress.json", dp->d_name)) continue;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir_path, dp->d_name);
        if (stat(path, &st)) continue;
        uint32_t mtime = (uint32_t)st.st_mtime;
        if (mtime < lo || mtime > hi) continue; // not written by this session
        char* json = nb_readAll(path);
        if (!json) continue;
        if (nb_parseProgress(json, lo, hi, &out[count]) && out[count].fresh) {
            nb_copy(out[count].tag, s->tag, sizeof(out[count].tag));
            count++;
        }
        free(json);
    }
    closedir(dh);
    return count;
}

static void nb_creditUnlocks(NB_Unlocks* u, const char* fallback_title) {
    char* set = nb_readSet(u->tag, u->hash);
    char title[NB_TITLE_LEN];
    if (!nb_setTitle(set, title, sizeof(title))) nb_copy(title, fallback_title, sizeof(title));

    // only what has never been paid for: a reset hands out the same ids again
    int before = u->unlocked - u->fresh;
    if (before < 0) before = 0;
    int fresh = 0, xp = 0;
    for (int i = 0; i < u->fresh; i++) {
        if (NB_paidOnce(NB_KEY_ACH(u->ids[i]))) continue;
        int points, rarity;
        nb_achInfo(set, u->ids[i], &points, &rarity);
        xp += NB_xpForUnlock(points, rarity, before + i, u->total);
        fresh++;
    }
    uint64_t game = nb_gameKey(set, u->hash);
    free(set);

    if (fresh) {
        xp = nb_scaled(xp);
        S.achievements += fresh;
        nb_event(NB_EV_ACH, fresh, xp, title);
        nb_moodUp(0);
        nb_addXp(xp);
    }

    // each mark of a set pays once in a lifetime, however often it's crossed.
    // A first mastery pays alone: the quarters it crossed with it are only
    // marked, as they always were.
    int crossed = NB_milestonesCrossed(before, u->unlocked, u->total), unpaid = 0;
    for (int i = 0; i < 4; i++)
        if ((crossed >> i & 1) && !NB_paidOnce(NB_KEY_SET(game, 25 * (i + 1)))) unpaid |= 1 << i;
    if (unpaid & 8) { // with what the unlocks paid already in
        int mastery = NB_masteryXp(S.level, S.masteries, S.xp_next);
        S.masteries++;
        nb_event(NB_EV_MASTERY, 100, mastery, title);
        S.mood = NB_MOOD_EXCITED;
        nb_addXp(mastery);
    }
    else if (unpaid) {
        int bonus = 0, percent = 0;
        for (int i = 0; i < 3; i++) {
            if (!(unpaid >> i & 1)) continue;
            bonus += NB_MILESTONE_XP[i];
            percent = 25 * (i + 1);
        }
        bonus = nb_scaled(bonus);
        nb_event(NB_EV_MILESTONE, percent, bonus, title);
        nb_moodUp(1);
        nb_addXp(bonus);
    }

    for (int i = 0; i < NB_MAX_MISSIONS; i++) {
        NB_Mission* m = &S.missions[i];
        int hit = m->type == NB_MIS_ACH_ANY ||
                  (m->type == NB_MIS_ACH_GAME && !strcmp(m->hash, u->hash)) ||
                  (m->type == NB_MIS_ACH_SYSTEM && !strcmp(m->tag, u->tag));
        if (!hit) continue;
        m->progress += fresh;
        if (m->type == NB_MIS_ACH_GAME) {
            m->ra_unlocked = u->unlocked;
            if (u->path[0]) nb_copy(m->path, u->path, sizeof(m->path)); // where it's played from now
        }
        nb_checkDone(i);
    }
}

///////////////////////////////////////
// sessions

void NB_beginSession(const char* rom, const char* game, const char* tag, int flags) {
    NB_Session s;
    memset(&s, 0, sizeof(s));
    s.magic = NB_SESSION_MAGIC;
    s.version = NB_SESSION_VERSION;
    s.mono_start_ms = NB_monoMs();
    s.wall_start = nb_now();
    s.flags = flags;
    nb_copy(s.rom, rom, sizeof(s.rom));
    nb_copy(s.game, game && game[0] ? game : rom, sizeof(s.game));
    nb_copy(s.tag, tag ? tag : "", sizeof(s.tag));
    if (s.tag[0]) {
        char dir_path[MAX_PATH];
        struct stat st;
        snprintf(dir_path, sizeof(dir_path), "%s/progress/%s", RETROACHIVEMENTS_PATH, s.tag);
        if (!stat(dir_path, &st)) s.ra_dir_mtime = (uint32_t)st.st_mtime;
    }
    mkdir(NB_TMP_DIR, 0755);
    NB_writeFile(NB_TMP_SESSION, &s, sizeof(s), 0);
}

void NB_endSession(const NB_Session* s, uint32_t elapsed, uint32_t wall_end) {
    if (elapsed < NB_MIN_SESSION) return;
    if (elapsed > NB_MAX_SESSION) elapsed = NB_MAX_SESSION;

    const char* game = nb_rel(s->game[0] ? s->game : s->rom);
    const char* rom = nb_rel(s->rom);

    S.play_clock += elapsed;
    S.sessions++;
    S.boots_since_play = 0;
    uint32_t now = nb_now();
    if (nb_wallOk(now)) S.last_wall = now;

    NB_Game* g = nb_game(game);
    g->secs += elapsed;
    g->plays++;
    g->last_session = S.sessions;

    for (int i = 0; i < NB_MAX_MISSIONS; i++) {
        NB_Mission* m = &S.missions[i];
        int hit = 0;
        switch (m->type) {
        case NB_MIS_PLAY_GAME: hit = nb_pathUnder(m->path, game) || nb_pathUnder(m->path, rom); break;
        case NB_MIS_PLAY_SYSTEM: hit = nb_inSystem(m->path, game); break;
        case NB_MIS_PLAY_ANY: hit = 1; break;
        }
        if (!hit) continue;
        m->progress += elapsed;
        nb_checkDone(i);
    }

    NB_Unlocks* found = calloc(4, sizeof(NB_Unlocks));
    if (!found) return;
    int n = nb_findUnlocks(s, elapsed, wall_end, found, 4);
    char title[NB_TITLE_LEN];
    nb_displayName(s->game[0] ? s->game : s->rom, title, sizeof(title));
    for (int i = 0; i < n; i++) nb_creditUnlocks(&found[i], title);
    free(found);
}

///////////////////////////////////////
// lifecycle

static void nb_newBoot(void) {
    S.boots++;
    S.boots_since_play++;

    // Away for a while: three boots without playing, or - if the clock can be
    // believed at both ends - three days. A clock that went back or reads
    // before this build is ignored, never "fixed" into the save.
    uint32_t now = nb_now();
    int away = S.boots_since_play > 3;
    if (nb_wallOk(now) && S.last_wall && now > S.last_wall) {
        uint32_t gone = now - S.last_wall;
        if (gone >= 3 * 86400u && gone <= 3650u * 86400u) away = 1;
    }
    if (away) S.mood = NB_MOOD_NEUTRAL;
    if (S.mood == NB_MOOD_SAD || S.mood == NB_MOOD_ANGRY)
        S.mood = nb_range(0, 1) ? NB_MOOD_NEUTRAL : NB_MOOD_SURPRISED;
    if (nb_wallOk(now)) S.last_wall = now;

    // the device was powered off mid-game last time
    NB_Session o;
    if (NB_readFile(NB_SD_ORPHAN, &o, sizeof(o)) == (int)sizeof(o) && o.magic == NB_SESSION_MAGIC &&
        o.version == NB_SESSION_VERSION && o.end_valid &&
        !(o.wall_start == S.orphan_wall && (uint32_t)o.mono_start_ms == S.orphan_mono)) {
        S.orphan_wall = o.wall_start;
        S.orphan_mono = (uint32_t)o.mono_start_ms;
        NB_endSession(&o, o.elapsed_s, o.wall_end);
    }
}

static int arrived = 0;
static int via_mission = 0;

int NB_cameFromMission(void) {
    return via_mission;
}

int NB_xpToShow(uint32_t* level, uint32_t* xp) {
    if (S.shown_level == S.level && S.shown_xp == S.xp) return 0;
    *level = S.shown_level;
    *xp = S.shown_xp;
    return 1;
}

void NB_xpShown(void) {
    if (S.shown_level == S.level && S.shown_xp == S.xp) return;
    S.shown_level = S.level;
    S.shown_xp = S.xp;
    nb_store(); // tmpfs; on the card with the rest at power-off
}

int NB_isAchievement(int type) {
    return type >= NB_MIS_ACH_ANY && type <= NB_MIS_ACH_SYSTEM;
}

int NB_isPresent(void) {
    return S.present;
}
int NB_arrived(void) {
    return arrived;
}
void NB_interact(void) {
    if (S.present && S.engaged) return;
    S.present = S.engaged = 1;
    nb_storeAs(0);
}

void NB_init(void) {
    NB_seed((uint32_t)NB_monoMs() ^ nb_now() ^ ((uint32_t)getpid() << 16));
    event_head = event_count = 0;
    leveled = 0;
    via_mission = 0;

    static NB_Save loaded;
    int was_present = 0;
    int new_boot = !nb_load(NB_TMP_STATE);
    loaded = S;
    if (new_boot) { // first minui of this boot
        if (!nb_load(NB_SD_STATE) && !nb_load(NB_SD_BACKUP)) nb_fresh();
        nb_newBoot();
        S.present = nb_range(0, 99) < 60; // sometimes waiting, sometimes hiding
        S.engaged = 0;
    }
    else { // the menu reloads (back from a game): an ignored NekoBit may wander
        was_present = S.present;
        if (S.present && !S.engaged && nb_range(0, 99) < 50) S.present = 0;
        else if (!S.present && nb_range(0, 99) < 20) S.present = 1;
    }

    NB_Session s;
    if (NB_readFile(NB_TMP_SESSION, &s, sizeof(s)) == (int)sizeof(s) && s.magic == NB_SESSION_MAGIC &&
        s.version == NB_SESSION_VERSION) {
        unlink(NB_TMP_SESSION);
        via_mission = (s.flags & NB_SESSION_VIA_NEKOBIT) != 0;
        uint64_t now = NB_monoMs();
        uint32_t elapsed = now > s.mono_start_ms ? (uint32_t)((now - s.mono_start_ms) / 1000) : 0;
        NB_endSession(&s, elapsed, nb_now());
    }
    if (event_count) S.present = 1; // it comes out to tell you the news
    arrived = S.present && !was_present;

    // only write what changed, and only mark the card behind for real changes
    NB_Save moved = loaded;
    moved.present = S.present;
    moved.engaged = S.engaged;
    if (new_boot || memcmp((uint8_t*)&moved + NB_CRC_OFFSET, (uint8_t*)&S + NB_CRC_OFFSET, sizeof(S) - NB_CRC_OFFSET))
        nb_store();
    else if (moved.present != loaded.present || moved.engaged != loaded.engaged)
        nb_storeAs(0);

    // A level up is a milestone: on the card straight away, not only at
    // power-off, so a hard power cut can't take it back. The game is over by
    // now (this is minui starting), so play itself still writes nothing.
    if (leveled) NB_flushToCard();
}

void NB_quit(void) {
    // nothing pending: every change is in tmpfs the moment it happens
}

void NB_reset(void) {
    nb_fresh();
    nb_store();
    nb_event(NB_EV_TEXT, STR_NB_EV_RESET, 0, NULL);
}

void NB_cancel(int slot) {
    if (slot < 0 || slot >= NB_MAX_MISSIONS || S.missions[slot].type == NB_MIS_NONE) return;
    memset(&S.missions[slot], 0, sizeof(S.missions[slot]));
    if (nb_range(0, 99) < 70) {
        S.mood = NB_MOOD_SAD;
        nb_event(NB_EV_TEXT, STR_NB_EV_CANCEL_SAD, 0, NULL);
    }
    else {
        S.mood = NB_MOOD_ANGRY;
        nb_event(NB_EV_TEXT, STR_NB_EV_CANCEL_ANGRY, 0, NULL);
    }
    nb_store();
}

const char* NB_greeting(void) {
    if (nb_range(0, 1)) return T(nb_range(STR_NB_GREET_FIRST, STR_NB_GREET_LAST));
    return T(nb_range(STR_NB_FACT_FIRST, STR_NB_FACT_LAST));
}

const char* NB_wokenGreeting(void) {
    return T(nb_range(STR_NB_WOKEN_FIRST, STR_NB_WOKEN_LAST));
}

///////////////////////////////////////
// offers

static int nb_isRomName(const char* name) {
    static const char* skip[] = {
        ".txt", ".log", ".cfg", ".ini", ".xml", ".json", ".md", ".html", ".htm",
        ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp", ".srm", ".sav", ".state",
        ".db", ".dat", ".bak", ".tmp", NULL,
    };
    if (hide((char*)name)) return 0;
    for (int i = 0; skip[i]; i++)
        if (suffixMatch((char*)skip[i], (char*)name)) return 0;
    return 1;
}

// A random game straight from a system folder, as minui would list it: a
// file, or a folder that launches itself (it holds <folder>.cue or .m3u).
static int nb_randomRom(const char* system, char* out, size_t size) {
    for (int attempt = 0; attempt < 4; attempt++) {
        DIR* dh = opendir(system);
        if (!dh) return 0;
        struct dirent* dp;
        char pick[256] = {0};
        int seen = 0;
        while ((dp = readdir(dh))) { // reservoir sampling: one pass, any size
            if (!nb_isRomName(dp->d_name)) continue;
            if (dp->d_type != DT_DIR && dp->d_type != DT_REG && dp->d_type != DT_UNKNOWN && dp->d_type != DT_LNK) continue;
            if (nb_range(0, seen++) == 0) nb_copy(pick, dp->d_name, sizeof(pick));
        }
        closedir(dh);
        if (!seen) return 0;

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", system, pick);
        struct stat st;
        if (stat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            char launcher[MAX_PATH];
            snprintf(launcher, sizeof(launcher), "%s/%s.cue", path, pick);
            int ok = !access(launcher, F_OK);
            snprintf(launcher, sizeof(launcher), "%s/%s.m3u", path, pick);
            ok = ok || !access(launcher, F_OK);
            if (!ok) continue; // a plain subfolder
        }
        nb_copy(out, path, size);
        return 1;
    }
    return 0;
}

static const NB_System* nb_systemFor(const NB_Library* lib, const char* path) {
    for (int i = 0; i < lib->system_count; i++)
        if (nb_inSystem(nb_rel(lib->systems[i].path), nb_rel(path))) return &lib->systems[i];
    return NULL;
}

static const NB_System* nb_systemForTag(const NB_Library* lib, const char* tag) {
    for (int i = 0; i < lib->system_count; i++) {
        char t[32];
        nb_tagOf(lib->systems[i].path, t, sizeof(t));
        if (!strcmp(t, tag)) return &lib->systems[i];
    }
    return NULL;
}

static void nb_setGame(NB_Mission* m, const NB_Library* lib, const char* abs_path) {
    nb_copy(m->path, nb_rel(abs_path), sizeof(m->path));
    nb_displayName(abs_path, m->title, sizeof(m->title));
    const NB_System* sys = nb_systemFor(lib, abs_path);
    nb_copy(m->system, sys ? sys->name : "", sizeof(m->system));
}

static int nb_offerHistory(const NB_Library* lib, NB_Mission* m) {
    // only what NekoBit has seen played itself: games[] lives in the save,
    // where minui's recents are a tmpfs file that every boot empties
    const char* cands[NB_MAX_GAMES];
    int plays[NB_MAX_GAMES];
    int n = 0;
    for (int i = 0; i < NB_MAX_GAMES; i++) {
        if (!S.games[i].path[0]) continue;
        cands[n] = S.games[i].path;
        plays[n++] = S.games[i].plays;
    }
    for (int tries = 0; n > 0 && tries < 6; tries++) {
        int i = nb_range(0, n - 1);
        char abs[MAX_PATH];
        nb_abs(cands[i], abs, sizeof(abs));
        if (access(abs, F_OK) || !nb_systemFor(lib, abs) || nb_hasMission(NB_MIS_PLAY_GAME, cands[i], NULL)) continue;
        memset(m, 0, sizeof(*m));
        m->type = NB_MIS_PLAY_GAME;
        if (plays[i] < 3) {
            m->flavor = NB_FLAVOR_REDISCOVER;
            m->target = nb_range(5, 14);
        }
        else {
            m->flavor = NB_FLAVOR_PLAYMORE;
            m->target = nb_range(10, 24);
        }
        m->xp = nb_scaled(m->target * 5);
        nb_setGame(m, lib, abs);
        return 1;
    }
    return 0;
}

static int nb_offerSystem(const NB_Library* lib, NB_Mission* m) {
    if (!lib->system_count) return 0;
    const NB_System* sys = &lib->systems[nb_range(0, lib->system_count - 1)];
    memset(m, 0, sizeof(*m));
    m->type = NB_MIS_PLAY_SYSTEM;
    m->target = nb_range(5, 19);
    m->xp = nb_scaled(m->target * 6);
    nb_copy(m->path, nb_rel(sys->path), sizeof(m->path));
    nb_copy(m->title, sys->name, sizeof(m->title));
    nb_copy(m->system, sys->name, sizeof(m->system));
    return 1;
}

static int nb_pickRandomGame(const NB_Library* lib, char* out, size_t size) {
    for (int tries = 0; lib->system_count && tries < 5; tries++)
        if (nb_randomRom(lib->systems[nb_range(0, lib->system_count - 1)].path, out, size)) return 1;
    return 0;
}

static int nb_offerRandomGame(const NB_Library* lib, NB_Mission* m) {
    char abs[MAX_PATH];
    if (!nb_pickRandomGame(lib, abs, sizeof(abs))) return 0;
    memset(m, 0, sizeof(*m));
    m->type = NB_MIS_PLAY_GAME;
    m->flavor = NB_FLAVOR_RANDOM;
    m->target = nb_range(5, 19);
    m->xp = nb_scaled(m->target * 7);
    nb_setGame(m, lib, abs);
    return 1;
}

typedef struct NB_RACandidate {
    char tag[32];
    char hash[33];
    char path[NB_PATH_LEN];
    uint16_t total;
    uint16_t unlocked;
} NB_RACandidate;

static int nb_offerAchievement(const NB_Library* lib, NB_Mission* m) {
    enum { MAX_CANDS = 96 };
    NB_RACandidate* cands = calloc(MAX_CANDS, sizeof(*cands));
    if (!cands) return 0;
    int n = 0, masteries = 0;

    char root[MAX_PATH];
    snprintf(root, sizeof(root), "%s/progress", RETROACHIVEMENTS_PATH);
    DIR* tags = opendir(root);
    struct dirent* tp;
    while (tags && (tp = readdir(tags)) && n < MAX_CANDS) {
        if (tp->d_name[0] == '.') continue;
        char dir_path[MAX_PATH];
        snprintf(dir_path, sizeof(dir_path), "%s/%s", root, tp->d_name);
        DIR* dh = opendir(dir_path);
        struct dirent* dp;
        while (dh && (dp = readdir(dh)) && n < MAX_CANDS) {
            if (!suffixMatch("_progress.json", dp->d_name)) continue;
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s/%s", dir_path, dp->d_name);
            char* json = nb_readAll(path);
            NB_Unlocks u;
            if (json && nb_parseProgress(json, 1, 0, &u) && u.total > 0 && u.unlocked < u.total &&
                !nb_hasMission(NB_MIS_ACH_GAME, NULL, u.hash)) {
                NB_RACandidate* c = &cands[n++];
                nb_copy(c->tag, tp->d_name, sizeof(c->tag));
                nb_copy(c->hash, u.hash, sizeof(c->hash));
                nb_copy(c->path, u.path, sizeof(c->path));
                c->total = u.total;
                c->unlocked = u.unlocked;
                masteries += u.unlocked * 2 >= u.total;
            }
            free(json);
        }
        if (dh) closedir(dh);
    }
    if (tags) closedir(tags);

    int found = 0;
    if (n) {
        // sets already half done get the mastery missions most of the time
        int want_mastery = masteries && nb_range(0, 99) < 60;
        int pick = -1;
        for (int tries = 0; tries < 32 && pick < 0; tries++) {
            int i = nb_range(0, n - 1);
            if (!want_mastery || cands[i].unlocked * 2 >= cands[i].total) pick = i;
        }
        if (pick < 0) pick = 0;
        NB_RACandidate* c = &cands[pick];
        int remaining = c->total - c->unlocked;
        int percent = c->unlocked * 100 / c->total;

        memset(m, 0, sizeof(*m));
        m->type = NB_MIS_ACH_GAME;
        m->flavor = c->unlocked * 2 >= c->total ? NB_FLAVOR_MASTERY : NB_FLAVOR_NONE;
        m->target = nb_range(1, remaining < 2 ? remaining : 2);
        m->xp = nb_scaled(m->target * (40 + 2 * percent)); // the deeper into a set, the bigger the reward
        m->ra_total = c->total;
        m->ra_unlocked = c->unlocked;
        nb_copy(m->tag, c->tag, sizeof(m->tag));
        nb_copy(m->hash, c->hash, sizeof(m->hash));
        char* set = nb_readSet(c->tag, c->hash);
        if (!nb_setTitle(set, m->title, sizeof(m->title))) nb_copy(m->title, c->tag, sizeof(m->title));
        free(set);
        const NB_System* sys = nb_systemForTag(lib, c->tag);
        nb_copy(m->system, sys ? sys->name : c->tag, sizeof(m->system));
        // where minarch last played the set from, if the game is still there
        char abs[MAX_PATH];
        nb_abs(c->path, abs, sizeof(abs));
        if (c->path[0] == '/' && !access(abs, F_OK)) nb_copy(m->path, c->path, sizeof(m->path));
        found = 1;
    }
    free(cands);
    if (found) return 1;

    // no progress yet: a system that has sets downloaded, or anything at all
    snprintf(root, sizeof(root), "%s/data", RETROACHIVEMENTS_PATH);
    DIR* dh = opendir(root);
    if (!dh) return 0;
    const NB_System* systems[64];
    int count = 0;
    struct dirent* dp;
    while ((dp = readdir(dh)) && count < 64) {
        if (dp->d_name[0] == '.') continue;
        const NB_System* sys = nb_systemForTag(lib, dp->d_name);
        if (sys && !nb_hasMission(NB_MIS_ACH_SYSTEM, nb_rel(sys->path), NULL)) systems[count++] = sys;
    }
    closedir(dh);
    memset(m, 0, sizeof(*m));
    if (count) {
        const NB_System* sys = systems[nb_range(0, count - 1)];
        m->type = NB_MIS_ACH_SYSTEM;
        m->target = 1;
        m->xp = nb_scaled(50);
        nb_tagOf(sys->path, m->tag, sizeof(m->tag));
        nb_copy(m->title, sys->name, sizeof(m->title));
        nb_copy(m->system, sys->name, sizeof(m->system));
        nb_copy(m->path, nb_rel(sys->path), sizeof(m->path));
        return 1;
    }
    if (nb_hasMission(NB_MIS_ACH_ANY, NULL, NULL)) return 0;
    m->type = NB_MIS_ACH_ANY;
    m->target = nb_range(1, 3);
    m->xp = nb_scaled(40 * m->target);
    return 1;
}

int NB_offers(const NB_Library* lib, NB_Mission* out, int max) {
    int n = 0;
    NB_Mission m;
    if (n < max) {
        if (nb_offerHistory(lib, &m)) out[n++] = m;
        else if (!nb_hasMission(NB_MIS_PLAY_ANY, NULL, NULL)) {
            memset(&m, 0, sizeof(m));
            m.type = NB_MIS_PLAY_ANY;
            m.target = nb_range(5, 14);
            m.xp = nb_scaled(m.target * 5);
            out[n++] = m;
        }
    }
    if (n < max && nb_offerSystem(lib, &m)) out[n++] = m;
    if (n < max && nb_offerRandomGame(lib, &m)) out[n++] = m;
    if (n < max && lib->system_count) {
        memset(&m, 0, sizeof(m));
        m.type = NB_MIS_PLAY_SURPRISE;
        m.flavor = NB_FLAVOR_SURPRISE;
        m.target = nb_range(10, 24);
        m.xp = nb_scaled(m.target * 8);
        out[n++] = m;
    }
    if (n < max && nb_offerAchievement(lib, &m)) out[n++] = m;
    return n;
}

int NB_accept(const NB_Mission* offer, const NB_Library* lib) {
    int slot = -1;
    for (int i = 0; i < NB_MAX_MISSIONS && slot < 0; i++)
        if (S.missions[i].type == NB_MIS_NONE) slot = i;
    if (slot < 0) return -1;

    NB_Mission m = *offer;
    if (m.type == NB_MIS_PLAY_SURPRISE) { // only now does anyone find out what it is
        char abs[MAX_PATH];
        if (!nb_pickRandomGame(lib, abs, sizeof(abs))) return -2;
        m.type = NB_MIS_PLAY_GAME;
        nb_setGame(&m, lib, abs);
    }
    m.progress = 0;
    m.accepted_at = S.play_clock;
    S.missions[slot] = m;
    nb_event(NB_EV_TEXT, STR_NB_EV_ACCEPTED, m.xp, NULL);
    nb_store();
    return slot;
}

int NB_target(const NB_Mission* m, NB_Target* t) {
    memset(t, 0, sizeof(*t));
    const char* roms = nb_rel(ROMS_PATH); // "/Roms"
    size_t len = strlen(roms);
    switch (m->type) {
    case NB_MIS_PLAY_GAME:
    case NB_MIS_ACH_GAME:
        if (m->path[0] && !strncmp(m->path, roms, len) && m->path[len] == '/') {
            nb_abs(m->path, t->select, sizeof(t->select));
            char system[NB_PATH_LEN];
            nb_copy(system, m->path, sizeof(system));
            char* slash = strchr(system + len + 1, '/');
            if (slash) *slash = '\0';
            nb_abs(system, t->system, sizeof(t->system));
            return 1;
        }
        if (m->type == NB_MIS_ACH_GAME && m->tag[0]) {
            nb_copy(t->tag, m->tag, sizeof(t->tag)); // the system, but no game to pick
            return 1;
        }
        return 0;
    case NB_MIS_PLAY_SYSTEM:
    case NB_MIS_ACH_SYSTEM:
        if (!m->path[0]) return 0;
        nb_abs(m->path, t->system, sizeof(t->system));
        return 1;
    }
    return 0;
}
