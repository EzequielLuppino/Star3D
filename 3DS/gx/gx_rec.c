// GX recorder and render thread (MULTICORE_PLAN.md step 2). Modes: gx_rec.h.
//
// The game thread records its GX calls into a ring of 32-bit words: one
// header word (opcode | payload words << 8) per command, then the arguments
// as the generated encoders store them (tools/gen_gxrec.py). Immediate-mode
// vertex data goes into GXREC_OP_IMM chunks after its GXBegin command, as raw
// big-endian bytes like the GameCube's write-gather pipe; the replay feeds them
// to gx_vtx.c, which draws once the declared vertices have arrived, as before.
//
// Replay calls the real GX functions with the recorded arguments, on the
// render thread (mode 2, core 2) or on the game thread at sync points (mode
// 1). Data behind pointers (display lists, vertex arrays, textures) is read at
// replay time, like the GameCube GPU reads it late; matrices and texture /
// light objects are copied at record time (gxrec.list).
//
// Frame boundary, two ways:
// - S3D_RT_OVERLAP=0, and modes 1 and 2 when the frame end cannot overlap:
//   the frame end drains the ring (gxrec_frame_end), and the boundary (touch
//   UI, C3D_FrameEnd, frame begin) runs on the game thread with the render
//   thread idle.
// - Overlapped (mode 2, S3D_RT_OVERLAP, MULTICORE_PLAN.md step 4): the game
//   thread records the boundary as one command (GXREC_OP_FRAME, run by
//   port_frame_boundary in vi_3ds.c) and goes on with the next frame's logic
//   while the render thread finishes the frame. Like the GameCube GPU, the
//   render thread then reads game memory up to one frame late, so the game
//   thread waits for the boundary (gxrec_fence) where the game rewrites or
//   frees what the previous frame's draws read: at sceneRender (skinning,
//   sky stars, splash colours and CPU textures are rewritten while drawing),
//   at frees with the free delay off (mm_free; the usual 2-tick delay covers
//   one frame of lag), and at the next frame end. Answers that depend on the
//   frame (stereo, HUD and offscreen passes) come from the boundary's
//   arguments instead of render-side state.
//
// Calls that return something are answered on the game thread from
// recorder-side state (scissor, display-list recording, HUD / offscreen pass
// state) or, when the answer is render-side state, after a drain ("sync"
// entries in gxrec.list, counters rt.sync.*). Port code that touches GX state
// outside the recorded calls brackets it with gxrec_direct_begin/_end.
//
// Profiler: rt.replay (replay time, either thread), rt.cmds (commands
// replayed), rt.frame_end_wait (game thread waiting for the render thread at
// the frame end; in mode 1 the replay itself), rt.sync_wait / rt.syncs (mid-
// frame drains), rt.space_wait (ring full). Watchdog: the render thread's
// phase is the GX call it is replaying.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx_internal.h"
#include "gx_init.h"
#include "gx_rec.h"
#include "gx_c3d_ctx.h"
#include "gxrec_gen.h"
#include "config.h"
#include "crash.h"
#include "log.h"
#include "profiler.h"
#include "ptag.h"
#include "watchdog.h"
#include "frametrace_3ds.h"
#ifdef PERF_WRAP
#include "perf/draw_src.h"
#endif

// Mode when S3D_RENDER_THREAD is not set
#define RT_DEFAULT_MODE 2

// gx_vtx.c / gx_init.c
void gx_vtx_imm_bytes(const void* data, u32 n);
bool gx_hud_active(void);
int gx_offscreen_check(float w, float h, int res, bool stereo);
bool gx_frame_stereo(void);
bool gx_render_eye_reduced(void);

#define HDR(op, words) ((u32)(op) | ((u32)(words) << 8))
#define HDR_OP(h) ((h) & 0xFF)
#define HDR_WORDS(h) (((h) >> 8) & 0x7FFFFF)
// PROFILE=1, S3D_RT_DEDUP bit 3: the record-time filter marks the commands it
// would drop instead, and the replay logs any marked command that changes state
#define HDR_DD_MARK 0x80000000u
#define IMM_CHUNK_WORDS 1024  // Space reserved per immediate-mode chunk (4 KB of vertex bytes)
#define PUBLISH_EVERY 16      // Commands between publishes to the render thread
#define RELEASE_EVERY 8       // Commands the render thread replays between read index updates
#define PREFETCH_WORDS 32     // Ring prefetch distance (128 bytes, 4 cache lines)
// Spin iterations before the render thread sleeps on an empty ring (~4-8 us
// at 804 MHz): absorbs the producer's per-draw publishes without a LightEvent
// round trip (rt.idle_spins vs rt.idle_sleeps, PROFILE counters)
#define RT_IDLE_SPIN 4096

_Static_assert(GXREC_OP_COUNT <= 256, "opcodes must fit in the header byte");

bool g_gxrec_on;
__thread bool t_gxrec_direct;

static int s_mode;   // Mode of the current frame
// S3D_RT_DEDUP, set at the frame start (MULTICORE_PLAN.md, "Render thread:
// redundant work"). Bits: 1 keep the unit map on texture-only changes
// (gx_render.c), 2 record-time filter, 4 no normal matrix loads, 8 (PROFILE=1)
// verify the filter instead of dropping. Default 5: the filter costs the game
// thread more than it saves the render thread (MULTICORE_PLAN.md, "Result
// (A1C2F0C5...)")
static int s_dd_mode = -1;
#if GX_PROFILE_DETAIL
int g_gxrec_replay_op = -1;  // Opcode being replayed (verify mode)
#endif
static u32* s_ring;
static u32 s_size;   // Ring size in words

// Producer (game thread)
static u32 s_w;            // Next free word (commands before it are complete)
static u32 s_unpublished;  // Commands recorded since the last publish
static u32* s_imm;         // Open immediate-mode chunk (its header), or NULL
static u8* s_imm_ptr;
static u8* s_imm_end;
static int s_direct_depth;
static bool s_direct_set;

// Shared with the render thread. The two indices sit on their own 32-byte
// cache lines (the MPCore line size): packed next to the game thread's
// per-command variables, every read from the other core moved the line
// between cores 0 and 2.
typedef struct {
    volatile u32 idx;
    u32 pad[7];
} __attribute__((aligned(32))) RingIndex;
static RingIndex s_pub;   // Commands before this index may be replayed (mode 2 only)
static RingIndex s_read;  // Next command to replay; written by the replaying thread
static u32 s_read_seen;   // Game thread: last value read from s_read (it only moves forward)
static bool s_prefetch;   // S3D_RT_PREFETCH (default 1), set at the frame start
static volatile bool s_thread_on;  // The render thread may consume (mode 2); changed only while it is idle
static volatile bool s_cons_sleeping;
static volatile bool s_prod_waiting;
// What the waiting game thread needs: the render thread signals once it has
// replayed s_wake_words words past s_wake_from (or reached the published end).
// One wakeup per block instead of one every RELEASE_EVERY commands: each
// LightEvent_Signal with a waiter is a syscall on core 2.
static volatile u32 s_wake_from;
static volatile u32 s_wake_words;
static u32 s_space_block;  // Words to free before a space wait ends (S3D_RT_WAKE_KB)
static volatile bool s_quit;
// Overlapped frame ends: boundaries recorded (game thread) and run (render thread)
static u32 s_frames_recorded;
static volatile u32 s_frames_done;
static bool s_frame_recorded;     // This frame end recorded its boundary (gxrec_frame_begin)
static __thread bool t_producer;  // The game thread (gxrec_init)
static aptHookCookie s_apt_hook;
static LightEvent s_ev_work;      // Render thread: commands were published
static LightEvent s_ev_progress;  // Game thread: the render thread advanced
static Thread s_thread;

static bool s_hud, s_off;
static bool s_rec_frame, s_rec_stereo;  // The recording frame is open / stereo
static struct {
    u8* buf;
    u32 size, len;
    bool on, overflow;
} s_dl;

// --- Replay -------------------------------------------------------------------

// Words replayed going forward from a to b (both ring indices; the replay
// never passes the game thread, so b is at most one lap ahead)
static inline u32 ring_dist(u32 a, u32 b) {
    return b >= a ? b - a : s_size - a + b;
}

