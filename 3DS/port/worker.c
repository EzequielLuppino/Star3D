#include "worker.h"
#include <3ds.h>
#include <string.h>
#include "config.h"
#include "crash.h"
#include "log.h"
#include "profiler.h"
#include "watchdog.h"

#define QUEUE_SIZE 128
#define JOB_IDLE    0
#define JOB_QUEUED  1
#define JOB_RUNNING 2
#define JOB_DONE    3

static WorkerJob* s_queue[QUEUE_SIZE];
static u32 s_head, s_tail;  // s_head: next to run, s_tail: next free
static LightLock s_lock;
static LightEvent s_wake;   // Signalled when a job is queued
static LightEvent s_done;   // Pulsed when a job finishes
static Thread s_thread;
static volatile bool s_running;
static int s_core = -1;
static u64 s_busy_ticks;
static volatile u64 s_job_start;   // Tick the running job started, 0 = idle
static volatile WorkerFn s_job_fn;

static void worker_main(void* arg) {
    (void)arg;
    crash_install_handler();
    t_thread_slot = THREAD_SLOT_HELPER;
    while (s_running) {
        LightLock_Lock(&s_lock);
        WorkerJob* job = s_head != s_tail ? s_queue[s_head % QUEUE_SIZE] : NULL;
        if (job) s_head++;
        LightLock_Unlock(&s_lock);
        if (!job) {
            LightEvent_Wait(&s_wake);
            continue;
        }
        job->state = JOB_RUNNING;
        u64 t0 = svcGetSystemTick();
        s_job_fn = job->fn;
        s_job_start = t0;
        job->fn(job->arg);
        s_job_start = 0;
        s_busy_ticks += svcGetSystemTick() - t0;
        __dmb();
        job->state = JOB_DONE;
        LightEvent_Pulse(&s_done);
    }
}

// Core 1 runs system threads too; the application only gets the share of its
// time granted here (APT_SetAppCpuTimeLimit, 5-80%). Off by default: on a New
// 3DS XL any share slows the system services on core 1 (GSP, DSP, FS) even
// while our threads there are idle. Hub frame by limit: 0% 57.3 ms, 30%
// 60.0 ms, 80% 74.0 ms; GSPGPU_FlushDataCache 0.13 / 0.25 / 0.97 ms per call
// (MULTICORE_PLAN.md, "A/B results"). With S3D_CORE1=0 the worker runs on
// core 2, the MusyX ticks in the ndsp callback and skinning inline.
// S3D_CORE1=1 puts the helpers on core 1, S3D_CORE1_LIMIT sets the share.
bool core1_init(void) {
    static int state;  // 0 not tried, 1 usable, -1 not
    if (state) return state > 0;
    state = -1;
    if (!cfg_bool("S3D_CORE1", false)) {
        LOG_INFO("core1", "not used (S3D_CORE1=0)");
        return false;
    }
    int limit = cfg_int("S3D_CORE1_LIMIT", 80);
    if (limit < 5 || limit > 80) limit = 80;
    Result rc = APT_SetAppCpuTimeLimit((u32)limit);
    LOG_INFO("core1", "core 1 time limit %d%%: %s (0x%08lX)", limit, R_SUCCEEDED(rc) ? "ok" : "failed", rc);
    if (R_FAILED(rc)) return false;
    state = 1;
    return true;
}

void worker_init(void) {
    LightLock_Init(&s_lock);
    LightEvent_Init(&s_wake, RESET_ONESHOT);
    LightEvent_Init(&s_done, RESET_PULSE);
    s_head = s_tail = 0;

    if (!cfg_bool("S3D_WORKER", true)) {
        LOG_INFO("worker", "disabled (S3D_WORKER=0): jobs run inline");
        return;
    }

    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_running = true;
    // Core 1 first when it is used (S3D_CORE1=1), else core 2. Below the
    // render thread (gx/gx_rec.c) and the other core 1 helpers (audio,
    // skinning), which are time-critical; the worker mostly waits for the SD
    // card.
    int first = cfg_int("S3D_WORKER_CORE", 1) == 2 ? 2 : 1;
    int cores[] = { first, 3 - first };
    for (unsigned i = 0; i < sizeof(cores) / sizeof(cores[0]) && !s_thread; i++) {
        if (cores[i] == 2 && !cfg_bool("S3D_USE_CORE2", true)) continue;
        if (cores[i] == 1 && !core1_init()) continue;
        s_thread = threadCreate(worker_main, NULL, 64 * 1024, prio - 1, cores[i], false);
        if (s_thread) s_core = cores[i];
    }
    if (!s_thread) {
        s_running = false;
        LOG_WARN("worker", "no second core available: jobs run inline on the game thread");
    } else {
        LOG_INFO("worker", "worker thread on core %d (priority 0x%lX)", s_core, (unsigned long)(prio - 1));
    }
}

void worker_shutdown(void) {
    if (!s_thread) return;
    s_running = false;
    LightEvent_Signal(&s_wake);
    threadJoin(s_thread, 1000000000ULL);
    threadFree(s_thread);
    s_thread = NULL;
}

int worker_core(void) { return s_core; }

void worker_submit(WorkerJob* job, WorkerFn fn, void* arg) {
    job->fn = fn;
    job->arg = arg;
    if (!s_thread) {
        job->state = JOB_RUNNING;
        fn(arg);
        job->state = JOB_DONE;
        return;
    }
    job->state = JOB_QUEUED;
    for (;;) {
        LightLock_Lock(&s_lock);
        if (s_tail - s_head < QUEUE_SIZE) {
            s_queue[s_tail % QUEUE_SIZE] = job;
            s_tail++;
            LightLock_Unlock(&s_lock);
            break;
        }
        LightLock_Unlock(&s_lock);
        PROF_COUNT("worker.queue_full", 1);
        LightEvent_Wait(&s_done);
    }
    LightEvent_Signal(&s_wake);
    PROF_COUNT("worker.jobs", 1);
}

bool worker_done(const WorkerJob* job) {
    return job->state == JOB_DONE || job->state == JOB_IDLE;
}

void worker_wait(WorkerJob* job) {
    if (worker_done(job)) return;
    PROF_ZONE("worker.wait");
    volatile const char* phase = g_main_phase;
    WD_PHASE("worker_wait");
    while (!worker_done(job)) LightEvent_WaitTimeout(&s_done, 2000000LL);  // 2 ms
    g_main_phase = phase;
}

void worker_debug_state(u32* queued, u32* running_ms, void** running_fn) {
    u64 start = s_job_start;
    *queued = s_tail - s_head;
    *running_ms = start ? (u32)((svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC) : 0;
    *running_fn = start ? (void*)s_job_fn : NULL;
}

// Reported through the profiler once per report window (see vi_3ds.c)
u64 worker_take_busy_ticks(void) {
    u64 t = s_busy_ticks;
    s_busy_ticks = 0;
    return t;
}
