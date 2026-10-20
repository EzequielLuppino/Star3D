#include "profiler.h"
#include "log.h"
#include <string.h>

typedef struct {
    u64 total;   // Ticks for zones, sum for counters
    u64 max;     // Longest single call (zones)
    u32 calls;
} prof_acc_t;

typedef struct {
    const char* name;
    int is_zone;
    // Two windows: threads add to acc[s_buf] while the report reads the other
    // half, so no thread has to be idle for the report
    prof_acc_t acc[2][THREAD_SLOTS];  // Written only by threads of that slot
} prof_entry_t;

__thread int t_thread_slot;

static prof_entry_t entries[PROF_MAX_ENTRIES];
static volatile int entry_count = 0;
static LightLock register_lock = 1;  // Unlocked (LightLock_Init stores 1)

static unsigned int frame_count = 0;
static unsigned int window_frames = 0;
static u64 frame_start = 0;
static u64 window_start = 0;
static u64 window_frame_total = 0;
static u64 window_frame_max = 0;
// Frame time histogram: <=16.7 ms (60 fps), <=33.4 (30), <=50 (20), longer
static u32 window_hist[4];
static u64 total_frames_slow;  // Frames over 50 ms since boot

// Report without a stall (the overlapped frame end used to drain the render
// thread for every report, a ~15-20 ms frame every 3 s):
// - prof_frame_end flips s_buf and keeps the frame stats of the closed window;
// - from one frame later it writes that window's report, a few lines per
//   frame, then clears its half.
// By then no thread adds to the old half any more: the render thread finished
// every command recorded before the flip (the frame end's gxrec_fence waits
// for the previous boundary, which was recorded after the flip), and commands
// recorded after the flip were published after s_buf changed. Helper threads
// only race if one sleeps a whole frame inside prof_add.
// The render thread's half runs one frame later than the game thread's, which
// only matters for the window's first and last frame.
static volatile int s_buf;
// Formatting the ~100 report lines takes ~5 ms, so they are written a few
// per frame (the next window only closes after dozens of frames)
#define REPORT_LINES_PER_FRAME 12
typedef struct {
    bool pending;
    int next;  // Next entry to write, 0 = the fps line first
    int buf;
    unsigned int first, frames;
    double fps;
    u64 frame_total, frame_max;
    u32 hist[4];
} prof_window_t;
static prof_window_t s_closed;

static double ticks_to_ms(u64 ticks) {
    return (double)ticks / CPU_TICKS_PER_MSEC;
}

void prof_init(void) {
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    frame_count = 0;
    window_frames = 0;
    window_frame_total = 0;
    window_frame_max = 0;
    window_start = svcGetSystemTick();
}

// Threads register lazily (PROF_ZONE / PROF_COUNT), so two of them can ask for
// the same name at once: both must get the same entry
static int register_entry(const char* name, int is_zone) {
    LightLock_Lock(&register_lock);
    int id = 0;
    while (id < entry_count && strcmp(entries[id].name, name) != 0) id++;
    if (id == entry_count) {
        if (entry_count >= PROF_MAX_ENTRIES) {
            id = PROF_MAX_ENTRIES;  // Out of range: ignored by prof_add
        } else {
            entries[id].name = name;
            entries[id].is_zone = is_zone;
            __dmb();
            entry_count = id + 1;
        }
    }
    LightLock_Unlock(&register_lock);
    if (id == PROF_MAX_ENTRIES) LOG_ONCE(LOG_LEVEL_WARN, "prof", "too many profiler entries, dropping '%s'", name);
    return id;
}

int prof_register_zone(const char* name) {
    return register_entry(name, 1);
}

int prof_register_counter(const char* name) {
    return register_entry(name, 0);
}

void prof_add(int id, u64 value) {
    if (id < 0 || id >= entry_count) return;

    prof_acc_t* a = &entries[id].acc[s_buf][t_thread_slot];
    a->total += value;
    a->calls++;
    if (value > a->max) a->max = value;
}

prof_scope_t prof_scope_begin(int id) {
    prof_scope_t scope = { id, svcGetSystemTick() };
    return scope;
}

void prof_scope_end(prof_scope_t* scope) {
    prof_add(scope->id, svcGetSystemTick() - scope->start);
}

void prof_frame_begin(void) {
    frame_start = svcGetSystemTick();
    log_set_frame(frame_count);
}

unsigned int prof_frame_count(void) {
    return frame_count;
}

