// citro3d setup for the GX emulation: render target for the top screen, the
// GX transform vertex shader, and frame begin/end (driven by VIWaitForRetrace).

#include "gx_init.h"
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "bench_shbin.h"
#include "default_shbin.h"
#include "hwtest_shbin.h"
#include "vbench_shbin.h"
#include "gx_c3d_ctx.h"
#include "gx_capture.h"
#include "gx_diag.h"
#include "gx_internal.h"
#include "log.h"
#include "profiler.h"
#include "watchdog.h"
#include "frametrace_3ds.h"

#define DISPLAY_TRANSFER_FLAGS \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// Render target for the top screen
static C3D_RenderTarget* top_target = NULL;

// Bottom screen (S3D_BOTTOM_HUD): the minimap is drawn here (gx_hud_begin);
// with the option off the bottom screen is the debug console
static C3D_RenderTarget* bottom_target = NULL;
static bool hud_active;
static bool offscreen_active;
static void offscreen_free(void);
static bool frame_stereo;
static float frame_sep;

// One big target is shared by the modes that need more than 240x400:
// - stereo 3D (S3D_STEREO): both eyes are drawn into it as 240x800, left eye
//   in memory rows 0-399 and right eye in rows 400-799. While the 3D slider
//   is up, top_target's buffers point at the left half (so EFB copies and
//   GXPeekZ read the left eye) and right_target, linked to the right eye
//   output, at the right half.
// - wide mode (S3D_WIDE_MODE): the 800x240 top screen mode, drawn as 240x800
//   through top_target. Not with the 3D slider up, nor on the Old 2DS.
// - antialiasing (S3D_AA): drawn at 480x800 and halved in both directions by
//   the display transfer. Used when neither of the others is.
// The big target is allocated at 480x800 for AA, or 240x800; its frameBuf
// size stays 240x800 so clearing and drawing on it work for stereo.
enum { TOP_MONO, TOP_STEREO, TOP_WIDE, TOP_AA };
static C3D_RenderTarget* big_target = NULL;
static C3D_RenderTarget* right_target = NULL;
static void* top_color;
static void* top_depth;
static int top_mode = TOP_MONO;
static bool wide_enabled, aa_enabled;
static float stereo_sep;  // Parallax at infinity per eye at full slider

// Reduced resolution (touch UI setting S3D_LOWRES): mono frames are drawn into
// a 128x256 texture target (about a third of the 240x400 pixels) and stretched
// over the top screen with bilinear filtering at frame end. Stereo, wide mode
// and antialiasing take precedence. It saves GPU fill and depth traffic on
// hardware; Azahar does not model GPU time, so it barely shows there.
#define LOWRES_W 128
#define LOWRES_H 256
static C3D_Tex lowres_tex;
static C3D_RenderTarget* lowres_target = NULL;
static bool lowres_failed;
static bool frame_lowres;
// Area of lowres_target the frame is drawn into (framebuffer width x height,
// i.e. screen height x width): S3D_LOWRES=1 the whole 128x256 texture (a
// third of the pixels), 2 half the resolution, 120x200 (a quarter of the
// pixels) through the viewport, from the framebuffer's origin corner
static int lowres_vw = LOWRES_W, lowres_vh = LOWRES_H;
static bool lowres_wanted(void);
static void lowres_present(void);

// Where the game's frame is drawn: top_target, or the low resolution target.
// While stereo, drawing binds big_target and top_target is its left eye half
static C3D_RenderTarget* game_target(void) {
    return frame_lowres ? lowres_target : top_target;
}
static C3D_RenderTarget* bind_target(void) {
    return frame_stereo ? big_target : game_target();
}

// Size of the game frame inside its target (the drawn area in half mode)
static int game_fb_w(void) {
    return frame_lowres ? lowres_vw : game_target()->frameBuf.width;
}

static int game_fb_h(void) {
    return frame_lowres ? lowres_vh : game_target()->frameBuf.height;
}

// Draw on the game's target again (after a HUD, UI or offscreen pass);
// C3D_FrameDrawOn resets the viewport to the whole target
static void draw_on_game(void) {
    C3D_FrameDrawOn(bind_target());
    if (frame_lowres) C3D_SetViewport(0, 0, lowres_vw, lowres_vh);
}

static DVLB_s* shader_dvlb = NULL;
// One program per vertex shader variant (DVLEs of default.shbin, see the
// Makefile); they share code and uniform registers, so switching is cheap
static shaderProgram_s shader_programs[GX_VSH_COUNT];
static int shader_variant;

static bool clear_black = false;  // VISetBlack
static bool frame_active = false;

static void setup_big_target(void);
static void set_top_mode(int mode);

static bool init_shader(void) {
    shader_dvlb = DVLB_ParseFile((u32*)default_shbin, default_shbin_size);
    if (!shader_dvlb) {
        LOG_ERROR("gx", "failed to parse vertex shader");
        return false;
    }

    if (shader_dvlb->numDVLE < GX_VSH_COUNT) {
        LOG_ERROR("gx", "vertex shader has %lu variants, expected %d", (unsigned long)shader_dvlb->numDVLE,
                  GX_VSH_COUNT);
        return false;
    }
    for (int i = 0; i < GX_VSH_COUNT; i++) {
        shaderProgramInit(&shader_programs[i]);
        shaderProgramSetVsh(&shader_programs[i], &shader_dvlb->DVLE[i]);
    }
    shader_variant = GX_VSH_LIT;
    C3D_BindProgram(&shader_programs[shader_variant]);

    // Uniform locations come from the vertex-lit variant, which declares all
    // of them; the others must place the ones they use at the same registers
    if (!gx_render_set_shader(shader_programs[GX_VSH_LIT].vertexShader)) return false;
    static const char* const shared[] = { "projection", "posmtx", "texmtx", "texsrc", "posscale", "texwarp",
                                          "tcaff" };
    // Bool b15 reads as set in vertex shaders on real hardware (lit models
    // drew their normals as color while nrmout sat there): keep it unused
    for (int v = 0; v < GX_VSH_COUNT; v++) {
        const DVLE_s* dvle = shader_programs[v].vertexShader->dvle;
        for (u32 i = 0; i < dvle->uniformTableSize; i++) {
            if (dvle->uniformTableData[i].endReg == 0x78 + 15) {
                LOG_ERROR("gx", "vertex shader variant %d: a bool uniform uses b15", v);
                return false;
            }
        }
    }
    for (int v = 1; v < GX_VSH_COUNT; v++) {
        for (int i = 0; i < (int)(sizeof(shared) / sizeof(shared[0])); i++) {
            s8 loc = shaderInstanceGetUniformLocation(shader_programs[v].vertexShader, shared[i]);
            s8 ref = shaderInstanceGetUniformLocation(shader_programs[GX_VSH_LIT].vertexShader, shared[i]);
            if (loc >= 0 && loc != ref) {
                LOG_ERROR("gx", "vertex shader variant %d: uniform %s at %d, not %d", v, shared[i], loc, ref);
                return false;
            }
        }
    }
    return true;
}

// Binds a vertex shader variant (GX_VSH_*). Bool uniforms are marked for
// upload again so the new program sees the current values.
void gx_shader_select(int variant) {
    if (variant == shader_variant) return;
    shader_variant = variant;
    C3D_BindProgram(&shader_programs[variant]);
    C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] = true;
    PROF_COUNT("gx.vsh_switches", 1);
}

bool gx_init(void) {
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) {
        LOG_ERROR("gx", "failed to initialize citro3d");
        return false;
    }

    if (!init_shader()) {
        C3D_Fini();
        return false;
    }

    gx_render_init();
    setup_display_targets();

    LOG_INFO("gx", "3DS graphics initialized");
    return top_target != NULL;
}

static bool s_perflog_on;  // GSP performance log enabled (gpu_perflog)

void gx_deinit(void) {
    gx_gpu_wait_idle();  // Pipelined, the last frame may still be drawing
    if (s_perflog_on) GSPGPU_SetPerfLogMode(false);
    gx_render_deinit();

    if (right_target) {
        C3D_RenderTargetDelete(right_target);  // Does not own the shared color buffer
        right_target = NULL;
    }
    if (top_target) {
        set_top_mode(TOP_MONO);
        C3D_RenderTargetDelete(top_target);
        top_target = NULL;
    }
    if (big_target) {
        C3D_RenderTargetDelete(big_target);
        big_target = NULL;
    }
    if (bottom_target) {
        C3D_RenderTargetDelete(bottom_target);
        bottom_target = NULL;
    }
    offscreen_free();
    if (lowres_target) {
        C3D_RenderTargetDelete(lowres_target);
        C3D_TexDelete(&lowres_tex);
        lowres_target = NULL;
    }

    if (shader_dvlb) {
        for (int i = 0; i < GX_VSH_COUNT; i++) shaderProgramFree(&shader_programs[i]);
        DVLB_Free(shader_dvlb);
        shader_dvlb = NULL;
    }

    C3D_Fini();
    LOG_INFO("gx", "3DS graphics deinitialized");
}

