#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "i18n.h"
#include "nekobit.h"
#include "nekobit_ui.h"

// Laid out like minui's own list: pill rows on the left, a card on the right
// where the art would be, button hints along the bottom. On the home screen
// the list takes only what its labels need and the card (what NekoBit says,
// its level and stats) gets the rest; the mission lists get 60/40.
#define NBUI_ROWS MAIN_ROW_COUNT
#define NBUI_BADGE_UI   COLOR_GRAY      // NekoBit's face and banners: on the UI
#define NBUI_BADGE_MAIN (SDL_Color){TRIAD_DARK_GRAY} // NekoBit's face and banners: on main menu

// ASSET_TROPHY as minarch's achievements list draws it: the atlas' 10x10
// (logical px) at 1:1, the text 1px after the cell
#define TROPHY_SIZE 10
#define TROPHY_GAP 1
#define BANNER_MS 2600
#define FACE_FONT_SIZE 24

enum { SCR_HOME, SCR_MISSIONS, SCR_ACTIONS, SCR_OFFERS, SCR_HISTORY, SCR_CONFIRM, SCR_MESSAGE, SCR_COUNT };
enum { HOME_MISSIONS, HOME_NEW, HOME_HISTORY, HOME_SAVE, HOME_RESET, HOME_COUNT };
enum { ACT_GO, ACT_CANCEL };
enum { CONFIRM_CANCEL, CONFIRM_RESET, CONFIRM_RESET2 };

static TTF_Font* face_font = NULL;

static struct {
    int open;
    int screen;
    int back; // where a dialog returns to
    int sel[SCR_COUNT];
    int start[SCR_COUNT];
    const NB_Library* lib;
    NB_Mission offers[NB_MAX_OFFERS];
    int offer_count;
    int slots[NB_MAX_MISSIONS]; // the active missions, in slot order
    int slot_count;
    int act_slot;
    int acts[2];
    int act_count;
    int confirm;
    int message; // string id
    int woken;   // SELECT woke it up: sleepy until the overlay closes
    char speech[256];
} ui;

static struct {
    int showing;
    uint32_t until;
    NB_Event ev;
} banner;

// NekoBit's moves, all on the pill's y: PEEK slides it up from below the
// bottom edge with a little overshoot, HOP is a small jump in place, RISE
// follows SELECT: up slowly while it's held, back down if let go too soon
enum { ANIM_NONE, ANIM_PEEK, ANIM_HOP, ANIM_RISE };
#define PEEK_MS 420
#define HOP_MS 260
#define RISE_MS 1200 // all the way up, SELECT held
#define SINK_MS 450  // all the way down again
static struct {
    int kind;
    uint32_t start;
    uint32_t ms;
    int summon;  // open the overlay when it's done
    int arrival; // NB_arrived(): peek in once the menu is showing
    int woken;   // summoned while hiding: it comes up sleepy
    float rise;  // ANIM_RISE: 0 hidden .. 1 all the way up
    uint32_t last; // ANIM_RISE: when it last moved
    uint32_t next_hop;
} anim;

static void nbui_animate(int kind, uint32_t now, int summon) {
    anim.kind = kind;
    anim.start = now;
    anim.ms = kind == ANIM_PEEK ? PEEK_MS : HOP_MS;
    anim.summon = summon;
}

// how far below its resting place the pill is, in pixels (negative = above)
static int nbui_offset(uint32_t now) {
    if (anim.kind == ANIM_NONE) return 0;
    if (anim.kind == ANIM_RISE) { // ease-out on the way up, so even a tap peeks
        float u = 1 - anim.rise;
        return (int)(u * u * u * SCALE1(PADDING + PILL_SIZE + 2));
    }
    float t = (float)(now - anim.start) / anim.ms;
    if (t > 1) t = 1;
    if (anim.kind == ANIM_PEEK) {
        const float c1 = 1.70158f, c3 = c1 + 1; // ease-out-back
        float u = t - 1;
        float p = 1 + c3 * u * u * u + c1 * u * u;
        return (int)((1 - p) * SCALE1(PADDING + PILL_SIZE + 2));
    }
    return -(int)(sinf(t * 3.14159265f) * SCALE1(6));
}

static int nbui_min(int a, int b) { return a < b ? a : b; }
static int nbui_max(int a, int b) { return a > b ? a : b; }

static const char* nbui_face(void) {
    return ui.woken ? NB_sleepyFace() : NB_face(NB_state()->mood);
}

void NBUI_init(void) {
    srand(SDL_GetTicks() ^ (unsigned)time(NULL));
    anim.arrival = NB_arrived();
    // the cache keys text by font, so this stays open until the process ends
    face_font = TTF_OpenFont(FONT_PATH, SCALE1(FACE_FONT_SIZE));
    if (!face_font) face_font = font.large;
}

int NBUI_isOpen(void) {
    return ui.open;
}

static void nbui_refreshSlots(void) {
    const NB_Save* s = NB_state();
    ui.slot_count = 0;
    for (int i = 0; i < NB_MAX_MISSIONS; i++)
        if (s->missions[i].type != NB_MIS_NONE) ui.slots[ui.slot_count++] = i;
    if (ui.sel[SCR_MISSIONS] >= ui.slot_count) ui.sel[SCR_MISSIONS] = nbui_max(0, ui.slot_count - 1);
    if (ui.start[SCR_MISSIONS] > ui.sel[SCR_MISSIONS]) ui.start[SCR_MISSIONS] = ui.sel[SCR_MISSIONS];
}

