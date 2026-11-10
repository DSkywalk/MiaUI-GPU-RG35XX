#ifndef __QUICKMENU_H__
#define __QUICKMENU_H__

// The Quick Menu MENU opens on the main menu: a row of round icons (Settings,
// Achievements, Network, Clock, Collections, SD Tool, About) with the time above them, and
// the pages behind them. GFX_* only, like nekobit_ui.

#include "api.h"

enum {
    QM_NONE,
    QM_CLOSED, // the menu closed
    QM_COLLECTIONS, // ...and minui should open the Collections manager (CM_openManager)
};

// After GFX_init. Works out the USB mode if this boot hasn't yet (adbd started
// by hand, or by the old ADB tool), so the battery pill can show it.
void QM_init(void);

int QM_isOpen(void);
void QM_open(void);
int QM_input(int* action); // after PAD_poll, while open: 1 = redraw
void QM_draw(SDL_Surface* screen, int show_setting); // while open, instead of the list

// Every frame, open or not: the clock's minute, ADB being switched on, the
// USB address showing up. 1 = redraw.
int QM_update(void);

// Kids Mode is simple_mode's flag file: Tools and the system options hide
int QM_kidsMode(void);

#endif
