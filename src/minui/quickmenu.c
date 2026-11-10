#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
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

// The one way the USB port is set up: usbnet.sh <adb|adbnet|net|off>. It
// tears the old mode down safely, so modes switch live, takes a few seconds,
// logs to /tmp/usbnet.log and writes USB_MODE_PATH. Its exit code says how
// it went.
#define QM_USBNET "/usr/sbin/usbnet.sh"
#define QM_USB_TIMEOUT 60 // s: a switch takes under 15; past this the port isn't answering
#define QM_USB_IF "rndis0"
#define QM_USB_LEASES "/tmp/udhcpd.leases" // usbnet.sh's DHCP server: the PC's address
#define QM_24HOUR_PATH USERDATA_PATH "/show_24hour" // shared with the Clock tool
#define QM_DATE_PATH USERDATA_PATH "/clock_date" // "1" day and month, "2" full date; none: time only

#define QM_ICON_SCALE 2 // the atlas' 10px icons, doubled in the round buttons
#define QM_ICON_GAP 10

enum { ITEM_SETTINGS, ITEM_ACHIEVEMENTS, ITEM_NETWORK, ITEM_CLOCK, ITEM_COLLECTIONS, ITEM_ABOUT, ITEM_COUNT };
static const struct {
    int icon;
    int label;
    int kids; // still there in Kids Mode
} items[ITEM_COUNT] = {
    [ITEM_SETTINGS]     = {ASSET_GEAR,    STR_QM_SETTINGS,     1},
    [ITEM_ACHIEVEMENTS] = {ASSET_TROPHY,  STR_QM_ACHIEVEMENTS, 1},
    [ITEM_NETWORK]      = {ASSET_WIFI_SMALL, STR_QM_NETWORK,   0},
    [ITEM_CLOCK]        = {ASSET_CLOCK,   STR_QM_CLOCK,        1},
    [ITEM_COLLECTIONS]  = {ASSET_STAR,    STR_QM_COLLECTIONS,  1},
    [ITEM_ABOUT]        = {ASSET_GAMEPAD, STR_QM_ABOUT,        1},
};

enum { SCR_ROW, SCR_SETTINGS, SCR_ACHIEVEMENTS, SCR_NETWORK, SCR_CLOCK, SCR_ABOUT, SCR_CONFIRM, SCR_MESSAGE, SCR_USB_BUSY, SCR_COUNT };

// the rows of the option pages
enum { SET_LANGUAGE, SET_AUTO_SLEEP, SET_SLEEP_OFF, SET_CLOCK_DATE, SET_KIDS, SET_RESET_NB, SET_COUNT };
enum { RA_ENABLED, RA_SOUND, RA_NOTIFY, RA_COUNT };
enum { NET_ADB, NET_ADB_NET, NET_ONLY, NET_OFF, NET_COUNT };
static const struct {
    int label;
    int desc;
    int mode; // USB_getMode()'s
    const char* arg; // usbnet.sh's
} net_rows[NET_COUNT] = {
    [NET_ADB]     = {STR_QM_ADB,      STR_QM_DESC_ADB,      USB_MODE_ADB,      "adb"},
    [NET_ADB_NET] = {STR_QM_ADB_NET,  STR_QM_DESC_ADB_NET,  USB_MODE_NET,      "adbnet"},
    [NET_ONLY]    = {STR_QM_NET_ONLY, STR_QM_DESC_NET_ONLY, USB_MODE_NET_ONLY, "net"},
    [NET_OFF]     = {STR_QM_USB_OFF,  STR_QM_DESC_USB_OFF,  USB_MODE_NONE,     "off"},
};

enum { CONFIRM_KIDS, CONFIRM_NB, CONFIRM_NB2, CONFIRM_USB };

static const int auto_sleep_minutes[] = {0, 1, 2, 5, 10}; // GetAutoSleep()
#define AUTO_SLEEP_COUNT 5
#define SLEEP_OFF_COUNT 3 // 5, 10, 15 minutes: GetSleepPowerOff()
static const int notify_labels[] = {STR_QM_NOTIFY_ALL, STR_QM_NOTIFY_UNLOCKS, STR_QM_NOTIFY_GENERAL, STR_QM_OFF};
#define NOTIFY_COUNT 4
// the date next to the time: a flat battery puts the clock back to 1970, and
// seeing the year is what tells
enum { DATE_NONE, DATE_SHORT, DATE_FULL, DATE_COUNT };
static const int date_labels[DATE_COUNT] = {STR_QM_DATE_NONE, STR_QM_DATE_SHORT, STR_QM_DATE_FULL};