void NBUI_open(const NB_Library* lib) {
    NB_interact(); // talked to: it stays for the rest of the boot
    memset(ui.sel, 0, sizeof(ui.sel));
    memset(ui.start, 0, sizeof(ui.start));
    ui.lib = lib;
    ui.screen = SCR_HOME;
    ui.open = 1;
    ui.woken = anim.woken;
    anim.woken = 0;
    snprintf(ui.speech, sizeof(ui.speech), "%s", ui.woken ? NB_wokenGreeting() : NB_greeting());
    nbui_refreshSlots();
}

static void nbui_close(void) {
    ui.open = 0;
    ui.woken = 0; // awake now: back to its mood's face
    ui.lib = NULL;
}

///////////////////////////////////////
// lists

static int nbui_historyCount(void) {
    return nbui_min(NB_state()->history_count, NB_MAX_HISTORY);
}
static const NB_History* nbui_history(int i) { // newest first
    const NB_Save* s = NB_state();
    return &s->history[(s->history_count - 1 - i) % NB_MAX_HISTORY];
}
static void nbui_historyText(const NB_History* h, char* out, size_t size, int label) {
    NB_Mission m;
    memset(&m, 0, sizeof(m));
    m.type = h->type;
    m.flavor = h->flavor;
    m.target = h->target;
    snprintf(m.title, sizeof(m.title), "%s", h->title);
    if (label) NB_label(&m, out, size);
    else NB_describe(&m, out, size);
}

static int nbui_count(int screen) {
    switch (screen) {
    case SCR_HOME: return HOME_COUNT;
    case SCR_MISSIONS: return ui.slot_count;
    case SCR_ACTIONS: return ui.act_count;
    case SCR_OFFERS: return ui.offer_count;
    case SCR_HISTORY: return nbui_historyCount();
    }
    return 0;
}

static void nbui_label(int screen, int i, char* out, size_t size) {
    const NB_Save* s = NB_state();
    out[0] = '\0';
    switch (screen) {
    case SCR_HOME: {
        static const int ids[HOME_COUNT] = {STR_NB_MENU_MISSIONS, STR_NB_MENU_NEW, STR_NB_MENU_HISTORY, STR_NB_MENU_SAVE, STR_NB_MENU_RESET};
        snprintf(out, size, T(ids[i]), ui.slot_count, NB_MAX_MISSIONS);
        break;
    }
    case SCR_MISSIONS: NB_label(&s->missions[ui.slots[i]], out, size); break;
    case SCR_ACTIONS:
        if (ui.acts[i] == ACT_GO) {
            NB_Target t;
            NB_target(&s->missions[ui.act_slot], &t);
            snprintf(out, size, "%s", T(t.select[0] ? STR_NB_GO_GAME : STR_NB_GO_SYSTEM));
        }
        else snprintf(out, size, "%s", T(STR_NB_CANCEL_MISSION));
        break;
    case SCR_OFFERS: NB_label(&ui.offers[i], out, size); break;
    case SCR_HISTORY: nbui_historyText(nbui_history(i), out, size, 1); break;
    }
}

///////////////////////////////////////
// input

static void nbui_dialog(int screen, int what, int back) {
    ui.screen = screen;
    ui.back = back;
    if (screen == SCR_CONFIRM) ui.confirm = what;
    else ui.message = what;
}

static void nbui_goto(NBUI_Action* action, const NB_Mission* m) {
    if (NB_target(m, &action->target)) action->kind = NBUI_GOTO;
    else action->kind = NBUI_CLOSE;
    nbui_close();
}

static void nbui_activate(NBUI_Action* action) {
    const NB_Save* s = NB_state();
    int sel = ui.sel[ui.screen];
    switch (ui.screen) {
    case SCR_HOME:
        switch (sel) {
        case HOME_MISSIONS:
            nbui_refreshSlots();
            ui.screen = SCR_MISSIONS;
            break;
        case HOME_NEW:
            if (!NB_freeSlots()) {
                nbui_dialog(SCR_MESSAGE, STR_NB_SLOTS_FULL, SCR_HOME);
                break;
            }
            uint64_t started = getMicroseconds();
            ui.offer_count = NB_offers(ui.lib, ui.offers, NB_MAX_OFFERS);
            LOG_info("NekoBit: %i offers in %ims\n", ui.offer_count, (int)((getMicroseconds() - started) / 1000));
            ui.sel[SCR_OFFERS] = ui.start[SCR_OFFERS] = 0;
            if (ui.offer_count) ui.screen = SCR_OFFERS;
            else nbui_dialog(SCR_MESSAGE, STR_NB_NO_OFFERS, SCR_HOME);
            break;
        case HOME_HISTORY:
            ui.sel[SCR_HISTORY] = ui.start[SCR_HISTORY] = 0;
            ui.screen = SCR_HISTORY;
            break;
        case HOME_SAVE:
            NB_saveNow(); // its banner says how it went
            break;
        case HOME_RESET:
            nbui_dialog(SCR_CONFIRM, CONFIRM_RESET, SCR_HOME);
            break;
        }
        break;
    case SCR_MISSIONS:
        if (!ui.slot_count) break;
        ui.act_slot = ui.slots[sel];
        ui.act_count = 0;
        NB_Target t;
        if (NB_target(&s->missions[ui.act_slot], &t)) ui.acts[ui.act_count++] = ACT_GO;
        ui.acts[ui.act_count++] = ACT_CANCEL;
        ui.sel[SCR_ACTIONS] = ui.start[SCR_ACTIONS] = 0;
        ui.screen = SCR_ACTIONS;
        break;
    case SCR_ACTIONS:
        if (ui.acts[sel] == ACT_GO) nbui_goto(action, &s->missions[ui.act_slot]);
        else nbui_dialog(SCR_CONFIRM, CONFIRM_CANCEL, SCR_ACTIONS);
        break;
    case SCR_OFFERS: {
        int slot = NB_accept(&ui.offers[sel], ui.lib);
        if (slot == -1) nbui_dialog(SCR_MESSAGE, STR_NB_SLOTS_FULL, SCR_HOME);
        else if (slot < 0) nbui_dialog(SCR_MESSAGE, STR_NB_NO_OFFERS, SCR_OFFERS);
        else nbui_goto(action, &s->missions[slot]); // straight to where it's played
        break;
    }
    }
}

