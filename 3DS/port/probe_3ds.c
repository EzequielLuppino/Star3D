// Core 0 probe (S3D_PROBE=1; MULTICORE_PLAN.md "Where the game thread's time
// goes"). game.sceneRender has tracked the GPU time build after build, so part
// of it may not be game work. Three short fixed workloads per frame tell
// which kind of loss it is:
// - ALU slice: a tight loop reading the system tick. Any gap above
//   PROBE_GAP_US is time core 0 ran something else (GSP event thread,
//   interrupts, other core 0 threads): probe.alu_us / probe.stolen_us /
//   probe.gaps.
// - Memory slice: reads one word per cache line from a buffer larger than the
//   L2 cache. Chunks without a gap give the cost per line, so it can be
//   compared with the boot baseline (taken before the GPU and the render
//   thread start): probe.mem_lines / probe.mem_us.
// Slices run at the frame start (vi_3ds.c, before game logic), before and
// after the game's sceneRender (frame_zones.c), so they are outside
// game.logic_us and game.sceneRender but inside the frame time (~3 x
// 2 x S3D_PROBE_US). It also turns on the coarse game function timers
// (perf/perf_lite.c), and GSP event counts are reported (gsp.*).
//
// S3D_PROBE is a bit mask: 1 = the slices above, 2 = the sampler below
// (MULTICORE_PLAN.md "Inside the game thread"). Either bit turns on the
// coarse timers and the GSP counts.
// - Sampler: a thread on core 2, above the render thread's priority, wakes
//   every S3D_PROBE_SAMPLE_US (default 500) and adds the time since its last
//   wake to the game thread's current scope x leaf cell (port/ptag.h), to the
//   render thread's watchdog phase (the opcode it is replaying) and, outside
//   every scope, to the game thread's watchdog phase. Every 300 frames the
//   "sample" lines give ms per frame per cell plus the per-scope draw counts.
//   The game thread pays only the tag stores; core 2 pays one wake per sample.

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "log.h"
#include "profiler.h"
#include "ptag.h"
#include "watchdog.h"

#define PROBE_GAP_US 5
#define PROBE_CHUNK_LINES 64  // One tick read per chunk (2 KB)
#define LINE 32

#ifndef PERF_WRAP
extern bool g_perf_lite;  // perf/perf_lite.c (PERF_WRAP builds wrap these instead)
#endif
extern volatile u32 g_gsp_event_counts[4];  // gx/gx_c3d_queue.c: P3D, PPF, VBlank0, VBlank1

static bool s_on, s_slices;
static u32 s_slice_ticks;
static volatile u32* s_buf;
static u32 s_buf_lines, s_pos;
static u32 s_chunk_base;  // Boot median ticks per clean chunk
static u32 s_gsp_seen[4];

typedef struct {
    u64 run, stolen;
    u32 gaps, maxgap;
} AluResult;

static AluResult alu_slice(u32 ticks) {
    AluResult r = { 0, 0, 0, 0 };
    const u64 gap = (u64)PROBE_GAP_US * CPU_TICKS_PER_USEC;
    u64 start = svcGetSystemTick(), prev = start, now = start;
    while (now - start < ticks) {
        now = svcGetSystemTick();
        u64 d = now - prev;
        if (d > gap) {
            r.stolen += d;
            r.gaps++;
            if (d > r.maxgap) r.maxgap = (u32)d;
        }
        prev = now;
    }
    r.run = now - start;
    return r;
}

typedef struct {
    u64 clean_ticks;
    u32 clean_lines, bad_chunks;
} MemResult;

// Chunks slower than 4x the boot median count as interrupted and are left out
static MemResult mem_slice(u32 ticks, u32 limit, u32* chunk_ticks, u32 max_chunks) {
    MemResult r = { 0, 0, 0 };
    u32 sum = 0, n = 0;
    u64 start = svcGetSystemTick(), prev = start;
    while (prev - start < ticks) {
        volatile u32* p = s_buf + s_pos * (LINE / 4);
        for (int i = 0; i < PROBE_CHUNK_LINES; i++) sum += p[i * (LINE / 4)];
        s_pos += PROBE_CHUNK_LINES;
        if (s_pos + PROBE_CHUNK_LINES > s_buf_lines) s_pos = 0;
        u64 now = svcGetSystemTick();
        u32 d = (u32)(now - prev);
        prev = now;
        if (chunk_ticks && n < max_chunks) chunk_ticks[n++] = d;
        if (limit && d > limit) {
            r.bad_chunks++;
            continue;
        }
        r.clean_ticks += d;
        r.clean_lines += PROBE_CHUNK_LINES;
    }
    __asm__ volatile("" ::"r"(sum));
    return r;
}

