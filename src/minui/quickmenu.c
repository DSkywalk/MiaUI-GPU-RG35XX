#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <msettings.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "i18n.h"
#include "nekobit.h"
#include "quickmenu.h"

// What the old ADB tool (Tools/System/ADB.pak) ran. There is no way back:
// stopping or switching the USB gadget once it's up hangs the console.
#define QM_ADB_START "/etc/init.d/D50adbd start"
#define QM_NET_START "sleep 2; /usr/sbin/usbeth.sh"
#define QM_ADB_LOG "/tmp/adb_cmd_log.txt"
#define QM_USB_IF "rndis0"
#define QM_24HOUR_PATH USERDATA_PATH "/show_24hour" // shared with the Clock tool

#define QM_ICON_SCALE 2 // the atlas' 10px icons, doubled in the round buttons
#define QM_ICON_GAP 14

enum { ITEM_SETTINGS, ITEM_ACHIEVEMENTS, ITEM_NETWORK, ITEM_CLOCK, ITEM_SDTOOL, ITEM_ABOUT, ITEM_COUNT };
static const struct {
    int icon;
    int label;
    int kids; // still there in Kids Mode
} items[ITEM_COUNT] = {
    [ITEM_SETTINGS]     = {ASSET_GEAR,    STR_QM_SETTINGS,     1},
    [ITEM_ACHIEVEMENTS] = {ASSET_TROPHY,  STR_QM_ACHIEVEMENTS, 1},
    [ITEM_NETWORK]      = {ASSET_WIFI_SMALL, STR_QM_NETWORK,   0},
    [ITEM_CLOCK]        = {ASSET_CLOCK,   STR_QM_CLOCK,        1},
    [ITEM_SDTOOL]       = {ASSET_SDCARD,  STR_QM_SDTOOL,       0},
    [ITEM_ABOUT]        = {ASSET_GAMEPAD, STR_QM_ABOUT,        1},
};

enum { SCR_ROW, SCR_SETTINGS, SCR_ACHIEVEMENTS, SCR_NETWORK, SCR_CLOCK, SCR_ABOUT, SCR_CONFIRM, SCR_MESSAGE, SCR_COUNT };

// the rows of the option pages
enum { SET_LANGUAGE, SET_AUTO_SLEEP, SET_SLEEP_OFF, SET_KIDS, SET_RESET_NB, SET_COUNT };
enum { RA_ENABLED, RA_SOUND, RA_NOTIFY, RA_COUNT };
enum { NET_ADB, NET_ADB_NET, NET_COUNT };

enum { CONFIRM_KIDS, CONFIRM_NB, CONFIRM_NB2, CONFIRM_ADB, CONFIRM_ADB_NET };

static const int auto_sleep_minutes[] = {0, 1, 2, 5, 10}; // GetAutoSleep()
#define AUTO_SLEEP_COUNT 5
#define SLEEP_OFF_COUNT 3 // 5, 10, 15 minutes: GetSleepPowerOff()
static const int notify_labels[] = {STR_QM_NOTIFY_ALL, STR_QM_NOTIFY_UNLOCKS, STR_QM_NOTIFY_GENERAL, STR_QM_OFF};
#define NOTIFY_COUNT 4

enum { CURSOR_YEAR, CURSOR_MONTH, CURSOR_DAY, CURSOR_HOUR, CURSOR_MINUTE, CURSOR_SECOND, CURSOR_AMPM };

static struct {
    int open;
    int screen;
    int back; // where a dialog returns to
    int item; // the icon picked, index into shown[]
    int shown[ITEM_COUNT];
    int shown_count;
    int sel[SCR_COUNT];
    int confirm;
    int message; // string id
    int kids;
    int settings_dirty; // msettings.bin to write when the menu closes
    int language_dirty; // language.conf, likewise
    int show_24hour;
    int minute; // last one drawn
    char ip[INET_ADDRSTRLEN];
    uint32_t ip_at;
    pid_t adb_pid; // switching USB on, in the background
    int adb_target;
    struct { int year, month, day, hour, minute, second, cursor; } clock;
} qm;

static int qm_min(int a, int b) { return a < b ? a : b; }
static int qm_max(int a, int b) { return a > b ? a : b; }

///////////////////////////////////////
// USB

static int qm_adbdRunning(void) {
    DIR* proc = opendir("/proc");
    if (!proc) return 0;
    int found = 0;
    struct dirent* e;
    while (!found && (e = readdir(proc))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64], comm[32] = {0};
        snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) found = !strcmp(comm, "adbd\n");
        fclose(f);
    }
    closedir(proc);
    return found;
}

static int qm_usbUp(void) {
    return exists("/sys/class/net/" QM_USB_IF);
}

