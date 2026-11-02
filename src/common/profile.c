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

// The layout prof_vfpCall's asm writes: before at 0, after at 64, fpscr at 128/132.
typedef struct {
    uint64_t before[8], after[8];
    uint32_t fpscr_before, fpscr_after;
} ProfVfp;
static ProfVfp prof_vfp;

static uint32_t vfp_calls, vfp_changed, vfp_mask, fpscr_changed, fpscr_seen_in, fpscr_seen_out;
// limitFF's 1000000.0, which GCC keeps in a callee-saved register across
// core.run: frames it went in to the core, and frames it came back altered
#define PROF_FF_CONST 0x412E848000000000ull
static uint32_t ff_seen, ff_bad, ff_reg;
static uint32_t fp_bad[PROF_FP_COUNT], fp_n[PROF_FP_COUNT];
static uint64_t fp_bad_bits[PROF_FP_COUNT];
static const char* fp_names[PROF_FP_COUNT] = { "refresh", "skew", "ratio", "fps" };

// r0 = the call, r1 = the ProfVfp to fill. The address comes in as an argument
// because LTO can't see a symbol referenced from inside asm, and drops it.
void prof_vfpCall(void (*fn)(void), ProfVfp* out);
#if defined(__arm__)
__asm__(
    ".text\n"
    ".align 2\n"
    ".arm\n"
    ".fpu neon\n"
    ".global prof_vfpCall\n"
    ".type prof_vfpCall, %function\n"
    "prof_vfpCall:\n"
    "    push    {r4, lr}\n"           // 8 bytes: the stack stays 8-aligned
    "    mov     r4, r1\n"
    "    vstmia  r4, {d8-d15}\n"
    "    vmrs    r1, fpscr\n"
    "    str     r1, [r4, #128]\n"
    "    blx     r0\n"                 // r4 is callee saved, so still the ProfVfp
    "    add     r1, r4, #64\n"
    "    vstmia  r1, {d8-d15}\n"
    "    vmrs    r1, fpscr\n"
    "    str     r1, [r4, #132]\n"
    "    pop     {r4, pc}\n"
    ".size prof_vfpCall, .-prof_vfpCall\n"
);
#else
void prof_vfpCall(void (*fn)(void), ProfVfp* out) { (void)out; fn(); }
#endif

// No frame of its own around the call, so nothing of ours saves and restores
// d8-d15 over the core: a tail call straight into the asm.
void PROF_vfpCall(void (*fn)(void)) {
    prof_vfpCall(fn, &prof_vfp);
}

void PROF_vfpCheck(void) {
    vfp_calls++;
    uint32_t mask = 0;
    for (int i=0; i<8; i++) if (prof_vfp.before[i] != prof_vfp.after[i]) mask |= 1u << i;
    if (mask) { vfp_changed++; vfp_mask |= mask; }
    for (int i=0; i<8; i++) {
        if (prof_vfp.before[i] != PROF_FF_CONST) continue;
        ff_seen++; ff_reg = 8 + i;
        if (prof_vfp.after[i] != PROF_FF_CONST) ff_bad++;
    }
    if (prof_vfp.fpscr_before != prof_vfp.fpscr_after) fpscr_changed++;
    fpscr_seen_in  = prof_vfp.fpscr_before;
    fpscr_seen_out = prof_vfp.fpscr_after;
}

void PROF_fp(int site, double value) {
    if (site<0 || site>=PROF_FP_COUNT) return;
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    fp_n[site]++;
    if (((bits >> 52) & 0x7ff) == 0x7ff) { fp_bad[site]++; fp_bad_bits[site] = bits; } // NaN or Inf
}

static void vfp_report(void) {
    char line[512];
    int len = snprintf(line, sizeof(line),
             "vfp: core_run %u calls, d8-d15 changed in %u (regs d8+ mask 0x%02x), fpscr changed %u (in 0x%08x out 0x%08x)  nonfinite:",
             vfp_calls, vfp_changed, vfp_mask, fpscr_changed, fpscr_seen_in, fpscr_seen_out);
    for (int i=0; i<PROF_FP_COUNT && len < (int)sizeof(line) - 48; i++) {
        len += snprintf(line + len, sizeof(line) - len, " %s %u/%u", fp_names[i], fp_bad[i], fp_n[i]);
        if (fp_bad[i]) len += snprintf(line + len, sizeof(line) - len, " (0x%016llx)", (unsigned long long)fp_bad_bits[i]);
    }
    if (ff_seen) len += snprintf(line + len, sizeof(line) - len, "  ff 1e6 in d%u: corrupted %u/%u", ff_reg, ff_bad, ff_seen);
    else         len += snprintf(line + len, sizeof(line) - len, "  ff 1e6: not in d8-d15");
    snprintf(line + len, sizeof(line) - len, "\n");
    prof_emit(line);
    ff_seen = ff_bad = 0;
    vfp_calls = vfp_changed = vfp_mask = fpscr_changed = 0;
    memset(fp_bad, 0, sizeof(fp_bad));
    memset(fp_n, 0, sizeof(fp_n));
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
    vfp_report();

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
