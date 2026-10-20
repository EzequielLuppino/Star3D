// Coarse frame zones in every build (MULTICORE_PLAN.md step 1). One zone per
// call of a whole frame phase, so they cost nothing per draw:
//   game.sceneRender  the game's rendering pass (traversal plus, with
//                     S3D_RENDER_THREAD=0, the GX translation of its draws)
// The rendering pass rewrites memory the previous frame's draws read
// (skinned normals, sky stars, CPU textures), so with an overlapped frame end
// it first waits for the render thread to finish that frame (gxrec_fence,
// zone rt.fence_wait, outside game.sceneRender).
// Everything else in the frame (logic, loads, audio, text) is the frame time
// minus game.sceneRender. With S3D_RENDER_THREAD=1 the GX translation runs at
// the frame end instead and is timed by rt.replay (gx/gx_rec.c), so one log
// splits the frame into game logic, traversal plus recording, and GX.
//
// PERF_WRAP builds (make PROFILE=1) keep their perf.sceneRender timer and
// draw-source tag here, since a symbol can only be wrapped once.

#include <3ds.h>
#include "gx_rec.h"
#include "profiler.h"
#include "ptag.h"
#include "perf/draw_src.h"

#ifdef PERF_WRAP
#define TIMED(counter, call) do { \
        u64 t0_ = svcGetSystemTick(); \
        call; \
        PROF_COUNT(counter, svcGetSystemTick() - t0_); \
    } while (0)
#endif

void port_skin_flush(void);
void probe_slice(int where);  // probe_3ds.c (S3D_PROBE)

// Last sceneRender in ticks, and the same without inline replay and render
// thread waits (diag_3ds.c A/B lines)
u32 g_scene_render_ticks, g_scene_own_ticks;
extern u64 g_rt_game_wait_ticks;  // gx_rec.c

void __real_sceneRender(int a, int b, int c, int d, int e, int f);
extern u64 g_frame_resume_tick, g_render_done_tick;  // vi_3ds.c

void __wrap_sceneRender(int a, int b, int c, int d, int e, int f) {
    if (g_frame_resume_tick) {
        PROF_COUNT("game.logic_us", (svcGetSystemTick() - g_frame_resume_tick) / CPU_TICKS_PER_USEC);
        g_frame_resume_tick = 0;
    }
    gxrec_fence();
    probe_slice(1);
    u64 t0 = svcGetSystemTick(), w0 = g_rt_game_wait_ticks;
    g_ptag_scope[THREAD_SLOT_GAME] = PS_SCENE;  // Sampler scope (ptag.h), cleared below
    {
        PROF_ZONE("game.sceneRender");
#ifdef PERF_WRAP
        draw_src_push(DSRC_SCENE);
        TIMED("perf.sceneRender", __real_sceneRender(a, b, c, d, e, f));
        draw_src_pop();
#else
        __real_sceneRender(a, b, c, d, e, f);
#endif
        port_skin_flush();
    }
    g_ptag_scope[THREAD_SLOT_GAME] = PS_OUT;
    g_scene_render_ticks = (u32)(svcGetSystemTick() - t0);
    g_scene_own_ticks = g_scene_render_ticks - (u32)(g_rt_game_wait_ticks - w0);
    PROF_COUNT("game.scene_own_us", g_scene_own_ticks / CPU_TICKS_PER_USEC);
    probe_slice(2);
    g_render_done_tick = svcGetSystemTick();
}