static void qm_readIP(void) {
    qm.ip[0] = '\0';
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, QM_USB_IF, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0)
        inet_ntop(AF_INET, &((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr, qm.ip, sizeof(qm.ip));
    close(fd);
}

static void qm_startUSB(int mode) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "(%s%s%s) > " QM_ADB_LOG " 2>&1",
             QM_ADB_START, mode == USB_MODE_NET ? "; " : "", mode == USB_MODE_NET ? QM_NET_START : "");
    LOG_info("Quick Menu: %s\n", cmd);
    pid_t pid = fork();
    if (pid == 0) {
        // adbd lives on after us: none of minui's files (the GPU's) go with it
        for (int fd = 3; fd < 1024; fd++) close(fd);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    if (pid < 0) {
        qm.message = STR_QM_ADB_FAILED;
        qm.back = qm.screen;
        qm.screen = SCR_MESSAGE;
        return;
    }
    qm.adb_pid = pid;
    qm.adb_target = mode;
}

///////////////////////////////////////

static void qm_refreshItems(void) {
    int picked = qm.shown_count ? qm.shown[qm.item] : ITEM_SETTINGS;
    qm.shown_count = 0;
    qm.item = 0;
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (qm.kids && !items[i].kids) continue;
        if (i == picked) qm.item = qm.shown_count;
        qm.shown[qm.shown_count++] = i;
    }
}

void QM_init(void) {
    qm.kids = exists(SIMPLE_MODE_PATH);
    qm.minute = -1;
    qm_refreshItems();
    // once a boot: after that the file says (it's in tmpfs)
    if (!exists(USB_MODE_PATH)) {
        int mode = qm_usbUp() ? USB_MODE_NET : qm_adbdRunning() ? USB_MODE_ADB : USB_MODE_NONE;
        if (mode != USB_MODE_NONE) USB_setMode(mode);
    }
}

int QM_isOpen(void) {
    return qm.open;
}

int QM_kidsMode(void) {
    return qm.kids;
}

void QM_open(void) {
    qm.open = 1;
    qm.screen = SCR_ROW;
    qm.show_24hour = exists(QM_24HOUR_PATH);
    qm.minute = -1;
    qm.ip_at = 0;
    if (USB_getMode() == USB_MODE_NET) qm_readIP();
}

static void qm_close(void) {
    qm.open = 0;
    if (qm.settings_dirty) SaveSettings();
    if (qm.language_dirty) I18N_save(I18N_code(I18N_index()));
    qm.settings_dirty = qm.language_dirty = 0;
}

static void qm_dialog(int screen, int what) {
    qm.back = qm.screen;
    qm.screen = screen;
    if (screen == SCR_CONFIRM) qm.confirm = what;
    else qm.message = what;
}

///////////////////////////////////////
// option pages

// the rows a page shows, in order; returns how many
static int qm_rows(int screen, int* rows) {
    int n = 0;
    switch (screen) {
    case SCR_SETTINGS:
        for (int i = 0; i < SET_COUNT; i++)
            if (!(qm.kids && i == SET_RESET_NB)) rows[n++] = i;
        break;
    case SCR_ACHIEVEMENTS:
        for (int i = 0; i < RA_COUNT; i++) rows[n++] = i;
        break;
    case SCR_NETWORK:
        for (int i = 0; i < NET_COUNT; i++) rows[n++] = i;
        break;
    }
    return n;
}

static int qm_rowLabel(int screen, int row) {
    switch (screen) {
    case SCR_SETTINGS: {
        static const int ids[SET_COUNT] = {STR_QM_LANGUAGE, STR_QM_AUTO_SLEEP, STR_QM_SLEEP_OFF, STR_QM_KIDS_MODE, STR_NB_MENU_RESET};
        return ids[row];
    }
    case SCR_ACHIEVEMENTS: {
        static const int ids[RA_COUNT] = {STR_QM_RA_ENABLED, STR_QM_RA_SOUND, STR_QM_RA_NOTIFY};
        return ids[row];
    }
    }
    return row == NET_ADB ? STR_QM_ADB : STR_QM_ADB_NET;
}

static int qm_netMode(int row) {
    return row == NET_ADB ? USB_MODE_ADB : USB_MODE_NET;
}

static int qm_rowDesc(int screen, int row) {
    switch (screen) {
    case SCR_SETTINGS: {
        static const int ids[SET_COUNT] = {STR_QM_DESC_LANGUAGE, STR_QM_DESC_AUTO_SLEEP, STR_QM_DESC_SLEEP_OFF, STR_QM_DESC_KIDS_MODE, STR_QM_DESC_RESET_NB};
        return ids[row];
    }
    case SCR_ACHIEVEMENTS: {
        static const int ids[RA_COUNT] = {STR_QM_DESC_RA_ENABLED, STR_QM_DESC_RA_SOUND, STR_QM_DESC_RA_NOTIFY};
        return ids[row];
    }
    }
    int mode = USB_getMode();
    if (mode != USB_MODE_NONE && mode != qm_netMode(row)) return STR_QM_DESC_LOCKED;
    return row == NET_ADB ? STR_QM_DESC_ADB : STR_QM_DESC_ADB_NET;
}

