#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libretro.h>

#include <rc_runtime.h>
#include <rc_api_runtime.h>
#include <rc_consoles.h>
#include <rc_hash.h>
#include <rc_error.h>
#include <rc_libretro.h>

#ifdef RA_HAS_CHD
#include "chd_reader.h"
#endif

#include "ra_offline.h"

// Declared by common/api.h, but that header drags in SDL/GLES which this
// module has no use for -- keep it standalone and just borrow the logger.
enum { RA_LOG_INFO = 1, RA_LOG_WARN = 2 };
void LOG_note(int level, const char* fmt, ...);
void SND_playChime(void); // common/api.h, declared here to keep this file standalone
#define LOG_info(fmt, ...) LOG_note(RA_LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_warn(fmt, ...) LOG_note(RA_LOG_WARN, fmt, ##__VA_ARGS__)

#ifndef MAX_PATH
#define MAX_PATH 512
#endif

///////////////////////////////////////

// how long a popup stays on screen, in frames (~4s at 60fps)
#define RA_MSG_FRAMES 240
#define RA_MSG_SIZE   160

// RA ships a dummy "Warning: Outdated Emulator" achievement in every set.
// Its trigger is `1=1.300.` so it would always pop after 300 frames.
#define RA_LOCAL_ID_BASE 101000000

static const char* ra_titleForId(uint32_t id);
static RA_Achievement* ra_findById(uint32_t id);
static int ra_isBaseSet(const rc_api_achievement_definition_t* ach);
static void ra_countPoints(void); // defined with the achievement list

static struct {
    rc_runtime_t runtime;
    rc_libretro_memory_regions_t regions;

    struct retro_memory_map mmap;
    struct retro_memory_descriptor* descriptors;

    RA_GetMemoryData get_data;
    RA_GetMemorySize get_size;

    uint32_t* unlocked;
    uint32_t  unlocked_count;
    uint32_t  unlocked_cap;
    uint32_t  total;
    uint32_t  points;      // sum of what's been earned, cached (see ra_countPoints)

    uint32_t  console_id;
    uint32_t  set_check;     // fingerprint of the base set in the data json
    int        invalidated;   // the progress file failed its checksum
    int        progress_stale;// the file on disk no longer matches what we hold
    char      hash[33];
    char      root[MAX_PATH];
    char      data_path[MAX_PATH];     // the set we parsed, so we can re-read it
    char      progress_path[MAX_PATH];

    char      msg[RA_MSG_SIZE];
    int       msg_frames;
    uint32_t  msg_id;      // bumped per message so the ui can cache its render

    int       active;      // runtime holds a valid achievement set
    int       mem_ready;   // rc_libretro memory regions resolved
    int       mem_tries;
    uint32_t  frames;
} ra;

///////////////////////////////////////
// small helpers

static char* ra_readFile(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0) { fclose(f); return NULL; }

    char* buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read = fread(buf, 1, len, f);
    fclose(f);

    buf[read] = '\0';
    if (out_len) *out_len = read;
    return buf;
}

static int ra_fileExists(const char* path) {
    struct stat st;
    return stat(path, &st)==0 && st.st_size>0;
}

// Reads a saved `r=patch` body and hands it to rcheevos' own parser. On
// success the caller owns `out` and must destroy it.
static int ra_readSet(const char* path, rc_api_fetch_game_data_response_t* out) {
    size_t json_len = 0;
    char* json = ra_readFile(path, &json_len);
    if (!json) return 0;

    memset(out, 0, sizeof(*out));

    // the _server_response form is the supported one; the plain
    // rc_api_process_fetch_game_data_response() is marked deprecated upstream
    rc_api_server_response_t server_response;
    memset(&server_response, 0, sizeof(server_response));
    server_response.body = json;
    server_response.body_length = json_len;
    server_response.http_status_code = 200; // the file is a saved 200 body

    int result = rc_api_process_fetch_game_data_server_response(out, &server_response);
    free(json);

    if (result!=RC_OK || !out->num_achievements) {
        LOG_warn("RA: %s is not a usable patch response (%s)\n", path, rc_error_str(result));
        rc_api_destroy_fetch_game_data_response(out);
        return 0;
    }
    return 1;
}

static int ra_isUnlocked(uint32_t id) {
    for (uint32_t i=0; i<ra.unlocked_count; i++) {
        if (ra.unlocked[i]==id) return 1;
    }
    return 0;
}