#if GX_PROFILE_DETAIL
// PROFILE=1: replay time per opcode, logged every ~3 s as "rtops" lines (top
// opcodes by time). One tick read per command: each command is charged from
// the previous read, so loop overhead lands on the command after it.
// Nested GX zones (gx.displaylist, ...) are included in their opcode. Written
// by whichever thread replays (never two at once), read at the frame end
// while the render thread is idle
#define OPS_TOP 40
static u64 s_op_ticks[GXREC_OP_COUNT];
static u32 s_op_calls[GXREC_OP_COUNT];
// Commands that changed no render state (g_gx_change_seq untouched). Only
// meaningful for setters that report changes through gx_state_changed: the
// vertex format/array setters, GXSetCurrentMtx and GXLoadNrmMtxImm never do
static u32 s_op_same[GXREC_OP_COUNT];
static u32 s_dd_resets[GXREC_OP_COUNT];  // Commands that made the record-time filter forget everything
static void dd_verify(u32 op, const u32* w, u32 n, bool marked, bool changed);

static bool op_reports_changes(int op) {
    const char* n = gxrec_op_names[op];
    if (!n || (strncmp(n, "GXSet", 5) != 0 && strncmp(n, "GXLoad", 6) != 0)) return false;
    static const char* const silent[] = { "GXSetVtxDesc", "GXSetVtxAttrFmt", "GXSetArray", "GXSetCurrentMtx",
                                          "GXLoadNrmMtxImm", "GXSetLineWidth", "GXSetPointSize", "GXSetCopyClear",
                                          "GXSetTexCopySrc", "GXSetTexCopyDst" };
    for (size_t i = 0; i < sizeof(silent) / sizeof(silent[0]); i++)
        if (!strcmp(n, silent[i])) return false;
    return true;
}
static u32 s_op_frames;
static u64 s_op_window_start;

static void ops_report(bool count_frame) {
    if (count_frame) s_op_frames++;
    if (!s_op_frames) return;
    u64 now = svcGetSystemTick();
    if (!s_op_window_start) s_op_window_start = now;
    if (now - s_op_window_start < 3000ULL * CPU_TICKS_PER_MSEC) return;
    u64 total = 0;
    u32 calls = 0;
    for (int i = 0; i < GXREC_OP_COUNT; i++) {
        total += s_op_ticks[i];
        calls += s_op_calls[i];
    }
    float frames = (float)s_op_frames;
    LOG_DEBUG("rtops", "replay by opcode, %lu frames: %.2f ms/frame, %lu commands/frame", (unsigned long)s_op_frames,
              (float)(total / CPU_TICKS_PER_MSEC) / frames, (unsigned long)(calls / s_op_frames));
    bool shown[GXREC_OP_COUNT] = { false };
    for (int k = 0; k < OPS_TOP; k++) {
        int best = -1;
        for (int i = 0; i < GXREC_OP_COUNT; i++)
            if (!shown[i] && s_op_calls[i] && (best < 0 || s_op_ticks[i] > s_op_ticks[best])) best = i;
        if (best < 0) break;
        shown[best] = true;
        const char* name = gxrec_op_names[best] ? gxrec_op_names[best] : "?";
        LOG_DEBUG("rtops", "  op %-30s %6.2f ms/frame %7.1f/frame %6.2f us each %3lu%% unchanged", name,
                  (float)(s_op_ticks[best] / CPU_TICKS_PER_MSEC) / frames, s_op_calls[best] / frames,
                  (float)(s_op_ticks[best] / CPU_TICKS_PER_MSEC) * 1000.0f / s_op_calls[best],
                  (unsigned long)((u64)s_op_same[best] * 100 / s_op_calls[best]));
    }
    // Unchanged state setters: what a record-time filter could drop
    u32 same = 0, setters = 0;
    for (int i = 0; i < GXREC_OP_COUNT; i++)
        if (op_reports_changes(i)) {
            same += s_op_same[i];
            setters += s_op_calls[i];
        }
    LOG_DEBUG("rtops", "  unchanged setters %lu/frame of %lu/frame (all commands %lu/frame)",
              (unsigned long)(same / s_op_frames), (unsigned long)(setters / s_op_frames),
              (unsigned long)(calls / s_op_frames));
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int i = 0; i < GXREC_OP_COUNT; i++)
            if (s_dd_resets[i] && (best < 0 || s_dd_resets[i] > s_dd_resets[best])) best = i;
        if (best < 0) break;
        LOG_DEBUG("rtops", "  dedup reset by %-30s %7.1f/frame", gxrec_op_names[best] ? gxrec_op_names[best] : "?",
                  s_dd_resets[best] / frames);
        s_dd_resets[best] = 0;
    }
    memset(s_dd_resets, 0, sizeof(s_dd_resets));
    memset(s_op_ticks, 0, sizeof(s_op_ticks));
    memset(s_op_calls, 0, sizeof(s_op_calls));
    memset(s_op_same, 0, sizeof(s_op_same));
    s_op_frames = 0;
    s_op_window_start = now;
}
#endif

// Display-list batching (gx_dlcache.c, S3D_DL_BATCH). Held instances stay
// held across these commands: setters (one that really changes render state
// draws them first in gx_state_changed) and further display-list calls. Any
// other command draws them first. s_dlb_cause counts what ended each batch
// (index GXREC_OP_COUNT: the replay caught up with the game thread), logged
// every DLB_LOG_FRAMES frames (tag dlbatch)
#define DLB_LOG_FRAMES 600
bool g_gxrec_replaying;
static u8 s_dlb_keep[GXREC_OP_COUNT];
static u32 s_dlb_cause[GXREC_OP_COUNT + 1];
static int s_cur_op = -1;
static u32 s_dlb_log_from;  // Frame number of the last log

static void dlb_keep_init(void) {
    static bool done;
    if (done) return;
    done = true;
    for (int op = 0; op < GXREC_OP_COUNT; op++) {
        const char* n = gxrec_op_names[op];
        if (!n) continue;
        s_dlb_keep[op] = strncmp(n, "GXSet", 5) == 0 || strncmp(n, "GXLoad", 6) == 0 ||
                         strcmp(n, "GXClearVtxDesc") == 0 || strcmp(n, "port_tev_indirect") == 0 ||
                         strcmp(n, "gx_dl_call_hashed") == 0;
    }
    // The delta and matrix block commands carry only setter changes (the
    // delta flushes held instances itself when it carries anything but
    // position matrices)
    s_dlb_keep[GXREC_OP_DELTA] = 1;
    s_dlb_keep[GXREC_OP_MBLOCK] = 1;
    s_dlb_keep[GXREC_OP_MODELDRAW] = 1;
}

// gx_dlcache.c, at every flush of held instances: what caused it
void gxrec_dlb_flushed(void) {
    int op = g_gxrec_replaying ? s_cur_op : -1;
    s_dlb_cause[op >= 0 && op < GXREC_OP_COUNT ? op : GXREC_OP_COUNT]++;
}

void gxrec_dlb_log(void) {
    u32 frames = gx_render_frame_number() - s_dlb_log_from;
    if (frames < DLB_LOG_FRAMES) return;
    s_dlb_log_from += frames;
    char buf[200];
    int len = 0;
    u32 total = 0;
    for (int i = 0; i <= GXREC_OP_COUNT; i++) total += s_dlb_cause[i];
    for (int k = 0; k < 6 && total; k++) {
        int best = -1;
        for (int i = 0; i <= GXREC_OP_COUNT; i++)
            if (s_dlb_cause[i] && (best < 0 || s_dlb_cause[i] > s_dlb_cause[best])) best = i;
        if (best < 0) break;
        const char* name = best == GXREC_OP_COUNT ? "(caught up)" : gxrec_op_names[best] ? gxrec_op_names[best] : "?";
        len += snprintf(buf + len, sizeof(buf) - len, "%s%s %.1f", k ? ", " : "", name,
                        (double)s_dlb_cause[best] / frames);
        s_dlb_cause[best] = 0;
        if (len >= (int)sizeof(buf) - 1) break;
    }
    if (total) LOG_INFO("dlbatch", "batch ends per frame (%.1f): %s", (double)total / frames, buf);
    memset(s_dlb_cause, 0, sizeof(s_dlb_cause));
}

// Render thread: the recorded frame boundary
static void run_frame_boundary(const u32* p) {
    float slider;
    memcpy(&slider, &p[2], sizeof(slider));
    port_frame_boundary(p[0] != 0, p[1] != 0, slider);
    __dmb();
    s_frames_done++;
    __dmb();
    gx_dlcache_prof_frame();
    if (s_prod_waiting) LightEvent_Signal(&s_ev_progress);
}