// A row's values, for the list to be as wide as the widest of them (as
// minarch's are): how many, which one is set, and each one's text.
static int qm_valueCount(int screen, int row) {
    switch (screen) {
    case SCR_SETTINGS:
        switch (row) {
        case SET_LANGUAGE: return I18N_count();
        case SET_AUTO_SLEEP: return AUTO_SLEEP_COUNT;
        case SET_SLEEP_OFF: return SLEEP_OFF_COUNT;
        case SET_KIDS: return 2;
        }
        return 0; // Reset NekoBit only does something
    case SCR_ACHIEVEMENTS: return row == RA_NOTIFY ? NOTIFY_COUNT : 2;
    case SCR_NETWORK: return 4; // see qm_valueText
    }
    return 0;
}

static int qm_valueIndex(int screen, int row) {
    switch (screen) {
    case SCR_SETTINGS:
        switch (row) {
        case SET_LANGUAGE: return I18N_index();
        case SET_AUTO_SLEEP: return qm_min(qm_max(GetAutoSleep(), 0), AUTO_SLEEP_COUNT - 1);
        case SET_SLEEP_OFF: return qm_min(qm_max(GetSleepPowerOff(), 0), SLEEP_OFF_COUNT - 1);
        case SET_KIDS: return qm.kids;
        }
        return 0;
    case SCR_ACHIEVEMENTS:
        switch (row) {
        case RA_ENABLED: return GetRAEnabled() ? 1 : 0;
        case RA_SOUND: return GetRASound() ? 1 : 0;
        }
        return qm_min(qm_max(GetRANotify(), 0), NOTIFY_COUNT - 1);
    case SCR_NETWORK: { // 0 enable, 1 active, 2 locked, 3 being enabled
        int mode = USB_getMode();
        int want = qm_netMode(row);
        if (qm.adb_pid) return qm.adb_target == want ? 3 : 2;
        if (mode == want) return 1;
        return mode != USB_MODE_NONE ? 2 : 0;
    }
    }
    return 0;
}

static void qm_valueText(int screen, int row, int i, char* out, size_t size) {
    out[0] = '\0';
    switch (screen) {
    case SCR_SETTINGS:
        switch (row) {
        case SET_LANGUAGE: snprintf(out, size, "%s", I18N_name(i)); break;
        case SET_AUTO_SLEEP:
            if (i == 0) snprintf(out, size, "%s", T(STR_QM_OFF));
            else snprintf(out, size, T(STR_QM_MINUTES), auto_sleep_minutes[i]);
            break;
        case SET_SLEEP_OFF: snprintf(out, size, T(STR_QM_MINUTES), (i + 1) * 5); break;
        case SET_KIDS: snprintf(out, size, "%s", T(i ? STR_QM_ON : STR_QM_OFF)); break;
        }
        break;
    case SCR_ACHIEVEMENTS:
        if (row == RA_NOTIFY) snprintf(out, size, "%s", T(notify_labels[i]));
        else snprintf(out, size, "%s", T(i ? STR_QM_ON : STR_QM_OFF));
        break;
    case SCR_NETWORK: {
        static const int ids[4] = {STR_QM_ENABLE, STR_QM_ACTIVE, STR_QM_LOCKED, STR_QM_ACTIVATING};
        snprintf(out, size, "%s", T(ids[i]));
        break;
    }
    }
}

static int qm_wrap(int value, int count) {
    return (value % count + count) % count;
}

// LEFT/RIGHT (and A on a row with values): dir -1 or 1
static void qm_change(int screen, int row, int dir) {
    switch (screen) {
    case SCR_SETTINGS:
        switch (row) {
        case SET_LANGUAGE:
            I18N_setLanguage(I18N_code(qm_wrap(I18N_index() + dir, I18N_count())));
            qm.language_dirty = 1;
            break;
        case SET_AUTO_SLEEP:
            SetAutoSleep(qm_wrap(GetAutoSleep() + dir, AUTO_SLEEP_COUNT));
            qm.settings_dirty = 1;
            break;
        case SET_SLEEP_OFF:
            SetSleepPowerOff(qm_wrap(GetSleepPowerOff() + dir, SLEEP_OFF_COUNT));
            qm.settings_dirty = 1;
            break;
        case SET_KIDS:
            // on, with a word on how to get out; off only with the combo (A)
            if (!qm.kids) qm_dialog(SCR_CONFIRM, CONFIRM_KIDS);
            break;
        }
        break;
    case SCR_ACHIEVEMENTS:
        switch (row) {
        case RA_ENABLED: SetRAEnabled(!GetRAEnabled()); break;
        case RA_SOUND: SetRASound(!GetRASound()); break;
        case RA_NOTIFY: SetRANotify(qm_wrap(GetRANotify() + dir, NOTIFY_COUNT)); break;
        }
        qm.settings_dirty = 1;
        break;
    }
}

static void qm_setKids(int on) {
    qm.kids = on;
    if (on) putFile(SIMPLE_MODE_PATH, "");
    else unlink(SIMPLE_MODE_PATH);
    qm_refreshItems();
    // Reset NekoBit comes and goes above it: stay on the Kids Mode row
    int rows[SET_COUNT];
    int count = qm_rows(SCR_SETTINGS, rows);
    for (int i = 0; i < count; i++)
        if (rows[i] == SET_KIDS) qm.sel[SCR_SETTINGS] = i;
}