static void ra_addUnlocked(uint32_t id) {
    if (ra_isUnlocked(id)) return;

    if (ra.unlocked_count==ra.unlocked_cap) {
        uint32_t cap = ra.unlocked_cap ? ra.unlocked_cap*2 : 32;
        uint32_t* grown = realloc(ra.unlocked, cap*sizeof(uint32_t));
        if (!grown) return;
        ra.unlocked = grown;
        ra.unlocked_cap = cap;
    }
    ra.unlocked[ra.unlocked_count++] = id;
}

///////////////////////////////////////
// progress file
//
//   {"hash":"<md5>","set":"a1b2c3d4","total":23,
//    "unlocked":[1001,1002],"check":"e5f6a7b8"}
//
// `set` fingerprints the base set the file was written against; `check` is an
// FNV-1a over the game hash, that `set` value and the unlocked list. A file
// that doesn't match its own checksum has been edited, so we drop the progress
// rather than trust it.
//
// This is a tamper *indicator*, not tamper protection: the algorithm is right
// here in the source, so anyone determined can recompute it. It catches a
// hand-edited id list, which is what it's for.

#define RA_FNV_OFFSET 2166136261UL
#define RA_FNV_PRIME  16777619UL

static uint32_t ra_fnv(uint32_t hash, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i=0; i<size; i++) {
        hash ^= p[i];
        hash *= RA_FNV_PRIME;
    }
    return hash;
}

// Fingerprint of the base set: every id, plus how many there are.
static uint32_t ra_setChecksum(const rc_api_fetch_game_data_response_t* game) {
    uint32_t sum = RA_FNV_OFFSET;
    uint32_t count = 0;

    for (uint32_t i=0; i<game->num_achievements; i++) {
        const rc_api_achievement_definition_t* ach = &game->achievements[i];
        if (!ra_isBaseSet(ach)) continue;
        sum = ra_fnv(sum, &ach->id, sizeof(ach->id));
        count++;
    }
    return ra_fnv(sum, &count, sizeof(count));
}

// Takes the set value as an argument so a load can verify against whatever the
// file claims, rather than what the set happens to be right now.
static uint32_t ra_progressChecksum(uint32_t set_check) {
    uint32_t sum = ra_fnv(RA_FNV_OFFSET, ra.hash, strlen(ra.hash));
    sum = ra_fnv(sum, &set_check, sizeof(set_check));
    sum = ra_fnv(sum, &ra.unlocked_count, sizeof(ra.unlocked_count));
    for (uint32_t i=0; i<ra.unlocked_count; i++) {
        sum = ra_fnv(sum, &ra.unlocked[i], sizeof(ra.unlocked[i]));
    }
    return sum;
}

// Pulls `"<key>":"<value>"` out of our own output. Returns 0 if absent.
static int ra_readField(const char* json, const char* key, char* out, size_t size) {
    const char* at = strstr(json, key);
    if (!at) return 0;
    at = strchr(at, ':');
    if (!at) return 0;
    at = strchr(at, '"');
    if (!at) return 0;

    at++;
    size_t i = 0;
    while (at[i] && at[i]!='"' && i<size-1) { out[i] = at[i]; i++; }
    out[i] = '\0';
    return 1;
}

static void ra_loadProgress(void) {
    char* json = ra_readFile(ra.progress_path, NULL);
    if (!json) return;

    char field[64];
    int  have_check = ra_readField(json, "\"check\"", field, sizeof(field));
    uint32_t stored_check = have_check ? (uint32_t)strtoul(field, NULL, 16) : 0;

    int have_set = ra_readField(json, "\"set\"", field, sizeof(field));
    uint32_t stored_set = have_set ? (uint32_t)strtoul(field, NULL, 16) : 0;

    // the file has to name this game, or it belongs to something else
    int hash_ok = ra_readField(json, "\"hash\"", field, sizeof(field)) &&
                  strcasecmp(field, ra.hash)==0;

    // we wrote this file ourselves, so a digit scan over the array is enough
    char* p = strstr(json, "\"unlocked\"");
    if (p) p = strchr(p, '[');
    while (p && *p && *p!=']') {
        p++;
        if (*p>='0' && *p<='9') {
            ra_addUnlocked((uint32_t)strtoul(p, &p, 10));
        }
    }

    free(json);

    if (!have_check || !have_set || !hash_ok ||
        ra_progressChecksum(stored_set)!=stored_check) {
        LOG_warn("RA: %s failed its checksum, discarding progress\n", ra.progress_path);
        ra.unlocked_count = 0;
        ra.invalidated = 1;
        ra.progress_stale = 1;
        return;
    }

    if (stored_set!=ra.set_check) {
        // the downloaded set changed since this file was written (an added or
        // retired achievement). The ids still mean the same thing, so keep the
        // progress and just refresh the file.
        LOG_info("RA: achievement set changed, keeping progress\n");
        ra.progress_stale = 1;
    }

    LOG_info("RA: restored %u unlocked achievement(s)\n", ra.unlocked_count);
}

