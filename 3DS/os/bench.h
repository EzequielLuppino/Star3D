#ifndef BENCH_H
#define BENCH_H

// Hardware benchmarks at boot (S3D_BENCH, default on): storage, decompression,
// memcpy and GPU fill/vertex rates, written to the log. Call after gx_init();
// the caller restores its own GPU state afterwards (gx_restore_state).
void bench_run(void);

#endif // BENCH_H
