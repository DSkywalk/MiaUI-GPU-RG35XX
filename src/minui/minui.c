#include <stdio.h>
#include <stdlib.h>
#include <msettings.h>
#include <sys/types.h>
#include <dirent.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <math.h>

#include "defines.h"
#include "api.h"
#include "utils.h"
#include "i18n.h"
#include "nekobit.h"
#include "nekobit_ui.h"
#include "quickmenu.h"
#include "collections.h"


static SDL_Surface *screen = NULL; // Must be assigned externally

// Ported helpers
static int show_game_art = 1;
static int thumbnail_radius = 8;

///////////////////////////////////////
// Game art: folder backgrounds and thumbnails
//
// A worker thread decodes the PNGs into RGBA surfaces; the main thread turns
// them into textures (GL is main thread only) and keeps the most recent ones
// in art_cache, so going back to something already seen doesn't decode it
// again. When art is asked for is unchanged: only after the selection has
// rested for 1.5s (200ms at the root), so scrolling stays plain text.

static pthread_t thumb_thread;
static pthread_mutex_t thumb_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thumb_cond = PTHREAD_COND_INITIALIZER;

// main -> worker ("" = nothing of that kind to load)
static char request_thumb_path[MAX_PATH] = {0};
static char request_bg_path[MAX_PATH] = {0};
static int request_pending = 0;
static int thread_running = 1;

// worker -> main: each result comes with the path it was loaded from, NULL
// surface = no such image
static SDL_Surface *result_thumb = NULL;
static SDL_Surface *result_bg = NULL;
static char result_thumb_path[MAX_PATH] = {0};
static char result_bg_path[MAX_PATH] = {0};
static int result_thumb_ready = 0;
static int result_bg_ready = 0;

// Antialiased rounded corners: alpha scaled by how much of each corner
// pixel lies inside the circle (sampled at its centre, +-0.5px ramp).
static void GFX_applyRoundedCorners(SDL_Surface *surface, int radius)
{
    if (!surface || surface->format->BytesPerPixel != 4 || radius <= 0)
        return;
    if (radius * 2 > surface->w) radius = surface->w / 2;
    if (radius * 2 > surface->h) radius = surface->h / 2;

    uint32_t ashift = surface->format->Ashift;
    uint32_t amask = surface->format->Amask;
    for (int y = 0; y < radius; y++)
    {
        for (int x = 0; x < radius; x++)
        {
            float dx = radius - (x + 0.5f);
            float dy = radius - (y + 0.5f);
            float coverage = radius + 0.5f - sqrtf(dx * dx + dy * dy);
            if (coverage >= 1.0f)
                continue;
            if (coverage < 0.0f)
                coverage = 0.0f;

            // the same offset into each of the four corners
            int xs[2] = {x, surface->w - 1 - x};
            int ys[2] = {y, surface->h - 1 - y};
            for (int i = 0; i < 4; i++)
            {
                uint32_t *p = (uint32_t *)((uint8_t *)surface->pixels + ys[i >> 1] * surface->pitch) + xs[i & 1];
                uint32_t a = (uint32_t)(((*p & amask) >> ashift) * coverage);
                *p = (*p & ~amask) | (a << ashift);
            }
        }
    }
}

// The visible part of src when it is scaled to cover dst, cropping whichever
// way the aspect ratios disagree.
static SDL_Rect GFX_aspectFillRect(int src_w, int src_h, int dst_w, int dst_h)
{
    float image_aspect = (float)src_w / (float)src_h;
    float preview_aspect = (float)dst_w / (float)dst_h;
    SDL_Rect r;
    if (preview_aspect > image_aspect)
    {
        r.w = src_w;
        r.h = (int)(src_w / preview_aspect + 0.5f);
    }
    else
    {
        r.w = (int)(src_h * preview_aspect + 0.5f);
        r.h = src_h;
    }
    r.x = abs(r.w - src_w) / 2;
    r.y = abs(r.h - src_h) / 2;
    return r;
}

// Same-format 32-bit stretch, bilinear (much better than SDL_BlitScaled's
// nearest for downscaled box art), nearest if SDL won't do it.
static void GFX_stretch32(SDL_Surface *src, SDL_Rect *src_rect, SDL_Surface *dst)
{
    if (SDL_SoftStretchLinear(src, src_rect, dst, NULL) < 0)
    {
        SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
        SDL_BlitScaled(src, src_rect, dst, NULL);
    }
}

static SDL_Surface *loadBackground(const char *path)
{
    SDL_Surface *raw = IMG_Load(path);
    if (!raw)
        return NULL;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(raw);
    if (!rgba)
        return NULL;

    SDL_Surface *bg = rgba;
    if (rgba->w != FIXED_WIDTH || rgba->h != FIXED_HEIGHT)
    {
        bg = SDL_CreateRGBSurfaceWithFormat(0, FIXED_WIDTH, FIXED_HEIGHT, 32, SDL_PIXELFORMAT_ABGR8888);
        if (bg)
        {
            SDL_Rect crop = GFX_aspectFillRect(rgba->w, rgba->h, FIXED_WIDTH, FIXED_HEIGHT);
            GFX_stretch32(rgba, &crop, bg);
        }
        SDL_FreeSurface(rgba);
        if (!bg)
            return NULL;
    }

    // a background is opaque, as it was when it was converted to RGB565
    uint32_t amask = bg->format->Amask;
    for (int y = 0; y < bg->h; y++)
    {
        uint32_t *row = (uint32_t *)((uint8_t *)bg->pixels + y * bg->pitch);
        for (int x = 0; x < bg->w; x++)
            row[x] |= amask;
    }
    return bg;
}

static SDL_Surface *loadThumbnail(const char *path)
{
    SDL_Surface *raw = IMG_Load(path);
    if (!raw)
        return NULL;
    SDL_Surface *rgba = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(raw);
    if (!rgba)
        return NULL;

    double aspect_ratio = (double)rgba->h / rgba->w;
    int max_w = (int)(screen->w * 0.45);
    int max_h = (int)(screen->h * 0.6);
    int new_w = max_w;
    int new_h = (int)(new_w * aspect_ratio);
    if (new_h > max_h)
    {
        new_h = max_h;
        new_w = (int)(new_h / aspect_ratio);
    }

    SDL_Surface *thumb = NULL;
    if (new_w > 0 && new_h > 0)
        thumb = SDL_CreateRGBSurfaceWithFormat(0, new_w, new_h, 32, SDL_PIXELFORMAT_ABGR8888);
    if (thumb)
    {
        GFX_stretch32(rgba, NULL, thumb);
        GFX_applyRoundedCorners(thumb, SCALE1(thumbnail_radius));
    }
    SDL_FreeSurface(rgba);
    return thumb;
}

static void *ThumbnailLoaderThread(void *arg)
{
    char current_thumb_path[MAX_PATH];
    char current_bg_path[MAX_PATH];

    while (1)
    {
        pthread_mutex_lock(&thumb_mutex);
        while (!request_pending && thread_running)
            pthread_cond_wait(&thumb_cond, &thumb_mutex);
        if (!thread_running)
        {
            pthread_mutex_unlock(&thumb_mutex);
            break;
        }
        strcpy(current_thumb_path, request_thumb_path);
        strcpy(current_bg_path, request_bg_path);
        request_pending = 0;
        pthread_mutex_unlock(&thumb_mutex);

        // each finished image is handed over at once, even if a newer
        // request has come in meanwhile: the main thread caches it either way
        if (current_bg_path[0])
        {
            SDL_Surface *bg = loadBackground(current_bg_path);
            pthread_mutex_lock(&thumb_mutex);
            if (result_bg)
                SDL_FreeSurface(result_bg);
            result_bg = bg;
            strcpy(result_bg_path, current_bg_path);
            result_bg_ready = 1;
            int superseded = request_pending;
            pthread_mutex_unlock(&thumb_mutex);
            if (superseded)
                continue;
        }

        if (current_thumb_path[0])
        {
            SDL_Surface *thumb = loadThumbnail(current_thumb_path);
            pthread_mutex_lock(&thumb_mutex);
            if (result_thumb)
                SDL_FreeSurface(result_thumb);
            result_thumb = thumb;
            strcpy(result_thumb_path, current_thumb_path);
            result_thumb_ready = 1;
            pthread_mutex_unlock(&thumb_mutex);
        }
    }
    return NULL;
}

// Main thread side: textures, most recently used kept within ART_CACHE_BYTES.
// A path that has no image is remembered too (tex 0), so it isn't asked for
// again. Slots never move, so a shown entry can be pointed at; shown ones
// are never evicted.
#define ART_CACHE_SLOTS 32
#define ART_CACHE_BYTES (6 * 1024 * 1024) // a thumbnail is ~0.24MB, a background 1.2MB

typedef struct Art
{
    char path[MAX_PATH]; // "" = free slot
    GFX_Image image;     // tex 0 = there is no such image
    uint32_t used;
} Art;

static Art art_cache[ART_CACHE_SLOTS];
static size_t art_bytes = 0;
static uint32_t art_clock = 0;
static Art *shown_bg = NULL;
static Art *shown_thumb = NULL;

static Art *Art_find(const char *path)
{
    for (int i = 0; i < ART_CACHE_SLOTS; i++)
    {
        if (art_cache[i].path[0] && !strcmp(art_cache[i].path, path))
        {
            art_cache[i].used = ++art_clock;
            return &art_cache[i];
        }
    }
    return NULL;
}

static void Art_evict(Art *art)
{
    art_bytes -= (size_t)art->image.w * art->image.h * 4;
    GFX_freeImage(&art->image);
    memset(art, 0, sizeof(*art));
}

// takes ownership of surface (which may be NULL)
static Art *Art_add(const char *path, SDL_Surface *surface)
{
    Art *art = Art_find(path);
    if (art) // already there (asked for twice before the first arrived)
    {
        if (surface)
            SDL_FreeSurface(surface);
        return art;
    }

    size_t bytes = surface ? (size_t)surface->w * surface->h * 4 : 0;
    for (;;)
    {
        Art *lru = NULL;
        Art *free_slot = NULL;
        for (int i = 0; i < ART_CACHE_SLOTS; i++)
        {
            Art *a = &art_cache[i];
            if (!a->path[0])
            {
                if (!free_slot)
                    free_slot = a;
                continue;
            }
            if (a == shown_bg || a == shown_thumb)
                continue;
            if (!lru || a->used < lru->used)
                lru = a;
        }
        if (free_slot && art_bytes + bytes <= ART_CACHE_BYTES)
        {
            art = free_slot;
            break;
        }
        if (!lru) // everything left is on screen: go over budget rather than fail
        {
            art = free_slot;
            break;
        }
        Art_evict(lru);
    }
    if (!art)
    {
        if (surface)
            SDL_FreeSurface(surface);
        return NULL;
    }

    snprintf(art->path, sizeof(art->path), "%s", path);
    art->image = GFX_loadImage(surface);
    art->used = ++art_clock;
    art_bytes += (size_t)art->image.w * art->image.h * 4;
    if (surface)
        SDL_FreeSurface(surface);
    return art;
}

