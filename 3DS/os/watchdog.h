#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <3ds.h>
#include "profiler.h"

// Background thread that reports when the game stops producing frames, with
// the last stub it called. Also flushes the log periodically.
void watchdog_start(void);
void watchdog_stop(void);

// Updated by the generated SDK stubs
extern volatile const char* g_last_stub;
extern volatile u32 g_stub_calls;

// Where the game thread is: set around the calls that can block (GPU sync,
// worker waits, file reads), reported when frames stop. A phase that stays
// set across a stall names the blocking call. Each thread slot (profiler.h)
// has its own phase, so g_main_phase on the render thread (gx/gx_rec.c) sets
// the render thread's phase and the game thread's stays readable.
extern volatile const char* g_wd_phase[THREAD_SLOTS];
#define g_main_phase (g_wd_phase[t_thread_slot])
#define WD_PHASE(name) (g_main_phase = (name))

#endif // WATCHDOG_H