static void nbui_confirmed(void) {
    switch (ui.confirm) {
    case CONFIRM_CANCEL:
        NB_cancel(ui.act_slot);
        nbui_refreshSlots();
        ui.screen = SCR_MISSIONS;
        break;
    case CONFIRM_RESET:
        nbui_dialog(SCR_CONFIRM, CONFIRM_RESET2, SCR_HOME);
        break;
    case CONFIRM_RESET2:
        NB_reset();
        nbui_refreshSlots();
        ui.screen = SCR_HOME;
        break;
    }
}

int NBUI_input(NBUI_Action* action) {
    action->kind = NBUI_NONE;
    if (!ui.open) return 0;

    if (ui.screen == SCR_CONFIRM || ui.screen == SCR_MESSAGE) {
        if (PAD_justPressed(BTN_A)) {
            if (ui.screen == SCR_CONFIRM) nbui_confirmed();
            else ui.screen = ui.back;
            return 1;
        }
        if (PAD_justPressed(BTN_B)) {
            ui.screen = ui.back;
            return 1;
        }
        return 0;
    }

    int dirty = 0;
    int count = nbui_count(ui.screen);
    int* sel = &ui.sel[ui.screen];
    int* start = &ui.start[ui.screen];
    if (count && PAD_justRepeated(BTN_UP)) {
        if (*sel > 0) *sel -= 1;
        else if (PAD_justPressed(BTN_UP)) *sel = count - 1; // wraps on a press, stops on a repeat
        dirty = 1;
    }
    else if (count && PAD_justRepeated(BTN_DOWN)) {
        if (*sel < count - 1) *sel += 1;
        else if (PAD_justPressed(BTN_DOWN)) *sel = 0;
        dirty = 1;
    }
    if (*sel < *start) *start = *sel;
    if (*sel >= *start + NBUI_ROWS) *start = *sel - NBUI_ROWS + 1;

    if (PAD_justPressed(BTN_SELECT)) {
        nbui_close();
        action->kind = NBUI_CLOSE;
        return 1;
    }
    if (PAD_justPressed(BTN_B)) {
        switch (ui.screen) {
        case SCR_HOME:
            nbui_close();
            action->kind = NBUI_CLOSE;
            break;
        case SCR_ACTIONS: ui.screen = SCR_MISSIONS; break;
        default: ui.screen = SCR_HOME; break;
        }
        return 1;
    }
    if (PAD_justPressed(BTN_A)) {
        nbui_activate(action);
        return 1;
    }
    if (ui.screen == SCR_OFFERS && PAD_justPressed(BTN_Y)) {
        ui.offer_count = NB_offers(ui.lib, ui.offers, NB_MAX_OFFERS);
        *sel = *start = 0;
        if (!ui.offer_count) nbui_dialog(SCR_MESSAGE, STR_NB_NO_OFFERS, SCR_HOME);
        return 1;
    }
    return dirty;
}

///////////////////////////////////////
// drawing

// in, cut to fit w with "..." (on a UTF-8 boundary). Returns its width.
static int nbui_fit(TTF_Font* f, const char* in, char* out, size_t size, int w) {
    snprintf(out, size, "%s", in);
    int tw;
    TTF_SizeUTF8(f, out, &tw, NULL);
    if (tw <= w) return tw;
    int len = strlen(out);
    while (len > 0) {
        do len--; while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80);
        snprintf(out + len, size - len, "...");
        TTF_SizeUTF8(f, out, &tw, NULL);
        if (tw <= w) break;
    }
    return tw;
}

#define NBUI_MAX_LINES 12
typedef struct {
    char line[NBUI_MAX_LINES][192];
    int count;
} NBUI_Lines;

// the last line there's room for, with "..." because there was more
static int nbui_clip(TTF_Font* f, NBUI_Lines* out, const char* line, int w) {
    char clipped[200];
    snprintf(clipped, sizeof(clipped), "%s...", line);
    nbui_fit(f, clipped, out->line[out->count++], sizeof(out->line[0]), w);
    return out->count;
}