enum { CURSOR_YEAR, CURSOR_MONTH, CURSOR_DAY, CURSOR_HOUR, CURSOR_MINUTE, CURSOR_SECOND, CURSOR_AMPM, CURSOR_COUNT };
// the Clock screen's fields left to right, which is also the cursor's path:
// the date as the language writes it, then the time, AM/PM last (12 hour only)
static const int clock_order_dmy[CURSOR_COUNT] = {CURSOR_DAY, CURSOR_MONTH, CURSOR_YEAR, CURSOR_HOUR, CURSOR_MINUTE, CURSOR_SECOND, CURSOR_AMPM};
static const int clock_order_mdy[CURSOR_COUNT] = {CURSOR_MONTH, CURSOR_DAY, CURSOR_YEAR, CURSOR_HOUR, CURSOR_MINUTE, CURSOR_SECOND, CURSOR_AMPM};

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
    int clock_date; // DATE_*
    int minute; // last one drawn
    char ip[INET_ADDRSTRLEN]; // the console's, on the USB network
    char pc[INET_ADDRSTRLEN]; // the PC's: its DHCP lease, or the ARP table for a fixed IP
    uint32_t ip_at;
    struct {
        pid_t pid; // usbnet.sh, switching the port in the background
        int row; // to which net_rows[] mode
        uint32_t started;
        int seconds; // shown on the wait screen
        int stuck; // past QM_USB_TIMEOUT: said so, still reaped when it ends
        int rc; // its exit code, for the message when it fails
    } usb;
    char dialog[256]; // a confirmation put together on the spot
    struct { int year, month, day, hour, minute, second, cursor; } clock; // cursor: a place in qm_clockOrder()
} qm;

static int qm_min(int a, int b) { return a < b ? a : b; }

// dates in the menu's language: month first in English (12/20/26), day first
// in Spanish (20/12/26)
static int qm_monthFirst(void) {
    return !strcmp(I18N_code(I18N_index()), "en");
}
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

// A PC with a fixed address (Windows ICS, NetworkManager "shared", a static
// IP) never asks our DHCP server for a lease. It still answers ARP, and
// usbnet.sh pings it once when the network comes up, so it's in the ARP table:
// the first complete entry (flags 0x2) on the USB interface.
static void qm_readPCFromARP(void) {
    FILE* f = fopen("/proc/net/arp", "r");
    if (!f) return;
    char line[160];
    if (fgets(line, sizeof(line), f)) { // the header
        char ip[INET_ADDRSTRLEN], dev[IFNAMSIZ];
        unsigned type, flags;
        while (fgets(line, sizeof(line), f)) {
            // IP address  HW type  Flags  HW address  Mask  Device
            if (sscanf(line, "%15s %x %x %*s %*s %15s", ip, &type, &flags, dev) == 4 &&
                (flags & 0x2) && !strcmp(dev, QM_USB_IF)) {
                snprintf(qm.pc, sizeof(qm.pc), "%s", ip);
                break;
            }
        }
    }
    fclose(f);
}

// The PC's address, from the DHCP server's lease file (busybox udhcpd: an
// 8-byte time, then 36-byte leases of expiry, address, MAC and name), else
// from the ARP table
static void qm_readPC(void) {
    qm.pc[0] = '\0';
    FILE* f = fopen(QM_USB_LEASES, "rb");
    if (!f) {
        qm_readPCFromARP();
        return;
    }
    unsigned char lease[36];
    if (fseek(f, 8, SEEK_SET) == 0) {
        while (fread(lease, sizeof(lease), 1, f) == 1) {
            uint32_t expires, nip;
            memcpy(&expires, lease, 4);
            memcpy(&nip, lease + 4, 4); // network order already
            if (!expires || !nip) continue;
            inet_ntop(AF_INET, &nip, qm.pc, sizeof(qm.pc));
            break;
        }
    }
    fclose(f);
    if (!qm.pc[0]) qm_readPCFromARP();
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
    qm_readPC();
}

static void qm_dialog(int screen, int what);

