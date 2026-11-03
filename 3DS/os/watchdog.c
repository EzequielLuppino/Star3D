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
volatile u32 g_wd_obj_defno;
extern volatile u32 g_log_commit_active;
volatile const char* g_wd_phase[THREAD_SLOTS] = { "startup", "(not started)", "(not started)" };

static Thread watchdog_thread;
static volatile bool watchdog_running;

#define POLL_NS   (100ULL * 1000 * 1000)
#define POLLS_PER_CHECK ((int)(WATCHDOG_PERIOD_NS / POLL_NS))

// The game runs on the main thread (main.c calls gameMain directly); the
// watchdog reads its saved context while it is stuck (same process, so the
// context svc may be allowed). 0 = capture failed or not permitted.
static Handle s_game_thread;

static inline Result svc_get_thread_context(ThreadContext* ctx, Handle thread) {
    register u32 r0 asm("r0") = (u32)(uintptr_t)ctx;
    register u32 r1 asm("r1") = thread;
    asm volatile("svc 0x3B" : "+r"(r0) : "r"(r1) : "r2", "r3", "r12", "memory");
    return r0;
}

static void dump_game_context(void) {
    if (!s_game_thread) return;
    ThreadContext ctx;
    Result rc = svc_get_thread_context(&ctx, s_game_thread);
    if (R_FAILED(rc)) {
        LOG_WARN("watchdog", "ctx game: 0x%08lX", (unsigned long)rc);
        return;
    }
    CpuRegisters* c = &ctx.cpu_registers;
    LOG_WARN("watchdog",
             "game ctx: pc %08lX lr %08lX sp %08lX cpsr %08lX | r0 %08lX r1 %08lX r2 %08lX r3 %08lX r4 %08lX r5 "
             "%08lX r12 %08lX",
             (unsigned long)c->pc, (unsigned long)c->lr, (unsigned long)c->sp, (unsigned long)c->cpsr,
             (unsigned long)c->r[0], (unsigned long)c->r[1], (unsigned long)c->r[2], (unsigned long)c->r[3],
             (unsigned long)c->r[4], (unsigned long)c->r[5], (unsigned long)c->r[12]);
    // Stack words: return addresses into .text show the call chain, as in the
    // crash handler (crash.c)
    u32 sp = c->sp & ~3u;
    for (int row = 0; row < 10; row++) {
        u32 addr = sp + row * 16;
        MemInfo mi;
        PageInfo pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) || (mi.perm & MEMPERM_READ) == 0 || mi.state == MEMSTATE_FREE)
            break;
        const u32* w = (const u32*)addr;
        LOG_WARN("watchdog", "game stack %08lX: %08lX %08lX %08lX %08lX", (unsigned long)addr, (unsigned long)w[0],
                 (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3]);
    }
}

extern Handle port_audio_stream_thread_handle(void);
extern Handle log_writer_thread_handle(void);
extern Handle render_thread_handle(void);
extern Handle worker_thread_handle(void);

static void dump_one(const char* name, Handle h) {
    if (!h) return;
    ThreadContext ctx;
    Result rc = svc_get_thread_context(&ctx, h);
    if (R_FAILED(rc)) {
        LOG_WARN("watchdog", "ctx %s: 0x%08lX", name, (unsigned long)rc);
        return;
    }
    CpuRegisters* c = &ctx.cpu_registers;
    LOG_WARN("watchdog", "ctx %s: pc %08lX lr %08lX sp %08lX cpsr %08lX", name, (unsigned long)c->pc,
             (unsigned long)c->lr, (unsigned long)c->sp, (unsigned long)c->cpsr);
    u32 sp = c->sp & ~3u;
    for (int row = 0; row < 3; row++) {
        u32 addr = sp + row * 16;
        MemInfo mi;
        PageInfo pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) || (mi.perm & MEMPERM_READ) == 0 || mi.state == MEMSTATE_FREE)
            break;
        const u32* w = (const u32*)addr;
        LOG_WARN("watchdog", "ctx %s stack %08lX: %08lX %08lX %08lX %08lX", name, (unsigned long)addr,
                 (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3]);
    }
}