static int cmp_u32(const void* a, const void* b) {
    u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}

// --- Sampler (S3D_PROBE bit 2) ---------------------------------------------

volatile u8 g_ptag_scope[THREAD_SLOTS];
volatile u8 g_ptag_leaf[THREAD_SLOTS];
volatile u8 g_ptag_op, g_ptag_sub;
u32 g_ptag_op_calls[256];
u32 g_ptag_imm_calls[PS_COUNT], g_ptag_begin_verts[PS_COUNT], g_ptag_dl_calls[PS_COUNT], g_ptag_dl_bytes[PS_COUNT];

#define SAMPLE_REPORT_FRAMES 300
#define SAMPLE_MAX_GAP_US 20000  // Longer gaps (HOME menu, sleep) are not counted
#define PHASES 96  // Render-thread phases include the GX_PROF_ZONE steps (gx_internal.h)

static const char* const s_scope_names[PS_COUNT] = {
    "outside", "logic", "lights", "scene", "objRender", "objRender.transp", "mapBlock", "transpPolys",
    "fur", "groundShadow", "shadow", "glow", "expgfx", "water", "sky", "particles", "reflections", "rcp",
};
static const char* const s_leaf_names[PL_COUNT] = {
    "game", "gx.cmd", "gx.vtx", "gx.draw", "dl.hash", "skin", "joints", "wait", "probe",
};

// Cumulative microseconds, written only by the sampler (u32 reads are atomic)
static volatile u32 s_cell_us[PS_COUNT][PL_COUNT];
static volatile u32 s_total_us, s_samples, s_dropped;
typedef struct {
    const char* volatile name[PHASES];
    volatile u32 us[PHASES + 1];  // [PHASES]: table full
} PhaseTable;
static PhaseTable s_rt_phases, s_out_phases;
// Recorder detail, while the game thread is in a gx.* or dl.hash leaf: the
// opcode being recorded (g_ptag_op, set in gxrec_begin and the generated
// wrappers) and the recorder step (g_ptag_sub, gx_rec.c)
static volatile u32 s_op_us[256], s_sub_us[PSUB_COUNT];
static const char* const s_sub_names[PSUB_COUNT] = {
    "wrapper/encode", "ring reserve", "dedup filter", "publish", "imm chunk",
};
const char* gxrec_op_name(u32 op);  // gx_rec.c
static Thread s_sampler;
static volatile bool s_sampler_quit;
static s64 s_sample_ns;

static void phase_add(PhaseTable* t, const char* name, u32 us) {
    if (!name) name = "(none)";
    for (int i = 0; i < PHASES; i++) {
        const char* n = t->name[i];
        if (n == name) {
            t->us[i] += us;
            return;
        }
        if (!n) {
            t->name[i] = name;
            t->us[i] += us;
            return;
        }
    }
    t->us[PHASES] += us;
}

static void sampler_main(void* arg) {
    (void)arg;
    // Microseconds from the tick count since the start, so no fraction is lost
    const u64 start = svcGetSystemTick();
    u64 last_us = 0;
    while (!s_sampler_quit) {
        svcSleepThread(s_sample_ns);
        u64 now_us = (svcGetSystemTick() - start) * 1000000ULL / SYSCLOCK_ARM11;
        u32 us = (u32)(now_us - last_us);
        last_us = now_us;
        if (us > SAMPLE_MAX_GAP_US) {
            s_dropped++;
            continue;
        }
        u32 sc = g_ptag_scope[THREAD_SLOT_GAME], lf = g_ptag_leaf[THREAD_SLOT_GAME];
        if (sc >= PS_COUNT) sc = PS_OUT;
        if (lf >= PL_COUNT) lf = PL_GAME;
        s_cell_us[sc][lf] += us;
        if (lf == PL_GXCMD || lf == PL_GXVTX || lf == PL_GXDRAW || lf == PL_DLHASH) {
            u32 sub = g_ptag_sub;
            s_op_us[g_ptag_op] += us;
            s_sub_us[sub < PSUB_COUNT ? sub : PSUB_NONE] += us;
        }
        s_total_us += us;
        s_samples++;
        phase_add(&s_rt_phases, (const char*)g_wd_phase[THREAD_SLOT_RENDER], us);
        if (sc == PS_OUT) phase_add(&s_out_phases, (const char*)g_wd_phase[THREAD_SLOT_GAME], us);
    }
}

