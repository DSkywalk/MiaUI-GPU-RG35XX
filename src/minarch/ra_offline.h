#ifndef __RA_OFFLINE_H__
#define __RA_OFFLINE_H__

#include <stddef.h>
#include <stdint.h>

// Fully offline RetroAchievements support.
//
// No network, no rc_client, no badges. Achievement definitions are read from
// a json previously downloaded with the `patch` endpoint:
//
//   <root>/data/<hash>.json              (the raw {"Success":..,"PatchData":..})
//   <root>/progress/<hash>_progress.json (written by us)
//
// Only the rcheevos runtime is linked in (trigger evaluation against core
// memory), which is what keeps this cheap enough for the device.

struct retro_memory_map;

// One base-set achievement. The strings live until RA_unloadGame().
typedef struct {
    uint32_t    id;
    uint32_t    points;
    const char* title;
    const char* description;
    int         unlocked;
    // "12/76" or "52%" for achievements that track progress, empty otherwise.
    // Only locked achievements measure anything; refresh with
    // RA_refreshProgress() before reading.
    char        progress[12];
} RA_Achievement;

typedef void*  (*RA_GetMemoryData)(unsigned id);
typedef size_t (*RA_GetMemorySize)(unsigned id);

// Call once at startup. `root` is the folder holding data/ and progress/,
// eg. SDCARD_PATH "/retroachievements". Nothing is allocated here.
void RA_init(const char* root);

// Wire up the core's memory accessors. Must be called before RA_loadGame.
void RA_setMemoryFuncs(RA_GetMemoryData get_data, RA_GetMemorySize get_size);

// Called from the RETRO_ENVIRONMENT_SET_MEMORY_MAPS handler. The descriptors
// are deep-copied, the core may free its own copy afterwards.
void RA_setMemoryMap(const struct retro_memory_map* mmap);

// Hash the rom, look for <root>/data/<hash>.json and arm the runtime.
// `rom_data`/`rom_size` may be NULL/0 when the core loads from disk.
// Returns 1 if achievements were loaded for this game, 0 otherwise.
int RA_loadGame(const char* emu_tag, const char* rom_path, const void* rom_data, size_t rom_size);

// Tear down the runtime and flush progress.
void RA_unloadGame(void);

// Once per frame, right after core.run().
void RA_doFrame(void);

// Reset trigger state (hit counts) after a core reset / state load.
void RA_resetRuntime(void);

// How much the on-screen toasts should say. The order matches the labels in
// minarch's option table, so the config value indexes straight into this.
enum {
    RA_NOTIFY_ALL = 0,  // startup summary, unlocks, warnings
    RA_NOTIFY_UNLOCKS,  // only "you just earned X"
    RA_NOTIFY_GENERAL,  // everything except the unlocks
    RA_NOTIFY_NONE,     // nothing at all
};

// Both are cosmetic, so unlike the per-game on/off switch they take effect
// straight away rather than at the next launch.
void RA_setNotify(int mode);
void RA_setChime(int enabled);

// Currently active toast, or NULL when there is nothing to draw.
const char* RA_getMessage(void);

// Increments every time a new toast is posted, so the caller can cache the
// rendered surface instead of rasterising the same string every frame.
uint32_t RA_getMessageId(void);

// 1 while a game with achievements is loaded. Everything downstream keys off
// this: the pause menu rows, hardcore, the popups and the per-frame
// evaluation. Decided once by RA_loadGame() and fixed for the session -- the
// per-game on/off switch is read at launch and never applied to a running
// game, or turning it off would hand back the save state menu mid-run.
int RA_isActive(void);

// Erase every local unlock for this game: forget them, delete the progress
// file and re-arm the whole set so it can be earned again right away.
// Destructive and not undoable; confirm with the user first.
// Returns 1 if the set was re-armed, 0 if nothing was loaded.
int RA_wipeProgress(void);

// unlocked/total for the loaded game, and the points earned so far. Any of
// the three may be NULL. All are 0 when nothing is loaded.
void RA_getSummary(uint32_t* unlocked, uint32_t* total, uint32_t* points);

// Base-set achievements for the loaded game, in the order the set defines
// them. Leaderboards, rich presence and non-core sets are never included.
int RA_getAchievementCount(void);
const RA_Achievement* RA_getAchievement(int index);

// Re-read the progress trackers from the runtime. Cheap, but it walks the
// whole set, so call it when opening the list rather than every frame.
void RA_refreshProgress(void);

#endif // __RA_OFFLINE_H__
