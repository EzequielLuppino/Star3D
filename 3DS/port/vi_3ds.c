// VI (video interface) for 3DS, replacing foxhollow's port/src/vi_shim.c.
// VIWaitForRetrace() is the game's frame boundary: it presents the frame that
// was just drawn, waits for the 3DS VBlank and opens the next frame.

#include <3ds.h>
#include <dolphin/types.h>
#include <dolphin/vi.h>
#include "diag_3ds.h"
#include "frametrace_3ds.h"
#include "config.h"
#include "gx_init.h"
#include "gx_internal.h"
#include "gx_rec.h"
#include "log.h"
#include "netsrv_3ds.h"
#include "remote_3ds.h"
#include "worker.h"
#include "port_3ds.h"
#include "profiler.h"
#include "perf/draw_src.h"
#include "touchui_3ds.h"
#include "watchdog.h"

typedef void (*VIRetraceCallback)(u32 retraceCount);

static VIRetraceCallback sPreRetraceCallback;
static VIRetraceCallback sPostRetraceCallback;
static void* sNextFrameBuffer;
static u32 sRetraceCount;
static bool sFrameOpen;
static bool sBlack;
static bool sGxReset;

// A save state was restored: the texture and display list caches are keyed by
// game memory addresses, so they are rebuilt once the GPU is done with them
void port_gx_request_reset(void) { sGxReset = true; }

void fhGXCompleteFrame(void);

void port_skin_flush(void);

void port_frame_pump_init(void) {
    gx_frame_begin();
    gxrec_frame_begin();
    prof_frame_begin();
    sFrameOpen = true;
}

// After a save state load (port_gx_request_reset)
static void gx_caches_reset(void) {
    gx_gpu_wait_idle();  // Pipelined, the previous frame may still use the caches
    gx_tex_deinit();
    gx_tex_init();
    gx_dlcache_deinit();
    gx_dlcache_init();
    LOG_INFO("vi", "GX caches rebuilt after a save state load");
}

// The frame boundary as the render thread runs it when the frame end overlaps
// (gx/gx_rec.c, GXREC_OP_FRAME): the touch UI, the frame end and the next
// frame's begin, in the order VIWaitForRetrace runs them otherwise
// Zones frame.touchui / frame.end / frame.begin time the same parts on either
// thread, so overlapped and non-overlapped logs compare directly
static void frame_touchui(void) {
    PROF_ZONE("frame.touchui");
    touchui_draw();
}

static void frame_end(void) {
    PROF_ZONE("frame.end");
    gx_frame_end();
}

void port_frame_boundary(bool black, bool reset, float slider) {
    PROF_ZONE("frame.boundary");
    ft_ev(FT_R_BND_B, 0);
    WD_PHASE("rt: touchui_draw");
    frame_touchui();
    WD_PHASE("rt: gx_frame_end (C3D_FrameEnd)");
    frame_end();
    gx_set_clear_black(black);
    WD_PHASE("rt: gx_frame_begin");
    {
        PROF_ZONE("frame.begin");
        gx_frame_begin_slider(slider);
    }
    if (reset) gx_caches_reset();
    ft_ev(FT_R_BND_E, 0);
}

// Game-thread time from the end of VIWaitForRetrace to sceneRender (counter
// game.logic_us) and from sceneRender to the next VIWaitForRetrace
// (game.post_us), both without the fence (port/frame_zones.c)
u64 g_frame_resume_tick, g_render_done_tick;
void probe_slice(int where);  // probe_3ds.c
void port_controls_frame(void);  // controls_3ds.c

