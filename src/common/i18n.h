#ifndef __I18N_H__
#define __I18N_H__

// Translatable strings. Every language is compiled in; a string a language
// doesn't translate falls back to English.
//
//   T(STR_OPEN)                          -> "OPEN" / "ABRIR"
//   snprintf(buf, n, T(STR_NB_LEVEL), 3) -> "Level 3" / "Nivel 3"
//
// To translate: copy lang/es.h, translate what you like, list it in i18n.c.

enum {
#define X(id, text) id,
#include "i18n_strings.h"
#undef X
    STR_COUNT
};

const char* T(int id);

// "en", "es", ... Unknown codes keep the current language. Returns 1 if set.
int I18N_setLanguage(const char* code);
const char* I18N_getLanguage(void);

// Reads the system language (Profile/language.conf, eg. "es"), if it's set.
void I18N_init(void);
// Writes it there, for every app that reads it. Returns 1 on success.
int I18N_save(const char* code);

// The languages compiled in, for a picker: 0..I18N_count()-1, the current one
// at I18N_index(). Names are in the language itself ("Español").
int I18N_count(void);
int I18N_index(void);
const char* I18N_code(int i);
const char* I18N_name(int i);

#endif