// Word wrap, paragraph by paragraph ('\n'), at most max_lines: the last one
// ends in "..." if there was more. A word wider than w is cut to fit.
static int nbui_wrap(TTF_Font* f, const char* str, int w, int max_lines, NBUI_Lines* out) {
    if (max_lines > NBUI_MAX_LINES) max_lines = NBUI_MAX_LINES;
    out->count = 0;
    if (max_lines < 1) return 0;
    char text[MAX_PATH];
    snprintf(text, sizeof(text), "%s", str);
    for (char* para = text; para;) {
        char* next = strchr(para, '\n');
        if (next) *next++ = '\0';
        char line[192] = "";
        char* save;
        for (char* word = strtok_r(para, " ", &save); word; word = strtok_r(NULL, " ", &save)) {
            char longer[192];
            snprintf(longer, sizeof(longer), line[0] ? "%s %s" : "%s%s", line, word);
            int tw;
            TTF_SizeUTF8(f, longer, &tw, NULL);
            if (tw <= w || !line[0]) {
                snprintf(line, sizeof(line), "%s", longer);
                continue;
            }
            if (out->count == max_lines - 1) return nbui_clip(f, out, line, w);
            snprintf(out->line[out->count++], sizeof(out->line[0]), "%s", line);
            snprintf(line, sizeof(line), "%s", word);
        }
        if (next && out->count == max_lines - 1) return nbui_clip(f, out, line, w);
        nbui_fit(f, line, out->line[out->count++], sizeof(out->line[0]), w);
        para = next;
    }
    return out->count;
}

static int nbui_lines(TTF_Font* f, const char* str, int w, int max_lines) {
    NBUI_Lines lines;
    return nbui_wrap(f, str, w, max_lines, &lines);
}

// Left aligned (or centred), wrapped to w, max_lines in all. Returns the
// height drawn.
static int nbui_text(SDL_Surface* dst, TTF_Font* f, const char* str, SDL_Color color, int x, int y, int w, int centred, int max_lines) {
    NBUI_Lines lines;
    nbui_wrap(f, str, w, max_lines, &lines);
    int line_h = TTF_FontLineSkip(f);
    for (int i = 0; i < lines.count; i++) {
        if (!lines.line[i][0]) continue;
        GFX_Text text = GFX_renderText(f, lines.line[i], color);
        GFX_blitRenderedText(&text, NULL, dst, centred ? x + (w - text.w) / 2 : x, y + i * line_h);
    }
    return lines.count * line_h;
}

static void nbui_bar(SDL_Surface* dst, int x, int y, int w, uint32_t value, uint32_t max) {
    int h = SCALE1(SETTINGS_SIZE);
    GFX_blitPill(ASSET_BAR_BG, dst, &(SDL_Rect){x, y, w, h});
    if (!max || !value) return;
    int fill = (int)((uint64_t)w * (value < max ? value : max) / max);
    GFX_blitPill(ASSET_BAR, dst, &(SDL_Rect){x, y, nbui_max(fill, h), h});
}

// an achievement mission's row starts with the trophy
static int nbui_trophy(int screen, int i) {
    const NB_Save* s = NB_state();
    switch (screen) {
    case SCR_MISSIONS: return NB_isAchievement(s->missions[ui.slots[i]].type);
    case SCR_OFFERS: return NB_isAchievement(ui.offers[i].type);
    case SCR_HISTORY: return NB_isAchievement(nbui_history(i)->type);
    }
    return 0;
}

static void nbui_rows(SDL_Surface* dst, int width) {
    int count = nbui_count(ui.screen);
    int sel = ui.sel[ui.screen];
    int start = ui.start[ui.screen];
    for (int j = 0, i = start; j < NBUI_ROWS && i < count; j++, i++) {
        char label[256], fit[256];
        nbui_label(ui.screen, i, label, sizeof(label));
        int icon = nbui_trophy(ui.screen, i) ? SCALE1(TROPHY_SIZE + TROPHY_GAP) : 0;
        int w = nbui_fit(font.large, label, fit, sizeof(fit), width - SCALE1(BUTTON_PADDING * 2) - icon) + SCALE1(BUTTON_PADDING * 2) + icon;
        int y = SCALE1(PADDING + j * PILL_SIZE);
        SDL_Color color = COLOR_WHITE;
        if (i == sel) {
            GFX_blitPill(ASSET_WHITE_PILL, dst, &(SDL_Rect){SCALE1(PADDING), y, nbui_min(w, width), SCALE1(PILL_SIZE)});
            color = COLOR_BLACK;
        }
        int x = SCALE1(PADDING + BUTTON_PADDING);
        if (icon) { // black on the white pill, white elsewhere
            GFX_blitAssetColor(ASSET_TROPHY, NULL, dst, &(SDL_Rect){x, y + (SCALE1(PILL_SIZE) - SCALE1(TROPHY_SIZE)) / 2}, color);
            x += icon;
        }
        GFX_Text text = GFX_renderText(font.large, fit, color);
        GFX_blitRenderedText(&text, NULL, dst, x, y + SCALE1(4));
    }
}

