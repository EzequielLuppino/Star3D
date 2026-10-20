#ifndef PROFILER_H
#define PROFILER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <3ds.h>

// Lightweight frame profiler. Zones measure time with the ARM11 tick counter,
// counters accumulate values (draw calls, vertices, ...). Every
// PROF_REPORT_INTERVAL frames a summary is written to the log (file and
// emulator output only, at debug level, to keep the console readable).
#define PROF_MAX_ENTRIES     512  // 256 was full by build B0083CA4: later counters were dropped
#define PROF_REPORT_INTERVAL 300  // ~5 s at 60 fps

// Which kind of thread is running: entries keep one accumulator per slot, so
// the game thread, the render thread (gx/gx_rec.c) and the helper threads
// never update the same counter at the same time. The report adds the slots.
// The watchdog keeps one phase per slot too (watchdog.h).
enum { THREAD_SLOT_GAME, THREAD_SLOT_RENDER, THREAD_SLOT_HELPER, THREAD_SLOTS };
extern __thread int t_thread_slot;  // 0 (game) unless the thread sets it

void prof_init(void);
void prof_frame_begin(void);
void prof_frame_end(void);
// True when the next prof_frame_end closes the window. Its report is written
// one frame later, without waiting for other threads (profiler.c, s_buf)
bool prof_report_due(void);
unsigned int prof_frame_count(void);
// Write the current window's stats to the log now. Only with every thread
// that counts idle (diag_3ds.c drains the render thread first)
void prof_report(void);

int prof_register_zone(const char* name);
int prof_register_counter(const char* name);
void prof_add(int id, u64 value);

typedef struct {
    int id;
    u64 start;
} prof_scope_t;

prof_scope_t prof_scope_begin(int id);
void prof_scope_end(prof_scope_t* scope);

#define PROF_CAT_(a, b) a##b
#define PROF_CAT(a, b)  PROF_CAT_(a, b)

// Time the rest of the enclosing block: { PROF_ZONE("render"); ... }
#define PROF_ZONE(name) \
    static int PROF_CAT(prof_zone_id_, __LINE__) = -1; \
    if (PROF_CAT(prof_zone_id_, __LINE__) < 0) \
        PROF_CAT(prof_zone_id_, __LINE__) = prof_register_zone(name); \
    prof_scope_t PROF_CAT(prof_scope_, __LINE__) __attribute__((cleanup(prof_scope_end))) = \
        prof_scope_begin(PROF_CAT(prof_zone_id_, __LINE__))

// Add to a named counter: PROF_COUNT("gx.vertices", n);
#define PROF_COUNT(name, value) do { \
        static int prof_counter_id_ = -1; \
        if (prof_counter_id_ < 0) prof_counter_id_ = prof_register_counter(name); \
        prof_add(prof_counter_id_, (value)); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif // PROFILER_H
