#ifndef __API_H__
#define __API_H__
#include "sdl.h"
#include "platform.h"

///////////////////////////////

enum {
	LOG_DEBUG = 0,
	LOG_INFO,
	LOG_WARN,
	LOG_ERROR,
};

#define LOG_debug(fmt, ...) LOG_note(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_info(fmt, ...) LOG_note(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_warn(fmt, ...) LOG_note(LOG_WARN, fmt, ##__VA_ARGS__)
#define LOG_error(fmt, ...) LOG_note(LOG_ERROR, fmt, ##__VA_ARGS__)
void LOG_note(int level, const char* fmt, ...);

///////////////////////////////

#define RGBA_MASK_565	0xF800, 0x07E0, 0x001F, 0x0000

///////////////////////////////

enum {
	ASSET_WHITE_PILL,
	ASSET_BLACK_PILL,
	ASSET_DARK_GRAY_PILL,
	ASSET_OPTION,
	ASSET_BUTTON,
	ASSET_PAGE_BG,
	ASSET_STATE_BG,
	ASSET_PAGE,
	ASSET_BAR,
	ASSET_BAR_BG,
	ASSET_BAR_BG_MENU,
	ASSET_UNDERLINE,
	ASSET_DOT,
	
	ASSET_COLORS,
	
	ASSET_BRIGHTNESS,
	ASSET_VOLUME_MUTE,
	ASSET_VOLUME,
	ASSET_BATTERY,
	ASSET_BATTERY_LOW,
	ASSET_BATTERY_FILL,
	ASSET_BATTERY_FILL_LOW,
	ASSET_BATTERY_BOLT,
	
	ASSET_SCROLL_UP,
	ASSET_SCROLL_DOWN,
	
	ASSET_WIFI,
	ASSET_SWAP,
	ASSET_SWAP_GRAY,
	ASSET_SWAP_DARK_GRAY,
	ASSET_TROPHY,

	ASSET_WIFI_SMALL, // sized like the usb plug: the Quick Menu, the battery pill
	ASSET_GEAR,
	ASSET_USB,
	ASSET_CLOCK,
	ASSET_GAMEPAD,
	ASSET_SDCARD,
};

typedef struct GFX_Fonts {
	TTF_Font* large; 	// menu
	TTF_Font* medium; 	// single char button label
	TTF_Font* small; 	// button hint
	TTF_Font* tiny; 	// multi char button label
} GFX_Fonts;
extern GFX_Fonts font;

enum {
	SHARPNESS_CRISP,
	SHARPNESS_SOFT,
};

typedef struct GFX_Renderer {
	void* src;
	void* dst;
	void* blit;
	double aspect; // 0 for integer, -1 for fullscreen, otherwise aspect ratio, used for SDL2 accelerated scaling
	int scale;
	
	// TODO: document this better
	int true_w;
	int true_h;

	int src_x;
	int src_y;
	int src_w;
	int src_h;
	int src_p;
	int src_bpp; // bytes per pixel of src, from the negotiated retro_pixel_format (0 = 2)
	
	// TODO: I think this is overscaled
	int dst_x;
	int dst_y;
	int dst_w;
	int dst_h;
	int dst_p;
} GFX_Renderer;

enum {
	MODE_MAIN,
	MODE_MENU,
};

// The ui is drawn by the GPU. Every SDL_Surface the GFX_* functions draw on
// is a render target: the screen GFX_init returns, and whatever
// GFX_createSurface makes. A target has a size but no pixels, so it is only
// ever drawn on with these functions, never with SDL's. Software surfaces
// (a decoded PNG, a game frame) are images: GFX_loadImage puts one on the
// GPU, GFX_blitImage draws it. Main thread only, like the rest of GFX.

SDL_Surface* GFX_init(int mode); // the screen; exits if the GPU can't draw the ui
#define GFX_setScaleClip PLAT_setVideoScaleClip // (int x, int y, int width, int height)
#define GFX_setNearestNeighbor PLAT_setNearestNeighbor // (int enabled)
#define GFX_setSharpness PLAT_setSharpness // (int sharpness)
#define GFX_initGameOverlay PLAT_initGameOverlay
#define GFX_blitGameOverlay PLAT_uiDrawGameOverlay // (SDL_Surface* dst) the bezel over all of dst, if there is one
void GFX_setMode(int mode);
int GFX_hdmiChanged(void);

#define GFX_clear PLAT_clearVideo // (SDL_Surface* target) to transparent
#define GFX_clearAll PLAT_clearAll // (void) the screen
#define GFX_setUILayer PLAT_setUILayer // (int visible)
#define GFX_loadToast PLAT_loadToast // (SDL_Surface* target) a GFX_createSurface the platform takes over, NULL to drop it
#define GFX_showToast PLAT_showToast // (int visible, int x, int y) per frame, in screen pixels

