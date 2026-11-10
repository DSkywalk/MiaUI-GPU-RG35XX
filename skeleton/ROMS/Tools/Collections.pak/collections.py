#!/usr/bin/env python3
"""Collections manager for MiaUI: create, rename and delete the lists in
/userdata/Collections. It only manages the lists: it never adds, removes or
touches the games in them.

A collection is /userdata/Collections/<name>.txt, one SD-relative path per line.
Every write goes through a temporary file and a rename, so a power loss can't
leave a list half written.

Testing: COLLECTIONS_DIR points it at another folder, COLLECTIONS_INPUT is a
comma-separated list of buttons played back as input (A,B,X,Y,START,MENU,UP,
DOWN,LEFT,RIGHT, or WAIT), and COLLECTIONS_SHOT=<file> saves the screen there
whenever a WAIT is reached.
"""
import os
import sys

import pygame

COLLECTIONS_DIR = os.environ.get("COLLECTIONS_DIR", "/userdata/Collections")
LANG_CONF = "/userdata/Profile/language.conf"
FONT = "/opt/minui/res/BPreplayBold-unhinted.otf"
MAX_NAME = 40
# offered when the tool opens and there is no collection at all
DEFAULTS = ["Backlog", "Playing", "Paused", "Beaten", "Abandoned"]

# the console's joystick buttons (as the other MiaUI tools)
BTN_A, BTN_B, BTN_X, BTN_Y = 0, 1, 2, 3
BTN_START, BTN_MENU = 9, 10
BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT = 13, 14, 15, 16
REPEATS = (BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT)
NAMES = {"A": BTN_A, "B": BTN_B, "X": BTN_X, "Y": BTN_Y, "START": BTN_START, "MENU": BTN_MENU,
         "UP": BTN_UP, "DOWN": BTN_DOWN, "LEFT": BTN_LEFT, "RIGHT": BTN_RIGHT}

BG = (30, 30, 30)
WHITE = (255, 255, 255)
GREY = (200, 200, 200)
DIM = (150, 150, 150)
GREEN = (50, 255, 50)
SOFT_GREEN = (150, 255, 150)
RED = (255, 100, 100)
BOX = (45, 45, 50)
LINE = (80, 80, 90)

TEXTS = {
    "en": {
        "title": "Collections",
        "new": "+ New collection",
        "games": "%d games",
        "game": "1 game",
        "empty_list": "No collections yet",
        "inst_list": "A: rename   X: delete   B: exit",
        "inst_new": "A: create a new collection   B: exit",
        "inst_list2": "Only the lists are managed here: the games stay.",
        "kb_new": "New collection",
        "kb_rename": "Rename collection",
        "kb_space": "SPACE",
        "kb_del": "DEL",
        "kb_ok": "OK",
        "kb_inst1": "A: type   B: delete   X: change keys",
        "kb_inst2": "START: done   MENU: cancel",
        "del_q": "Delete \"%s\"?",
        "del_1": "Only the list is deleted: the games stay.",
        "del_2": "A: delete   B: cancel",
        "err_empty": "The name can't be empty.",
        "err_slash": "The name can't have / or \\ in it.",
        "err_dot": "The name can't start with a dot.",
        "err_exists": "There's already a collection called \"%s\".",
        "err_io": "It couldn't be saved: %s",
        "press": "Press A to go back.",
        "def_q": "No collections yet",
        "def_1": "Create the default ones?",
        "def_2": "A: create them   B: no, thanks",
    },
    "es": {
        "title": "Colecciones",
        "new": "+ Nueva colección",
        "games": "%d juegos",
        "game": "1 juego",
        "empty_list": "Aún no hay colecciones",
        "inst_list": "A: renombrar   X: borrar   B: salir",
        "inst_new": "A: crear una colección nueva   B: salir",
        "inst_list2": "Aquí solo se gestionan las listas: los juegos no se tocan.",
        "kb_new": "Nueva colección",
        "kb_rename": "Renombrar colección",
        "kb_space": "ESPACIO",
        "kb_del": "BORRAR",
        "kb_ok": "OK",
        "kb_inst1": "A: escribir   B: borrar   X: cambiar teclas",
        "kb_inst2": "START: listo   MENU: cancelar",
        "del_q": "¿Borrar \"%s\"?",
        "del_1": "Solo se borra la lista: los juegos se quedan.",
        "del_2": "A: borrar   B: cancelar",
        "err_empty": "El nombre no puede estar vacío.",
        "err_slash": "El nombre no puede tener / ni \\.",
        "err_dot": "El nombre no puede empezar por un punto.",
        "err_exists": "Ya hay una colección llamada \"%s\".",
        "err_io": "No se ha podido guardar: %s",
        "press": "Pulsa A para volver.",
        "def_q": "Aún no hay colecciones",
        "def_1": "¿Crear las de siempre?",
        "def_2": "A: crearlas   B: no, gracias",
    },
}