// Its level and stats; the card is just as tall as they need (draw 0: only
// measure). Returns that height.
static int nbui_cardHome(SDL_Surface* dst, SDL_Rect* card, int draw) {
    const NB_Save* s = NB_state();
    int pad = SCALE1(PADDING);
    int x = card->x + pad;
    int w = card->w - pad * 2;
    int tiny_h = TTF_FontLineSkip(font.tiny);
    int small_h = TTF_FontLineSkip(font.small);
    char level[128], xp[64], played[64], clock_s[32], missions[64], achievements[64], counts[160];

    snprintf(level, sizeof(level), T(STR_NB_LEVEL), (int)s->level);
    snprintf(level + strlen(level), sizeof(level) - strlen(level), " - %s", T(NB_moodString(s->mood)));
    snprintf(xp, sizeof(xp), T(STR_NB_XP), (int)s->xp, (int)s->xp_next);
    NB_formatTime(s->play_clock, clock_s, sizeof(clock_s));
    snprintf(played, sizeof(played), T(STR_NB_STAT_PLAYED), clock_s);
    snprintf(missions, sizeof(missions), T(STR_NB_STAT_MISSIONS), (int)s->missions_completed);
    snprintf(achievements, sizeof(achievements), T(STR_NB_STAT_ACH), (int)s->achievements);
    snprintf(counts, sizeof(counts), "%s · %s", missions, achievements);
    int tw;
    TTF_SizeUTF8(font.tiny, counts, &tw, NULL);
    int one_line = tw <= w; // missions and achievements share a line if they fit

    int h = SCALE1(8) + small_h + SCALE1(2 + SETTINGS_SIZE + 2) + tiny_h + SCALE1(4) + tiny_h * (one_line ? 2 : 3) + SCALE1(8);
    if (!draw) return h;

    int y = card->y + SCALE1(8);
    y += nbui_text(dst, font.small, level, COLOR_WHITE, x, y, w, 1, 1) + SCALE1(2);
    nbui_bar(dst, x, y, w, s->xp, s->xp_next);
    y += SCALE1(SETTINGS_SIZE + 2);
    y += nbui_text(dst, font.tiny, xp, COLOR_LIGHT_TEXT, x, y, w, 1, 1) + SCALE1(4);
    y += nbui_text(dst, font.tiny, played, COLOR_WHITE, x, y, w, 1, 1);
    if (one_line) nbui_text(dst, font.tiny, counts, COLOR_WHITE, x, y, w, 1, 1);
    else {
        y += nbui_text(dst, font.tiny, missions, COLOR_WHITE, x, y, w, 1, 1);
        nbui_text(dst, font.tiny, achievements, COLOR_WHITE, x, y, w, 1, 1);
    }
    return h;
}

// The description gets whatever the rest leaves: in the small font if it
// fits there, else in the tiny one
static int nbui_description(SDL_Surface* dst, const char* text, int x, int y, int w, int room) {
    int small_h = TTF_FontLineSkip(font.small);
    int tiny_h = TTF_FontLineSkip(font.tiny);
    int n = nbui_lines(font.small, text, w, NBUI_MAX_LINES);
    if (n * small_h <= room) return nbui_text(dst, font.small, text, COLOR_WHITE, x, y, w, 0, n);
    return nbui_text(dst, font.tiny, text, COLOR_WHITE, x, y, w, 0, nbui_max(1, room / tiny_h));
}

static void nbui_cardMission(SDL_Surface* dst, SDL_Rect* card, const NB_Mission* m, int accepted) {
    int pad = SCALE1(PADDING);
    int x = card->x + pad;
    int w = card->w - pad * 2;
    int y = card->y + SCALE1(8);
    int tiny_h = TTF_FontLineSkip(font.tiny);
    int small_h = TTF_FontLineSkip(font.small);
    char desc[256], sys[160], progress[128], set[64], reward[64];

    NB_describe(m, desc, sizeof(desc));
    sys[0] = progress[0] = set[0] = '\0';
    if (m->system[0] && strcmp(m->system, m->title)) snprintf(sys, sizeof(sys), "%s", m->system);
    int is_play = m->type >= NB_MIS_PLAY_GAME && m->type <= NB_MIS_PLAY_ANY;
    if (accepted) {
        if (is_play) {
            char done[32], goal[32];
            NB_formatTime(m->progress, done, sizeof(done));
            NB_formatTime((uint32_t)m->target * 60, goal, sizeof(goal));
            snprintf(progress, sizeof(progress), T(STR_NB_PROGRESS_TIME), done, goal);
        }
        else snprintf(progress, sizeof(progress), T(STR_NB_PROGRESS_ACH), (int)m->progress, (int)m->target);
    }
    if (m->type == NB_MIS_ACH_GAME && m->ra_total) // how far into the set
        snprintf(set, sizeof(set), T(STR_NB_M_ACH_PROGRESS), (int)m->ra_unlocked, (int)m->ra_total);
    snprintf(reward, sizeof(reward), T(STR_NB_REWARD), (int)m->xp);

    // measure everything under the description first
    int system_n = sys[0] ? nbui_lines(font.tiny, sys, w, 2) : 0;
    int progress_n = progress[0] ? nbui_lines(font.tiny, progress, w, 2) : 0;
    int set_n = set[0] ? nbui_lines(font.tiny, set, w, 2) : 0;
    int below = SCALE1(2) + system_n * tiny_h + SCALE1(6)
              + (progress_n ? SCALE1(SETTINGS_SIZE + 4) + progress_n * tiny_h + SCALE1(4) : 0)
              + (set_n ? set_n * tiny_h + SCALE1(SETTINGS_SIZE + 8) : 0)
              + small_h;
    y += nbui_description(dst, desc, x, y, w, card->y + card->h - SCALE1(8) - below - y) + SCALE1(2);

    if (system_n) y += nbui_text(dst, font.tiny, sys, COLOR_LIGHT_TEXT, x, y, w, 0, 2);
    y += SCALE1(6);
    if (progress_n) {
        if (is_play) nbui_bar(dst, x, y, w, m->progress, (uint32_t)m->target * 60);
        else nbui_bar(dst, x, y, w, m->progress, m->target);
        y += SCALE1(SETTINGS_SIZE + 4);
        y += nbui_text(dst, font.tiny, progress, COLOR_WHITE, x, y, w, 0, 2) + SCALE1(4);
    }
    if (set_n) {
        y += nbui_text(dst, font.tiny, set, COLOR_LIGHT_TEXT, x, y, w, 0, 2);
        nbui_bar(dst, x, y + SCALE1(2), w, m->ra_unlocked, m->ra_total);
        y += SCALE1(SETTINGS_SIZE + 8);
    }
    nbui_text(dst, font.small, reward, COLOR_WHITE, x, y, w, 0, 1);
}