static void sampler_start(void) {
    int us = cfg_int("S3D_PROBE_SAMPLE_US", 500);
    if (us < 100) us = 100;
    s_sample_ns = (s64)us * 1000;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Above the render thread (game priority - 2) and the worker, on their core
    s_sampler = threadCreate(sampler_main, NULL, 8 * 1024, prio - 8, 2, false);
    if (!s_sampler) LOG_WARN("probe", "sampler thread not created");
    else LOG_INFO("probe", "sampler on core 2 every %d us (priority 0x%lX)", us, (unsigned long)(prio - 8));
}

void probe_shutdown(void) {
    if (!s_sampler) return;
    s_sampler_quit = true;
    threadJoin(s_sampler, 100000000ULL);
    threadFree(s_sampler);
    s_sampler = NULL;
}

typedef struct {
    u32 cell[PS_COUNT][PL_COUNT];
    u32 total, samples, dropped;
    u32 imm[PS_COUNT], verts[PS_COUNT], dls[PS_COUNT], dlb[PS_COUNT];
    u32 rt[PHASES + 1], out[PHASES + 1];
    u32 op_us[256], op_calls[256], sub[PSUB_COUNT];
} SampleSnap;

static void snap(SampleSnap* s) {
    for (int i = 0; i < PS_COUNT; i++) {
        for (int j = 0; j < PL_COUNT; j++) s->cell[i][j] = s_cell_us[i][j];
        s->imm[i] = g_ptag_imm_calls[i];
        s->verts[i] = g_ptag_begin_verts[i];
        s->dls[i] = g_ptag_dl_calls[i];
        s->dlb[i] = g_ptag_dl_bytes[i];
    }
    for (int i = 0; i <= PHASES; i++) {
        s->rt[i] = s_rt_phases.us[i];
        s->out[i] = s_out_phases.us[i];
    }
    for (int i = 0; i < 256; i++) {
        s->op_us[i] = s_op_us[i];
        s->op_calls[i] = g_ptag_op_calls[i];
    }
    for (int i = 0; i < PSUB_COUNT; i++) s->sub[i] = s_sub_us[i];
    s->total = s_total_us;
    s->samples = s_samples;
    s->dropped = s_dropped;
}

// Top phases of one table, "name ms" pairs above 0.05 ms per frame, 10 per
// line (longer log lines are cut)
static void report_phases(const char* what, PhaseTable* t, const u32* now, const u32* before, double per) {
    char line[400];
    bool used[PHASES + 1] = { false };
    for (int row = 0; row < 5; row++) {
        int len = 0;
        for (int k = 0; k < 10; k++) {
            int best = -1;
            u32 bv = 0;
            for (int i = 0; i <= PHASES; i++) {
                u32 v = now[i] - before[i];
                if (!used[i] && v > bv) {
                    bv = v;
                    best = i;
                }
            }
            if (best < 0 || bv * per < 0.05) break;
            used[best] = true;
            const char* n = best < PHASES ? t->name[best] : "(table full)";
            len += snprintf(line + len, sizeof(line) - len, "%s%s %.2f", k ? ", " : "", n ? n : "?", bv * per);
            if (len >= (int)sizeof(line) - 1) break;
        }
        if (!len && row) break;
        LOG_INFO("probe", "sample %s%s: %s", what, row ? " (cont.)" : "", len ? line : "(none)");
        if (!len) break;
    }
}

static SampleSnap s_prev, s_cur;
static bool s_have_prev, s_ab_driven;
static u32 s_frames;

