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
    PROF_ZONE_COUNT,
};

enum {
    PROF_DRAWS,        // draw calls
    PROF_TEX_BYTES,    // bytes pushed through glTexSubImage2D/glTexImage2D
    PROF_VBO_REBUILDS, // geometry uploads
    PROF_AUDIO_FRAMES, // audio frames handed to SND_batchSamples
    PROF_COUNTER_COUNT,
};

void PROF_begin(int zone);
void PROF_end(int zone);
void PROF_count(int counter, uint64_t n);
void PROF_frame(void);   // once per presented frame, just counts
void PROF_tick(void);    // at the top of the main loop, reports when a window is full
void PROF_log(const char* fmt, ...);  // free-form line into the same log

#define PROF_BEGIN(z)     PROF_begin(z)
#define PROF_END(z)       PROF_end(z)
#define PROF_COUNT(c,n)   PROF_count((c),(n))
#define PROF_FRAME()      PROF_frame()
#define PROF_TICK()       PROF_tick()
#define PROF_LOG(...)     PROF_log(__VA_ARGS__)

#else

#define PROF_BEGIN(z)     ((void)0)
#define PROF_END(z)       ((void)0)
#define PROF_COUNT(c,n)   ((void)0)
#define PROF_FRAME()      ((void)0)
#define PROF_TICK()       ((void)0)
#define PROF_LOG(...)     ((void)0)

#endif

#endif // __PROFILE_H__