// Ids can outlive the set they came from, so forget the ones that are gone or
// the totals we report would be nonsense.
static void ra_pruneUnlocked(void) {
    uint32_t kept = 0;
    for (uint32_t i=0; i<ra.unlocked_count; i++) {
        if (ra_findById(ra.unlocked[i])) ra.unlocked[kept++] = ra.unlocked[i];
    }
    if (kept!=ra.unlocked_count) {
        LOG_info("RA: dropped %u unlocked id(s) no longer in the set\n",
                 ra.unlocked_count - kept);
        ra.unlocked_count = kept;
        ra.progress_stale = 1;
    }
}

static void ra_saveProgress(void) {
    if (!ra.progress_path[0]) return;

    char tmp_path[MAX_PATH];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", ra.progress_path);

    FILE* f = fopen(tmp_path, "w");
    if (!f) {
        LOG_warn("RA: unable to write %s\n", tmp_path);
        return;
    }

    fprintf(f, "{\"hash\":\"%s\",\"set\":\"%08x\",\"total\":%u,\"unlocked\":[",
            ra.hash, ra.set_check, ra.total);
    for (uint32_t i=0; i<ra.unlocked_count; i++) {
        fprintf(f, "%s%u", i ? "," : "", ra.unlocked[i]);
    }
    fprintf(f, "],\"check\":\"%08x\"}\n", ra_progressChecksum(ra.set_check));

    fclose(f);

    // rename so a crash mid-write can't leave a truncated file behind
    rename(tmp_path, ra.progress_path);
    ra.progress_stale = 0;
}

///////////////////////////////////////
// memory access

static void ra_getCoreMemoryInfo(uint32_t id, rc_libretro_core_memory_info_t* info) {
    if (ra.get_data && ra.get_size) {
        info->data = (uint8_t*)ra.get_data(id);
        info->size = ra.get_size(id);
    }
    else {
        info->data = NULL;
        info->size = 0;
    }
}

// Some cores only expose their memory after the first retro_run(), so this is
// deferred out of RA_loadGame and retried until it sticks.
#define RA_MEM_MAX_TRIES 10
#define RA_MEM_RETRY_FRAMES 60

static void ra_initMemory(void) {
    ra.mem_tries++;

    if (rc_libretro_memory_init(&ra.regions,
                                ra.mmap.num_descriptors ? &ra.mmap : NULL,
                                ra_getCoreMemoryInfo, ra.console_id)) {
        ra.mem_ready = 1;
        LOG_info("RA: %u memory region(s), %zu bytes (console %u)\n",
                 ra.regions.count, ra.regions.total_size, ra.console_id);
    }
    else if (ra.mem_tries>=RA_MEM_MAX_TRIES) {
        LOG_warn("RA: no usable memory regions for console %u, giving up\n", ra.console_id);
    }
}

static uint32_t ra_readMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, void* ud) {
    return rc_libretro_memory_read(&ra.regions, address, buffer, num_bytes);
}

///////////////////////////////////////
// runtime events

static void ra_eventHandler(const rc_runtime_event_t* event) {
    if (event->type!=RC_RUNTIME_EVENT_ACHIEVEMENT_TRIGGERED) return;

    const char* title = ra_titleForId(event->id);

    ra_addUnlocked(event->id);
    rc_runtime_deactivate_achievement(&ra.runtime, event->id);

    RA_Achievement* entry = ra_findById(event->id);
    if (entry) entry->unlocked = 1;
    ra_countPoints();

    // the trophy the toast draws is the points symbol, so the number needs no
    // unit beside it
    snprintf(ra.msg, sizeof(ra.msg), "%u   %s",
             entry ? entry->points : 0, title ? title : "???");
    ra.msg_frames = RA_MSG_FRAMES;
    ra.msg_id++;

    //LOG_debug("RA: unlocked %u (%s)\n", event->id, title ? title : "???");

    SND_playChime(); // just sets a flag; the audio thread does the work

    ra_saveProgress();
}