static void dump_all_contexts(void) {
    dump_game_context();
    dump_one("stream", port_audio_stream_thread_handle());
    dump_one("logwriter", log_writer_thread_handle());
    dump_one("render", render_thread_handle());
    dump_one("worker", worker_thread_handle());
}

static Thread starve_thread;
static volatile bool starve_running;
static volatile u32 s_ss_samples, s_ss_late, s_ss_late_ms, s_ss_max_ms;

static void starve_entry(void* arg) {
    (void)arg;
    u64 prev = svcGetSystemTick();
    while (starve_running) {
        svcSleepThread(1000000LL);
        u64 now = svcGetSystemTick();
        u32 d = (u32)((now - prev) / (SYSCLOCK_ARM11 / 1000));
        prev = now;
        s_ss_samples++;
        if (d > 3) {
            s_ss_late++;
            s_ss_late_ms += d;
            if (d > s_ss_max_ms) s_ss_max_ms = d;
        }
    }
}

// No GPU register read here: on hardware (bug 12), GSPGPU_ReadHWRegs while
// the GPU hung froze the system (the log stopped right after the call, in two
// runs, once from a separate thread). S3D_GPU_TRACE finds the hanging draw instead.

static void watchdog_entry(void* arg) {
    (void)arg;
    crash_install_handler();
    // Its own TLS slot: WD_PHASE writes from here (log_flush) must not clobber
    // the game thread's phase, which names the stall
    t_thread_slot = THREAD_SLOT_HELPER;
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
                     "worker: %lu queued, %s%p for %lu ms; render thread: \"%s\"; log commit: %s; last obj defNo %lu; "
                     "core0 sampler: %lu/%lu late (max %lu ms, total %lu ms)",
                     stalled_checks * 2, frame, (const char*)g_wd_phase[THREAD_SLOT_GAME], (const char*)g_last_stub,
                     (unsigned long)(calls - last_calls), (unsigned long)queued,
                     running_fn ? "running " : "idle ", running_fn, (unsigned long)running_ms,
                     (const char*)g_wd_phase[THREAD_SLOT_RENDER],
                     g_log_commit_active ? "active" : "idle", (unsigned long)g_wd_obj_defno,
                     (unsigned long)s_ss_late, (unsigned long)s_ss_samples, (unsigned long)s_ss_max_ms,
                     (unsigned long)s_ss_late_ms);
            dump_all_contexts();
            if (stalled_checks == 3) LOG_WARN("watchdog", "hold START+SELECT to quit");
        } else {
            stalled_checks = 0;
        }
        last_frame = frame;
        last_calls = calls;
        // Flush rarely (every 5th check, ~10 s): each commit stalls the SD
        // card for seconds and delays every game DVD read issued during it
        // (frame pacing hitches). The writer flushes on warnings, and the
        // crash handler and the stuck-path exit above flush everything.
        // Never while stalled: the flush would block this thread behind the
        // writer's commit and hide the next stall check.
        static unsigned flushes;
        if (stalled_checks == 0 && ++flushes % 5 == 0) log_flush();
    }
}

void watchdog_start(void) {
    s_game_thread = threadGetHandle(NULL);  // The main thread: the game runs on it
    watchdog_running = true;
    watchdog_thread = threadCreate(watchdog_entry, NULL, 0x2000, WATCHDOG_PRIORITY, -2, false);
    if (!watchdog_thread) {
        LOG_WARN("watchdog", "could not create watchdog thread");
        watchdog_running = false;
        return;
    }
    starve_running = true;
    starve_thread = threadCreate(starve_entry, NULL, 0x1000, 0x10, 0, false);
}

void watchdog_stop(void) {
    if (starve_thread) {
        starve_running = false;
        threadJoin(starve_thread, U64_MAX);
        threadFree(starve_thread);
        starve_thread = NULL;
    }
    if (!watchdog_thread) return;
    watchdog_running = false;
    threadJoin(watchdog_thread, U64_MAX);
    threadFree(watchdog_thread);
    watchdog_thread = NULL;
}