static void execute(u32 op, const u32* p, u32 words) {
    if (op == GXREC_OP_IMM) {
        gx_vtx_imm_bytes(p + 1, p[0]);
        return;
    }
    if (op == GXREC_OP_FRAME) {
        run_frame_boundary(p);
        return;
    }
    if (op == GXREC_OP_DELTA) {
        gxrec_delta_apply(p, words);
        return;
    }
    if (op == GXREC_OP_MBLOCK) {
        gxrec_delta_mblk_apply(p, words);
        return;
    }
    if (op == GXREC_OP_MODELDRAW) {
        const void* list = (const void*)(uintptr_t)p[0];
        u32 nbytes = p[1], mw = p[2], dw = p[3];
        const u32* st = p + 4;
        static const u32 h0[1] = { 0 };
        if (mw) gxrec_delta_mblk_apply(st, mw);
        if (dw) gxrec_delta_apply(st + mw, dw);
        __real_gx_dl_call_hashed(list, nbytes, 0, h0);  // mask 0: the DL side hashes
        return;
    }
    if (!gxrec_replay_generated(op, p)) LOG_ONCE(LOG_LEVEL_ERROR, "rt", "unknown recorded opcode %lu", (unsigned long)op);
}

// Replays the commands from s_read.idx up to end; s_read.idx advances as they complete
static void replay_until(u32 end) {
    u32 r = s_read.idx;
    u32 n = 0;
    dlb_keep_init();
    g_gxrec_replaying = true;
#if GX_PROFILE_DETAIL
    u64 t_prev = svcGetSystemTick();
#endif
    while (r != end) {
        if (r >= s_size) {  // The last command ended exactly at the ring end
            r = 0;
            continue;
        }
        u32 h = s_ring[r];
        // The game thread wrote these lines on core 0: ask for them early
        // (PLD is only a hint, so reading past the ring end is harmless)
        if (s_prefetch) __builtin_prefetch(&s_ring[r + PREFETCH_WORDS]);
        u32 op = HDR_OP(h);
        if (op == GXREC_OP_WRAP) {
            r = 0;
            continue;
        }
        const char* name = op < GXREC_OP_COUNT ? gxrec_op_names[op] : NULL;
        g_main_phase = name ? name : "rt: replay";
#if GX_PROFILE_DETAIL
        u32 seq = g_gx_change_seq;
        g_gxrec_replay_op = (int)op;
#endif
        s_cur_op = (int)op;
        if (g_dlb_pending && (op >= GXREC_OP_COUNT || !s_dlb_keep[op])) gx_dlbatch_flush();
        execute(op, &s_ring[r + 1], HDR_WORDS(h));
#if GX_PROFILE_DETAIL
        g_gxrec_replay_op = -1;
#endif
#if GX_PROFILE_DETAIL
        if (op < GXREC_OP_COUNT) {
            u64 t = svcGetSystemTick();
            s_op_ticks[op] += t - t_prev;
            s_op_calls[op]++;
            if (seq == g_gx_change_seq) s_op_same[op]++;
            if (s_dd_mode & 8) dd_verify(op, &s_ring[r + 1], HDR_WORDS(h), (h & HDR_DD_MARK) != 0, seq != g_gx_change_seq);
            t_prev = t;
        }
#endif
        r += 1 + HDR_WORDS(h);
        if ((++n % RELEASE_EVERY) == 0) {
            __dmb();  // The command's words are read before the producer may reuse them
            s_read.idx = r;
            if (s_prod_waiting) {
                __dmb();  // s_wake_* were written before s_prod_waiting
                if (ring_dist(s_wake_from, r) >= s_wake_words) LightEvent_Signal(&s_ev_progress);
            }
        }
    }
    // Nothing outside a replay may see held instances
    s_cur_op = -1;
    if (g_dlb_pending) gx_dlbatch_flush();
    g_gxrec_replaying = false;
    __dmb();
    s_read.idx = r;
    __dmb();
    if (s_prod_waiting) LightEvent_Signal(&s_ev_progress);
    PROF_COUNT("rt.cmds", n);
}

static void render_main(void* arg) {
    (void)arg;
    crash_install_handler();
    t_thread_slot = THREAD_SLOT_RENDER;
    t_gxrec_direct = true;
    for (;;) {
        u32 end = s_pub.idx;
        if (!s_thread_on || s_read.idx == end) {
            if (s_quit) break;
            WD_PHASE("rt: idle");
            s_cons_sleeping = true;
            __dmb();
            if ((!s_thread_on || s_pub.idx == s_read.idx) && !s_quit) {
                // The producer publishes at every draw, so when the replay
                // catches up the next publish usually follows within a few
                // microseconds. A short spin absorbs it and saves a
                // LightEvent round trip per draw (~700-800 wake cycles per
                // hub frame without it); then sleep for real.
                for (u32 spin = 0; spin < RT_IDLE_SPIN && s_read.idx == s_pub.idx; spin++)
                    __dmb();
                if (s_read.idx == s_pub.idx) {
                    ft_ev(FT_R_IDLE_B, 0);
                    LightEvent_Wait(&s_ev_work);
                    ft_ev(FT_R_IDLE_E, 0);
                    // The GPU had time to catch up: empty its queue if it is done
                    C3Dx_Recycle();
                    PROF_COUNT("rt.idle_sleeps", 1);
                } else {
                    PROF_COUNT("rt.idle_spins", 1);
                }
            }
            s_cons_sleeping = false;
            continue;
        }
        __dmb();  // Everything before end was written before it was published
        PROF_ZONE("rt.replay");
        replay_until(end);
    }
    WD_PHASE("rt: stopped");
}

// --- Ring (game thread) ---------------------------------------------------------

static void close_imm(void);

static void publish(void) {
    PTAG_SUB(PSUB_PUBLISH);
    s_unpublished = 0;
    if (!s_thread_on || s_pub.idx == s_w) return;
    __dmb();
    s_pub.idx = s_w;
    __dmb();
    if (s_cons_sleeping) LightEvent_Signal(&s_ev_work);
}

// Game thread time spent replaying inline or waiting for the render thread
// (port/frame_zones.c subtracts it from sceneRender: game.scene_own_us)
u64 g_rt_game_wait_ticks;
static void game_wait_add(u64 t0) {
    if (t_producer) g_rt_game_wait_ticks += svcGetSystemTick() - t0;
}

// Mode 1, and mode 2 without its thread: replay everything now, here
static void drain_inline(void) {
    close_imm();
    gxrec_delta_flush();
    if (s_read.idx == s_w) return;
    PTAG_LEAF(PL_WAIT);
    PROF_ZONE("rt.replay");
    u64 t0 = svcGetSystemTick();
    bool was = t_gxrec_direct;
    t_gxrec_direct = true;
    volatile const char* phase = g_main_phase;
    replay_until(s_w);
    g_main_phase = phase;
    t_gxrec_direct = was;
    game_wait_add(t0);
}

// Sleeps until the render thread has replayed words words past from, or
// everything recorded (words 1: the next release point)
static void wait_progress(u32 from, u32 words) {
    PTAG_LEAF(PL_WAIT);
    s_wake_from = from;
    s_wake_words = words;
    __dmb();
    s_prod_waiting = true;
    __dmb();
    u32 r = s_read.idx;
    if (r != s_w && ring_dist(from, r) < words)
        LightEvent_WaitTimeout(&s_ev_progress, 2000000LL);  // 2 ms, then look again
    s_prod_waiting = false;
}

// Waits until everything recorded so far has been replayed
static void sync_all(void) {
    if (s_mode != 2 || !s_thread) {
        drain_inline();
        return;
    }
    close_imm();
    gxrec_delta_flush();
    publish();
    if (s_read.idx != s_w) {
        PROF_ZONE("rt.sync_wait");
        u64 t0 = svcGetSystemTick();
        volatile const char* phase = g_main_phase;
        WD_PHASE("rt: waiting for the render thread");
        // Woken only when the replay reaches the end (s_size is never reached)
        while (s_read.idx != s_w) wait_progress(s_read.idx, s_size);
        game_wait_add(t0);
        g_main_phase = phase;
    }
    __dmb();  // What the render thread wrote is visible from here on
}

// Room for need contiguous words at s_w, wrapping to the ring start when the
// end is too short. s_w never catches up with s_read.idx (equal means empty).
static void reserve(u32 need) {
    PTAG_SUB(PSUB_RESERVE);
    for (;;) {
        // A stale read index only underestimates the free space, so the
        // shared one is read only when the cached one says the ring is full
        u32 r = s_read_seen;
        if (s_w >= r) {
            // Free: [s_w, end) and [0, r)
            if (s_size - s_w > need || (r != 0 && s_size - s_w == need)) return;
            if (r > need) {
                if (s_w < s_size) s_ring[s_w] = HDR(GXREC_OP_WRAP, 0);
                s_w = 0;
                continue;
            }
        } else if (r - s_w > need) {
            return;
        }
        if (s_read.idx != r) {
            s_read_seen = s_read.idx;
            continue;
        }
        if (s_mode == 2 && s_thread) {
            PROF_ZONE("rt.space_wait");
            u64 t0 = svcGetSystemTick();
            ft_ev(FT_G_SPACE_B, 0);
            publish();
            // Wait for a whole block to free up, not for the next release
            // point: otherwise a full ring costs a wakeup every few commands
            u32 block = s_space_block ? s_space_block : 1;
            do wait_progress(r, block);
            while (s_read.idx != s_w && ring_dist(r, s_read.idx) < block);
            ft_ev(FT_G_SPACE_E, 0);
            game_wait_add(t0);
        } else {
            drain_inline();
        }
    }
}

