#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "i18n.h"
#include "collections.h"

#define CM_MAX 64 // lists shown, in name order: past that the rest don't
#define CM_ROWS (MAIN_ROW_COUNT - 1) // the title takes the first row
#define CM_TOP SCALE1(PADDING + PILL_SIZE + 4) // the first row's y: NekoBit's NBUI_TOP
#define CM_NAME_CHARS 32 // a new list's name, typed (utf-8: more bytes)

// CM_LIST: one game's lists (SELECT + Y). CM_MANAGER: all of them (the Quick
// Menu), CM_ITEMS the games in one, CM_DELETE asking before one goes.
// CM_SYSTEMS and CM_GAMES: adding games to the open list, a system at a time.
enum { CM_LIST, CM_KEYBOARD, CM_MANAGER, CM_ITEMS, CM_DELETE, CM_SYSTEMS, CM_GAMES };

// a row of the picker: a system folder, or a game as a list holds it
typedef struct {
    char* path; // systems: the folder, with SDCARD_PATH; games: without
    char* name;
} CM_Pick;
typedef struct {
    CM_Pick* rows;
    int count, size;
    int selected, start;
} CM_PickList;

typedef struct {
    char file[MAX_PATH];
    char name[MAX_PATH]; // getDisplayName's, as the root's Collections folder shows it
    int in; // the game is in it (CM_LIST)
    int games; // how many of its games are on the card (CM_MANAGER), as the folder shows them
} CM_List;

static struct {
    int open;
    int screen;
    int changed;
    int manager; // opened as the manager: no game
    int kids; // Kids Mode: lists can't be deleted
    char game[MAX_PATH]; // as a list holds it: no SDCARD_PATH
    char title[MAX_PATH];
    CM_List lists[CM_MAX];
    int count;
    int selected; // CM_LIST: count = New Collection; CM_MANAGER: 0 = New Collection, then the lists
    int start;
    struct {
        int list; // which one is open (CM_ITEMS) or being deleted (CM_DELETE)
        char** lines; // its games, as the file has them
        int count;
        int selected, start;
    } items;
    CM_PickList systems, games;
    struct {
        char text[CM_NAME_CHARS * 4 + 1];
        int page, row, col; // row 4 is the wide keys
        int auto_caps; // the capitals page was put up by kb_autoCaps, not chosen
    } kb;
} cm;

///////////////////////////////////////
// the lists

static int cm_contains(const char* file, const char* game) {
    FILE* f = fopen(file, "r");
    if (!f) return 0;
    char line[MAX_PATH];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        normalizeNewline(line);
        trimTrailingNewlines(line);
        found = exactMatch(line, (char*)game);
    }
    fclose(f);
    return found;
}

// a line at the end, after a newline if the last line hasn't one
static int cm_add(const char* file, const char* game) {
    FILE* f = fopen(file, "a+");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    if (ftell(f) > 0) {
        fseek(f, -1, SEEK_END);
        if (fgetc(f) != '\n') fputc('\n', f);
    }
    fprintf(f, "%s\n", game);
    return fclose(f) == 0;
}

// every line that is the game goes, the rest stay as they were
static int cm_remove(const char* file, const char* game) {
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", file); // not a .txt: never shown as a list
    FILE* in = fopen(file, "r");
    if (!in) return 0;
    FILE* out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return 0;
    }
    char line[MAX_PATH], bare[MAX_PATH];
    while (fgets(line, sizeof(line), in)) {
        strcpy(bare, line);
        normalizeNewline(bare);
        trimTrailingNewlines(bare);
        if (!exactMatch(bare, (char*)game)) fputs(line, out);
    }
    fclose(in);
    if (fclose(out) != 0 || rename(tmp, file) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
}

// a list's games that are on the card, in its order: what its folder shows
// (getCollection in minui.c). With lines NULL, just how many.
static int cm_readGames(const char* file, char*** lines) {
    FILE* f = fopen(file, "r");
    if (!f) return 0;
    int n = 0, size = 0;
    char line[MAX_PATH], sd_path[MAX_PATH + 16];
    while (fgets(line, sizeof(line), f)) {
        normalizeNewline(line);
        trimTrailingNewlines(line);
        if (!line[0]) continue;
        snprintf(sd_path, sizeof(sd_path), "%s%s", SDCARD_PATH, line);
        if (!exists(sd_path)) continue;
        if (lines) {
            if (n == size) {
                size = size ? size * 2 : 32;
                *lines = realloc(*lines, size * sizeof(char*));
            }
            (*lines)[n] = strdup(line);
        }
        n++;
    }
    fclose(f);
    return n;
}

static int cm_sortList(const void* a, const void* b) {
    return strcasecmp(((const CM_List*)a)->name, ((const CM_List*)b)->name); // as EntryArray_sort
}

