#ifndef __NEKOBIT_H__
#define __NEKOBIT_H__

// NekoBit, the gaming companion that lives in minui: moods, XP and levels,
// play-time and achievement missions. Logic only (no SDL, no GL): the overlay
// and the face pill are in nekobit_ui.c, storage in common/nekobit_store.h.
//
// Time: nothing here trusts the calendar. A session is measured with
// CLOCK_MONOTONIC across minui's exit and relaunch, mission progress is
// counted in seconds played (play_clock), and the wall clock is only used to
// place minarch's achievement unlock dates inside a session, and as a hint
// for "you've been away a while".

#include <stdint.h>
#include <stddef.h>
#include "nekobit_store.h"

enum {
    NB_MOOD_HAPPY,
    NB_MOOD_EXCITED,
    NB_MOOD_NEUTRAL,
    NB_MOOD_SAD,
    NB_MOOD_ANGRY,
    NB_MOOD_SURPRISED,
    NB_MOOD_COUNT
};

enum {
    NB_MIS_NONE = 0,
    NB_MIS_PLAY_GAME,     // one game (path) for `target` minutes
    NB_MIS_PLAY_SYSTEM,   // any game from a system folder (path)
    NB_MIS_PLAY_SURPRISE, // offers only: picks its game (a PLAY_GAME) on accept
    NB_MIS_PLAY_ANY,      // any game at all
    NB_MIS_ACH_ANY,       // `target` unlocks anywhere
    NB_MIS_ACH_GAME,      // `target` unlocks in one RA set (hash)
    NB_MIS_ACH_SYSTEM,    // an unlock on one system (tag)
};

enum { // wording and reward variants
    NB_FLAVOR_NONE,
    NB_FLAVOR_REDISCOVER, // a game played less than 3 times
    NB_FLAVOR_PLAYMORE,
    NB_FLAVOR_RANDOM,
    NB_FLAVOR_SURPRISE,
    NB_FLAVOR_MASTERY,    // an RA set at 50% or more
};

#define NB_PATH_LEN     256 // relative to SDCARD_PATH, like recent.txt
#define NB_TITLE_LEN    64
#define NB_MAX_MISSIONS 5
#define NB_MAX_HISTORY  32
#define NB_MAX_GAMES    32
#define NB_MAX_OFFERS   5

typedef struct NB_Mission {
    uint8_t  type;
    uint8_t  flavor;
    uint16_t target;      // minutes (PLAY_*) or unlocks (ACH_*)
    uint16_t xp;          // reward on completion
    uint16_t ra_total;    // ACH_GAME: the set, for display
    uint16_t ra_unlocked;
    uint16_t pad;
    uint32_t progress;    // seconds (PLAY_*) or unlocks (ACH_*)
    uint32_t accepted_at; // play_clock
    char path[NB_PATH_LEN];    // the game or the system folder, "" = any
    char tag[32];              // emulator tag (ACH_*)
    char hash[33];             // RA set (ACH_GAME)
    char title[NB_TITLE_LEN];  // game title or system name
    char system[NB_TITLE_LEN]; // system name of a game
} NB_Mission;

typedef struct NB_History {
    uint8_t  type;
    uint8_t  flavor;
    uint16_t xp;
    uint16_t target;
    uint16_t pad;
    uint32_t done_at; // play_clock
    char title[NB_TITLE_LEN];
} NB_History;

// What NekoBit knows about a game it has seen played. The RA hash is learned
// the first time an unlock happens in one of its sessions.
typedef struct NB_Game {
    char path[NB_PATH_LEN];
    char tag[32];
    char hash[33];
    uint8_t pad[3];
    uint16_t ra_total;
    uint16_t ra_unlocked;
    uint32_t secs;
    uint32_t plays;
    uint32_t last_session;
} NB_Game;

#define NB_SAVE_MAGIC   0x5342424e // "NBBS"
#define NB_SAVE_VERSION 1