// --- Record-time filter (S3D_RT_DEDUP bit 1, MULTICORE_PLAN.md "Render thread:
// redundant work") ---
// The game sets most of its state again before every draw: in the hub ~73% of
// the state commands leave the render-side state as it was. Dropping them here
// saves their ring space and their replay on the render thread, the frame's
// bottleneck. A command is dropped when its arguments are bit-identical to
// the last one recorded for the same state (opcode + key argument, e.g. the
// TEV stage), so the render-side setter would have returned without a change.
// That holds only while the render-side state is exactly what was last
// recorded, so the remembered commands are forgotten:
// - all of them, at any command not classified below (it may change filtered
//   state in a way the table does not know), and at every direct section,
//   sync and frame start (refresh_shadows: GX then ran outside the ring);
// - the matrix group (matrices and texgen matrix indices), at every display
//   list call: a list can load XF matrices and the CP matrix index registers
//   (gx_vtx.c handle_cp / xf_write). Lists cannot write BP state (TEV, pixel);
// - the indirect group (GXSetTevDirect, GXSetNumIndStages), at indirect
//   stage setups, which change the same per-stage warp and bump state
//   (gx_tev.c port_tev_indirect).
// Channel commands with overlapping channel IDs (COLOR0 vs COLOR0A0) forget
// each other.
enum { DD_INVALIDATE, DD_NEUTRAL, DD_FILTER, DD_FORGET };  // DD_FORGET: forgets its group
enum { DD_GRP_NONE, DD_GRP_XF, DD_GRP_IND, DD_GROUPS };
// PLAIN: the key word as is (TEV stage, texmap, ...); NONE: global state (one
// entry per opcode); CHAN: a GXChannelID; LOWBIT: a GXLightID mask, only its
// lowest light is written
enum { DD_KEY_PLAIN, DD_KEY_CHAN, DD_KEY_LOWBIT, DD_KEY_NONE };
typedef struct {
    u8 kind, group, keymode;
    s8 key_word;  // Payload word holding the key, negative = from the end
} DdOp;
#define DD_SLOT_BITS 10
#define DD_MAX_WORDS 20  // GXLoadTexObj and GXLoadLightObjImm: 18 (64-byte objects in this SDK)
typedef struct {
    u32 op, key, gen_all, gen_grp, n;
    u32 w[DD_MAX_WORDS];
} DdEntry;
static DdOp s_dd_ops[GXREC_OP_COUNT];
static DdEntry s_dd[1 << DD_SLOT_BITS];
static u32 s_dd_gen_all = 1, s_dd_gen[DD_GROUPS] = { 1, 1, 1 };
static bool s_dd_on;  // Set at the frame start from S3D_RT_DEDUP bit 1

static void dd_init(void) {
    static bool done;
    if (done) return;
    done = true;
    // Unlisted opcodes stay DD_INVALIDATE
    static const struct { u8 op, kind, group, keymode; s8 key; } table[] = {
        // Draws and state no filtered command depends on
        { GXREC_OP_GXBegin, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXEnd, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetVtxDesc, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXClearVtxDesc, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetVtxAttrFmt, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetArray, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetCurrentMtx, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXLoadNrmMtxImm, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetScissor, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_port_tex_want_mips, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXFlush, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetLineWidth, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetPointSize, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXEnableTexOffsets, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetCopyClear, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetTexCopySrc, DD_NEUTRAL, 0, 0, 0 },
        { GXREC_OP_GXSetTexCopyDst, DD_NEUTRAL, 0, 0, 0 },
        // Matrices and texgens: display lists can change them
        { GXREC_OP_gx_dl_call_hashed, DD_FORGET, DD_GRP_XF, 0, 0 },
        { GXREC_OP_MODELDRAW, DD_FORGET, DD_GRP_XF, 0, 0 },
        { GXREC_OP_GXLoadPosMtxImm, DD_FILTER, DD_GRP_XF, 0, -1 },  // (mtx, id)
        { GXREC_OP_GXLoadTexMtxImm, DD_FILTER, DD_GRP_XF, 0, -2 },  // (mtx, id, type)
        { GXREC_OP_GXSetTexCoordGen2, DD_FILTER, DD_GRP_XF, 0, 0 },
        // Per-stage warp and bump
        { GXREC_OP_port_tev_indirect, DD_FORGET, DD_GRP_IND, 0, 0 },
        { GXREC_OP_GXSetTevIndirect, DD_FORGET, DD_GRP_IND, 0, 0 },
        { GXREC_OP_GXSetTevDirect, DD_FILTER, DD_GRP_IND, 0, 0 },
        { GXREC_OP_GXSetNumIndStages, DD_FILTER, DD_GRP_IND, DD_KEY_NONE, 0 },
        // TEV
        { GXREC_OP_GXSetNumTevStages, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetTevOrder, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevColorIn, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevAlphaIn, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevColorOp, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevAlphaOp, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevColor, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevKColor, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevKColorSel, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevKAlphaSel, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevSwapMode, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetTevSwapModeTable, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXSetIndTexOrder, DD_FILTER, 0, 0, 0 },
        { GXREC_OP_GXLoadTexObj, DD_FILTER, 0, 0, -1 },  // (obj, id)
        // Texgen count, lighting, pixel pipeline, transform
        { GXREC_OP_GXSetNumTexGens, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetNumChans, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetChanCtrl, DD_FILTER, 0, DD_KEY_CHAN, 0 },
        { GXREC_OP_GXSetChanAmbColor, DD_FILTER, 0, DD_KEY_CHAN, 0 },
        { GXREC_OP_GXSetChanMatColor, DD_FILTER, 0, DD_KEY_CHAN, 0 },
        { GXREC_OP_GXLoadLightObjImm, DD_FILTER, 0, DD_KEY_LOWBIT, -1 },  // (obj, light mask)
        { GXREC_OP_GXSetBlendMode, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetZMode, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetColorUpdate, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetAlphaUpdate, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetCullMode, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetAlphaCompare, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetFog, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetProjection, DD_FILTER, 0, DD_KEY_NONE, 0 },
        { GXREC_OP_GXSetViewport, DD_FILTER, 0, DD_KEY_NONE, 0 },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        DdOp* d = &s_dd_ops[table[i].op];
        d->kind = table[i].kind;
        d->group = table[i].group;
        d->keymode = table[i].keymode;
        d->key_word = table[i].key;
    }
}

static inline DdEntry* dd_entry(u32 op, u32 key) {
    return &s_dd[((op * 0x9E3779B1u) ^ (key * 0x85EBCA77u)) >> (32 - DD_SLOT_BITS)];
}

// Channels each GXChannelID writes (COLOR0, COLOR1, ALPHA0, ALPHA1 = bits 0-3)
static u32 dd_chan_mask(u32 chan) {
    static const u8 masks[] = { 1, 2, 4, 8, 5, 10 };  // GX_COLOR0 .. GX_COLOR1A1
    return chan < sizeof(masks) ? masks[chan] : 0;
}

static void dd_reset(u32 op) {
    s_dd_gen_all++;
#if GX_PROFILE_DETAIL
    s_dd_resets[op]++;
#else
    (void)op;
#endif
}