void setup_display_targets(void) {
    // Top screen target (main game display). The screen is physically
    // rotated, so the framebuffer is 240x400.
    top_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!top_target) {
        LOG_ERROR("gx", "failed to create top render target");
        return;
    }
    C3D_RenderTargetSetOutput(top_target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    top_color = top_target->frameBuf.colorBuf;
    top_depth = top_target->frameBuf.depthBuf;
    setup_big_target();

    if (cfg_bool("S3D_BOTTOM_HUD", true)) {
        bottom_target = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (!bottom_target) bottom_target = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
        if (bottom_target) {
            C3D_RenderTargetSetOutput(bottom_target, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
            LOG_INFO("gx", "bottom screen HUD ready");
        } else {
            LOG_WARN("gx", "bottom screen HUD unavailable: no VRAM for the target");
        }
    }
}

static bool is_old_2ds(void) {
    u8 model = 0;
    if (R_FAILED(cfguInit())) return false;
    bool r = R_SUCCEEDED(CFGU_GetSystemModel(&model)) && model == CFG_MODEL_2DS;
    cfguExit();
    return r;
}

static void setup_big_target(void) {
    bool stereo = cfg_bool("S3D_STEREO", true);
    wide_enabled = cfg_bool("S3D_WIDE_MODE", false);
    aa_enabled = cfg_bool("S3D_AA", false);
    if (!stereo) LOG_INFO("gx", "stereo 3D disabled (S3D_STEREO=0)");
    if (wide_enabled && is_old_2ds()) {
        LOG_INFO("gx", "wide mode unavailable on the Old 2DS");
        wide_enabled = false;
    }
    if (!stereo && !wide_enabled && !aa_enabled) return;

    if (aa_enabled) {
        big_target = C3D_RenderTargetCreate(480, 800, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (!big_target) {
            LOG_WARN("gx", "antialiasing unavailable: no VRAM for the 480x800 target");
            aa_enabled = false;
        }
    }
    if (!big_target) big_target = C3D_RenderTargetCreate(240, 800, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (stereo && big_target) right_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
    if (!big_target || (stereo && !right_target)) {
        LOG_WARN("gx", "stereo 3D, wide mode and antialiasing unavailable: no VRAM for the 240x800 target");
        if (big_target) C3D_RenderTargetDelete(big_target);
        if (right_target) C3D_RenderTargetDelete(right_target);
        big_target = right_target = NULL;
        wide_enabled = aa_enabled = false;
        return;
    }
    big_target->frameBuf.width = 240;
    big_target->frameBuf.height = 800;
    if (aa_enabled) LOG_INFO("gx", "antialiasing ready (480x800, downscaled)");
    if (wide_enabled) LOG_INFO("gx", "800x240 wide mode ready");
    if (!stereo) return;

    // The right eye output reads the lower half of the stereo target
    vramFree(right_target->frameBuf.colorBuf);
    right_target->ownsColor = false;
    right_target->frameBuf.colorBuf =
        (u8*)big_target->frameBuf.colorBuf + C3D_CalcColorBufSize(240, 400, GPU_RB_RGBA8);
    C3D_RenderTargetSetOutput(right_target, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);
    gfxSet3D(true);
    stereo_sep = cfg_int("S3D_STEREO_SEP_X1000", 25) / 1000.0f;
    LOG_INFO("gx", "stereo 3D ready (separation %.3f, convergence %d)", stereo_sep, cfg_int("S3D_STEREO_CONV", 80));
}

// Point top_target at its own buffers (mono) or the big target: its left eye
// half (stereo), all of it as 240x800 (wide) or as 480x800 (AA)
static void set_top_mode(int mode) {
    static const char* const names[] = { "mono", "stereo 3D", "800x240 wide", "antialiased" };
    if (mode == top_mode) return;
    bool big = mode != TOP_MONO;
    C3D_FrameBuf* fb = &top_target->frameBuf;
    fb->colorBuf = big ? big_target->frameBuf.colorBuf : top_color;
    fb->depthBuf = big ? big_target->frameBuf.depthBuf : top_depth;
    fb->width = mode == TOP_AA ? 480 : 240;
    fb->height = mode == TOP_WIDE || mode == TOP_AA ? 800 : 400;
    top_target->transferFlags = mode == TOP_AA
        ? (DISPLAY_TRANSFER_FLAGS & ~GX_TRANSFER_SCALING(3)) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY)
        : DISPLAY_TRANSFER_FLAGS;
    if (mode == TOP_WIDE || top_mode == TOP_WIDE) {
        gfxSetWide(mode == TOP_WIDE);
        gfxSet3D(right_target != NULL && mode != TOP_WIDE);
    }
    top_mode = mode;
    LOG_INFO("gx", "top screen: %s", names[mode]);
}

float g_efb_x0 = 40.0f, g_efb_xs = 0.5f, g_efb_y0 = 0.0f, g_efb_ys = 0.5f, g_scr_w = 400.0f;
int g_fb_w = 240, g_fb_h = 400;

// The game's 16:9 option (lightmap_draw.c); absent in test builds
extern int isWidescreen(void) __attribute__((weak));
extern int setWidescreen(u8 v) __attribute__((weak));
extern int fhConfigScreenStyleIsWide(void) __attribute__((weak));

static void update_screen_mapping(void) {
    // The port owns the aspect (FOXHOLLOW_SCREEN_STYLE, main.c). The game only
    // applies the forced style when it calls setWidescreen, which a fresh boot
    // never does before the title screen, so enforce it here
    if (isWidescreen && setWidescreen && fhConfigScreenStyleIsWide &&
        !isWidescreen() != !fhConfigScreenStyleIsWide())
        setWidescreen(1);
    bool wide = isWidescreen && isWidescreen();
    float x0 = wide ? 0.0f : 40.0f, xs = wide ? 400.0f / 640.0f : 0.5f;
    if (x0 == g_efb_x0 && xs == g_efb_xs) return;
    g_efb_x0 = x0;
    g_efb_xs = xs;
    g_gx.dirty |= GX_DIRTY_PROJ;
    LOG_INFO("gx", "screen: %s", wide ? "16:9, full width" : "4:3, pillarboxed");
}

// S3D_GPU_TRACE=N (bug 12: the boot hangs on a real New 3DS, never in
// Azahar): for frames S3D_GPU_TRACE_FROM (default 0) to N-1, every draw,
// render target switch and transfer is logged, then submitted, and the GPU
// is waited for. The last "gputrace" line before the hang names the
// operation the GPU did not finish.
// S3D_GPU_TRACE_MAP=id (bug 17: hang on the hub's first frame, frame numbers
// depend on time spent in menus): nothing is traced until map id loads, then
// S3D_GPU_TRACE frames from that one
// S3D_GPU_TRACE_PAUSE=1 (bug 21: hang ~5 frames after the pause menu opens
// gametext/Communicator/English.bin): the same, from the first DVDOpen of a
// gametext/Communicator file
static int s_gpu_trace = -1, s_gpu_trace_from, s_gpu_trace_map = -1;
static const char* s_gpu_trace_file;

static void gpu_trace_config(void) {
    if (s_gpu_trace >= 0) return;
    s_gpu_trace = cfg_int("S3D_GPU_TRACE", 0);
    s_gpu_trace_from = cfg_int("S3D_GPU_TRACE_FROM", 0);
    s_gpu_trace_map = cfg_int("S3D_GPU_TRACE_MAP", -1);
    if (cfg_bool("S3D_GPU_TRACE_PAUSE", false)) s_gpu_trace_file = "gametext/Communicator";
    if (s_gpu_trace_map >= 0 || s_gpu_trace_file) s_gpu_trace_from = 0x7FFFFFFF;
}

// Called by DVDOpen (port/dvd_3ds.c): S3D_GPU_TRACE_PAUSE
void gx_gpu_trace_note_file(const char* name) {
    gpu_trace_config();
    if (!s_gpu_trace_file || !name || s_gpu_trace_from != 0x7FFFFFFF || !strstr(name, s_gpu_trace_file)) return;
    int n = s_gpu_trace;
    s_gpu_trace = (int)prof_frame_count() + n;
    s_gpu_trace_from = (int)prof_frame_count();
    LOG_INFO("gputrace", "\"%s\" opened: tracing frames %d-%d (S3D_GPU_SYNC %d)", name, s_gpu_trace_from,
             s_gpu_trace - 1, gx_gpu_sync());
    log_flush();
}

bool gx_gpu_trace_on(void) {
    gpu_trace_config();
    unsigned f = prof_frame_count();
    return s_gpu_trace > 0 && f < (unsigned)s_gpu_trace && f >= (unsigned)s_gpu_trace_from;
}

// Linked with --wrap=fhNoteMapLoaded (called by the game's map loader)
extern void __real_fhNoteMapLoaded(int mapId);
void diag_map_loaded(int map_id);  // port/diag_3ds.c (S3D_GPU_SWEEP)
void __wrap_fhNoteMapLoaded(int mapId) {
    __real_fhNoteMapLoaded(mapId);
    diag_map_loaded(mapId);
    gpu_trace_config();
    if (s_gpu_trace_map < 0 || mapId != s_gpu_trace_map || s_gpu_trace_from != 0x7FFFFFFF) return;
    int n = s_gpu_trace;
    s_gpu_trace_from = (int)prof_frame_count();
    s_gpu_trace = s_gpu_trace_from + n;
    LOG_INFO("gputrace", "map %d loaded: tracing frames %d-%d (S3D_GPU_SYNC %d)", mapId, s_gpu_trace_from, s_gpu_trace - 1, gx_gpu_sync());
}

// S3D_GPU_SYNC=mask (bug 17: the hub hang went away under the trace): in the
// trace window, nothing is logged per operation; instead each bit copies one
// thing the trace does, to find which one hides the hang.
//   1: flush the frame's CPU-written vertex/index data before each operation
//   2: submit each operation and wait for the GPU (2 s timeout, logged)
//   4: wait for the GPU at frame end, so the next frame's CPU work (game
//      logic, loading, buffer and texture updates) never overlaps it
//   8: after each operation, only add the framebuffer flush / invalidate /
//      early depth clear that citro3d writes when it splits the command list
//  16: split and wait only before draws with chosen dirty state, see
//      gx_gpu_sync_before_draw (S3D_GPU_SYNC_FLAGS)
// S3D_GPU_SYNC_OPS limits bit 2 to draws (1) or to everything else (2).
// 28th/29th logs: 4 still hung, 3 ran the hub
int gx_gpu_sync(void) {
    static int sync = -1;
    if (sync < 0) sync = cfg_int("S3D_GPU_SYNC", 0);
    return sync;
}

// citro3d only starts its queue in C3D_FrameEnd, so start it here. Its
// completion callback does not clear the queue mid-frame: a busy frame filled
// it and gxCmdQueueAdd hit svcBreak (21st/22nd logs, also in Azahar), so stop
// and clear it here once the GPU has finished every entry
static void gpu_trace_wait(const char* what) {
    u64 t0 = svcGetSystemTick();
    gxCmdQueueRun(&__C3D_Context.gxQueue);
    if (!gxCmdQueueWait(&__C3D_Context.gxQueue, 2000LL * 1000 * 1000)) {
        LOG_ERROR("gputrace", "GPU still busy 2 s after \"%s\": this operation hung it", what);
        log_flush();
        gxCmdQueueWait(&__C3D_Context.gxQueue, -1);
    }
    gxCmdQueueStop(&__C3D_Context.gxQueue);
    gxCmdQueueClear(&__C3D_Context.gxQueue);
    u64 us = (svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000);
    if (us > 50000) LOG_WARN("gputrace", "\"%s\" took %llu us on the GPU", what, (unsigned long long)us);
}

static u32 s_trace_seq;

// Before work is submitted mid-frame: citro3d's C3D_FrameEnd(0) flushes the
// whole linear heap, but gx_render_flush_cpu_caches only covers the frame
// buffers and the dyn pool. Display list cache entries patched in place and
// texture memory need the full flush too, or the GPU reads stale data
static void gpu_trace_flush_all(void) {
    extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
    gx_render_flush_cpu_caches();
    GSPGPU_FlushDataCache((void*)__ctru_linear_heap, __ctru_linear_heap_size);
}

// The command list written since the last split, for tools/pica_decode.py
static void gpu_trace_dump_cmdlist(void) {
    for (u32 i = 0; i < gpuCmdBufOffset; i += 8) {
        const u32* w = gpuCmdBuf + i;
        LOG_INFO("gpucmd", "%04lX: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX", (unsigned long)i,
                 (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
                 (unsigned long)w[4], (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7]);
    }
    LOG_INFO("gpucmd", "end (%lu words)", (unsigned long)gpuCmdBufOffset);
}

// S3D_GPU_SKIP=k drops the draw that would be trace step #k (A/B test)
bool gx_gpu_trace_skip_draw(void) {
    static int skip = -1;
    if (skip < 0) skip = cfg_int("S3D_GPU_SKIP", 0);
    if (!skip || !frame_active || !gx_gpu_trace_on() || s_trace_seq + 1 != (u32)skip) return false;
    LOG_WARN("gputrace", "f%u #%lu draw skipped (S3D_GPU_SKIP)", prof_frame_count(), (unsigned long)++s_trace_seq);
    log_flush();
    return true;
}

// S3D_GPU_SYNC with S3D_GPU_DUMP=k: dumps the command list that step #k
// submits (everything since the previous synced operation). gx_copy.c calls
// this before its own splits
void gx_gpu_trace_before_split(void) {
    static int dump = -1;
    if (dump < 0) dump = cfg_int("S3D_GPU_DUMP", 0);
    if (!dump || !frame_active || !gx_gpu_sync() || !gx_gpu_trace_on() || s_trace_seq + 1 != (u32)dump) return;
    LOG_INFO("gputrace", "f%u: command list submitted with step #%d", prof_frame_count(), dump);
    gpu_trace_dump_cmdlist();
}

// S3D_GPU_SYNC bit 16 (bug 17, runs E/F: the hub hangs with offscreen passes
// and shadows off too): before each draw whose pending citro3d state matches
// S3D_GPU_SYNC_FLAGS (citro3d's dirty bits after prune_state; 0x prefix for
// hex), submit everything so far and wait, so the GPU is idle when that state
// is written. Bit 0 (always set) = every draw. Bit 10 also matches a dirty
// light env (light LUT uploads). The first frame with 50+ draws logs how many
// draws had each bit
static u32 s_sync_counts[32];
static int s_sync_count_state;  // 0 = not started, 1 = counting this frame, 2 = logged

void gx_gpu_sync_before_draw(void) {
    static int mask = -1;
    if (!frame_active || !(gx_gpu_sync() & 16) || !gx_gpu_trace_on()) return;
    if (mask < 0) mask = cfg_int("S3D_GPU_SYNC_FLAGS", 1);
    u32 f = __C3D_Context.flags;
    if (__C3D_Context.lightEnv && *(const u32*)__C3D_Context.lightEnv) f |= BIT(10);
    if (s_sync_count_state < 2) {
        s_sync_count_state = 1;
        for (int i = 0; i < 32; i++) s_sync_counts[i] += (f >> i) & 1;
    }
    s_trace_seq++;
    if (!(f & (u32)mask) || !gpuCmdBufOffset) return;
    char op[80];
    snprintf(op, sizeof(op), "f%u commands before draw #%lu (dirty %08lX)", prof_frame_count(),
             (unsigned long)s_trace_seq, (unsigned long)f);
    gpu_trace_flush_all();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    gpu_trace_wait(op);
}

// At frame end: the first frame with 50+ draws logs how many draws had each bit
static void gpu_sync_log_counts(void) {
    if (s_sync_count_state != 1) return;
    if (s_sync_counts[0] < 50) {
        memset(s_sync_counts, 0, sizeof(s_sync_counts));
        return;
    }
    s_sync_count_state = 2;
    char buf[320];
    int n = 0;
    for (int i = 0; i < 32; i++)
        if (s_sync_counts[i]) n += snprintf(buf + n, sizeof(buf) - n, " b%d:%lu", i, (unsigned long)s_sync_counts[i]);
    LOG_INFO("gputrace", "f%u draws per dirty bit (S3D_GPU_SYNC_FLAGS %d):%s", prof_frame_count(),
             cfg_int("S3D_GPU_SYNC_FLAGS", 1), buf);
}

void gx_gpu_trace(const char* what, u32 a, u32 b, u32 c) {
    if (!frame_active || !gx_gpu_trace_on()) return;
    if (gx_gpu_sync() & 16) return;  // gx_gpu_sync_before_draw only
    int sync = gx_gpu_sync();
    if (sync) {
        static int ops = -1;
        if (ops < 0) ops = cfg_int("S3D_GPU_SYNC_OPS", 3);
        s_trace_seq++;
        if (sync & 8) {
            GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
            GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
            GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
        }
        if ((sync & 2) && !(ops & (strcmp(what, "draw") ? 2 : 1))) return;
        if (sync & 1) gpu_trace_flush_all();
        if (sync & 2) {
            char op[96];
            snprintf(op, sizeof(op), "f%u #%lu %s %08lX %08lX %08lX", prof_frame_count(), (unsigned long)s_trace_seq,
                     what, (unsigned long)a, (unsigned long)b, (unsigned long)c);
            if (gpuCmdBufOffset) {
                s_trace_seq--;
                gx_gpu_trace_before_split();
                s_trace_seq++;
            }
            C3D_FrameSplit(GX_CMDLIST_FLUSH);
            gpu_trace_wait(op);
        }
        return;
    }
    u32 seq = s_trace_seq;
    gpu_trace_flush_all();
    s_trace_seq = ++seq;
    LOG_INFO("gputrace", "f%u #%lu %s %08lX %08lX %08lX", prof_frame_count(), (unsigned long)seq, what,
             (unsigned long)a, (unsigned long)b, (unsigned long)c);
    // S3D_GPU_DUMP=k: the raw command list of step #k, for tools/pica_decode.py.
    // S3D_GPU_DUMP_LIT=1: the first draw with fragment lighting (bug 14), which
    // needs no step number (they differ between runs)
    static int dump = -1, dump_lit = -1;
    if (dump < 0) dump = cfg_int("S3D_GPU_DUMP", 0);
    if (dump_lit < 0) dump_lit = cfg_bool("S3D_GPU_DUMP_LIT", false);
    bool lit = dump_lit == 1 && __C3D_Context.lightEnv && !strcmp(what, "draw");
    if (lit) dump_lit = 0;
    if ((dump && seq == (u32)dump) || lit) gpu_trace_dump_cmdlist();
    log_flush();
    volatile const char* phase = g_main_phase;
    WD_PHASE("gputrace: C3D_FrameSplit");
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    WD_PHASE("gputrace: waiting for the GPU");
    gpu_trace_wait(what);
    g_main_phase = phase;
}

// --- GPU fence (S3D_GPU_PIPELINE, PIPELINE_PLAN.md) ---------------------------
// Pipelined (gx_c3d_queue.c), the GPU runs frame F while the CPU builds F+1:
// C3D_FrameEnd(F+1) waits for F. Memory the GPU reads is only freed once the
// last frame that used it is done; until then it waits in s_gpu_deferred.
// Without pipelining the same rules hold (C3D_FrameBegin(F+1) waits for F),
// so the code paths don't depend on the mode.
static u32 s_gpu_submitted;  // Last render frame handed to the GPU (gx_frame_end)
static u32 s_gpu_done;       // Last render frame the GPU has finished
static bool s_frame_pipelined;

typedef struct {
    u32 frame;
    bool is_tex;
    union {
        void* mem;
        C3D_Tex tex;
    };
} GpuDeferred;
static GpuDeferred* s_gpu_deferred;
static u32 s_gpu_deferred_n, s_gpu_deferred_cap;

bool gx_gpu_frame_done(u32 frame) {
    return (s32)(s_gpu_done - frame) >= 0;
}

bool gx_gpu_pipelined(void) {
    return s_frame_pipelined;
}

static void gpu_release(GpuDeferred* d) {
    if (d->is_tex) C3D_TexDelete(&d->tex);
    else linearFree(d->mem);
}

static void gpu_retire(void) {
    u32 keep = 0;
    for (u32 i = 0; i < s_gpu_deferred_n; i++) {
        if (gx_gpu_frame_done(s_gpu_deferred[i].frame)) gpu_release(&s_gpu_deferred[i]);
        else s_gpu_deferred[keep++] = s_gpu_deferred[i];
    }
    s_gpu_deferred_n = keep;
}

static void gpu_defer(const GpuDeferred* d) {
    if (s_gpu_deferred_n == s_gpu_deferred_cap) {
        u32 cap = s_gpu_deferred_cap ? s_gpu_deferred_cap * 2 : 128;
        GpuDeferred* p = realloc(s_gpu_deferred, cap * sizeof(*p));
        if (!p) {
            // No memory for the list: make the GPU idle and free it now
            LOG_ONCE(LOG_LEVEL_WARN, "gx", "no memory for the deferred free list: waiting for the GPU");
            gx_gpu_wait_idle();
            GpuDeferred copy = *d;
            gpu_release(&copy);
            return;
        }
        s_gpu_deferred = p;
        s_gpu_deferred_cap = cap;
    }
    s_gpu_deferred[s_gpu_deferred_n++] = *d;
    PROF_COUNT("gpu.deferred_frees", 1);
}

void gx_gpu_free_linear(void* p, u32 last_used) {
    if (!p) return;
    if (gx_gpu_frame_done(last_used)) {
        linearFree(p);
        return;
    }
    GpuDeferred d = { .frame = last_used, .is_tex = false, .mem = p };
    gpu_defer(&d);
}

void gx_gpu_free_tex(const C3D_Tex* tex, u32 last_used) {
    if (!tex || !tex->data) return;
    GpuDeferred d = { .frame = last_used, .is_tex = true, .tex = *tex };
    if (gx_gpu_frame_done(last_used)) gpu_release(&d);
    else gpu_defer(&d);
}

void gx_gpu_wait_idle(void) {
    {
        PROF_ZONE("gx.wait_gpu_idle");
        volatile const char* phase = g_main_phase;
        WD_PHASE("gx_gpu_wait_idle (GPU: everything queued)");
        C3Dx_WaitIdle(-1);
        g_main_phase = phase;
    }
    s_gpu_done = s_gpu_submitted;
    gpu_retire();
}

// The last submitted frame only: unlike gx_gpu_wait_idle, not this frame's
// mid-frame work (the scene is usually submitted at the post effect copies)
void gx_gpu_wait_previous(void) {
    {
        PROF_ZONE("gx.wait_gpu_prev");
        volatile const char* phase = g_main_phase;
        WD_PHASE("gx_gpu_wait_previous (GPU: previous frame)");
        C3Dx_WaitPrevFrame(-1);
        g_main_phase = phase;
    }
    s_gpu_done = s_gpu_submitted;
}

bool gx_gpu_reclaim(void) {
    if (!s_gpu_deferred_n) return false;
    u32 before = s_gpu_deferred_n;
    gx_gpu_wait_idle();
    PROF_COUNT("gpu.reclaims", 1);
    return s_gpu_deferred_n < before;
}

// S3D_GPU_PIPELINE (default 1, touch UI "GPU overlap"). Off while the GPU
// trace runs: its per-operation waits assume the stock queue
static void gpu_pipeline_update(void) {
    static int on = -1;
    CFG_LIVE(on, cfg_bool("S3D_GPU_PIPELINE", true) ? 1 : 0);
    bool want = on && !gx_gpu_trace_on() && gx_render_can_pipeline();
    // S3D_GPU_SPLIT_FULLFLUSH=1: mid-frame submits flush the whole linear heap
    // (the safe fallback if something written mid-frame is missed by the hook)
    static int full_flush = -1;
    CFG_LIVE(full_flush, cfg_bool("S3D_GPU_SPLIT_FULLFLUSH", false) ? 1 : 0);
    C3Dx_SetSplitFlushHook(full_flush ? NULL : gx_render_flush_new_writes);
    // S3D_RT_PACING (MULTICORE_PLAN.md, "Frame pacing"): 1 swap as soon as the
    // GPU is done (GSP callbacks), 2 empty an idle queue when the render thread wakes
    static int pacing = -1;
    CFG_LIVE(pacing, cfg_int("S3D_RT_PACING", 3));
    C3Dx_SetPacing(pacing);
    if (want != C3Dx_Pipelined()) {
        bool now = C3Dx_SetPipelined(want);
        if (now != want) LOG_ONCE(LOG_LEVEL_WARN, "gx", "GPU pipelining unavailable (no linear memory for a second command buffer)");
        LOG_INFO("gx", "GPU pipelining %s", now ? "on" : "off");
    }
    s_frame_pipelined = C3Dx_Pipelined();
}

// The 3D slider value a frame starts with (0 without a right-eye target).
// With the render thread overlapping the frame boundary (gx_rec.c), the game
// thread reads it and passes it to gx_frame_begin_slider, so its answers
// (stereo, offscreen passes) agree with the frame the render thread opens
float gx_frame_slider(void) {
    if (!right_target) return 0.0f;
    // S3D_STEREO_FORCE=1: render stereo regardless of the physical slider
    // (console A/B tests; the slider stays where it is)
    static int force = -1;
    if (force < 0) force = cfg_int("S3D_STEREO_FORCE", 0);
    if (force) return 1.0f;
    return osGet3DSliderState();
}

void gx_frame_begin(void) {
    gx_frame_begin_slider(gx_frame_slider());
}

void gx_frame_begin_slider(float slider) {
    if (!top_target || frame_active) return;
    // Stall finder (BFDF5C33 HW: 3 s frames in here every ~13 s, outside the
    // VBlank wait): the steps' times, logged when the whole begin is slow
    u64 st[6];
    int sn = 0;
    st[sn++] = svcGetSystemTick();
    gpu_pipeline_update();
    st[sn++] = svcGetSystemTick();

    // SYNCDRAW waits for the next VBlank, then for the previous frame. A frame
    // that already took longer than one VBlank period skips the VBlank wait
    // (S3D_FRAME_PACE_ADAPTIVE, default 1): the display still swaps on a
    // VBlank and frames are never closer than one period, but a 56 ms frame
    // no longer rounds up to 67 ms. The game times itself with a stopwatch
    // (waitNextFrame), so uneven frame times are fine. S3D_FRAME_PACE_ADAPTIVE=0
    // keeps the old behavior
    {
        static int pace = -1;
        static u64 last_begin;
        CFG_LIVE(pace, cfg_bool("S3D_FRAME_PACE_ADAPTIVE", true) ? 1 : 0);
        // 30 FPS cap (touch UI setting S3D_FPS_CAP30, on by default): citro3d's frame sync
        // then waits for every second VBlank, which evens out frame times in
        // areas that cannot hold 60
        static int cap30 = -1;
        int prev_cap = cap30;
        CFG_LIVE(cap30, cfg_bool("S3D_FPS_CAP30", true) ? 1 : 0);
        if (cap30 != prev_cap) C3D_FrameRate(cap30 ? 30.0f : 60.0f);
        u64 now = svcGetSystemTick();
        bool slow = pace && last_begin && now - last_begin >= (u64)(SYSCLOCK_ARM11 / (cap30 ? 30 : 60));
        PROF_ZONE("gx.wait_vsync");
        volatile const char* phase = g_main_phase;
        // Pipelined, C3D_FrameBegin does not wait for the GPU (C3D_FrameEnd does)
        if (s_frame_pipelined) WD_PHASE(slow ? "C3D_FrameBegin" : "C3D_FrameBegin (VBlank)");
        else WD_PHASE(slow ? "C3D_FrameBegin (GPU: previous frame)" : "C3D_FrameBegin (VBlank, then GPU: previous frame)");
        C3D_FrameBegin(slow ? 0 : C3D_FRAME_SYNCDRAW);
        g_main_phase = phase;
        st[sn++] = svcGetSystemTick();
        gx_c3d_invalidate();
        gx_capture_frame(&game_target()->frameBuf, top_mode, frame_lowres);  // Previous frame, before the clears
        st[sn++] = svcGetSystemTick();
        if (slow) PROF_COUNT("gx.unpaced_frames", 1);
        last_begin = svcGetSystemTick();
    }

    // Stereo while the 3D slider is up; at 0 it costs nothing
    if (!right_target) slider = 0.0f;
    bool stereo = slider > 0.0f;
    set_top_mode(stereo ? TOP_STEREO : wide_enabled ? TOP_WIDE : aa_enabled ? TOP_AA : TOP_MONO);
    frame_lowres = top_mode == TOP_MONO && lowres_wanted();

    // GXSetCopyClear color (0xRRGGBBAA); depth clears to "far" (0 on PICA)
    GXColor c = g_gx.clear_color;
    u32 color = clear_black ? 0x000000FF : ((u32)c.r << 24) | (c.g << 16) | (c.b << 8) | 0xFF;
    if (bottom_target) {
        C3D_RenderTargetClear(bottom_target, C3D_CLEAR_ALL, 0x000000FF, 0);
        C3D_FrameDrawOn(bottom_target);
    }
    if (stereo) {
        // Mark both eye outputs used, then draw on the shared target
        C3D_FrameDrawOn(top_target);
        C3D_FrameDrawOn(right_target);
        C3D_RenderTargetClear(big_target, C3D_CLEAR_ALL, color, 0);
        C3D_FrameDrawOn(big_target);
    } else if (frame_lowres) {
        // The top target is only drawn by lowres_present
        C3D_RenderTargetClear(lowres_target, C3D_CLEAR_ALL, color, 0);
        draw_on_game();
    } else {
        C3D_RenderTargetClear(top_target, C3D_CLEAR_ALL, color, 0);
        C3D_FrameDrawOn(top_target);
    }

    st[sn++] = svcGetSystemTick();
    update_screen_mapping();
    g_fb_w = game_fb_w();
    g_fb_h = game_fb_h();
    frame_stereo = stereo;
    frame_sep = slider * stereo_sep;
    gx_render_set_stereo(stereo, frame_sep);
    gx_render_frame_begin();
    st[sn] = svcGetSystemTick();
    if (st[sn] - st[0] > (u64)(SYSCLOCK_ARM11 / 1000) * 250) {
        const gxCmdQueue_s* q = &__C3D_Context.gxQueue;
        LOG_WARN("stall", "frame begin took %lu ms: pipeline %lu, C3D_FrameBegin %lu, capture %lu, clears %lu, "
                 "render begin %lu (queue %u/%u done, max %u)",
                 (unsigned long)((st[sn] - st[0]) / (SYSCLOCK_ARM11 / 1000)), (unsigned long)((st[1] - st[0]) / (SYSCLOCK_ARM11 / 1000)),
                 (unsigned long)((st[2] - st[1]) / (SYSCLOCK_ARM11 / 1000)), (unsigned long)((st[3] - st[2]) / (SYSCLOCK_ARM11 / 1000)),
                 (unsigned long)((st[4] - st[3]) / (SYSCLOCK_ARM11 / 1000)), (unsigned long)((st[5] - st[4]) / (SYSCLOCK_ARM11 / 1000)),
                 q->lastEntry, q->numEntries, q->maxEntries);
    }
    frame_active = true;
    gx_gpu_trace(stereo ? "frame begin: clears (stereo)" : "frame begin: clears", top_mode, frame_lowres,
                 bottom_target != NULL);
}

// Redirect drawing to the bottom screen. The EFB region around the minimap
// and its d-pad icon (x 16-256, y 292-472, the 120x100 box at 50,340 grows
// to 200 wide for area names) is scaled by 4/3 onto the 320x240 screen.
static float saved_map[5];

bool gx_hud_begin(void) {
    if (!bottom_target || !frame_active || hud_active || offscreen_active) return false;
    gx_render_flush();
    C3D_FrameDrawOn(bottom_target);
    gx_render_set_stereo(false, 0.0f);
    saved_map[0] = g_efb_x0; saved_map[1] = g_efb_xs;
    saved_map[2] = g_efb_y0; saved_map[3] = g_efb_ys; saved_map[4] = g_scr_w;
    g_efb_xs = g_efb_ys = 4.0f / 3.0f;
    g_efb_x0 = -16.0f * g_efb_xs;
    g_efb_y0 = -292.0f * g_efb_ys;
    g_scr_w = 320.0f;
    g_fb_w = 240;
    g_fb_h = 320;
    g_gx.dirty |= GX_DIRTY_PROJ;
    hud_active = true;
    gx_gpu_trace("hud begin (bottom screen)", 0, 0, 0);
    return true;
}

// Inside a HUD pass: map another EFB region instead of the minimap's, the
// point (src_x, src_y) lands on bottom screen pixel (dst_x, dst_y), scaled by
// scale (the C-menu ring, port/touchui_3ds.c). gx_hud_end restores the game's
// mapping as usual. Recorded (gx/gxrec.list), so the render thread replays it
// inside the same pass
void gx_hud_set_map(float src_x, float src_y, float scale, float dst_x, float dst_y) {
    if (!hud_active) return;
    gx_render_flush();
    g_efb_xs = g_efb_ys = scale;
    g_efb_x0 = dst_x - src_x * scale;
    g_efb_y0 = dst_y - src_y * scale;
    g_gx.dirty |= GX_DIRTY_PROJ;
}

void gx_hud_end(void) {
    if (!hud_active) return;
    gx_render_flush();
    g_efb_x0 = saved_map[0]; g_efb_xs = saved_map[1];
    g_efb_y0 = saved_map[2]; g_efb_ys = saved_map[3]; g_scr_w = saved_map[4];
    g_fb_w = game_fb_w();
    g_fb_h = game_fb_h();
    draw_on_game();
    gx_render_set_stereo(frame_stereo, frame_sep);
    g_gx.dirty |= GX_DIRTY_PROJ;
    hud_active = false;
}

// Touch UI (port/touchui_3ds.c): citro2d draws straight into the bottom
// target after the game's frame; end re-binds the GX state and the top target
C3D_RenderTarget* gx_ui_begin(void) {
    if (!bottom_target || !frame_active || hud_active || offscreen_active) return NULL;
    gx_render_flush();
    // citro2d refills its vertex buffer from the start every frame: pipelined,
    // the previous frame's GPU work may still read it
    if (s_frame_pipelined) gx_gpu_wait_previous();
    return bottom_target;
}

void gx_ui_end(void) {
    gx_restore_state();
    draw_on_game();
    gx_render_set_stereo(frame_stereo, frame_sep);
    g_gx.dirty |= GX_DIRTY_PROJ;
}

bool gx_hud_enabled(void) {
    return bottom_target != NULL;
}

// For the GX recorder (gx_rec.c): whether a HUD pass is open
bool gx_hud_active(void) {
    return hud_active;
}

bool gx_frame_stereo(void) {
    return frame_active && frame_stereo;
}

// --- Offscreen passes (GRAPHICS_PLAN.md 6.1) ---------------------------------
// Passes that draw into a corner of the EFB only to copy it out (shadow
// casters, the object shadow, the distortion slots) draw into a small square
// scratch target instead: once in stereo, at the copy's own resolution, and
// without overwriting the frame. The scratch target has the same rotated
// layout as the screen, so GXCopyTex reads it with the usual mapping
// (gx_copy_source_target). One target per size, created on first use; when
// VRAM is short, begin returns false and the pass draws into the EFB as before.
enum { OFFSCREEN_SIZES = 4 };  // 32, 64, 128, 256
static C3D_RenderTarget* offscreen_targets[OFFSCREEN_SIZES];
static bool offscreen_failed[OFFSCREEN_SIZES];
static C3D_RenderTarget* offscreen_target;
static float offscreen_map[5];

static void offscreen_free(void) {
    for (int i = 0; i < OFFSCREEN_SIZES; i++) {
        if (offscreen_targets[i]) C3D_RenderTargetDelete(offscreen_targets[i]);
        offscreen_targets[i] = NULL;
        offscreen_failed[i] = false;
    }
    offscreen_target = NULL;
    offscreen_active = false;
}

static int offscreen_on = -1, offscreen_stereo = -1;

static void offscreen_config(void) {
    if (offscreen_on < 0) offscreen_on = cfg_bool("S3D_OFFSCREEN", true);
    // Stereo frames keep the EFB corner: switching away from the 240x800
    // target lost the left eye's earlier drawing (seen on Azahar)
    if (offscreen_stereo < 0) offscreen_stereo = cfg_bool("S3D_OFFSCREEN_STEREO", false);
}

static int offscreen_size_index(int res) {
    int i = 0;
    while (i < OFFSCREEN_SIZES - 1 && (32 << i) < res) i++;
    return i;
}

// For the GX recorder (gx_rec.c), which answers gx_offscreen_begin on the game
// thread and tracks the HUD / offscreen pass state itself: 1 = it would begin,
// 0 = it would not, -1 = its target size was never tried (begin creates it).
// stereo is the recording frame's stereo flag (the render thread may still be
// in the previous frame); reads only settings and the targets created so far.
int gx_offscreen_check(float w, float h, int res, bool stereo) {
    offscreen_config();
    if (!offscreen_on || (stereo && !offscreen_stereo) || w <= 0.0f || h <= 0.0f) return 0;
    int i = offscreen_size_index(res);
    if (offscreen_targets[i]) return 1;
    return offscreen_failed[i] ? 0 : -1;
}

bool gx_offscreen_begin(float x, float y, float w, float h, int res) {
    offscreen_config();
    if (!offscreen_on || (frame_stereo && !offscreen_stereo) || !frame_active || hud_active || offscreen_active || w <= 0.0f || h <= 0.0f) return false;
    int i = offscreen_size_index(res);
    if (!offscreen_targets[i] && !offscreen_failed[i]) {
        int n = 32 << i;
        offscreen_targets[i] = C3D_RenderTargetCreate(n, n, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (!offscreen_targets[i]) {
            offscreen_failed[i] = true;
            LOG_WARN("gx", "offscreen: no VRAM for a %dx%d target", n, n);
        }
    }
    if (!offscreen_targets[i]) {
        g_gx_offscreen_fails++;
        return false;
    }
    offscreen_target = offscreen_targets[i];

    gx_render_flush();
    C3D_FrameDrawOn(offscreen_target);
    gx_render_set_stereo(false, 0.0f);
    offscreen_map[0] = g_efb_x0; offscreen_map[1] = g_efb_xs;
    offscreen_map[2] = g_efb_y0; offscreen_map[3] = g_efb_ys; offscreen_map[4] = g_scr_w;
    // The EFB rectangle fills the 240x240 logical screen of the target
    g_scr_w = 240.0f;
    g_efb_xs = 240.0f / w;
    g_efb_x0 = -x * g_efb_xs;
    g_efb_ys = 240.0f / h;
    g_efb_y0 = -y * g_efb_ys;
    g_fb_w = g_fb_h = offscreen_target->frameBuf.width;
    g_gx.dirty |= GX_DIRTY_PROJ;
    offscreen_active = true;

    // Start from the copy clear color and depth, as the EFB region would be
    GxPixelState* px = &g_gx.pixel;
    bool cu = px->color_update, au = px->alpha_update, zu = px->z_update;
    px->color_update = px->alpha_update = px->z_update = true;
    gx_render_clear_efb_rect((u32)x, (u32)y, (u32)w, (u32)h);
    px->color_update = cu; px->alpha_update = au; px->z_update = zu;
    gx_gpu_trace("offscreen begin (size, target)", 32 << i, (u32)(uintptr_t)offscreen_target->frameBuf.colorBuf, 0);
    PROF_COUNT("gx.offscreen", 1);
    // Per target size, to tell the passes apart (MULTICORE_PLAN.md "Other
    // offscreen passes"): 32 = Rcp distortion slots, 128 = renderObjectShadowTexture,
    // casters use the shadow texture's size
    switch (i) {
        case 0: PROF_COUNT("gx.offscreen_32", 1); break;
        case 1: PROF_COUNT("gx.offscreen_64", 1); break;
        case 2: PROF_COUNT("gx.offscreen_128", 1); break;
        default: PROF_COUNT("gx.offscreen_256", 1); break;
    }
    return true;
}

void gx_offscreen_end(void) {
    if (!offscreen_active) return;
    gx_render_flush();
    g_efb_x0 = offscreen_map[0]; g_efb_xs = offscreen_map[1];
    g_efb_y0 = offscreen_map[2]; g_efb_ys = offscreen_map[3]; g_scr_w = offscreen_map[4];
    g_fb_w = game_fb_w();
    g_fb_h = game_fb_h();
    draw_on_game();
    gx_render_set_stereo(frame_stereo, frame_sep);
    g_gx.dirty |= GX_DIRTY_PROJ;
    offscreen_active = false;
}

bool gx_offscreen_active(void) {
    return offscreen_active;
}

bool port_shadows_enabled(void);

void gx_render_get_stats(GxRenderStats* out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < OFFSCREEN_SIZES; i++) {
        if (offscreen_targets[i]) out->offscreen_ok |= 1u << i;
        if (offscreen_failed[i]) out->offscreen_failed |= 1u << i;
    }
    out->lowres = lowres_target != NULL;
    out->shadows = port_shadows_enabled();
}

// Where GXCopyTex reads from: the scratch target during an offscreen pass
C3D_RenderTarget* gx_copy_source_target(void) {
    return offscreen_active ? offscreen_target : game_target();
}

// The drawn area of that target (smaller than the target in half resolution)
void gx_copy_source_size(int* w, int* h) {
    if (offscreen_active) {
        *w = offscreen_target->frameBuf.width;
        *h = offscreen_target->frameBuf.height;
    } else {
        *w = game_fb_w();
        *h = game_fb_h();
    }
}

// GPU busy time per frame from GSP's performance log (S3D_GPU_PERFLOG,
// default 1): counters gpu.p3d_us (command lists: vertex and pixel work),
// gpu.ppf_us (display transfers and texture copies) and gpu.psc_us (memory
// fills). GSP times each PICA operation from its start to its interrupt; the
// queue runs them one at a time, so frame time minus their sum is GPU idle
// time. Unlike gpu.draw_us, this does not include waiting for the CPU

volatile u32 g_gx_gpu_p3d_last;  // Last frame's gpu.p3d_us (diag_3ds.c lean A/B), 0 = none

static void gpu_perflog(void) {
    static int on = -1;
    static GSPGPU_PerfLog prev;
    static bool have;
    if (on < 0) {
        on = cfg_bool("S3D_GPU_PERFLOG", true) ? 1 : 0;
        if (on && R_FAILED(GSPGPU_SetPerfLogMode(true))) {
            LOG_WARN("gx", "GSP performance log unavailable: no gpu.p3d_us");
            on = 0;
        }
        s_perflog_on = on;
    }
    if (!on) return;
    GSPGPU_PerfLog cur;
    if (R_FAILED(GSPGPU_GetPerfLog(&cur))) return;
    if (have) {
        const GSPGPU_PerfLogEntry* c = cur.entries;
        const GSPGPU_PerfLogEntry* p = prev.entries;
        u32 p3d = c[GSPGPU_EVENT_P3D].totalDurationUs - p[GSPGPU_EVENT_P3D].totalDurationUs;
        u32 ppf = c[GSPGPU_EVENT_PPF].totalDurationUs - p[GSPGPU_EVENT_PPF].totalDurationUs;
        u32 psc = (c[GSPGPU_EVENT_PSC0].totalDurationUs - p[GSPGPU_EVENT_PSC0].totalDurationUs) +
                  (c[GSPGPU_EVENT_PSC1].totalDurationUs - p[GSPGPU_EVENT_PSC1].totalDurationUs);
        // Over 1 s per frame is not a real value (first frame after enabling or
        // a suspend, or an emulator without the log)
        if (p3d < 1000000 && ppf < 1000000 && psc < 1000000) {
            PROF_COUNT("gpu.p3d_us", p3d);
            g_gx_gpu_p3d_last = p3d;
            PROF_COUNT("gpu.ppf_us", ppf);
            PROF_COUNT("gpu.psc_us", psc);
        }
    }
    prev = cur;
    have = true;
}

void gx_frame_end(void) {
    if (!frame_active) return;
    gx_offscreen_end();

    gx_render_frame_end();
    if (frame_lowres) lowres_present();
    gx_gpu_trace("frame end: last draws", frame_lowres, 0, 0);
    gpu_sync_log_counts();
    bool trace = gx_gpu_trace_on();
    {
        // Pipelined, this is where the CPU waits for the previous frame's GPU work
        volatile const char* phase = g_main_phase;
        if (s_frame_pipelined) WD_PHASE("C3D_FrameEnd (GPU: previous frame)");
        ft_ev(FT_R_FE_B, 0);
        C3D_FrameEnd(0);
        ft_ev(FT_R_FE_E, 0);
        g_main_phase = phase;
    }
    frame_active = false;
    // Both modes: the previous frame is done (C3D_FrameEnd or C3D_FrameBegin waited)
    s_gpu_done = s_gpu_submitted;
    s_gpu_submitted = gx_render_frame_number();
    int sync = trace ? gx_gpu_sync() : 0;
    if (trace && !sync) {
        LOG_INFO("gputrace", "f%u frame end: screen transfers", prof_frame_count());
        log_flush();
        gpu_trace_wait("frame end: screen transfers");
        s_gpu_done = s_gpu_submitted;
    } else if (sync & 6) {
        char op[64];
        snprintf(op, sizeof(op), "f%u frame end (S3D_GPU_SYNC)", prof_frame_count());
        gpu_trace_wait(op);
        s_gpu_done = s_gpu_submitted;
    }
    gpu_retire();

    // GPU-side timing of the last finished frame (real numbers on hardware;
    // Azahar does not model GPU time) and command buffer use
    PROF_COUNT("gpu.draw_us", (u64)(C3D_GetDrawingTime() * 1000.0f));
    PROF_COUNT("gpu.submit_us", (u64)(C3D_GetProcessingTime() * 1000.0f));
    PROF_COUNT("gpu.cmdbuf_pct", (u64)(C3D_GetCmdBufUsage() * 100.0f));
    // Pipelined: time C3D_FrameEnd waited for the previous frame (the GPU is
    // the bottleneck when this is large) and frames that ran pipelined
    PROF_COUNT("gpu.wait_us", (u64)(C3Dx_GetWaitTime() * 1000.0f));
    gpu_perflog();
    if (s_frame_pipelined) PROF_COUNT("gpu.pipelined_frames", 1);
    {
        static u32 recycled;
        u32 n = C3Dx_Recycles();
        if (n != recycled) PROF_COUNT("gpu.queue_recycles", n - recycled);
        recycled = n;
    }
    {
        static u32 forced;
        u32 n = C3Dx_ForcedClears();
        if (n != forced) PROF_COUNT("gpu.queue_full_waits", n - forced);
        forced = n;
    }
}

// Called after code that used its own shader/state (os/bench.c)
// S3D_GPU_SELFTEST=1 (bug 12: the game's first draw hangs the GPU on a real
// New 3DS, never in Azahar). Before the game, draws one quad per step with the
// game's vertex shaders and GxOutVertex layout, each step in its own frame
// with a 2 s timeout. The log names the first step that does not finish.
static void hwtest_unif(shaderProgram_s* prog, const char* name, int row, float x, float y, float z, float w) {
    s8 loc = shaderInstanceGetUniformLocation(prog->vertexShader, name);
    if (loc >= 0) C3D_FVUnifSet(GPU_VERTEX_SHADER, loc + row, x, y, z, w);
}

static void hwtest_bool(shaderProgram_s* prog, const char* name, bool on) {
    s8 loc = shaderInstanceGetUniformLocation(prog->vertexShader, name);
    if (loc >= 0) C3D_BoolUnifSet(GPU_VERTEX_SHADER, loc, on);
}

// Leaner vertex shader bools: every texture slot on the affine path or not
// (gx_render.c upload_unit_texmtx; unused slots are marked in texsrc instead)
static void hwtest_lean(shaderProgram_s* prog, bool aff) {
    s8 loc = shaderInstanceGetUniformLocation(prog->vertexShader, "tcaff");
    for (int s = 0; s < 3 && loc >= 0; s++) C3D_BoolUnifSet(GPU_VERTEX_SHADER, loc + s, aff);
}

// --- Vertex shader cost benchmark (S3D_BENCH / S3D_BENCH_VSH, boot) ------------
// The hub's GPU time follows its geometry, not pixels or state changes (boot
// "gpu per draw" bench, MULTICORE_PLAN.md). This times the game's own vertex
// shaders with the GxOutVertex layout: VB_VERTS unique vertices (tiny
// triangles, DrawArrays: no vertex reuse) per configuration, GPU time from the
// GSP perf log (P3D), best of 3. Plus the packed layout and an indexed draw
// that reuses each vertex 3 times (post-transform cache).
// Leaner vertex shaders (MULTICORE_PLAN.md) add: the lean texture coordinate
// paths, the vertex data in VRAM instead of FCRAM, and the I/O shaders of
// gx/vbench (almost no ALU work) that split the cost into instructions,
// outputs, input attributes and bytes fetched.
#define VB_VERTS 12000
enum { VB_IO7 = GX_VSH_COUNT, VB_IO7O3, VB_IN3, VB_PROGS };  // Programs besides GX_VSH_*
enum { VBL_STD, VBL_PACKED, VBL_IN3_STRIDE, VBL_COMPACT };  // Vertex layouts
enum { VBX_REUSE = 1, VBX_FRAG = 2, VBX_VRAM = 4 };
typedef struct {
    const char* name;
    u8 prog, layout, lights;
    u8 lean;  // 0: every slot on the general path (as before), 1-3: that many slots on the affine path, rest off
    u8 flags;
} VbTest;
static const VbTest s_vb_tests[] = {
    { "unlit shader", GX_VSH_UNLIT, VBL_STD, 0, 0, 0 },
    { "lit shader, lighting off", GX_VSH_LIT, VBL_STD, 0, 0, 0 },
    { "lit, 1 light", GX_VSH_LIT, VBL_STD, 1, 0, 0 },
    { "lit, 2 lights", GX_VSH_LIT, VBL_STD, 2, 0, 0 },
    { "lit, 4 lights", GX_VSH_LIT, VBL_STD, 4, 0, 0 },
    { "lit, 8 lights", GX_VSH_LIT, VBL_STD, 8, 0, 0 },
    { "lit, 1 light + fragment light out", GX_VSH_LIT, VBL_STD, 1, 0, VBX_FRAG },
    { "warp shader, warp on", GX_VSH_WARP, VBL_STD, 0, 0, 0 },
    { "unlit, packed layout (36 B)", GX_VSH_UNLIT, VBL_PACKED, 0, 0, 0 },
    { "lit, 1 light, indexed (3x reuse)", GX_VSH_LIT, VBL_STD, 1, 0, VBX_REUSE },
    { "unlit lean, 1 affine slot", GX_VSH_UNLIT, VBL_STD, 0, 1, 0 },
    { "unlit lean, 2 affine slots", GX_VSH_UNLIT, VBL_STD, 0, 2, 0 },
    { "unlit lean, 3 affine slots", GX_VSH_UNLIT, VBL_STD, 0, 3, 0 },
    { "lit lean, lighting off, 1 slot", GX_VSH_LIT, VBL_STD, 0, 1, 0 },
    { "lit lean, 1 light, 1 slot", GX_VSH_LIT, VBL_STD, 1, 1, 0 },
    { "unlit lean, 1 slot, packed", GX_VSH_UNLIT, VBL_PACKED, 0, 1, 0 },
    { "unlit, VRAM", GX_VSH_UNLIT, VBL_STD, 0, 0, VBX_VRAM },
    { "unlit lean, 1 slot, VRAM", GX_VSH_UNLIT, VBL_STD, 0, 1, VBX_VRAM },
    { "unlit, packed, VRAM", GX_VSH_UNLIT, VBL_PACKED, 0, 0, VBX_VRAM },
    { "io: 7 in (52 B), 7 out, ~9 instr", VB_IO7, VBL_STD, 0, 0, 0 },
    { "io: 7 in (52 B), 3 out", VB_IO7O3, VBL_STD, 0, 0, 0 },
    { "io: 3 in (52 B stride), 7 out", VB_IN3, VBL_IN3_STRIDE, 0, 0, 0 },
    { "io: 3 in (24 B), 7 out", VB_IN3, VBL_COMPACT, 0, 0, 0 },
    { "io: 7 in (52 B), 7 out, VRAM", VB_IO7, VBL_STD, 0, 0, VBX_VRAM },
};
#define VB_COUNT ((int)(sizeof(s_vb_tests) / sizeof(s_vb_tests[0])))

// Compact layout for the 3-input I/O test
typedef struct {
    float pos[3];
    u8 color[4];
    float tc[2];
} VbCompactVertex;  // 24 bytes

static u32 vb_p3d_us(void) {
    GSPGPU_PerfLog l;
    if (R_FAILED(GSPGPU_GetPerfLog(&l))) return 0;
    return l.entries[GSPGPU_EVENT_P3D].totalDurationUs;
}

// Copy of a linear buffer in VRAM (GPU texture copy), NULL if VRAM is short
// or the copy does not read back
static void* vb_vram_copy(const void* src, u32 size) {
    void* dst = vramAlloc(size);
    if (!dst) {
        LOG_WARN("bench", "vertex shader bench: no VRAM for %lu bytes", (unsigned long)size);
        return NULL;
    }
    C3D_SyncTextureCopy((u32*)src, GX_BUFFER_DIM(size >> 4, 0), (u32*)dst, GX_BUFFER_DIM(size >> 4, 0), size,
                        GX_TRANSFER_RAW_COPY(1));
    if (memcmp(dst, src, 64) != 0 || memcmp((const u8*)dst + size - 64, (const u8*)src + size - 64, 64) != 0) {
        LOG_WARN("bench", "vertex shader bench: VRAM copy of %lu bytes does not read back", (unsigned long)size);
        vramFree(dst);
        return NULL;
    }
    return dst;
}

void gx_vbench(void) {
    if (!top_target || !cfg_bool("S3D_BENCH", true) || !cfg_bool("S3D_BENCH_VSH", true)) return;
    GxOutVertex* v = linearAlloc(VB_VERTS * sizeof(GxOutVertex));
    GxPackedVertex* pv = linearAlloc(VB_VERTS * sizeof(GxPackedVertex));
    VbCompactVertex* cv = linearAlloc(VB_VERTS * sizeof(VbCompactVertex));
    u16* idx = linearAlloc(VB_VERTS * sizeof(u16));
    void *v_vram = NULL, *pv_vram = NULL;
    DVLB_s* io_dvlb = NULL;
    shaderProgram_s io_progs[VB_PROGS - VB_IO7];
    int io_count = 0;
    if (!v || !pv || !cv || !idx) {
        LOG_WARN("bench", "vertex shader bench: out of linear memory");
        goto done;
    }
    for (int i = 0; i < VB_VERTS; i++) {
        float cx = (i / 3 % 64) / 32.0f - 1.0f, cy = (i / 192 % 64) / 32.0f - 1.0f;
        memset(&v[i], 0, sizeof(v[i]));
        v[i].pos[0] = cx + ((i % 3) == 1 ? 0.01f : 0.0f);
        v[i].pos[1] = cy + ((i % 3) == 2 ? 0.01f : 0.0f);
        v[i].pos[2] = -0.5f;
        memset(v[i].color, 200, 4);
        v[i].nrm[2] = 16384;
        memset(&pv[i], 0, sizeof(pv[i]));
        pv[i].pos[0] = (s16)(v[i].pos[0] * 16384);
        pv[i].pos[1] = (s16)(v[i].pos[1] * 16384);
        pv[i].pos[2] = (s16)(v[i].pos[2] * 16384);
        memset(pv[i].color, 200, 4);
        pv[i].nrm[2] = 16384;
        memcpy(cv[i].pos, v[i].pos, sizeof(cv[i].pos));
        memset(cv[i].color, 200, 4);
        cv[i].tc[0] = cv[i].tc[1] = 0.0f;
        // Sliding triangles (k, k+1, k+2): each vertex in 3 consecutive triangles
        idx[i] = (u16)(i / 3 + i % 3);
    }
    GSPGPU_FlushDataCache(v, VB_VERTS * sizeof(GxOutVertex));
    GSPGPU_FlushDataCache(pv, VB_VERTS * sizeof(GxPackedVertex));
    GSPGPU_FlushDataCache(cv, VB_VERTS * sizeof(VbCompactVertex));
    GSPGPU_FlushDataCache(idx, VB_VERTS * sizeof(u16));
    v_vram = vb_vram_copy(v, VB_VERTS * sizeof(GxOutVertex));
    pv_vram = vb_vram_copy(pv, VB_VERTS * sizeof(GxPackedVertex));
    io_dvlb = DVLB_ParseFile((u32*)vbench_shbin, vbench_shbin_size);
    if (io_dvlb) {
        io_count = io_dvlb->numDVLE < VB_PROGS - VB_IO7 ? (int)io_dvlb->numDVLE : VB_PROGS - VB_IO7;
        for (int i = 0; i < io_count; i++) {
            shaderProgramInit(&io_progs[i]);
            shaderProgramSetVsh(&io_progs[i], &io_dvlb->DVLE[i]);
        }
    }
    C3D_AttrInfo ai, pai, ai3;
    AttrInfo_Init(&ai);
    AttrInfo_AddLoader(&ai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&ai, 2, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&ai, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ai, 4, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ai, 5, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ai, 6, GPU_SHORT, 4);
    AttrInfo_Init(&pai);
    AttrInfo_AddLoader(&pai, 0, GPU_SHORT, 4);
    AttrInfo_AddLoader(&pai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&pai, 2, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&pai, 3, GPU_SHORT, 2);
    AttrInfo_AddLoader(&pai, 4, GPU_SHORT, 2);
    AttrInfo_AddLoader(&pai, 5, GPU_SHORT, 2);
    AttrInfo_AddLoader(&pai, 6, GPU_SHORT, 4);
    AttrInfo_Init(&ai3);
    AttrInfo_AddLoader(&ai3, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&ai3, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&ai3, 2, GPU_FLOAT, 2);
    GSPGPU_SetPerfLogMode(true);

    double best[VB_COUNT];
    for (int t = 0; t < VB_COUNT; t++) best[t] = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        for (int t = 0; t < VB_COUNT; t++) {
            const VbTest* tc = &s_vb_tests[t];
            bool vram = (tc->flags & VBX_VRAM) != 0;
            bool packed = tc->layout == VBL_PACKED;
            if (vram && !(packed ? pv_vram : v_vram)) continue;
            if (tc->prog >= VB_IO7 && tc->prog - VB_IO7 >= io_count) continue;
            int lights = tc->lights;
            bool warp = tc->prog == GX_VSH_WARP;
            shaderProgram_s* prog = tc->prog >= VB_IO7 ? &io_progs[tc->prog - VB_IO7] : &shader_programs[tc->prog];
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            u32 before = vb_p3d_us();
            C3D_FrameDrawOn(top_target);
            C3D_BindProgram(prog);
            C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] = true;
            for (int r = 0; r < 4; r++) hwtest_unif(prog, "projection", r, r == 0, r == 1, r == 2 ? -1.0f : 0.0f, r == 3);
            for (int m = 0; m < 10; m++)
                for (int r = 0; r < 3; r++) hwtest_unif(prog, "posmtx", m * 3 + r, r == 0, r == 1, r == 2, 0.0f);
            for (int u = 0; u < 3; u++) {
                hwtest_unif(prog, "texmtx", u * 3 + 0, 1.0f, 0.0f, 0.0f, 0.0f);
                hwtest_unif(prog, "texmtx", u * 3 + 1, 0.0f, 1.0f, 0.0f, 0.0f);
                hwtest_unif(prog, "texmtx", u * 3 + 2, 0.0f, 0.0f, 0.0f, 1.0f);
                if (tc->lean && u >= tc->lean) hwtest_unif(prog, "texsrc", u, 0.0f, 2.0f, 0.0f, 0.0f);  // Skipped
                else hwtest_unif(prog, "texsrc", u, 1.0f, 0.0f, 0.0f, warp ? 0.5f : 0.0f);
            }
            float ps = packed ? 1.0f / 16384.0f : 1.0f;
            hwtest_unif(prog, "posscale", 0, ps, 1.0f / 16384.0f, packed ? 1.0f / 16384.0f : 1.0f, 0.0f);
            hwtest_unif(prog, "texwarp", 0, 0.1f, 0.1f, 0.25f, 0.5f);
            for (int l = 0; l < 8; l++) {
                hwtest_unif(prog, "lightpos", l, 0.3f * l, 0.5f, 1.0f, 1.0f);
                hwtest_unif(prog, "lightdir", l, 0.0f, 0.0f, -1.0f, 1.0f);
                hwtest_unif(prog, "lightcol", l, 1.0f, 0.9f, 0.8f, 1.0f);
                hwtest_unif(prog, "lighta", l, 1.0f, 0.0f, 0.0f, 0.0f);
                hwtest_unif(prog, "lightk", l, 1.0f, 0.1f, 0.01f, 1.0f);
            }
            hwtest_unif(prog, "matreg", 0, 1.0f, 1.0f, 1.0f, 1.0f);
            hwtest_unif(prog, "ambreg", 0, 0.2f, 0.2f, 0.2f, 1.0f);
            hwtest_unif(prog, "chansel", 0, 1.0f, 1.0f, 0.0f, 0.0f);
            hwtest_unif(prog, "litsel", 0, 1.0f, 1.0f, 0.0f, 0.0f);
            hwtest_bool(prog, "lighting", lights > 0);
            s8 len = shaderInstanceGetUniformLocation(prog->vertexShader, "lighten");
            for (int l = 0; l < 8 && len >= 0; l++) C3D_BoolUnifSet(GPU_VERTEX_SHADER, len + l, l < lights);
            hwtest_bool(prog, "warpon", warp);
            hwtest_bool(prog, "fraglit", (tc->flags & VBX_FRAG) != 0);
            hwtest_bool(prog, "nrmout", false);
            hwtest_lean(prog, tc->lean != 0);
            C3D_BufInfo bi;
            BufInfo_Init(&bi);
            if (packed) {
                C3D_SetAttrInfo(&pai);
                BufInfo_Add(&bi, vram ? pv_vram : pv, sizeof(GxPackedVertex), 7, 0x6543210);
            } else if (tc->layout == VBL_IN3_STRIDE) {
                // Position, color, (skip 4: matrix index), texcoord 0, (skip 16 + 8: tc1, tc2, normal)
                C3D_SetAttrInfo(&ai3);
                BufInfo_Add(&bi, v, sizeof(GxOutVertex), 6, 0xDF2C10);
            } else if (tc->layout == VBL_COMPACT) {
                C3D_SetAttrInfo(&ai3);
                BufInfo_Add(&bi, cv, sizeof(VbCompactVertex), 3, 0x210);
            } else {
                C3D_SetAttrInfo(&ai);
                BufInfo_Add(&bi, vram ? v_vram : v, sizeof(GxOutVertex), 7, 0x6543210);
            }
            C3D_SetBufInfo(&bi);
            for (int u = 0; u < 3; u++) gx_c3d_tex_unbind(u);
            C3D_TexEnv* env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
            for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
            C3D_LightEnvBind(NULL);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_CullFace(GPU_CULL_NONE);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            if (tc->flags & VBX_REUSE) C3D_DrawElements(GPU_TRIANGLES, VB_VERTS, C3D_UNSIGNED_SHORT, idx);
            else C3D_DrawArrays(GPU_TRIANGLES, 0, VB_VERTS);
            C3D_FrameEnd(0);
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // Waits for the frame above
            u32 us = vb_p3d_us() - before;
            C3D_FrameEnd(0);
            if (us > 0 && us < 1000000 && us < best[t]) best[t] = us;
        }
    }
    LOG_INFO("bench", "vertex shaders (%d vertices or indices per test, GSP P3D time):", VB_VERTS);
    for (int t = 0; t < VB_COUNT; t++) {
        if (best[t] >= 1e9) {
            LOG_INFO("bench", "  %-36s n/a", s_vb_tests[t].name);
            continue;
        }
        LOG_INFO("bench", "  %-36s %7.0f us: %6.3f us per vertex, %5.2f Mvert/s", s_vb_tests[t].name, best[t],
                 best[t] / VB_VERTS, VB_VERTS / best[t]);
    }
    gx_restore_state();  // Binds the game's program again before the I/O programs go
done:
    for (int i = 0; i < io_count; i++) shaderProgramFree(&io_progs[i]);
    if (io_dvlb) DVLB_Free(io_dvlb);
    if (v_vram) vramFree(v_vram);
    if (pv_vram) vramFree(pv_vram);
    if (v) linearFree(v);
    if (pv) linearFree(pv);
    if (cv) linearFree(cv);
    if (idx) linearFree(idx);
}

enum { HW_BENCH = -1, HW_T0 = 100 };  // Program sources besides the game's GX_VSH_* variants

// Fragment lighting setups for the bug 14 steps (the first game draw with
// fragment lighting hangs on hardware)
enum { HW_LIGHT_NONE, HW_LIGHT_PLAIN, HW_LIGHT_SPEC, HW_LIGHT_FOG };
// Self-test textures on unit 0. _OFS: the data starts 0x60 bytes past a
// 128-byte boundary, like the zero-copy game texture that hung (bug 15)
enum { HW_TEX_NONE, HW_TEX_565MIP, HW_TEX_ETC1, HW_TEX_ETC1MIP, HW_TEX_565MIP_OFS, HW_TEX_ETC1MIP_OFS, HW_TEX_COUNT };

static void hwtest_lut_fill(C3D_LightLut* lut, bool ramp) {
    float data[512];
    for (int j = 0; j < 256; j++) data[j] = ramp ? (j < 128 ? j / 128.0f : 0.0f) : 1.0f;
    for (int j = 0; j < 256; j++) data[256 + j] = j < 255 ? data[j + 1] - data[j] : 0.0f;
    LightLut_FromArray(lut, data);
}

// HW_LIGHT_PLAIN: one directional light, no LUTs. HW_LIGHT_SPEC: gx_render.c
// setup_spec_lights as in the hanging draw (2 directional specular lights with
// an N.H spotlight LUT, D0 off, clamped highlights). HW_LIGHT_FOG: fog_env_init
// (ambient light at the eye with a distance attenuation LUT)
static C3D_LightEnv* hwtest_light_env(int kind) {
    static C3D_LightEnv env;
    static C3D_Light light[2];
    static C3D_LightLut sp;
    static C3D_LightLutDA da;
    C3D_LightEnvInit(&env);
    C3D_Material mtl = { .ambient = { 1.0f, 1.0f, 1.0f }, .specular0 = { 1.0f, 1.0f, 1.0f } };
    C3D_LightEnvMaterial(&env, &mtl);
    C3D_LightEnvAmbient(&env, 0.0f, 0.0f, 0.0f);
    if (kind == HW_LIGHT_FOG) {
        C3D_LightInit(&light[0], &env);
        C3D_LightAmbient(&light[0], 1.0f, 1.0f, 1.0f);
        C3D_LightColor(&light[0], 0.0f, 0.0f, 0.0f);
        C3D_FVec eye = FVec4_New(0.0f, 0.0f, 0.0f, 1.0f);
        C3D_LightPosition(&light[0], &eye);
        hwtest_lut_fill(&da.lut, true);
        da.scale = 1.0f / 100.0f;
        da.bias = 0.0f;
        C3D_LightDistAttn(&light[0], &da);
        C3D_LightDistAttnEnable(&light[0], true);
        return &env;
    }
    int n = kind == HW_LIGHT_SPEC ? 2 : 1;
    if (kind == HW_LIGHT_SPEC) {
        hwtest_lut_fill(&sp, true);
        C3D_LightEnvLut(&env, GPU_LUT_D0, GPU_LUTINPUT_LN, true, NULL);
        C3D_LightEnvLut(&env, GPU_LUT_SP, GPU_LUTINPUT_NH, true, NULL);
        C3D_LightEnvClampHighlights(&env, true);
    }
    for (int i = 0; i < n; i++) {
        C3D_LightInit(&light[i], &env);
        C3D_LightAmbient(&light[i], 0.0f, 0.0f, 0.0f);
        C3D_LightDiffuse(&light[i], kind == HW_LIGHT_PLAIN ? 1.0f : 0.0f, 0.0f, 0.0f);
        C3D_LightSpecular0(&light[i], 0.0f, 1.0f, 0.0f);
        C3D_FVec dir = FVec4_New(0.0f, 0.0f, 1.0f, 0.0f);
        C3D_LightPosition(&light[i], &dir);
        if (kind == HW_LIGHT_SPEC) {
            C3D_LightSpotEnable(&light[i], true);
            C3D_LightSpotLut(&light[i], &sp);
        }
        C3D_LightDistAttnEnable(&light[i], false);
    }
    return &env;
}

void gx_hwtest(void) {
    if (!cfg_int("S3D_GPU_SELFTEST", 0) || !top_target) return;
    // Steps with a mode only run in that S3D_GPU_SELFTEST mode (9: the steps
    // known to hang). A hang ends the run, so S3D_GPU_SELFTEST_FROM=k starts
    // at step k to test the steps after a hanging one
    int mode = cfg_int("S3D_GPU_SELFTEST", 0);
    int from = cfg_int("S3D_GPU_SELFTEST_FROM", 1);
    static const struct {
        const char* name;
        int prog;  // GX_VSH_*, HW_BENCH, or HW_T0 + n (gx/hwtest/tn.v.pica)
        bool bench_layout, elements, lighting;
        int mode;  // 0: always, else only in that S3D_GPU_SELFTEST mode
        bool fraglit;       // Shader fragnq path
        int tex;            // HW_TEX_*: texture on unit 0
        int light;          // HW_LIGHT_*: fragment lighting environment
    } steps[] = {
        { "bench shader + bench layout (24 B), as os/bench.c", HW_BENCH, true, false, false, 0 },
        { "t0 (bench shader copy) + GxOutVertex layout (52 B)", HW_T0 + 0, false, false, false, 0 },
        { "t1: position + color only", HW_T0 + 1, false, false, false, 0 },
        { "t2: + mova / relative posmtx + projection", HW_T0 + 2, false, false, false, 0 },
        { "tz: t2's code at the end of hwtest.shbin", HW_T0 + 16, false, false, false, 0 },
        { "t8: t4 with all output components written", HW_T0 + 8, false, false, false, 0 },
        { "game lit shader (entry 0, lighting off), DrawArrays", GX_VSH_LIT, false, false, false, 0 },
        { "game unlit shader, DrawArrays", GX_VSH_UNLIT, false, false, false, 0 },
        { "game unlit shader, DrawElements", GX_VSH_UNLIT, false, true, false, 0 },
        { "game lit shader (lighting off), DrawElements", GX_VSH_LIT, false, true, false, 0 },
        { "game lit shader (lighting on), DrawElements", GX_VSH_LIT, false, true, true, 0 },
        { "game warp shader, DrawElements", GX_VSH_WARP, false, true, false, 0 },
        // texcoord0w layouts, all with full writes. t7 passed on hardware but needs
        // an 8th output map slot (the game shaders use all 7); t6 and ta share
        // texcoord0's register, so either would restore projective unit 0
        { "t7: texcoord0w in its own output register", HW_T0 + 7, false, false, false, 0 },
        { "t6: texcoord0w in texcoord0's .w", HW_T0 + 6, false, false, false, 0 },
        { "ta: texcoord0w in texcoord0's .z (old game layout)", HW_T0 + 10, false, false, false, 0 },
        // Bug 14: the first game draw with fragment lighting and a mipmapped
        // texture hangs. One suspect per step
        { "game lit shader, fraglit on (fragnq path), no light env", GX_VSH_LIT, false, true, true, 0, true, HW_TEX_NONE, HW_LIGHT_NONE },
        { "mipmapped 64x64 RGB565 texture (3 levels)", GX_VSH_LIT, false, true, false, 0, false, HW_TEX_565MIP, HW_LIGHT_NONE },
        { "fragment lighting: 1 directional light, no LUTs", GX_VSH_LIT, false, true, true, 0, true, HW_TEX_NONE, HW_LIGHT_PLAIN },
        { "fragment lighting: 2 specular lights, N.H spotlight LUT", GX_VSH_LIT, false, true, true, 0, true, HW_TEX_NONE, HW_LIGHT_SPEC },
        { "fragment lighting: fog light (distance attenuation LUT)", GX_VSH_LIT, false, true, false, 0, false, HW_TEX_NONE, HW_LIGHT_FOG },
        { "game draw: spec lights + mipmapped texture", GX_VSH_LIT, false, true, true, 0, true, HW_TEX_565MIP, HW_LIGHT_SPEC },
        // Bug 14, 17th log: tb / tc (cmp, jmpc) passed but step 16 still hung with a
        // branchless fragnq proc. Suspects: the proc writing outview / outnq a
        // second time, or reaching it through ifu + call. td (both) is the old
        // game shader's structure, at the end of mode 9. 19th log: tf passed, te
        // hung (the double write is the cause), so te only runs in mode 9
        { "tf: fraglit proc called under ifu, each output written once", HW_T0 + 15, false, true, false, 0, true },
        { "te: outview / outnq written twice, inline under ifu (hangs)", HW_T0 + 14, false, true, false, 9, true },
        // Hang on hardware (partial output writes): only with S3D_GPU_SELFTEST=9
        { "t9: texcoord0 written as .xy only (hangs)", HW_T0 + 9, false, false, false, 9 },
        { "t4: + texcoord0/1/2 divided per vertex, .xy writes (hangs)", HW_T0 + 4, false, false, false, 9 },
        // Bug 14: the game lit shader's old fragnq code (step 16 hung with it).
        // Is it cmp itself or the jmpc after it?
        { "tb: cmp, result unused (old fragnq without jmpc)", HW_T0 + 11, false, false, false, 9 },
        { "tc: cmp + jmpc (old fragnq, hung in step 16)", HW_T0 + 12, false, false, false, 9 },
        { "td: outview / outnq written twice, the second time in a called proc (old structure)", HW_T0 + 13, false, true, false, 9, true },
        // Bug 15 (20th log): the game hung on its first ETC1 texture, 128x64 with
        // levels 0-3, zero-copy at pa 210C5960 (32-byte aligned; every texture
        // that passed was 128-byte aligned). ETC1 itself, ETC1 mipmaps, or the
        // alignment? Appended so the step numbers above stay the same
        { "ETC1 64x64 texture, no mipmaps", GX_VSH_LIT, false, true, false, 0, false, HW_TEX_ETC1, HW_LIGHT_NONE },
        { "ETC1 128x64 texture, levels 0-3", GX_VSH_LIT, false, true, false, 0, false, HW_TEX_ETC1MIP, HW_LIGHT_NONE },
        { "mipmapped 64x64 RGB565 texture at pa % 128 = 0x60", GX_VSH_LIT, false, true, false, 0, false, HW_TEX_565MIP_OFS, HW_LIGHT_NONE },
        { "ETC1 128x64 levels 0-3 at pa % 128 = 0x60 (the hanging game texture)", GX_VSH_LIT, false, true, false, 0, false, HW_TEX_ETC1MIP_OFS, HW_LIGHT_NONE },
    };
    enum { NSTEPS = sizeof(steps) / sizeof(steps[0]) };

    DVLB_s* bench_dvlb = DVLB_ParseFile((u32*)bench_shbin, bench_shbin_size);
    DVLB_s* test_dvlb = DVLB_ParseFile((u32*)hwtest_shbin, hwtest_shbin_size);
    if (!bench_dvlb || !test_dvlb) {
        LOG_WARN("hwtest", "could not parse the test shaders");
        return;
    }
    static shaderProgram_s bench_prog, test_progs[24];
    shaderProgramInit(&bench_prog);
    shaderProgramSetVsh(&bench_prog, &bench_dvlb->DVLE[0]);
    int ntest = test_dvlb->numDVLE < 24 ? (int)test_dvlb->numDVLE : 24;
    for (int i = 0; i < ntest; i++) {
        shaderProgramInit(&test_progs[i]);
        shaderProgramSetVsh(&test_progs[i], &test_dvlb->DVLE[i]);
    }

    GxOutVertex* v = linearAlloc(6 * sizeof(GxOutVertex));
    u8* bv = linearAlloc(6 * 24);  // Bench layout: float pos[3], u8 color[4], float tc[2]
    u16* idx = linearAlloc(8 * sizeof(u16));
    if (!v || !bv || !idx) return;
    static const float xy[6][2] = { { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f },
                                    { -0.5f, 0.5f },  { 0.5f, -0.5f }, { 0.5f, 0.5f } };
    memset(v, 0, 6 * sizeof(GxOutVertex));
    memset(bv, 0, 6 * 24);
    for (int i = 0; i < 6; i++) {
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].pos[2] = 0.5f;
        memset(v[i].color, 0xFF, 4);
        v[i].nrm[2] = 16384;
        idx[i] = (u16)i;
        float* p = (float*)(bv + i * 24);
        p[0] = xy[i][0];
        p[1] = xy[i][1];
        p[2] = 0.5f;
        memset(bv + i * 24 + 12, 0xFF, 4);
    }
    GSPGPU_FlushDataCache(v, 6 * sizeof(GxOutVertex));
    GSPGPU_FlushDataCache(bv, 6 * 24);
    GSPGPU_FlushDataCache(idx, 8 * sizeof(u16));
    for (int i = 0; i < 6; i++) {
        v[i].tc[0][0] = xy[i][0] + 0.5f;
        v[i].tc[0][1] = xy[i][1] + 0.5f;
    }
    GSPGPU_FlushDataCache(v, 6 * sizeof(GxOutVertex));

    // Unit 0 textures. 565MIP is like bug 14's draw (64x64 RGB565, levels
    // 0-3), ETC1MIP like bug 15's (128x64 ETC1, levels 0-3). The _OFS copies
    // share the data of the 128-aligned ones, moved 0x60 bytes into a buffer
    static C3D_Tex hw_tex[HW_TEX_COUNT];
    static bool hw_tex_ok[HW_TEX_COUNT];
    static u8* hw_ofs_buf[HW_TEX_COUNT];
    hw_tex_ok[HW_TEX_565MIP] = C3D_TexInitMipmap(&hw_tex[HW_TEX_565MIP], 64, 64, GPU_RGB565);
    hw_tex_ok[HW_TEX_ETC1] = C3D_TexInit(&hw_tex[HW_TEX_ETC1], 64, 64, GPU_ETC1);
    hw_tex_ok[HW_TEX_ETC1MIP] = C3D_TexInitMipmap(&hw_tex[HW_TEX_ETC1MIP], 128, 64, GPU_ETC1);
    for (int t = HW_TEX_565MIP; t <= HW_TEX_ETC1MIP; t++) {
        if (!hw_tex_ok[t]) continue;
        C3D_Tex* tex = &hw_tex[t];
        u32 size = C3D_TexCalcTotalSize(tex->size, tex->maxLevel);
        if (tex->fmt == GPU_RGB565) {
            u16* px = (u16*)tex->data;
            for (u32 i = 0; i < size / 2; i++) px[i] = (i & 8) ? 0xF800 : 0x07FF;
        } else {
            // Any 64-bit word is a valid ETC1 block: alternate two flat colors
            u32* w = (u32*)tex->data;
            for (u32 i = 0; i < size / 4; i += 2) {
                w[i] = 0;
                w[i + 1] = (i & 2) ? 0xF000F000 : 0x00F0F000;
            }
        }
        GSPGPU_FlushDataCache(tex->data, size);
        C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);
        if (tex->maxLevel) C3D_TexSetFilterMipmap(tex, GPU_LINEAR);
        LOG_INFO("hwtest", "tex %d: %ux%u fmt %d levels 0-%u, %lu B at pa %08lX", t, tex->width, tex->height,
                 (int)tex->fmt, tex->maxLevel, (unsigned long)size, (unsigned long)osConvertVirtToPhys(tex->data));
        int ofs = t == HW_TEX_565MIP ? HW_TEX_565MIP_OFS : t == HW_TEX_ETC1MIP ? HW_TEX_ETC1MIP_OFS : 0;
        if (!ofs || !(hw_ofs_buf[ofs] = linearMemAlign(size + 0x80, 0x80))) continue;
        hw_tex[ofs] = *tex;
        hw_tex[ofs].data = hw_ofs_buf[ofs] + 0x60;
        memcpy(hw_tex[ofs].data, tex->data, size);
        GSPGPU_FlushDataCache(hw_tex[ofs].data, size);
        hw_tex_ok[ofs] = true;
        LOG_INFO("hwtest", "tex %d: tex %d's data at pa %08lX", ofs, t, (unsigned long)osConvertVirtToPhys(hw_tex[ofs].data));
    }

    C3D_AttrInfo ai;  // gx_render.c s_attr_std
    AttrInfo_Init(&ai);
    AttrInfo_AddLoader(&ai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&ai, 2, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&ai, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ai, 4, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ai, 5, GPU_FLOAT, 2);
    AttrInfo_AddLoader(&ai, 6, GPU_SHORT, 4);
    C3D_AttrInfo bai;  // os/bench.c s_attr_f
    AttrInfo_Init(&bai);
    AttrInfo_AddLoader(&bai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&bai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&bai, 2, GPU_FLOAT, 2);

    LOG_INFO("hwtest", "GPU self-test: %d steps (%d test shaders)", NSTEPS, ntest);
    // Bugs 12/13: is the shader code in memory the code in the file? Every
    // shader that hung starts at code word 31 or later, every one that passed
    // ends before it. tools/build_id.py prints the expected hashes
    extern u32 port_fnv1a(const void* lo, const void* hi);
    LOG_INFO("hwtest", "shbin hash: default %08lX (%lu B), bench %08lX (%lu B), hwtest %08lX (%lu B)",
             (unsigned long)port_fnv1a(default_shbin, default_shbin + (default_shbin_size & ~3u)), (unsigned long)default_shbin_size,
             (unsigned long)port_fnv1a(bench_shbin, bench_shbin + (bench_shbin_size & ~3u)), (unsigned long)bench_shbin_size,
             (unsigned long)port_fnv1a(hwtest_shbin, hwtest_shbin + (hwtest_shbin_size & ~3u)), (unsigned long)hwtest_shbin_size);
    for (u32 o = 0; o < hwtest_shbin_size; o += 32) {
        char line[80];
        int n = 0;
        for (u32 i = o; i < o + 32 && i < hwtest_shbin_size; i += 4)
            n += snprintf(line + n, sizeof(line) - n, " %08lX", (unsigned long)*(const u32*)(hwtest_shbin + i));
        LOG_INFO("hwtest", "shbin %04lX:%s", (unsigned long)o, line);
    }
    log_flush();
    for (int s = 0; s < NSTEPS; s++) {
        int src = steps[s].prog;
        shaderProgram_s* prog = src == HW_BENCH ? &bench_prog
                                : src >= HW_T0  ? (src - HW_T0 < ntest ? &test_progs[src - HW_T0] : NULL)
                                                : &shader_programs[src];
        if (!prog || (steps[s].mode && steps[s].mode != mode) || s + 1 < from) continue;
        LOG_INFO("hwtest", "step %d: %s (code words %u-%u)", s + 1, steps[s].name,
                 (unsigned)prog->vertexShader->dvle->mainOffset, (unsigned)prog->vertexShader->dvle->endmainOffset);
        log_flush();
        u64 t0 = svcGetSystemTick();
        C3D_FrameBegin(0);
        C3D_RenderTargetClear(top_target, C3D_CLEAR_ALL, 0x000000FF, 0);
        C3D_FrameDrawOn(top_target);
        C3D_BindProgram(prog);
        for (int r = 0; r < 4; r++) hwtest_unif(prog, "projection", r, r == 0, r == 1, r == 2 ? -1.0f : 0.0f, r == 3);
        for (int m = 0; m < 10; m++)
            for (int r = 0; r < 3; r++) hwtest_unif(prog, "posmtx", m * 3 + r, r == 0, r == 1, r == 2, 0.0f);
        for (int u = 0; u < 3; u++) {
            hwtest_unif(prog, "texmtx", u * 3 + 0, 1.0f, 0.0f, 0.0f, 0.0f);
            hwtest_unif(prog, "texmtx", u * 3 + 1, 0.0f, 1.0f, 0.0f, 0.0f);
            hwtest_unif(prog, "texmtx", u * 3 + 2, 0.0f, 0.0f, 0.0f, 1.0f);
            hwtest_unif(prog, "texsrc", u, 1.0f, 0.0f, 0.0f, 0.0f);
        }
        hwtest_unif(prog, "posscale", 0, 1.0f, 1.0f / 16384.0f, 1.0f, 0.0f);
        for (int l = 0; l < 8; l++) {
            hwtest_unif(prog, "lightpos", l, 0.0f, 0.0f, 1.0f, 1.0f);
            hwtest_unif(prog, "lightdir", l, 0.0f, 0.0f, -1.0f, 1.0f);
            hwtest_unif(prog, "lightcol", l, 1.0f, 1.0f, 1.0f, 1.0f);
            hwtest_unif(prog, "lighta", l, 1.0f, 0.0f, 0.0f, 0.0f);
            hwtest_unif(prog, "lightk", l, 1.0f, 0.0f, 0.0f, 1.0f);
        }
        hwtest_unif(prog, "matreg", 0, 1.0f, 1.0f, 1.0f, 1.0f);
        hwtest_unif(prog, "ambreg", 0, 0.2f, 0.2f, 0.2f, 1.0f);
        hwtest_bool(prog, "lighting", steps[s].lighting);
        for (int l = 0; l < 8; l++) {
            s8 loc = shaderInstanceGetUniformLocation(prog->vertexShader, "lighten");
            if (loc >= 0) C3D_BoolUnifSet(GPU_VERTEX_SHADER, loc + l, steps[s].lighting && l == 0);
        }
        hwtest_bool(prog, "warpon", false);
        hwtest_bool(prog, "fraglit", steps[s].fraglit);
        hwtest_lean(prog, false);  // Every slot on the general path, as before the lean shaders
        hwtest_bool(prog, "nrmout", false);
        C3D_BufInfo bi;
        BufInfo_Init(&bi);
        if (steps[s].bench_layout) {
            C3D_SetAttrInfo(&bai);
            BufInfo_Add(&bi, bv, 24, 3, 0x210);
        } else {
            C3D_SetAttrInfo(&ai);
            BufInfo_Add(&bi, v, sizeof(GxOutVertex), 7, 0x6543210);
        }
        C3D_SetBufInfo(&bi);
        for (int u = 0; u < 3; u++) gx_c3d_tex_unbind(u);
        bool mip = steps[s].tex != HW_TEX_NONE && hw_tex_ok[steps[s].tex];
        if (mip) C3D_TexBind(0, &hw_tex[steps[s].tex]);
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        if (mip) {
            C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
        } else {
            C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
        for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
        if (steps[s].light != HW_LIGHT_NONE) {
            // Use the lighting result, as the game's combiners do
            C3D_TexEnv* e1 = C3D_GetTexEnv(1);
            C3D_TexEnvSrc(e1, C3D_RGB, GPU_PREVIOUS, GPU_FRAGMENT_PRIMARY_COLOR, GPU_FRAGMENT_SECONDARY_COLOR);
            C3D_TexEnvSrc(e1, C3D_Alpha, GPU_PREVIOUS, GPU_PREVIOUS, GPU_PREVIOUS);
            C3D_TexEnvFunc(e1, C3D_RGB, GPU_ADD_MULTIPLY);
            C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
        }
        C3D_LightEnvBind(steps[s].light != HW_LIGHT_NONE ? hwtest_light_env(steps[s].light) : NULL);
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_CullFace(GPU_CULL_NONE);
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);
        C3D_AlphaTest(false, GPU_ALWAYS, 0);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        if (steps[s].elements) C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, idx);
        else C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
        C3D_FrameEnd(0);
        if (!gxCmdQueueWait(&__C3D_Context.gxQueue, 2000LL * 1000 * 1000)) {
            LOG_ERROR("hwtest", "step %d (%s) hung the GPU", s + 1, steps[s].name);
            log_flush();
            gxCmdQueueWait(&__C3D_Context.gxQueue, -1);
        }
        LOG_INFO("hwtest", "step %d ok (%llu us)", s + 1,
                 (unsigned long long)((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000000)));
    }
    LOG_INFO("hwtest", "GPU self-test passed");
    log_flush();
    linearFree(v);
    linearFree(bv);
    linearFree(idx);
    for (int t = HW_TEX_565MIP; t <= HW_TEX_ETC1MIP; t++)
        if (hw_tex_ok[t]) C3D_TexDelete(&hw_tex[t]);
    for (int t = 0; t < HW_TEX_COUNT; t++)
        if (hw_ofs_buf[t]) linearFree(hw_ofs_buf[t]);
    gx_restore_state();
}

