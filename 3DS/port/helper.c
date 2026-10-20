#include "helper.h"
#include <3ds.h>
#include "config.h"
#include "crash.h"
#include "log.h"
#include "profiler.h"
#include "worker.h"

// One batch at a time. s_claim holds the batch generation (top 12 bits) and
// the next unclaimed index (low 20 bits): a claim is a compare-and-swap on it, so a
// helper still holding an old batch's generation can never take an index of
// the next batch. The caller only starts a new batch after every job of the
// previous one is done.
#define GEN_SHIFT 20
#define GEN_MASK 0xFFFu
#define INDEX_MASK ((1u << GEN_SHIFT) - 1)

static Thread s_thread;
static LightEvent s_start;     // Helper: a batch was published
static LightEvent s_finished;  // Caller: the last job of the batch is done
static volatile u32 s_claim;
static volatile u32 s_done;
static volatile bool s_caller_waiting;
static HelperFn volatile s_fn;
static void* volatile s_ctx;
static volatile int s_count;
static u32 s_gen;

// Runs jobs of batch gen until none are left (returns at once when the batch has moved on)
static void run_jobs(u32 gen, HelperFn fn, void* ctx, int count) {
    for (;;) {
        u32 v = s_claim;
        u32 idx = v & INDEX_MASK;
        if ((v >> GEN_SHIFT) != gen || idx >= (u32)count) return;
        if (!__atomic_compare_exchange_n(&s_claim, &v, v + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
        fn(ctx, (int)idx);
        u32 done = __atomic_add_fetch(&s_done, 1, __ATOMIC_ACQ_REL);
        if (done == (u32)count && s_caller_waiting) LightEvent_Signal(&s_finished);
    }
}

static void helper_main(void* arg) {
    (void)arg;
    crash_install_handler();
    t_thread_slot = THREAD_SLOT_HELPER;
    for (;;) {
        LightEvent_Wait(&s_start);
        u32 v = __atomic_load_n(&s_claim, __ATOMIC_ACQUIRE);
        // fn, ctx and count are written before s_claim is published
        run_jobs(v >> GEN_SHIFT, s_fn, s_ctx, s_count);
    }
}

void helper_init(void) {
    LightEvent_Init(&s_start, RESET_ONESHOT);
    LightEvent_Init(&s_finished, RESET_ONESHOT);
    if (!cfg_bool("S3D_HELPER", true) || !core1_init()) {
        LOG_INFO("helper", "no core 1 helper: jobs run inline");
        return;
    }
    // Above the worker on core 1: the game thread may be waiting for its jobs
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_thread = threadCreate(helper_main, NULL, 16 * 1024, prio - 4, 1, true);
    if (s_thread) LOG_INFO("helper", "helper thread on core 1 (priority 0x%lX)", (unsigned long)(prio - 4));
    else LOG_WARN("helper", "cannot create the core 1 helper thread: jobs run inline");
}

bool helper_available(void) { return s_thread != NULL; }

void helper_parallel(HelperFn fn, void* ctx, int count) {
    if (count <= 0) return;
    if (!s_thread || count == 1 || (u32)count > INDEX_MASK) {
        for (int i = 0; i < count; i++) fn(ctx, i);
        return;
    }
    s_gen = (s_gen + 1) & GEN_MASK;
    s_fn = fn;
    s_ctx = ctx;
    s_count = count;
    s_done = 0;
    __atomic_store_n(&s_claim, s_gen << GEN_SHIFT, __ATOMIC_RELEASE);
    LightEvent_Signal(&s_start);
    run_jobs(s_gen, fn, ctx, count);
    if (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE) == (u32)count) {
        PROF_COUNT("helper.batches", 1);
        return;
    }
    // The helper is finishing the jobs it took: usually a few microseconds
    u64 t0 = svcGetSystemTick();
    for (int spin = 0; __atomic_load_n(&s_done, __ATOMIC_ACQUIRE) != (u32)count; spin++) {
        if (spin < 4000) continue;
        s_caller_waiting = true;
        __dmb();
        if (__atomic_load_n(&s_done, __ATOMIC_ACQUIRE) != (u32)count)
            LightEvent_WaitTimeout(&s_finished, 1000000LL);  // 1 ms, then check again
        s_caller_waiting = false;
    }
    LightEvent_Clear(&s_finished);
    PROF_COUNT("helper.batches", 1);
    PROF_COUNT("helper.join_wait_us", (svcGetSystemTick() - t0) / CPU_TICKS_PER_USEC);
}