def language():
    try:
        with open(LANG_CONF) as f:
            code = f.read().strip()[:2]
    except OSError:
        code = "en"
    return code if code in TEXTS else "en"


T = TEXTS[language()]


# ---- the collection files ------------------------------------------------------

def path_of(name):
    return os.path.join(COLLECTIONS_DIR, name + ".txt")


def list_collections():
    """[(name, game count)], sorted like the main menu (case-insensitive)."""
    out = []
    try:
        names = os.listdir(COLLECTIONS_DIR)
    except OSError:
        return out
    for n in names:
        if n.startswith(".") or not n.endswith(".txt"):
            continue
        p = os.path.join(COLLECTIONS_DIR, n)
        if not os.path.isfile(p):
            continue
        try:
            with open(p, errors="replace") as f:
                count = sum(1 for line in f if line.strip())
        except OSError:
            count = 0
        out.append((n[:-4], count))
    out.sort(key=lambda c: c[0].lower())
    return out


def check_name(name, current=None):
    """None if `name` can be used (for a new list, or to rename `current`), else the error."""
    if not name:
        return T["err_empty"]
    if "/" in name or "\\" in name:
        return T["err_slash"]
    if name.startswith("."):
        return T["err_dot"]
    for other, _ in list_collections():
        if other.lower() == name.lower() and other != current:
            return T["err_exists"] % other
    return None


def create(name):
    os.makedirs(COLLECTIONS_DIR, exist_ok=True)
    tmp = os.path.join(COLLECTIONS_DIR, ".collection.tmp")
    with open(tmp, "w") as f:
        os.fchmod(f.fileno(), 0o666)
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, path_of(name))
    os.sync()


def rename(old, new):
    if old == new:
        return
    if old.lower() == new.lower():   # only the case changes: safe on any filesystem
        tmp = os.path.join(COLLECTIONS_DIR, ".collection.rename")
        os.rename(path_of(old), tmp)
        os.rename(tmp, path_of(new))
    else:
        os.rename(path_of(old), path_of(new))
    os.sync()


def delete(name):
    os.remove(path_of(name))
    os.sync()


# ---- input -------------------------------------------------------------------------

