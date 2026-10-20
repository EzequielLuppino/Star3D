// Per-frame timing trace (MULTICORE_PLAN.md, "Frame pacing").
//
// S3D_FRAME_TRACE=<frames per phase> in env.txt, then L+R+SELECT (instead of
// the "mark" snapshot) or the remote "frametrace" command starts a trace of
// three phases, each 30 settle frames + N recorded frames: S3D_RT_DEDUP 7 with
// S3D_RT_PACING 3 (the defaults when it was written), 7 with pacing 0 (the old swap/queue
// handling), 0 with pacing 3. Then both settings go back. Stand still.
//
// Events (frametrace_3ds.h) are timestamped into a memory buffer from the game
// thread, the render thread and the GSP event thread (GPU command list and
// transfer completions and VBlank, from gx_c3d_queue.c's GSP callbacks), so
// nothing is written to the SD card while recording. At the end the buffer goes to sdmc:/3ds/star3d/frametrace.csv
// ("us,event,arg" per line, us from the trace start). Read it with
// tmp_scripts/frametrace.py.

#include "frametrace_3ds.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "log.h"
#include "profiler.h"

#define FT_SETTLE 30
#define FT_MAX_EVENTS (1 << 16)  // 512 KB; ~60 events per frame

typedef struct {
    u32 t;  // svcGetSystemTick() >> 4 (~60 ns, wraps after ~4 min)
    u16 ev, arg;
} FtEntry;

volatile bool g_ft_on;
static FtEntry* s_buf;
static volatile u32 s_n;
static int s_frames = -1;  // S3D_FRAME_TRACE, -1 = not read yet
static int s_state;        // 0 idle, 1 requested, 2+ running (phase index + 2)
static int s_count, s_saved_dedup, s_saved_pacing;
static u32 s_t0;
static bool s_requested;

void ft_record(u32 ev, u32 arg) {
    u32 i = __atomic_fetch_add(&s_n, 1, __ATOMIC_RELAXED);
    if (i >= FT_MAX_EVENTS) return;
    s_buf[i].t = (u32)(svcGetSystemTick() >> 4);
    s_buf[i].ev = (u16)ev;
    s_buf[i].arg = (u16)arg;
}

bool ft_enabled(void) {
    if (s_frames < 0) s_frames = cfg_int("S3D_FRAME_TRACE", 0);
    return s_frames > 0;
}

void ft_request(void) {
    if (!ft_enabled()) {
        LOG_WARN("frametrace", "S3D_FRAME_TRACE is not set (frames per phase)");
        return;
    }
    if (s_state) return;
    s_requested = true;
}

static void write_csv(void) {
    u32 n = s_n < FT_MAX_EVENTS ? s_n : FT_MAX_EVENTS;
    FILE* f = fopen("sdmc:/3ds/star3d/frametrace.csv", "w");
    if (!f) {
        LOG_ERROR("frametrace", "cannot write sdmc:/3ds/star3d/frametrace.csv");
        return;
    }
    static char line[64];
    setvbuf(f, NULL, _IOFBF, 16384);
    fputs("us,event,arg\n", f);
    for (u32 i = 0; i < n; i++) {
        // 16 ticks = 16 / 268.111856 us
        u32 dt = s_buf[i].t - s_t0;
        unsigned long us = (unsigned long)((u64)dt * 16 * 1000000 / SYSCLOCK_ARM11);
        snprintf(line, sizeof(line), "%lu,%u,%u\n", us, s_buf[i].ev, s_buf[i].arg);
        fputs(line, f);
    }
    fclose(f);
    LOG_INFO("frametrace", "wrote %lu events to sdmc:/3ds/star3d/frametrace.csv%s", (unsigned long)n,
             s_n > FT_MAX_EVENTS ? " (buffer full, the rest was lost)" : "");
}

// Phase settings: S3D_RT_DEDUP, S3D_RT_PACING (FT_PHASE arg = dedup * 16 + pacing)
static const struct { int dedup, pacing; } k_phases[] = { { 7, 3 }, { 7, 0 }, { 0, 3 } };
#define FT_PHASES (int)(sizeof(k_phases) / sizeof(k_phases[0]))

static void start_phase(int i) {
    cfg_set("S3D_RT_DEDUP", k_phases[i].dedup);
    cfg_set("S3D_RT_PACING", k_phases[i].pacing);
    ft_ev(FT_PHASE, (u32)(k_phases[i].dedup * 16 + k_phases[i].pacing));
    s_count = 0;
}

void ft_frame(void) {
    if (s_state == 0) {
        if (!s_requested) return;
        s_requested = false;
        if (!s_buf) s_buf = malloc(FT_MAX_EVENTS * sizeof(FtEntry));
        if (!s_buf) {
            LOG_ERROR("frametrace", "no memory for the trace buffer");
            return;
        }
        s_saved_dedup = cfg_int("S3D_RT_DEDUP", 5);
        s_saved_pacing = cfg_int("S3D_RT_PACING", 3);
        s_n = 0;
        s_t0 = (u32)(svcGetSystemTick() >> 4);
        __dmb();
        g_ft_on = true;
        LOG_INFO("frametrace", "started: %d frames per phase, %d phases (dedup/pacing 7/3, 7/0, 0/3); stand still",
                 s_frames, FT_PHASES);
        s_state = 2;
        start_phase(0);
        return;
    }
    if (++s_count < FT_SETTLE + s_frames) return;
    if (s_state - 2 + 1 < FT_PHASES) {
        s_state++;
        start_phase(s_state - 2);
        return;
    }
    g_ft_on = false;
    cfg_set("S3D_RT_DEDUP", s_saved_dedup);
    cfg_set("S3D_RT_PACING", s_saved_pacing);
    s_state = 0;
    write_csv();
}