// usbnet.sh in the background: the wait screen shows until it's done
static void qm_startUSB(int row) {
    LOG_info("Quick Menu: %s %s\n", QM_USBNET, net_rows[row].arg);
    pid_t pid = fork();
    if (pid == 0) {
        // the daemons it starts live on after us: none of minui's files (the
        // GPU's) go with them, and it logs on its own
        setsid();
        for (int fd = 3; fd < 1024; fd++) close(fd);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, 0);
            dup2(null, 1);
            dup2(null, 2);
        }
        execl("/bin/sh", "sh", QM_USBNET, net_rows[row].arg, (char*)NULL);
        _exit(127);
    }
    if (pid < 0) {
        qm.usb.rc = -1;
        qm_dialog(SCR_MESSAGE, STR_QM_USB_FAILED);
        return;
    }
    qm.usb.pid = pid;
    qm.usb.row = row;
    qm.usb.started = SDL_GetTicks();
    qm.usb.seconds = 0;
    qm.usb.stuck = 0;
    qm.back = SCR_NETWORK;
    qm.screen = SCR_USB_BUSY;
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
    // usbnet.sh writes the file; this is for a port set up some other way
    // (once a boot: after that the file says, it's in tmpfs)
    if (!exists(USB_MODE_PATH)) {
        int adbd = qm_adbdRunning();
        int mode = qm_usbUp() ? (adbd ? USB_MODE_NET : USB_MODE_NET_ONLY) : adbd ? USB_MODE_ADB : USB_MODE_NONE;
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
    qm.clock_date = exists(QM_DATE_PATH) ? qm_min(qm_max(getInt(QM_DATE_PATH), DATE_NONE), DATE_COUNT - 1) : DATE_NONE;
    qm.minute = -1;
    qm.ip_at = 0;
    if (USB_hasNetwork(USB_getMode())) qm_readIP();
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
        static const int ids[SET_COUNT] = {STR_QM_LANGUAGE, STR_QM_AUTO_SLEEP, STR_QM_SLEEP_OFF, STR_QM_CLOCK_DATE, STR_QM_KIDS_MODE, STR_NB_MENU_RESET};
        return ids[row];
    }
    case SCR_ACHIEVEMENTS: {
        static const int ids[RA_COUNT] = {STR_QM_RA_ENABLED, STR_QM_RA_SOUND, STR_QM_RA_NOTIFY};
        return ids[row];
    }
    }
    return net_rows[row].label;
}