static void qm_activate(int screen, int row) {
    switch (screen) {
    case SCR_SETTINGS:
        if (row == SET_RESET_NB) qm_dialog(SCR_CONFIRM, CONFIRM_NB);
        else if (row == SET_KIDS) {
            if (!qm.kids) qm_dialog(SCR_CONFIRM, CONFIRM_KIDS);
            else if (PAD_isPressed(BTN_L1) && PAD_isPressed(BTN_R1)) qm_setKids(0);
        }
        else qm_change(screen, row, 1);
        break;
    case SCR_ACHIEVEMENTS:
        qm_change(screen, row, 1);
        break;
    case SCR_NETWORK:
        if (USB_getMode() == USB_MODE_NONE && !qm.adb_pid)
            qm_dialog(SCR_CONFIRM, row == NET_ADB ? CONFIRM_ADB : CONFIRM_ADB_NET);
        break;
    }
}

static void qm_confirmed(void) {
    qm.screen = qm.back;
    switch (qm.confirm) {
    case CONFIRM_KIDS:
        qm_setKids(1);
        break;
    case CONFIRM_NB:
        qm_dialog(SCR_CONFIRM, CONFIRM_NB2);
        qm.back = SCR_SETTINGS;
        break;
    case CONFIRM_NB2:
        NB_reset(); // its banner says so
        break;
    case CONFIRM_ADB:
        qm_startUSB(USB_MODE_ADB);
        break;
    case CONFIRM_ADB_NET:
        qm_startUSB(USB_MODE_NET);
        break;
    }
}

///////////////////////////////////////
// clock

static void qm_clockLoad(void) {
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    qm.clock.year = tm.tm_year + 1900;
    qm.clock.month = tm.tm_mon + 1;
    qm.clock.day = tm.tm_mday;
    qm.clock.hour = tm.tm_hour;
    qm.clock.minute = tm.tm_min;
    qm.clock.second = tm.tm_sec;
    qm.clock.cursor = 0;
}

static void qm_clockValidate(void) {
    int y = qm.clock.year;
    int february = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0) ? 29 : 28;
    if (qm.clock.month > 12) qm.clock.month -= 12;
    else if (qm.clock.month < 1) qm.clock.month += 12;
    if (qm.clock.year > 2100) qm.clock.year = 2100;
    else if (qm.clock.year < 1970) qm.clock.year = 1970;
    int days = 31;
    switch (qm.clock.month) {
    case 2: days = february; break;
    case 4: case 6: case 9: case 11: days = 30; break;
    }
    if (qm.clock.day > days) qm.clock.day -= days;
    else if (qm.clock.day < 1) qm.clock.day += days;
    if (qm.clock.hour > 23) qm.clock.hour -= 24;
    else if (qm.clock.hour < 0) qm.clock.hour += 24;
    if (qm.clock.minute > 59) qm.clock.minute -= 60;
    else if (qm.clock.minute < 0) qm.clock.minute += 60;
    if (qm.clock.second > 59) qm.clock.second -= 60;
    else if (qm.clock.second < 0) qm.clock.second += 60;
}

static int qm_clockFields(void) {
    return (qm.show_24hour ? CURSOR_SECOND : CURSOR_AMPM) + 1;
}

static void qm_clockStep(int dir) {
    switch (qm.clock.cursor) {
    case CURSOR_YEAR: qm.clock.year += dir; break;
    case CURSOR_MONTH: qm.clock.month += dir; break;
    case CURSOR_DAY: qm.clock.day += dir; break;
    case CURSOR_HOUR: qm.clock.hour += dir; break;
    case CURSOR_MINUTE: qm.clock.minute += dir; break;
    case CURSOR_SECOND: qm.clock.second += dir; break;
    case CURSOR_AMPM: qm.clock.hour += dir * 12; break;
    }
    qm_clockValidate();
}

static void qm_clockSave(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "date -u -s '%d-%d-%d %d:%d:%d';hwclock --utc -w",
             qm.clock.year, qm.clock.month, qm.clock.day, qm.clock.hour, qm.clock.minute, qm.clock.second);
    system(cmd);
    qm.minute = -1; // the time on top changed with it
}

static int qm_clockInput(void) {
    if (PAD_justRepeated(BTN_UP)) qm_clockStep(1);
    else if (PAD_justRepeated(BTN_DOWN)) qm_clockStep(-1);
    else if (PAD_justRepeated(BTN_LEFT)) qm.clock.cursor = qm_wrap(qm.clock.cursor - 1, qm_clockFields());
    else if (PAD_justRepeated(BTN_RIGHT)) qm.clock.cursor = qm_wrap(qm.clock.cursor + 1, qm_clockFields());
    else if (PAD_justPressed(BTN_A)) {
        qm_clockSave();
        qm.screen = SCR_ROW;
    }
    else if (PAD_justPressed(BTN_B)) qm.screen = SCR_ROW;
    else if (PAD_justPressed(BTN_SELECT)) {
        qm.show_24hour = !qm.show_24hour;
        if (qm.show_24hour) putFile(QM_24HOUR_PATH, "");
        else unlink(QM_24HOUR_PATH);
        if (qm.clock.cursor >= qm_clockFields()) qm.clock.cursor = 0;
        qm.minute = -1;
    }
    else return 0;
    return 1;
}

