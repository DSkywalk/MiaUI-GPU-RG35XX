#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <ctype.h>
#include <sys/time.h>
#include "defines.h"
#include "mame-db.h"
#include "utils.h"

// -------------------------------------------------------------------------
// String Utilities
// -------------------------------------------------------------------------

/**
 * Checks if `str` starts with `pre`.
 * Case insensitive.
 */
int prefixMatch(char* pre, char* str) {
    return (strncasecmp(pre,str,strlen(pre))==0);
}

/**
 * Checks if `str` ends with `suf`.
 * Case insensitive.
 */
int suffixMatch(char* suf, char* str) {
    int len = strlen(suf);
    int offset = strlen(str)-len;
    return (offset>=0 && strncasecmp(suf, str+offset, len)==0);
}

/**
 * Checks if `str1` matches `str2` exactly (case sensitive).
 */
int exactMatch(char* str1, char* str2) {
    int len1 = strlen(str1);
    if (len1!=strlen(str2)) return 0;
    return (strncmp(str1,str2,len1)==0);
}

/**
 * Determines if a filename should be hidden from lists.
 * Hides dotfiles, .disabled files, and neogeo.zip bios.
 */
int hide(char* file_name) {
    return file_name[0]=='.' || suffixMatch(".disabled", file_name) || exactMatch("neogeo.zip", file_name);
}

/**
 * Cleans up a filename for display.
 * Removes extensions, parenthesis content, and handles specific MAME naming.
 */
void getDisplayName(const char* in_name, char* out_name) {
    char* tmp;
    char work_name[256];
    strcpy(work_name, in_name);
    strcpy(out_name, in_name);

    //printf("[utils] getDisplayName %s %s\n", in_name, work_name);
    
    // extract just the filename if necessary
    tmp = strrchr(work_name, '/');
    if (tmp) strcpy(out_name, tmp+1);

    //printf("[1] %s\n", )
    
    // remove extension(s), eg. .p8.png
    while ((tmp = strrchr(out_name, '.'))!=NULL) {
        int len = strlen(tmp);
        if (len>2 && len<=4) tmp[0] = '\0'; // 3 letter extension plus dot
        else break;
    }

    // --- INICIO DE LA LÓGICA DEL "DOT" (Eliminar números de pista) ---
    char *dot_ptr = strstr(out_name, ".");
    // Nos aseguramos de que haya un punto y que no sea el primer carácter (ej: archivos ocultos como ".juego")
    if (dot_ptr != NULL && dot_ptr != out_name) { 
        char *s = out_name;
        // Avanzamos mientras sean números y no lleguemos al punto
        while (isdigit((unsigned char)*s) && s < dot_ptr) {
            s++;
        }
        
        // Si s llegó exactamente al punto, significa que todo antes eran números
        if (s == dot_ptr) {
            dot_ptr++; // Saltamos el punto
            if (*dot_ptr == ' ') {
                dot_ptr++; // Saltamos el espacio si lo hay justo después
            }
            // Desplazamos el texto restante al principio de out_name para "borrar" el prefijo
            memmove(out_name, dot_ptr, strlen(dot_ptr) + 1);
        }
    }
    // --- FIN DE LA LÓGICA DEL "DOT" ---


    // remove trailing parens (round and square)
    strcpy(work_name, out_name);
    while ((tmp=strrchr(out_name, '('))!=NULL || (tmp=strrchr(out_name, '['))!=NULL) {
        if (tmp==out_name) break;
        tmp[0] = '\0';
        tmp = out_name;
    }
    
    // make sure we haven't nuked the entire name
    if (out_name[0]=='\0') strcpy(out_name, work_name);
    
    // remove trailing whitespace
    tmp = out_name + strlen(out_name) - 1;
    while(tmp>out_name && isspace((unsigned char)*tmp)) tmp--;
    tmp[1] = '\0';

    // if arcade list, look up real name in MAME DB
    for (int i = 0; mame_list[i].code != NULL; i++) {
        if (strcmp(mame_list[i].code, out_name) == 0) {
            strcpy(out_name, mame_list[i].name);
        }
    }
}

/**
 * Extracts the emulator/core name from a path.
 * Handles paths inside "Roms" folder and pak naming conventions.
 */