static int qm_rowDesc(int screen, int row) {
    switch (screen) {
    case SCR_SETTINGS: {
        static const int ids[SET_COUNT] = {STR_QM_DESC_LANGUAGE, STR_QM_DESC_AUTO_SLEEP, STR_QM_DESC_SLEEP_OFF, STR_QM_DESC_CLOCK_DATE, STR_QM_DESC_KIDS_MODE, STR_QM_DESC_RESET_NB};
        return ids[row];
    }
    case SCR_ACHIEVEMENTS: {
        static const int ids[RA_COUNT] = {STR_QM_DESC_RA_ENABLED, STR_QM_DESC_RA_SOUND, STR_QM_DESC_RA_NOTIFY};
        return ids[row];
    }
    }
    return net_rows[row].desc;
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
        case SET_CLOCK_DATE: return DATE_COUNT;
        case SET_KIDS: return 2;
        }
        return 0; // Reset NekoBit only does something
    case SCR_ACHIEVEMENTS: return row == RA_NOTIFY ? NOTIFY_COUNT : 2;
    case SCR_NETWORK: return 3; // see qm_valueText
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
        case SET_CLOCK_DATE: return qm.clock_date;
        case SET_KIDS: return qm.kids;
        }
        return 0;
    case SCR_ACHIEVEMENTS:
        switch (row) {
        case RA_ENABLED: return GetRAEnabled() ? 1 : 0;
        case RA_SOUND: return GetRASound() ? 1 : 0;
        }
        return qm_min(qm_max(GetRANotify(), 0), NOTIFY_COUNT - 1);
    case SCR_NETWORK: // 0 enable, 1 active, 2 being enabled
        if (qm.usb.pid) return qm.usb.row == row ? 2 : 0;
        return USB_getMode() == net_rows[row].mode ? 1 : 0;
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
        case SET_CLOCK_DATE: snprintf(out, size, "%s", T(date_labels[i])); break;
        case SET_KIDS: snprintf(out, size, "%s", T(i ? STR_QM_ON : STR_QM_OFF)); break;
        }
        break;
    case SCR_ACHIEVEMENTS:
        if (row == RA_NOTIFY) snprintf(out, size, "%s", T(notify_labels[i]));
        else snprintf(out, size, "%s", T(i ? STR_QM_ON : STR_QM_OFF));
        break;
    case SCR_NETWORK: {
        static const int ids[3] = {STR_QM_ENABLE, STR_QM_ACTIVE, STR_QM_ACTIVATING};
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
        case SET_CLOCK_DATE:
            qm.clock_date = qm_wrap(qm.clock_date + dir, DATE_COUNT);
            if (qm.clock_date == DATE_NONE) unlink(QM_DATE_PATH);
            else putInt(QM_DATE_PATH, qm.clock_date);
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
    case SCR_NETWORK: {
        // live: from whatever it is now, once the last switch is over
        int from = USB_getMode(), to = net_rows[row].mode;
        if (qm.usb.pid || from == to) break;
        // what goes, said up front: ADB, the network, or both
        int drops_adb = from == USB_MODE_ADB || from == USB_MODE_NET;
        drops_adb = drops_adb && !(to == USB_MODE_ADB || to == USB_MODE_NET);
        int drops_net = USB_hasNetwork(from) && !USB_hasNetwork(to);
        int n;
        if (to == USB_MODE_NONE) n = snprintf(qm.dialog, sizeof(qm.dialog), "%s", T(STR_QM_CONFIRM_OFF));
        else n = snprintf(qm.dialog, sizeof(qm.dialog), T(STR_QM_CONFIRM_USB), T(net_rows[row].label));
        int drops = drops_adb && drops_net ? STR_QM_DROPS_BOTH : drops_adb ? STR_QM_DROPS_ADB : drops_net ? STR_QM_DROPS_NET : -1;
        if (drops >= 0 && n > 0 && n < (int)sizeof(qm.dialog))
            snprintf(qm.dialog + n, sizeof(qm.dialog) - n, "%s", T(drops));
        qm.usb.row = row;
        qm_dialog(SCR_CONFIRM, CONFIRM_USB);
        break;
    }
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
    case CONFIRM_USB:
        qm_startUSB(qm.usb.row);
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

static const int* qm_clockOrder(void) {
    return qm_monthFirst() ? clock_order_mdy : clock_order_dmy;
}

// how many places the cursor has: AM/PM, the last, only in 12 hour mode
static int qm_clockFields(void) {
    return qm.show_24hour ? CURSOR_COUNT - 1 : CURSOR_COUNT;
}

static void qm_clockStep(int dir) {
    switch (qm_clockOrder()[qm.clock.cursor]) {
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

    // a switch half done can't be stopped, nor the menu left: wait it out
    if (qm.screen == SCR_USB_BUSY) return 0;

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
            case ITEM_COLLECTIONS:
                qm_close();
                *action = QM_COLLECTIONS;
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
    uint32_t now = SDL_GetTicks();
    if (qm.usb.pid) {
        int status;
        if (waitpid(qm.usb.pid, &status, WNOHANG) == qm.usb.pid) {
            qm.usb.pid = 0;
            qm.usb.rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            LOG_info("Quick Menu: usbnet.sh %s -> %i\n", net_rows[qm.usb.row].arg, qm.usb.rc);
            USB_reloadMode(); // usbnet.sh wrote it
            qm.ip[0] = qm.pc[0] = '\0';
            qm.ip_at = 0;
            if (qm.screen == SCR_USB_BUSY) qm.screen = SCR_NETWORK;
            if (qm.usb.rc != 0 && !qm.usb.stuck && qm.open) qm_dialog(SCR_MESSAGE, STR_QM_USB_FAILED);
            dirty = 1;
        }
        else if (qm.screen == SCR_USB_BUSY) {
            int seconds = (now - qm.usb.started) / 1000;
            if (seconds != qm.usb.seconds) {
                qm.usb.seconds = seconds;
                dirty = 1;
            }
            // not waited for any longer (stuck in the kernel, it may never
            // end), but still reaped above if it does
            if (seconds >= QM_USB_TIMEOUT) {
                qm.usb.stuck = 1;
                qm.screen = SCR_NETWORK;
                qm_dialog(SCR_MESSAGE, STR_QM_USB_STUCK);
            }
        }
    }
    if (!qm.open) return dirty;

    if (USB_hasNetwork(USB_getMode()) && (!qm.ip_at || now - qm.ip_at >= 2000)) {
        char was[sizeof(qm.ip) + sizeof(qm.pc)];
        snprintf(was, sizeof(was), "%s %s", qm.ip, qm.pc);
        qm_readIP();
        qm.ip_at = now;
        char is[sizeof(was)];
        snprintf(is, sizeof(is), "%s %s", qm.ip, qm.pc);
        if (strcmp(was, is)) dirty = 1;
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

// "20:40", or with the date before it: "20/12 20:40", "20/12/26 20:40" (day
// first; month first in English, "12/20/26 8:40 PM")
static void qm_timeText(char* out, size_t size) {
    time_t t = time(NULL);
    struct tm tm = *localtime(&t);
    int n = 0;
    if (qm.clock_date != DATE_NONE) {
        int month_first = qm_monthFirst();
        int a = month_first ? tm.tm_mon + 1 : tm.tm_mday;
        int b = month_first ? tm.tm_mday : tm.tm_mon + 1;
        if (qm.clock_date == DATE_FULL) n = snprintf(out, size, "%02i/%02i/%02i ", a, b, tm.tm_year % 100);
        else n = snprintf(out, size, "%02i/%02i ", a, b);
        if (n < 0 || n >= (int)size) n = 0;
    }
    if (qm.show_24hour) snprintf(out + n, size - n, "%02i:%02i", tm.tm_hour, tm.tm_min);
    else {
        int hour = tm.tm_hour % 12;
        snprintf(out + n, size - n, "%i:%02i %s", hour ? hour : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
    }
}

// the time, centred on the battery pill's row
static void qm_drawTime(SDL_Surface* dst) {
    char now[32];
    qm_timeText(now, sizeof(now));
    GFX_Text text = GFX_renderText(font.small, now, COLOR_WHITE); // smaller than the menu's: the date fits
    int w = text.w + SCALE1(BUTTON_PADDING * 2);
    int x = (dst->w - w) / 2;
    GFX_blitPill(ASSET_DARK_GRAY_PILL, dst, &(SDL_Rect){x, SCALE1(PADDING), w, SCALE1(PILL_SIZE)});
    GFX_blitRenderedText(&text, NULL, dst, x + SCALE1(BUTTON_PADDING), SCALE1(PADDING) + (SCALE1(PILL_SIZE) - text.h) / 2);
}

// a page's name, top left: its icon and label in a pill, darker, as NekoBit's
static void qm_drawTitle(SDL_Surface* dst, int item) {
    GFX_Text text = GFX_renderText(font.large, T(items[item].label), COLOR_DARK_TEXT);
    int icon_w = SCALE1(PILL_SIZE - BUTTON_PADDING);
    int w = SCALE1(BUTTON_PADDING / 2) + icon_w + text.w + SCALE1(BUTTON_PADDING);
    GFX_blitPill(ASSET_DARK_GRAY_PILL, dst, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING), w, SCALE1(PILL_SIZE)});
    GFX_blitAssetScaled(items[item].icon, dst, &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING / 2), SCALE1(PADDING), icon_w, SCALE1(PILL_SIZE)}, 1, COLOR_DARK_TEXT);
    GFX_blitRenderedText(&text, NULL, dst, SCALE1(PADDING + BUTTON_PADDING / 2) + icon_w, SCALE1(PADDING) + (SCALE1(PILL_SIZE) - text.h) / 2);
}

// what USB is doing, a line of small text centred at y
static void qm_drawUSBInfo(SDL_Surface* dst, int y) {
    int mode = USB_getMode();
    if (mode == USB_MODE_NONE) return;
    char line[128];
    if (mode == USB_MODE_ADB) snprintf(line, sizeof(line), "%s", T(STR_QM_INFO_ADB));
    else if (!qm.ip[0]) snprintf(line, sizeof(line), "%s", T(STR_QM_INFO_NO_IP));
    else if (qm.pc[0]) snprintf(line, sizeof(line), T(STR_QM_INFO_NET), qm.ip, qm.pc);
    else snprintf(line, sizeof(line), T(STR_QM_INFO_NET_NO_PC), qm.ip);
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
            int index = qm_valueIndex(qm.screen, rows[j]);
            qm_valueText(qm.screen, rows[j], index, value, sizeof(value));
            // the USB mode in use: its value on a white badge, on any row
            int badge = qm.screen == SCR_NETWORK && index == 1;
            text = GFX_renderText(font.tiny, value, badge ? COLOR_BLACK : COLOR_WHITE);
            int vx = ox + mw - text.w - SCALE1(QM_OPTION_PADDING);
            if (badge) GFX_blitPill(ASSET_BUTTON, dst, &(SDL_Rect){vx - SCALE1(QM_OPTION_PADDING), y, text.w + SCALE1(QM_OPTION_PADDING * 2), SCALE1(BUTTON_SIZE)});
            GFX_blitRenderedText(&text, NULL, dst, vx, y + SCALE1(3));
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
    };
    char text[256];
    if (qm.screen == SCR_CONFIRM && qm.confirm == CONFIRM_USB) snprintf(text, sizeof(text), "%s", qm.dialog);
    else if (qm.screen == SCR_MESSAGE && qm.message == STR_QM_USB_FAILED) snprintf(text, sizeof(text), T(STR_QM_USB_FAILED), qm.usb.rc);
    else snprintf(text, sizeof(text), "%s", T(qm.screen == SCR_CONFIRM ? confirms[qm.confirm] : qm.message));
    GFX_blitMessage(font.medium, text, dst, &(SDL_Rect){0, SCALE1(PADDING), dst->w, dst->h - SCALE1(PILL_SIZE + PADDING)});

    if (qm.screen == SCR_CONFIRM)
        qm_hints(dst, show_setting, (char*[]){"B", (char*)T(STR_MA_CANCEL), "A", (char*)T(STR_MA_CONFIRM), NULL});
    else
        qm_hints(dst, show_setting, (char*[]){"A", (char*)T(STR_MA_OKAY), NULL, NULL});
}