///////////////////////////////////////
// input

int QM_input(int* action) {
    *action = QM_NONE;
    if (!qm.open) return 0;

    if (qm.screen == SCR_CONFIRM || qm.screen == SCR_MESSAGE) {
        if (PAD_justPressed(BTN_A)) {
            if (qm.screen == SCR_CONFIRM) qm_confirmed();
            else qm.screen = qm.back;
            return 1;
        }
        if (PAD_justPressed(BTN_B)) {
            qm.screen = qm.back;
            return 1;
        }
        return 0;
    }

    if (PAD_tappedMenu(SDL_GetTicks())) { // MENU: all the way out (an unset clock stays unset)
        qm_close();
        *action = QM_CLOSED;
        return 1;
    }

    if (qm.screen == SCR_CLOCK) return qm_clockInput();

    if (qm.screen == SCR_ROW) {
        if (PAD_justRepeated(BTN_LEFT)) qm.item = qm_wrap(qm.item - 1, qm.shown_count);
        else if (PAD_justRepeated(BTN_RIGHT)) qm.item = qm_wrap(qm.item + 1, qm.shown_count);
        else if (PAD_justPressed(BTN_B)) {
            qm_close();
            *action = QM_CLOSED;
        }
        else if (PAD_justPressed(BTN_A)) {
            switch (qm.shown[qm.item]) {
            case ITEM_SETTINGS: qm.screen = SCR_SETTINGS; break;
            case ITEM_ACHIEVEMENTS: qm.screen = SCR_ACHIEVEMENTS; break;
            case ITEM_NETWORK: qm.screen = SCR_NETWORK; break;
            case ITEM_CLOCK: qm_clockLoad(); qm.screen = SCR_CLOCK; break;
            case ITEM_ABOUT: qm.screen = SCR_ABOUT; break;
            case ITEM_SDTOOL:
                qm_close();
                *action = QM_SDTOOL;
                break;
            }
        }
        else return 0;
        return 1;
    }

    if (qm.screen == SCR_ABOUT) {
        if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_A)) {
            qm.screen = SCR_ROW;
            return 1;
        }
        return 0;
    }

    int rows[SET_COUNT];
    int count = qm_rows(qm.screen, rows);
    int* sel = &qm.sel[qm.screen];
    if (*sel >= count) *sel = count - 1;
    if (PAD_justRepeated(BTN_UP)) *sel = qm_wrap(*sel - 1, count);
    else if (PAD_justRepeated(BTN_DOWN)) *sel = qm_wrap(*sel + 1, count);
    else if (PAD_justRepeated(BTN_LEFT)) qm_change(qm.screen, rows[*sel], -1);
    else if (PAD_justRepeated(BTN_RIGHT)) qm_change(qm.screen, rows[*sel], 1);
    else if (PAD_justPressed(BTN_A)) qm_activate(qm.screen, rows[*sel]);
    else if (PAD_justPressed(BTN_B)) qm.screen = SCR_ROW;
    else return 0;
    return 1;
}

int QM_update(void) {
    int dirty = 0;
    if (qm.adb_pid) {
        int status;
        if (waitpid(qm.adb_pid, &status, WNOHANG) == qm.adb_pid) {
            qm.adb_pid = 0;
            if (qm_adbdRunning()) {
                USB_setMode(qm.adb_target);
                qm.ip_at = 0;
            }
            else if (qm.open) qm_dialog(SCR_MESSAGE, STR_QM_ADB_FAILED);
            dirty = 1;
        }
    }
    if (!qm.open) return dirty;

    uint32_t now = SDL_GetTicks();
    if (USB_getMode() == USB_MODE_NET && (!qm.ip_at || now - qm.ip_at >= 2000)) {
        char was[sizeof(qm.ip)];
        strcpy(was, qm.ip);
        qm_readIP();
        qm.ip_at = now;
        if (strcmp(was, qm.ip)) dirty = 1;
    }

    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    if (tm.tm_min != qm.minute) {
        qm.minute = tm.tm_min;
        dirty = 1;
    }
    return dirty;
}

///////////////////////////////////////
// drawing