void gx_restore_state(void) {
    gx_c3d_invalidate();
    for (int u = 0; u < 3; u++) gx_c3d_tex_unbind(u);  // citro2d's textures (bug 12)
    C3D_BindProgram(&shader_programs[shader_variant]);
    C3D_BoolUnifsDirty[GPU_VERTEX_SHADER] = true;
    C3D_LightEnvBind(NULL);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    gx_tev_hw_invalidate();
    g_gx.dirty = GX_DIRTY_ALL;
    g_gx.posmtx_rows = (1u << 30) - 1;
}

void render_frame(void) {
    gx_frame_begin();
    gx_frame_end();
}

void clear_top_screen(unsigned int color) {
    g_gx.clear_color = (GXColor){ color >> 24, (color >> 16) & 0xFF, (color >> 8) & 0xFF, 0xFF };
}

void gx_set_projection(const C3D_Mtx* projection) {
    (void)projection;  // Projection comes from GXSetProjection
}

void gx_set_clear_black(bool black) {
    clear_black = black;
}

// The target the game's frame is drawn into (GXPeekZ, self test)
C3D_RenderTarget* gx_top_target(void) {
    return frame_active ? game_target() : top_target;
}

// citro2d is set up once for the touch UI and the low resolution upscale
bool gx_c2d_init(void) {
    static int state = -1;
    if (state < 0) state = C2D_Init(1024);
    return state;
}