static void Art_quit(void)
{
    shown_bg = shown_thumb = NULL;
    for (int i = 0; i < ART_CACHE_SLOTS; i++)
    {
        if (art_cache[i].path[0])
            Art_evict(&art_cache[i]);
    }
}

static void Art_draw(Art *art, int x, int y)
{
    if (art)
        GFX_blitImage(&art->image, NULL, screen, &(SDL_Rect){x, y});
}

///////////////////////////////////////

typedef struct Array
{
    int count;
    int capacity;
    void **items;
} Array;

static Array *Array_new(void)
{
    Array *self = malloc(sizeof(Array));
    self->count = 0;
    self->capacity = 8;
    self->items = malloc(sizeof(void *) * self->capacity);
    return self;
}
static void Array_push(Array *self, void *item)
{
    if (self->count >= self->capacity)
    {
        self->capacity *= 2;
        self->items = realloc(self->items, sizeof(void *) * self->capacity);
    }
    self->items[self->count++] = item;
}
static void Array_unshift(Array *self, void *item)
{
    if (self->count == 0)
        return Array_push(self, item);
    Array_push(self, NULL); // ensures we have enough capacity
    for (int i = self->count - 2; i >= 0; i--)
    {
        self->items[i + 1] = self->items[i];
    }
    self->items[0] = item;
}
static void *Array_pop(Array *self)
{
    if (self->count == 0)
        return NULL;
    return self->items[--self->count];
}
static void Array_reverse(Array *self)
{
    int end = self->count - 1;
    int mid = self->count / 2;
    for (int i = 0; i < mid; i++)
    {
        void *item = self->items[i];
        self->items[i] = self->items[end - i];
        self->items[end - i] = item;
    }
}
static void Array_free(Array *self)
{
    free(self->items);
    free(self);
}

static int StringArray_indexOf(Array *self, char *str)
{
    for (int i = 0; i < self->count; i++)
    {
        if (exactMatch(self->items[i], str))
            return i;
    }
    return -1;
}
static void StringArray_free(Array *self)
{
    for (int i = 0; i < self->count; i++)
    {
        free(self->items[i]);
    }
    Array_free(self);
}

///////////////////////////////////////

typedef struct Hash
{
    Array *keys;
    Array *values;
} Hash; // not really a hash

static Hash *Hash_new(void)
{
    Hash *self = malloc(sizeof(Hash));
    self->keys = Array_new();
    self->values = Array_new();
    return self;
}
static void Hash_free(Hash *self)
{
    StringArray_free(self->keys);
    StringArray_free(self->values);
    free(self);
}
static void Hash_set(Hash *self, char *key, char *value)
{
    Array_push(self->keys, strdup(key));
    Array_push(self->values, strdup(value));
}
static char *Hash_get(Hash *self, char *key)
{
    int i = StringArray_indexOf(self->keys, key);
    if (i == -1)
        return NULL;
    return self->values->items[i];
}

///////////////////////////////////////

enum EntryType
{
    ENTRY_DIR,
    ENTRY_PAK,
    ENTRY_ROM,
};
// A string as GFX_truncateText last fitted it to a width, so a redraw
// doesn't measure it again (it measures once per character it cuts).
typedef struct Fit
{
    char *src;  // what was fitted (a copy: names can be rebuilt, see Directory_index)
    char *text; // the result
    int max;    // the width it was fitted to
    int width;  // GFX_truncateText's return
} Fit;

static void Fit_free(Fit *fit)
{
    free(fit->src);
    free(fit->text);
    fit->src = fit->text = NULL;
}

static const char *fitText(Fit *fit, const char *src, int max_width, int *width)
{
    if (!fit->text || fit->max != max_width || strcmp(fit->src, src))
    {
        char out[256];
        int w = GFX_truncateText(font.large, src, out, max_width, SCALE1(BUTTON_PADDING * 2));
        Fit_free(fit);
        fit->src = strdup(src);
        fit->text = strdup(out);
        fit->max = max_width;
        fit->width = w;
    }
    if (width)
        *width = fit->width;
    return fit->text;
}

typedef struct Entry
{
    char *path;
    char *name;
    char *unique;
    int type;
    int alpha; // index in parent Directory's alphas Array, which points to the index of an Entry in its entries Array :sweat_smile:
    int needs_swap;  // if need swap
    int is_darker; // user darker
    int mame_index; // mame romname index
    Fit fit_main;   // unique, or else name: the row as drawn when selected
    Fit fit_unique; // unique, sorting prefix trimmed (unselected rows)
    Fit fit_name;   // name, drawn over the unique one (unselected rows)
} Entry;

/**
 * Allocates and initializes a new Entry object.
 * Processes the path to extract a clean display name and checks if it's a MAME game.
 *
 * @param path The absolute or relative file path of the entry.
 * @param type The type of the entry (e.g., file, directory, or archive).
 * @return A pointer to the newly allocated Entry.
 */
static Entry *Entry_new(char *path, int type)
{
    char display_name[256];
    int mame_index = getDisplayName(path, display_name);
    
    Entry *self = calloc(1, sizeof(Entry));
    self->path = strdup(path);
    self->name = strdup(display_name);
    self->unique = NULL;
    self->type = type;
    self->alpha = 0;
    self->needs_swap = 0;
    self->is_darker = 0;
    
    // Store the index in the MAME database (-1 if it is not a MAME game)
    self->mame_index = mame_index; 
    
    return self;
}
static void Entry_free(Entry *self)
{
    free(self->path);
    free(self->name);
    if (self->unique)
        free(self->unique);
    Fit_free(&self->fit_main);
    Fit_free(&self->fit_unique);
    Fit_free(&self->fit_name);
    free(self);
}

static int EntryArray_indexOf(Array *self, char *path)
{
    for (int i = 0; i < self->count; i++)
    {
        Entry *entry = self->items[i];
        if (exactMatch(entry->path, path))
            return i;
    }
    return -1;
}
static int EntryArray_sortEntry(const void *a, const void *b)
{
    Entry *item1 = *(Entry **)a;
    Entry *item2 = *(Entry **)b;
    return strcasecmp(item1->name, item2->name);
}
static void EntryArray_sort(Array *self)
{
    qsort(self->items, self->count, sizeof(void *), EntryArray_sortEntry);
}

static void EntryArray_free(Array *self)
{
    for (int i = 0; i < self->count; i++)
    {
        Entry_free(self->items[i]);
    }
    Array_free(self);
}

///////////////////////////////////////

#define INT_ARRAY_MAX 27
typedef struct IntArray
{
    int count;
    int items[INT_ARRAY_MAX];
} IntArray;
static IntArray *IntArray_new(void)
{
    IntArray *self = malloc(sizeof(IntArray));
    self->count = 0;
    memset(self->items, 0, sizeof(int) * INT_ARRAY_MAX);
    return self;
}
static void IntArray_push(IntArray *self, int i)
{
    self->items[self->count++] = i;
}
static void IntArray_free(IntArray *self)
{
    free(self);
}

///////////////////////////////////////

typedef struct Directory
{
    char *path;
    char *name;
    Array *entries;
    IntArray *alphas;
    // rendering
    int selected;
    int start;
    int end;
} Directory;

static int getIndexChar(char *str)
{
    char i = 0;
    char c = tolower(str[0]);
    if (c >= 'a' && c <= 'z')
        i = (c - 'a') + 1;
    return i;
}

static void getUniqueName(Entry *entry, char *out_name)
{
    char *filename = strrchr(entry->path, '/') + 1;
    char emu_tag[256];
    getEmuName(entry->path, emu_tag);

    char *tmp;
    strcpy(out_name, entry->name);
    tmp = out_name + strlen(out_name);
    strcpy(tmp, " (");
    tmp = out_name + strlen(out_name);
    strcpy(tmp, emu_tag);
    tmp = out_name + strlen(out_name);
    strcpy(tmp, ")");
}

/**
 * Iterates through a directory's entries to assign alphabetical indices
 * and handle display name collisions (duplicate titles).
 * It will fall back to original MAME names or filenames to ensure uniqueness.
 *
 * @param self Pointer to the Directory object to be indexed.
 */
static void Directory_index(Directory *self)
{
    // Skip alphabetizing for specific virtual or collection paths
    int skip_index = exactMatch(FAUX_RECENT_PATH, self->path) || prefixMatch(COLLECTIONS_PATH, self->path); 

    Entry *prior = NULL;
    int alpha = -1;
    int index = 0;

    for (int i = 0; i < self->entries->count; i++)
    {
        Entry *entry = self->entries->items[i];

        // Handle naming collisions between the prior entry and current entry
        if (prior != NULL && exactMatch(prior->name, entry->name))
        {
            if (prior->unique)
                free(prior->unique);
            if (entry->unique)
                free(entry->unique);

            char *prior_filename = strrchr(prior->path, '/') + 1;
            char *entry_filename = strrchr(entry->path, '/') + 1;

            if (exactMatch(prior_filename, entry_filename))
            {
                // Files are identical, fallback to prepending/appending emulator tags
                char prior_unique[256];
                char entry_unique[256];
                getUniqueName(prior, prior_unique);
                getUniqueName(entry, entry_unique);

                prior->unique = strdup(prior_unique);
                entry->unique = strdup(entry_unique);
            }
            else
            {
                // Files are different (e.g., clones). 
                // Fast retrieval of the intact MAME name using the stored index.
                if (prior->mame_index != -1) {
                    prior->unique = strdup(getMameName(prior->mame_index));
                } else {
                    prior->unique = strdup(prior_filename);
                }

                if (entry->mame_index != -1) {
                    entry->unique = strdup(getMameName(entry->mame_index));
                } else {
                    entry->unique = strdup(entry_filename);
                }
            }
        }

        // Build the alphabetical fast-scroll index
        if (!skip_index)
        {
            int a = getIndexChar(entry->name);
            if (a != alpha)
            {
                index = self->alphas->count;
                IntArray_push(self->alphas, i);
                alpha = a;
            }
            entry->alpha = index;
        }

        prior = entry;
    }
}

static Array *getRoot(void);
static Array *getRecents(void);
static Array *getCollection(char *path);
static Array *getDiscs(char *path);
static Array *getEntries(char *path);

static Directory *Directory_new(char *path, int selected)
{
    char display_name[256];
    getDisplayName(path, display_name);

    Directory *self = malloc(sizeof(Directory));
    self->path = strdup(path);
    self->name = strdup(display_name);
    if (exactMatch(path, SDCARD_PATH))
    {
        self->entries = getRoot();
    }
    else if (exactMatch(path, FAUX_RECENT_PATH))
    {
        self->entries = getRecents();
    }
    else if (!exactMatch(path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, path) && suffixMatch(".txt", path))
    {
        self->entries = getCollection(path);
    }
    else if (suffixMatch(".m3u", path))
    {
        self->entries = getDiscs(path);
    }
    else
    {
        self->entries = getEntries(path);
    }
    self->alphas = IntArray_new();
    self->selected = selected;
    Directory_index(self);
    return self;
}
static void Directory_free(Directory *self)
{
    free(self->path);
    free(self->name);
    EntryArray_free(self->entries);
    IntArray_free(self->alphas);
    free(self);
}