// Recorder lines: the steps, then the top opcodes with calls per frame
static void report_recorder(double per) {
    char line[1000];
    int len = 0;
    for (int i = 0; i < PSUB_COUNT; i++)
        len += snprintf(line + len, sizeof(line) - len, "%s%s %.2f", i ? ", " : "", s_sub_names[i],
                        (s_cur.sub[i] - s_prev.sub[i]) * per);
    LOG_INFO("probe", "sample recorder steps (ms/frame): %s", line);
    bool used[256] = { false };
    for (int row = 0; row < 4; row++) {
        len = 0;
        for (int k = 0; k < 8; k++) {
            int best = -1;
            u32 bv = 0;
            for (int i = 0; i < 256; i++) {
                u32 v = s_cur.op_us[i] - s_prev.op_us[i];
                if (!used[i] && v > bv) {
                    bv = v;
                    best = i;
                }
            }
            if (best < 0 || bv * per < 0.05) break;
            used[best] = true;
            u32 calls = s_cur.op_calls[best] - s_prev.op_calls[best];
            double cpf = (double)calls / s_frames;
            len += snprintf(line + len, sizeof(line) - len, "%s%s %.2f ms (%.0f/frame, %.2f us)", k ? ", " : "",
                            gxrec_op_name(best), bv * per, cpf, calls ? bv / (double)calls : 0.0);
        }
        if (!len) break;
        LOG_INFO("probe", "sample recorder ops%s: %s", row ? " (cont.)" : "", line);
    }
}

static void sampler_reset(void) {
    snap(&s_prev);
    s_have_prev = true;
    s_frames = 0;
}

// Report the window since the last report or reset; label names an A/B phase
static void sampler_report(const char* label) {
    if (!s_have_prev || s_frames == 0) {
        sampler_reset();
        return;
    }
    snap(&s_cur);
    u32 frames = s_frames;
    double per = 1.0 / 1000.0 / frames;  // us -> ms per frame
    u32 samples = s_cur.samples - s_prev.samples;
    LOG_INFO("probe", "sample%s%s: %lu frames, %.2f ms/frame sampled, %lu samples (%.0f us apart), %lu long gaps "
             "dropped", label ? " " : "", label ? label : "", (unsigned long)frames,
             (s_cur.total - s_prev.total) * per, (unsigned long)samples,
             samples ? (double)(s_cur.total - s_prev.total) / samples : 0.0,
             (unsigned long)(s_cur.dropped - s_prev.dropped));
    char head[160];
    int hl = 0;
    for (int j = 0; j < PL_COUNT; j++) hl += snprintf(head + hl, sizeof(head) - hl, " %s", s_leaf_names[j]);
    LOG_INFO("probe", "sample columns (ms/frame): total |%s | per frame: imm calls, GXBegin verts, DL calls, DL KB",
             head);
    for (int i = 0; i < PS_COUNT; i++) {
        u32 tot = 0;
        for (int j = 0; j < PL_COUNT; j++) tot += s_cur.cell[i][j] - s_prev.cell[i][j];
        u32 dls = s_cur.dls[i] - s_prev.dls[i];
        if (tot * per < 0.05 && dls == 0) continue;
        char cells[160];
        int len = 0;
        for (int j = 0; j < PL_COUNT; j++)
            len += snprintf(cells + len, sizeof(cells) - len, " %5.2f", (s_cur.cell[i][j] - s_prev.cell[i][j]) * per);
        LOG_INFO("probe", "sample %-16s %6.2f |%s | %.0f %.0f %.1f %.1f", s_scope_names[i], tot * per, cells,
                 (double)(s_cur.imm[i] - s_prev.imm[i]) / frames, (double)(s_cur.verts[i] - s_prev.verts[i]) / frames,
                 (double)dls / frames, (s_cur.dlb[i] - s_prev.dlb[i]) / 1024.0 / frames);
    }
    report_phases("render thread", &s_rt_phases, s_cur.rt, s_prev.rt, per);
    report_phases("game outside scopes", &s_out_phases, s_cur.out, s_prev.out, per);
    report_recorder(per);
    s_prev = s_cur;
    s_frames = 0;
}

// Frame start: periodic reports, unless an A/B run drives them (probe_sample_mark)
static void sampler_frame(void) {
    if (!s_have_prev) {
        sampler_reset();
        return;
    }
    s_frames++;
    if (!s_ab_driven && s_frames >= SAMPLE_REPORT_FRAMES) sampler_report(NULL);
}

// A/B runs (diag_3ds.c): NULL starts a window after the settle frames, a
// label ends it with a report named after the phase
void probe_sample_mark(const char* label) {
    if (!s_sampler) return;
    s_ab_driven = true;
    if (label) sampler_report(label);
    else sampler_reset();
}