void getEmuName(const char* in_name, char* out_name) { // NOTE: both char arrays need to be MAX_PATH length!
    strcpy(out_name, in_name);
    
    // CHECK for PORTs (.pak)
    char* pak_ptr = strstr(out_name, ".pak");
    if (pak_ptr) {
        // Cut ".pak"
        // Example: "/userdata/Ports/OutRun.pak/..." into "/userdata/Ports/OutRun"
        *pak_ptr = '\0';
        
        // find last slash '/' 
        char* last_slash = strrchr(out_name, '/');
        if (last_slash) {
            // move "PortName" at the begining of out_name
            memmove(out_name, last_slash + 1, strlen(last_slash + 1) + 1);
        }
        return;
    }

    // check ROM (Console Name (EMUNAME))
    char* tmp = out_name;
    
    if (prefixMatch(ROMS_PATH, tmp)) {
        tmp += strlen(ROMS_PATH) + 1;
        char* tmp2 = strchr(tmp, '/');
        if (tmp2) *tmp2 = '\0';
    }

    char* paren = strrchr(tmp, '(');
    if (paren) {
        paren += 1;
        char* paren_end = strchr(paren, ')');
        if (paren_end) *paren_end = '\0';
        
        memmove(out_name, paren, strlen(paren) + 1);
    } else if (tmp != out_name) {
        // Fallback: if missing () but in ROMS, return the folder name
        memmove(out_name, tmp, strlen(tmp) + 1);
    }
}

/**
 * Constructs the full path to an emulator pak's launch script.
 * checks /userdata/Emus then /korki/Emus.
 */
void getEmuPath(char* emu_name, char* pak_path) {
    sprintf(pak_path, "%s/Emus/%s.pak/launch.sh", SDCARD_PATH, emu_name);
    if (exists(pak_path)) return;
    sprintf(pak_path, "%s/Emus/%s.pak/launch.sh", PAKS_PATH, emu_name);
}

void normalizeNewline(char* line) {
    int len = strlen(line);
    if (len>1 && line[len-1]=='\n' && line[len-2]=='\r') { // windows!
        line[len-2] = '\n';
        line[len-1] = '\0';
    }
}
void trimTrailingNewlines(char* line) {
    int len = strlen(line);
    while (len>0 && line[len-1]=='\n') {
        line[len-1] = '\0'; // trim newline
        len -= 1;
    }
}
void trimSortingMeta(char** str) { // eg. `001) `
    // TODO: this code is suss
    char* safe = *str;
    while(isdigit(**str)) *str += 1; // ignore leading numbers

    if (*str[0]==')') { // then match a closing parenthesis
        *str += 1;
    }
    else { //  or bail, restoring the string to its original value
        *str = safe;
        return;
    }
    
    while(isblank(**str)) *str += 1; // ignore leading space
}

// -------------------------------------------------------------------------
// File System & Time Utilities
// -------------------------------------------------------------------------

int exists(char* path) {
    return access(path, F_OK)==0;
}
void touch(char* path) {
    close(open(path, O_RDWR|O_CREAT, 0777));
}
void putFile(char* path, char* contents) {
    FILE* file = fopen(path, "w");
    if (file) {
        fputs(contents, file);
        fclose(file);
    }
}
void getFile(char* path, char* buffer, size_t buffer_size) {
    FILE *file = fopen(path, "r");
    if (file) {
        fseek(file, 0L, SEEK_END);
        size_t size = ftell(file);
        if (size>buffer_size-1) size = buffer_size - 1;
        rewind(file);
        fread(buffer, sizeof(char), size, file);
        fclose(file);
        buffer[size] = '\0';
    }
}
char* allocFile(char* path) { // caller must free!
    char* contents = NULL;
    FILE *file = fopen(path, "r");
    if (file) {
        fseek(file, 0L, SEEK_END);
        size_t size = ftell(file);
        contents = calloc(size+1, sizeof(char));
        fseek(file, 0L, SEEK_SET);
        fread(contents, sizeof(char), size, file);
        fclose(file);
        contents[size] = '\0';
    }
    return contents;
}
int getInt(char* path) {
    int i = 0;
    FILE *file = fopen(path, "r");
    if (file!=NULL) {
        fscanf(file, "%i", &i);
        fclose(file);
    }
    return i;
}
void putInt(char* path, int value) {
    char buffer[8];
    sprintf(buffer, "%d", value);
    putFile(path, buffer);
}

uint64_t getMicroseconds(void) {
    uint64_t ret;
    struct timeval tv;

    gettimeofday(&tv, NULL);

    ret = (uint64_t)tv.tv_sec * 1000000;
    ret += (uint64_t)tv.tv_usec;

    return ret;
}