class Input:
    """Buttons pressed since the last call, with key repeat on the D-pad
    (a keyboard works too, for a PC)."""
    KEYS = {pygame.K_RETURN: BTN_A, pygame.K_BACKSPACE: BTN_B, pygame.K_ESCAPE: BTN_MENU,
            pygame.K_UP: BTN_UP, pygame.K_DOWN: BTN_DOWN, pygame.K_LEFT: BTN_LEFT,
            pygame.K_RIGHT: BTN_RIGHT, pygame.K_TAB: BTN_X, pygame.K_F1: BTN_Y,
            pygame.K_F2: BTN_START}

    def __init__(self, screen):
        self.screen = screen
        self.held = {}   # button -> time of its next repeat
        script = os.environ.get("COLLECTIONS_INPUT")
        self.script = [s.strip().upper() for s in script.split(",")] if script else None
        self.shot = os.environ.get("COLLECTIONS_SHOT")
        self.shots = 0

    def get(self):
        if self.script is not None:
            return self.scripted()
        now = pygame.time.get_ticks()
        out = []
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                out.append(BTN_MENU)
            elif event.type in (pygame.JOYBUTTONDOWN, pygame.KEYDOWN):
                b = event.button if event.type == pygame.JOYBUTTONDOWN else self.KEYS.get(event.key)
                if b is None:
                    continue
                out.append(b)
                if b in REPEATS:
                    self.held[b] = now + 350
            elif event.type in (pygame.JOYBUTTONUP, pygame.KEYUP):
                b = event.button if event.type == pygame.JOYBUTTONUP else self.KEYS.get(event.key)
                self.held.pop(b, None)
        for b, t in list(self.held.items()):
            if now >= t:
                out.append(b)
                self.held[b] = now + 90
        return out

    def scripted(self):
        """One scripted button per frame; the screen of the frame before a WAIT is saved."""
        pygame.event.get()
        if not self.script:
            return [BTN_MENU]
        step = self.script.pop(0)
        if step == "WAIT":
            if self.shot:
                self.shots += 1
                pygame.image.save(self.screen, "%s-%02d.bmp" % (self.shot, self.shots))
            return []
        return [NAMES[step]] if step in NAMES else []

    def release(self):
        """After a screen closes: what is still held doesn't carry into the next one."""
        self.held.clear()
        if self.script is None:
            pygame.time.delay(150)
            pygame.event.clear()


# ---- drawing ----------------------------------------------------------------------

def fit(font, text, width):
    if font.size(text)[0] <= width:
        return text
    while text and font.size(text + "...")[0] > width:
        text = text[:-1]
    return text + "..."


def draw_instructions(screen, font, lines):
    box = pygame.Rect(10, 370, 620, 100)
    pygame.draw.rect(screen, BOX, box)
    pygame.draw.rect(screen, LINE, box, 2)
    y = 385
    for line in lines:
        screen.blit(font.render(fit(font, line, 590), True, GREY), (25, y))
        y += 26