static void cm_load(void) {
    cm.count = 0;
    DIR* dh = opendir(COLLECTIONS_PATH);
    if (dh) {
        struct dirent* dp;
        while (cm.count < CM_MAX && (dp = readdir(dh))) {
            if (dp->d_type == DT_DIR || hide(dp->d_name) || !suffixMatch(".txt", dp->d_name)) continue;
            CM_List* list = &cm.lists[cm.count++];
            snprintf(list->file, sizeof(list->file), "%s/%s", COLLECTIONS_PATH, dp->d_name);
            getDisplayName(list->file, list->name);
            if (cm.manager) list->games = cm_readGames(list->file, NULL);
            else list->in = cm_contains(list->file, cm.game);
        }
        closedir(dh);
    }
    qsort(cm.lists, cm.count, sizeof(CM_List), cm_sortList);
}

static void cm_select(int index) {
    cm.selected = index;
    if (cm.selected < cm.start) cm.start = cm.selected;
    else if (cm.selected >= cm.start + CM_ROWS) cm.start = cm.selected - CM_ROWS + 1;
}

// the name typed becomes a list with the game in it (the manager: an empty
// one); a list of that name already (whatever the case) just gets the game
static void cm_create(void) {
    char* name = cm.kb.text;
    while (*name == ' ' || *name == '.') name++; // a leading dot would hide it
    int n = strlen(name);
    while (n > 0 && name[n - 1] == ' ') name[--n] = '\0';
    if (!n) return; // nothing typed: OK waits for a name

    char file[MAX_PATH];
    snprintf(file, sizeof(file), "%s/%s.txt", COLLECTIONS_PATH, name);
    for (int i = 0; i < cm.count; i++)
        if (!strcasecmp(cm.lists[i].file, file)) strcpy(file, cm.lists[i].file);

    mkdir(COLLECTIONS_PATH, 0755); // the first list
    if (cm.manager) {
        FILE* f = fopen(file, "a");
        if (f) fclose(f);
    }
    else if (!cm_contains(file, cm.game)) cm_add(file, cm.game);
    cm.changed = 1;

    cm_load();
    for (int i = 0; i < cm.count; i++)
        if (!strcmp(cm.lists[i].file, file)) cm_select(cm.manager ? i + 1 : i);
    cm.screen = cm.manager ? CM_MANAGER : CM_LIST;
}

static void cm_freeItems(void) {
    for (int i = 0; i < cm.items.count; i++) free(cm.items.lines[i]);
    free(cm.items.lines);
    cm.items.lines = NULL;
    cm.items.count = 0;
}

static void cm_loadItems(void) {
    cm_freeItems();
    cm.items.count = cm_readGames(cm.lists[cm.items.list].file, &cm.items.lines);
    if (cm.items.selected >= cm.items.count) cm.items.selected = cm.items.count ? cm.items.count - 1 : 0;
    if (cm.items.start > cm.items.selected) cm.items.start = cm.items.selected;
    int last_start = cm.items.count > CM_ROWS ? cm.items.count - CM_ROWS : 0;
    if (cm.items.start > last_start) cm.items.start = last_start;
}

///////////////////////////////////////
// the picker: every system in ROMS_PATH, then its games with a box each

static void pl_free(CM_PickList* list) {
    for (int i = 0; i < list->count; i++) {
        free(list->rows[i].path);
        free(list->rows[i].name);
    }
    free(list->rows);
    memset(list, 0, sizeof(*list));
}

static void pl_add(CM_PickList* list, const char* path, const char* name) {
    if (list->count == list->size) {
        list->size = list->size ? list->size * 2 : 64;
        list->rows = realloc(list->rows, list->size * sizeof(CM_Pick));
    }
    list->rows[list->count].path = strdup(path);
    list->rows[list->count].name = strdup(name);
    list->count++;
}

static int pl_sortRow(const void* a, const void* b) {
    return strcasecmp(((const CM_Pick*)a)->name, ((const CM_Pick*)b)->name);
}