// Optional extra line after the fps report (savestate_3ds.c logs the player position)
__attribute__((weak)) void prof_report_extra(void) {}

// Closes the window: later prof_add calls go to the other half
static void close_window(void) {
    prof_window_t* w = &s_closed;
    u64 window_ticks = svcGetSystemTick() - window_start;
    w->buf = s_buf;
    w->first = frame_count - window_frames;
    w->frames = window_frames;
    w->fps = window_frames * 1000.0 / ticks_to_ms(window_ticks);
    w->frame_total = window_frame_total;
    w->frame_max = window_frame_max;
    memcpy(w->hist, window_hist, sizeof(w->hist));
    w->pending = true;
    w->next = 0;
    __dmb();
    s_buf = !s_buf;
    __dmb();  // Published before any later ring command (gx_rec.c publish)

    window_frames = 0;
    window_frame_total = 0;
    window_frame_max = 0;
    memset(window_hist, 0, sizeof(window_hist));
    window_start = svcGetSystemTick();
}

// Writes the next lines of the closed window's report; returns true when done
static bool write_closed_window(void) {
    prof_window_t* w = &s_closed;
    if (!w->pending) return true;
    PROF_ZONE("prof.report");  // Counted in the next window
    if (w->next == 0) {
        LOG_INFO("perf", "frames %u-%u: %.1f fps, frame avg %.2f ms max %.2f ms | <=17ms %lu <=33ms %lu <=50ms %lu "
                 "slower %lu",
                 w->first, w->first + w->frames - 1, w->fps,
                 ticks_to_ms(w->frame_total) / w->frames, ticks_to_ms(w->frame_max),
                 (unsigned long)w->hist[0], (unsigned long)w->hist[1], (unsigned long)w->hist[2],
                 (unsigned long)w->hist[3]);
        prof_report_extra();
    }

    int lines = 0;
    for (; w->next < entry_count && lines < REPORT_LINES_PER_FRAME; w->next++) {
        prof_entry_t* e = &entries[w->next];
        prof_acc_t sum = { 0, 0, 0 };
        for (int s = 0; s < THREAD_SLOTS; s++) {
            sum.total += e->acc[w->buf][s].total;
            sum.calls += e->acc[w->buf][s].calls;
            if (e->acc[w->buf][s].max > sum.max) sum.max = e->acc[w->buf][s].max;
        }
        if (sum.calls == 0) continue;
        lines++;

        if (e->is_zone) {
            LOG_DEBUG("prof", "  zone %-24s %6.2f ms/frame  %6lu calls  max %.2f ms",
                     e->name, ticks_to_ms(sum.total) / w->frames,
                     (unsigned long)sum.calls, ticks_to_ms(sum.max));
        } else {
            LOG_DEBUG("prof", "  count %-23s %8llu/frame  total %llu",
                     e->name, sum.total / w->frames, sum.total);
        }
    }
    if (w->next < entry_count) return false;

    log_kick();  // Not log_flush: a slow SD card write would stall the frame
    for (int i = 0; i < entry_count; i++) memset(entries[i].acc[w->buf], 0, sizeof(entries[i].acc[w->buf]));
    w->pending = false;
    return true;
}

void prof_report(void) {
    while (!write_closed_window()) {}  // A window closed earlier comes first
    if (window_frames == 0) return;
    close_window();
    while (!write_closed_window()) {}  // The caller has every thread idle
}

bool prof_report_due(void) {
    return window_frames + 1 >= PROF_REPORT_INTERVAL ||
           svcGetSystemTick() - window_start >= 3000ULL * CPU_TICKS_PER_MSEC;
}

void prof_frame_end(void) {
    u64 frame_ticks = svcGetSystemTick() - frame_start;
    window_frame_total += frame_ticks;
    if (frame_ticks > window_frame_max) window_frame_max = frame_ticks;
    double ms = ticks_to_ms(frame_ticks);
    window_hist[ms <= 16.7 ? 0 : ms <= 33.4 ? 1 : ms <= 50.0 ? 2 : 3]++;
    if (ms > 50.0) total_frames_slow++;

    frame_count++;
    window_frames++;

    // The window closed one frame ago is complete now (see s_buf)
    bool written = write_closed_window();
    // Report every PROF_REPORT_INTERVAL frames, or every 3 s when running
    // slowly (not before the previous report is written)
    if (written && (window_frames >= PROF_REPORT_INTERVAL ||
                    svcGetSystemTick() - window_start >= 3000ULL * CPU_TICKS_PER_MSEC)) {
        close_window();
    }
}
