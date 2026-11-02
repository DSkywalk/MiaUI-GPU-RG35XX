#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

int prefixMatch(char* pre, char* str);
int suffixMatch(char* suf, char* str);
int exactMatch(char* str1, char* str2);
int hide(char* file_name);

int getDisplayName(const char* in_name, char* out_name);
const char* getMameName(int index);
void getEmuName(const char* in_name, char* out_name);
void getEmuPath(char* emu_name, char* pak_path);

// RetroAchievements knows every arcade romset as one system (the hash is the
// romset's name, rc_hash_arcade), which this card splits over several Roms
// folders so each set runs on the core it suits. Tags compare without case.
// Keep in step with ra_fetch.py's CONSOLES and the Rust tool's.
extern const char* const RA_ARCADE_TAGS[]; // NULL-terminated
int isArcadeTag(const char* tag);
int sameRaSystem(const char* a, const char* b); // the same tag, or both arcade

void normalizeNewline(char* line);
void trimTrailingNewlines(char* line);
void trimSortingMeta(char** str);

int exists(char* path);
void touch(char* path);
void putFile(char* path, char* contents);
char* allocFile(char* path); // caller must free
void getFile(char* path, char* buffer, size_t buffer_size);
void putInt(char* path, int value);
int getInt(char* path);

uint64_t getMicroseconds(void);

#endif
