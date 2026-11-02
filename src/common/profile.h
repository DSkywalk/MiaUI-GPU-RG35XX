#ifndef __PROFILE_H__
#define __PROFILE_H__

// Frame-stage profiler for the in-game render path.
//
// Off unless built with -DPROFILE=1, in which case every macro below expands
// to nothing and the argument is never even tokenised into an expression - so
// a normal build carries no cost and needs no extra symbols.
//
// Build it with:  make PROFILE=1
//
// Reports land on stdout AND in PROFILE_LOG, because minarch is started by
// launch.sh without any redirection, so stdout is awkward to capture over adb.

#ifndef PROFILE
#define PROFILE 0
#endif

#if PROFILE

#include <stdint.h>

// how many presented frames per report
#ifndef PROFILE_FRAMES
#define PROFILE_FRAMES 300
#endif

// ...or this many seconds of wall time, whichever comes first. Without the
// time limit a stalled core never reports at all: at 0.1 fps a 300 frame
// window takes 50 minutes, which is exactly the case you most want measured.
#ifndef PROFILE_SECONDS
#define PROFILE_SECONDS 5
#endif

#ifndef PROFILE_LOG
#define PROFILE_LOG "/userdata/minarch_profile.log"
#endif

// PROFILE_GLFINISH=1 inserts a glFinish() before the swap so GPU time shows up
// in its own zone instead of hiding inside the vsync wait. It serialises CPU
// and GPU, so it measures GPU cost at the price of pipelining - diagnosis only,
// never a number to quote as the frame time.
#ifndef PROFILE_GLFINISH
#define PROFILE_GLFINISH 0
#endif

enum {
    PROF_FRAME_TOTAL,  // one whole main-loop iteration
    PROF_CORE_RUN,     // libretro emulation
    PROF_VIDEO,        // video_refresh_callback, excluding the flip
    PROF_AUDIO,        // SND_batchSamples: blocks while the ring buffer is full,
                       // so this is where a stalled frame clock shows up
    PROF_TEX_GAME,     // uploading the core framebuffer as a texture
    PROF_TEX_UI,       // uploading the full-screen ui layer
    PROF_DRAW,         // the quad draws
    PROF_GPU_FINISH,   // glFinish, only with PROFILE_GLFINISH=1
    PROF_SWAP,         // SDL_GL_SwapWindow: vsync wait plus any gpu backlog
    PROF_RA,           // RA_doFrame: achievement triggers against core memory
    PROF_ZONE_COUNT,
};

enum {
    PROF_DRAWS,        // draw calls
    PROF_TEX_BYTES,    // bytes pushed through glTexSubImage2D/glTexImage2D
    PROF_VBO_REBUILDS, // geometry uploads
    PROF_AUDIO_FRAMES, // audio frames handed to SND_batchSamples
    PROF_AUDIO_SLEEPS, // SDL_Delay(1) polls while the ring was full
    PROF_AUDIO_STARVED,// frames of silence the audio callback had to invent
    PROF_COUNTER_COUNT,
};

// min/avg/max of a sampled value
enum {
    PROF_G_RING_US,    // audio ring fill, sampled as each batch arrives
    PROF_G_RATE_PPM,   // audio stretch vs the core's own rate: panel skew plus fill correction
    PROF_G_REFRESH_MHZ,// GFX_flip's running estimate of the panel's refresh
    PROF_GAUGE_COUNT,
};

void PROF_begin(int zone);
void PROF_end(int zone);
void PROF_count(int counter, uint64_t n);
void PROF_gauge(int gauge, int64_t value);
void PROF_present(void); // right after the swap returns: frame pacing histogram
void PROF_frame(void);   // once per presented frame, just counts
void PROF_tick(void);    // at the top of the main loop, reports when a window is full
void PROF_log(const char* fmt, ...);  // free-form line into the same log

// Floating point against the core. A core's asm that writes d8-d15 (callee
// saved) or FPSCR without restoring them corrupts whatever double the frontend
// keeps in a register across retro_run. PROF_vfpCall records d8-d15 and FPSCR
// either side of the call and restores nothing, so minarch sees exactly what
// it would without the probe; PROF_vfpCheck compares, from a separate function
// so its own register saves can't mask anything. PROF_fp counts NaN/Inf by
// their bits (isnan() is folded away under -Ofast).
enum {
    PROF_FP_REFRESH,   // GFX_getRefreshRate(), Hz
    PROF_FP_SKEW,      // SND_updateClock: panel / core rate
    PROF_FP_RATIO,     // SND_updateClock: resampling ratio
    PROF_FP_FPS,       // trackFPS: fps_double
    PROF_FP_COUNT,
};
void PROF_vfpCall(void (*fn)(void));
void PROF_vfpCheck(void);
void PROF_fp(int site, double value);

#if defined(__arm__)
#define PROF_CORE_CALL(fn) do { PROF_vfpCall(fn); PROF_vfpCheck(); } while (0)
#else
#define PROF_CORE_CALL(fn) (fn)()
#endif
#define PROF_FP(s,v)      PROF_fp((s),(v))
#define PROF_BEGIN(z)     PROF_begin(z)
#define PROF_END(z)       PROF_end(z)
#define PROF_COUNT(c,n)   PROF_count((c),(n))
#define PROF_GAUGE(g,v)   PROF_gauge((g),(v))
#define PROF_PRESENT()    PROF_present()
#define PROF_FRAME()      PROF_frame()
#define PROF_TICK()       PROF_tick()
#define PROF_LOG(...)     PROF_log(__VA_ARGS__)

#else

#define PROF_CORE_CALL(fn) (fn)()
#define PROF_FP(s,v)      ((void)0)
#define PROF_BEGIN(z)     ((void)0)
#define PROF_END(z)       ((void)0)
#define PROF_COUNT(c,n)   ((void)0)
#define PROF_GAUGE(g,v)   ((void)0)
#define PROF_PRESENT()    ((void)0)
#define PROF_FRAME()      ((void)0)
#define PROF_TICK()       ((void)0)
#define PROF_LOG(...)     ((void)0)

#endif

#endif // __PROFILE_H__