// the folders with something in them, by the names the root shows (two that
// show the same, "Arcade (FBNEO)" and "Arcade (MAME)", by their own names)
static void cm_loadSystems(void) {
    pl_free(&cm.systems);
    DIR* dh = opendir(ROMS_PATH);
    if (!dh) return;
    struct dirent* dp;
    while ((dp = readdir(dh))) {
        if (hide(dp->d_name) || dp->d_type != DT_DIR) continue;
        char path[MAX_PATH], name[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", ROMS_PATH, dp->d_name);
        DIR* inner = opendir(path);
        if (!inner) continue;
        struct dirent* ip;
        int has = 0;
        while (!has && (ip = readdir(inner))) has = !hide(ip->d_name);
        closedir(inner);
        if (!has) continue;
        getDisplayName(path, name);
        pl_add(&cm.systems, path, name);
    }
    closedir(dh);
    qsort(cm.systems.rows, cm.systems.count, sizeof(CM_Pick), pl_sortRow);
    for (int i = 0; i < cm.systems.count; i++) {
        int same = (i > 0 && !strcmp(cm.systems.rows[i - 1].name, cm.systems.rows[i].name)) ||
                   (i + 1 < cm.systems.count && !strcmp(cm.systems.rows[i + 1].name, cm.systems.rows[i].name));
        if (!same) continue;
        free(cm.systems.rows[i].name);
        cm.systems.rows[i].name = strdup(strrchr(cm.systems.rows[i].path, '/') + 1);
    }
    if (cm.systems.count > 1) qsort(cm.systems.rows, cm.systems.count, sizeof(CM_Pick), pl_sortRow);
}

// a system's games, as its folder lists them: its files, and a multi-disc
// game's folder by the .cue or .m3u in it (other folders are left out)
static void cm_loadGames(const char* dir) {
    pl_free(&cm.games);
    DIR* dh = opendir(dir);
    if (!dh) return;
    struct dirent* dp;
    while ((dp = readdir(dh))) {
        if (hide(dp->d_name)) continue;
        char path[MAX_PATH * 2], name[MAX_PATH];
        snprintf(path, sizeof(path), "%s/%s", dir, dp->d_name);
        if (dp->d_type == DT_DIR) {
            char disc[MAX_PATH * 3];
            snprintf(disc, sizeof(disc), "%s/%s.cue", path, dp->d_name);
            if (!exists(disc)) snprintf(disc, sizeof(disc), "%s/%s.m3u", path, dp->d_name);
            if (!exists(disc)) continue;
            getDisplayName(path, name);
            pl_add(&cm.games, disc + strlen(SDCARD_PATH), name);
            continue;
        }
        getDisplayName(path, name);
        pl_add(&cm.games, path + strlen(SDCARD_PATH), name);
    }
    closedir(dh);
    qsort(cm.games.rows, cm.games.count, sizeof(CM_Pick), pl_sortRow);
}

// is that game in the open list? (its lines are loaded: cm.items)
static int cm_inItems(const char* game) {
    for (int i = 0; i < cm.items.count; i++)
        if (!strcmp(cm.items.lines[i], game)) return 1;
    return 0;
}

///////////////////////////////////////

void CM_open(const char* path, const char* name) {
    cm_freeItems();
    pl_free(&cm.systems);
    pl_free(&cm.games);
    memset(&cm, 0, sizeof(cm));
    if (prefixMatch(SDCARD_PATH "/", (char*)path)) path += strlen(SDCARD_PATH); // as addRecent: paths platform agnostic
    snprintf(cm.game, sizeof(cm.game), "%s", path);
    snprintf(cm.title, sizeof(cm.title), "%s", name);
    cm_load();
    cm.open = 1;
    cm.screen = CM_LIST;
}

void CM_openManager(void) {
    cm_freeItems();
    pl_free(&cm.systems);
    pl_free(&cm.games);
    memset(&cm, 0, sizeof(cm));
    cm.manager = 1;
    cm.kids = exists(SIMPLE_MODE_PATH);
    cm_load();
    cm.open = 1;
    cm.screen = CM_MANAGER;
}

int CM_isOpen(void) {
    return cm.open;
}

int CM_changed(void) {
    int changed = cm.changed;
    cm.changed = 0;
    return changed;
}

///////////////////////////////////////
// the keyboard: the RetroAchievements tool's, three pages of 4 x 10 keys
// and a row of four. No / \ : * ? " < > |, so a list's name can go through
// any computer; no '.', which getDisplayName would take for an extension.

#define KB_ROWS 4
#define KB_COLS 10
#define KB_PAGES 3
#define KB_WIDE 4
static const char* kb_keys[KB_PAGES][KB_ROWS] = {
    {"1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm&_-"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM&_-"},
    {"1234567890", "áéíóúüñç¡¿", "!#$%&'()+,", "-;=@[]_{}~"},
};
static const char* kb_next[KB_PAGES] = {"ABC", "#+=", "abc"}; // the page X goes to
enum { KB_LOWER, KB_UPPER, KB_SYMBOLS };

// the key in a row: its col-th utf-8 character
static const char* kb_key(int page, int row, int col, char* out) {
    const char* s = kb_keys[page][row];
    while (*s && col--) {
        s++;
        while ((*s & 0xC0) == 0x80) s++;
    }
    int n = 1;
    while ((s[n] & 0xC0) == 0x80) n++;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static int kb_chars(const char* s) {
    int n = 0;
    for (; *s; s++) n += (*s & 0xC0) != 0x80;
    return n;
}

static void kb_autoCaps(void);

static void kb_type(const char* key) {
    if (kb_chars(cm.kb.text) >= CM_NAME_CHARS) return;
    if (strlen(cm.kb.text) + strlen(key) >= sizeof(cm.kb.text)) return;
    strcat(cm.kb.text, key);
    kb_autoCaps();
}

static void kb_backspace(void) {
    int n = strlen(cm.kb.text);
    if (!n) return;
    do n--; while (n > 0 && (cm.kb.text[n] & 0xC0) == 0x80);
    cm.kb.text[n] = '\0';
    kb_autoCaps();
}

// Capitals by themselves at the start of each word, as a phone does: the
// first letter and the one after a space come up on the capitals page, which
// goes back to small letters once one is typed. Capitals chosen by hand (L1)
// stay until changed; the symbols page is never switched away from.
static void kb_autoCaps(void) {
    int n = strlen(cm.kb.text);
    int word_start = n == 0 || cm.kb.text[n - 1] == ' ';
    if (word_start && cm.kb.page == KB_LOWER) {
        cm.kb.page = KB_UPPER;
        cm.kb.auto_caps = 1;
    }
    else if (!word_start && cm.kb.auto_caps && cm.kb.page == KB_UPPER) {
        cm.kb.page = KB_LOWER;
        cm.kb.auto_caps = 0;
    }
}

// a page picked by hand: L1 between small letters and capitals, X (or the
// first wide key) round all three
static void kb_setPage(int page) {
    cm.kb.page = page;
    cm.kb.auto_caps = 0;
}

static void kb_open(void) {
    cm.kb.text[0] = '\0';
    cm.kb.page = KB_LOWER;
    cm.kb.auto_caps = 0;
    kb_autoCaps(); // the first letter: a capital
    cm.kb.row = 1; // on the q
    cm.kb.col = 0;
    cm.screen = CM_KEYBOARD;
}

static int kb_input(void) {
    int* row = &cm.kb.row;
    int* col = &cm.kb.col;
    if (PAD_justPressed(BTN_START)) cm_create();
    else if (PAD_justPressed(BTN_B) && !cm.kb.text[0]) cm.screen = cm.manager ? CM_MANAGER : CM_LIST; // nothing typed: B is back
    else if (PAD_justRepeated(BTN_B) && cm.kb.text[0]) kb_backspace();
    else if (PAD_justPressed(BTN_X)) kb_setPage((cm.kb.page + 1) % KB_PAGES);
    else if (PAD_justPressed(BTN_L1)) kb_setPage(cm.kb.page == KB_LOWER ? KB_UPPER : KB_LOWER);
    else if (PAD_justRepeated(BTN_UP)) {
        if (*row == KB_ROWS) {
            *row = KB_ROWS - 1;
            *col = *col * KB_COLS / KB_WIDE + 1;
            if (*col > KB_COLS - 1) *col = KB_COLS - 1;
        }
        else if (*row == 0) {
            *row = KB_ROWS;
            *col = *col * KB_WIDE / KB_COLS;
        }
        else *row -= 1;
    }
    else if (PAD_justRepeated(BTN_DOWN)) {
        if (*row == KB_ROWS - 1) {
            *row = KB_ROWS;
            *col = *col * KB_WIDE / KB_COLS;
        }
        else if (*row == KB_ROWS) {
            *row = 0;
            *col = *col * KB_COLS / KB_WIDE + 1;
            if (*col > KB_COLS - 1) *col = KB_COLS - 1;
        }
        else *row += 1;
    }
    else if (PAD_justRepeated(BTN_LEFT) || PAD_justRepeated(BTN_RIGHT)) {
        int n = *row == KB_ROWS ? KB_WIDE : KB_COLS;
        *col = (*col + (PAD_justRepeated(BTN_LEFT) ? n - 1 : 1)) % n;
    }
    else if (PAD_justRepeated(BTN_A)) {
        char key[8];
        if (*row < KB_ROWS) kb_type(kb_key(cm.kb.page, *row, *col, key));
        else if (!PAD_justPressed(BTN_A)) return 0; // the wide keys don't repeat
        else if (*col == 0) kb_setPage((cm.kb.page + 1) % KB_PAGES);
        else if (*col == 1) kb_type(" ");
        else if (*col == 2) kb_backspace();
        else cm_create();
    }
    else return 0;
    return 1;
}

///////////////////////////////////////
// input

static int cm_rows(void) {
    return cm.count + 1; // the lists, then New Collection
}

// as the main menu: up and down a row (a held button stops at the ends, a
// press wraps round), left and right a page
static int cm_nav(int* selected, int* start, int rows) {
    if (rows <= 0) return 0;
    int to = *selected;
    if (PAD_justRepeated(BTN_UP)) {
        if (*selected > 0) to = *selected - 1;
        else if (PAD_justPressed(BTN_UP)) to = rows - 1;
        else return 0;
    }
    else if (PAD_justRepeated(BTN_DOWN)) {
        if (*selected < rows - 1) to = *selected + 1;
        else if (PAD_justPressed(BTN_DOWN)) to = 0;
        else return 0;
    }
    else if (PAD_justRepeated(BTN_LEFT)) to = *selected - CM_ROWS < 0 ? 0 : *selected - CM_ROWS;
    else if (PAD_justRepeated(BTN_RIGHT)) to = *selected + CM_ROWS > rows - 1 ? rows - 1 : *selected + CM_ROWS;
    else return 0;
    *selected = to;
    if (*selected < *start) *start = *selected;
    else if (*selected >= *start + CM_ROWS) *start = *selected - CM_ROWS + 1;
    return 1;
}

static int cm_managerInput(void) {
    if (cm_nav(&cm.selected, &cm.start, cm.count + 1)) return 1;
    int list = cm.selected - 1; // row 0 is New Collection
    if (PAD_justPressed(BTN_A)) {
        if (list < 0) kb_open();
        else {
            cm.items.list = list;
            cm.items.selected = cm.items.start = 0;
            cm_loadItems();
            cm.screen = CM_ITEMS;
        }
    }
    else if (PAD_justPressed(BTN_X) && list >= 0 && !cm.kids) {
        cm.items.list = list;
        cm.screen = CM_DELETE;
    }
    else if (PAD_justPressed(BTN_B)) {
        cm_freeItems();
        pl_free(&cm.systems);
        pl_free(&cm.games);
        cm.open = 0;
    }
    else return 0;
    return 1;
}

// a list's games: X takes the one selected out of it
static int cm_itemsInput(void) {
    if (cm_nav(&cm.items.selected, &cm.items.start, cm.items.count)) return 1;
    if (PAD_justPressed(BTN_X) && cm.items.count) {
        if (cm_remove(cm.lists[cm.items.list].file, cm.items.lines[cm.items.selected])) {
            cm.changed = 1;
            cm_loadItems();
        }
    }
    else if (PAD_justPressed(BTN_A)) { // more games: the systems, then a system's
        if (!cm.systems.count) cm_loadSystems();
        cm.screen = CM_SYSTEMS;
    }
    else if (PAD_justPressed(BTN_B)) {
        cm.lists[cm.items.list].games = cm.items.count;
        cm_freeItems();
        pl_free(&cm.systems);
        cm.screen = CM_MANAGER;
    }
    else return 0;
    return 1;
}

static int cm_systemsInput(void) {
    CM_PickList* l = &cm.systems;
    if (cm_nav(&l->selected, &l->start, l->count)) return 1;
    if (PAD_justPressed(BTN_A) && l->count) {
        cm_loadGames(l->rows[l->selected].path);
        cm.screen = CM_GAMES;
    }
    else if (PAD_justPressed(BTN_B)) cm.screen = CM_ITEMS;
    else return 0;
    return 1;
}

// A ticks a game into the list or out of it
static int cm_gamesInput(void) {
    CM_PickList* l = &cm.games;
    if (cm_nav(&l->selected, &l->start, l->count)) return 1;
    if (PAD_justPressed(BTN_A) && l->count) {
        const char* file = cm.lists[cm.items.list].file;
        const char* game = l->rows[l->selected].path;
        if (cm_inItems(game) ? cm_remove(file, game) : cm_add(file, game)) {
            cm.changed = 1;
            cm_loadItems();
        }
    }
    else if (PAD_justPressed(BTN_B)) {
        pl_free(&cm.games);
        cm.screen = CM_SYSTEMS;
    }
    else return 0;
    return 1;
}

// the list goes, its games stay where they are
static int cm_deleteInput(void) {
    if (PAD_justPressed(BTN_A)) {
        if (unlink(cm.lists[cm.items.list].file) == 0) {
            cm.changed = 1;
            cm_load();
            if (cm.selected > cm.count) cm.selected = cm.count;
            if (cm.start > cm.selected) cm.start = cm.selected;
        }
        cm.screen = CM_MANAGER;
    }
    else if (PAD_justPressed(BTN_B)) cm.screen = CM_MANAGER;
    else return 0;
    return 1;
}

int CM_input(void) {
    if (!cm.open) return 0;
    switch (cm.screen) {
    case CM_KEYBOARD: return kb_input();
    case CM_MANAGER: return cm_managerInput();
    case CM_ITEMS: return cm_itemsInput();
    case CM_DELETE: return cm_deleteInput();
    case CM_SYSTEMS: return cm_systemsInput();
    case CM_GAMES: return cm_gamesInput();
    }

    if (cm_nav(&cm.selected, &cm.start, cm_rows())) return 1;
    if (PAD_justPressed(BTN_A)) {
        if (cm.selected == cm.count) kb_open();
        else {
            CM_List* list = &cm.lists[cm.selected];
            if (list->in ? cm_remove(list->file, cm.game) : cm_add(list->file, cm.game)) {
                list->in = !list->in;
                cm.changed = 1;
            }
        }
    }
    else if (PAD_justPressed(BTN_B)) cm.open = 0;
    else return 0;
    return 1;
}

///////////////////////////////////////
// drawing

// the pills of the main menu with a box before the text: what's in it
// starts after CM_ICON_W
#define CM_ICON_W SCALE1(PILL_SIZE - BUTTON_PADDING)
#define CM_TEXT_X (SCALE1(PADDING + BUTTON_PADDING / 2) + CM_ICON_W)

static void cm_drawRow(SDL_Surface* dst, int y, int max_w, int pill, int icon, const char* icon_text,
                       SDL_Color icon_color, const char* label, SDL_Color color) {
    char fit[MAX_PATH];
    int pad = SCALE1(BUTTON_PADDING / 2 + BUTTON_PADDING) + CM_ICON_W;
    int w = GFX_truncateText(font.large, label, fit, max_w, pad);
    if (pill >= 0) GFX_blitPill(pill, dst, &(SDL_Rect){SCALE1(PADDING), y, w, SCALE1(PILL_SIZE)});
    SDL_Rect box = {SCALE1(PADDING + BUTTON_PADDING / 2), y, CM_ICON_W, SCALE1(PILL_SIZE)};
    if (icon_text) {
        GFX_Text text = GFX_renderText(font.large, icon_text, icon_color);
        GFX_blitRenderedText(&text, NULL, dst, box.x + (box.w - text.w) / 2, y + SCALE1(4));
    }
    else if (icon >= 0) GFX_blitAssetScaled(icon, dst, &box, 1, icon_color);
    GFX_Text text = GFX_renderText(font.large, fit, color);
    GFX_blitRenderedText(&text, NULL, dst, CM_TEXT_X, y + SCALE1(4));
}

// where the screen is, as NekoBit's and the Quick Menu's titles: the dark
// pill, its text darker. icon: the star inside Collections (-1: none)
static void cm_drawTitle(SDL_Surface* dst, int hardware_w, const char* label, int icon) {
    int max_w = dst->w - hardware_w - SCALE1(PADDING * 2 + BUTTON_MARGIN);
    if (icon >= 0) {
        cm_drawRow(dst, SCALE1(PADDING), max_w, ASSET_DARK_GRAY_PILL, icon, NULL, COLOR_DARK_TEXT, label, COLOR_DARK_TEXT);
        return;
    }
    char fit[MAX_PATH];
    int w = GFX_truncateText(font.large, label, fit, max_w, SCALE1(BUTTON_PADDING * 2));
    GFX_blitPill(ASSET_DARK_GRAY_PILL, dst, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), w, SCALE1(PILL_SIZE)});
    GFX_Text text = GFX_renderText(font.large, fit, COLOR_DARK_TEXT);
    GFX_blitRenderedText(&text, NULL, dst, SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4));
}