static void qm_timeText(char* out, size_t size) {
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    if (qm.show_24hour) snprintf(out, size, "%02i:%02i", tm.tm_hour, tm.tm_min);
    else {
        int hour = tm.tm_hour % 12;
        snprintf(out, size, "%i:%02i %s", hour ? hour : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    }
}

// the time, centred on the battery pill's row
static void qm_drawTime(SDL_Surface* dst) {
    char now[16];
    qm_timeText(now, sizeof(now));
    GFX_Text text = GFX_renderText(font.large, now, COLOR_WHITE);
    int w = text.w + SCALE1(BUTTON_PADDING * 2);
    int x = (dst->w - w) / 2;
    GFX_blitPill(ASSET_DARK_GRAY_PILL, dst, &(SDL_Rect){x, SCALE1(PADDING), w, SCALE1(PILL_SIZE)});
    GFX_blitRenderedText(&text, NULL, dst, x + SCALE1(BUTTON_PADDING), SCALE1(PADDING) + (SCALE1(PILL_SIZE) - text.h) / 2);
}

// a page's name, top left: its icon and label in a pill
static void qm_drawTitle(SDL_Surface* dst, int item) {
    GFX_Text text = GFX_renderText(font.large, T(items[item].label), COLOR_WHITE);
    int icon_w = SCALE1(PILL_SIZE - BUTTON_PADDING);
    int w = SCALE1(BUTTON_PADDING / 2) + icon_w + text.w + SCALE1(BUTTON_PADDING);
    GFX_blitPill(ASSET_DARK_GRAY_PILL, dst, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), w, SCALE1(PILL_SIZE)});
    GFX_blitAssetScaled(items[item].icon, dst, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING / 2), SCALE1(PADDING), icon_w, SCALE1(PILL_SIZE)}, 1, COLOR_WHITE);
    GFX_blitRenderedText(&text, NULL, dst, SCALE1(PADDING + BUTTON_PADDING / 2) + icon_w, SCALE1(PADDING) + (SCALE1(PILL_SIZE) - text.h) / 2);
}

// what USB is doing, a line of small text centred at y
static void qm_drawUSBInfo(SDL_Surface* dst, int y) {
    int mode = USB_getMode();
    if (mode == USB_MODE_NONE) return;
    char line[128];
    if (mode == USB_MODE_ADB) snprintf(line, sizeof(line), "%s", T(STR_QM_INFO_ADB));
    else if (qm.ip[0]) snprintf(line, sizeof(line), T(STR_QM_INFO_NET), qm.ip);
    else snprintf(line, sizeof(line), "%s", T(STR_QM_INFO_NO_IP));
    GFX_Text text = GFX_renderText(font.small, line, COLOR_LIGHT_TEXT);
    GFX_blitRenderedText(&text, NULL, dst, (dst->w - text.w) / 2, y);
}

static void qm_hints(SDL_Surface* dst, int show_setting, char** right) {
    if (show_setting && !GetHDMI()) GFX_blitHardwareHints(dst, show_setting);
    GFX_blitButtonGroup(right, right[2] ? 1 : 0, dst, 1);
}

static void qm_drawRow(SDL_Surface* dst, int show_setting) {
    int n = qm.shown_count;
    int d = SCALE1(PILL_SIZE); // a pill's two caps together are a circle: the button
    int gap = SCALE1(QM_ICON_GAP);
    int x = (dst->w - (n * d + (n - 1) * gap)) / 2;
    int y = (dst->h - d) / 2 - SCALE1(PADDING);
    for (int i = 0; i < n; i++) {
        int selected = i == qm.item;
        SDL_Rect box = {x + i * (d + gap), y, d, d};
        GFX_blitAsset(selected ? ASSET_WHITE_PILL : ASSET_DARK_GRAY_PILL, NULL, dst, &box);
        GFX_blitAssetScaled(items[qm.shown[i]].icon, dst, &box, QM_ICON_SCALE, selected ? COLOR_BLACK : COLOR_WHITE);
    }
    GFX_Text label = GFX_renderText(font.large, T(items[qm.shown[qm.item]].label), COLOR_WHITE);
    int label_y = y + d + SCALE1(PADDING);
    GFX_blitRenderedText(&label, NULL, dst, (dst->w - label.w) / 2, label_y);
    qm_drawUSBInfo(dst, label_y + label.h + SCALE1(PADDING));

    qm_hints(dst, show_setting, (char*[]){"B", (char*)T(STR_BACK), "A", (char*)T(STR_OPEN), NULL});
}