#define GFX_createSurface PLAT_uiCreateSurface // (int w, int h) a transparent render target, NULL on failure
#define GFX_freeSurface PLAT_uiFreeSurface // (SDL_Surface* target)

#define PAD_init PLAT_initInput
#define PAD_quit PLAT_quitInput

void GFX_startFrame(void);
void GFX_flip(SDL_Surface* screen);
double GFX_getRefreshRate(void); // the panel's measured rate, not what SDL reports
#define GFX_repeatFrame PLAT_repeatFrame // (void) next flip shows the last game frame again, no upload
void GFX_sync(void); // when not calling GFX_flip() this frame: sleeps until input or the next frame is due
void GFX_quit(void);

enum {
	VSYNC_OFF = 0,
	VSYNC_LENIENT, // default
	VSYNC_STRICT,
};

enum {
	// actual views
	SCREEN_GAMELIST,
	SCREEN_GAMESWITCHER,
	SCREEN_QUICKMENU,
	// meta
	SCREEN_GAME,
	SCREEN_OFF
};

int GFX_getVsync(void);
void GFX_setVsync(int vsync);

// A string in one colour. Strings are rendered once, into a texture cache
// keyed by font and string, and drawn tinted from then on, so asking again
// for one that was drawn recently costs nothing. Nothing to free: the cache
// owns the texture, and keeps everything asked for in the current frame. A
// font of your own has to stay open until GFX_quit.
typedef struct GFX_Text {
	unsigned tex;
	int w, h;   // 0x0 if nothing was rendered (an empty string)
	SDL_Color color;
} GFX_Text;
GFX_Text GFX_renderText(TTF_Font* font, const char* str, SDL_Color color);
void GFX_blitRenderedText(GFX_Text* text, SDL_Rect* src_rect, SDL_Surface* dst, int x, int y); // src_rect NULL = all

// A picture on the GPU, from a software surface (premultiplied on the way)
typedef struct GFX_Image {
	unsigned tex; // 0 if it couldn't be loaded
	int w, h;
} GFX_Image;
GFX_Image GFX_loadImage(SDL_Surface* surface); // a copy: free the surface whenever
void GFX_freeImage(GFX_Image* image);
// src_rect NULL = all; dst_rect NULL = at 0,0, a zero w/h = 1:1, otherwise
// scaled into it (nearest neighbour)
void GFX_blitImage(GFX_Image* image, SDL_Rect* src_rect, SDL_Surface* dst, SDL_Rect* dst_rect);

void GFX_fillRect(SDL_Surface* dst, SDL_Rect* rect, SDL_Color color, int opacity); // rect NULL = all, opacity 0..255

int GFX_truncateText(TTF_Font* font, const char* in_name, char* out_name, int max_width, int padding); // returns final width
int GFX_wrapText(TTF_Font* font, char* str, int max_width, int max_lines);

#define GFX_getScaler PLAT_getScaler		// scaler_t:(GFX_Renderer* renderer)
#define GFX_blitRenderer PLAT_blitRenderer	// void:(GFX_Renderer* renderer)

// NOTE: all dimensions should be pre-scaled
void GFX_blitAsset(int asset, SDL_Rect* src_rect, SDL_Surface* dst, SDL_Rect* dst_rect);
void GFX_blitPill(int asset, SDL_Surface* dst, SDL_Rect* dst_rect);
void GFX_blitPillAlpha(int asset, SDL_Surface* dst, SDL_Rect* dst_rect, int opacity); // 0..255
void GFX_blitPillColor(SDL_Surface* dst, SDL_Rect* dst_rect, SDL_Color color); // any colour, eg. COLOR_DARK_PILL
void GFX_blitPillOutline(SDL_Surface* dst, SDL_Rect* dst_rect, int border, SDL_Color line, SDL_Color fill); // any height: a `border` ring of line around fill
void GFX_blitAssetColor(int asset, SDL_Rect* src_rect, SDL_Surface* dst, SDL_Rect* dst_rect, SDL_Color color); // white parts take color
void GFX_blitAssetScaled(int asset, SDL_Surface* dst, SDL_Rect* box, int scale, SDL_Color color); // `scale` times its size (nearest), centred in box
void GFX_blitAssetSized(int asset, SDL_Surface* dst, SDL_Rect* dst_rect, SDL_Color color); // stretched to dst_rect's w x h (nearest)
void GFX_blitRect(int asset, SDL_Surface* dst, SDL_Rect* dst_rect);
void GFX_blitBattery(SDL_Surface* dst, SDL_Rect* dst_rect);
int GFX_getButtonWidth(char* hint, char* button);
void GFX_blitButton(char* hint, char*button, SDL_Surface* dst, SDL_Rect* dst_rect);
void GFX_blitMessage(TTF_Font* font, char* msg, SDL_Surface* dst, SDL_Rect* dst_rect);

