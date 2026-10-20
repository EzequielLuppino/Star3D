#ifndef PORT_HELPER_H
#define PORT_HELPER_H

// Fork-join helper on core 1 (MULTICORE_PLAN.md step 1). helper_parallel runs
// fn(ctx, 0..count-1) on the calling thread and on a helper thread on core 1
// at the same time: both take the next index until none are left, so the
// caller does all the work itself when core 1 is busy with system threads
// (core 1 is preemptible), and only waits for the jobs the helper already
// started. Returns when every job is done.
//
// For short, independent pieces of game-thread work (skinning, port/skin_3ds.c).
// Jobs must not call PROF_* zones that the caller times too, take locks the
// caller holds, or call helper_parallel themselves.

#include <stdbool.h>

typedef void (*HelperFn)(void* ctx, int index);

void helper_init(void);
bool helper_available(void);  // false: helper_parallel runs everything inline
void helper_parallel(HelperFn fn, void* ctx, int count);

#endif // PORT_HELPER_H