static void nbui_cardHistory(SDL_Surface* dst, SDL_Rect* card, const NB_History* h) {
    int pad = SCALE1(PADDING);
    int x = card->x + pad;
    int w = card->w - pad * 2;
    int y = card->y + SCALE1(8);
    int tiny_h = TTF_FontLineSkip(font.tiny);
    int small_h = TTF_FontLineSkip(font.small);
    char desc[256], earned[64], done[160], when[32];
    nbui_historyText(h, desc, sizeof(desc), 0);
    snprintf(earned, sizeof(earned), T(STR_NB_EARNED), (int)h->xp);
    NB_formatTime(h->done_at, when, sizeof(when));
    snprintf(done, sizeof(done), T(STR_NB_DONE_AFTER), when);
    int done_n = nbui_lines(font.tiny, done, w, 3);
    int below = SCALE1(8) + small_h + SCALE1(4) + done_n * tiny_h;
    y += nbui_description(dst, desc, x, y, w, card->y + card->h - SCALE1(8) - below - y) + SCALE1(8);
    y += nbui_text(dst, font.small, earned, COLOR_WHITE, x, y, w, 0, 1) + SCALE1(4);
    nbui_text(dst, font.tiny, done, COLOR_LIGHT_TEXT, x, y, w, 0, 3);
}

// NekoBit's face in a pill as tall as the hints, at (x, y)
static int nbui_blitFace(SDL_Surface* dst, const char* face, int x, int y, SDL_Color badge) {
    GFX_Text text = GFX_renderText(font.large, face, COLOR_WHITE);
    int w = text.w + SCALE1(BUTTON_PADDING * 2);
    GFX_blitPillColor(dst, &(SDL_Rect){x, y, w, SCALE1(PILL_SIZE)}, badge);
    GFX_blitRenderedText(&text, NULL, dst, x + SCALE1(BUTTON_PADDING), y + (SCALE1(PILL_SIZE - 3) - text.h) / 2);
    return w;
}

// left_button NULL: NekoBit's face goes there (SELECT still closes, unsaid)
static void nbui_buttons(SDL_Surface* dst, int show_setting, char* left_button, const char* left_hint, const char* a_hint) {
    if (show_setting && !GetHDMI()) GFX_blitHardwareHints(dst, show_setting);
    else if (left_button) GFX_blitButtonGroup((char*[]){left_button, (char*)left_hint, NULL}, 0, dst, 0);
    else nbui_blitFace(dst, nbui_face(), SCALE1(PADDING), dst->h - SCALE1(PADDING + PILL_SIZE), NBUI_BADGE_UI);
    if (a_hint) GFX_blitButtonGroup((char*[]){"B", (char*)T(STR_BACK), "A", (char*)a_hint, NULL}, 1, dst, 1);
    else GFX_blitButtonGroup((char*[]){"B", (char*)T(STR_BACK), NULL}, 0, dst, 1);
}

static void nbui_drawDialog(SDL_Surface* dst, int show_setting) {
    static const int confirms[] = {STR_NB_CONFIRM_CANCEL, STR_NB_CONFIRM_RESET, STR_NB_CONFIRM_RESET2};
    int id = ui.screen == SCR_CONFIRM ? confirms[ui.confirm] : ui.message;
    SDL_Rect box = {SCALE1(PADDING * 4), SCALE1(PADDING * 2 + PILL_SIZE), dst->w - SCALE1(PADDING * 8), 0};
    box.h = dst->h - box.y * 2;
    GFX_blitRect(ASSET_DARK_GRAY_PILL, dst, &box);
    int y = box.y + SCALE1(12);
    y += nbui_text(dst, face_font, nbui_face(), COLOR_WHITE, box.x, y, box.w, 1, 1);
    SDL_Rect msg = {box.x, y, box.w, box.y + box.h - y - SCALE1(8)};
    GFX_blitMessage(font.large, (char*)T(id), dst, &msg);

    if (show_setting && !GetHDMI()) GFX_blitHardwareHints(dst, show_setting);
    if (ui.screen == SCR_CONFIRM)
        GFX_blitButtonGroup((char*[]){"B", (char*)T(STR_NB_NO), "A", (char*)T(STR_NB_YES), NULL}, 1, dst, 1);
    else
        GFX_blitButtonGroup((char*[]){"A", (char*)T(STR_NB_OK), NULL}, 0, dst, 1);
}

// Home: the list is as wide as its widest label, between 2/5 and 1/2 of the
// screen; the card gets the rest
static int nbui_homeSplit(SDL_Surface* dst) {
    int widest = 0;
    for (int i = 0; i < HOME_COUNT; i++) {
        char label[256];
        int tw;
        nbui_label(SCR_HOME, i, label, sizeof(label));
        TTF_SizeUTF8(font.large, label, &tw, NULL);
        widest = nbui_max(widest, tw);
    }
    int split = SCALE1(PADDING + BUTTON_PADDING * 2) + widest;
    return nbui_min(nbui_max(split, dst->w * 2 / 5), dst->w / 2);
}