int GFX_blitHardwareGroup(SDL_Surface* dst, int show_setting);
void GFX_blitHardwareHints(SDL_Surface* dst, int show_setting);
int GFX_blitButtonGroup(char** hints, int primary, SDL_Surface* dst, int align_right);

void GFX_sizeText(TTF_Font* font, char* str, int leading, int* w, int* h);
void GFX_blitText(TTF_Font* font, char* str, int leading, SDL_Color color, SDL_Surface* dst, SDL_Rect* dst_rect);

///////////////////////////////

typedef struct SND_Frame {
	int16_t left;
	int16_t right;
} SND_Frame;

void SND_init(double sample_rate, double frame_rate);
size_t SND_batchSamples(const SND_Frame* frames, size_t frame_count);
int SND_isVideoClocked(void); // 1 when vsync paces the emulator and audio is resampled to follow
void SND_setVideoSync(int enabled); // 0 = always pace by audio (exact core speed)
void SND_quit(void);
// Mixes a short two-note chime into the output. Non-blocking and callable from
// any thread; synthesised in the audio callback, so it never adds work to the
// emulation loop and never ducks the game audio.
void SND_playChime(void);

///////////////////////////////

void PAD_reset(void);
void PAD_poll(void);
int PAD_anyPressed(void);
int PAD_justPressed(int btn);
int PAD_isPressed(int btn);
int PAD_justReleased(int btn);
int PAD_justRepeated(int btn);

int PAD_tappedMenu(uint32_t now); // special case, returns 1 on release of BTN_MENU within 250ms if BTN_PLUS/BTN_MINUS haven't been pressed

///////////////////////////////

void VIB_init(void);
void VIB_quit(void);
void VIB_setStrength(int strength);
 int VIB_getStrength(void);

///////////////////////////////

// Save data, written off the emulation thread. Every write is an atomic
// replace (<path>.tmp, fsync, rename, fsync the directory), so a crash or a
// power cut leaves the old file or the new one, never a truncated one.

void IO_writeFile(const char* path, void* data, size_t size); // queues it; takes ownership of data (malloc'd)
 int IO_writeFileNow(const char* path, const void* data, size_t size); // same, on this thread; 0 on success
void IO_flush(void); // returns once everything queued so far is on disk
void IO_quit(void);  // flush, then stop the writer thread

///////////////////////////////

#define BRIGHTNESS_BUTTON_LABEL "+ -" // ew

typedef void (*PWR_callback_t)(void);
void PWR_init(void);
void PWR_quit(void);
void PWR_warn(int enable);

int PWR_ignoreSettingInput(int btn, int show_setting);
void PWR_update(int* dirty, int* show_setting, PWR_callback_t before_sleep, PWR_callback_t after_sleep);

void PWR_disablePowerOff(void);
void PWR_powerOff(void);
int PWR_isPoweringOff(void);

void PWR_fauxSleep(void);

void PWR_disableSleep(void);
void PWR_enableSleep(void);

void PWR_disableAutosleep(void);
void PWR_enableAutosleep(void);
int PWR_preventAutosleep(void);

// The USB gadget mode minui's Quick Menu switched on. It can't be switched off
// or changed (the console hangs), so it holds until a reboot; the file is in
// tmpfs for the same reason. Read once per process, it's drawn by the battery.
// what usbnet.sh has set the port up as (it writes the file): ADB, ADB + a
// network, or the network alone
enum { USB_MODE_NONE, USB_MODE_ADB, USB_MODE_NET, USB_MODE_NET_ONLY };
#define USB_MODE_PATH "/tmp/usb_mode"
int USB_getMode(void);
void USB_setMode(int mode);
void USB_reloadMode(void); // the file changed: read it again
int USB_hasNetwork(int mode);

int PWR_isCharging(void);
int PWR_getBattery(void);

enum {
	CPU_SPEED_MENU,
	CPU_SPEED_POWERSAVE,
	CPU_SPEED_NORMAL,
	CPU_SPEED_PERFORMANCE,
};
#define PWR_setCPUSpeed PLAT_setCPUSpeed
/////////////

typedef void (*scaler_t)(void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dw, uint32_t dh, uint32_t dp);


///////////////////////////////

void PLAT_initInput();
void PLAT_quitInput();