// minarch's option lists (Menu_options, MENU_VAR), to the pixel: centred rows
// as wide as the widest name and value, the selected one on the dark option
// pill with its name on a white one, and the help text at the foot
#define QM_OPTION_PADDING 8 // minarch's OPTION_PADDING
static void qm_drawPage(SDL_Surface* dst, int show_setting) {
    static const int page_items[SCR_COUNT] = {
        [SCR_SETTINGS] = ITEM_SETTINGS, [SCR_ACHIEVEMENTS] = ITEM_ACHIEVEMENTS, [SCR_NETWORK] = ITEM_NETWORK,
    };
    qm_drawTitle(dst, page_items[qm.screen]);

    int rows[SET_COUNT];
    int count = qm_rows(qm.screen, rows);
    int sel = qm_min(qm.sel[qm.screen], count - 1);
    char value[64];

    int mw = 0;
    for (int i = 0; i < count; i++) {
        int lw = 0, rw = 0, w;
        TTF_SizeUTF8(font.small, T(qm_rowLabel(qm.screen, rows[i])), &lw, NULL);
        w = lw;
        for (int v = 0; v < qm_valueCount(qm.screen, rows[i]); v++) {
            qm_valueText(qm.screen, rows[i], v, value, sizeof(value));
            TTF_SizeUTF8(font.tiny, value, &rw, NULL);
            w = qm_max(w, lw + rw);
        }
        mw = qm_max(mw, w + SCALE1(QM_OPTION_PADDING * 4));
    }
    mw = qm_min(mw, dst->w - SCALE1(PADDING * 2));

    int ox = (dst->w - mw) / 2;
    int oy = SCALE1(PADDING + PILL_SIZE + 10);
    for (int j = 0; j < count; j++) {
        int y = oy + SCALE1(j * BUTTON_SIZE);
        const char* label = T(qm_rowLabel(qm.screen, rows[j]));
        SDL_Color color = COLOR_WHITE;
        if (j == sel) {
            GFX_blitPill(ASSET_OPTION, dst, &(SDL_Rect){ox, y, mw, SCALE1(BUTTON_SIZE)});
            int w = 0;
            TTF_SizeUTF8(font.small, label, &w, NULL);
            GFX_blitPill(ASSET_BUTTON, dst, &(SDL_Rect){ox, y, w + SCALE1(QM_OPTION_PADDING * 2), SCALE1(BUTTON_SIZE)});
            color = COLOR_BLACK;
        }
        GFX_Text text = GFX_renderText(font.small, label, color);
        GFX_blitRenderedText(&text, NULL, dst, ox + SCALE1(QM_OPTION_PADDING), y + SCALE1(1));
        if (qm_valueCount(qm.screen, rows[j])) {
            qm_valueText(qm.screen, rows[j], qm_valueIndex(qm.screen, rows[j]), value, sizeof(value));
            text = GFX_renderText(font.tiny, value, COLOR_WHITE);
            GFX_blitRenderedText(&text, NULL, dst, ox + mw - text.w - SCALE1(QM_OPTION_PADDING), y + SCALE1(3));
        }
    }

    // the help text, at the foot as in minarch; USB's state just above it
    const char* desc = T(qm_rowDesc(qm.screen, rows[sel]));
    int w, h;
    GFX_sizeText(font.tiny, (char*)desc, SCALE1(12), &w, &h);
    int desc_y = dst->h - SCALE1(PADDING) - h;
    GFX_blitText(font.tiny, (char*)desc, SCALE1(12), COLOR_LIGHT_TEXT, dst, &(SDL_Rect){(dst->w - w) / 2, desc_y, w, h});
    if (qm.screen == SCR_NETWORK) qm_drawUSBInfo(dst, desc_y - TTF_FontLineSkip(font.small) - SCALE1(4));
}

// as minarch's Menu_prompt: the message on its own, the answers under it
static void qm_drawDialog(SDL_Surface* dst, int show_setting) {
    static const int confirms[] = {
        [CONFIRM_KIDS] = STR_QM_KIDS_ON, [CONFIRM_NB] = STR_NB_CONFIRM_RESET, [CONFIRM_NB2] = STR_NB_CONFIRM_RESET2,
        [CONFIRM_ADB] = STR_QM_CONFIRM_ADB, [CONFIRM_ADB_NET] = STR_QM_CONFIRM_ADB_NET,
    };
    int id = qm.screen == SCR_CONFIRM ? confirms[qm.confirm] : qm.message;
    GFX_blitMessage(font.medium, (char*)T(id), dst, &(SDL_Rect){0, SCALE1(PADDING), dst->w, dst->h - SCALE1(PILL_SIZE + PADDING)});

    if (qm.screen == SCR_CONFIRM)
        qm_hints(dst, show_setting, (char*[]){"B", (char*)T(STR_MA_CANCEL), "A", (char*)T(STR_MA_CONFIRM), NULL});
    else
        qm_hints(dst, show_setting, (char*[]){"A", (char*)T(STR_MA_OKAY), NULL, NULL});
}

static void qm_drawAbout(SDL_Surface* dst, int show_setting) {
    qm_drawTitle(dst, ITEM_ABOUT);

    static char release[256];
    static char* commit = NULL;
    if (!commit) {
        getFile(ROOT_SYSTEM_PATH "/version.txt", release, sizeof(release));
        // release on the first line, the commit on the last
        char* nl = strrchr(release, '\n');
        if (nl && nl[1] == '\0') *nl = '\0';
        nl = strrchr(release, '\n');
        commit = nl ? nl + 1 : release + strlen(release);
        char* first = strchr(release, '\n');
        if (first) *first = '\0';
    }
    const char* keys[3] = {T(STR_QM_RELEASE), T(STR_QM_VERSION), T(STR_QM_MODEL)};
    const char* vals[3] = {release, commit, PLAT_getModel()};
    GFX_Text k[3], v[3];
    int kw = 0, vw = 0;
    for (int i = 0; i < 3; i++) {
        k[i] = GFX_renderText(font.large, keys[i], COLOR_DARK_TEXT);
        v[i] = GFX_renderText(font.large, vals[i], COLOR_WHITE);
        kw = qm_max(kw, k[i].w);
        vw = qm_max(vw, v[i].w);
    }
    int line = SCALE1(24);
    int x = (dst->w - (kw + SCALE1(8) + vw)) / 2;
    int y = (dst->h - line * 3) / 2;
    for (int i = 0; i < 3; i++, y += line) {
        GFX_blitRenderedText(&k[i], NULL, dst, x, y);
        GFX_blitRenderedText(&v[i], NULL, dst, x + kw + SCALE1(8), y);
    }
    qm_hints(dst, show_setting, (char*[]){"B", (char*)T(STR_BACK), NULL, NULL});
}

