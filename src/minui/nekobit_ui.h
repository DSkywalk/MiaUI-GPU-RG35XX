#ifndef __NEKOBIT_UI_H__
#define __NEKOBIT_UI_H__

// NekoBit on screen: the kaomoji pill centred on the root menu's button bar
// (no hint: an easter egg), the overlay SELECT summons, and the banners after
// a game, along the bottom. GFX_* only.

#include "api.h"
#include "nekobit.h"

enum {
    NBUI_NONE,
    NBUI_CLOSE, // the overlay closed
    NBUI_GOTO,  // ...and minui should navigate to `target`
};

typedef struct NBUI_Action {
    int kind;
    NB_Target target;
} NBUI_Action;

void NBUI_init(void); // after GFX_init

int NBUI_isOpen(void);
void NBUI_open(const NB_Library* lib); // lib must stay valid while open
int NBUI_input(NBUI_Action* action);   // after PAD_poll, while open: 1 = redraw
void NBUI_draw(SDL_Surface* screen, int show_setting); // while open, instead of the list

// The pill on the button bar: centred, or centred between x = left and
// x = right (the two button groups) if that's in the way. Drawn while
// NekoBit is out or moving.
void NBUI_drawFace(SDL_Surface* screen, int left, int right);

// SELECT pressed on the root. Out already, NekoBit hops. Hiding, it rises
// from the bottom edge while SELECT stays held, and sinks back if it's let
// go too soon. NBUI_SUMMONED comes back from NBUI_update once it's fully in
// view, which is when the overlay should open.
void NBUI_summon(uint32_t now);
int NBUI_busy(void); // a summon is under way: hold the menu's input

enum {
    NBUI_REDRAW = 1,
    NBUI_SUMMONED = 2,
};
// Every frame: banners, animations and NekoBit's own whims (peeking in when
// it arrives, the odd hop). `on_root`: the root list is what's on screen.
int NBUI_update(uint32_t now, int on_root);
void NBUI_drawBanner(SDL_Surface* screen);

#endif