typedef struct NB_Save {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t crc; // of everything after this field
    uint32_t level;
    uint32_t xp;
    uint32_t xp_next;
    uint32_t mood;
    uint32_t missions_completed;
    uint32_t achievements;
    uint32_t masteries;
    uint32_t sessions;
    uint32_t boots;
    uint32_t boots_since_play;
    uint32_t play_clock; // seconds played, ever: the only clock NekoBit trusts
    uint32_t last_wall;  // advisory only, 0 = unknown
    uint32_t orphan_wall; // the last power-off session credited, so it
    uint32_t orphan_mono; // never counts twice
    uint32_t history_count;
    uint8_t present; // NekoBit is out on the root menu (rolled each boot)
    uint8_t engaged; // ...and the player has talked to it this boot, so it stays
    uint8_t pad8[2];
    uint32_t reserved[3];
    NB_Mission missions[NB_MAX_MISSIONS];
    NB_History history[NB_MAX_HISTORY]; // ring, newest at (history_count-1) % NB_MAX_HISTORY
    NB_Game games[NB_MAX_GAMES];
} NB_Save;

enum {
    NB_EV_MISSION,   // a = xp
    NB_EV_LEVEL,     // a = level
    NB_EV_ACH,       // a = unlocks, b = xp
    NB_EV_MILESTONE, // a = percent, b = xp, text = game
    NB_EV_MASTERY,   // b = xp, text = game
    NB_EV_TEXT,      // a = string id
};

typedef struct NB_Event {
    int kind;
    int a, b;
    char text[NB_TITLE_LEN];
} NB_Event;

// What minui knows and NekoBit picks missions from. Paths are absolute.
typedef struct NB_System {
    const char* path; // a root entry under ROMS_PATH
    const char* name;
} NB_System;

// Only the systems: "play it again" missions come from NekoBit's own games[]
// in the save, never minui's recents (a tmpfs file, gone after every boot).
typedef struct NB_Library {
    const NB_System* systems;
    int system_count;
} NB_Library;

// Where a mission's game lives, for minui to navigate to. Absolute paths;
// `system` empty = use `tag` to find the system, both empty = nowhere.
typedef struct NB_Target {
    char system[MAX_PATH];
    char select[MAX_PATH];
    char tag[32];
} NB_Target;

void NB_init(void);  // loads the state, settles the game that just ended
void NB_quit(void);
// Right before minui exits to launch a game. Safe before NB_init().
void NB_beginSession(const char* rom, const char* game, const char* tag);

const NB_Save* NB_state(void);

// NekoBit has a mind of its own: out on the root menu or hiding, decided on
// each boot; ignored, it may wander off when the menu reloads; talked to, it
// stays for the rest of the boot; news (a banner) always brings it out.
int NB_isPresent(void);
int NB_arrived(void);  // it came out since the last menu: play the peek-in
void NB_interact(void); // the player summoned it
const char* NB_face(int mood);
const char* NB_sleepyFace(void); // woken up by SELECT while hiding
int NB_moodString(int mood); // STR_NB_MOOD_*
int NB_popEvent(NB_Event* out); // 1 while there are banners to show
int NB_saveNow(void);            // the explicit save: <0 failed

int NB_freeSlots(void);
int NB_offers(const NB_Library* lib, NB_Mission* out, int max);
int NB_accept(const NB_Mission* offer, const NB_Library* lib); // slot, -1 full, -2 no game found
void NB_cancel(int slot);
void NB_reset(void);
void NB_describe(const NB_Mission* m, char* out, size_t size); // the full sentence
void NB_label(const NB_Mission* m, char* out, size_t size);    // short, for a list row: just the target
int NB_isAchievement(int type); // an NB_MIS_ACH_*: its row gets the trophy
int NB_target(const NB_Mission* m, NB_Target* out);
void NB_formatTime(uint32_t secs, char* out, size_t size);
const char* NB_greeting(void); // a random greeting or gaming fact
const char* NB_wokenGreeting(void); // what it says when SELECT wakes it

// exposed for the unit tests
int NB_xpForUnlock(int points, int rarity_x100, int before, int total);
int NB_milestoneBonus(int before, int after, int total, int* percent);
void NB_endSession(const NB_Session* s, uint32_t elapsed_s, uint32_t wall_end);
uint32_t NB_buildEpoch(void);
void NB_seed(uint32_t seed);

#endif