// The command at s_w (payload w, n words) leaves the render-side state as it is
static bool dd_redundant(u32 op, const u32* w, u32 n) {
    const DdOp* d = &s_dd_ops[op];
    switch (d->kind) {
        case DD_NEUTRAL: return false;
        case DD_FORGET: s_dd_gen[d->group]++; return false;
        case DD_FILTER: break;
        default: dd_reset(op); return false;
    }
    int kw = d->key_word >= 0 ? d->key_word : (int)n + d->key_word;
    if (n > DD_MAX_WORDS || kw < 0 || kw >= (int)n) {
        dd_reset(op);
        return false;
    }
    u32 key = d->keymode == DD_KEY_NONE ? 0 : w[kw];
    if (d->keymode == DD_KEY_LOWBIT) key = key ? (u32)__builtin_ctz(key) : 32;
    DdEntry* e = dd_entry(op, key);
    u32 gen_grp = s_dd_gen[d->group];
    if (e->op == op && e->key == key && e->n == n && e->gen_all == s_dd_gen_all && e->gen_grp == gen_grp &&
        memcmp(e->w, w, n * 4) == 0)
        return true;
#if GX_PROFILE_DETAIL
    if (e->op != op || e->key != key) PROF_COUNT("rec.dedup_miss_slot", 1);
    else if (e->gen_all != s_dd_gen_all) PROF_COUNT("rec.dedup_miss_reset", 1);
    else if (e->gen_grp != gen_grp && d->group == DD_GRP_XF) PROF_COUNT("rec.dedup_miss_dlcall", 1);
    else if (e->gen_grp != gen_grp) PROF_COUNT("rec.dedup_miss_ind", 1);
    else PROF_COUNT("rec.dedup_miss_changed", 1);
#endif
    e->op = op;
    e->key = key;
    e->n = n;
    e->gen_all = s_dd_gen_all;
    e->gen_grp = gen_grp;
    memcpy(e->w, w, n * 4);
    if (d->keymode == DD_KEY_CHAN) {
        u32 m = dd_chan_mask(key);
        for (u32 c = 0; m && c < 6; c++) {
            if (c == key || !(dd_chan_mask(c) & m)) continue;
            DdEntry* o = dd_entry(op, c);
            if (o->op == op && o->key == c) o->gen_all = 0;
        }
    }
    return false;
}

#if GX_PROFILE_DETAIL
// Verify mode, render side: the last replayed arguments per filtered state, to
// tell "something outside the commands changed the state" (same arguments
// replayed last) from "the two sides disagree on the last command"
static DdEntry s_rdd[1 << DD_SLOT_BITS];
static u32 s_dd_verify_fails[GXREC_OP_COUNT];

// gx_state_changed hook (verify mode): filtered state may only change under
// a filtered or a forget-everything command. Logs the first changes made
// under neutral / group commands or outside any replayed command
void gxrec_dd_check_change(void* where) {
    if (!(s_dd_mode & 8)) return;
    int op = g_gxrec_replay_op;
    if (op >= 0 && (s_dd_ops[op].kind == DD_FILTER || s_dd_ops[op].kind == DD_INVALIDATE)) return;
    PROF_COUNT("rec.dedup_outside_changes", 1);
    static void* seen[32];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == where) return;
    if (nseen < 32) seen[nseen++] = where;
    LOG_WARN("rt", "dedup verify: state changed by %p under %s (thread slot %d)", where,
             op >= 0 && gxrec_op_names[op] ? gxrec_op_names[op] : "no replayed command", (int)t_thread_slot);
}

static void dd_verify(u32 op, const u32* w, u32 n, bool marked, bool changed) {
    const DdOp* d = &s_dd_ops[op];
    if (d->kind != DD_FILTER || n > DD_MAX_WORDS) return;
    int kw = d->key_word >= 0 ? d->key_word : (int)n + d->key_word;
    if (kw < 0 || kw >= (int)n) return;
    u32 key = d->keymode == DD_KEY_NONE ? 0 : w[kw];
    if (d->keymode == DD_KEY_LOWBIT) key = key ? (u32)__builtin_ctz(key) : 32;
    DdEntry* e = &s_rdd[((op * 0x9E3779B1u) ^ (key * 0x85EBCA77u)) >> (32 - DD_SLOT_BITS)];
    bool same_last = e->op == op && e->key == key && e->n == n && memcmp(e->w, w, n * 4) == 0;
    if (marked && changed) {
        if (same_last) PROF_COUNT("rec.dedup_verify_fail_outside", 1);
        else PROF_COUNT("rec.dedup_verify_fail_stream", 1);
        if (s_dd_verify_fails[op]++ < 3)
            LOG_WARN("rt", "dedup verify: %s changed state, last replayed args %s (args %lX %lX %lX ... %lX, %lu words)",
                     gxrec_op_names[op] ? gxrec_op_names[op] : "?", same_last ? "identical" : "different",
                     (unsigned long)(n > 0 ? w[0] : 0), (unsigned long)(n > 1 ? w[1] : 0),
                     (unsigned long)(n > 2 ? w[2] : 0), (unsigned long)(n > 0 ? w[n - 1] : 0), (unsigned long)n);
    }
    e->op = op;
    e->key = key;
    e->n = n;
    memcpy(e->w, w, n * 4);
}
#endif

u32* gxrec_begin(u32 op, u32 words) {
    g_ptag_op = (u8)op;  // Probe sampler (port/ptag.h)
    g_ptag_op_calls[op & 0xFF]++;
    if (s_imm) close_imm();
    // A recorded command draws with the state of the setters before it: flush
    // the pending delta and matrix block first (no-op outside delta mode)
    if (op != GXREC_OP_DELTA && op != GXREC_OP_MBLOCK) gxrec_delta_flush();
    reserve(1 + words);
    s_ring[s_w] = HDR(op, words);
    return &s_ring[s_w + 1];
}

void gxrec_end(u32* end) {
    if (s_dd_on) {
        PTAG_SUB(PSUB_FILTER);
        u32 h = s_ring[s_w];
        if (dd_redundant(HDR_OP(h), &s_ring[s_w + 1], HDR_WORDS(h))) {
            GX_PROF_COUNT("rec.dedup_dropped", 1);
#if GX_PROFILE_DETAIL
            if (s_dd_mode & 8) s_ring[s_w] = h | HDR_DD_MARK;  // Verify: replay it anyway
            else
#endif
            return;  // Not committed: the next command overwrites it
        }
    }
    s_w = (u32)(end - s_ring);
    if (++s_unpublished >= PUBLISH_EVERY) publish();
}

static void close_imm(void) {
    if (!s_imm) return;
    PTAG_SUB(PSUB_IMM);
    u32 bytes = (u32)(s_imm_ptr - (u8*)(s_imm + 2));
    u32 words = 1 + (bytes + 3) / 4;
    s_imm[0] = HDR(GXREC_OP_IMM, words);
    s_imm[1] = bytes;
    s_w = (u32)(s_imm - s_ring) + 1 + words;
    s_imm = NULL;
    if (++s_unpublished >= PUBLISH_EVERY) publish();
}

static void open_imm(void) {
    PTAG_SUB(PSUB_IMM);
    reserve(2 + IMM_CHUNK_WORDS);
    s_imm = &s_ring[s_w];
    s_imm_ptr = (u8*)(s_imm + 2);
    s_imm_end = s_imm_ptr + IMM_CHUNK_WORDS * 4;
}

// Display list being recorded (GXBeginDisplayList): bytes go into game memory
static void dl_emit(const void* data, u32 n) {
    if (s_dl.len + n > s_dl.size) {
        s_dl.overflow = true;
        return;
    }
    memcpy(s_dl.buf + s_dl.len, data, n);
    s_dl.len += n;
}

static inline void rec_emit(const void* data, u32 n) {
    if (s_dl.on) {
        dl_emit(data, n);
        return;
    }
    if (!s_imm || s_imm_ptr + n > s_imm_end) {
        close_imm();
        open_imm();
    }
    memcpy(s_imm_ptr, data, n);
    s_imm_ptr += n;
}

// --- Modes, frame boundary, direct sections --------------------------------------

static void refresh_shadows(void) {
    s_dd_gen_all++;  // GX ran outside the ring: forget the recorded state
    gxrec_delta_refresh();  // Shadow, vertex descriptors, arrays, scissor
    s_hud = gx_hud_active();
    s_off = gx_offscreen_active();
    s_rec_frame = gx_frame_is_active();
    s_rec_stereo = gx_frame_stereo();
}

static bool alloc_ring(void) {
    if (s_ring) return true;
    // 128 KB: a ring as large as the 2 MB shared L2 evicted everything else
    // (hardware A/B in MULTICORE_PLAN.md: 2048 -> 256 KB in build 36F640FC,
    // 256 -> 128 KB in build 654B271E)
    int kb = cfg_int("S3D_RT_RING_KB", 128);
    if (kb < 64) kb = 64;
    s_size = (u32)kb * 256;
    int wake_kb = cfg_int("S3D_RT_WAKE_KB", 64);
    if (wake_kb < 0) wake_kb = 0;
    s_space_block = (u32)wake_kb * 256;
    if (s_space_block > s_size / 4) s_space_block = s_size / 4;
    s_ring = malloc(s_size * 4);
    if (!s_ring) {
        LOG_ERROR("rt", "no memory for the %d KB command ring", kb);
        return false;
    }
    s_w = s_read.idx = s_pub.idx = s_read_seen = 0;
    return true;
}

