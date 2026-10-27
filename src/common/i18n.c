#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "i18n.h"

// the system's own language setting, shared with mplayer and the Updater
#ifndef I18N_PATH
#include "defines.h"
#define I18N_PATH USERDATA_PATH "/language.conf"
#endif

static const char* const lang_en[STR_COUNT] = {
#define X(id, text) [id] = text,
#include "i18n_strings.h"
#undef X
};

// Each translation is a sparse table: [STR_x] = "...". Gaps fall back to English.
static const char* const lang_es[STR_COUNT] = {
#include "lang/es.h"
};

static const struct {
    const char* code;
    const char* const* strings;
} languages[] = {
    {"en", lang_en},
    {"es", lang_es},
};

static int current = 0;

const char* T(int id) {
    if (id < 0 || id >= STR_COUNT) return "";
    const char* s = languages[current].strings[id];
    return s ? s : lang_en[id];
}

int I18N_setLanguage(const char* code) {
    for (int i = 0; i < (int)(sizeof(languages) / sizeof(languages[0])); i++) {
        if (!strcmp(code, languages[i].code)) {
            current = i;
            return 1;
        }
    }
    return 0;
}

const char* I18N_getLanguage(void) {
    return languages[current].code;
}

// "es", "ES", "es_ES", "es-ES\n": the first word, lowercased, then just its
// first two letters if the whole word isn't a known code
void I18N_init(void) {
    FILE* f = fopen(I18N_PATH, "r");
    if (!f) return;
    char code[32] = {0};
    if (fgets(code, sizeof(code), f)) {
        code[strcspn(code, " \t\r\n")] = '\0';
        for (int i = 0; code[i]; i++) code[i] = tolower((unsigned char)code[i]);
        if (!I18N_setLanguage(code) && strlen(code) > 2) {
            code[2] = '\0';
            I18N_setLanguage(code);
        }
    }
    fclose(f);
}