static void cm_hints(SDL_Surface* dst, int show_setting, char** left, char** right) {
    if (show_setting && !GetHDMI()) GFX_blitHardwareHints(dst, show_setting);
    else if (left) GFX_blitButtonGroup(left, 0, dst, 0);
    GFX_blitButtonGroup(right, right[2] ? 1 : 0, dst, 1);
}

static void cm_drawList(SDL_Surface* dst, int show_setting, int hardware_w) {
    cm_drawTitle(dst, hardware_w, cm.title, -1);

    int max_w = dst->w - SCALE1(PADDING * 2);
    for (int j = 0, i = cm.start; j < CM_ROWS && i < cm_rows(); j++, i++) {
        int y = CM_TOP + SCALE1(j * PILL_SIZE);
        int on = i == cm.selected;
        SDL_Color color = on ? COLOR_BLACK : COLOR_WHITE;
        if (i == cm.count) {
            cm_drawRow(dst, y, max_w, on ? ASSET_WHITE_PILL : -1, 0, "+", color, T(STR_CM_NEW), color);
            continue;
        }
        char* name = cm.lists[i].name;
        trimSortingMeta(&name);
        // the box ticked: the game is in this one
        cm_drawRow(dst, y, max_w, on ? ASSET_WHITE_PILL : -1, cm.lists[i].in ? ASSET_CHECKED : ASSET_UNCHECKED, NULL, color, name, color);
    }

    const char* action = cm.selected == cm.count ? T(STR_CM_CREATE)
                       : cm.lists[cm.selected].in ? T(STR_CM_REMOVE) : T(STR_CM_ADD);
    cm_hints(dst, show_setting, NULL, (char*[]){"B", (char*)T(STR_BACK), "A", (char*)action, NULL});
}