// --- Reduced resolution --------------------------------------------------------

static bool lowres_wanted(void) {
    static int on = -1;
    CFG_LIVE(on, cfg_int("S3D_LOWRES", 0));
    if (on <= 0 || lowres_failed) return false;
    lowres_vw = on == 2 ? 120 : LOWRES_W;
    lowres_vh = on == 2 ? 200 : LOWRES_H;
    if (lowres_target) return true;
    if (C3D_TexInitVRAM(&lowres_tex, LOWRES_W, LOWRES_H, GPU_RGBA8))
        lowres_target = C3D_RenderTargetCreateFromTex(&lowres_tex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
    if (!lowres_target) {
        if (lowres_tex.data) C3D_TexDelete(&lowres_tex);
        lowres_failed = true;
        LOG_WARN("gx", "reduced resolution unavailable: no VRAM for the %dx%d target", LOWRES_W, LOWRES_H);
        return false;
    }
    C3D_TexSetFilter(&lowres_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&lowres_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    LOG_INFO("gx", "reduced resolution ready (%dx%d)", LOWRES_W, LOWRES_H);
    return true;
}

// Stretch the low resolution frame over the top screen. Texture u runs along
// the framebuffer width (the screen's y, bottom to top) and v along its height
// (the screen's x): a rotated subtexture
static void lowres_present(void) {
    if (!gx_c2d_init()) return;
    PROF_ZONE("gx.lowres_present");
    if (s_frame_pipelined) gx_gpu_wait_previous();  // citro2d's vertex buffer, see gx_ui_begin
    // Only the drawn area (all of it, or 120x200 from the origin in half mode)
    Tex3DS_SubTexture sub = { 400, 240, 0.0f, 0.0f, (float)lowres_vw / LOWRES_W, (float)lowres_vh / LOWRES_H };
    C2D_Image img = { &lowres_tex, &sub };
    C2D_DrawParams p = { { 0.0f, 240.0f, 400.0f, -240.0f }, { 0.0f, 0.0f }, 0.5f, 0.0f };
    C2D_Prepare();
    C2D_SceneBegin(top_target);
    // The frame's alpha is the game's EFB alpha: copy it opaque
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C2D_DrawImage(img, &p, NULL);
    C2D_Flush();
    gx_restore_state();
}

bool gx_frame_is_active(void) {
    return frame_active;
}

bool gx_right_eye_buffers(u8** color, u8** depth) {
    if (!frame_active || !frame_stereo || hud_active || offscreen_active || !big_target) return false;
    *color = (u8*)big_target->frameBuf.colorBuf + C3D_CalcColorBufSize(240, 400, GPU_RB_RGBA8);
    *depth = (u8*)big_target->frameBuf.depthBuf + C3D_CalcDepthBufSize(240, 400, GPU_RB_DEPTH24_STENCIL8);
    return true;
}

// Shadows (touch UI setting S3D_SHADOWS, patches/main/lightmap.c.sed): off,
// objects queue no shadow casters and draw no shadow textures
bool port_shadows_enabled(void) {
    static int on = -1;
    CFG_LIVE(on, cfg_bool("S3D_SHADOWS", true));
    return on;
}

// Shadow levers (MULTICORE_PLAN.md "Shadow levers"; hooks in
// patches/main/newshadows.c.sed renderShadows). Shadows cost ~7-8 ms of GPU
// time in the hub (5A5EC2CB A/B). The casters' render-to-texture passes (a
// full object draw each, up to 8 per frame) are the part these two cut.
// S3D_SHADOW_TEXTURES: casters that get a pass (0-8, default 8); the others
// use their static texture, as casters past the 8th always did
int port_shadow_textures(void) {
    static int n = -1;
    CFG_LIVE(n, cfg_int("S3D_SHADOW_TEXTURES", 8));
    return n < 0 ? 0 : n > 8 ? 8 : n;
}

// S3D_SHADOW_REFRESH=N (default 2, 1 = every frame): caster texture `tex` is
// redrawn on every Nth frame, staggered by slot so the passes spread over the
// frames, as long as the same object held the slot on its last pass.
// Otherwise (new object, slot changed hands) it is drawn now. Returns 1 when
// last frame's copy can be sampled again (copies live 300 frames, gx_copy.c).
// Default 2 since ECE5E4A8 HW: hub frame -3.6 ms, GPU -2.8, no visible lag
int port_shadow_reuse(int tex, const void* obj, int kind) {
    static int refresh = -1;
    static const void* last_obj[8];
    static u32 last_frame[8];
    CFG_LIVE(refresh, cfg_int("S3D_SHADOW_REFRESH", 2));
    (void)kind;
    if (tex < 0 || tex >= 8) return 0;
    u32 f = prof_frame_count();
    bool reuse = refresh >= 2 && last_obj[tex] == obj && f - last_frame[tex] < (u32)refresh &&
                 (f + (u32)tex) % (u32)refresh != 0;
    if (reuse) {
        PROF_COUNT("shadow.tex_reused", 1);
        return 1;
    }
    last_obj[tex] = obj;
    last_frame[tex] = f;
    PROF_COUNT("shadow.tex_drawn", 1);
    return 0;
}

// S3D_RCP_REFRESH=N (default 1 = every frame; patches/main/rcp_dolphin.c.sed
// Rcp_UpdateDistortionTextures, MULTICORE_PLAN.md "Other offscreen passes"):
// the 32x32 lighting textures (a lit hemisphere mesh per slot, sun/moon
// specular or an object's light colour) are redrawn on every Nth frame,
// staggered by slot, while the slot keeps its model and group. Otherwise the
// last copy is sampled again: the mesh pass and the copy are skipped
int port_rcp_reuse(int slot, const void* model, int group) {
    static int refresh = -1;
    static const void* last_model[6];
    static int last_group[6];
    static u32 last_frame[6];
    CFG_LIVE(refresh, cfg_int("S3D_RCP_REFRESH", 1));
    if (slot < 0 || slot >= 6) return 0;
    u32 f = prof_frame_count();
    bool reuse = refresh >= 2 && last_model[slot] == model && last_group[slot] == group &&
                 f - last_frame[slot] < (u32)refresh && (f + (u32)slot) % (u32)refresh != 0;
    if (reuse) {
        PROF_COUNT("rcp.tex_reused", 1);
        return 1;
    }
    last_model[slot] = model;
    last_group[slot] = group;
    last_frame[slot] = f;
    PROF_COUNT("rcp.tex_drawn", 1);
    return 0;
}