static bool start_thread(void) {
    static bool failed;
    if (s_thread) return true;
    if (failed) return false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_quit = false;
    // Alone on core 2 (the worker moved to core 1); above the worker in case
    // S3D_WORKER_CORE=2 puts it back
    s_thread = threadCreate(render_main, NULL, 128 * 1024, prio - 2, 2, false);
    if (!s_thread) {
        failed = true;
        LOG_WARN("rt", "cannot create the render thread on core 2: S3D_RENDER_THREAD=2 runs as 1");
        return false;
    }
    LOG_INFO("rt", "render thread on core 2 (priority 0x%lX)", (unsigned long)(prio - 2));
    return true;
}

// Suspend (HOME menu), sleep and exit hand the GPU to the system inside
// aptMainLoop, which the game thread calls while an overlapped frame may still
// be replaying: finish it first. aptHook puts this hook in front of citro3d's.
// The overlapped frame is also still open (its boundary is recorded after
// aptMainLoop), and a frame open across a suspend never completes on the GPU
// afterwards (hardware, F767D896: the next gx_gpu_wait_previous hung after
// HOME): end it here, as the non-overlapped frame end does before aptMainLoop.
// The recorded boundary then skips the touch UI and the frame end (no frame
// open) and only begins the next frame.
static void apt_hook(APT_HookType hook, void* param) {
    (void)param;
    if (hook != APTHOOK_ONSUSPEND && hook != APTHOOK_ONSLEEP && hook != APTHOOK_ONEXIT) return;
    if (!t_producer) {
        LOG_WARN("rt", "APT hook %d on another thread: render thread not drained", (int)hook);
        return;
    }
    if (!g_gxrec_on || t_gxrec_direct) return;
    sync_all();
    if (s_mode == 2 && gx_frame_is_active()) {
        bool was = t_gxrec_direct;
        t_gxrec_direct = true;
        gx_frame_end();
        t_gxrec_direct = was;
    }
    LOG_INFO("rt", "APT %s: render thread drained", hook == APTHOOK_ONSUSPEND ? "suspend" : hook == APTHOOK_ONSLEEP ? "sleep" : "exit");
}

void gxrec_init(void) {
    LightEvent_Init(&s_ev_work, RESET_ONESHOT);
    LightEvent_Init(&s_ev_progress, RESET_ONESHOT);
    t_producer = true;
    aptHook(&s_apt_hook, apt_hook, NULL);
}

int gxrec_mode(void) { return s_mode; }

// gx_rec_delta.c
int gxrec_dedup_mode(void) { return s_dd_mode; }
bool gxrec_delta_dl_recording(void) { return s_dl.on; }

static bool overlap_wanted(void) {
    static int on = -1;
    CFG_LIVE(on, cfg_bool("S3D_RT_OVERLAP", true) ? 1 : 0);
    return on != 0;
}

bool gxrec_overlap_frame_end(void) {
    return g_gxrec_on && !t_gxrec_direct && s_mode == 2 && s_thread_on && !s_dl.on && overlap_wanted();
}

void gxrec_record_frame(bool black, bool reset, float slider) {
    s_frames_recorded++;
    u32* p = gxrec_begin(GXREC_OP_FRAME, 3);
    p[0] = black;
    p[1] = reset;
    memcpy(&p[2], &slider, sizeof(slider));
    gxrec_end(p + 3);
    publish();
    ft_ev(FT_G_REC, 0);
    s_frame_recorded = true;
    // What the next frame's answers need, as the render thread will open it
    // (gx_frame_begin_slider); the frame end closes any offscreen pass
    s_rec_frame = gx_top_target() != NULL;
    s_rec_stereo = slider > 0.0f;
    s_off = false;
#if GX_PROFILE_DETAIL
    s_op_frames++;
#endif
}

void gxrec_fence(void) {
    if (!t_producer || s_frames_done == s_frames_recorded) return;
    PROF_ZONE("rt.fence_wait");
    ft_ev(FT_G_FENCE_B, 0);
    volatile const char* phase = g_main_phase;
    WD_PHASE("rt: waiting for the previous frame (fence)");
    publish();
    // Woken by the boundary itself, or when the replay reaches the end
    while (s_frames_done != s_frames_recorded) wait_progress(s_read.idx, s_size);
    ft_ev(FT_G_FENCE_E, 0);
    __dmb();  // What the render thread wrote is visible from here on
    g_main_phase = phase;
}

// The game frees through mm_free: with its free delay (2 ticks, mmFreeTick at
// the end of gameLoop) the memory outlives one frame of render-thread lag, but
// sections that set the delay to 0 (attract mode, map unloads, some objects)
// free at once, so those wait for the previous frame first
extern int gMmFreeDelay;
void __real_mm_free(void* p);
void __wrap_mm_free(void* p) {
    if (gMmFreeDelay == 0 && s_frames_done != s_frames_recorded) gxrec_fence();
    __real_mm_free(p);
}

// Opcode name for the probe's recorder report (port/probe_3ds.c)
const char* gxrec_op_name(u32 op) {
    if (op == GXREC_OP_IMM) return "immediate-mode data";
    if (op < GXREC_OP_COUNT && gxrec_op_names[op]) return gxrec_op_names[op];
    return "(other)";
}

void gxrec_report_drain(void) {
    if (!g_gxrec_on || t_gxrec_direct) return;
    sync_all();
#if GX_PROFILE_DETAIL
    ops_report(false);
#endif
}

void gxrec_ops_report_drain(void) {
#if GX_PROFILE_DETAIL
    gxrec_report_drain();
#endif
}

void gxrec_frame_begin(void) {
    static int want = -1;
    CFG_LIVE(want, cfg_int("S3D_RENDER_THREAD", RT_DEFAULT_MODE));
    dd_init();
    CFG_LIVE(s_dd_mode, cfg_int("S3D_RT_DEDUP", 5));
    bool delta_change = gxrec_delta_switch_pending();
    if (s_dd_on != ((s_dd_mode & 2) != 0)) {
        s_dd_on = (s_dd_mode & 2) != 0;
        s_dd_gen_all++;  // The table may not answer from before the switch
    }
    int mode = want < 0 ? 0 : want > 2 ? 2 : want;
    // The GPU trace / sync debug modes (gx_init.c) time each operation on the
    // game thread
    static int debug_gpu = -1;
    if (debug_gpu < 0) debug_gpu = cfg_int("S3D_GPU_TRACE", 0) > 0 || gx_gpu_sync() != 0;
    if (debug_gpu) mode = 0;
    if (mode && !alloc_ring()) mode = 0;
    if (mode == 2 && !start_thread()) mode = 1;
    if (s_frame_recorded) {
        // The render thread runs the boundary that opens this frame: keep
        // recording into the ring as it is. A delta-mode switch drains first
        // (the pending delta must flush with the old mode, and the shadow
        // must be refreshed from the real state before recording resumes)
        s_frame_recorded = false;
        if (mode == s_mode && overlap_wanted() && !delta_change) {
            CFG_LIVE(s_prefetch, cfg_bool("S3D_RT_PREFETCH", true));
            s_dl.on = false;
            s_direct_depth = 0;
            s_direct_set = false;
            t_gxrec_direct = false;
            gxrec_delta_frame_begin();
            return;
        }
        // Switching: finish the previous frame and the boundary first, then
        // continue below with the render thread idle
        sync_all();
    }
    gxrec_delta_frame_begin();
    if (mode != s_mode) {
        gxrec_forget_answers();
        static const char* const names[] = { "off", "recorded, replayed at the frame end", "render thread on core 2" };
        LOG_INFO("rt", "render thread mode %d: %s", mode, names[mode]);
    }
    // The ring is empty and the render thread idle here (drained at the frame
    // end): it only consumes in mode 2, from the current position
    s_pub.idx = s_w;
    __dmb();
    s_thread_on = mode == 2;
    CFG_LIVE(s_prefetch, cfg_bool("S3D_RT_PREFETCH", true));
    s_mode = mode;
    g_gxrec_on = mode != 0;
    if (g_gxrec_on) refresh_shadows();
    s_dl.on = false;
    s_direct_depth = 0;
    s_direct_set = false;
    t_gxrec_direct = false;
}

void gxrec_frame_end(void) {
    if (!g_gxrec_on) return;
    {
        PROF_ZONE("rt.frame_end_wait");
        sync_all();
    }
#if GX_PROFILE_DETAIL
    ops_report(true);
#endif
    t_gxrec_direct = true;  // Until gxrec_frame_begin: the frame boundary runs GX directly
}