// while usbnet.sh works: what it's doing and for how long, no buttons
static void qm_drawUSBBusy(SDL_Surface* dst) {
    qm_drawTitle(dst, ITEM_NETWORK);
    char text[256];
    snprintf(text, sizeof(text), T(STR_QM_USB_BUSY), T(net_rows[qm.usb.row].label), qm.usb.seconds);
    GFX_blitMessage(font.medium, text, dst, &(SDL_Rect){0, SCALE1(PADDING), dst->w, dst->h - SCALE1(PILL_SIZE + PADDING)});
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
    int field_x[CURSOR_COUNT] = {0}, field_w[CURSOR_COUNT] = {0}; // where each field went, for the underline

    // the date, in the language's order (the same width either way)
    const int* order = qm_clockOrder();
    for (int i = 0; i < 3; i++) {
        int field = order[i];
        if (i) x = qm_clockGlyph(dst, "/", x, y);
        field_x[field] = x;
        if (field == CURSOR_YEAR) x = qm_clockNumber(dst, qm.clock.year, 4, x, y);
        else x = qm_clockNumber(dst, field == CURSOR_MONTH ? qm.clock.month : qm.clock.day, 2, x, y);
        field_w[field] = x - field_x[field];
    }
    x += SCALE1(10);
    int hour = qm.clock.hour;
    if (!qm.show_24hour) {
        hour %= 12;
        if (!hour) hour = 12;
    }
    const int time_fields[3] = {CURSOR_HOUR, CURSOR_MINUTE, CURSOR_SECOND};
    const int time_values[3] = {hour, qm.clock.minute, qm.clock.second};
    for (int i = 0; i < 3; i++) {
        if (i) x = qm_clockGlyph(dst, ":", x, y);
        field_x[time_fields[i]] = x;
        x = qm_clockNumber(dst, time_values[i], 2, x, y);
        field_w[time_fields[i]] = x - field_x[time_fields[i]];
    }
    if (!qm.show_24hour) {
        x += SCALE1(10);
        GFX_Text text = GFX_renderText(font.large, qm.clock.hour < 12 ? "AM" : "PM", COLOR_WHITE);
        field_x[CURSOR_AMPM] = x;
        field_w[CURSOR_AMPM] = text.w + SCALE1(2);
        GFX_blitRenderedText(&text, NULL, dst, x, y - SCALE1(3));
    }

    // the field being changed
    int field = order[qm_min(qm.clock.cursor, qm_clockFields() - 1)];
    GFX_blitPill(ASSET_UNDERLINE, dst, &(SDL_Rect){field_x[field], y + SCALE1(19), field_w[field]});

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
    case SCR_USB_BUSY: qm_drawUSBBusy(dst); break;
    default: qm_drawPage(dst, show_setting); break;
    }
}
