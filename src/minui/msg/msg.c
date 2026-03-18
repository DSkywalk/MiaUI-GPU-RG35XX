/****************************************************************************
 *  MiaUI
 *
 *  Copyright David Colmenero - D_Skywalk (2024-2026)
 *
 *  Redistribution and use of this code or any derivative works are permitted
 *  provided that the following conditions are met:
 *
 *   - Redistributions may not be sold, nor may they be used in a commercial
 *     product or activity.
 *
 *   - Redistributions that are modified from the original source must include the
 *     complete source code, including the source code for all components used by a
 *     binary built from the modified sources. However, as a special exception, the
 *     source code distributed need not include anything that is normally distributed
 *     (in either source or binary form) with the major components (compiler, kernel,
 *     and so on) of the operating system on which the executable runs, unless that
 *     component itself accompanies the executable.
 *
 *   - Redistributions must reproduce the above copyright notice, this list of
 *     conditions and the following disclaimer in the documentation and/or other
 *     materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************************/

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <msettings.h>
#include <stdlib.h>
#include <string.h>

#include "defines.h"
#include "api.h"
#include "utils.h"

void render_text_centered(SDL_Surface *screen, TTF_Font *font, const char *text, SDL_Color color, int y) {
    if (!text || strlen(text) == 0) return;
    
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (surface) {
        SDL_Rect dest = { (screen->w - surface->w) / 2, y, surface->w, surface->h };
        SDL_BlitSurface(surface, NULL, screen, &dest);
        SDL_FreeSurface(surface);
    }
}

void replace_escaped_newlines(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (src[0] == '\\' && src[1] == 'n') {
            *dst++ = '\n';
            src += 2;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

int render_multiline_text(SDL_Surface *screen, TTF_Font *font, char *text, SDL_Color color, int start_y, int line_spacing) {
    if (!text || strlen(text) == 0) return start_y;
    
    char *line = strtok(text, "\n");
    int y = start_y;
    
    while (line != NULL) {
        render_text_centered(screen, font, line, color, y);
        y += line_spacing; // next line
        line = strtok(NULL, "\n");
    }
    
    return y;
}

int main(int argc, char *argv[]) {
    char *title = "WARN";
    char *text = "";
    int delay = 5;
    int is_error = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--error") == 0) is_error = 1;
        else if (strcmp(argv[i], "--info") == 0) is_error = 0;
        else if (strncmp(argv[i], "title=", 6) == 0) title = argv[i] + 6;
        else if (strncmp(argv[i], "text=", 5) == 0) text = argv[i] + 5;
        else if (strncmp(argv[i], "delay=", 6) == 0) delay = atoi(argv[i] + 6);
    }

    replace_escaped_newlines(title);
    replace_escaped_newlines(text);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) return 1;
    if (TTF_Init() < 0) { GFX_quit(); return 1; }

    SDL_Surface* screen = GFX_init(MODE_MAIN);

    if (!screen) { TTF_Quit(); GFX_quit(); return 1; }

    TTF_Font *font_title = TTF_OpenFont(FONT_PATH, 32);
    TTF_Font *font_text = TTF_OpenFont(FONT_PATH, 22);

    if (!font_title || !font_text) {
        printf("Font Error: %s\n", TTF_GetError());
        GFX_quit();
        return 1;
    }

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0, 0, 0));

    SDL_Color title_color = is_error ? (SDL_Color){255, 60, 60, 255} : (SDL_Color){60, 200, 255, 255}; // Red or Blue
    SDL_Color text_color = {220, 220, 220, 255}; // Gris claro

    int current_y = 60;
    current_y = render_multiline_text(screen, font_title, title, title_color, current_y, 52);

    // 20px margin
    current_y += 20;

    render_multiline_text(screen, font_text, text, text_color, current_y, 38);

    GFX_flip(screen);

    int ticks = SDL_GetTicks();
    SDL_Event event;
    int running = 1;
    
    while (running && (SDL_GetTicks() - ticks < delay * 1000)) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_KEYDOWN || event.type == SDL_JOYBUTTONDOWN) {
                running = 0; 
            }
        }
        SDL_Delay(50);
    }

    TTF_CloseFont(font_title);
    TTF_CloseFont(font_text);
    TTF_Quit();
    GFX_quit();

    return 0;
}
