#ifndef PORT_WORKER_H
#define PORT_WORKER_H

// Background worker for port-side jobs (file reads, video frames). It runs on
// core 2 below the render thread (gx/gx_rec.c), since core 1 is off by default
// (see core1_init); with S3D_CORE1=1 it moves to core 1 unless
// S3D_WORKER_CORE=2. When neither core is available, jobs run inline on the
// calling thread, so callers never depend on a second core.
//
// Jobs run in submission order. A job struct must stay valid until it is
// done (worker_wait or worker_done).

#include <stdbool.h>
#include <3ds/types.h>

typedef void (*WorkerFn)(void* arg);

typedef struct WorkerJob {
    WorkerFn fn;
    void* arg;
    volatile u32 state;  // 0 idle, 1 queued, 2 running, 3 done
} WorkerJob;

void worker_init(void);
// Core 1 setup shared by the worker, the audio threads and the skinning
// helper (port/helper.c): grants the application its share of core 1 once.
// Returns false when core 1 is not used (S3D_CORE1=0, the default).
bool core1_init(void);
void worker_shutdown(void);
int worker_core(void);  // Core the worker runs on, -1 = inline

void worker_submit(WorkerJob* job, WorkerFn fn, void* arg);
bool worker_done(const WorkerJob* job);
void worker_wait(WorkerJob* job);
u64 worker_take_busy_ticks(void);  // Time spent in jobs since the last call
// For the watchdog: jobs not yet started, and the running job (0/NULL = idle)
void worker_debug_state(u32* queued, u32* running_ms, void** running_fn);

#endif // PORT_WORKER_H