// minarch's option pills: a dark one for a key, the white one under the cursor
#define KB_GAP 2
#define KB_KEY_H SCALE1(BUTTON_SIZE)
static void kb_drawKey(SDL_Surface* dst, int x, int y, int w, const char* label, int on) {
    GFX_blitPill(on ? ASSET_BUTTON : ASSET_OPTION, dst, &(SDL_Rect){x, y, w, KB_KEY_H});
    GFX_Text text = GFX_renderText(font.small, label, on ? COLOR_BLACK : COLOR_WHITE);
    GFX_blitRenderedText(&text, NULL, dst, x + (w - text.w) / 2, y + (KB_KEY_H - text.h) / 2);
}

static void cm_drawKeyboard(SDL_Surface* dst, int show_setting, int hardware_w) {
    cm_drawTitle(dst, hardware_w, T(STR_CM_NEW), -1);

    // the name so far, on a dark pill across the screen; a long one shows its end
    int field_y = SCALE1(PADDING + PILL_SIZE + BUTTON_MARGIN);
    int field_w = dst->w - SCALE1(PADDING * 2);
    GFX_blitPill(ASSET_DARK_GRAY_PILL, dst, &(SDL_Rect){SCALE1(PADDING), field_y, field_w, SCALE1(PILL_SIZE)});
    char shown[sizeof(cm.kb.text) + 2];
    snprintf(shown, sizeof(shown), "%s_", cm.kb.text);
    GFX_Text text = GFX_renderText(font.large, shown, COLOR_WHITE);
    int room = field_w - SCALE1(BUTTON_PADDING * 2);
    SDL_Rect clip = {text.w > room ? text.w - room : 0, 0, text.w > room ? room : text.w, text.h};
    GFX_blitRenderedText(&text, &clip, dst, SCALE1(PADDING + BUTTON_PADDING), field_y + SCALE1(4));

    // the keys, between the field and the hints
    int grid_w = dst->w - SCALE1(PADDING * 2);
    int key_w = (grid_w - SCALE1(KB_GAP) * (KB_COLS - 1)) / KB_COLS;
    grid_w = key_w * KB_COLS + SCALE1(KB_GAP) * (KB_COLS - 1);
    int x0 = (dst->w - grid_w) / 2;
    int pitch = KB_KEY_H + SCALE1(KB_GAP);
    int hints_y = dst->h - SCALE1(PADDING + PILL_SIZE);
    int top = field_y + SCALE1(PILL_SIZE);
    int y0 = top + (hints_y - top - (pitch * (KB_ROWS + 1) - SCALE1(KB_GAP))) / 2;
    char key[8];
    for (int r = 0; r < KB_ROWS; r++)
        for (int c = 0; c < KB_COLS; c++)
            kb_drawKey(dst, x0 + c * (key_w + SCALE1(KB_GAP)), y0 + r * pitch, key_w,
                       kb_key(cm.kb.page, r, c, key), cm.kb.row == r && cm.kb.col == c);
    const char* wide[KB_WIDE] = {kb_next[cm.kb.page], T(STR_KB_SPACE), T(STR_KB_DEL), T(STR_KB_OK)};
    int wide_w = (grid_w - SCALE1(KB_GAP) * (KB_WIDE - 1)) / KB_WIDE;
    for (int c = 0; c < KB_WIDE; c++)
        kb_drawKey(dst, x0 + c * (wide_w + SCALE1(KB_GAP)), y0 + KB_ROWS * pitch, wide_w, wide[c],
                   cm.kb.row == KB_ROWS && cm.kb.col == c);

    cm_hints(dst, show_setting,
             (char*[]){"L1", (char*)(cm.kb.page == KB_UPPER ? "abc" : "ABC"), "START", (char*)T(STR_KB_DONE), NULL}, // X, all the pages: on its key
             (char*[]){"B", (char*)(cm.kb.text[0] ? T(STR_DELETE) : T(STR_BACK)), NULL, NULL});
}