static void DirectoryArray_pop(Array *self)
{
    Directory_free(Array_pop(self));
}
static void DirectoryArray_free(Array *self)
{
    for (int i = 0; i < self->count; i++)
    {
        Directory_free(self->items[i]);
    }
    Array_free(self);
}
///////////////////////////////////////

/**
 * Deletes the file associated with the given entry from the filesystem,
 * removes it from the directory's entry list, and re-indexes the directory.
 *
 * @param dir Pointer to the Directory object.
 * @param index The index of the entry to be deleted.
 */
static int deleteCurrentEntry(Directory *dir, int index)
{
    if (!dir || index < 0 || index >= dir->entries->count)
        return 0;

    Entry *entry = dir->entries->items[index];

    // 1. Delete the actual file from the filesystem
    if (remove(entry->path) != 0) {
        printf("Error deleting file: %s\n", entry->path);
        // If the filesystem prevents deletion, we abort removing it from the UI
        return 0;
    }

    // Prepare the UI message based on the result
    char msg[256];
    snprintf(msg, sizeof(msg), T(STR_DELETED), entry->name);

    // 2. Free the memory allocated for this specific entry
    Entry_free(entry);

    // 3. Shift the remaining entries in the array to fill the gap
    for (int i = index; i < dir->entries->count - 1; i++) {
        dir->entries->items[i] = dir->entries->items[i + 1];
    }
    dir->entries->count--;

    // 4. Clear and rebuild the alphabetical index to prevent crashes when using L1/R1
    dir->alphas->count = 0;
    Directory_index(dir);

    // The message on its own (the frame before it was the question)
    GFX_clear(screen);
    GFX_Text notif_txt = GFX_renderText(font.large, msg, COLOR_LIGHT_RED);

    // Use MinUI's exact scaling constants so the pill edges render without clipping
    int pill_width = notif_txt.w + SCALE1(BUTTON_PADDING * 2);
    int pill_height = SCALE1(PILL_SIZE);
    
    // Center the notification box on the screen
    SDL_Rect bg_rect = {
        (screen->w - pill_width) / 2,
        (screen->h - pill_height) / 2, 
        pill_width,
        pill_height
    };
    
    // Draw the background pill using proper dimensions
    GFX_blitPill(ASSET_DARK_GRAY_PILL, screen, &bg_rect);
    
    // Position the text accurately inside the pill
    GFX_blitRenderedText(&notif_txt, NULL, screen,
                         bg_rect.x + SCALE1(BUTTON_PADDING),
                         bg_rect.y + SCALE1(4)); // Standard MinUI vertical offset for pills
    
    
    // Force the hardware to draw this frame immediately, then block the thread
    GFX_flip(screen);
    SDL_Delay(2000); // 1000 milliseconds = 1 second delay

    sync();

    return 1;
}

///////////////////////////////////////

typedef struct Recent
{
    char *path; // NOTE: this is without the SDCARD_PATH prefix!
    int available;
} Recent;
static int hasEmu(char *emu_name);
static Recent *Recent_new(char *path)
{
    Recent *self = malloc(sizeof(Recent));

    char sd_path[256]; // only need to get emu name
    sprintf(sd_path, "%s%s", SDCARD_PATH, path);

    char emu_name[256];
    getEmuName(sd_path, emu_name);

    self->path = strdup(path);
    self->available = hasEmu(emu_name);
    return self;
}
static void Recent_free(Recent *self)
{
    free(self->path);
    free(self);
}

static int RecentArray_indexOf(Array *self, char *str)
{
    for (int i = 0; i < self->count; i++)
    {
        Recent *item = self->items[i];
        if (exactMatch(item->path, str))
            return i;
    }
    return -1;
}
static void RecentArray_free(Array *self)
{
    for (int i = 0; i < self->count; i++)
    {
        Recent_free(self->items[i]);
    }
    Array_free(self);
}

///////////////////////////////////////

static Directory *top;
static Array *stack;   // DirectoryArray
static Array *recents; // RecentArray

static int quit = 0;
static int can_resume = 0;
static int should_resume = 0; // set to 1 on BTN_RESUME but only if can_resume==1
static int simple_mode = 0;
static char slot_path[256];

static int restore_depth = -1;
static int restore_relative = -1;
static int restore_selected = 0;
static int restore_start = 0;
static int restore_end = 0;

///////////////////////////////////////

#define MAX_RECENTS 24 // a multiple of all menu rows
static void saveRecents(void)
{
    FILE *file = fopen(RECENT_PATH, "w");
    if (file)
    {
        for (int i = 0; i < recents->count; i++)
        {
            Recent *recent = recents->items[i];
            fputs(recent->path, file);
            putc('\n', file);
        }
        fclose(file);
    }
}
static void addRecent(char *path)
{
    path += strlen(SDCARD_PATH); // makes paths platform agnostic
    int id = RecentArray_indexOf(recents, path);
    if (id == -1)
    { // add
        while (recents->count >= MAX_RECENTS)
        {
            Recent_free(Array_pop(recents));
        }
        Array_unshift(recents, Recent_new(path));
    }
    else if (id > 0)
    { // bump to top
        for (int i = id; i > 0; i--)
        {
            void *tmp = recents->items[i - 1];
            recents->items[i - 1] = recents->items[i];
            recents->items[i] = tmp;
        }
    }
    saveRecents();
}

static int hasEmu(char *emu_name)
{
    char pak_path[256];
    sprintf(pak_path, "%s/Emus/%s.pak/launch.sh", PAKS_PATH, emu_name);
    if (exists(pak_path))
        return 1;

    sprintf(pak_path, "%s/Emus/%s.pak/launch.sh", SDCARD_PATH, emu_name);
    return exists(pak_path);
}
static int hasCue(char *dir_path, char *cue_path)
{                                           // NOTE: dir_path not rom_path
    char *tmp = strrchr(dir_path, '/') + 1; // folder name
    sprintf(cue_path, "%s/%s.cue", dir_path, tmp);
    return exists(cue_path);
}
static int hasM3u(char *rom_path, char *m3u_path)
{ // NOTE: rom_path not dir_path
    char *tmp;

    strcpy(m3u_path, rom_path);
    tmp = strrchr(m3u_path, '/') + 1;
    tmp[0] = '\0';

    // path to parent directory
    char base_path[256];
    strcpy(base_path, m3u_path);

    tmp = strrchr(m3u_path, '/');
    tmp[0] = '\0';

    // get parent directory name
    char dir_name[256];
    tmp = strrchr(m3u_path, '/');
    strcpy(dir_name, tmp);

    // dir_name is also our m3u file name
    tmp = m3u_path + strlen(m3u_path);
    strcpy(tmp, dir_name);

    // add extension
    tmp = m3u_path + strlen(m3u_path);
    strcpy(tmp, ".m3u");

    return exists(m3u_path);
}