void NBUI_draw(SDL_Surface* dst, int show_setting) {
    if (ui.screen == SCR_CONFIRM || ui.screen == SCR_MESSAGE) {
        nbui_drawDialog(dst, show_setting);
        return;
    }

    int split = ui.screen == SCR_HOME ? nbui_homeSplit(dst) : dst->w * 3 / 5;
    int list_w = split - SCALE1(PADDING);
    // between the battery pill and the button bar
    SDL_Rect card = {split + SCALE1(PADDING / 2), SCALE1(PADDING + PILL_SIZE + 4), 0, 0};
    card.w = dst->w - card.x - SCALE1(PADDING);
    card.h = ui.screen == SCR_HOME ? nbui_cardHome(dst, &card, 0) : dst->h - card.y * 2;

    int count = nbui_count(ui.screen);
    nbui_rows(dst, list_w);
    if (!count) {
        int id = ui.screen == SCR_HISTORY ? STR_NB_NO_HISTORY : STR_NB_NO_MISSIONS;
        nbui_text(dst, font.small, T(id), COLOR_LIGHT_TEXT, SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4), list_w - SCALE1(BUTTON_PADDING), 0, 4);
    }

    const NB_Save* s = NB_state();
    int sel = ui.sel[ui.screen];
    int has_card = 1;
    switch (ui.screen) {
    case SCR_HOME: has_card = 1; break;
    case SCR_MISSIONS: has_card = count > 0; break;
    case SCR_HISTORY: has_card = count > 0; break;
    }
    if (has_card) {
        GFX_blitRect(ASSET_DARK_GRAY_PILL, dst, &card);
        switch (ui.screen) {
        case SCR_HOME: nbui_cardHome(dst, &card, 1); break;
        case SCR_MISSIONS: nbui_cardMission(dst, &card, &s->missions[ui.slots[sel]], 1); break;
        case SCR_ACTIONS: nbui_cardMission(dst, &card, &s->missions[ui.act_slot], 1); break;
        case SCR_OFFERS: nbui_cardMission(dst, &card, &ui.offers[sel], 0); break;
        case SCR_HISTORY: nbui_cardHistory(dst, &card, nbui_history(sel)); break;
        }
    }

    if (ui.screen == SCR_HOME) { // what it says: under the list, the whole width, just above its face
        int line_h = TTF_FontLineSkip(font.tiny);
        int y = nbui_max(SCALE1(PADDING + HOME_COUNT * PILL_SIZE), card.y + card.h) + SCALE1(6);
        int bottom = dst->h - SCALE1(PADDING + PILL_SIZE + 2);
        int x = SCALE1(PADDING + BUTTON_PADDING);
        int w = dst->w - x * 2;
        int max_lines = (bottom - y) / line_h;
        char quote[300];
        snprintf(quote, sizeof(quote), T(STR_NB_SPEECH), ui.speech);
        // bottom-aligned: a single line sits where a longer text's last one would
        y += (max_lines - nbui_lines(font.tiny, quote, w, max_lines)) * line_h;
        nbui_text(dst, font.tiny, quote, COLOR_LIGHT_TEXT, x, y, w, 0, max_lines);
    }

    switch (ui.screen) {
    case SCR_HOME: nbui_buttons(dst, show_setting, NULL, NULL, T(STR_OPEN)); break;
    case SCR_MISSIONS: nbui_buttons(dst, show_setting, NULL, NULL, count ? T(STR_OPEN) : NULL); break;
    case SCR_ACTIONS: nbui_buttons(dst, show_setting, NULL, NULL, T(STR_NB_OK)); break;
    case SCR_OFFERS: nbui_buttons(dst, show_setting, "Y", T(STR_NB_SHUFFLE), T(STR_NB_ACCEPT)); break;
    case SCR_HISTORY: nbui_buttons(dst, show_setting, NULL, NULL, NULL); break;
    }
}

///////////////////////////////////////
// on the main menu

// Just the kaomoji in a button bar pill, as tall as "A OPEN": centred on the
// screen, or in the gap between the hints when a long translation is in the way
void NBUI_drawFace(SDL_Surface* dst, int left, int right) {
    if (!NB_isPresent() && anim.kind == ANIM_NONE) return; // hiding
    const char* face = anim.woken ? NB_sleepyFace() : NB_face(NB_state()->mood);
    GFX_Text text = GFX_renderText(font.large, face, COLOR_WHITE);
    int w = text.w + SCALE1(BUTTON_PADDING * 2);
    int x = (dst->w - w) / 2;
    if (x < left || x + w > right) {
        if (right - left >= w) x = left + (right - left - w) / 2;
        else if (!anim.summon) return; // no room at all: only a summon (SELECT) still shows it
    }
    nbui_blitFace(dst, face, x, dst->h - SCALE1(PADDING + PILL_SIZE) + nbui_offset(SDL_GetTicks()), NBUI_BADGE_MAIN);
}