// a row as the main menu draws it: the white pill under the selected one
static void cm_drawPlain(SDL_Surface* dst, int y, int max_w, int on, const char* label) {
    char fit[MAX_PATH];
    int w = GFX_truncateText(font.large, label, fit, max_w, SCALE1(BUTTON_PADDING * 2));
    if (on) GFX_blitPill(ASSET_WHITE_PILL, dst, &(SDL_Rect){SCALE1(PADDING), y, w, SCALE1(PILL_SIZE)});
    GFX_Text text = GFX_renderText(font.large, fit, on ? COLOR_BLACK : COLOR_WHITE);
    GFX_blitRenderedText(&text, NULL, dst, SCALE1(PADDING + BUTTON_PADDING), y + SCALE1(4));
}

// every list, how many games each has; New Collection first
static void cm_drawManager(SDL_Surface* dst, int show_setting, int hardware_w) {
    cm_drawTitle(dst, hardware_w, T(STR_QM_COLLECTIONS), ASSET_STAR); // the Quick Menu's page title

    for (int j = 0, i = cm.start; j < CM_ROWS && i <= cm.count; j++, i++) {
        int y = CM_TOP + SCALE1(j * PILL_SIZE);
        int on = i == cm.selected;
        if (i == 0) {
            char label[128];
            snprintf(label, sizeof(label), "+ %s", T(STR_CM_NEW));
            cm_drawPlain(dst, y, dst->w - SCALE1(PADDING * 2), on, label);
            continue;
        }
        CM_List* list = &cm.lists[i - 1];
        char count[32];
        if (list->games == 1) snprintf(count, sizeof(count), "%s", T(STR_CM_ONE_GAME));
        else snprintf(count, sizeof(count), T(STR_CM_GAMES), list->games);
        GFX_Text text = GFX_renderText(font.small, count, COLOR_LIGHT_TEXT);
        int count_x = dst->w - SCALE1(PADDING + BUTTON_PADDING) - text.w;
        GFX_blitRenderedText(&text, NULL, dst, count_x, y + (SCALE1(PILL_SIZE) - text.h) / 2);
        char* name = list->name;
        trimSortingMeta(&name);
        cm_drawPlain(dst, y, count_x - SCALE1(PADDING + BUTTON_PADDING), on, name);
    }

    if (cm.selected == 0)
        cm_hints(dst, show_setting, NULL, (char*[]){"B", (char*)T(STR_BACK), "A", (char*)T(STR_CM_CREATE), NULL});
    else
        cm_hints(dst, show_setting, cm.kids ? NULL : (char*[]){"X", (char*)T(STR_DELETE), NULL},
                 (char*[]){"B", (char*)T(STR_BACK), "A", (char*)T(STR_OPEN), NULL});
}