class UI:
    def __init__(self):
        pygame.display.init()
        pygame.font.init()
        pygame.joystick.init()
        for i in range(pygame.joystick.get_count()):
            pygame.joystick.Joystick(i).init()
        pygame.mouse.set_visible(False)
        self.screen = pygame.display.set_mode((640, 480))
        try:
            self.font_title = pygame.font.Font(FONT, 29, bold=True)
        except TypeError:
            self.font_title = pygame.font.Font(FONT, 29)
        except OSError:
            self.font_title = pygame.font.Font(None, 34)
        try:
            self.font_list = pygame.font.Font(FONT, 25)
            self.font_info = pygame.font.Font(FONT, 23)
        except OSError:
            self.font_list = pygame.font.Font(None, 30)
            self.font_info = pygame.font.Font(None, 27)
        self.input = Input(self.screen)
        self.clock = pygame.time.Clock()

    def flip(self):
        pygame.display.flip()
        self.clock.tick(30)

    def message(self, lines, color=WHITE):
        """Lines of text until A, B or MENU."""
        while True:
            self.screen.fill(BG)
            y = 120
            for line in lines:
                self.screen.blit(self.font_list.render(fit(self.font_list, line, 590), True, color), (30, y))
                y += 35
            draw_instructions(self.screen, self.font_info, [T["press"]])
            self.flip()
            if any(b in (BTN_A, BTN_B, BTN_MENU) for b in self.input.get()):
                self.input.release()
                return

    def confirm(self, lines):
        """True for A, False for B / MENU."""
        while True:
            self.screen.fill(BG)
            y = 120
            for i, line in enumerate(lines[:-1]):
                font = self.font_title if i == 0 else self.font_list
                self.screen.blit(font.render(fit(font, line, 590), True, WHITE if i == 0 else GREY), (30, y))
                y += 45
            draw_instructions(self.screen, self.font_info, [lines[-1]])
            self.flip()
            for b in self.input.get():
                if b == BTN_A:
                    self.input.release()
                    return True
                if b in (BTN_B, BTN_MENU):
                    self.input.release()
                    return False

    # three pages of 4 x 10 keys, then a row of four wide keys
    KB_PAGES = [
        ["1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.&"],
        ["1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.&"],
        ["1234567890", "!@#$%+=?'\"", "()[]{}<>~^", ":;,*|`_-.&"],
    ]
    KB_NEXT = ["ABC", "#+=", "abc"]

    def keyboard(self, title, text=""):
        """The typed text, or None if MENU cancels it."""
        page, row, col = 0, 1, 0
        key_w, key_h, x0, y0 = 58, 44, 22, 128
        wide_w = (10 * (key_w + 2) - 2 - 3 * 2) // 4
        while True:
            s = self.screen
            s.fill(BG)
            s.blit(self.font_title.render(title, True, WHITE), (20, 15))
            field = pygame.Rect(20, 60, 600, 50)
            pygame.draw.rect(s, BOX, field)
            pygame.draw.rect(s, LINE, field, 2)
            surf = self.font_list.render(text + "_", True, SOFT_GREEN)
            if surf.get_width() > 580:   # long text: keep its end in view
                s.blit(surf, (610 - surf.get_width(), 70), pygame.Rect(surf.get_width() - 580, 0, 580, 40))
            else:
                s.blit(surf, (30, 70))
            for r, chars in enumerate(self.KB_PAGES[page]):
                for c, ch in enumerate(chars):
                    self.key(pygame.Rect(x0 + c * (key_w + 2), y0 + r * (key_h + 4), key_w, key_h),
                             ch, (r, c) == (row, col))
            labels = [self.KB_NEXT[page], T["kb_space"], T["kb_del"], T["kb_ok"]]
            for c, label in enumerate(labels):
                self.key(pygame.Rect(x0 + c * (wide_w + 2), y0 + 4 * (key_h + 4), wide_w, key_h),
                         label, (row, col) == (4, c))
            draw_instructions(s, self.font_info, [T["kb_inst1"], T["kb_inst2"]])
            self.flip()

            for b in self.input.get():
                if b == BTN_MENU:
                    self.input.release()
                    return None
                if b == BTN_START:
                    self.input.release()
                    return text
                if b == BTN_B:
                    text = text[:-1]
                elif b == BTN_X:
                    page = (page + 1) % len(self.KB_PAGES)
                elif b == BTN_UP:
                    if row == 4:
                        row, col = 3, min(9, col * 10 // 4 + 1)
                    elif row == 0:
                        row, col = 4, col * 4 // 10
                    else:
                        row -= 1
                elif b == BTN_DOWN:
                    if row == 3:
                        row, col = 4, col * 4 // 10
                    elif row == 4:
                        row, col = 0, min(9, col * 10 // 4 + 1)
                    else:
                        row += 1
                elif b == BTN_LEFT:
                    col = (col - 1) % (4 if row == 4 else 10)
                elif b == BTN_RIGHT:
                    col = (col + 1) % (4 if row == 4 else 10)
                elif b == BTN_A:
                    if row < 4:
                        if len(text) < MAX_NAME:
                            text += self.KB_PAGES[page][row][col]
                    elif col == 0:
                        page = (page + 1) % len(self.KB_PAGES)
                    elif col == 1:
                        if len(text) < MAX_NAME:
                            text += " "
                    elif col == 2:
                        text = text[:-1]
                    else:
                        self.input.release()
                        return text

    def key(self, rect, label, on):
        pygame.draw.rect(self.screen, (60, 90, 60) if on else (50, 50, 55), rect)
        pygame.draw.rect(self.screen, GREEN if on else LINE, rect, 2)
        k = self.font_list.render(fit(self.font_list, label, rect.w - 8), True, WHITE if on else GREY)
        self.screen.blit(k, (rect.centerx - k.get_width() // 2, rect.centery - k.get_height() // 2))

    def ask_name(self, title, text="", current=None):
        """A valid name typed on the keyboard, or None if cancelled."""
        while True:
            name = self.keyboard(title, text)
            if name is None:
                return None
            name = " ".join(name.split())   # no leading, trailing or double spaces
            error = check_name(name, current)
            if error is None:
                return name
            self.message([error], RED)
            text = name

    def offer_defaults(self):
        """No collection at all: offer the usual ones."""
        if list_collections():
            return
        if not self.confirm([T["def_q"], T["def_1"], ", ".join(DEFAULTS), T["def_2"]]):
            return
        try:
            for name in DEFAULTS:
                if check_name(name) is None:
                    create(name)
        except OSError as e:
            self.message([T["err_io"] % e.strerror], RED)

    def main(self):
        self.offer_defaults()
        selected, top, rows = 0, 0, 6
        while True:
            lists = list_collections()
            items = [T["new"]] + [n for n, _ in lists]
            selected = max(0, min(selected, len(items) - 1))
            if selected < top:
                top = selected
            elif selected >= top + rows:
                top = selected - rows + 1

            s = self.screen
            s.fill(BG)
            s.blit(self.font_title.render(T["title"], True, WHITE), (20, 20))
            y = 70
            for i in range(top, min(top + rows, len(items))):
                on = i == selected
                color = GREEN if on else GREY
                label = ("> " if on else "  ") + items[i]
                count = ""
                if i > 0:
                    n = lists[i - 1][1]
                    count = T["game"] if n == 1 else T["games"] % n
                    cw = self.font_info.size(count)[0]
                    s.blit(self.font_info.render(count, True, DIM), (620 - cw, y + 3))
                    width = 600 - cw - 20
                else:
                    width = 600
                s.blit(self.font_list.render(fit(self.font_list, label, width), True, color), (20, y))
                y += 45
            if not lists:
                s.blit(self.font_info.render(T["empty_list"], True, DIM), (40, y + 10))
            if len(items) > rows:
                more = self.font_info.render("%d / %d" % (selected + 1, len(items)), True, DIM)
                s.blit(more, (620 - more.get_width(), 26))
            draw_instructions(s, self.font_info, [T["inst_new"] if selected == 0 else T["inst_list"],
                                                  T["inst_list2"]])
            self.flip()

            for b in self.input.get():
                if b in (BTN_B, BTN_MENU):
                    return
                if b == BTN_UP:
                    selected = (selected - 1) % len(items)
                elif b == BTN_DOWN:
                    selected = (selected + 1) % len(items)
                elif b == BTN_LEFT:
                    selected = max(0, selected - rows)
                elif b == BTN_RIGHT:
                    selected = min(len(items) - 1, selected + rows)
                elif b == BTN_A and selected == 0:
                    self.input.release()
                    name = self.ask_name(T["kb_new"])
                    if name is not None:
                        try:
                            create(name)
                            selected = 1 + [n for n, _ in list_collections()].index(name)
                        except OSError as e:
                            self.message([T["err_io"] % e.strerror], RED)
                    break
                elif b == BTN_A:
                    self.input.release()
                    old = items[selected]
                    name = self.ask_name(T["kb_rename"], old, current=old)
                    if name is not None and name != old:
                        try:
                            rename(old, name)
                            selected = 1 + [n for n, _ in list_collections()].index(name)
                        except OSError as e:
                            self.message([T["err_io"] % e.strerror], RED)
                    break
                elif b == BTN_X and selected > 0:
                    self.input.release()
                    name = items[selected]
                    if self.confirm([T["del_q"] % name, T["del_1"], T["del_2"]]):
                        try:
                            delete(name)
                        except OSError as e:
                            self.message([T["err_io"] % e.strerror], RED)
                    break


def main():
    ui = UI()
    try:
        ui.main()
    finally:
        # as the Updater: leave the screen blank for minui
        for _ in range(10):
            ui.screen.fill(BG)
            pygame.display.flip()
        pygame.quit()
        os.system("cat /dev/zero > /dev/fb0 2>/dev/null")
    return 0


if __name__ == "__main__":
    sys.exit(main())