static int hasRecents(void)
{
    int has = 0;

    Array *parent_paths = Array_new();
    if (exists(CHANGE_DISC_PATH))
    {
        char sd_path[256];
        getFile(CHANGE_DISC_PATH, sd_path, 256);
        if (exists(sd_path))
        {
            char *disc_path = sd_path + strlen(SDCARD_PATH); // makes path platform agnostic
            Recent *recent = Recent_new(disc_path);
            if (recent->available)
                has += 1;
            Array_push(recents, recent);

            char parent_path[256];
            strcpy(parent_path, disc_path);
            char *tmp = strrchr(parent_path, '/') + 1;
            tmp[0] = '\0';
            Array_push(parent_paths, strdup(parent_path));
        }
        unlink(CHANGE_DISC_PATH);
    }

    FILE *file = fopen(RECENT_PATH, "r"); // newest at top
    if (file)
    {
        char line[256];
        while (fgets(line, 256, file) != NULL)
        {
            normalizeNewline(line);
            trimTrailingNewlines(line);
            if (strlen(line) == 0)
                continue; // skip empty lines

            char sd_path[256];
            sprintf(sd_path, "%s%s", SDCARD_PATH, line);
            if (exists(sd_path))
            {
                if (recents->count < MAX_RECENTS)
                {
                    // this logic replaces an existing disc from a multi-disc game with the last used
                    char m3u_path[256];
                    if (hasM3u(sd_path, m3u_path))
                    { // TODO: this might tank launch speed
                        char parent_path[256];
                        strcpy(parent_path, line);
                        char *tmp = strrchr(parent_path, '/') + 1;
                        tmp[0] = '\0';

                        int found = 0;
                        for (int i = 0; i < parent_paths->count; i++)
                        {
                            char *path = parent_paths->items[i];
                            if (prefixMatch(path, parent_path))
                            {
                                found = 1;
                                break;
                            }
                        }
                        if (found)
                            continue;

                        Array_push(parent_paths, strdup(parent_path));
                    }
                    Recent *recent = Recent_new(line);
                    if (recent->available)
                        has += 1;
                    Array_push(recents, recent);
                }
            }
        }
        fclose(file);
    }

    saveRecents();

    StringArray_free(parent_paths);
    return has > 0;
}
static int hasCollections(void)
{
    int has = 0;
    if (!exists(COLLECTIONS_PATH))
        return has;

    DIR *dh = opendir(COLLECTIONS_PATH);
    struct dirent *dp;
    while ((dp = readdir(dh)) != NULL)
    {
        if (hide(dp->d_name))
            continue;
        has = 1;
        break;
    }
    closedir(dh);
    return has;
}
static int hasRoms(char *dir_name)
{
    int has = 0;
    char emu_name[256];
    char rom_path[256];

    getEmuName(dir_name, emu_name);

    // check for emu pak
    if (!hasEmu(emu_name))
        return has;

    // check for at least one non-hidden file (we're going to assume it's a rom)
    sprintf(rom_path, "%s/%s/", ROMS_PATH, dir_name);
    DIR *dh = opendir(rom_path);
    if (dh != NULL)
    {
        struct dirent *dp;
        while ((dp = readdir(dh)) != NULL)
        {
            if (hide(dp->d_name))
                continue;
            has = 1;
            break;
        }
        closedir(dh);
    }
    // if (!has) printf("No roms for %s!\n", dir_name);
    return has;
}
static Array *getRoot(void)
{
    Array *root = Array_new();

    if (hasRecents())
        Array_push(root, Entry_new(FAUX_RECENT_PATH, ENTRY_DIR));

    DIR *dh;

    Array *entries = Array_new();
    dh = opendir(ROMS_PATH);
    if (dh != NULL)
    {
        struct dirent *dp;
        char *tmp;
        char full_path[512];
        sprintf(full_path, "%s/", ROMS_PATH);
        tmp = full_path + strlen(full_path);
        Array *emus = Array_new();
        while ((dp = readdir(dh)) != NULL)
        {
            if (hide(dp->d_name))
                continue;
            if (hasRoms(dp->d_name))
            {
                strcpy(tmp, dp->d_name);
                Array_push(emus, Entry_new(full_path, ENTRY_DIR));
            }
        }
        EntryArray_sort(emus);
        Entry *prev_entry = NULL;
        for (int i = 0; i < emus->count; i++)
        {
            Entry *entry = emus->items[i];
            if (prev_entry != NULL)
            {
                if (exactMatch(prev_entry->name, entry->name))
                {
                    Entry_free(entry);
                    continue;
                }
            }
            Array_push(entries, entry);
            prev_entry = entry;
        }
        Array_free(emus); // just free the array part, entries now owns emus entries
        closedir(dh);
    }

    if (hasCollections())
    {
        if (entries->count)
            Array_push(root, Entry_new(COLLECTIONS_PATH, ENTRY_DIR));
        else
        { // no visible systems, promote collections to root
            dh = opendir(COLLECTIONS_PATH);
            if (dh != NULL)
            {
                struct dirent *dp;
                char *tmp;
                char full_path[256];
                sprintf(full_path, "%s/", COLLECTIONS_PATH);
                tmp = full_path + strlen(full_path);
                Array *collections = Array_new();
                while ((dp = readdir(dh)) != NULL)
                {
                    if (hide(dp->d_name))
                        continue;
                    strcpy(tmp, dp->d_name);
                    Array_push(collections, Entry_new(full_path, ENTRY_DIR)); // yes, collections are fake directories
                }
                EntryArray_sort(collections);
                for (int i = 0; i < collections->count; i++)
                {
                    Array_push(entries, collections->items[i]);
                }
                Array_free(collections); // just free the array part, entries now owns collections entries
                closedir(dh);
            }
        }
    }

    // add systems to root
    for (int i = 0; i < entries->count; i++)
    {
        Array_push(root, entries->items[i]);
    }
    Array_free(entries); // root now owns entries' entries

    char *tools_path = SDCARD_PATH "/Tools";
    if (exists(tools_path) && !simple_mode) // Kids Mode
        Array_push(root, Entry_new(tools_path, ENTRY_DIR));

    char *ports_path = SDCARD_PATH "/Ports";
    if (exists(ports_path)) // kids can play them: only Tools hides
        Array_push(root, Entry_new(ports_path, ENTRY_DIR));

    return root;
}
static Array *getRecents(void)
{
    Array *entries = Array_new();
    for (int i = 0; i < recents->count; i++)
    {
        Recent *recent = recents->items[i];
        if (!recent->available)
            continue;

        char sd_path[256];
        sprintf(sd_path, "%s%s", SDCARD_PATH, recent->path);
        int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM; // ???
        Array_push(entries, Entry_new(sd_path, type));
    }
    return entries;
}
static Array *getCollection(char *path)
{
    Array *entries = Array_new();
    FILE *file = fopen(path, "r");
    if (file)
    {
        char line[256];
        while (fgets(line, 256, file) != NULL)
        {
            normalizeNewline(line);
            trimTrailingNewlines(line);
            if (strlen(line) == 0)
                continue; // skip empty lines

            char sd_path[256];
            sprintf(sd_path, "%s%s", SDCARD_PATH, line);
            if (exists(sd_path))
            {
                int type = suffixMatch(".pak", sd_path) ? ENTRY_PAK : ENTRY_ROM; // ???
                Array_push(entries, Entry_new(sd_path, type));

                // char emu_name[256];
                // getEmuName(sd_path, emu_name);
                // if (hasEmu(emu_name)) {
                // Array_push(entries, Entry_new(sd_path, ENTRY_ROM));
                // }
            }
        }
        fclose(file);
    }
    return entries;
}
static Array *getDiscs(char *path)
{

    // TODO: does path have SDCARD_PATH prefix?

    Array *entries = Array_new();

    char base_path[256];
    strcpy(base_path, path);
    char *tmp = strrchr(base_path, '/') + 1;
    tmp[0] = '\0';

    // TODO: limit number of discs supported (to 9?)
    FILE *file = fopen(path, "r");
    if (file)
    {
        char line[256];
        int disc = 0;
        while (fgets(line, 256, file) != NULL)
        {
            normalizeNewline(line);
            trimTrailingNewlines(line);
            if (strlen(line) == 0)
                continue; // skip empty lines

            char disc_path[256];
            sprintf(disc_path, "%s%s", base_path, line);

            if (exists(disc_path))
            {
                disc += 1;
                Entry *entry = Entry_new(disc_path, ENTRY_ROM);
                free(entry->name);
                char name[16];
                sprintf(name, "Disc %i", disc);
                entry->name = strdup(name);
                Array_push(entries, entry);
            }
        }
        fclose(file);
    }
    return entries;
}
static int getFirstDisc(char *m3u_path, char *disc_path)
{ // based on getDiscs() natch
    int found = 0;

    char base_path[256];
    strcpy(base_path, m3u_path);
    char *tmp = strrchr(base_path, '/') + 1;
    tmp[0] = '\0';

    FILE *file = fopen(m3u_path, "r");
    if (file)
    {
        char line[256];
        while (fgets(line, 256, file) != NULL)
        {
            normalizeNewline(line);
            trimTrailingNewlines(line);
            if (strlen(line) == 0)
                continue; // skip empty lines

            sprintf(disc_path, "%s%s", base_path, line);

            if (exists(disc_path))
                found = 1;
            break;
        }
        fclose(file);
    }
    return found;
}

static void addEntries(Array *entries, char *path)
{
    DIR *dh = opendir(path);
    if (dh != NULL)
    {
        struct dirent *dp;
        char *tmp;
        char full_path[256];
        sprintf(full_path, "%s/", path);
        tmp = full_path + strlen(full_path);
        while ((dp = readdir(dh)) != NULL)
        {
            if (hide(dp->d_name))
                continue;
            strcpy(tmp, dp->d_name);
            int is_dir = dp->d_type == DT_DIR;
            int type;
            if (is_dir)
            {
                // TODO: this should make sure launch.sh exists
                if (suffixMatch(".pak", dp->d_name))
                {
                    type = ENTRY_PAK;
                }
                else
                {
                    type = ENTRY_DIR;
                }
            }
            else
            {
                if (prefixMatch(COLLECTIONS_PATH, full_path))
                {
                    type = ENTRY_DIR; // :shrug:
                }
                else
                {
                    type = ENTRY_ROM;
                }
            }

            Entry *new_entry = Entry_new(full_path, type);
            
            // Buscamos el tag [SWP] en el nombre original del archivo/carpeta
            if (
                (strstr(dp->d_name, "[SWP]") != NULL)||
                (strstr(dp->d_name, "(SWP)") != NULL)||
                (strstr(dp->d_name, "(SWAP)") != NULL)||
                (strstr(dp->d_name, "[SWAP]") != NULL)
            ) {
                new_entry->needs_swap = 1;
            } else {
                new_entry->needs_swap = 0;
            }

            if (
                (strstr(dp->d_name, "[DRK]") != NULL)||
                (strstr(dp->d_name, "(DRK)") != NULL)||
                (strstr(dp->d_name, "(DARK)") != NULL)||
                (strstr(dp->d_name, "[DARK]") != NULL)
            ) {
                new_entry->is_darker = 1;
            } else {
                new_entry->is_darker = 0;
            }

            Array_push(entries, new_entry);
        }
        closedir(dh);
    }
}

static int isSubDir(Entry *entry)
{
    if ((entry->type == ENTRY_DIR) && stack->count > 1 && !prefixMatch(COLLECTIONS_PATH, entry->path))
        return 1;
    else
        return 0;
}

static int isConsoleDir(char *path)
{
    char *tmp;
    char parent_dir[256];
    strcpy(parent_dir, path);
    tmp = strrchr(parent_dir, '/');
    tmp[0] = '\0';

    return exactMatch(parent_dir, ROMS_PATH);
}

static Array *getEntries(char *path)
{
    Array *entries = Array_new();

    if (isConsoleDir(path))
    { // top-level console folder, might collate
        char collated_path[256];
        strcpy(collated_path, path);
        char *tmp = strrchr(collated_path, '(');
        // 1 because we want to keep the opening parenthesis to avoid collating "Game Boy Color" and "Game Boy Advance" into "Game Boy"
        // but conditional so we can continue to support a bare tag name as a folder name
        if (tmp)
            tmp[1] = '\0';

        DIR *dh = opendir(ROMS_PATH);
        if (dh != NULL)
        {
            struct dirent *dp;
            char full_path[256];
            sprintf(full_path, "%s/", ROMS_PATH);
            tmp = full_path + strlen(full_path);
            // while loop so we can collate paths, see above
            while ((dp = readdir(dh)) != NULL)
            {
                if (hide(dp->d_name))
                    continue;
                if (dp->d_type != DT_DIR)
                    continue;
                strcpy(tmp, dp->d_name);

                if (!prefixMatch(collated_path, full_path))
                    continue;
                addEntries(entries, full_path);
            }
            closedir(dh);
        }
    }
    else
        addEntries(entries, path); // just a subfolder

    EntryArray_sort(entries);
    return entries;
}

///////////////////////////////////////

static void queueNext(char *cmd)
{
    LOG_info("cmd: %s\n", cmd);
    putFile("/tmp/next", cmd);
    quit = 1;
}
static char *escapeSingleQuotes(char *str)
{
    // based on https://stackoverflow.com/a/31775567/145965
    int replaceString(char *line, const char *search, const char *replace)
    {
        char *sp; // start of pattern
        if ((sp = strstr(line, search)) == NULL)
        {
            return 0;
        }
        int count = 1;
        int sLen = strlen(search);
        int rLen = strlen(replace);
        if (sLen > rLen)
        {
            // move from right to left
            char *src = sp + sLen;
            char *dst = sp + rLen;
            while ((*dst = *src) != '\0')
            {
                dst++;
                src++;
            }
        }
        else if (sLen < rLen)
        {
            // move from left to right
            int tLen = strlen(sp) - sLen;
            char *stop = sp + rLen;
            char *src = sp + sLen + tLen;
            char *dst = sp + rLen + tLen;
            while (dst >= stop)
            {
                *dst = *src;
                dst--;
                src--;
            }
        }
        memcpy(sp, replace, rLen);
        count += replaceString(sp + rLen, search, replace);
        return count;
    }
    replaceString(str, "'", "'\\''");
    return str;
}

///////////////////////////////////////