// the games in one list, as its folder orders them
static void cm_drawItems(SDL_Surface* dst, int show_setting, int hardware_w) {
    char* title = cm.lists[cm.items.list].name;
    trimSortingMeta(&title);
    cm_drawTitle(dst, hardware_w, title, ASSET_STAR); // still inside Collections

    if (!cm.items.count) {
        GFX_blitMessage(font.large, (char*)T(STR_CM_EMPTY), dst, &(SDL_Rect){0, 0, dst->w, dst->h});
        cm_hints(dst, show_setting, NULL, (char*[]){"B", (char*)T(STR_BACK), "A", (char*)T(STR_CM_ADD), NULL});
        return;
    }
    for (int j = 0, i = cm.items.start; j < CM_ROWS && i < cm.items.count; j++, i++) {
        char name[MAX_PATH];
        getDisplayName(cm.items.lines[i], name);
        char* shown = name;
        trimSortingMeta(&shown);
        cm_drawPlain(dst, CM_TOP + SCALE1(j * PILL_SIZE), dst->w - SCALE1(PADDING * 2), i == cm.items.selected, shown);
    }
    cm_hints(dst, show_setting, (char*[]){"X", (char*)T(STR_CM_REMOVE), NULL},
             (char*[]){"B", (char*)T(STR_BACK), "A", (char*)T(STR_CM_ADD), NULL});
}