// SELECT always brings it, whatever the dice said. Out already, it hops and
// the overlay opens. Hiding, it's woken up: it rises sleepy while SELECT is
// held and the overlay opens once it's all the way up; let go before that
// and it sinks back into hiding (pressed again, it rises from where it is).
void NBUI_summon(uint32_t now) {
    if (anim.summon) return;
    anim.arrival = 0;
    if (NB_isPresent()) {
        nbui_animate(ANIM_HOP, now, 1);
        return;
    }
    if (anim.kind != ANIM_RISE) anim.rise = 0;
    anim.woken = 1;
    nbui_animate(ANIM_RISE, now, 1);
    anim.last = now;
}

int NBUI_busy(void) {
    return anim.summon;
}

int NBUI_update(uint32_t now, int on_root) {
    int result = 0;
    if (banner.showing && anim.summon) banner.until = now + BANNER_MS; // on hold: NekoBit is coming up through it
    if (banner.showing && (int32_t)(now - banner.until) >= 0) {
        banner.showing = 0;
        result |= NBUI_REDRAW;
    }
    if (!banner.showing && NB_popEvent(&banner.ev)) {
        banner.showing = 1;
        banner.until = now + BANNER_MS;
        result |= NBUI_REDRAW;
    }

    if (anim.kind == ANIM_RISE) {
        if (anim.summon && !PAD_isPressed(BTN_SELECT)) anim.summon = 0; // let go too soon
        anim.rise += (float)(now - anim.last) / (anim.summon ? RISE_MS : -SINK_MS);
        anim.last = now;
        result |= NBUI_REDRAW;
        if (anim.rise >= 1) { // all the way up: now the overlay
            anim.rise = 1;
            anim.kind = ANIM_NONE;
            anim.summon = 0;
            result |= NBUI_SUMMONED;
        }
        else if (anim.rise <= 0) { // back in hiding, asleep again
            anim.rise = 0;
            anim.kind = ANIM_NONE;
            anim.woken = 0;
        }
        return result;
    }
    if (anim.kind != ANIM_NONE) {
        result |= NBUI_REDRAW; // a frame of movement
        if (now - anim.start >= anim.ms) {
            anim.kind = ANIM_NONE;
            if (anim.summon) result |= NBUI_SUMMONED;
            anim.summon = 0;
        }
        return result;
    }
    if (!on_root || ui.open || banner.showing) return result; // the news goes first

    if (anim.arrival) { // it just came out: peek in from the bottom edge
        anim.arrival = 0;
        nbui_animate(ANIM_PEEK, now, 0);
        return result | NBUI_REDRAW;
    }
    // now and then a happy NekoBit hops, which is all the idle CPU it costs
    int mood = NB_state()->mood;
    int lively = mood == NB_MOOD_HAPPY || mood == NB_MOOD_EXCITED || mood == NB_MOOD_SURPRISED;
    if (!anim.next_hop) anim.next_hop = now + 6000 + rand() % 9000;
    if (NB_isPresent() && lively && (int32_t)(now - anim.next_hop) >= 0) {
        nbui_animate(ANIM_HOP, now, 0);
        anim.next_hop = now + (mood == NB_MOOD_EXCITED ? 4000 : 7000) + rand() % 9000;
        result |= NBUI_REDRAW;
    }
    return result;
}

void NBUI_drawBanner(SDL_Surface* dst) {
    if (!banner.showing || anim.summon) return;
    NB_Event* ev = &banner.ev;
    char text[192];
    switch (ev->kind) {
    case NB_EV_MISSION: snprintf(text, sizeof(text), T(STR_NB_EV_MISSION), ev->a); break;
    case NB_EV_LEVEL: snprintf(text, sizeof(text), T(STR_NB_EV_LEVEL), ev->a); break;
    case NB_EV_ACH:
        if (ev->a == 1) snprintf(text, sizeof(text), T(STR_NB_EV_ACH_1), ev->b);
        else snprintf(text, sizeof(text), T(STR_NB_EV_ACH_N), ev->a, ev->b);
        break;
    case NB_EV_MILESTONE: snprintf(text, sizeof(text), T(STR_NB_EV_MILESTONE), ev->a, ev->text, ev->b); break;
    case NB_EV_MASTERY: snprintf(text, sizeof(text), T(STR_NB_EV_MASTERY), ev->text, ev->b); break;
    default: snprintf(text, sizeof(text), T(ev->a), ev->b); break;
    }
    char line[256], fit[256];
    snprintf(line, sizeof(line), "%s  %s", nbui_face(), text);
    // the whole button bar, both hint groups covered, always in the main
    // menu's dark grey (only the overlay's face pill is the lighter one)
    SDL_Rect pill = {SCALE1(PADDING), dst->h - SCALE1(PADDING + PILL_SIZE), dst->w - SCALE1(PADDING * 2), SCALE1(PILL_SIZE)};
    // the hints' font if it fits, the tiny one if not, and only then cut
    int room = pill.w - SCALE1(BUTTON_PADDING * 2);
    TTF_Font* f = font.small;
    int tw;
    TTF_SizeUTF8(f, line, &tw, NULL);
    if (tw > room) f = font.tiny;
    int w = nbui_fit(f, line, fit, sizeof(fit), room) + SCALE1(BUTTON_PADDING * 2);
    GFX_blitPillColor(dst, &pill, NBUI_BADGE_MAIN);
    GFX_Text t = GFX_renderText(f, fit, COLOR_WHITE);
    GFX_blitRenderedText(&t, NULL, dst, pill.x + (pill.w - nbui_min(w, pill.w)) / 2 + SCALE1(BUTTON_PADDING), pill.y + (pill.h - t.h) / 2);
}