void gxrec_sync_direct_begin(const char* counter, int* counter_id) {
    if (*counter_id < 0) *counter_id = prof_register_counter(counter);
    prof_add(*counter_id, 1);
    PROF_COUNT("rt.syncs", 1);
    sync_all();
    t_gxrec_direct = true;
}

void gxrec_sync_direct_end(void) {
    refresh_shadows();
    t_gxrec_direct = false;
}

void gxrec_direct_begin(void) {
    if (s_direct_depth++ > 0) return;
    s_direct_set = gxrec_recording();
    if (!s_direct_set) return;
    PROF_COUNT("rt.syncs", 1);
    PROF_COUNT("rt.sync.port", 1);
    sync_all();
    t_gxrec_direct = true;
}

void gxrec_direct_end(void) {
    if (s_direct_depth <= 0 || --s_direct_depth > 0 || !s_direct_set) return;
    s_direct_set = false;
    refresh_shadows();
    t_gxrec_direct = false;
}

void gxrec_shutdown(void) {
    if (g_gxrec_on && !t_gxrec_direct) sync_all();
    g_gxrec_on = false;
    if (!s_thread) return;
    s_thread_on = false;
    s_quit = true;
    __dmb();
    LightEvent_Signal(&s_ev_work);
    threadJoin(s_thread, 1000000000ULL);  // 1 s: a hung GPU must not keep the app from exiting
    threadFree(s_thread);
    s_thread = NULL;
}

// --- Hand-written wrappers ("manual" and "wrap" entries in gxrec.list) -------------

#ifdef PERF_WRAP
#define TIMED_DRAW(counter, call) do { \
        draw_src_set_caller(__builtin_return_address(0)); \
        u64 t0_ = svcGetSystemTick(); \
        call; \
        PROF_COUNT(counter, svcGetSystemTick() - t0_); \
        draw_src_set_caller(NULL); \
    } while (0)
#else
#define TIMED_DRAW(counter, call) call
#endif

// The display list cache's game-memory hashes used to be computed on the
// game thread while the render thread was the bottleneck. The probe showed
// the game thread now records ~750 unique lists per frame (~8 us each), so
// the hashing moved to the render thread, which has CPU headroom under the
// GPU: the recorded call carries mask 0 and the render side hashes the list
// and the arrays itself (gx_dl_call_hashed, gx_dlcache.c)
// The game-side model-draw seam (RENDERER_CHANNEL.md stage B): the patched
// GXCallDisplayList sites in the object renderer call this directly. v1
// records the draw exactly like the GX wrapper below; the next step embeds
// the pending state delta and matrix block into this one op.
void port_model_draw(const void* list, u32 nbytes) {
    PTAG_LEAF(PL_GXDRAW);
    g_ptag_op = GXREC_OP_MODELDRAW;
    u8 scope = g_ptag_scope[THREAD_SLOT_GAME];
    g_ptag_dl_calls[scope]++;
    g_ptag_dl_bytes[scope] += nbytes;
    if (!gxrec_recording()) {
        GXCallDisplayList(list, nbytes);
        return;
    }
    if (!list || nbytes == 0) return;
    // One op per draw: the pending matrix block and state delta travel
    // inside the model-draw op instead of as separate MBLOCK/DELTA ops
    u32 mw, dw;
    const u32* st = gxrec_delta_take(&mw, &dw);
    u32 words = 4 + mw + dw;
    u32* p = gxrec_begin(GXREC_OP_MODELDRAW, words);
    p[0] = (u32)(uintptr_t)list;
    p[1] = nbytes | (scope == PS_MAP ? 0x80000000u : 0);
    p[2] = mw;
    p[3] = dw;
    memcpy(p + 4, st, (mw + dw) * 4);
    gxrec_end(p + words);
    publish();  // Draws are where the render thread should get going
}

void __wrap_GXCallDisplayList(const void* list, u32 nbytes) {
    PTAG_LEAF(PL_GXDRAW);
    g_ptag_op = GXREC_OP_gx_dl_call_hashed;
    u8 scope = g_ptag_scope[THREAD_SLOT_GAME];
    g_ptag_dl_calls[scope]++;
    g_ptag_dl_bytes[scope] += nbytes;
    if (!gxrec_recording()) {
        TIMED_DRAW("perf.GXCallDisplayList", __real_GXCallDisplayList(list, nbytes));
        return;
    }
    if (!list || nbytes == 0) return;
    // The list and array hashes are computed on the render thread now (the
    // game thread recorded ~750 unique lists per frame at ~8 us each, ~6 ms
    // of its recorder cost, while the render thread has CPU headroom under
    // the GPU). mask 0 tells the render side to hash everything itself.
    u32 h[1];
    h[0] = 0;
    // Bit 31 of the size marks map-block calls (map census, gx_dlcache.c)
    gxrec_put_gx_dl_call_hashed(list, nbytes | (scope == PS_MAP ? 0x80000000u : 0), 0, h);
    publish();  // Draws are where the render thread should get going
}

void __wrap_GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    PTAG_LEAF(PL_GXDRAW);
    g_ptag_begin_verts[g_ptag_scope[THREAD_SLOT_GAME]] += nverts;
    if (!gxrec_recording()) {
        __real_GXBegin(type, vtxfmt, nverts);
        return;
    }
    if (s_dl.on) {
        u8 b[3] = { (u8)((u8)type | (vtxfmt & 7)), (u8)(nverts >> 8), (u8)nverts };
        dl_emit(b, 3);
        return;
    }
    gxrec_put_GXBegin(type, vtxfmt, nverts);
}

void __wrap_GXEnd(void) {
    PTAG_LEAF(PL_GXDRAW);
    if (!gxrec_recording()) {
        TIMED_DRAW("perf.GXEnd", __real_GXEnd());
        return;
    }
    if (s_dl.on) return;
    gxrec_put_GXEnd();
}

void __wrap_GXBeginDisplayList(void* list, u32 size) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) {
        __real_GXBeginDisplayList(list, size);
        return;
    }
    s_dl.buf = list;
    s_dl.size = size;
    s_dl.len = 0;
    s_dl.on = true;
    s_dl.overflow = false;
}

// Same result as gx_vtx.c's GXEndDisplayList
u32 __wrap_GXEndDisplayList(void) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) return __real_GXEndDisplayList();
    if (!s_dl.on) {
        // Begun before recording started (a mode switch in between)
        static int id = -1;
        gxrec_sync_direct_begin("rt.sync.GXEndDisplayList", &id);
        u32 n = __real_GXEndDisplayList();
        gxrec_sync_direct_end();
        return n;
    }
    s_dl.on = false;
    if (s_dl.overflow) {
        LOG_WARN("gx", "display list overflow (buffer 0x%lX bytes)", (unsigned long)s_dl.size);
        return 0;
    }
    while ((s_dl.len & 31) && s_dl.len < s_dl.size) s_dl.buf[s_dl.len++] = 0;
    return s_dl.len;
}

// Nothing reads the normal matrices: the vertex shader transforms normals with
// the position matrix (gx_state.c). S3D_RT_DEDUP bit 2 stops recording them
// (~1,350 per hub frame, 14 words each); the generated delta wrapper consults
// gxrec_delta_GXLoadNrmMtxImm, which also applies that skip when delta is off.

// The scissor only changes through GXSetScissor (display lists cannot set it:
// gx_vtx.c ignores BP writes) and GXInit (a sync call, which refreshes it)
void __wrap_GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) {
        __real_GXGetScissor(left, top, wd, ht);
        return;
    }
    u32 sc[4];
    gxrec_delta_get_scissor(sc);
    if (left) *left = sc[0];
    if (top) *top = sc[1];
    if (wd) *wd = sc[2];
    if (ht) *ht = sc[3];
}

// gx_hud_begin's answer: the bottom screen exists, a frame is open (both fixed
// during a frame) and no HUD or offscreen pass is open (tracked here)
bool __wrap_gx_hud_begin(void) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) return __real_gx_hud_begin();
    if (s_hud || s_off || !s_rec_frame || !gx_hud_enabled()) return false;
    s_hud = true;
    gxrec_put_gx_hud_begin();
    return true;
}

void __wrap_gx_hud_end(void) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) {
        __real_gx_hud_end();
        return;
    }
    s_hud = false;
    gxrec_put_gx_hud_end();
}

