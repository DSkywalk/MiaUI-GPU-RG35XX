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
    "audio_batch",
    "tex_game",
    "tex_ui",
    "draw",
    "gpu_finish",
    "swap",
    "ra_frame",
};

typedef struct {
    uint64_t open;   // timestamp of the current PROF_begin, 0 when closed
    uint64_t total;
    uint64_t min;
    uint64_t max;
    uint32_t hits;
} Zone;

typedef struct {
    int64_t  min;
    int64_t  max;
    int64_t  sum;
    uint32_t n;
} Gauge;

static Zone     zones[PROF_ZONE_COUNT];
static uint64_t counters[PROF_COUNTER_COUNT];
static Gauge    gauges[PROF_GAUGE_COUNT];

// Present-to-present intervals. Averages hide stutter: 59 frames at 16.7ms and
// one at 33ms average out to 17ms, and that one frame is what the player sees.
#define PRES_BUCKET_US 250
#define PRES_BUCKETS   200 // 0..50ms, the last bucket takes everything longer
static uint32_t pres_hist[PRES_BUCKETS + 1];
static uint32_t pres_n;
static uint64_t pres_max;
static uint64_t pres_last;
static uint32_t frames = 0;
static uint64_t window_start = 0;
static FILE*    logfile = NULL;

static void prof_emit(const char* line);

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
    // atomic because the audio callback counts from its own thread
    __atomic_add_fetch(&counters[counter], n, __ATOMIC_RELAXED);
}

void PROF_gauge(int gauge, int64_t value) {
    if (gauge<0 || gauge>=PROF_GAUGE_COUNT) return;
    Gauge* g = &gauges[gauge];
    if (!g->n || value < g->min) g->min = value;
    if (!g->n || value > g->max) g->max = value;
    g->sum += value;
    g->n++;
}

void PROF_present(void) {
    uint64_t t = prof_now();
    if (pres_last) {
        uint64_t dt = t - pres_last;
        uint64_t b  = dt / (PRES_BUCKET_US * 1000ull);
        pres_hist[b > PRES_BUCKETS ? PRES_BUCKETS : b]++;
        pres_n++;
        if (dt > pres_max) pres_max = dt;
    }
    pres_last = t;
}

// upper edge of the bucket holding the q-th fraction of intervals, in ms
static double pres_quantile(double q) {
    uint32_t want = (uint32_t)(q * pres_n), seen = 0;
    for (int b = 0; b <= PRES_BUCKETS; b++) {
        seen += pres_hist[b];
        if (seen > want) return (b + 1) * PRES_BUCKET_US / 1000.0;
    }
    return PRES_BUCKETS * PRES_BUCKET_US / 1000.0;
}

static void pres_report(void) {
    char line[512];
    if (pres_n < 2) return;
    double p50 = pres_quantile(0.50);
    uint32_t hitches = 0;
    for (int b = 0; b <= PRES_BUCKETS; b++) {
        if ((b + 1) * PRES_BUCKET_US / 1000.0 > p50 * 1.5) hitches += pres_hist[b];
    }
    snprintf(line, sizeof(line),
             "present: %u intervals  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  hitches %u (>1.5x p50)\n",
             pres_n, p50, pres_quantile(0.95), pres_quantile(0.99), pres_max / 1e6, hitches);
    prof_emit(line);

    // the non-empty buckets, so a bimodal 16.7/33.4 split is visible at a glance
    int len = snprintf(line, sizeof(line), "present hist (ms:count):");
    for (int b = 0; b <= PRES_BUCKETS && len < (int)sizeof(line) - 24; b++) {
        if (!pres_hist[b]) continue;
        len += snprintf(line + len, sizeof(line) - len, " %s%.2f:%u",
                        b == PRES_BUCKETS ? ">" : "", (b + 1) * PRES_BUCKET_US / 1000.0, pres_hist[b]);
    }
    snprintf(line + len, sizeof(line) - len, "\n");
    prof_emit(line);
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
             "  iters/frame %.2f  emu %.1f Hz  audio %.0f frames/frame\n",
             counters[PROF_DRAWS]/f, counters[PROF_TEX_BYTES]/f/1024.0,
             counters[PROF_VBO_REBUILDS]/f, iters/f, emu_hz,
             counters[PROF_AUDIO_FRAMES]/f);
    prof_emit(line);

    // core_run wraps every libretro callback, so it bills the core for our
    // upload, swap and audio wait. Take those out to see what the core costs.
    {
        double inside = 0;
        static const int callbacks[] = { PROF_VIDEO, PROF_AUDIO, PROF_TEX_GAME, PROF_TEX_UI,
                                         PROF_DRAW, PROF_GPU_FINISH, PROF_SWAP };
        for (unsigned i=0; i<sizeof(callbacks)/sizeof(callbacks[0]); i++) inside += zones[callbacks[i]].total;
        double emu_only = (zones[PROF_CORE_RUN].total - inside) / 1e6 / iters;
        Gauge* r = &gauges[PROF_G_RING_US];
        Gauge* q = &gauges[PROF_G_RATE_PPM];
        snprintf(line, sizeof(line),
                 "derived: emulation only %.3f ms/iter  audio ring %.1f/%.1f/%.1f ms (min/avg/max)"
                 "  sleeps %.1f/frame  starved %llu frames  rate %+.0f/%+.0f/%+.0f ppm\n",
                 iters ? emu_only : 0,
                 r->n ? r->min / 1000.0 : 0, r->n ? (double)r->sum / r->n / 1000.0 : 0, r->n ? r->max / 1000.0 : 0,
                 counters[PROF_AUDIO_SLEEPS]/f, (unsigned long long)counters[PROF_AUDIO_STARVED],
                 q->n ? (double)q->min : 0, q->n ? (double)q->sum / q->n : 0, q->n ? (double)q->max : 0);
        prof_emit(line);
    }
    {
        Gauge* g = &gauges[PROF_G_REFRESH_MHZ];
        if (g->n) {
            snprintf(line, sizeof(line), "refresh estimate: %.3f/%.3f Hz (min/max)\n", g->min / 1000.0, g->max / 1000.0);
            prof_emit(line);
        }
    }
    pres_report();

    if (logfile) fflush(logfile);
    fflush(stdout);

    memset(zones, 0, sizeof(zones));
    memset(counters, 0, sizeof(counters));
    memset(gauges, 0, sizeof(gauges));
    memset(pres_hist, 0, sizeof(pres_hist));
    pres_n = 0;
    pres_max = 0;
    pres_last = 0; // an interval spanning the report isn't a frame's
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
    if (frames >= PROFILE_FRAMES) { prof_report(); return; }

    // A window that has been open a long time with few frames in it is the
    // interesting case, not one to wait out -- report on elapsed time too.
    if (window_start) {
        uint64_t elapsed = prof_now() - window_start;
        if (elapsed >= (uint64_t)PROFILE_SECONDS * 1000000000ULL) prof_report();
    }
}

#endif // PROFILE
