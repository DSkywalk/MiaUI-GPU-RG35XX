#ifndef __COLLECTIONS_H__
#define __COLLECTIONS_H__

// The collections one game is in: every list in COLLECTIONS_PATH with a box
// ticked if the game is in it, A to put it in or take it out, and a
// last row that names a new list on the on-screen keyboard (the game goes
// straight into it). Opened by Y while SELECT is held on a game. GFX_* only,
// like quickmenu.c.
//
// A collection is a .txt with one path per line, without SDCARD_PATH, as
// getCollection() in minui.c reads them.

#include "api.h"

// path: the game as a list holds it (a rom, a pak, or a multi-disc game's
// .cue / .m3u), with SDCARD_PATH. name: what the title shows.
void CM_open(const char* path, const char* name);
// The Quick Menu's: every list with how many games it has; A opens one (X
// takes a game out of it), X deletes one (its games stay), and the first row
// makes a new empty one.
void CM_openManager(void);
int CM_isOpen(void);
int CM_input(void); // after PAD_poll, while open: 1 = redraw
void CM_draw(SDL_Surface* dst, int show_setting, int hardware_w); // the battery pill's width, from GFX_blitHardwareGroup

// Once it has closed: did a list change? Then minui reloads what it shows (a
// first list brings Collections to the root, the list open may have lost the
// game). Reading it clears it.
int CM_changed(void);

#endif
