#include "watchdog.h"
#include <3ds.h>
#include "crash.h"
#include "log.h"
#include "profiler.h"
#include "worker.h"

// Runs at a higher priority than the game thread so it still gets scheduled
// while the game spins in a loop (the 3DS app core does not time-slice
// threads of equal priority).
#define WATCHDOG_PRIORITY  0x18
#define WATCHDOG_PERIOD_NS (2000ULL * 1000 * 1000)

volatile const char* g_last_stub = "(none)";
volatile u32 g_stub_calls;
volatile const char* g_wd_phase[THREAD_SLOTS] = { "startup", "(not started)", "(not started)" };

static Thread watchdog_thread;
static volatile bool watchdog_running;

#define POLL_NS   (100ULL * 1000 * 1000)
#define POLLS_PER_CHECK ((int)(WATCHDOG_PERIOD_NS / POLL_NS))

// No GPU register read here: on hardware (bug 12), GSPGPU_ReadHWRegs while
// the GPU hung froze the system (the log stopped right after the call, in two
// runs, once from a separate thread). S3D_GPU_TRACE finds the hanging draw instead.

static void watchdog_entry(void* arg) {
    (void)arg;
    crash_install_handler();
    unsigned int last_frame = prof_frame_count();
    u32 last_calls = g_stub_calls;
    int stalled_checks = 0;
    int polls = 0;

    while (watchdog_running) {
        svcSleepThread(POLL_NS);
        if (!watchdog_running) break;

        // Stuck for 6 s or more: START+SELECT quits from here, since the game
        // thread no longer reaches its own check (and HOME does nothing)
        if (stalled_checks >= 3) {
            hidScanInput();
            if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) {
                LOG_ERROR("watchdog", "START+SELECT while stuck in \"%s\": exiting the process",
                          (const char*)g_wd_phase[THREAD_SLOT_GAME]);
                log_flush();
                svcExitProcess();
            }
        }
        if (++polls < POLLS_PER_CHECK) continue;
        polls = 0;

        unsigned int frame = prof_frame_count();
        u32 calls = g_stub_calls;
        if (frame == last_frame) {
            stalled_checks++;
            u32 queued, running_ms;
            void* running_fn;
            worker_debug_state(&queued, &running_ms, &running_fn);
            LOG_WARN("watchdog", "no frame for %d s (stuck at frame %u) in \"%s\": last stub %s, %lu stub calls since last check; "
                     "worker: %lu queued, %s%p for %lu ms; render thread: \"%s\"",
                     stalled_checks * 2, frame, (const char*)g_wd_phase[THREAD_SLOT_GAME], (const char*)g_last_stub,
                     (unsigned long)(calls - last_calls), (unsigned long)queued,
                     running_fn ? "running " : "idle ", running_fn, (unsigned long)running_ms,
                     (const char*)g_wd_phase[THREAD_SLOT_RENDER]);
            if (stalled_checks == 3) LOG_WARN("watchdog", "hold START+SELECT to quit");
        } else {
            stalled_checks = 0;
        }
        last_frame = frame;
        last_calls = calls;
        log_flush();
    }
}

void watchdog_start(void) {
    watchdog_running = true;
    watchdog_thread = threadCreate(watchdog_entry, NULL, 0x2000, WATCHDOG_PRIORITY, -2, false);
    if (!watchdog_thread) {
        LOG_WARN("watchdog", "could not create watchdog thread");
        watchdog_running = false;
    }
}

void watchdog_stop(void) {
    if (!watchdog_thread) return;
    watchdog_running = false;
    threadJoin(watchdog_thread, U64_MAX);
    threadFree(watchdog_thread);
    watchdog_thread = NULL;
}