///////////////////////////////////////
// achievement list
//
// The api response is freed right after activation, so keep a flat copy of
// everything the pause menu needs: one array plus one string pool.

static struct {
    RA_Achievement* items;
    char*           pool;
    uint32_t        count;
} ra_list;

// RA injects a dummy "Warning: Outdated Emulator" achievement into every set
// (id >= 101000000, trigger `1=1.300.`) to tell online clients their hardcore
// unlocks won't be counted. It would pop after 300 frames and means nothing
// offline, so it never reaches the runtime or the list.
static int ra_isBaseSet(const rc_api_achievement_definition_t* ach) {
    return ach->category==RC_ACHIEVEMENT_CATEGORY_PROMOTED && ach->id<RA_LOCAL_ID_BASE;
}

static RA_Achievement* ra_findById(uint32_t id) {
    for (uint32_t i=0; i<ra_list.count; i++) {
        if (ra_list.items[i].id==id) return &ra_list.items[i];
    }
    return NULL;
}

static const char* ra_titleForId(uint32_t id) {
    const RA_Achievement* ach = ra_findById(id);
    return ach ? ach->title : NULL;
}

static void ra_freeList(void) {
    free(ra_list.items);
    free(ra_list.pool);
    memset(&ra_list, 0, sizeof(ra_list));
}

static void ra_buildList(const rc_api_fetch_game_data_response_t* game) {
    uint32_t count = 0;
    size_t pool_size = 0;

    for (uint32_t i=0; i<game->num_achievements; i++) {
        const rc_api_achievement_definition_t* ach = &game->achievements[i];
        if (!ra_isBaseSet(ach)) continue;
        count++;
        pool_size += (ach->title ? strlen(ach->title) : 0) + 1;
        pool_size += (ach->description ? strlen(ach->description) : 0) + 1;
    }
    if (!count) return;

    ra_list.items = calloc(count, sizeof(RA_Achievement));
    ra_list.pool  = malloc(pool_size);
    if (!ra_list.items || !ra_list.pool) {
        ra_freeList();
        return;
    }

    char* cursor = ra_list.pool;
    uint32_t at = 0;
    for (uint32_t i=0; i<game->num_achievements; i++) {
        const rc_api_achievement_definition_t* ach = &game->achievements[i];
        if (!ra_isBaseSet(ach)) continue;

        RA_Achievement* out = &ra_list.items[at++];
        out->id = ach->id;
        out->points = ach->points;
        out->unlocked = ra_isUnlocked(ach->id);

        const char* src = ach->title ? ach->title : "";
        size_t len = strlen(src);
        memcpy(cursor, src, len); cursor[len] = '\0';
        out->title = cursor;
        cursor += len + 1;

        src = ach->description ? ach->description : "";
        len = strlen(src);
        memcpy(cursor, src, len); cursor[len] = '\0';
        out->description = cursor;
        cursor += len + 1;
    }
    ra_list.count = at;
}

// The earned score. Recounted rather than accumulated so it can't drift out of
// step with the unlock list after a prune, a reload or a wipe; the set is a few
// hundred entries at most and this only runs when something actually changes.
static void ra_countPoints(void) {
    uint32_t points = 0;
    for (uint32_t i=0; i<ra_list.count; i++) {
        if (ra_list.items[i].unlocked) points += ra_list.items[i].points;
    }
    ra.points = points;
}

// Arms every base-set achievement that isn't already unlocked. The runtime
// must be freshly initialised; it won't deactivate anything.
static uint32_t ra_activateSet(const rc_api_fetch_game_data_response_t* game) {
    uint32_t activated = 0;
    for (uint32_t i=0; i<game->num_achievements; i++) {
        const rc_api_achievement_definition_t* ach = &game->achievements[i];

        if (!ra_isBaseSet(ach)) continue;
        if (ra_isUnlocked(ach->id)) continue;

        if (rc_runtime_activate_achievement(&ra.runtime, ach->id, ach->definition, NULL, 0)==RC_OK) {
            activated++;
        }
        else {
            LOG_warn("RA: skipping achievement %u (%s)\n", ach->id, ach->title ? ach->title : "");
        }
    }
    return activated;
}

///////////////////////////////////////
// public api