// With the render thread (gx/gx_rec.c), the frame's GX calls are recorded.
// - Overlapped (mode 2, S3D_RT_OVERLAP): the frame boundary is recorded too
//   and run by the render thread (port_frame_boundary), so the game thread
//   goes on with the next frame while the render thread finishes this one.
//   It only waits for the previous boundary here (gxrec_fence), so the touch
//   UI input never runs while the render thread draws the menu.
// - Otherwise the frame end waits until everything is replayed, and from there
//   to the next frame's start everything here runs GX and citro3d directly.
void VIWaitForRetrace(void) {
    bool overlap = false;
    ft_ev(FT_G_VIW, 0);
    if (g_render_done_tick) {
        PROF_COUNT("game.post_us", (svcGetSystemTick() - g_render_done_tick) / CPU_TICKS_PER_USEC);
        g_render_done_tick = 0;
    }
    if (sFrameOpen) {
        WD_PHASE("vi: fhGXCompleteFrame");
        fhGXCompleteFrame();
        port_skin_flush();
        overlap = gxrec_overlap_frame_end();
        if (overlap) {
            WD_PHASE("vi: gxrec_fence (render thread)");
            gxrec_fence();
        } else {
            WD_PHASE("vi: gxrec_frame_end (render thread)");
            gxrec_frame_end();
            WD_PHASE("vi: touchui_draw");
            frame_touchui();
            WD_PHASE("vi: gx_frame_end (C3D_FrameEnd)");
            frame_end();
        }
        PROF_COUNT("worker.busy_us", (u64)(worker_take_busy_ticks() / CPU_TICKS_PER_USEC));
        DRAW_SRC_FRAME_END();
        // Only the per-opcode report needs the render thread idle: the
        // profiler's own report no longer does (profiler.c, s_buf)
        if (overlap && prof_report_due()) {
            WD_PHASE("vi: gxrec_ops_report_drain (render thread)");
            gxrec_ops_report_drain();
        }
        prof_frame_end();
        sFrameOpen = false;
        // Switches read lazily by the renderer are known by now
        if (prof_frame_count() == 600) cfg_log_summary();
    }

    // HOME menu "close" or a system shutdown ends the app
    WD_PHASE("vi: aptMainLoop");
    if (!aptMainLoop()) {
        LOG_INFO("vi", "aptMainLoop returned false after %lu retraces, exiting",
                 (unsigned long)sRetraceCount);
        port_shutdown_and_exit(0);
    }

    WD_PHASE("vi: input / touch UI");
    hidScanInput();
    // Hardware or remote (cmd.txt "press SC") START+SELECT quits. The remote
    // pad uses dolphin PAD bits (SELECT = PAD_BUTTON_SELECT), hardware KEY_*.
    {
        u32 hw = hidKeysHeld() & (KEY_START | KEY_SELECT);
        u16 rp = remote_pad_buttons & (PAD_BUTTON_START | PAD_BUTTON_SELECT);
        if (hw == (KEY_START | KEY_SELECT) || rp == (PAD_BUTTON_START | PAD_BUTTON_SELECT)) {
            LOG_INFO("vi", "START+SELECT pressed, exiting");
            port_shutdown_and_exit(0);
        }
        // SELECT alone marks the spot without a capture ("pmark" log line)
        if (hidKeysDown() & KEY_SELECT) diag_pmark();
    }
    // Net server restart (GET /restart): relaunch the app with the new build
    // (PUT /app.3dsx) through Rosalina's hb:ldr, then exit
    if (netsrv_restart_requested()) {
        LOG_INFO("vi", "restart requested over the network, exiting");
        netsrv_relaunch_app();
        port_shutdown_and_exit(0);
    }
    port_controls_frame();  // Gyro, C-stick camera (controls_3ds.c)
    touchui_input();
    touchui_frame();
    diag_frame();
    ft_frame();

    prof_frame_begin();
    if (overlap) {
        WD_PHASE("vi: gxrec_record_frame");
        gxrec_record_frame(sBlack, sGxReset, gx_frame_slider());
    } else {
        gx_set_clear_black(sBlack);
        WD_PHASE("vi: gx_frame_begin");
        {
            PROF_ZONE("frame.begin");
            gx_frame_begin();  // Waits for VBlank (C3D_FRAME_SYNCDRAW)
        }
        if (sGxReset) gx_caches_reset();
    }
    if (sGxReset) {
        sGxReset = false;
        gxrec_forget_answers();
    }
    sFrameOpen = true;
    gxrec_frame_begin();  // Recording starts or goes on (S3D_RENDER_THREAD)

    WD_PHASE("vi: fhAIPump");
    fhAIPump();

    sRetraceCount++;
    WD_PHASE("vi: retrace callbacks");
    if (sPreRetraceCallback) {
        sPreRetraceCallback(sRetraceCount);
    }
    if (sPostRetraceCallback) {
        sPostRetraceCallback(sRetraceCount);
    }
    probe_slice(0);  // S3D_PROBE (probe_3ds.c), outside game.logic_us
    WD_PHASE("game code");
    g_frame_resume_tick = svcGetSystemTick();
    ft_ev(FT_G_FRAME, sRetraceCount);
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback prev = sPreRetraceCallback;
    sPreRetraceCallback = cb;
    return prev;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback prev = sPostRetraceCallback;
    sPostRetraceCallback = cb;
    return prev;
}

void VISetNextFrameBuffer(void* fb) { sNextFrameBuffer = fb; }
void* VIGetNextFrameBuffer(void) { return sNextFrameBuffer; }
void VISetBlack(BOOL black) { sBlack = black ? true : false; }
u32 VIGetRetraceCount(void) { return sRetraceCount; }
u32 VIGetNextField(void) { return 0; }
u32 VIGetDTVStatus(void) { return 1; }