// Opt-in video modes, set before GFX_init (minarch's --32bits and --gl).
// Without any, the window gets the lightest config the driver has: 565, no
// depth, no stencil.
#define PLAT_VIDEO_32BIT  1 // 8888 window with depth/stencil, XRGB8888 cores accepted
#define PLAT_VIDEO_GL     2 // libretro hardware rendering (implies the above)
#define PLAT_VIDEO_RGB888 4 // 888 window, no depth: for when the ui is the whole picture (minui, tools)
void PLAT_setVideoFlags(int flags);

// The GPU-drawn ui, under the GFX_* functions above. Render targets are
// SDL_Surfaces with no pixels; textures are GL names, 0 = none. Everything
// is premultiplied by alpha. Main thread only.
SDL_Surface* PLAT_uiCreateSurface(int w, int h); // RGBA8 render target, transparent; NULL on failure
void PLAT_uiFreeSurface(SDL_Surface* target);
unsigned PLAT_uiSurfaceTexture(SDL_Surface* target); // what it holds, to draw it elsewhere
void PLAT_uiClear(SDL_Surface* target); // to transparent
unsigned PLAT_uiTexture(SDL_Surface* image); // a copy of a software surface's pixels, premultiplied
unsigned PLAT_uiAlphaTexture(const void* alpha, int w, int h); // 8 bits a pixel, rows packed
void PLAT_uiFreeTexture(unsigned tex);
// src_rect NULL = all; dst_rect as GFX_blitImage. RGBA textures are
// multiplied by tint, alpha ones are drawn in it; opacity 0..255 on top.
void PLAT_uiDraw(SDL_Surface* dst, unsigned tex, int tex_w, int tex_h, int alpha_only,
                 const SDL_Rect* src_rect, const SDL_Rect* dst_rect, SDL_Color tint, int opacity);
void PLAT_uiFill(SDL_Surface* dst, const SDL_Rect* rect, SDL_Color color, int opacity); // rect NULL = all
void PLAT_uiDrawGameOverlay(SDL_Surface* dst); // the game overlay (bezel) stretched over dst, if one is loaded
// 1 if the core's 32-bit XRGB8888 output can go to the GPU directly. Always 0
// without PLAT_VIDEO_32BIT.
int PLAT_supports32Bit(void);

// Hardware rendering, only ever called with PLAT_VIDEO_GL. The core renders
// into an FBO in the frontend's own context; its colour texture then stands
// in for the game layer, and while it does PLAT_getScaler hands out a scaler
// that draws it instead of uploading - so a hardware frame goes through the
// same renderer calls as a software one.
unsigned PLAT_hwInit(int width, int height, int depth, int stencil, int bottom_left); // the FBO, 0 on failure
void PLAT_hwQuit(void);
void PLAT_hwRestoreState(void); // put back what the core may have changed
void PLAT_hwPresent(int hw_frame); // 1: the FBO is the game layer, 0: software frames again
SDL_Surface* PLAT_hwCapture(int w, int h); // the last frame as RGB565 (~40ms), NULL on failure

SDL_Surface* PLAT_initVideo(void); // the screen, a render target; NULL if the GPU can't draw the ui
void PLAT_quitVideo(void);
void PLAT_clearVideo(SDL_Surface* target);
void PLAT_clearAll(void);
// The ui layer is a full-screen texture composited over the game, and in
// game it is almost always empty - so let the caller say when it has nothing
// in it and the composite can be skipped. Defaults to visible.
void PLAT_setUILayer(int visible);
void PLAT_loadToast(SDL_Surface* target);
void PLAT_showToast(int visible, int x, int y);
void PLAT_setVsync(int vsync);
void PLAT_setVideoScaleClip(int x, int y, int width, int height);
void PLAT_setNearestNeighbor(int enabled);
void PLAT_setSharpness(int sharpness);
scaler_t PLAT_getScaler(GFX_Renderer* renderer);
void PLAT_blitRenderer(GFX_Renderer* renderer);
void PLAT_flip(SDL_Surface* screen, int sync);
void PLAT_repeatFrame(void);

void PLAT_initGameOverlay(const char* ovl_path);
SDL_Surface* PLAT_initOverlay(void); // the battery pill's render target
void PLAT_quitOverlay(void);
void PLAT_enableOverlay(int enable);
	
#define PWR_LOW_CHARGE 10
void PLAT_getBatteryStatus(int* is_charging, int* charge); // 0,1 and 0,10,20,40,60,80,100
void PLAT_enableBacklight(int enable);
void PLAT_powerOff(void);
	
void PLAT_setCPUSpeed(int speed); // enum
void PLAT_setRumble(int strength);
int PLAT_pickSampleRate(int requested, int max);

char* PLAT_getModel(void);
int PLAT_isOnline(void);

#endif