void RA_init(const char* root) {
    memset(&ra, 0, sizeof(ra));
    if (root) strncpy(ra.root, root, sizeof(ra.root)-1);
}

void RA_setMemoryFuncs(RA_GetMemoryData get_data, RA_GetMemorySize get_size) {
    ra.get_data = get_data;
    ra.get_size = get_size;
}

void RA_setMemoryMap(const struct retro_memory_map* mmap) {
    free(ra.descriptors);
    ra.descriptors = NULL;
    memset(&ra.mmap, 0, sizeof(ra.mmap));

    if (!mmap || !mmap->num_descriptors || !mmap->descriptors) return;

    size_t size = mmap->num_descriptors * sizeof(struct retro_memory_descriptor);
    ra.descriptors = malloc(size);
    if (!ra.descriptors) return;

    memcpy(ra.descriptors, mmap->descriptors, size);
    ra.mmap.descriptors = ra.descriptors;
    ra.mmap.num_descriptors = mmap->num_descriptors;

    // a map arriving mid-game invalidates whatever we resolved before
    if (ra.mem_ready) {
        rc_libretro_memory_destroy(&ra.regions);
        ra.mem_ready = 0;
    }
    ra.mem_tries = 0;
}

static uint32_t ra_consoleIdForTag(const char* tag) {
    // Tags are the part in parentheses of the Roms/ folder name, which is what
    // getEmuName() extracts -- ie. the *emulator* name, not the RA console name.
    // tools/ra_fetch.py carries the same table and must stay in sync with it.
    static const struct { const char* tag; uint32_t console_id; } map[] = {
        { "BMSX",      RC_CONSOLE_MSX },
        { "CPC",       RC_CONSOLE_AMSTRAD_PC },
        { "FBANEO",    RC_CONSOLE_ARCADE },
        { "FC",        RC_CONSOLE_NINTENDO },
        { "FUSE",      RC_CONSOLE_ZX_SPECTRUM },
        { "GB",        RC_CONSOLE_GAMEBOY },
        { "GBA",       RC_CONSOLE_GAMEBOY_ADVANCE },
        { "GBC",       RC_CONSOLE_GAMEBOY_COLOR },
        { "GG",        RC_CONSOLE_GAME_GEAR },
        { "HANDY",     RC_CONSOLE_ATARI_LYNX },
        { "KMMAME",    RC_CONSOLE_ARCADE },
        { "KMMAMECPS", RC_CONSOLE_ARCADE },
        { "MAME",      RC_CONSOLE_ARCADE },
        { "MD",        RC_CONSOLE_MEGA_DRIVE },
        { "MD32X",     RC_CONSOLE_SEGA_32X },
        { "MDCD",      RC_CONSOLE_SEGA_CD },
        { "N64",       RC_CONSOLE_NINTENDO_64 },
        { "NGP",       RC_CONSOLE_NEOGEO_POCKET },
        { "NGPC",      RC_CONSOLE_NEOGEO_POCKET },
        { "PCE",       RC_CONSOLE_PC_ENGINE },
        { "PKM",       RC_CONSOLE_POKEMON_MINI },
        { "PS",        RC_CONSOLE_PLAYSTATION },
        { "SFC",       RC_CONSOLE_SUPER_NINTENDO },
        { "SGB",       RC_CONSOLE_GAMEBOY },      // SGB plays GB roms
        { "SMS",       RC_CONSOLE_MASTER_SYSTEM },
        { "STELLA",    RC_CONSOLE_ATARI_2600 },
        { "SUPA",      RC_CONSOLE_SUPER_NINTENDO },
        { "T80",       RC_CONSOLE_TIC80 },
        { "VB",        RC_CONSOLE_VIRTUAL_BOY },
        { "VICE",      RC_CONSOLE_COMMODORE_64 },
        { "WATA",      RC_CONSOLE_SUPERVISION },
    };

    if (!tag) return RC_CONSOLE_UNKNOWN;

    for (size_t i=0; i<sizeof(map)/sizeof(map[0]); i++) {
        if (!strcasecmp(map[i].tag, tag)) return map[i].console_id;
    }
    return RC_CONSOLE_UNKNOWN;
}

