#include "profile.h"

#if PROFILE

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static const char* zone_names[PROF_ZONE_COUNT] = {
    "frame_total",
    "core_run",
    "video_refresh",
    "tex_game",
    "tex_ui",
    "draw",
    "gpu_finish",
    "swap",
};

typedef struct {
    uint64_t open;   // timestamp of the current PROF_begin, 0 when closed
    uint64_t total;
    uint64_t min;
    uint64_t max;
    uint32_t hits;
} Zone;

static Zone     zones[PROF_ZONE_COUNT];
static uint64_t counters[PROF_COUNTER_COUNT];
static uint32_t frames = 0;
static uint64_t window_start = 0;
static FILE*    logfile = NULL;

static uint64_t prof_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

void PROF_begin(int zone) {
    if (zone<0 || zone>=PROF_ZONE_COUNT) return;
    uint64_t now = prof_now();
    if (!window_start) window_start = now; // so fps covers the whole window
    zones[zone].open = now;
}

void PROF_end(int zone) {
    if (zone<0 || zone>=PROF_ZONE_COUNT) return;
    Zone* z = &zones[zone];
    if (!z->open) return; // never opened, ignore rather than record nonsense

    uint64_t dt = prof_now() - z->open;
    z->open = 0;
    z->total += dt;
    if (!z->hits || dt < z->min) z->min = dt;
    if (dt > z->max) z->max = dt;
    z->hits++;
}

void PROF_count(int counter, uint64_t n) {
    if (counter<0 || counter>=PROF_COUNTER_COUNT) return;
    counters[counter] += n;
}

// two sinks: stdout for when you can see it, and a file because launch.sh
// doesn't redirect minarch's output anywhere adb can reach
static void prof_emit(const char* line) {
    fputs(line, stdout);
    if (!logfile) logfile = fopen(PROFILE_LOG, "a");
    if (logfile) fputs(line, logfile);
}

void PROF_log(const char* fmt, ...) {
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    prof_emit(line);
    if (logfile) fflush(logfile);
}

static void prof_report(void) {
    char line[256];
    uint64_t elapsed = prof_now() - window_start;
    double   secs    = elapsed / 1e9;
    if (secs <= 0) secs = 1e-9;

    // Wall time per presented frame. Deliberately per frame, not per call:
    // a core that frameskips runs more main-loop iterations than it presents,
    // and every other zone here is already per presented frame. Dividing by
    // the per-call average instead would report shares above 100%.
    double frame_ms = frames ? (zones[PROF_FRAME_TOTAL].total / 1e6) / frames : 0;

    snprintf(line, sizeof(line),
             "\n=== minarch profile: %u frames in %.3fs = %.1f fps"
#if PROFILE_GLFINISH
             "  [glFinish: pipelining disabled]"
#endif
             " ===\n", frames, secs, frames / secs);
    prof_emit(line);

    snprintf(line, sizeof(line), "%-14s %8s %8s %8s %8s %7s %7s\n",
             "zone", "avg_ms", "min_ms", "max_ms", "per_fr", "%frame", "calls");
    prof_emit(line);

    for (int i=0; i<PROF_ZONE_COUNT; i++) {
        Zone* z = &zones[i];
        if (!z->hits) continue;

        double avg = (z->total / 1e6) / z->hits;        // per call
        double per = (z->total / 1e6) / (frames ? frames : 1); // per frame
        snprintf(line, sizeof(line), "%-14s %8.3f %8.3f %8.3f %8.3f %6.1f%% %7u\n",
                 zone_names[i], avg, z->min/1e6, z->max/1e6, per,
                 frame_ms>0 ? per*100.0/frame_ms : 0, z->hits);
        prof_emit(line);
    }

    double f = frames ? frames : 1;
    // iters/frame > 1 means the core is emulating frames it never presents,
    // ie. frameskip; emu_hz is the true emulated speed regardless of that
    double iters = zones[PROF_FRAME_TOTAL].hits;
    double emu_hz = zones[PROF_FRAME_TOTAL].total > 0
                  ? iters / (zones[PROF_FRAME_TOTAL].total / 1e9) : 0;
    snprintf(line, sizeof(line),
             "per frame: draws %.1f  uploaded %.1f KiB  vbo_rebuilds %.3f"
             "  iters/frame %.2f  emu %.1f Hz\n",
             counters[PROF_DRAWS]/f, counters[PROF_TEX_BYTES]/f/1024.0,
             counters[PROF_VBO_REBUILDS]/f, iters/f, emu_hz);
    prof_emit(line);

    if (logfile) fflush(logfile);
    fflush(stdout);

    memset(zones, 0, sizeof(zones));
    memset(counters, 0, sizeof(counters));
    frames = 0;
    window_start = 0; // re-armed by the next PROF_begin
}

void PROF_frame(void) {
    frames++;
}

// Reporting is deliberately split out of PROF_frame: a presented frame is
// counted from inside video_refresh_callback, which sits in the middle of the
// main loop iteration, so reporting there would throw away the frame_total
// span that is still open. PROF_tick runs at the top of the loop, where
// nothing is open and the window boundary is clean.
void PROF_tick(void) {
    if (frames >= PROFILE_FRAMES) prof_report();
}

#endif // PROFILE