// gx_offscreen_check answers from the settings, the frame and the targets
// created so far; the first use of a target size creates it (VRAM), which is
// done directly after a drain
bool __wrap_gx_offscreen_begin(float x, float y, float w, float h, int res) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) return __real_gx_offscreen_begin(x, y, w, h, res);
    if (s_hud || s_off) return false;
    int ok = s_rec_frame ? gx_offscreen_check(w, h, res, s_rec_stereo) : 0;
    if (ok == 0) return false;
    if (ok < 0) {
        static int id = -1;
        gxrec_sync_direct_begin("rt.sync.gx_offscreen_begin", &id);
        bool r = __real_gx_offscreen_begin(x, y, w, h, res);
        gxrec_sync_direct_end();
        return r;
    }
    s_off = true;
    gxrec_put_gx_offscreen_begin(x, y, w, h, res);
    return true;
}

void __wrap_gx_offscreen_end(void) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) {
        __real_gx_offscreen_end();
        return;
    }
    s_off = false;
    gxrec_put_gx_offscreen_end();
}

// gx_render.c's answers: two eyes while a stereo frame draws, one during HUD
// and offscreen passes (gx_render_set_stereo calls in gx_init.c)
bool __wrap_port_stereo_active(void) {
    if (!gxrec_recording()) return __real_port_stereo_active();
    return s_rec_frame && s_rec_stereo && !s_hud && !s_off;
}

bool __wrap_port_stereo_reduced(void) {
    if (!gxrec_recording()) return __real_port_stereo_reduced();
    return s_rec_frame && s_rec_stereo && !s_hud && !s_off && gx_render_eye_reduced();
}

// drawReflectionTexture asks every frame whether its small copy can alias a
// mip level of the big one (gx_copy.c). The answer needs the big copy's entry
// (render side), but once it was true it stays true while the game keeps
// copying into src every frame: later calls with the same arguments answer
// true here and only record the call (the replay updates the alias). The
// remembered answers are dropped when the GX caches are reset.
#define ALIAS_ANSWERS 8
static struct {
    const void* dest;
    const void* src;
    int w;
} s_alias_ok[ALIAS_ANSWERS];
static int s_alias_next;

void gxrec_forget_answers(void) {
    memset(s_alias_ok, 0, sizeof(s_alias_ok));
}

bool __wrap_port_copytex_mip_alias(const void* dest, const void* src, int w) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) return __real_port_copytex_mip_alias(dest, src, w);
    for (int i = 0; i < ALIAS_ANSWERS; i++) {
        if (s_alias_ok[i].dest == dest && s_alias_ok[i].src == src && s_alias_ok[i].w == w && dest) {
            gxrec_put_port_copytex_mip_alias(dest, src, w);
            PROF_COUNT("rt.answered.mip_alias", 1);
            return true;
        }
    }
    static int id = -1;
    gxrec_sync_direct_begin("rt.sync.port_copytex_mip_alias", &id);
    bool r = __real_port_copytex_mip_alias(dest, src, w);
    gxrec_sync_direct_end();
    if (r) {
        s_alias_ok[s_alias_next].dest = dest;
        s_alias_ok[s_alias_next].src = src;
        s_alias_ok[s_alias_next].w = w;
        s_alias_next = (s_alias_next + 1) % ALIAS_ANSWERS;
    }
    return r;
}

// Ending a scope (level 0), or with the GPU blur off, always answers false;
// starting one needs the copy texture of dest (render-side)
bool __wrap_port_copytex_blur_scope(const void* dest, int level) {
    PTAG_LEAF(PL_GXCMD);
    if (!gxrec_recording()) return __real_port_copytex_blur_scope(dest, level);
    if (level <= 0 || !dest || !cfg_bool("S3D_GPU_BLUR", true)) {
        gxrec_put_port_copytex_blur_scope(dest, level);
        return false;
    }
    static int id = -1;
    gxrec_sync_direct_begin("rt.sync.port_copytex_blur_scope", &id);
    bool r = __real_port_copytex_blur_scope(dest, level);
    gxrec_sync_direct_end();
    return r;
}

// --- Immediate-mode vertex data (same bytes as gx_vtx.c's emit) ---------------------

static inline u8* be16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; return p + 2; }
static inline u8* be32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; return p + 4; }
static inline u8* bef(u8* p, f32 f) { u32 v; memcpy(&v, &f, 4); return be32(p, v); }
static inline u8* b8(u8* p, u8 v) { *p = v; return p + 1; }

#define VTX(name, params, args, ...) \
    void __wrap_##name params { \
        PTAG_LEAF(PL_GXVTX); \
        g_ptag_imm_calls[g_ptag_scope[THREAD_SLOT_GAME]]++; \
        if (!gxrec_recording()) { \
            __real_##name args; \
            return; \
        } \
        u8 b[12], *q = b; \
        __VA_ARGS__; \
        rec_emit(b, (u32)(q - b)); \
    }

VTX(GXPosition3f32, (f32 x, f32 y, f32 z), (x, y, z), q = bef(q, x); q = bef(q, y); q = bef(q, z))
VTX(GXPosition3u16, (u16 x, u16 y, u16 z), (x, y, z), q = be16(q, x); q = be16(q, y); q = be16(q, z))
VTX(GXPosition3s16, (s16 x, s16 y, s16 z), (x, y, z), q = be16(q, x); q = be16(q, y); q = be16(q, z))
VTX(GXPosition3u8, (u8 x, u8 y, u8 z), (x, y, z), q = b8(q, x); q = b8(q, y); q = b8(q, z))
VTX(GXPosition3s8, (s8 x, s8 y, s8 z), (x, y, z), q = b8(q, x); q = b8(q, y); q = b8(q, z))
VTX(GXPosition2f32, (f32 x, f32 y), (x, y), q = bef(q, x); q = bef(q, y))
VTX(GXPosition2u16, (u16 x, u16 y), (x, y), q = be16(q, x); q = be16(q, y))
VTX(GXPosition2s16, (s16 x, s16 y), (x, y), q = be16(q, x); q = be16(q, y))
VTX(GXPosition2u8, (u8 x, u8 y), (x, y), q = b8(q, x); q = b8(q, y))
VTX(GXPosition2s8, (s8 x, s8 y), (x, y), q = b8(q, x); q = b8(q, y))
VTX(GXPosition1x16, (u16 index), (index), q = be16(q, index))
VTX(GXPosition1x8, (u8 index), (index), q = b8(q, index))
VTX(GXNormal3f32, (f32 x, f32 y, f32 z), (x, y, z), q = bef(q, x); q = bef(q, y); q = bef(q, z))
VTX(GXNormal3s16, (s16 x, s16 y, s16 z), (x, y, z), q = be16(q, x); q = be16(q, y); q = be16(q, z))
VTX(GXNormal3s8, (s8 x, s8 y, s8 z), (x, y, z), q = b8(q, x); q = b8(q, y); q = b8(q, z))
VTX(GXNormal1x16, (u16 index), (index), q = be16(q, index))
VTX(GXNormal1x8, (u8 index), (index), q = b8(q, index))
VTX(GXColor4u8, (u8 r, u8 g, u8 b_, u8 a), (r, g, b_, a), q = b8(q, r); q = b8(q, g); q = b8(q, b_); q = b8(q, a))
VTX(GXColor3u8, (u8 r, u8 g, u8 b_), (r, g, b_), q = b8(q, r); q = b8(q, g); q = b8(q, b_))
VTX(GXColor1u32, (u32 clr), (clr), q = be32(q, clr))
VTX(GXColor1u16, (u16 clr), (clr), q = be16(q, clr))
VTX(GXColor1x16, (u16 index), (index), q = be16(q, index))
VTX(GXColor1x8, (u8 index), (index), q = b8(q, index))
VTX(GXTexCoord2f32, (f32 s, f32 t), (s, t), q = bef(q, s); q = bef(q, t))
VTX(GXTexCoord2u16, (u16 s, u16 t), (s, t), q = be16(q, s); q = be16(q, t))
VTX(GXTexCoord2s16, (s16 s, s16 t), (s, t), q = be16(q, s); q = be16(q, t))
VTX(GXTexCoord2u8, (u8 s, u8 t), (s, t), q = b8(q, s); q = b8(q, t))
VTX(GXTexCoord2s8, (s8 s, s8 t), (s, t), q = b8(q, s); q = b8(q, t))
VTX(GXTexCoord1f32, (f32 s), (s), q = bef(q, s))
VTX(GXTexCoord1u16, (u16 s), (s), q = be16(q, s))
VTX(GXTexCoord1s16, (s16 s), (s), q = be16(q, s))
VTX(GXTexCoord1u8, (u8 s), (s), q = b8(q, s))
VTX(GXTexCoord1s8, (s8 s), (s), q = b8(q, s))
VTX(GXTexCoord1x16, (u16 index), (index), q = be16(q, index))
VTX(GXTexCoord1x8, (u8 index), (index), q = b8(q, index))