// The Clock tool's screen, as it was: digits in cells of their own so the
// numbers don't jitter as they change
#define DIGIT_WIDTH 10
#define DIGIT_HEIGHT 16
static int qm_clockGlyph(SDL_Surface* dst, const char* c, int x, int y) {
    GFX_Text digit = GFX_renderText(font.large, c, COLOR_WHITE);
    int cell_h = SCALE1(DIGIT_HEIGHT);
    int oy = (c[0] == ':' ? SCALE1(-1.5) : 0) + (cell_h - digit.h) / 2; // : sits too low naturally
    int top = oy < 0 ? -oy : 0;
    int bottom = qm_min(digit.h, cell_h - oy);
    if (bottom > top)
        GFX_blitRenderedText(&digit, &(SDL_Rect){0, top, digit.w, bottom - top}, dst,
                             x + (SCALE1(DIGIT_WIDTH) - digit.w) / 2, y + oy + top);
    return x + SCALE1(DIGIT_WIDTH);
}
static int qm_clockNumber(SDL_Surface* dst, int num, int digits, int x, int y) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%0*i", digits, num);
    for (int i = 0; buf[i]; i++) x = qm_clockGlyph(dst, (char[]){buf[i], 0}, x, y);
    return x;
}

static void qm_drawClock(SDL_Surface* dst, int show_setting) {
    qm_drawTitle(dst, ITEM_CLOCK);

    int ox = (dst->w - (qm.show_24hour ? SCALE1(188) : SCALE1(223))) / 2;
    int x = ox;
    int y = SCALE1(((FIXED_HEIGHT / FIXED_SCALE) - PILL_SIZE - DIGIT_HEIGHT) / 2);
    x = qm_clockNumber(dst, qm.clock.year, 4, x, y);
    x = qm_clockGlyph(dst, "/", x, y);
    x = qm_clockNumber(dst, qm.clock.month, 2, x, y);
    x = qm_clockGlyph(dst, "/", x, y);
    x = qm_clockNumber(dst, qm.clock.day, 2, x, y);
    x += SCALE1(10);
    int hour = qm.clock.hour;
    if (!qm.show_24hour) {
        hour %= 12;
        if (!hour) hour = 12;
    }
    x = qm_clockNumber(dst, hour, 2, x, y);
    x = qm_clockGlyph(dst, ":", x, y);
    x = qm_clockNumber(dst, qm.clock.minute, 2, x, y);
    x = qm_clockGlyph(dst, ":", x, y);
    x = qm_clockNumber(dst, qm.clock.second, 2, x, y);
    int ampm_w = 0;
    if (!qm.show_24hour) {
        x += SCALE1(10);
        GFX_Text text = GFX_renderText(font.large, qm.clock.hour < 12 ? "AM" : "PM", COLOR_WHITE);
        ampm_w = text.w + SCALE1(2);
        GFX_blitRenderedText(&text, NULL, dst, x, y - SCALE1(3));
    }

    // the field being changed
    x = ox;
    y += SCALE1(19);
    if (qm.clock.cursor != CURSOR_YEAR) x += SCALE1(50) + (qm.clock.cursor - 1) * SCALE1(30);
    int w = qm.clock.cursor == CURSOR_YEAR ? SCALE1(40) : qm.clock.cursor == CURSOR_AMPM ? ampm_w : SCALE1(20);
    GFX_blitPill(ASSET_UNDERLINE, dst, &(SDL_Rect){x, y, w});

    if (show_setting && !GetHDMI()) GFX_blitHardwareHints(dst, show_setting);
    else GFX_blitButtonGroup((char*[]){"SELECT", (char*)T(qm.show_24hour ? STR_QM_12_HOUR : STR_QM_24_HOUR), NULL}, 0, dst, 0);
    GFX_blitButtonGroup((char*[]){"B", (char*)T(STR_BACK), "A", (char*)T(STR_QM_SET), NULL}, 1, dst, 1);
}

void QM_draw(SDL_Surface* dst, int show_setting) {
    if (qm.screen != SCR_CLOCK) qm_drawTime(dst);
    switch (qm.screen) {
    case SCR_ROW: qm_drawRow(dst, show_setting); break;
    case SCR_CLOCK: qm_drawClock(dst, show_setting); break;
    case SCR_ABOUT: qm_drawAbout(dst, show_setting); break;
    case SCR_CONFIRM:
    case SCR_MESSAGE: qm_drawDialog(dst, show_setting); break;
    default: qm_drawPage(dst, show_setting); break;
    }
}