// the picker: systems, then a system's games with their boxes; the list
// being filled in the title
static void cm_drawPicker(SDL_Surface* dst, int show_setting, int hardware_w) {
    char* title = cm.lists[cm.items.list].name;
    trimSortingMeta(&title);
    cm_drawTitle(dst, hardware_w, title, ASSET_STAR);

    int games = cm.screen == CM_GAMES;
    CM_PickList* l = games ? &cm.games : &cm.systems;
    if (!l->count) {
        GFX_blitMessage(font.large, (char*)T(STR_EMPTY_FOLDER), dst, &(SDL_Rect){0, 0, dst->w, dst->h});
        cm_hints(dst, show_setting, NULL, (char*[]){"B", (char*)T(STR_BACK), NULL, NULL});
        return;
    }
    int max_w = dst->w - SCALE1(PADDING * 2);
    for (int j = 0, i = l->start; j < CM_ROWS && i < l->count; j++, i++) {
        int y = CM_TOP + SCALE1(j * PILL_SIZE);
        int on = i == l->selected;
        char* name = l->rows[i].name;
        trimSortingMeta(&name);
        if (!games) {
            cm_drawPlain(dst, y, max_w, on, name);
            continue;
        }
        SDL_Color color = on ? COLOR_BLACK : COLOR_WHITE;
        int in = cm_inItems(l->rows[i].path);
        cm_drawRow(dst, y, max_w, on ? ASSET_WHITE_PILL : -1, in ? ASSET_CHECKED : ASSET_UNCHECKED, NULL, color, name, color);
    }
    const char* action = !games ? T(STR_OPEN) : cm_inItems(l->rows[l->selected].path) ? T(STR_CM_REMOVE) : T(STR_CM_ADD);
    cm_hints(dst, show_setting, NULL, (char*[]){"B", (char*)T(STR_BACK), "A", (char*)action, NULL});
}

// as the Quick Menu's dialogs: the question on its own, the answers under it
static void cm_drawDelete(SDL_Surface* dst, int show_setting) {
    char* name = cm.lists[cm.items.list].name;
    trimSortingMeta(&name);
    char text[MAX_PATH];
    snprintf(text, sizeof(text), T(STR_CM_DELETE_LIST), name);
    GFX_blitMessage(font.medium, text, dst, &(SDL_Rect){0, SCALE1(PADDING), dst->w, dst->h - SCALE1(PILL_SIZE + PADDING)});
    cm_hints(dst, show_setting, NULL, (char*[]){"B", (char*)T(STR_MA_CANCEL), "A", (char*)T(STR_DELETE), NULL});
}

void CM_draw(SDL_Surface* dst, int show_setting, int hardware_w) {
    switch (cm.screen) {
    case CM_KEYBOARD: cm_drawKeyboard(dst, show_setting, hardware_w); break;
    case CM_MANAGER: cm_drawManager(dst, show_setting, hardware_w); break;
    case CM_ITEMS: cm_drawItems(dst, show_setting, hardware_w); break;
    case CM_DELETE: cm_drawDelete(dst, show_setting); break;
    case CM_SYSTEMS:
    case CM_GAMES: cm_drawPicker(dst, show_setting, hardware_w); break;
    default: cm_drawList(dst, show_setting, hardware_w); break;
    }
}
