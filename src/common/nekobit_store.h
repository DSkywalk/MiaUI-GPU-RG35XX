#ifndef __NEKOBIT_STORE_H__
#define __NEKOBIT_STORE_H__

// NekoBit's storage, shared by minui (which owns the logic) and api.c (whose
// PWR_powerOff() calls NB_flushToCard() in every binary, minarch included).
//
// minui exits on every launch, so NekoBit's "RAM" is tmpfs: the state and the
// session in progress live in NB_TMP_DIR and never touch the card while
// playing. The card is written only by NB_flushToCard(): on power-off, and
// when the player asks for it ("Save now").
//
// Header only and libc only, so api.c can carry the hook without any binary's
// makefile changing.

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "defines.h"

#ifndef NB_TMP_DIR
#define NB_TMP_DIR "/tmp/nekobit"
#endif
#define NB_TMP_STATE   NB_TMP_DIR "/state.bin"
#define NB_TMP_SESSION NB_TMP_DIR "/session.bin"
#define NB_TMP_DIRTY   NB_TMP_DIR "/dirty" // exists while the card is behind

#define NB_SD_DIR    SHARED_USERDATA_PATH "/NekoBit"
#define NB_SD_STATE  NB_SD_DIR "/nekobit.bin"
#define NB_SD_BACKUP NB_SD_DIR "/nekobit.bak"   // the state before the last flush
#define NB_SD_ORPHAN NB_SD_DIR "/session.bin"   // a game that was running at power-off

#define NB_STATE_MAX (64 * 1024) // generous: the state is ~15 KB

#define NB_SESSION_MAGIC   0x5353424e // "NBSS"
#define NB_SESSION_VERSION 1
#define NB_SESSION_VIA_NEKOBIT 1 // launched from where a mission took the player

// One game launch, written by minui just before it exits to start the game and
// consumed by the next minui. Times are CLOCK_MONOTONIC (system wide, so it
// survives minui exiting, and immune to the date changing) plus the wall clock
// only to place achievement unlock dates, which minarch stamps with time().
typedef struct NB_Session {
    uint32_t magic;
    uint32_t version;
    uint64_t mono_start_ms;
    uint32_t wall_start;
    uint32_t ra_dir_mtime; // progress/<tag>/ mtime at launch, 0 = no such folder
    // filled in by NB_flushToCard() when the device powers off mid-game
    uint32_t end_valid;
    uint32_t elapsed_s;
    uint32_t wall_end;
    uint32_t flags; // NB_SESSION_*
    char rom[MAX_PATH];  // what the emulator was given
    char game[MAX_PATH]; // what the player picked: the m3u for multi-disc games
    char tag[64];        // emulator tag, as minarch's RA code files it
} NB_Session;

static inline uint64_t NB_monoMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static inline uint32_t NB_crc32(const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xffffffff;
    while (size--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
    }
    return ~crc;
}

// 0..size bytes read, -1 if the file can't be opened
static inline int NB_readFile(const char* path, void* buf, size_t size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, (char*)buf + got, size - got);
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    return (int)got;
}

// tmp file + rename, so a reader never sees half a file. `durable` adds the
// fsyncs a power cut needs (card only: tmpfs doesn't survive one anyway).
static inline int NB_writeFile(const char* path, const void* data, size_t size, int durable) {
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t put = 0;
    while (put < size) {
        ssize_t n = write(fd, (const char*)data + put, size - put);
        if (n <= 0) break;
        put += n;
    }
    if (durable) fsync(fd);
    close(fd);
    if (put != size || rename(tmp, path)) {
        unlink(tmp);
        return -1;
    }
    if (durable) {
        char dir[MAX_PATH];
        snprintf(dir, sizeof(dir), "%s", path);
        char* slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            int dfd = open(dir, O_RDONLY);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        }
    }
    return 0;
}

// tmpfs -> card, only what changed. Returns -1 on a failed write, else the
// number of files written (0 = the card was already up to date).
static inline int NB_flushToCard(void) {
    int written = 0;
    int failed = 0;
    static char buf[NB_STATE_MAX];

    // a game is running (we're inside minarch): stamp how long it has been
    // going, so the next boot can credit it
    NB_Session s;
    if (NB_readFile(NB_TMP_SESSION, &s, sizeof(s)) == (int)sizeof(s) && s.magic == NB_SESSION_MAGIC) {
        s.end_valid = 1;
        s.elapsed_s = (uint32_t)((NB_monoMs() - s.mono_start_ms) / 1000);
        s.wall_end = (uint32_t)time(NULL);
        mkdir(NB_SD_DIR, 0755);
        if (NB_writeFile(NB_SD_ORPHAN, &s, sizeof(s), 1) == 0) written++;
        else failed = 1;
    }

    if (access(NB_TMP_DIRTY, F_OK) == 0) {
        int size = NB_readFile(NB_TMP_STATE, buf, sizeof(buf));
        if (size > 0) {
            mkdir(NB_SD_DIR, 0755);
            rename(NB_SD_STATE, NB_SD_BACKUP); // the loader falls back to it
            if (NB_writeFile(NB_SD_STATE, buf, size, 1) == 0) {
                unlink(NB_TMP_DIRTY);
                written++;
            }
            else failed = 1;
        }
    }
    return failed ? -1 : written;
}

#endif