void probe_init(void) {
    int mode = cfg_int("S3D_PROBE", 0);
    s_on = mode > 0;
    s_slices = (mode & 1) != 0;
#ifndef PERF_WRAP
    g_perf_lite = s_on;
#endif
    if (!s_on) return;
    if (mode & 2) sampler_start();
    if (!s_slices) {
        LOG_INFO("probe", "on: timers%s, no slices", s_sampler ? " and sampler" : "");
        return;
    }
    s_slice_ticks = (u32)cfg_int("S3D_PROBE_US", 300) * CPU_TICKS_PER_USEC;
    // Larger than the 2 MB L2, so every line comes from FCRAM
    static const u32 sizes[] = { 4u << 20, 3u << 20, 2u << 20, 1u << 20 };
    u32 bytes = 0;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]) && !s_buf; i++) {
        s_buf = (volatile u32*)malloc(sizes[i]);
        if (s_buf) bytes = sizes[i];
    }
    if (!s_buf) {
        LOG_WARN("probe", "no memory for the probe buffer: probe off");
        s_on = false;
        return;
    }
    s_buf_lines = bytes / LINE;
    for (u32 i = 0; i < bytes / 4; i += LINE / 4) s_buf[i] = i;

    // Baseline: GPU and render thread idle
    AluResult a = alu_slice(20 * 1000 * CPU_TICKS_PER_USEC);
    static u32 chunks[4096];
    mem_slice(2 * 1000 * CPU_TICKS_PER_USEC, 0, NULL, 0);  // Warm up
    MemResult m0 = mem_slice(20 * 1000 * CPU_TICKS_PER_USEC, 0, chunks, 4096);
    u32 n = m0.clean_lines / PROBE_CHUNK_LINES;
    if (n > 4096) n = 4096;
    qsort(chunks, n, sizeof(chunks[0]), cmp_u32);
    s_chunk_base = n ? chunks[n / 2] : 0;
    LOG_INFO("probe", "on: slices %d us x 3 per frame, buffer %lu KB; boot baseline: alu stolen %.1f%% "
             "(%lu gaps, max %.0f us), memory %.1f ns/line (median chunk %.1f us, %lu chunks)",
             cfg_int("S3D_PROBE_US", 300), (unsigned long)(bytes / 1024),
             a.run ? 100.0 * a.stolen / a.run : 0.0, (unsigned long)a.gaps, a.maxgap / (double)CPU_TICKS_PER_USEC,
             m0.clean_lines ? m0.clean_ticks * 1000.0 / CPU_TICKS_PER_USEC / m0.clean_lines : 0.0,
             s_chunk_base / (double)CPU_TICKS_PER_USEC, (unsigned long)n);
}

bool probe_enabled(void) { return s_on; }

// where: 0 frame start, 1 before sceneRender, 2 after it
void probe_slice(int where) {
    if (!s_on) return;
    if (where == 0) {
        // GSP events since the last frame (counted on the GSP thread)
        static const char* const names[4] = { "gsp.p3d", "gsp.ppf", "gsp.vblank0", "gsp.vblank1" };
        for (int i = 0; i < 4; i++) {
            u32 c = g_gsp_event_counts[i];
            static int ids[4] = { -1, -1, -1, -1 };
            if (ids[i] < 0) ids[i] = prof_register_counter(names[i]);
            prof_add(ids[i], c - s_gsp_seen[i]);
            s_gsp_seen[i] = c;
        }
        if (s_sampler) sampler_frame();
    }
    if (!s_slices) return;
    PTAG_LEAF(PL_PROBE);
    AluResult a = alu_slice(s_slice_ticks);
    MemResult m = mem_slice(s_slice_ticks, s_chunk_base * 4, NULL, 0);
    PROF_COUNT("probe.alu_us", a.run / CPU_TICKS_PER_USEC);
    PROF_COUNT("probe.stolen_us", a.stolen / CPU_TICKS_PER_USEC);
    PROF_COUNT("probe.gaps", a.gaps);
    PROF_COUNT("probe.mem_lines", m.clean_lines);
    PROF_COUNT("probe.mem_us", m.clean_ticks / CPU_TICKS_PER_USEC);
    PROF_COUNT("probe.mem_bad_chunks", m.bad_chunks);
    switch (where) {
    case 0: PROF_COUNT("probe.stolen_us.frame_start", a.stolen / CPU_TICKS_PER_USEC); break;
    case 1: PROF_COUNT("probe.stolen_us.scene_begin", a.stolen / CPU_TICKS_PER_USEC); break;
    default: PROF_COUNT("probe.stolen_us.scene_end", a.stolen / CPU_TICKS_PER_USEC); break;
    }
}