int RA_loadGame(const char* emu_tag, const char* rom_path, const void* rom_data, size_t rom_size) {
    RA_unloadGame();

    if (!ra.root[0]) return 0;

    uint32_t console_id = ra_consoleIdForTag(emu_tag);
    if (console_id==RC_CONSOLE_UNKNOWN) {
        LOG_info("RA: no console mapping for tag '%s'\n", emu_tag ? emu_tag : "");
        return 0;
    }

    // 1. hash the rom the same way RA does (console specific header handling)
    rc_hash_iterator_t iterator;
    rc_hash_initialize_iterator(&iterator, rom_path, (const uint8_t*)rom_data, rom_size);

#ifdef RA_HAS_CHD
    // rcheevos' own cdreader only knows bin/cue and iso, so route .chd
    // through libchdr. Anything else keeps the default reader.
    if (chd_reader_is_chd(rom_path)) {
        iterator.callbacks.cdreader.open_track = chd_open_track;
        iterator.callbacks.cdreader.open_track_iterator = NULL;
        iterator.callbacks.cdreader.read_sector = chd_read_sector;
        iterator.callbacks.cdreader.first_track_sector = chd_first_track_sector;
        iterator.callbacks.cdreader.close_track = chd_close_track;
    }
#endif

    int hashed = rc_hash_generate(ra.hash, console_id, &iterator);
    rc_hash_destroy_iterator(&iterator);

    if (!hashed) {
        LOG_warn("RA: unable to hash %s\n", rom_path ? rom_path : "(null)");
        ra.hash[0] = '\0';
        return 0;
    }
    LOG_info("RA: hash %s\n", ra.hash);

    // 2. the set has to be on the card already. Sets are filed per console so
    // a big library doesn't end up as one directory with thousands of entries.
    snprintf(ra.data_path, sizeof(ra.data_path), "%s/data/%s/%s.json", ra.root, emu_tag, ra.hash);
    if (!ra_fileExists(ra.data_path)) {
        // tolerate a flat data/ folder from an older run of the fetch tool
        snprintf(ra.data_path, sizeof(ra.data_path), "%s/data/%s.json", ra.root, ra.hash);
    }

    // 3. reuse rcheevos' own parser for the `patch` response we downloaded
    rc_api_fetch_game_data_response_t game;
    if (!ra_readSet(ra.data_path, &game)) {
        LOG_info("RA: no usable achievement data for %s (%s)\n", ra.hash, emu_tag);
        ra.data_path[0] = '\0';
        ra.hash[0] = '\0';
        return 0;
    }

    // the json carries the authoritative console id, prefer it over the tag
    ra.console_id = game.console_id ? game.console_id : console_id;

    // needed before the progress file is read, so its checksum can be checked
    ra.set_check = ra_setChecksum(&game);

    // 4. restore what was already unlocked (mirrors the data/ layout)
    char progress_dir[MAX_PATH];
    snprintf(progress_dir, sizeof(progress_dir), "%s/progress", ra.root);
    mkdir(progress_dir, 0755); // neither level will exist on a first run
    snprintf(progress_dir, sizeof(progress_dir), "%s/progress/%s", ra.root, emu_tag);
    mkdir(progress_dir, 0755);

    snprintf(ra.progress_path, sizeof(ra.progress_path),
             "%s/%s_progress.json", progress_dir, ra.hash);
    ra_loadProgress();

    // 5. arm the runtime with everything still locked
    rc_runtime_init(&ra.runtime);
    ra_buildList(&game);
    ra.total = ra_list.count;
    ra_pruneUnlocked();
    ra_countPoints();

    uint32_t activated = ra_activateSet(&game);

    rc_api_destroy_fetch_game_data_response(&game);

    if (!ra.total) {
        LOG_info("RA: no core achievements in set\n");
        rc_runtime_destroy(&ra.runtime);
        ra_freeList();
        ra.hash[0] = '\0';
        ra.data_path[0] = '\0';
        ra.progress_path[0] = '\0';
        return 0;
    }

    ra.active = 1;
    LOG_info("RA: %u/%u unlocked, tracking %u\n", ra.unlocked_count, ra.total, activated);

    // put a well-formed file back on the card, so a rejected or outdated one
    // isn't re-read (and re-warned about) on every launch
    if (ra.progress_stale) ra_saveProgress();

    if (ra.invalidated) {
        snprintf(ra.msg, sizeof(ra.msg), "Progress file was modified - progress reset");
    }
    else {
        snprintf(ra.msg, sizeof(ra.msg), "%u   %u/%u", ra.points, ra.unlocked_count, ra.total);
    }
    ra.msg_frames = RA_MSG_FRAMES;
    ra.msg_id++;

    return 1;
}