static void readyResumePath(char *rom_path, int type)
{
    char *tmp;
    can_resume = 0;
    char path[256];
    strcpy(path, rom_path);

    if (!prefixMatch(ROMS_PATH, path))
        return;

    char auto_path[256];
    if (type == ENTRY_DIR)
    {
        if (!hasCue(path, auto_path))
        {                                      // no cue?
            tmp = strrchr(auto_path, '.') + 1; // extension
            strcpy(tmp, "m3u");                // replace with m3u
            if (!exists(auto_path))
                return; // no m3u
        }
        strcpy(path, auto_path); // cue or m3u if one exists
    }

    if (!suffixMatch(".m3u", path))
    {
        char m3u_path[256];
        if (hasM3u(path, m3u_path))
        {
            // change path to m3u path
            strcpy(path, m3u_path);
        }
    }

    char emu_name[256];
    getEmuName(path, emu_name);

    char rom_file[256];
    tmp = strrchr(path, '/') + 1;
    strcpy(rom_file, tmp);

    sprintf(slot_path, "%s/.minui/%s/%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file); // /.userdata/arm-480/.minui/<EMU>/<romname>.ext.txt

    can_resume = exists(slot_path);
}
static void readyResume(Entry *entry)
{
    readyResumePath(entry->path, entry->type);
}

static void saveLast(char *path);
static void loadLast(void);

static int autoResume(void)
{
    // NOTE: bypasses recents

    if (!exists(AUTO_RESUME_PATH))
        return 0;

    char path[256];
    getFile(AUTO_RESUME_PATH, path, 256);
    unlink(AUTO_RESUME_PATH);
    sync();

    // make sure rom still exists
    char sd_path[256];
    sprintf(sd_path, "%s%s", SDCARD_PATH, path);
    if (!exists(sd_path))
        return 0;

    // make sure emu still exists
    char emu_name[256];
    getEmuName(sd_path, emu_name);

    char emu_path[256];
    getEmuPath(emu_name, emu_path);

    if (!exists(emu_path))
        return 0;

    // putFile(LAST_PATH, FAUX_RECENT_PATH); // saveLast() will crash here because top is NULL

    NB_beginSession(sd_path, sd_path, emu_name, 0); // before the quotes get escaped

    char cmd[256];
    sprintf(cmd, "sh '%s' '%s'", escapeSingleQuotes(emu_path), escapeSingleQuotes(sd_path));
    putInt(RESUME_SLOT_PATH, AUTO_RESUME_SLOT);
    queueNext(cmd);
    return 1;
}

static void openPak(char *path)
{
    // NOTE: escapeSingleQuotes() modifies the passed string
    // so we need to save the path before we call that
    // if (prefixMatch(ROMS_PATH, path)) {
    //     addRecent(path);
    // }
    saveLast(path);

    if (prefixMatch(SDCARD_PATH "/Ports/", path))
    {
        char tag[MAX_PATH];
        getEmuName(path, tag);
        NB_beginSession(path, path, tag, 0);
    }

    char cmd[256];
    sprintf(cmd, "sh '%s/launch.sh'", escapeSingleQuotes(path));
    queueNext(cmd);
}
// Where a NekoBit mission took the player: the system folder it opened and
// the game it asked for. Launching that game is playing the mission: back
// from it, NekoBit opens by itself to show what was earned. Forgotten once
// the player is back on the root.
static char nb_mission_dir[MAX_PATH];
static NB_Target nb_mission_target;

// path is base, or inside it (a mission names a multi-disc game's folder)
static int NekoBit_pathUnder(const char *base, const char *path)
{
    int len = strlen(base);
    return !strncmp(base, path, len) && (path[len] == '\0' || path[len] == '/');
}

// The mission's own game (any of its discs), or for a mission without one (a
// system, an RA set never played) anything in the system it opened. Browsing
// off and launching something else isn't the mission any more.
static int NekoBit_sessionFlags(char *rom, char *game)
{
    if (!nb_mission_dir[0] || stack->count < 2)
        return 0;
    const char *want = nb_mission_target.select;
    int hit;
    if (want[0])
    {
        hit = NekoBit_pathUnder(want, rom) || NekoBit_pathUnder(want, game);
        const char *slash = strrchr(game, '/');
        if (!hit && slash && suffixMatch(".m3u", game)) // the mission names one of its discs
            hit = !strncmp(want, game, slash - game) && want[slash - game] == '/';
    }
    else
    {
        Directory *system = stack->items[1];
        hit = exactMatch(system->path, nb_mission_dir);
    }
    return hit ? NB_SESSION_VIA_NEKOBIT : 0;
}

static void openRom(char *path, char *last)
{
    LOG_info("openRom(%s,%s)\n", path, last);

    char sd_path[256];
    strcpy(sd_path, path);

    char m3u_path[256];
    int has_m3u = hasM3u(sd_path, m3u_path);

    char recent_path[256];
    strcpy(recent_path, has_m3u ? m3u_path : sd_path);

    if (has_m3u && suffixMatch(".m3u", sd_path))
    {
        getFirstDisc(m3u_path, sd_path);
    }

    char emu_name[256];
    getEmuName(sd_path, emu_name);

    if (should_resume)
    {
        char slot[16];
        getFile(slot_path, slot, 16);
        putFile(RESUME_SLOT_PATH, slot);
        should_resume = 0;

        if (has_m3u)
        {
            char rom_file[256];
            strcpy(rom_file, strrchr(m3u_path, '/') + 1);

            // get disc for state
            char disc_path_path[256];
            sprintf(disc_path_path, "%s/.minui/%s/%s.%s.txt", SHARED_USERDATA_PATH, emu_name, rom_file, slot); // /.userdata/arm-480/.minui/<EMU>/<romname>.ext.0.txt

            if (exists(disc_path_path))
            {
                // switch to disc path
                char disc_path[256];
                getFile(disc_path_path, disc_path, 256);
                if (disc_path[0] == '/')
                    strcpy(sd_path, disc_path); // absolute
                else
                { // relative
                    strcpy(sd_path, m3u_path);
                    char *tmp = strrchr(sd_path, '/') + 1;
                    strcpy(tmp, disc_path);
                }
            }
        }
    }
    else
        putInt(RESUME_SLOT_PATH, 8); // resume hidden default state

    char emu_path[256];
    getEmuPath(emu_name, emu_path);

    NB_beginSession(sd_path, recent_path, emu_name, NekoBit_sessionFlags(sd_path, recent_path));

    // NOTE: escapeSingleQuotes() modifies the passed string
    // so we need to save the path before we call that
    addRecent(recent_path);
    saveLast(last == NULL ? sd_path : last);

    char cmd[256];
    sprintf(cmd, "sh '%s' '%s'", escapeSingleQuotes(emu_path), escapeSingleQuotes(sd_path));
    queueNext(cmd);
}
static void openDirectory(char *path, int auto_launch)
{
    char auto_path[256];
    if (hasCue(path, auto_path) && auto_launch)
    {
        openRom(auto_path, path);
        return;
    }

    char m3u_path[256];
    strcpy(m3u_path, auto_path);
    char *tmp = strrchr(m3u_path, '.') + 1; // extension
    strcpy(tmp, "m3u");                     // replace with m3u
    if (exists(m3u_path) && auto_launch)
    {
        auto_path[0] = '\0';
        if (getFirstDisc(m3u_path, auto_path))
        {
            openRom(auto_path, path);
            return;
        }
        // TODO: doesn't handle empty m3u files
    }

    int selected = 0;
    int start = selected;
    int end = 0;
    if (top && top->entries->count > 0)
    {
        if (restore_depth == stack->count && top->selected == restore_relative)
        {
            selected = restore_selected;
            start = restore_start;
            end = restore_end;
        }
    }

    top = Directory_new(path, selected);
    top->start = start;
    top->end = end ? end : ((top->entries->count < MAIN_ROW_COUNT) ? top->entries->count : MAIN_ROW_COUNT);

    Array_push(stack, top);
}
static void closeDirectory(void)
{
    restore_selected = top->selected;
    restore_start = top->start;
    restore_end = top->end;
    DirectoryArray_pop(stack);
    restore_depth = stack->count;
    top = stack->items[stack->count - 1];
    restore_relative = top->selected;
}

static void Entry_open(Entry *self)
{
    if (self->type == ENTRY_ROM)
    {
        char *last = NULL;
        if (prefixMatch(COLLECTIONS_PATH, top->path))
        {
            char *tmp;
            char filename[256];

            tmp = strrchr(self->path, '/');
            if (tmp)
                strcpy(filename, tmp + 1);

            char last_path[256];
            sprintf(last_path, "%s/%s", top->path, filename);
            last = last_path;
        }
        openRom(self->path, last);
    }
    else if (self->type == ENTRY_PAK)
    {
        openPak(self->path);
    }
    else if (self->type == ENTRY_DIR)
    {
        openDirectory(self->path, 1);
    }
}

///////////////////////////////////////

static void saveLast(char *path)
{
    // special case for recently played
    if (exactMatch(top->path, FAUX_RECENT_PATH))
    {
        // NOTE: that we don't have to save the file because
        // your most recently played game will always be at
        // the top which is also the default selection
        path = FAUX_RECENT_PATH;
    }
    putFile(LAST_PATH, path);
}
static void loadLast(void)
{ // call after loading root directory
    if (!exists(LAST_PATH))
        return;

    char last_path[256];
    getFile(LAST_PATH, last_path, 256);
    if (!prefixMatch(SDCARD_PATH "/", last_path))
        return; // not ours (a test build's, a stale one): the walk below would never reach SDCARD_PATH

    char full_path[256];
    strcpy(full_path, last_path);

    char *tmp;
    char filename[256];
    tmp = strrchr(last_path, '/');
    if (tmp)
        strcpy(filename, tmp);

    Array *last = Array_new();
    while (!exactMatch(last_path, SDCARD_PATH))
    {
        Array_push(last, strdup(last_path));

        char *slash = strrchr(last_path, '/');
        last_path[(slash - last_path)] = '\0';
    }

    while (last->count > 0)
    {
        char *path = Array_pop(last);
        if (!exactMatch(path, ROMS_PATH))
        { // romsDir is effectively root as far as restoring state after a game
            char collated_path[256];
            collated_path[0] = '\0';
            if (suffixMatch(")", path) && isConsoleDir(path))
            {
                strcpy(collated_path, path);
                tmp = strrchr(collated_path, '(');
                if (tmp)
                    tmp[1] = '\0'; // 1 because we want to keep the opening parenthesis to avoid collating "Game Boy Color" and "Game Boy Advance" into "Game Boy"
            }

            for (int i = 0; i < top->entries->count; i++)
            {
                Entry *entry = top->entries->items[i];

                // NOTE: strlen() is required for collated_path, '\0' wasn't reading as NULL for some reason
                if (exactMatch(entry->path, path) || (strlen(collated_path) && prefixMatch(collated_path, entry->path)) || (prefixMatch(COLLECTIONS_PATH, full_path) && suffixMatch(filename, entry->path)))
                {
                    top->selected = i;
                    if (i >= top->end)
                    {
                        top->start = i;
                        top->end = top->start + MAIN_ROW_COUNT;
                        if (top->end > top->entries->count)
                        {
                            top->end = top->entries->count;
                            top->start = top->end - MAIN_ROW_COUNT;
                        }
                    }
                    if (last->count == 0 && !exactMatch(entry->path, FAUX_RECENT_PATH) && !(!exactMatch(entry->path, COLLECTIONS_PATH) && prefixMatch(COLLECTIONS_PATH, entry->path)))
                        break; // don't show contents of auto-launch dirs

                    if (entry->type == ENTRY_DIR)
                    {
                        openDirectory(entry->path, 0);
                        break;
                    }
                }
            }
        }
        free(path); // we took ownership when we popped it
    }

    StringArray_free(last);
}

///////////////////////////////////////

static void Menu_init(void)
{
    stack = Array_new(); // array of open Directories
    recents = Array_new();

    openDirectory(SDCARD_PATH, 0);
    loadLast(); // restore state when available
}
static void Menu_quit(void)
{
    RecentArray_free(recents);
    DirectoryArray_free(stack);
}

///////////////////////////////////////
// NekoBit: what it picks missions from, and taking the player to one

#define NB_MAX_SYSTEMS 96
static NB_System nb_systems[NB_MAX_SYSTEMS];
static NB_Library nb_library;

static void NekoBit_open(void)
{
    Directory *root = stack->items[0];
    nb_library.systems = nb_systems;
    nb_library.system_count = 0;
    for (int i = 0; i < root->entries->count && nb_library.system_count < NB_MAX_SYSTEMS; i++)
    {
        Entry *entry = root->entries->items[i];
        if (entry->type == ENTRY_DIR && prefixMatch(ROMS_PATH "/", entry->path))
            nb_systems[nb_library.system_count++] = (NB_System){entry->path, entry->name};
    }
    NBUI_open(&nb_library);
}

static void Directory_scrollTo(Directory *self, int i)
{
    int count = self->entries->count;
    self->selected = i;
    if (i < self->start || i >= self->end)
    {
        self->start = i;
        self->end = self->start + MAIN_ROW_COUNT;
        if (self->end > count)
        {
            self->end = count;
            self->start = self->end - MAIN_ROW_COUNT;
            if (self->start < 0)
                self->start = 0;
        }
    }
}

// Opens the system folder the way the player would from the root (a folder
// collated with its "Name (" siblings counts as the same system) and selects
// the mission's game in it.
static void NekoBit_navigate(NB_Target *target)
{
    while (stack->count > 1)
        closeDirectory();

    int found = -1;
    for (int i = 0; i < top->entries->count && found < 0; i++)
    {
        Entry *entry = top->entries->items[i];
        if (entry->type != ENTRY_DIR || !prefixMatch(ROMS_PATH "/", entry->path))
            continue;
        if (target->system[0])
        {
            char collated[256];
            strcpy(collated, entry->path);
            char *paren = strrchr(collated, '(');
            if (paren && paren > strrchr(collated, '/'))
                paren[1] = '\0';
            if (exactMatch(entry->path, target->system) || (paren && prefixMatch(collated, target->system)))
                found = i;
        }
        else if (target->tag[0])
        {
            char tag[MAX_PATH];
            getEmuName(entry->path, tag);
            if (exactMatch(tag, target->tag))
                found = i;
        }
    }
    if (found < 0)
        return;

    Directory_scrollTo(top, found);
    restore_depth = -1; // a fresh folder, not the one closed last
    openDirectory(((Entry *)top->entries->items[found])->path, 0);
    strcpy(nb_mission_dir, top->path);
    nb_mission_target = *target;

    // down the folders to the game, however deep: each level selects what
    // holds it, and opens it unless it's the game (a multi-disc folder)
    for (int depth = 0; target->select[0] && depth < 16; depth++)
    {
        Entry *hit = NULL;
        for (int i = 0; i < top->entries->count && !hit; i++)
        {
            Entry *entry = top->entries->items[i];
            if (NekoBit_pathUnder(entry->path, target->select))
            {
                Directory_scrollTo(top, i);
                hit = entry;
            }
        }
        if (!hit || hit->type != ENTRY_DIR || exactMatch(hit->path, target->select))
            break;
        char m3u[MAX_PATH];
        snprintf(m3u, sizeof(m3u), "%s%s.m3u", hit->path, strrchr(hit->path, '/'));
        if (exists(m3u))
            break;
        restore_depth = -1;
        openDirectory(hit->path, 0);
    }
}

///////////////////////////////////////
// Quick Menu

#define SDTOOL_PAK_PATH SDCARD_PATH "/Tools/1) System [DRK]/SD Tool.pak"
#define QM_REOPEN_PATH "/tmp/qm_reopen" // back from the SD tool: straight to the Quick Menu

// where the list is, for loadLast() to come back to
static void saveCurrent(void)
{
    if (top->entries->count > 0)
        saveLast(((Entry *)top->entries->items[top->selected])->path);
    else
        saveLast(top->path);
}

// Kids Mode changed what the root shows: rebuild it, back where we were
static void reloadMenu(void)
{
    saveCurrent();
    Menu_quit();
    Menu_init();
}

///////////////////////////////////////
// Collections, and deleting a game

// the game behind an entry, as recents and collections hold it: a rom or a
// pak as it is, a multi-disc folder by the .cue or .m3u openRom() plays
static int gamePath(Entry *entry, char *out)
{
    if (entry->type == ENTRY_ROM || entry->type == ENTRY_PAK)
    {
        strcpy(out, entry->path);
        return 1;
    }
    char path[MAX_PATH];
    if (hasCue(entry->path, path))
    {
        strcpy(out, path);
        return 1;
    }
    strcpy(strrchr(path, '.') + 1, "m3u");
    if (!exists(path))
        return 0;
    strcpy(out, path);
    return 1;
}

// SELECT + Y: the selected game's collections
static void openCollections(void)
{
    if (top->entries->count == 0)
        return;
    Entry *entry = top->entries->items[top->selected];
    char path[MAX_PATH];
    if (!gamePath(entry, path))
        return;
    char *name = entry->name;
    trimSortingMeta(&name);
    CM_open(path, name);
}

// what the left pill offers while buttons are held on a game in a folder:
// SELECT, Y for its collections; SELECT + START, X to delete it (not in Kids
// Mode). On the root SELECT is NekoBit's.
enum { HOLD_NONE, HOLD_FAV, HOLD_DELETE };
static int heldAction(int show_setting)
{
    if (stack->count < 2 || top->entries->count == 0 || show_setting || !PAD_isPressed(BTN_SELECT))
        return HOLD_NONE;
    Entry *entry = top->entries->items[top->selected];
    if (PAD_isPressed(BTN_START))
        return !simple_mode && entry->type == ENTRY_ROM ? HOLD_DELETE : HOLD_NONE;
    char path[MAX_PATH];
    return gamePath(entry, path) ? HOLD_FAV : HOLD_NONE;
}

// a list whose entries changed: entry i selected (or the last), and the rows
// shown kept where they were as far as they still fit
static void Directory_fit(Directory *self, int i)
{
    int count = self->entries->count;
    if (count == 0)
    {
        self->selected = self->start = self->end = 0;
        return;
    }
    if (i >= count)
        i = count - 1;
    int rows = count < MAIN_ROW_COUNT ? count : MAIN_ROW_COUNT;
    if (i < self->start)
        self->start = i;
    if (i >= self->start + rows)
        self->start = i - rows + 1;
    if (self->start + rows > count)
        self->start = count - rows;
    self->end = self->start + rows;
    self->selected = i;
}

// After the Collections screen changed a list: the root (a first list brings
// Collections), the Collections folder and an open list are read again, each
// on the entry it was on, or where it was if that one left (reloadMenu()
// would put the player in the game's own folder instead)
static void refreshCollections(void)
{
    // a list deleted while it was open: back out of it
    while (stack->count > 1 && prefixMatch(COLLECTIONS_PATH, top->path) && suffixMatch(".txt", top->path) && !exists(top->path))
        closeDirectory();
    for (int i = 0; i < stack->count; i++)
    {
        Directory *old = stack->items[i];
        if (!exactMatch(old->path, SDCARD_PATH) && !prefixMatch(COLLECTIONS_PATH, old->path))
            continue;
        if (i == 0)
        { // getRoot() reads recents again, into this array: as Menu_init()
            RecentArray_free(recents);
            recents = Array_new();
        }
        Directory *dir = Directory_new(old->path, old->selected);
        dir->start = old->start;
        int at = old->selected;
        if (old->entries->count > 0)
        {
            Entry *was = old->entries->items[old->selected];
            for (int j = 0; j < dir->entries->count; j++)
                if (exactMatch(((Entry *)dir->entries->items[j])->path, was->path))
                    at = j;
        }
        Directory_fit(dir, at);
        stack->items[i] = dir;
        if (top == old)
            top = dir;
        Directory_free(old);
    }
}

///////////////////////////////////////

int main(int argc, char *argv[])
{
    if (autoResume())
        return 0; // nothing to do

    simple_mode = exists(SIMPLE_MODE_PATH);

    LOG_info("MinUI - MiaUI\n");
    InitSettings();
    I18N_init();
    NB_init(); // settles the game that just ended, before anything is drawn

    // the whole ui is drawn by the GPU: screen is only the handle the GFX_*
    // helpers are given, nothing is drawn into its pixels
    PLAT_setVideoFlags(PLAT_VIDEO_RGB888);
    screen = GFX_init(MODE_MAIN);
    NBUI_init();
    QM_init();
    PWR_init();
    if (!HAS_POWER_BUTTON && !simple_mode)
        PWR_disableSleep();

    Menu_init();
    uint32_t xp_level, xp_from;
    if (exists(QM_REOPEN_PATH))
    {
        unlink(QM_REOPEN_PATH);
        QM_open();
    }
    else if (NB_cameFromMission() && NB_xpToShow(&xp_level, &xp_from))
        NekoBit_open(); // back from a mission's game with XP earned: show it go up

    // now that (most of) the heavy lifting is done, take a load off
    PWR_setCPUSpeed(CPU_SPEED_MENU);
    GFX_setVsync(VSYNC_STRICT);

    int lastScreen = SCREEN_OFF;

    PAD_reset();
    GFX_clear(screen);

    int dirty = 1;

    int show_setting = 0; // 1=brightness,2=volume
    int delete_confirm = 0; // asking before the selected game is deleted
    int resume_blocked = 0; // X went down for DELETE: its release doesn't resume
    int cm_from_qm = 0; // the Collections manager came from the Quick Menu: B goes back to it
    int was_online = PLAT_isOnline();

    // game art for the selected entry: what it should show, and what has
    // been asked for (from the cache or the loader) since it last changed
    int had_thumb = 0;
    int ox = 0;
    char wanted_bg_path[MAX_PATH] = {0};
    char wanted_thumb_path[MAX_PATH] = {0};
    char asked_bg_path[MAX_PATH] = {0};
    char asked_thumb_path[MAX_PATH] = {0};

    uint32_t update_timer = SDL_GetTicks();

    // Initialize thumbnail thread
    pthread_create(&thumb_thread, NULL, &ThumbnailLoaderThread, NULL);

    while (!quit)
    {
        GFX_startFrame();
        unsigned long now = SDL_GetTicks();

        PAD_poll();

        if (stack->count == 1)
            nb_mission_dir[0] = '\0'; // back on the root: on their own again

        int selected = top->selected;
        int total = top->entries->count;

        PWR_update(&dirty, &show_setting, NULL, NULL);

        int overlay = NBUI_isOpen() || QM_isOpen() || CM_isOpen() || delete_confirm;
        int held = overlay ? HOLD_NONE : heldAction(show_setting);
        if (stack->count > 1 && !overlay &&
            (PAD_justPressed(BTN_SELECT) || PAD_justReleased(BTN_SELECT) || PAD_justPressed(BTN_START) || PAD_justReleased(BTN_START)))
            dirty = 1; // the left pill follows the buttons
        if (PAD_justReleased(BTN_RESUME) && delete_confirm)
            resume_blocked = 0; // DELETE's X let go under the question (later, the list takes it below)

        int is_online = PLAT_isOnline();
        if (was_online != is_online)
            dirty = 1;
        was_online = is_online;

        if (QM_update())
            dirty = 1;

        int nb = NBUI_update(now, stack->count == 1 && !QM_isOpen() && !NBUI_isOpen() && !CM_isOpen());
        if (nb & NBUI_REDRAW)
            dirty = 1;
        if (nb & NBUI_SUMMONED) // NekoBit is fully out: now the overlay
        {
            NekoBit_open();
            dirty = 1;
        }

        if (NBUI_busy())
        {
            // NekoBit is on its way up: the menu holds still
        }
        else if (NBUI_isOpen())
        {
            NBUI_Action action;
            if (NBUI_input(&action))
                dirty = 1;
            if (action.kind == NBUI_GOTO)
            {
                NekoBit_navigate(&action.target);
                update_timer = now;
                if (top->entries->count > 0)
                    readyResume(top->entries->items[top->selected]);
            }
        }
        else if (CM_isOpen())
        {
            if (CM_input())
                dirty = 1;
            if (!CM_isOpen())
            {
                if (CM_changed())
                    refreshCollections();
                if (cm_from_qm)
                    QM_open(); // as its other pages: B is back to the icons
                cm_from_qm = 0;
                total = top->entries->count;
                update_timer = now;
                dirty = 1;
                if (total > 0)
                    readyResume(top->entries->items[top->selected]);
            }
        }
        else if (delete_confirm)
        {
            if (PAD_justPressed(BTN_A))
            {
                delete_confirm = 0;
                if (deleteCurrentEntry(top, top->selected))
                    Directory_fit(top, top->selected);
                total = top->entries->count;
                if (total > 0)
                    readyResume(top->entries->items[top->selected]);
                update_timer = SDL_GetTicks(); // the message held the loop: no skipped frames
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B))
            {
                delete_confirm = 0;
                dirty = 1;
            }
        }
        else if (QM_isOpen())
        {
            int action;
            if (QM_input(&action))
                dirty = 1;
            if (action != QM_NONE)
            {
                if (QM_kidsMode() != simple_mode)
                {
                    simple_mode = QM_kidsMode();
                    reloadMenu();
                    total = top->entries->count;
                    update_timer = now;
                }
                if (!HAS_POWER_BUTTON && !simple_mode)
                    PWR_disableSleep();
                else if (action == QM_COLLECTIONS)
                {
                    CM_openManager();
                    cm_from_qm = 1;
                }
            }
        }
        else
        {
            // SELECT calls NekoBit, on the root menu only, as soon as it goes
            // down: held, a hiding NekoBit rises; let go early, it sinks back
            if (PAD_justPressed(BTN_SELECT) && stack->count == 1)
            {
                NBUI_summon(now); // the overlay opens when it's all the way up
                dirty = 1;
            }
            else if (PAD_tappedMenu(now))
            {
                QM_open();
                dirty = 1;
                if (!HAS_POWER_BUTTON && !simple_mode)
                    PWR_enableSleep();
            }
            else if (held == HOLD_FAV && PAD_justPressed(BTN_Y))
            {
                openCollections();
                dirty = 1;
            }
            else if (held == HOLD_DELETE && PAD_justPressed(BTN_X))
            {
                delete_confirm = 1;
                resume_blocked = 1;
                dirty = 1;
            }
            else if (total > 0)
            {
                if (PAD_justRepeated(BTN_UP))
                {
                    if (selected == 0 && !PAD_justPressed(BTN_UP))
                    {
                        // stop at top
                    }
                    else
                    {
                        update_timer = now;
                        selected -= 1;
                        if (selected < 0)
                        {
                            selected = total - 1;
                            int start = total - MAIN_ROW_COUNT;
                            top->start = (start < 0) ? 0 : start;
                            top->end = total;
                        }
                        else if (selected < top->start)
                        {
                            top->start -= 1;
                            top->end -= 1;
                        }
                    }
                }
                else if (PAD_justRepeated(BTN_DOWN))
                {
                    if (selected == total - 1 && !PAD_justPressed(BTN_DOWN))
                    {
                        // stop at bottom
                    }
                    else
                    {
                        update_timer = now;
                        selected += 1;
                        if (selected >= total)
                        {
                            selected = 0;
                            top->start = 0;
                            top->end = (total < MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
                        }
                        else if (selected >= top->end)
                        {
                            top->start += 1;
                            top->end += 1;
                        }
                    }
                }
                if (PAD_justRepeated(BTN_LEFT))
                {
                    update_timer = now;
                    selected -= MAIN_ROW_COUNT;
                    if (selected < 0)
                    {
                        selected = 0;
                        top->start = 0;
                        top->end = (total < MAIN_ROW_COUNT) ? total : MAIN_ROW_COUNT;
                    }
                    else if (selected < top->start)
                    {
                        top->start -= MAIN_ROW_COUNT;
                        if (top->start < 0)
                            top->start = 0;
                        top->end = top->start + MAIN_ROW_COUNT;
                    }
                }
                else if (PAD_justRepeated(BTN_RIGHT))
                {
                    update_timer = now;
                    selected += MAIN_ROW_COUNT;
                    if (selected >= total)
                    {
                        selected = total - 1;
                        int start = total - MAIN_ROW_COUNT;
                        top->start = (start < 0) ? 0 : start;
                        top->end = total;
                    }
                    else if (selected >= top->end)
                    {
                        top->end += MAIN_ROW_COUNT;
                        if (top->end > total)
                            top->end = total;
                        top->start = top->end - MAIN_ROW_COUNT;
                    }
                }
            }

            if (PAD_justRepeated(BTN_L1) && !PAD_isPressed(BTN_R1) && !PWR_ignoreSettingInput(BTN_L1, show_setting))
            { // previous alpha
                Entry *entry = top->entries->items[selected];
                int i = entry->alpha - 1;
                if (i >= 0)
                {
                    selected = top->alphas->items[i];
                    if (total > MAIN_ROW_COUNT)
                    {
                        top->start = selected;
                        top->end = top->start + MAIN_ROW_COUNT;
                        if (top->end > total)
                            top->end = total;
                        top->start = top->end - MAIN_ROW_COUNT;
                    }
                }
            }
            else if (PAD_justRepeated(BTN_R1) && !PAD_isPressed(BTN_L1) && !PWR_ignoreSettingInput(BTN_R1, show_setting))
            { // next alpha
                Entry *entry = top->entries->items[selected];
                int i = entry->alpha + 1;
                if (i < top->alphas->count)
                {
                    selected = top->alphas->items[i];
                    if (total > MAIN_ROW_COUNT)
                    {
                        top->start = selected;
                        top->end = top->start + MAIN_ROW_COUNT;
                        if (top->end > total)
                            top->end = total;
                        top->start = top->end - MAIN_ROW_COUNT;
                    }
                }
            }

            if (selected != top->selected)
            {
                top->selected = selected;
                dirty = 1;
            }

            if (dirty && total > 0)
                readyResume(top->entries->items[top->selected]);

            if (resume_blocked && PAD_justReleased(BTN_RESUME))
                resume_blocked = 0;
            else if (total > 0 && can_resume && held == HOLD_NONE && PAD_justReleased(BTN_RESUME))
            {
                should_resume = 1;
                Entry_open(top->entries->items[top->selected]);
                dirty = 1;
            }
            else if (total > 0 && PAD_justPressed(BTN_A))
            {
                update_timer = now;
                Entry_open(top->entries->items[top->selected]);
                total = top->entries->count;
                dirty = 1;

                if (total > 0)
                    readyResume(top->entries->items[top->selected]);
            }
            else if (PAD_justPressed(BTN_B) && stack->count > 1)
            {
                update_timer = now;
                closeDirectory();
                total = top->entries->count;
                dirty = 1;
                // can_resume = 0;
                if (total > 0)
                    readyResume(top->entries->items[top->selected]);
            }
        }

        // Ask for the selected entry's art once the selection has rested:
        // what is cached shows straight away, the rest goes to the loader
        int wait = stack->count == 1 ? 200 : 1500;
        if (now - update_timer >= wait)
        {
            update_timer = now;
            char load_bg[MAX_PATH] = {0};
            char load_thumb[MAX_PATH] = {0};
            if (strcmp(asked_bg_path, wanted_bg_path))
            {
                strcpy(asked_bg_path, wanted_bg_path);
                if (wanted_bg_path[0])
                {
                    Art *art = Art_find(wanted_bg_path);
                    if (art)
                    {
                        shown_bg = art;
                        dirty |= art->image.tex != 0;
                    }
                    else
                        strcpy(load_bg, wanted_bg_path);
                }
            }
            if (strcmp(asked_thumb_path, wanted_thumb_path))
            {
                strcpy(asked_thumb_path, wanted_thumb_path);
                if (wanted_thumb_path[0])
                {
                    Art *art = Art_find(wanted_thumb_path);
                    if (art)
                    {
                        shown_thumb = art;
                        dirty |= art->image.tex != 0;
                    }
                    else
                        strcpy(load_thumb, wanted_thumb_path);
                }
            }
            if (load_bg[0] || load_thumb[0])
            {
                pthread_mutex_lock(&thumb_mutex);
                strcpy(request_bg_path, load_bg);
                strcpy(request_thumb_path, load_thumb);
                request_pending = 1;
                pthread_cond_signal(&thumb_cond);
                pthread_mutex_unlock(&thumb_mutex);
            }
        }

        // Whatever the loader finished goes into the cache, and on screen if
        // it is still what the selection asked for
        if (result_bg_ready || result_thumb_ready)
        {
            pthread_mutex_lock(&thumb_mutex);
            if (result_bg_ready)
            {
                Art *art = Art_add(result_bg_path, result_bg);
                if (art && !strcmp(result_bg_path, asked_bg_path) && !strcmp(asked_bg_path, wanted_bg_path))
                {
                    shown_bg = art;
                    dirty |= art->image.tex != 0;
                }
                result_bg = NULL; // the cache took it
                result_bg_ready = 0;
            }
            if (result_thumb_ready)
            {
                Art *art = Art_add(result_thumb_path, result_thumb);
                if (art && !strcmp(result_thumb_path, asked_thumb_path) && !strcmp(asked_thumb_path, wanted_thumb_path))
                {
                    shown_thumb = art;
                    dirty |= art->image.tex != 0;
                }
                result_thumb = NULL;
                result_thumb_ready = 0;
            }
            pthread_mutex_unlock(&thumb_mutex);
        }
        had_thumb = shown_thumb && shown_thumb->image.tex;

        if (dirty)
        {
            GFX_clear(screen);

            if (total > 0)
            {
                Entry *entry = top->entries->items[top->selected];
                char tmp_path[MAX_PATH];
                strncpy(tmp_path, entry->path, sizeof(tmp_path) - 1);
                tmp_path[sizeof(tmp_path) - 1] = '\0';

                char *res_name = strrchr(tmp_path, '/');
                if (res_name)
                    res_name++;

                char path_copy[MAX_PATH];
                strncpy(path_copy, entry->path, sizeof(path_copy) - 1);
                path_copy[sizeof(path_copy) - 1] = '\0';

                char *rompath = dirname(path_copy);

                char bg_path[MAX_PATH] = {0};
                char thumbpath[MAX_PATH] = {0};

                // folder-specific background
                if (entry->type == ENTRY_DIR)
                {
                    snprintf(bg_path, sizeof(bg_path), "%s/.media/bg.png", tmp_path);
                }

                // Thumbnail logic
                if ((entry->type == ENTRY_ROM)||(entry->type == ENTRY_DIR))
                {
                    char res_copy[MAX_PATH];
                    if (res_name)
                    {
                        strncpy(res_copy, res_name, sizeof(res_copy) - 1);
                        res_copy[sizeof(res_copy) - 1] = '\0';

                        // buscar extension
                        char *dot = strrchr(res_copy, '.');
                        if (dot)
                            *dot = '\0';

                        snprintf(thumbpath, sizeof(thumbpath), "%s/.media/%s.png", rompath, res_copy);
                    }
                }

                // A new selection hides the old art at once; its own is asked
                // for when the selection has rested (see above), cached or not
                if (strcmp(wanted_bg_path, bg_path))
                {
                    strcpy(wanted_bg_path, bg_path);
                    asked_bg_path[0] = '\0';
                    shown_bg = NULL;
                }
                if (strcmp(wanted_thumb_path, thumbpath))
                {
                    strcpy(wanted_thumb_path, thumbpath);
                    asked_thumb_path[0] = '\0';
                    shown_thumb = NULL;
                    had_thumb = 0;
                }
            }

            // the folder background goes under everything, the battery and
            // brightness pill included (it used to be drawn over them)
            if (!QM_isOpen() && !NBUI_isOpen() && !CM_isOpen() && !delete_confirm)
                Art_draw(shown_bg, 0, 0);

            int ow = GFX_blitHardwareGroup(screen, show_setting);

            if (NBUI_isOpen())
            {
                NBUI_draw(screen, show_setting);
            }
            else if (QM_isOpen())
            {
                QM_draw(screen, show_setting);
            }
            else if (CM_isOpen())
            {
                CM_draw(screen, show_setting, ow);
            }
            else if (delete_confirm)
            {
                // as the Quick Menu's dialogs: the question on its own, the answers under it
                Entry *entry = top->entries->items[top->selected];
                char *name = entry->name;
                trimSortingMeta(&name);
                char text[MAX_PATH];
                snprintf(text, sizeof(text), T(STR_DELETE_GAME), name);
                GFX_blitMessage(font.medium, text, screen, &(SDL_Rect){0, SCALE1(PADDING), screen->w, screen->h - SCALE1(PILL_SIZE + PADDING)});
                if (show_setting && !GetHDMI())
                    GFX_blitHardwareHints(screen, show_setting);
                GFX_blitButtonGroup((char *[]){"B", (char *)T(STR_MA_CANCEL), "A", (char *)T(STR_DELETE), NULL}, 1, screen, 1);
            }
            else
            {
                if (had_thumb)
                {
                    int target_y = (int)(screen->h * 0.48);
                    int center_y = target_y - (shown_thumb->image.h / 2);
                    ox = (int)(screen->w - shown_thumb->image.w) - SCALE1(BUTTON_MARGIN);
                    Art_draw(shown_thumb, screen->w - (shown_thumb->image.w + SCALE1(BUTTON_MARGIN * 3)), center_y);
                }

                // list
                if (total > 0)
                {
                    int selected_row = top->selected - top->start;
                    for (int i = top->start, j = 0; i < top->end; i++, j++)
                    {
                        Entry *entry = top->entries->items[i];
                        char *entry_name = entry->name;
                        char *entry_unique = entry->unique;

                        // Modified width calculation
                        // int available_width = screen->w - SCALE1(PADDING * 2);
                        // if (i==top->start) available_width -= ow;
                        int available_width = (had_thumb ? ox - SCALE1(BUTTON_MARGIN) : screen->w - SCALE1(BUTTON_PADDING)) - SCALE1(PADDING * 2);
                        if (i == top->start && !(had_thumb))
                            available_width -= ow;

                        SDL_Color text_color = COLOR_WHITE;
                        trimSortingMeta(&entry_name);

                        int text_width;
                        const char *display_name = fitText(&entry->fit_main, entry_unique ? entry_unique : entry_name, available_width, &text_width);
                        int max_width = MIN(available_width, text_width);
                        int text_x = SCALE1(PADDING + BUTTON_PADDING);
                        int text_y = SCALE1(PADDING + (j * PILL_SIZE) + 4);
                        SDL_Rect text_clip = {0, 0, max_width - SCALE1(BUTTON_PADDING * 2), 0};
                        if (j == selected_row)
                        {
                            text_color = COLOR_BLACK;
                            int pill_type = ASSET_WHITE_PILL;
                            if (entry->is_darker)
                            {
                                text_color = COLOR_DARK_PILL;
                                pill_type = ASSET_DARK_GRAY_PILL;
                            }
                            if (entry->needs_swap) {
                                GFX_blitPill(pill_type, screen, &(SDL_Rect){SCALE1(PADDING - 4), SCALE1(PADDING + (j * PILL_SIZE)), max_width, SCALE1(PILL_SIZE)});
                                GFX_blitAsset(ASSET_SWAP_DARK_GRAY, NULL, screen, &(SDL_Rect){SCALE1(PADDING + 2), SCALE1(PADDING + 11 + (j * PILL_SIZE))});
                            } else {
                                GFX_blitPill(pill_type, screen, &(SDL_Rect){SCALE1(PADDING), SCALE1(PADDING + (j * PILL_SIZE)), max_width, SCALE1(PILL_SIZE)});
                            }
                        }
                        else if (entry->unique)
                        {
                            trimSortingMeta(&entry_unique);
                            const char *unique_name = fitText(&entry->fit_unique, entry_unique, available_width, NULL);

                            GFX_Text text = GFX_renderText(font.large, unique_name, COLOR_DARK_TEXT);
                            text_clip.h = text.h;
                            GFX_blitRenderedText(&text, &text_clip, screen, text_x, text_y);

                            display_name = fitText(&entry->fit_name, entry_name, available_width, NULL);
                        }
                        else if (entry->is_darker)
                        {
                            text_color = COLOR_DARK_TEXT;
                        }
                        else if (entry->needs_swap) {
                            GFX_blitAsset(ASSET_SWAP_GRAY, NULL, screen, &(SDL_Rect){SCALE1(PADDING + 2), SCALE1(PADDING + 11 + (j * PILL_SIZE))});
                        }
                        GFX_Text text = GFX_renderText(font.large, display_name, text_color);
                        text_clip.h = text.h;
                        GFX_blitRenderedText(&text, &text_clip, screen, text_x, text_y);
                    }
                }
                else
                {
                    // TODO: for some reason screen's dimensions end up being 0x0 in GFX_blitMessage...
                    GFX_blitMessage(font.large, (char *)T(STR_EMPTY_FOLDER), screen, &(SDL_Rect){0, 0, screen->w, screen->h});
                }

                // buttons
                int lw = screen->w; // the left group's width: NekoBit's pill keeps clear of it
                if (show_setting && !GetHDMI())
                    GFX_blitHardwareHints(screen, show_setting);
                else if (held == HOLD_FAV)
                    lw = GFX_blitButtonGroup((char *[]){"Y", (char *)T(STR_FAV), NULL}, 0, screen, 0);
                else if (held == HOLD_DELETE)
                    lw = GFX_blitButtonGroup((char *[]){"X", (char *)T(STR_DELETE), NULL}, 0, screen, 0);
                else if (can_resume)
                    lw = GFX_blitButtonGroup((char *[]){"X", (char *)T(STR_RESUME), NULL}, 0, screen, 0);
                else
                    lw = GFX_blitButtonGroup((char *[]){
                                            BTN_SLEEP == BTN_POWER ? "POWER" : "MENU",
                                            (char *)(BTN_SLEEP == BTN_POWER || simple_mode ? T(STR_SLEEP) : T(STR_INFO)),
                                            NULL},
                                        0, screen, 0);

                if (total == 0)
                {
                    if (stack->count > 1)
                    {
                        GFX_blitButtonGroup((char *[]){"B", (char *)T(STR_BACK), NULL}, 0, screen, 1);
                    }
                }
                else
                {
                    if (stack->count > 1)
                    {
                        GFX_blitButtonGroup((char *[]){"B", (char *)T(STR_BACK), "A", (char *)T(STR_OPEN), NULL}, 1, screen, 1);
                    }
                    else
                    {
                        int bw = GFX_blitButtonGroup((char *[]){"A", (char *)T(STR_OPEN), NULL}, 0, screen, 1);
                        if (stack->count == 1) // NekoBit lives on the root menu, between the two groups
                            NBUI_drawFace(screen, SCALE1(PADDING + BUTTON_MARGIN) + lw, screen->w - SCALE1(PADDING + BUTTON_MARGIN) - bw);
                    }
                }
            }

            NBUI_drawBanner(screen);
            GFX_flip(screen);
            dirty = 0;
        }
        else
            GFX_sync(); // sleeps until input or the next frame (key repeat, the art delay)
    }

    // Thread cleanup
    pthread_mutex_lock(&thumb_mutex);
    thread_running = 0;
    pthread_cond_signal(&thumb_cond);
    pthread_mutex_unlock(&thumb_mutex);
    pthread_join(thumb_thread, NULL);

    if (result_thumb)
        SDL_FreeSurface(result_thumb);
    if (result_bg)
        SDL_FreeSurface(result_bg);
    Art_quit(); // while there is still a GL context

    Menu_quit();
    NB_quit();
    PWR_quit();
    GFX_quit();
    QuitSettings();
}