void RA_unloadGame(void) {
    if (ra.active) {
        ra_saveProgress();
        rc_runtime_destroy(&ra.runtime);
    }
    if (ra.mem_ready) rc_libretro_memory_destroy(&ra.regions);

    ra_freeList();
    free(ra.unlocked);
    free(ra.descriptors);

    // keep root and the memory funcs; they outlive the game
    RA_GetMemoryData get_data = ra.get_data;
    RA_GetMemorySize get_size = ra.get_size;
    char root[MAX_PATH];
    memcpy(root, ra.root, sizeof(root));

    memset(&ra, 0, sizeof(ra));

    ra.get_data = get_data;
    ra.get_size = get_size;
    memcpy(ra.root, root, sizeof(ra.root));
}

void RA_doFrame(void) {
    if (!ra.active) return;

    if (ra.msg_frames>0) ra.msg_frames--;

    if (!ra.mem_ready) {
        // some cores only map their memory after a few retro_run() calls
        if (ra.mem_tries>=RA_MEM_MAX_TRIES) return;
        if (ra.frames % RA_MEM_RETRY_FRAMES) { ra.frames++; return; }
        ra.frames++;
        ra_initMemory();
        if (!ra.mem_ready) return;
    }

    rc_runtime_do_frame(&ra.runtime, ra_eventHandler, ra_readMemory, NULL, NULL);
}

void RA_resetRuntime(void) {
    if (ra.active) rc_runtime_reset(&ra.runtime);
}

int RA_wipeProgress(void) {
    if (!ra.active) return 0;

    // the file goes first: if anything below fails we still end up with a
    // clean slate on the next launch rather than a half-erased one
    if (ra.progress_path[0]) unlink(ra.progress_path);

    ra.unlocked_count = 0;
    ra.invalidated = 0;
    ra.progress_stale = 1; // nothing on disk now, so the next save must write
    for (uint32_t i=0; i<ra_list.count; i++) ra_list.items[i].unlocked = 0;
    ra.points = 0;

    // Earned achievements were either never activated or were deactivated the
    // moment they fired, so clearing the unlock list isn't enough to make them
    // earnable again -- the runtime has to be rebuilt from the set. Cheap
    // enough: this only ever runs from the menu.
    rc_runtime_destroy(&ra.runtime);
    rc_runtime_init(&ra.runtime);

    rc_api_fetch_game_data_response_t game;
    uint32_t activated = 0;
    if (ra_readSet(ra.data_path, &game)) {
        activated = ra_activateSet(&game);
        rc_api_destroy_fetch_game_data_response(&game);
    }
    else {
        // the set is gone or unreadable; progress is still wiped, but nothing
        // can be tracked until the next launch
        LOG_warn("RA: wiped progress but could not re-arm from %s\n", ra.data_path);
    }

    LOG_info("RA: progress wiped, tracking %u/%u\n", activated, ra.total);

    snprintf(ra.msg, sizeof(ra.msg), "0   0/%u", ra.total);
    ra.msg_frames = RA_MSG_FRAMES;
    ra.msg_id++;

    return activated>0;
}

const char* RA_getMessage(void) {
    return (ra.active && ra.msg_frames>0) ? ra.msg : NULL;
}

uint32_t RA_getMessageId(void) {
    return ra.msg_id;
}

int RA_isActive(void) {
    return ra.active;
}

void RA_getSummary(uint32_t* unlocked, uint32_t* total, uint32_t* points) {
    if (unlocked) *unlocked = ra.unlocked_count;
    if (total)    *total    = ra.total;
    if (points)   *points   = ra.points;
}

int RA_getAchievementCount(void) {
    return ra.active ? (int)ra_list.count : 0;
}

const RA_Achievement* RA_getAchievement(int index) {
    if (!ra.active || index<0 || index>=(int)ra_list.count) return NULL;
    return &ra_list.items[index];
}

void RA_refreshProgress(void) {
    if (!ra.active) return;

    for (uint32_t i=0; i<ra_list.count; i++) {
        RA_Achievement* ach = &ra_list.items[i];
        ach->progress[0] = '\0';

        // the runtime only measures achievements it is still tracking
        if (ach->unlocked) continue;

        rc_runtime_format_achievement_measured(&ra.runtime, ach->id,
                                               ach->progress, sizeof(ach->progress));
    }
}
