// GX draw submission: batching of decoded triangles, translation of GX pixel
// state (blend, depth, alpha test, cull) to citro3d and upload of transform
// uniforms. Consecutive draws with unchanged GPU state share one draw call.

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "gx_init.h"
#include "gx_internal.h"
#include "gx_c3d_ctx.h"
#include "gx_capture.h"
#include "gx_diag.h"
#include "log.h"
#include "movie_3ds.h"
#include "profiler.h"
#include "perf/draw_src.h"

// Back-face culling (S3D_CULL, read live; MULTICORE_PLAN.md "Back-face
// culling"): 0 = off, 1 = the game's GX cull mode (default), 2 = the same
// with the opposite winding (wrong, for checks: front faces vanish). GX
// front faces come out counter-clockwise on the PICA (checked in Azahar:
// title, attract scenes and hub), so GX back maps to GPU_CULL_FRONT_CCW. The
// PICA culls after the vertex shader: it saves triangle setup and fill, not
// vertex shading
static int cull_setting(void) {
    static int v = -1;
    // On by default: verified clean on hardware (2026-11-01, a live remote
    // A/B: mode 1 identical to culling off at the hub, mode 2 eats whole
    // fronts as the wrong winding must). The earlier "culling errors" (bug
    // 41) were the polluted tests - S3D_OBJ_DIST=30 object popping and
    // S3D_DL_CACHE=0's missing geometry - not the culling itself.
    CFG_LIVE(v, cfg_int("S3D_CULL", 1));
    return v;
}

// Per-frame vertex/index storage in linear memory. Two sets used in turn:
// with GPU pipelining (S3D_GPU_PIPELINE) the GPU may still read the previous
// frame's set while this frame fills the other one
#define MAX_FRAME_VERTICES 0x8000
#define MAX_FRAME_INDICES  0x18000

static GxOutVertex* s_vbuf;
static u16* s_ibuf;

// Per-frame positions/normals of partially cached (CPU-animated) lists
#define FRAME_DYN_BYTES (0xC000 * sizeof(GxDynVertex))
static u8* s_dynbuf;
static u32 s_dynused;  // Bytes

static GxOutVertex* s_vbufs[2];
static u16* s_ibufs[2];
static u8* s_dynbufs[2];
static bool s_buffers_doubled;

// Vertex layouts: standard (GxOutVertex), packed cached lists (GxPackedVertex)
// and the compact dynamic one, where position/normal come as 16-bit/8-bit
// integers from a second buffer
enum { LAYOUT_STD, LAYOUT_DYN16, LAYOUT_PACKED, LAYOUT_COUNT };
static int s_attr_layout;
// Shader inputs (default.v.pica), in GxOutVertex order
enum { VA_POS, VA_CLR, VA_IDX, VA_TC0, VA_TC1, VA_TC2, VA_NRM, VA_COUNT };
static C3D_AttrInfo* attr_info(int layout);
static void apply_vsrc(void);
static C3D_BufInfo* s_ctx_buf;  // citro3d's own copy, in sync with the hardware once bound
static int s_unif_layout;
static float s_cur_tc_scale;
static float s_cur_pos_scale;
static bool s_cur_nrm_pv;  // posscale.w: normal texgens per vertex (upload_texmtx)
static int s_loc_posscale;
static void use_layout(int layout, float pos_scale, float tc_scale);
static u32 s_vused, s_iused;
static u32 s_batch_vstart, s_batch_istart;
static u32 s_vflushed, s_iflushed, s_dynflushed;  // Written back by gx_render_flush_new_writes

static GxUnitMap s_map;

// Per PICA unit: texture coordinate mapping u = au . (s, t, q), v = av . (s, t, q)
static float s_unit_au[GX_PICA_TEX_UNITS][3], s_unit_av[GX_PICA_TEX_UNITS][3];
// Stereo: per-eye EFB copies bound to each unit (NULL: same texture for both eyes)
static C3D_Tex* s_unit_copy[2][GX_PICA_TEX_UNITS];
static void bind_unit(int u, C3D_Tex* t, int eye);
static bool s_eye_rebind;
static bool s_drawn;  // Draws since the last gx_render_clear_drawn (EFB copies)

bool gx_render_has_drawn(void) { return s_drawn; }
void gx_render_clear_drawn(void) { s_drawn = false; }

// Frame dump (debug): create sdmc:/3ds/star3d/gxdump to log the state of
// every batch of the next frame, or of frame N if the file contains N (the
// file is deleted afterwards)
#define DUMP_TRIGGER "sdmc:/3ds/star3d/gxdump"
static u32 s_frame_number;
static bool s_dumping;
static bool s_cap_dump;            // s_dumping comes from the render capture (no hold)
static C3D_Tex s_white_tex;
static bool s_tex_mips = true;    // S3D_TEX_MIPS
static float s_tex_lod_bias;      // S3D_TEX_LODBIAS_X4 / 4
static int s_dbg_mip;             // S3D_DBG_MIP: 0 off, N = every mipmapped texture sampled at level N-1 only
// S3D_RT_DEDUP (MULTICORE_PLAN.md, "Render thread: redundant work"), bit 0:
// texture-only state changes keep the unit map (bits 1-2: gx_rec.c)
static int s_rt_dedup = 5;
int g_rt_memo = 15;
// S3D_PREP_FAST (DRAWCALL_PLAN.md "Render-thread state setup"): 1 konst-only
// changes just refresh the combiner constants (gx_tev_repatch), 2 a unit that
// gets the same texture object again this frame keeps its binding, 4 a
// texture-only change that keeps the combiner program skips fog and light
// environment setup. Default 7
static int s_prep_fast = 7;
// S3D_C3D_FAST (third_party/citro3d/source/s3d_hooks.h): citro3d fast paths,
// 1 template draw packet, 2 uniform upload skips clean flag words, 4 skips
// uniform values the GPU registers already hold. Default 7
extern int g_c3d_fast;
extern u32 g_c3d_unif_stats[3];  // citro3d uniforms.c: vec4 uploaded / skipped, bool writes skipped
static bool s_fog_on = true;      // S3D_FOG
static bool s_fog_zflip;          // S3D_FOG_ZFLIP (LUT index direction, for testing)
static bool s_fog_frag = true;    // S3D_FOG_FRAG (perspective fog through fragment lighting)
static bool s_frag_light = true;   // S3D_FRAG_LIGHT (channel 1 specular through fragment lighting)
static bool s_frag_spec;          // Current TEV program reads channel 1 color from the secondary color
static bool s_vsh_lit, s_vsh_warp;
static int s_light_count;  // Lights the vertex shader evaluates (0 when lighting is off)
// Leaner vertex shaders (MULTICORE_PLAN.md): per texture slot, the affine
// fast path (bool tcaff) and whether the slot is sampled; unused slots 1 and
// 2 are skipped (marker texsrc.y = 2). 0 = every slot on the general path
static int s_vsh_lean = 1;         // S3D_VSH_LEAN: bit 0 affine path, bit 1 skip unused slots 1 and 2
static int s_tex_res;              // S3D_TEX_RES: mip level every texture binds at (0 = full)
static bool s_tc_used[GX_PICA_TEX_UNITS];
// Path of each slot: affine from the texcoord, the position or the normal,
// or general because of a projective q row, a warp or S3D_VSH_LEAN=0
enum { TC_AFF, TC_AFF_POS, TC_AFF_NRM, TC_GEN_PROJ, TC_GEN_WARP, TC_GEN_OFF, TC_KINDS };
static u8 s_tc_kind[GX_PICA_TEX_UNITS];
static u8 s_tev_nout;             // PICA combiner stages used by the current TEV program
static bool s_dbg_idcolor;        // S3D_DBG_IDCOLOR: paint each state change in a color encoding its index
static int s_dbg_texdump = -1;   // S3D_DBG_TEXDUMP: dump this state's textures on the held frame
static C3D_Tex* s_texdump_tex[GX_PICA_TEX_UNITS];
static int s_dbg_skip_from = -1, s_dbg_skip_to = -1;  // S3D_DBG_SKIP_FROM/TO: dumped frame's states not drawn
static bool s_dbg_hold;           // S3D_DBG_HOLD (implied by S3D_DBG_IDCOLOR): freeze on the dumped frame
static u32 s_state_index;         // State changes so far this frame (the dump's "state #N")
static bool s_depth_bias = true;  // S3D_DEPTH_BIAS (viewport near/far -> depth map)

// Uniform locations in default.v.pica
static int s_loc_projection, s_loc_posmtx, s_loc_texmtx;
static int s_loc_lightpos, s_loc_lightdir, s_loc_lightcol, s_loc_lighta, s_loc_lightk;
static int s_loc_matreg, s_loc_ambreg, s_loc_chansel, s_loc_litsel;
static int s_loc_lighten, s_loc_lighting, s_loc_texsrc, s_loc_texwarp, s_loc_warpon, s_loc_fraglit, s_loc_nrmout;
static int s_loc_tcaff;

// 2x2 rotation for the 3DS's sideways-mounted top screen
static float s_tilt[2][2];

// Stereo 3D: both eyes are drawn into one 240x800 target (gx_init.c), the
// left eye in memory rows 0-399 and the right eye in rows 400-799. Each
// draw is submitted once per eye with that eye's viewport, scissor and
// projection.
static int s_eyes = 1;
static float s_eye_sep;            // Parallax at infinity per eye (clip x units)
static float s_eye_conv = 80.0f;   // View distance with zero parallax (S3D_STEREO_CONV)
static bool s_eye_swap;            // S3D_STEREO_SWAP
static bool s_eye_autoconv;        // S3D_STEREO_AUTOCONV: convergence follows Fox
static bool s_eye_reduced;         // S3D_STEREO_REDUCED: "3D: reduced" (port_stereo_reduced)
static float s_eye_focus = -1.0f;  // Latest camera-to-Fox distance (port_stereo_focus), <0 = none
static float s_eye_hud;            // S3D_STEREO_HUD_X1000: HUD parallax, fraction of s_eye_sep
static int s_eye_y[2];             // Viewport y of each eye
static C3D_Mtx s_eye_proj[2];
static int s_scissor[4];           // Left, top, right, bottom of one eye
static bool s_raw_draw;            // gx_render_clear_efb_rect: own projection, no scissor
static bool s_discard;             // gx_draw_discard: draws are dropped, state still applies

// Render census and buffer statistics (gx_render_census_request, gx_render_get_draw_stats)
static bool s_census_on;
static void census_draw(u32 count);
static void census_frame_begin(void);
static u32 s_drop_draws, s_drop_verts, s_drop_frames, s_drop_last, s_drop_cur;
static u32 s_vtx_peak, s_idx_peak, s_dyn_peak, s_dyn_fails;

// Multipass (GRAPHICS_PLAN.md 4.1): a second pass draws the term of a texture
// that found no unit (gx_tev_multipass)
static bool s_mp_enabled = true;   // S3D_TEV_MULTIPASS
static bool s_mp_on;               // The current state draws a second pass
static bool s_mp_split;            // ... for a projected term split off on unit 0 (bug 32)
static bool s_mp_proj;             // ... sampled per fragment with the water shader
static GxMultipass s_mp;
static C3D_TexEnv s_mp_main_env[GX_PICA_TEV_STAGES];
static u8 s_mp_main_buf[2];
enum { FOG_MODE_NONE, FOG_MODE_LUT, FOG_MODE_FRAG };
static u8 s_fog_mode;              // Set by apply_fog
static u32 s_fog_color;

// Water split (gx_tev_water_split): reflection and refraction screen copies
// each need unit 0's per-fragment projection, so they draw in two passes
static bool s_ws_enabled = true;   // S3D_WATER_SPLIT
static bool s_ws_on;
static GxWaterSplit s_ws;
static C3D_TexEnv s_ws_env[2][GX_PICA_TEV_STAGES];
// Bug 28: the split's passes draw with the water shader (GX_VSH_WATER) and
// unit 0 in projection mode, so the PICA divides by q per fragment
static bool s_ws_proj;                 // The current water split samples projectively
static C3D_Tex s_proj_view[2];         // Per eye: unit 0's texture in projection mode
static C3D_Tex* s_proj_src[2];         // The texture each view shows

bool gx_render_set_shader(shaderInstance_s* vsh) {
    struct { int* loc; const char* name; } uniforms[] = {
        { &s_loc_projection, "projection" }, { &s_loc_posmtx, "posmtx" }, { &s_loc_texmtx, "texmtx" },
        { &s_loc_lightpos, "lightpos" }, { &s_loc_lightdir, "lightdir" }, { &s_loc_lightcol, "lightcol" },
        { &s_loc_lighta, "lighta" }, { &s_loc_lightk, "lightk" }, { &s_loc_matreg, "matreg" },
        { &s_loc_ambreg, "ambreg" }, { &s_loc_chansel, "chansel" }, { &s_loc_litsel, "litsel" },
        { &s_loc_lighten, "lighten" }, { &s_loc_lighting, "lighting" }, { &s_loc_texsrc, "texsrc" },
        { &s_loc_posscale, "posscale" }, { &s_loc_texwarp, "texwarp" }, { &s_loc_warpon, "warpon" },
        { &s_loc_fraglit, "fraglit" },
        { &s_loc_nrmout, "nrmout" },
        { &s_loc_tcaff, "tcaff" },
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(uniforms) / sizeof(uniforms[0]); i++) {
        *uniforms[i].loc = shaderInstanceGetUniformLocation(vsh, uniforms[i].name);
        if (*uniforms[i].loc < 0) {
            LOG_ERROR("gx", "shader uniform '%s' not found", uniforms[i].name);
            ok = false;
        }
    }
    return ok;
}

void gx_render_init(void) {
    s_tex_mips = cfg_bool("S3D_TEX_MIPS", true);
    s_tex_lod_bias = cfg_int("S3D_TEX_LODBIAS_X4", 0) / 4.0f;
    s_rt_dedup = cfg_int("S3D_RT_DEDUP", 5);
    s_dbg_mip = cfg_int("S3D_DBG_MIP", 0);
    s_fog_on = cfg_bool("S3D_FOG", true);
    s_fog_zflip = cfg_bool("S3D_FOG_ZFLIP", false);
    s_fog_frag = cfg_bool("S3D_FOG_FRAG", true);
    s_frag_light = cfg_bool("S3D_FRAG_LIGHT", true);
    s_vsh_lean = cfg_int("S3D_VSH_LEAN", 1);
    s_tex_res = cfg_int("S3D_TEX_RES", 0);
    s_dbg_idcolor = cfg_bool("S3D_DBG_IDCOLOR", false);
    s_dbg_hold = s_dbg_idcolor || cfg_bool("S3D_DBG_HOLD", false);
    s_dbg_texdump = cfg_int("S3D_DBG_TEXDUMP", -1);
    s_dbg_skip_from = cfg_int("S3D_DBG_SKIP_FROM", -1);
    s_dbg_skip_to = cfg_int("S3D_DBG_SKIP_TO", s_dbg_skip_from);
    s_depth_bias = cfg_bool("S3D_DEPTH_BIAS", true);
    s_mp_enabled = cfg_bool("S3D_TEV_MULTIPASS", true);
    s_ws_enabled = cfg_bool("S3D_WATER_SPLIT", true);
    for (int i = 0; i < 2; i++) {
        s_vbufs[i] = linearAlloc(MAX_FRAME_VERTICES * sizeof(GxOutVertex));
        s_ibufs[i] = linearAlloc(MAX_FRAME_INDICES * sizeof(u16));
        s_dynbufs[i] = linearAlloc(FRAME_DYN_BYTES);
    }
    if (!s_vbufs[0] || !s_ibufs[0] || !s_dynbufs[0]) {
        LOG_ERROR("gx", "failed to allocate frame vertex/index buffers");
    }
    s_buffers_doubled = s_vbufs[1] && s_ibufs[1] && s_dynbufs[1];
    if (!s_buffers_doubled) {
        // One set only: no GPU pipelining (gx_render_can_pipeline)
        LOG_WARN("gx", "no linear memory for a second set of frame buffers: GPU pipelining off");
        if (s_vbufs[1]) linearFree(s_vbufs[1]);
        if (s_ibufs[1]) linearFree(s_ibufs[1]);
        if (s_dynbufs[1]) linearFree(s_dynbufs[1]);
        s_vbufs[1] = s_vbufs[0];
        s_ibufs[1] = s_ibufs[0];
        s_dynbufs[1] = s_dynbufs[0];
    }
    s_vbuf = s_vbufs[0];
    s_ibuf = s_ibufs[0];
    s_dynbuf = s_dynbufs[0];

    // Extract citro3d's screen rotation from its "tilt" orthographic matrix:
    // with l=-1, r=1, b=-1, t=1 the plain projection is identity in x/y.
    C3D_Mtx tilt;
    Mtx_OrthoTilt(&tilt, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, true);
    s_tilt[0][0] = tilt.r[0].x; s_tilt[0][1] = tilt.r[0].y;
    s_tilt[1][0] = tilt.r[1].x; s_tilt[1][1] = tilt.r[1].y;
    LOG_INFO("gx", "screen tilt [%.0f %.0f; %.0f %.0f]", s_tilt[0][0], s_tilt[0][1], s_tilt[1][0], s_tilt[1][1]);

    // Screen x runs along the memory rows. The PICA's window y is flipped
    // against the rows, so with screen x mapped to -y the left eye (rows
    // 0-399) is the upper half of the 800-pixel window.
    bool flipped = s_tilt[1][0] < 0.0f;
    s_eye_y[0] = flipped ? 400 : 0;
    s_eye_y[1] = flipped ? 0 : 400;
    s_eye_conv = (float)cfg_int("S3D_STEREO_CONV", 80);
    s_eye_swap = cfg_bool("S3D_STEREO_SWAP", false);
    s_eye_autoconv = cfg_bool("S3D_STEREO_AUTOCONV", true);
    s_eye_reduced = cfg_bool("S3D_STEREO_REDUCED", false);
    s_eye_hud = cfg_int("S3D_STEREO_HUD_X1000", 0) / 1000.0f;

    // 8x8 white texture bound when a texture is missing
    C3D_TexInit(&s_white_tex, 8, 8, GPU_RGBA8);
    memset(s_white_tex.data, 0xFF, s_white_tex.size);
    C3D_TexFlush(&s_white_tex);

    gx_state_reset();
    gx_tex_init();
    gx_dlcache_init();
}

void gx_render_deinit(void) {
    gx_dlcache_deinit();
    gx_tex_deinit();
    C3D_TexDelete(&s_white_tex);
    for (int i = 0; i < (s_buffers_doubled ? 2 : 1); i++) {
        if (s_vbufs[i]) linearFree(s_vbufs[i]);
        if (s_ibufs[i]) linearFree(s_ibufs[i]);
        if (s_dynbufs[i]) linearFree(s_dynbufs[i]);
    }
    memset(s_vbufs, 0, sizeof(s_vbufs));
    memset(s_ibufs, 0, sizeof(s_ibufs));
    memset(s_dynbufs, 0, sizeof(s_dynbufs));
    s_buffers_doubled = false;
    s_vbuf = NULL;
    s_ibuf = NULL;
    s_dynbuf = NULL;
    s_vbuf = NULL;
    s_ibuf = NULL;
}

void gx_render_set_stereo(bool on, float separation) {
    s_eyes = on ? 2 : 1;
    s_eye_sep = s_eye_swap ? -separation : separation;
    if (on) PROF_COUNT("gx.stereo_frames", 1);
}

// True while drawing a stereo frame (fur budget, port/fur_3ds.c)
bool port_stereo_active(void) {
    return s_eyes == 2;
}

// "3D: reduced" (GRAPHICS_PLAN.md step 11), true only on stereo frames: half
// the fur shells and grass passes, no motion blur and no DOF/blur filter
// (patches/main/lightmap.c.sed, port/fur_3ds.c)
bool port_stereo_reduced(void) {
    return s_eyes == 2 && s_eye_reduced;
}

// The "3D: reduced" setting (fixed during a frame), for the GX recorder (gx_rec.c)
bool gx_render_eye_reduced(void) {
    return s_eye_reduced;
}

// Camera-to-Fox vector, from Camera_UpdateViewMatrices (patches/main/camera.c.sed)
void port_stereo_focus(float x, float y, float z) {
    s_eye_focus = sqrtf(x * x + y * y + z * z);
}

// Once per frame: ease the zero-parallax distance toward Fox (about 0.2 s)
static void update_convergence(void) {
    if (!s_eye_autoconv || s_eye_focus < 0.0f) return;
    float d = s_eye_focus;
    if (d < 20.0f) d = 20.0f;
    if (d > 400.0f) d = 400.0f;
    s_eye_conv += (d - s_eye_conv) * 0.15f;
}

_Static_assert(offsetof(GxC3DContext, flags) == 32, "citro3d context layout");

static struct {
    bool effect_ok, viewport_ok, scissor_ok;
    u8 tex_ok;
    GxC3DEffect effect;
    u32 viewport[5];
    u32 scissor[3];
    u32 tex[3][6];
} s_emit;

void gx_c3d_invalidate(void) {
    s_emit.effect_ok = s_emit.viewport_ok = s_emit.scissor_ok = false;
    s_emit.tex_ok = 0;
}

void gx_c3d_tex_invalidate(void) { s_emit.tex_ok = 0; }

// Disable texture unit u. C3D_TexBind(u, NULL) dereferences the texture in
// citro3d 1.7.1 (a data abort on hardware; Azahar reads address 0 silently),
// so clear citro3d's binding directly: a NULL unit is left out of texConfig
void gx_c3d_tex_unbind(int u) {
    if (!__C3D_Context.tex[u]) return;
    __C3D_Context.tex[u] = NULL;
    __C3D_Context.flags |= GXC3D_F_TEX(u);
}

static void prune_state(void) {
    GxC3DContext* ctx = &__C3D_Context;
    u32 f = ctx->flags;
    if (f & GXC3D_F_PROGRAM) gx_c3d_invalidate();
    if (f & GXC3D_F_FRAMEBUF) s_emit.tex_ok = 0;
    if (f & GXC3D_F_EFFECT) {
        if (s_emit.effect_ok && !memcmp(&s_emit.effect, &ctx->effect, sizeof(ctx->effect))) {
            f &= ~GXC3D_F_EFFECT;
        } else {
            s_emit.effect = ctx->effect;
            s_emit.effect_ok = true;
        }
    }
    if (f & GXC3D_F_VIEWPORT) {
        if (s_emit.viewport_ok && !memcmp(s_emit.viewport, ctx->viewport, sizeof(ctx->viewport))) {
            f &= ~GXC3D_F_VIEWPORT;
        } else {
            memcpy(s_emit.viewport, ctx->viewport, sizeof(ctx->viewport));
            s_emit.viewport_ok = true;
        }
    }
    if (f & GXC3D_F_SCISSOR) {
        if (s_emit.scissor_ok && !memcmp(s_emit.scissor, ctx->scissor, sizeof(ctx->scissor))) {
            f &= ~GXC3D_F_SCISSOR;
        } else {
            memcpy(s_emit.scissor, ctx->scissor, sizeof(ctx->scissor));
            s_emit.scissor_ok = true;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!(f & GXC3D_F_TEX(i))) continue;
        C3D_Tex* t = ctx->tex[i];
        u32 k[6] = {0};
        if (t) {
            k[0] = t->border;
            k[1] = t->dim;
            k[2] = t->param;
            k[3] = t->lodParam;
            k[4] = (u32)t->data;
            k[5] = t->fmt | 0x80000000u;
        }
        if ((s_emit.tex_ok & (1u << i)) && !memcmp(s_emit.tex[i], k, sizeof(k))) {
            f &= ~GXC3D_F_TEX(i);
        } else {
            memcpy(s_emit.tex[i], k, sizeof(k));
            s_emit.tex_ok |= (u8)(1u << i);
        }
    }
    ctx->flags = f;
}

// S3D_GPU_CHECK_REUSE=1 (bug 17): the GPU runs a frame's draws only at
// C3D_FrameEnd, so data the CPU changes after a draw was recorded reaches the
// GPU changed. Garbage indices could make the real GPU's vertex loader read
// outside memory and hang (Azahar does not hang on that). Each draw records a
// hash of its indices, of the vertex range they reach in every buffer, and of
// its bound textures; at frame end the hashes are compared
#define REUSE_MAX_DRAWS 2048
#define REUSE_TEX_BYTES 2048
typedef struct {
    const u16* idx;
    void* ra;
    u32 count, state;
    u32 h_idx;
    const u8* vtx[3];
    u32 vtx_len[3], h_vtx[3];
    const u8* tex[3];
    u32 tex_len[3], h_tex[3];
} ReuseRec;
static ReuseRec* s_reuse;
static u32 s_reuse_n, s_reuse_seq;
static void* s_flush_ra;  // Caller of the gx_render_flush that drew the batch
static u16* s_reuse_icopy;  // Copies of each draw's first indices
#define REUSE_ICOPY 16
static int s_reuse_on = -1;

// libctru has no osConvertPhysToVirt: FCRAM (linear heap) and VRAM only
static void* reuse_phys_to_virt(u32 pa) {
    extern u32 __ctru_linear_heap;
    if (pa >= 0x18000000 && pa < 0x18600000) return (void*)(pa + 0x07000000);
    if (pa >= 0x20000000 && pa < 0x30000000)
        return (void*)(__ctru_linear_heap >= 0x30000000 ? pa + 0x10000000 : pa - 0x0C000000);
    return NULL;
}

// Exactly [p, p + len): the next draw's data often starts right after
static u32 reuse_hash(const void* p, u32 len) {
    const u8* b = p;
    u32 h = 2166136261u;
    while (len && ((uintptr_t)b & 3)) { h = (h ^ *b++) * 16777619u; len--; }
    for (; len >= 4; len -= 4, b += 4) h = (h ^ *(const u32*)b) * 16777619u;
    while (len--) h = (h ^ *b++) * 16777619u;
    return h;
}

static void reuse_record(u32 count, const void* indices) {
    if (s_reuse_on < 0) {
        s_reuse_on = cfg_bool("S3D_GPU_CHECK_REUSE", false);
        if (s_reuse_on) s_reuse = calloc(REUSE_MAX_DRAWS, sizeof(ReuseRec));
        if (s_reuse_on) s_reuse_icopy = calloc(REUSE_MAX_DRAWS * REUSE_ICOPY, sizeof(u16));
        if (s_reuse_on)
            LOG_INFO("reuse", "frame buffers: vertices %p-%p indices %p-%p dyn %p-%p", (void*)s_vbuf,
                     (void*)(s_vbuf + MAX_FRAME_VERTICES), (void*)s_ibuf, (void*)(s_ibuf + MAX_FRAME_INDICES),
                     (void*)s_dynbuf, (void*)((u8*)s_dynbuf + FRAME_DYN_BYTES));
        if (!s_reuse || !s_reuse_icopy) s_reuse_on = 0;
    }
    s_reuse_seq++;
    if (!s_reuse_on || s_reuse_n >= REUSE_MAX_DRAWS || !count) return;
    GxC3DContext* ctx = &__C3D_Context;
    ReuseRec* r = &s_reuse[s_reuse_n++];
    memset(r, 0, sizeof(*r));
    r->idx = indices;
    r->count = count;
    r->state = s_reuse_seq;
    r->h_idx = reuse_hash(indices, count * 2);
    r->ra = (r->idx >= s_ibuf && r->idx < s_ibuf + MAX_FRAME_INDICES) ? s_flush_ra : NULL;
    memcpy(&s_reuse_icopy[(s_reuse_n - 1) * REUSE_ICOPY], r->idx, (count < REUSE_ICOPY ? count : REUSE_ICOPY) * 2);
    u32 lo = 0xFFFF, hi = 0;
    for (u32 i = 0; i < count; i++) {
        u32 v = r->idx[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    for (int b = 0; b < ctx->bufInfo.bufCount && b < 3; b++) {
        const C3D_BufCfg* c = &ctx->bufInfo.buffers[b];
        u32 stride = (c->flags[1] >> 16) & 0xFF;
        void* base = reuse_phys_to_virt(ctx->bufInfo.base_paddr + c->offset);
        if (!base || !stride) continue;
        r->vtx[b] = (const u8*)base + lo * stride;
        r->vtx_len[b] = (hi - lo + 1) * stride;
        r->h_vtx[b] = reuse_hash(r->vtx[b], r->vtx_len[b]);
    }
    for (int u = 0; u < 3; u++) {
        const C3D_Tex* t = (ctx->texConfig & (1u << u)) ? ctx->tex[u] : NULL;
        if (!t || !t->data) continue;
        r->tex[u] = t->data;
        r->tex_len[u] = t->size < REUSE_TEX_BYTES ? t->size : REUSE_TEX_BYTES;
        r->h_tex[u] = reuse_hash(r->tex[u], r->tex_len[u]);
    }
}

// Before C3D_FrameEnd: log what changed since it was recorded (first 16 per frame)
static void reuse_check(void) {
    if (s_reuse_on <= 0) return;
    u32 bad = 0, bad_idx = 0, bad_vtx = 0, bad_tex = 0;
    for (u32 i = 0; i < s_reuse_n; i++) {
        ReuseRec* r = &s_reuse[i];
        bool di = reuse_hash(r->idx, r->count * 2) != r->h_idx;
        u32 dv = 0, dt = 0;
        for (int k = 0; k < 3; k++) {
            if (r->vtx[k] && reuse_hash(r->vtx[k], r->vtx_len[k]) != r->h_vtx[k]) dv |= 1u << k;
            if (r->tex[k] && reuse_hash(r->tex[k], r->tex_len[k]) != r->h_tex[k]) dt |= 1u << k;
        }
        if (!di && !dv && !dt) continue;
        bad_idx += di;
        bad_vtx += dv != 0;
        bad_tex += dt != 0;
        if (di && bad < 16) {
            const u16* old = &s_reuse_icopy[i * REUSE_ICOPY];
            u32 n = r->count < 6 ? r->count : 6;
            char ob[64] = "", nb[64] = "";
            for (u32 k = 0, o1 = 0, o2 = 0; k < n; k++) {
                o1 += snprintf(ob + o1, sizeof(ob) - o1, " %u", old[k]);
                o2 += snprintf(nb + o2, sizeof(nb) - o2, " %u", r->idx[k]);
            }
            LOG_WARN("reuse", "  indices then:%s now:%s | frame buffer flush called from %p", ob, nb, r->ra);
        }
        if (bad++ < 16)
            LOG_WARN("reuse", "f%u draw #%lu changed after it was recorded:%s%s%s idx %p n %lu vtx %p+%lu tex %p",
                     prof_frame_count(), (unsigned long)r->state, di ? " indices" : "", dv ? " vertices" : "",
                     dt ? " texture" : "", (const void*)r->idx, (unsigned long)r->count, (const void*)r->vtx[0],
                     (unsigned long)r->vtx_len[0], (const void*)r->tex[0]);
    }
    if (bad)
        LOG_WARN("reuse", "f%u: %lu of %lu draws changed (indices %lu, vertices %lu, textures %lu)",
                 prof_frame_count(), (unsigned long)bad, (unsigned long)s_reuse_n, (unsigned long)bad_idx,
                 (unsigned long)bad_vtx, (unsigned long)bad_tex);
    s_reuse_n = 0;
    s_reuse_seq = 0;
}

// Vertex shader work per frame (counters gx.vidx_*, MULTICORE_PLAN.md: the
// GPU is vertex-bound): indices drawn with each shader variant, and with the
// lit one, indices x lights evaluated. Summed here, logged at the frame end
static u32 s_vidx[GX_VSH_COUNT], s_vidx_lights;
// Leaner vertex shaders: indices x texture slots computed, by path (TC_*);
// indices x slots sampled; indices with slot 1 / slot 2 skipped
static u32 s_vidx_slot[TC_KINDS], s_vidx_used, s_vidx_skip[GX_PICA_TEX_UNITS];

// Vertex shader variant (GX_VSH_*) the current state draws with
static int state_vsh(void) {
    return s_ws_proj                  ? GX_VSH_WATER
           : s_vsh_lit || s_frag_spec ? GX_VSH_LIT
           : s_vsh_warp               ? GX_VSH_WARP
                                      : GX_VSH_UNLIT;
}

static void count_vertex_work(u32 count) {
    int v = state_vsh();
    u32 n = count * (u32)s_eyes;
    s_vidx[v] += n;
    if (v == GX_VSH_LIT && s_vsh_lit) s_vidx_lights += n * (u32)s_light_count;
    for (int s = 0; s < GX_PICA_TEX_UNITS; s++) {
        s_vidx_used += s_tc_used[s] ? n : 0;
        if (s > 0 && (s_vsh_lean & 2) && !s_tc_used[s]) s_vidx_skip[s] += n;
        else s_vidx_slot[s_tc_kind[s]] += n;
    }
}

static void report_vertex_work(void) {
    PROF_COUNT("gx.vidx_lit", s_vidx[GX_VSH_LIT]);
    PROF_COUNT("gx.vidx_unlit", s_vidx[GX_VSH_UNLIT]);
    PROF_COUNT("gx.vidx_warp", s_vidx[GX_VSH_WARP]);
    PROF_COUNT("gx.vidx_water", s_vidx[GX_VSH_WATER]);
    PROF_COUNT("gx.vidx_x_lights", s_vidx_lights);
    PROF_COUNT("gx.vidx_skip1", s_vidx_skip[1]);
    PROF_COUNT("gx.vidx_skip2", s_vidx_skip[2]);
    PROF_COUNT("gx.vidx_slots_used", s_vidx_used);
    PROF_COUNT("gx.vidx_aff_tc", s_vidx_slot[TC_AFF]);
    PROF_COUNT("gx.vidx_aff_pos", s_vidx_slot[TC_AFF_POS]);
    PROF_COUNT("gx.vidx_aff_nrm", s_vidx_slot[TC_AFF_NRM]);
    PROF_COUNT("gx.vidx_gen_proj", s_vidx_slot[TC_GEN_PROJ]);
    PROF_COUNT("gx.vidx_gen_warp", s_vidx_slot[TC_GEN_WARP]);
    PROF_COUNT("gx.vidx_gen_off", s_vidx_slot[TC_GEN_OFF]);
    memset(s_vidx, 0, sizeof(s_vidx));
    s_vidx_lights = 0;
    s_vidx_used = 0;
    memset(s_vidx_skip, 0, sizeof(s_vidx_skip));
    memset(s_vidx_slot, 0, sizeof(s_vidx_slot));
}

// C3D_DrawElements once per eye
static void draw_eyes(u32 count, const void* indices) {
    apply_vsrc();
    count_vertex_work(count);
    if (s_eyes == 1) {
        prune_state();
        gx_gpu_sync_before_draw();
        if (s_reuse_on) reuse_record(count, indices);
#if GX_PROFILE_DETAIL
        {
            // Split citro3d's state flush from the draw packet itself
            extern void C3Di_UpdateContext(void);
            u32 f = __C3D_Context.flags;
            PROF_COUNT("c3d.f_bufinfo", (f >> 3) & 1);
            PROF_COUNT("c3d.f_attr", (f >> 2) & 1);
            PROF_COUNT("c3d.f_effect", (f >> 4) & 1);
            PROF_COUNT("c3d.f_viewport", (f >> 6) & 1);
            PROF_COUNT("c3d.f_scissor", (f >> 7) & 1);
            PROF_COUNT("c3d.f_program", (f >> 8) & 1);
            PROF_COUNT("c3d.f_tex", (f & 0x3800000) != 0);
            PROF_COUNT("c3d.f_texcfg", (f >> 14) & 1);
            PROF_COUNT("c3d.f_fogetc", (f & 0x718000) != 0);
            PROF_COUNT("c3d.f_texenv", (f & 0xFC000000u) != 0);
            PROF_COUNT("c3d.f_200", (f >> 9) & 1);
            PROF_COUNT("c3d.f_400", (f >> 10) & 1);
            GX_PROF_ZONE("gx.draw.update");
            C3Di_UpdateContext();
        }
        GX_PROF_ZONE("gx.draw.submit");
#else
        if (g_c3d_fast & 1) {
            // One context update, then the template draw packet (citro3d
            // drawElements.c, MULTICORE_PLAN.md "Thin emitter"); it sets the
            // update / submit sampler phases itself
            C3D_DrawElementsFast(count, C3D_UNSIGNED_SHORT, indices);
            return;
        }
        {
            // Sampler phases only: citro3d's state flush, then the draw packet
            extern void C3Di_UpdateContext(void);
            GX_PROF_ZONE("gx.draw.update");
            C3Di_UpdateContext();
        }
        GX_PROF_ZONE("gx.draw.submit");
#endif
        C3D_DrawElements(GPU_TRIANGLES, count, C3D_UNSIGNED_SHORT, indices);
        return;
    }
    for (int e = 0; e < 2; e++) {
        int y = s_eye_y[e];
        C3D_SetViewport(0, y, 240, 400);
        if (!s_raw_draw) {
            C3D_SetScissor(GPU_SCISSOR_NORMAL, s_scissor[0], s_scissor[1] + y, s_scissor[2], s_scissor[3] + y);
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_loc_projection, &s_eye_proj[e]);
        }
        if (s_eye_rebind && e == 1) {
            for (int u = 0; u < GX_PICA_TEX_UNITS; u++) {
                if (s_unit_copy[e][u]) bind_unit(u, s_unit_copy[e][u], e);
            }
        }
        prune_state();
        gx_gpu_sync_before_draw();
        if (s_reuse_on) reuse_record(count, indices);
        C3D_DrawElementsFast(count, C3D_UNSIGNED_SHORT, indices);  // Library path unless S3D_C3D_FAST has 1
    }
    if (s_eye_rebind) {
        for (int u = 0; u < GX_PICA_TEX_UNITS; u++) {
            if (s_unit_copy[0][u]) bind_unit(u, s_unit_copy[0][u], 0);
        }
    }
}

// The game shaders give every unit (u / q, v / q) per vertex. Unit 0 used to
// sample projectively everywhere (texcoord0w, the PICA divided per fragment);
// that was turned off when self-test t3 hung on hardware (bug 12, caused by
// partial output writes). Screen-projected coordinates smear across large
// triangles that way, and break where a vertex is behind the camera
// (q <= 0): the hub water went black on one side (bug 28). Now only a water
// split draws projectively: the water shader outputs q as texcoord0w and
// unit 0 gets a copy of its texture in GPU_TEX_PROJECTION mode
static void bind_unit(int u, C3D_Tex* t, int eye) {
    if (u == 0 && s_ws_proj && t) {
        C3D_Tex* v = &s_proj_view[eye];
        if (t != v) {
            *v = *t;
            s_proj_src[eye] = t;
        }
        v->param = (v->param & ~GPU_TEXTURE_MODE(7)) | GPU_TEXTURE_MODE(GPU_TEX_PROJECTION);
        t = v;
    }
    // S3D_TEX_RES: sample every texture from a clamped mip level (1 = half
    // resolution, 2 = quarter, ...). The mip chain stays, so the game's own
    // LOD picks levels within the clamped range. Off (0) resets the level:
    // the setting is live, and a previous clamp must not stick to the
    // cached texture entries.
    if (t) t->minLevel = s_tex_res > 0 && (u32)s_tex_res <= t->maxLevel ? (u32)s_tex_res : 0;
    C3D_TexBind(u, t);
}

// Starts or ends projective sampling on unit 0 (bug 28): rebinds the texture
// it holds with the mode bits for the new setting
static void set_ws_proj(bool on) {
    if (on == s_ws_proj && !on) return;
    C3D_Tex* cur = __C3D_Context.tex[0];
    C3D_Tex* src = cur == &s_proj_view[0] || cur == &s_proj_view[1] ? s_proj_src[cur - s_proj_view] : cur;
    s_ws_proj = on;
    if (src) bind_unit(0, src, 0);
    if (on) PROF_COUNT("gx.water_proj_states", 1);
}

static void bind_decoded(int u, const GxTexObjData* obj);
static void bind_texobj(int u, const GxTexObjData* obj);
static bool upload_unit_texmtx(int s, int coord, float amp, bool pv);
static void apply_pixel_state(void);

static void set_combiners(const C3D_TexEnv* env, const u8* buf_mask) {
    gx_tev_hw_write(env, buf_mask);
}

// Pass 2 of a multipass state (multipass_prepare): the dropped texture on the
// chosen unit, drawn over pass 1's pixels (equal depth) and added to or
// multiplied with them. Fog fades the term toward black (add) or white
// (multiply); for multiply that only approximates fogging the product.
// Pass 1's state is restored afterwards, since the next batch may draw
// without a prepare. A split-off projected term (bug 32, s_mp_split) samples
// on unit 0, per fragment with the water shader when s_mp_proj; unit 0 may
// hold anything in pass 1 (or nothing), so its binding is saved as it is
static void draw_second_pass(u32 count, const void* indices) {
    int u = s_mp.unit;
    bool add = s_mp.kind == GX_MULTIPASS_ADD;
    C3D_Tex* keep_tex = NULL;
    C3D_Tex* keep_copy[2] = { NULL, NULL };
    float keep_au[3], keep_av[3];
    u8 keep_swz = 0;
    bool keep_rebind = false;
    if (s_mp_split) {
        keep_tex = __C3D_Context.tex[u];
        keep_copy[0] = s_unit_copy[0][u];
        keep_copy[1] = s_unit_copy[1][u];
        memcpy(keep_au, s_unit_au[u], sizeof(keep_au));
        memcpy(keep_av, s_unit_av[u], sizeof(keep_av));
        keep_swz = s_map.unit_swizzle[u];
        keep_rebind = s_eye_rebind;
        s_unit_copy[0][u] = s_unit_copy[1][u] = NULL;
        s_ws_proj = s_mp_proj;  // bind_unit makes the projection view
        bind_texobj(u, &g_gx.tex[s_mp.texmap]);
        if (s_mp_proj) gx_shader_select(GX_VSH_WATER);
    } else {
        bind_decoded(u, &g_gx.tex[s_mp.texmap]);
    }
    upload_unit_texmtx(u, s_mp.texcoord, 0.0f, s_cur_nrm_pv);
    set_combiners(s_mp.env, s_mp.buf_mask);
    if (s_fog_mode == FOG_MODE_FRAG && s_mp.nout < GX_PICA_TEV_STAGES) {
        C3D_TexEnv env = s_mp_main_env[s_tev_nout];
        C3D_TexEnvColor(&env, add ? 0x000000u : 0xFFFFFFu);
        gx_tev_hw_stage(s_mp.nout, &env);
    } else if (s_fog_mode == FOG_MODE_LUT) {
        C3D_FogColor(add ? 0x000000u : 0xFFFFFFu);
    }
    C3D_DepthTest(true, GPU_EQUAL, GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    if (add) {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ZERO, GPU_ONE);
    } else {
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_COLOR, GPU_ZERO, GPU_ZERO, GPU_ONE);
    }
    draw_eyes(count, indices);
    PROF_COUNT("gx.multipass_draws", 1);
    if (s_mp_split) {
        // Debug: S3D_DBG_REFL_BOOST=n adds the split-off term n times, to see it
        static int dbg_boost = -1;
        CFG_LIVE(dbg_boost, cfg_int("S3D_DBG_REFL_BOOST", 1));
        for (int i = 1; i < dbg_boost && i < 16; i++) draw_eyes(count, indices);
    }

    if (s_mp_split) {
        s_ws_proj = false;
        s_unit_copy[0][u] = keep_copy[0];
        s_unit_copy[1][u] = keep_copy[1];
        memcpy(s_unit_au[u], keep_au, sizeof(keep_au));
        memcpy(s_unit_av[u], keep_av, sizeof(keep_av));
        s_map.unit_swizzle[u] = keep_swz;
        s_eye_rebind = keep_rebind;
        if (keep_tex) C3D_TexBind(u, keep_tex);
        else gx_c3d_tex_unbind(u);
        gx_shader_select(state_vsh());
        bool used = u < s_map.unit_count;
        upload_unit_texmtx(u, used ? s_map.unit_texcoord[u] : -1, used ? s_map.unit_warp[u] : 0.0f, s_cur_nrm_pv);
    } else {
        bind_decoded(u, &g_gx.tex[s_map.unit_texmap[u]]);
        upload_unit_texmtx(u, s_map.unit_texcoord[u], s_map.unit_warp[u], s_cur_nrm_pv);
    }
    set_combiners(s_mp_main_env, s_mp_main_buf);
    apply_pixel_state();
    if (s_fog_mode == FOG_MODE_LUT) C3D_FogColor(s_fog_color);
}

// Pass 2 of a water split: refraction + konst on unit 0, added in with
// weight alpha * ka over pass 1's reflection * (1 - ka). Fog fades the
// term toward black, as pass 1 already carries the fog color
static void draw_water_pass2(u32 count, const void* indices) {
    upload_unit_texmtx(0, s_ws.refr_texcoord, s_map.unit_warp[s_ws.refr_unit], s_cur_nrm_pv);
    static const u8 buf[2] = { 0, 0 };
    set_combiners(s_ws_env[1], buf);
    if (s_fog_mode == FOG_MODE_LUT) C3D_FogColor(0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ZERO, GPU_ONE);
    draw_eyes(count, indices);
    PROF_COUNT("gx.water_split_draws", 1);

    upload_unit_texmtx(0, s_map.unit_texcoord[0], s_map.unit_warp[0], s_cur_nrm_pv);
    set_combiners(s_ws_env[0], buf);
    apply_pixel_state();
    if (s_fog_mode == FOG_MODE_LUT) C3D_FogColor(s_fog_color);
}

// S3D_GPU_TRACE: what the draw hands the GPU. Any address the GPU reads must
// be in FCRAM or VRAM; a bad one is logged as an ERROR (Azahar ignores it,
// hardware may hang on it)
static void trace_draw_state(const void* indices) {
    GxC3DContext* ctx = &__C3D_Context;
    static bool font_logged;
    if (!font_logged && fontGetSystemFont()) {
        // The touch UI's citro2d text samples these sheets directly
        void* sheet = fontGetGlyphSheetTex(NULL, 0);
        LOG_INFO("gputrace", "system font sheet 0 at %p pa %08lX", sheet, (unsigned long)osConvertVirtToPhys(sheet));
        font_logged = true;
    }
    char buf[320];
    int n = 0;
    bool bad = false;
    for (int u = 0; u < 3; u++) {
        if (!(ctx->texConfig & (1u << u))) continue;
        const C3D_Tex* t = ctx->tex[u];
        u32 pa = t ? osConvertVirtToPhys(t->data) : 0;
        if (!pa || (pa & 7)) bad = true;
        n += snprintf(buf + n, sizeof(buf) - n, "tex%d %p pa %08lX %ux%u fmt %d lv %u | ", u, t ? t->data : NULL,
                      (unsigned long)pa, t ? t->width : 0, t ? t->height : 0, t ? (int)t->fmt : -1,
                      t ? t->maxLevel : 0);
    }
    u32 cpa = osConvertVirtToPhys(ctx->fb.colorBuf), dpa = osConvertVirtToPhys(ctx->fb.depthBuf);
    u32 ipa = osConvertVirtToPhys(indices);
    if (!cpa || !ipa || (ctx->fb.depthBuf && !dpa)) bad = true;
    n += snprintf(buf + n, sizeof(buf) - n, "texcfg %08lX fb %08lX/%08lX %ux%u idx %08lX vbase %08lX light %p fog %p proctex %p",
                  (unsigned long)ctx->texConfig, (unsigned long)cpa, (unsigned long)dpa, ctx->fb.width,
                  ctx->fb.height, (unsigned long)ipa, (unsigned long)ctx->bufInfo.base_paddr, (void*)ctx->lightEnv,
                  (void*)ctx->fogLut, (void*)ctx->procTex);
    if (bad) LOG_ERROR("gputrace", "bad GPU address: %s", buf);
    else LOG_INFO("gputrace", "  %s", buf);
}

static void draw_triangles(u32 count, const void* indices) {    if (s_discard) {
        PROF_COUNT("gx.discarded_draws", 1);
        return;
    }
    if (s_dumping && (int)s_state_index - 1 >= s_dbg_skip_from && (int)s_state_index - 1 <= s_dbg_skip_to) return;
    GX_PROF_ZONE("gx.draw");
    if (s_census_on) census_draw(count);
    if (gx_gpu_trace_skip_draw()) return;
    draw_eyes(count, indices);
    if (gx_gpu_trace_on() && !gx_gpu_sync()) trace_draw_state(indices);  // After citro3d applied the state
    if (s_mp_on && !s_raw_draw) draw_second_pass(count, indices);
    if (s_ws_on && !s_raw_draw) draw_water_pass2(count, indices);
    gx_gpu_trace("draw", count, (u32)(uintptr_t)indices, s_eyes);
}

// Debug: write a decoded texture's base level to sdmc:/3ds/star3d/texN.ppm
// (rows in PICA order, so the image is upside down)
// Bug 37: level 0 of a sampled texture as the GPU stores it (PICA tiles),
// for tools/pica_tex_dump.py: "S3DT", width, height, PICA format (u16 each),
// then the data. Written into the capture folder by the ID pass
static void dump_tex_raw(const C3D_Tex* t, u32 state, int unit) {
    static const u8 bits[14] = { 32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8 };  // GPU_RGBA8 .. GPU_ETC1A4
    if (!t->data || (unsigned)t->fmt >= 14) return;
    char path[160];
    snprintf(path, sizeof(path), "%s/tex_s%lu_u%d.bin", gx_capture_dir(), (unsigned long)state, unit);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u16 hdr[3] = { (u16)t->width, (u16)t->height, (u16)t->fmt };
    fwrite("S3DT", 1, 4, f);
    fwrite(hdr, 2, 3, f);
    fwrite(t->data, 1, (size_t)t->width * t->height * bits[t->fmt] / 8, f);
    fclose(f);
}

static void dump_tex(const C3D_Tex* t, int n) {
    int bpp;
    switch (t->fmt) {
        case GPU_ETC1: case GPU_ETC1A4: bpp = 0; break;
        case GPU_RGBA8: bpp = 4; break;
        case GPU_RGB8: bpp = 3; break;
        case GPU_RGBA5551: case GPU_RGB565: case GPU_RGBA4: case GPU_LA8: bpp = 2; break;
        case GPU_L8: case GPU_A8: case GPU_LA4: bpp = 1; break;
        default:
            LOG_INFO("dump", "tex%d: format %d not dumped", n, t->fmt);
            return;
    }
    char path[64];
    snprintf(path, sizeof(path), "sdmc:/3ds/star3d/tex%d.ppm", n);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", t->width, t->height);
    const u8* d = (const u8*)t->data;
    for (u32 y = 0; y < t->height; y++) {
        for (u32 x = 0; x < t->width; x++) {
            if (bpp == 0) {
                // ETC1: 8x8 tiles of four 4x4 blocks (Z order), alpha block first in ETC1A4
                u32 bsize = t->fmt == GPU_ETC1A4 ? 16 : 8;
                u32 sub = ((x >> 2) & 1) | (((y >> 2) & 1) << 1);
                const u8* blk = d + (((y >> 3) * (t->width >> 3) + (x >> 3)) * 4 + sub) * bsize;
                u8 rgb[3];
                gx_etc1_texel(blk + (bsize - 8), x & 3, y & 3, rgb);
                if (t->fmt == GPU_ETC1A4) {
                    // Show transparency as a checker so color under alpha 0 stays visible
                    u32 i = (x & 3) * 4 + (y & 3);
                    u32 a = ((blk[i >> 1] >> ((i & 1) * 4)) & 15) * 17;
                    u8 bg = ((x >> 2) ^ (y >> 2)) & 1 ? 0x60 : 0xA0;
                    if (a == 0 && ((x ^ y) & 1)) { rgb[0] = rgb[1] = rgb[2] = bg; }
                }
                fwrite(rgb, 1, 3, f);
                continue;
            }
            u32 m = 0;
            for (int b = 0; b < 3; b++) m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
            const u8* p = d + (((y >> 3) * (t->width >> 3) + (x >> 3)) * 64 + m) * bpp;
            u32 v = bpp == 2 ? (u32)(p[0] | p[1] << 8) : 0;
            u8 rgb[3];
            switch (t->fmt) {
                case GPU_RGBA8: rgb[0] = p[3]; rgb[1] = p[2]; rgb[2] = p[1]; break;
                case GPU_RGB8: rgb[0] = p[2]; rgb[1] = p[1]; rgb[2] = p[0]; break;
                case GPU_RGB565: rgb[0] = (v >> 11) << 3; rgb[1] = ((v >> 5) & 63) << 2; rgb[2] = (v & 31) << 3; break;
                case GPU_RGBA5551: rgb[0] = (v >> 11) << 3; rgb[1] = ((v >> 6) & 31) << 3; rgb[2] = ((v >> 1) & 31) << 3; break;
                case GPU_RGBA4: rgb[0] = (v >> 12) << 4; rgb[1] = ((v >> 8) & 15) << 4; rgb[2] = ((v >> 4) & 15) << 4; break;
                case GPU_LA8: rgb[0] = rgb[1] = rgb[2] = p[1]; break;
                case GPU_LA4: rgb[0] = rgb[1] = rgb[2] = p[0] & 0xF0; break;
                default: rgb[0] = rgb[1] = rgb[2] = p[0]; break;
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    LOG_INFO("dump", "dumped tex%d: %ux%u fmt %d", n, t->width, t->height, t->fmt);
}

static void water_tess_frame_begin(void);  // gx_render.c: water tess pool reset

void gx_render_frame_begin(void) {
    PROF_COUNT("c3d.unif_vec4", g_c3d_unif_stats[0]);
    PROF_COUNT("c3d.unif_vec4_skipped", g_c3d_unif_stats[1]);
    PROF_COUNT("c3d.unif_bool_skipped", g_c3d_unif_stats[2]);
    memset(g_c3d_unif_stats, 0, sizeof(g_c3d_unif_stats));
    water_tess_frame_begin();  // Pool/slots full last frame: safe to reset now
    s_discard = false;
    // Switches the touch UI can change while the game runs
    {
        static unsigned gen;
        if (gen != cfg_gen()) {
            gen = cfg_gen();
            s_fog_on = cfg_bool("S3D_FOG", true);
            s_frag_light = cfg_bool("S3D_FRAG_LIGHT", true);
            s_vsh_lean = cfg_int("S3D_VSH_LEAN", 1);
            s_tex_res = cfg_int("S3D_TEX_RES", 0);
            s_eye_autoconv = cfg_bool("S3D_STEREO_AUTOCONV", true);
            s_eye_reduced = cfg_bool("S3D_STEREO_REDUCED", false);
            s_tex_lod_bias = cfg_int("S3D_TEX_LODBIAS_X4", 0) / 4.0f;
            s_rt_dedup = cfg_int("S3D_RT_DEDUP", 5);
            g_rt_memo = cfg_int("S3D_RT_MEMO", 15);
            s_prep_fast = cfg_int("S3D_PREP_FAST", 7);
            g_c3d_fast = cfg_int("S3D_C3D_FAST", 7);
            // S3D_RT_STEPS (A/B S3D_RT_STEPS_AB): the render-thread steps of
            // DRAWCALL_PLAN.md in one switch, -1 = the individual settings.
            // Bits 0-2 = S3D_PREP_FAST, bit 3 = S3D_C3D_FAST bit 4 (uniform value check)
            int steps = cfg_int("S3D_RT_STEPS", -1);
            if (steps >= 0) {
                s_prep_fast = steps & 7;
                g_c3d_fast = (g_c3d_fast & ~4) | ((steps & 8) ? 4 : 0);
            }
            s_dbg_mip = cfg_int("S3D_DBG_MIP", 0);
            g_gx.dirty = GX_DIRTY_ALL;
        }
    }
    s_frame_number++;
    update_convergence();
    if (s_dumping && s_dbg_hold && !s_cap_dump) {
        // Hold the dumped frame on screen so a screenshot matches the dump's indices
        LOG_INFO("dump", "=== holding dumped frame ===");
        gx_gpu_wait_idle();
        for (int i = 0; i < 10; i++) gspWaitForVBlank();
        gx_copy_dump_all();  // The dumped frame's EFB copies, now that the GPU is idle
        for (int u = 0; u < GX_PICA_TEX_UNITS; u++) {
            if (s_texdump_tex[u]) dump_tex(s_texdump_tex[u], u);
        }
        while (aptMainLoop()) gspWaitForVBlank();
    }
    s_dumping = false;
    s_cap_dump = false;
    // The trigger is checked once a second; if it contains a frame number,
    // that frame is dumped instead of the next one
    static u32 s_dump_at;
    if (s_frame_number % 60 == 0 && !s_dump_at) {
        FILE* f = fopen(DUMP_TRIGGER, "r");
        if (f) {
            unsigned long at = 0;
            if (fscanf(f, "%lu", &at) != 1 || at <= s_frame_number) at = s_frame_number;
            fclose(f);
            remove(DUMP_TRIGGER);
            s_dump_at = (u32)at;
        }
    }
    if (s_dump_at && s_frame_number == s_dump_at) {
        s_dump_at = 0;
        s_dumping = true;
        LOG_INFO("dump", "=== frame %lu batch dump ===", (unsigned long)s_frame_number);
    }
    if (gx_capture_mode() == GX_CAP_ID) {
        // The capture's ID pass logs every state into its states.txt
        s_dumping = s_cap_dump = true;
        LOG_INFO("dump", "=== frame %lu capture ID pass ===", (unsigned long)s_frame_number);
    }
    census_frame_begin();
    // This frame's set; the other one belongs to the previous frame (GPU
    // pipelining). The set used two frames ago is done: C3D_FrameEnd of the
    // previous frame waited for it
    s_vbuf = s_vbufs[s_frame_number & 1];
    s_ibuf = s_ibufs[s_frame_number & 1];
    s_dynbuf = s_dynbufs[s_frame_number & 1];
    s_vused = s_iused = 0;
    s_dynused = 0;
    s_vflushed = s_iflushed = s_dynflushed = 0;
    s_batch_vstart = s_batch_istart = 0;
    s_state_index = 0;

    // Vertex layouts (attr_info; match default.v.pica inputs)
    C3D_SetAttrInfo(attr_info(LAYOUT_STD));
    s_attr_layout = LAYOUT_STD;
    s_ctx_buf = NULL;  // Re-read: other GPU users may have bound their own buffers
    s_unif_layout = LAYOUT_STD;
    s_cur_tc_scale = 1.0f;
    s_cur_pos_scale = 1.0f;
    s_cur_nrm_pv = false;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_posscale, 1.0f, 1.0f / 16384.0f, 1.0f, 0.0f);

    g_gx.dirty = GX_DIRTY_ALL;
    g_gx.posmtx_rows = (1u << 30) - 1;
    gx_tex_frame_begin();
    gx_copy_frame_begin();
    s_drawn = false;
    gx_dlcache_frame_stats();
}

// --- Vertex layouts --------------------------------------------------------------
// Attribute pruning was tried and dropped (MULTICORE_PLAN.md, "Vertex attribute
// pruning" and "Attribute dropping instead of fixing"): PICA fixed attributes
// cost as much as loaded ones and exploded vertices on hardware, and leaving
// out loaders the shader doesn't read saved ~3% at most. Every draw loads all 7.

// Loader format of each attribute per layout. Each buffer's components must
// add up to its stride on hardware (the loader hangs the GPU otherwise, bug
// 12; Azahar just steps by the stride), so the normal loads all 4 components
// (w unused)
static const struct { u8 fmt, count; } k_va_fmt[LAYOUT_COUNT][VA_COUNT] = {
    [LAYOUT_STD] = { { GPU_FLOAT, 3 }, { GPU_UNSIGNED_BYTE, 4 }, { GPU_UNSIGNED_BYTE, 4 },
                     { GPU_FLOAT, 2 }, { GPU_FLOAT, 2 }, { GPU_FLOAT, 2 },
                     { GPU_SHORT, 4 } },  // Normal x16384
    // Position (fixed point) and normal (x64) from the GxDyn16Vertex buffer
    [LAYOUT_DYN16] = { { GPU_SHORT, 4 }, { GPU_UNSIGNED_BYTE, 4 }, { GPU_UNSIGNED_BYTE, 4 },
                       { GPU_FLOAT, 2 }, { GPU_FLOAT, 2 }, { GPU_FLOAT, 2 },
                       { GPU_BYTE, 4 } },
    // Fixed-point positions and texcoords (gx_dlcache.c pack_vertices)
    [LAYOUT_PACKED] = { { GPU_SHORT, 4 }, { GPU_UNSIGNED_BYTE, 4 }, { GPU_UNSIGNED_BYTE, 4 },
                        { GPU_SHORT, 2 }, { GPU_SHORT, 2 }, { GPU_SHORT, 2 },
                        { GPU_SHORT, 4 } },
};

static C3D_AttrInfo* attr_info(int layout) {
    static C3D_AttrInfo tab[LAYOUT_COUNT];
    static bool built[LAYOUT_COUNT];
    if (!built[layout]) {
        AttrInfo_Init(&tab[layout]);
        for (int a = 0; a < VA_COUNT; a++)
            AttrInfo_AddLoader(&tab[layout], a, (GPU_FORMATS)k_va_fmt[layout][a].fmt, k_va_fmt[layout][a].count);
        built[layout] = true;
    }
    return &tab[layout];
}

// The next draw's vertex source, bound by apply_vsrc right before the draw
// (draw_eyes), which multipass and water passes also go through
enum { VS_STD, VS_PACKED, VS_DYN, VS_DYN16 };
static struct {
    int kind;
    const void* a;  // GxOutVertex / GxPackedVertex array
    const void* b;  // GxDynVertex / GxDyn16Vertex array (VS_DYN*)
} s_vsrc;

static void set_vsrc(int kind, const void* a, const void* b) {
    s_vsrc.kind = kind;
    s_vsrc.a = a;
    s_vsrc.b = b;
}

static void bind_vbufs(const void* a, u32 stride_a, int count_a, u64 perm_a, const void* b, u32 stride_b,
                       int count_b, u64 perm_b);

static void apply_vsrc(void) {
    int layout = s_vsrc.kind == VS_PACKED ? LAYOUT_PACKED : s_vsrc.kind == VS_DYN16 ? LAYOUT_DYN16 : LAYOUT_STD;
    if (layout != s_attr_layout) {
        C3D_SetAttrInfo(attr_info(layout));
        s_attr_layout = layout;
    }
    switch (s_vsrc.kind) {
        case VS_STD:
            bind_vbufs(s_vsrc.a, sizeof(GxOutVertex), 7, 0x6543210, NULL, 0, 0, 0);
            break;
        case VS_PACKED:
            bind_vbufs(s_vsrc.a, sizeof(GxPackedVertex), 7, 0x6543210, NULL, 0, 0, 0);
            break;
        default:
            // From color: color, idx, tc0-2 (32 bytes), then padding over nrm
            // and the next position: 16 + 4 bytes (0xF, 0xC), so the
            // components fill the stride. Position and normal from the second
            // buffer
            bind_vbufs(&((const GxOutVertex*)s_vsrc.a)->color, sizeof(GxOutVertex), 7, 0xCF54321, s_vsrc.b,
                       s_vsrc.kind == VS_DYN16 ? sizeof(GxDyn16Vertex) : sizeof(GxDynVertex), 2, 0x60);
            break;
    }
}

// Point the vertex loader at this draw's buffers. Draws in a row usually
// share the buffer layout and only move the start address, so then only the
// offset registers are rewritten instead of citro3d's full buffer rebind

static void bind_vbufs(const void* a, u32 stride_a, int count_a, u64 perm_a, const void* b, u32 stride_b,
                       int count_b, u64 perm_b) {
    C3D_BufInfo want;
    BufInfo_Init(&want);
    BufInfo_Add(&want, a, stride_a, count_a, perm_a);
    if (b) BufInfo_Add(&want, b, stride_b, count_b, perm_b);

    C3D_BufInfo* cur = s_ctx_buf;
    bool same = cur && cur->bufCount == want.bufCount && cur->base_paddr == want.base_paddr;
    for (int i = 0; same && i < want.bufCount; i++)
        same = cur->buffers[i].flags[0] == want.buffers[i].flags[0] &&
               cur->buffers[i].flags[1] == want.buffers[i].flags[1];
    if (!same) {
        C3D_SetBufInfo(&want);
        if (!s_ctx_buf) s_ctx_buf = C3D_GetBufInfo();
        PROF_COUNT("gx.vbuf_rebinds", 1);
        return;
    }
    // A pending full rebind (citro3d dirty flag) rewrites these with the same values
    for (int i = 0; i < want.bufCount; i++) {
        if (cur->buffers[i].offset == want.buffers[i].offset) continue;
        cur->buffers[i].offset = want.buffers[i].offset;
        GPUCMD_AddWrite(GPUREG_ATTRIBBUFFER0_OFFSET + 3 * i, want.buffers[i].offset);
    }
}

// Water tessellation (OPEN_BUGS.md bugs 33 and 35). The ripple of the
// game's water is indirect texturing, which the PICA lacks; the stand-in moves
// the texture coordinates per vertex, so it needs vertices. Water planes are
// coarse (Krazoa Palace's round wall panes are fans of 8-48 triangles), so
// water split draws that warp are cut into N x N sub-triangles per triangle
// (barycentric grid, integer weights: a shared edge gets the same points
// from both sides, so no cracks inside a draw).
//
// Bug 35: the offsets are the GameCube's own. setupWaterCausticTev reads the
// caustic texture at TEX0 (indirect stage 0: TEX0 scrolled; stage 1: TEX0
// turned 45 degrees and scaled 0.83, scrolled) and offsets the screen
// coordinates by up to ~21 texels. The Krazoa panes have radial UVs (v from
// the rim to the centre, u bouncing with the angle: 8 mirrored wedges), which
// gives the GameCube's vortex. The unit map carries TEX0 in a spare unit
// (ripple_unit, gx_tev.c), and the offsets of each grid vertex are computed
// here and handed to the water shader in the vertex colour's red/green (its
// combiners read only the alpha). N follows the UV extent
// (S3D_WATER_TESS_UV grid steps per UV unit; the caustic has 4 waves per UV
// along v) and the object size (S3D_WATER_TESS_STEP), up to S3D_WATER_TESS,
// within WATER_TESS_DRAW_TRIS / WATER_TESS_FRAME_TRIS. Without a ripple unit
// (or S3D_WATER_RIPPLE=0) the colour carries bug 33's analytic waves over
// object (x + y, z + y) instead, with the wave length following the grid.
// One N for the whole draw: per-triangle levels would leave T-junctions
// whose offsets differ on the two sides (seams).
#define WATER_TESS_FRAME_TRIS 12288
#define WATER_TESS_DRAW_TRIS  4096
#define WATER_TESS_DRAW_VERTS 16384
// Largest offset the colour encodes, in PICA texture units
#define WATER_OFS_RANGE 0.2f
static int s_wt_max = -1, s_wt_step = -1, s_wt_wave = -1, s_wt_uv = -1, s_wt_ripple = -1, s_wt_pct = -1;
static u32 s_wt_frame, s_wt_tris;
static bool s_wt_waves;  // texwarp holds the water decode constants (restore after the draw)

// Default texwarp for the default/warp shaders: waves of length `len` across
// (x + y) and len * 34/48 across (z + y) in object units, travelling at 0.9
// and 0.7 half-periods per second
static void wave_params(float len, float out[4]) {
    double t = (double)osGetTime() * 0.001;
    out[0] = 1.0f / len;
    out[1] = 1.0f / (len * (34.0f / 48.0f));
    out[2] = (float)fmod(0.5 * 0.9 * t, 1.0) + 0.5f;
    out[3] = (float)fmod(0.5 * 0.7 * t, 1.0) + 0.5f;
}

static void upload_texwarp(float len) {
    float w[4];
    wave_params(len, w);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_texwarp, w[0], w[1], w[2], w[3]);
}

// The water shader's wave mode: the same wave_params with the phases raised
// by 2 (the marker water.v.pica tests, texwarp.z > 2). frac() ignores the +2,
// so the waves are identical; the game's wave_params stay in [0.5, 1.5) and
// the ripple decode's z is <= 0, so neither triggers the wave branch (the
// split's copy pass inherits the game's params)
static void upload_water_waves(float len) {
    float w[4];
    wave_params(len, w);
    w[2] += 2.0f;
    w[3] += 2.0f;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_texwarp, w[0], w[1], w[2], w[3]);
    s_wt_waves = true;  // water_tess_restore resets the default waves
}

// Wave mode carries the warp amplitude in texsrc[0].w (the shader multiplies
// its waves by it, the max wave being 0.25). The CPU fallback clamps the
// final offset to WATER_OFS_RANGE, so the amplitude is capped the same way:
// 0.25 * 4 * WATER_OFS_RANGE keeps the offsets in range. Only the wave path
// caps: the ripple's CPU-encoded offsets are already clamped.
static void upload_water_amp(void) {
    int coord = s_map.unit_texcoord[0];
    const GxTexGen* tg = coord >= 0 && coord < g_gx.num_texgens ? &g_gx.texgen[coord] : NULL;
    u8 src = tg ? tg->src : GX_TG_TEX0;
    bool from_tc = src != GX_TG_POS && src != GX_TG_NRM;
    float amp = 4.0f * s_map.unit_warp[0];
    static bool logged;
    if (!logged) {
        logged = true;
        LOG_WARN("water", "wave mode: unit_warp[0] %.3f, amp4 %.3f (cap %.3f)", s_map.unit_warp[0], amp,
                 4.0 * WATER_OFS_RANGE);
    }
    if (amp > 4.0f * WATER_OFS_RANGE) amp = 4.0f * WATER_OFS_RANGE;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_texsrc + 0, from_tc, src == GX_TG_POS, src == GX_TG_NRM, amp);
}

// The default waves back after a water draw (the next draw's prepare may
// keep the uniform); the water shader goes back to decoding the colour
static void water_tess_restore(void) {
    if (!s_wt_waves) return;
    upload_texwarp(48.0f);
    s_wt_waves = false;
}

// water.v.pica: offset = texsrc[0].w * (colour.rg * texwarp.xy + texwarp.zw).
// `amp4` is texsrc[0].w of the refraction pass; 0 turns the warp off
static void upload_water_decode(float amp4) {
    float k = amp4 > 0.0f ? WATER_OFS_RANGE / (127.0f * amp4) : 0.0f;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_texwarp, k, k, -128.0f * k, -128.0f * k);
    s_wt_waves = true;
}

static int water_tess_level(u32 tris, int n) {
    if (n > s_wt_max) n = s_wt_max;
    if (n < 1) n = 1;
    if (s_wt_frame != s_frame_number) {
        s_wt_frame = s_frame_number;
        s_wt_tris = 0;
    }
    while (n > 1 && (s_wt_tris + tris * (u32)(n * n) > WATER_TESS_FRAME_TRIS ||
                     tris * (u32)(n * n) > WATER_TESS_DRAW_TRIS ||
                     tris * (u32)((n + 1) * (n + 2) / 2) > WATER_TESS_DRAW_VERTS))
        n--;
    return n;
}

// A water split draw with a warp: the water shader reads its offsets from
// the vertex colour, so every such draw goes through water_tessellate
static bool water_warp_draw(void) {
    // The six config reads happen once per config generation instead of six
    // CFG_LIVE checks per call; the live water state is still checked every
    // call (it changes without a config bump)
    static unsigned gen_ = 0xFFFFFFFF;
    if (gen_ != cfg_gen()) {
        gen_ = cfg_gen();
        s_wt_max = cfg_int("S3D_WATER_TESS", 16);
        s_wt_step = cfg_int("S3D_WATER_TESS_STEP", 5);
        s_wt_wave = cfg_int("S3D_WATER_WAVE_MIN", 20);
        s_wt_uv = cfg_int("S3D_WATER_TESS_UV", 12);
        s_wt_ripple = cfg_bool("S3D_WATER_RIPPLE", true) ? 1 : 0;
        s_wt_pct = cfg_int("S3D_WATER_RIPPLE_PCT", 100);
    }
    if (!s_ws_on || !s_ws_proj) return false;
    return s_map.unit_warp[0] > 0.0f || s_map.unit_warp[s_ws.refr_unit] > 0.0f;
}

// The game's caustic texture (newshadows.c): 64 x 64 IA8, I = 127 wave + 127,
// A = 127 wave carrier + 127; kept minus the indirect bias (128)
static float s_caustic[64 * 64][2];  // [A, I]
static bool s_caustic_ready;

static void caustic_init(void) {
    for (int x = 0; x < 64; x++) {
        for (int y = 0; y < 64; y++) {
            float xp = (6.284f / 64.0f) * x, yp = (6.284f / 16.0f) * y;
            float wave = cosf(0.5f * sinf(yp) + xp), carrier = cosf(yp);
            s_caustic[y * 64 + x][0] = (float)(int)(127.0f * wave * carrier + 127.0f) - 128.0f;
            s_caustic[y * 64 + x][1] = (float)(int)(127.0f * wave + 127.0f) - 128.0f;
        }
    }
    s_caustic_ready = true;
}

// Bilinear, repeating (s, t: normalized coordinates)
// VFPv2 has no hardware floor/round, so floorf() and lroundf() are newlib
// calls (~100 cycles each). The water offset path runs them per tessellated
// vertex (thousands per frame), so it uses these inline versions instead:
// a truncating vcvt plus a fixup, exact for the bounded values here
// (|x| < 2^22).
static inline float wt_floor(float x) {
    float r = (float)(int)x;
    return r > x ? r - 1.0f : r;
}

static void caustic_sample(float s, float t, float out[2]) {
    float x = s * 64.0f - 0.5f, y = t * 64.0f - 0.5f;
    float fx = wt_floor(x), fy = wt_floor(y);
    float ax = x - fx, ay = y - fy;
    int x0 = (int)fx & 63, y0 = (int)fy & 63, x1 = (x0 + 1) & 63, y1 = (y0 + 1) & 63;
    for (int c = 0; c < 2; c++) {
        float top = s_caustic[y0 * 64 + x0][c] + (s_caustic[y0 * 64 + x1][c] - s_caustic[y0 * 64 + x0][c]) * ax;
        float bot = s_caustic[y1 * 64 + x0][c] + (s_caustic[y1 * 64 + x1][c] - s_caustic[y1 * 64 + x0][c]) * ax;
        out[c] = top + (bot - top) * ay;
    }
}

// Rows 0-1 of a texgen's matrix (TEX0 input: (s, t, 1, 1))
static void texgen_rows2(int coord, float out[2][4]) {
    memset(out, 0, sizeof(float) * 8);
    out[0][0] = out[1][1] = 1.0f;
    if (coord < 0 || coord >= g_gx.num_texgens) return;
    const GxTexGen* tg = &g_gx.texgen[coord];
    if (tg->mtx < GX_IDENTITY && tg->mtx + 2 <= GX_XF_MTX_ROWS) {
        memcpy(out[0], g_gx.xf_mtx[tg->mtx], sizeof(float) * 4);
        memcpy(out[1], g_gx.xf_mtx[tg->mtx + 1], sizeof(float) * 4);
    }
}

typedef struct {
    bool ripple;
    float m1[2][4], m2[2][4];  // Indirect stages 0 and 1: TEX0 -> caustic coordinate
    float su, sv, tu, tv;      // GameCube texel offset (s, t) -> PICA (u, v) offset
    float waves[4];            // Fallback: analytic waves (wave_params)
    float wave_len;            // Fallback-wave spatial period (for the vsh wave mode)
    float amp4;                // texsrc[0].w of the refraction pass
} WaterOfs;

static void water_ofs_setup(WaterOfs* w, float wave_len) {
    w->amp4 = 4.0f * (s_map.unit_warp[s_ws.refr_unit] > 0.0f ? s_map.unit_warp[s_ws.refr_unit] : s_map.unit_warp[0]);
    w->ripple = s_wt_ripple && s_map.ripple_unit >= 0;
    w->wave_len = wave_len;
    wave_params(wave_len, w->waves);
    if (!w->ripple) return;
    if (!s_caustic_ready) caustic_init();
    texgen_rows2(s_map.unit_texcoord[s_map.ripple_unit], w->m1);
    texgen_rows2(gx_tev_ind_coord(1), w->m2);
    // Texel offsets of the screen copy (unit 0), through its coordinate
    // mapping u = au . (s, t, q)
    const GxTexObjData* obj = &g_gx.tex[s_map.unit_texmap[0]];
    float iw = obj->width ? 1.0f / obj->width : 1.0f / 320.0f, ih = obj->height ? 1.0f / obj->height : 1.0f / 240.0f;
    float pct = (float)s_wt_pct / 100.0f;
    w->su = s_unit_au[0][0] * iw * pct;
    w->sv = s_unit_av[0][0] * iw * pct;
    w->tu = s_unit_au[0][1] * ih * pct;
    w->tv = s_unit_av[0][1] * ih * pct;
}

static inline u8 water_ofs_enc(float o) {
    // lroundf(128 + o * s) written with a truncating convert: non-tie values
    // match exactly, ties land on exact integers
    int c = (int)(128.5f + o * (127.0f / WATER_OFS_RANGE));
    return (u8)(c < 1 ? 1 : c > 255 ? 255 : c);
}

// Offsets of one vertex into color[0..1]: uv = TEX0, p = object position
static void water_ofs_vertex(const WaterOfs* w, const float uv[2], const float p[3], u8* color) {    float du, dv;
    if (w->ripple) {
        float a[2], b[2];
        caustic_sample(w->m1[0][0] * uv[0] + w->m1[0][1] * uv[1] + w->m1[0][2] + w->m1[0][3],
                       w->m1[1][0] * uv[0] + w->m1[1][1] * uv[1] + w->m1[1][2] + w->m1[1][3], a);
        caustic_sample(w->m2[0][0] * uv[0] + w->m2[0][1] * uv[1] + w->m2[0][2] + w->m2[0][3],
                       w->m2[1][0] * uv[0] + w->m2[1][1] * uv[1] + w->m2[1][2] + w->m2[1][3], b);
        // Stage 0: matrix 0.5 I, scale 2^-2; stage 1 (added): rows (0.3, 0.3),
        // (-0.3, 0.3), scale 2^-4. S reads the alpha, T the intensity
        float os = 0.125f * a[0] + 0.01875f * (b[0] + b[1]);
        float ot = 0.125f * a[1] + 0.01875f * (b[1] - b[0]);
        du = w->su * os + w->tu * ot;
        dv = w->sv * os + w->tv * ot;
    } else {
        // Bug 33's waves: 4 u (1 - |u|) of a sawtooth over (x + y, z + y)
        float r[2] = { w->waves[0] * (p[0] + p[1]) + w->waves[2], w->waves[1] * (p[2] + p[1]) + w->waves[3] };
        for (int k = 0; k < 2; k++) {
            float u = 2.0f * (r[k] - wt_floor(r[k])) - 1.0f;
            r[k] = u * (1.0f - fabsf(u));
        }
        du = w->amp4 * r[0];
        dv = w->amp4 * r[1];
    }
    color[0] = water_ofs_enc(du);
    color[1] = water_ofs_enc(dv);
}

// The split's combiner multiplies the copy texture by the vertex colour
// (GPU_PRIMARY_COLOR), so the wave mode still writes the fallback-wave
// offsets into colour[0..1]: the shader computes the UV offsets itself from
// the position, but the colour carries the brightness ripple the CPU path
// had. Without it the input mesh colours (near black on the game's water)
// multiply the copy to black.
static void water_wave_color(const WaterOfs* w, const float p[3], u8* color) {
    float r[2] = { w->waves[0] * (p[0] + p[1]) + w->waves[2], w->waves[1] * (p[2] + p[1]) + w->waves[3] };
    for (int k = 0; k < 2; k++) {
        float u = 2.0f * (r[k] - wt_floor(r[k])) - 1.0f;
        r[k] = u * (1.0f - fabsf(u));
    }
    color[0] = water_ofs_enc(w->amp4 * r[0]);
    color[1] = water_ofs_enc(w->amp4 * r[1]);
}

// Water grid interpolation: rounds sum/n half away from zero. ARM11 has no
// integer divider (a __aeabi_idiv per component is ~60 cycles and a VFP
// software division ~100, so a full vertex lerp costs ~800), so the quotient
// is formed from one reciprocal per draw instead. |sum| <= 32768*n with n
// the tessellation level, so the float product is within ~0.002 of the exact
// rational; only exact ties (r == n/2, n even) can land on the other side,
// off by 1 on an interpolated grid vertex (invisible), and the rule is a
// deterministic function of (sum, inv) so shared mesh edges keep matching
// vertices (the grid stays watertight).
static inline int wt_round(int sum, float inv) {
    return (int)((float)sum * inv + (sum < 0 ? -0.5f : 0.5f));
}

// One grid vertex: weights (wa, wb, wc) out of n on the corners a, b, c;
// inv = 1/n (computed once per draw)
static void wt_lerp_packed(GxPackedVertex* o, const GxPackedVertex* a, const GxPackedVertex* b,
                           const GxPackedVertex* c, int wa, int wb, int wc, float inv) {
    for (int k = 0; k < 3; k++) o->pos[k] = (s16)wt_round(wa * a->pos[k] + wb * b->pos[k] + wc * c->pos[k], inv);
    o->pos[3] = a->pos[3];
    for (int k = 0; k < 4; k++) o->color[k] = (u8)wt_round(wa * a->color[k] + wb * b->color[k] + wc * c->color[k], inv);
    memcpy(o->idx, a->idx, sizeof(o->idx));
    for (int u = 0; u < GX_PICA_TEX_UNITS; u++)
        for (int k = 0; k < 2; k++)
            o->tc[u][k] = (s16)wt_round(wa * a->tc[u][k] + wb * b->tc[u][k] + wc * c->tc[u][k], inv);
    for (int k = 0; k < 4; k++) o->nrm[k] = (s16)wt_round(wa * a->nrm[k] + wb * b->nrm[k] + wc * c->nrm[k], inv);
}

static void wt_lerp_std(GxOutVertex* o, const GxOutVertex* a, const GxOutVertex* b, const GxOutVertex* c, int wa,
                        int wb, int wc, float inv) {
    float fa = (float)wa * inv, fb = (float)wb * inv, fc = (float)wc * inv;
    for (int k = 0; k < 3; k++) o->pos[k] = fa * a->pos[k] + fb * b->pos[k] + fc * c->pos[k];
    for (int k = 0; k < 4; k++) o->color[k] = (u8)wt_round(wa * a->color[k] + wb * b->color[k] + wc * c->color[k], inv);
    memcpy(o->idx, a->idx, sizeof(o->idx));
    for (int u = 0; u < GX_PICA_TEX_UNITS; u++)
        for (int k = 0; k < 2; k++) o->tc[u][k] = fa * a->tc[u][k] + fb * b->tc[u][k] + fc * c->tc[u][k];
    for (int k = 0; k < 4; k++) o->nrm[k] = (s16)wt_round(wa * a->nrm[k] + wb * b->nrm[k] + wc * c->nrm[k], inv);
}

// Replaces *verts / *indices / *count with the tessellated draw (dynamic
// buffer) carrying the warp offsets when the current state is a warping
// water split. `packed`: the vertices are GxPackedVertex (positions x
// pos_scale, texcoords x tc_scale), else GxOutVertex. Returns true when the
// draw needs water_tess_restore afterwards
// The draw-cached cost split's warp bucket: water_tessellate entry + warp
// check time, reported by gx_render_drawc_report ("warp-entry" in the drawc
// line)
static u64 s_drawc_warp;
// Phase split of the active tessellation calls: scan / alloc / vert / idx
static u64 s_wt_ph[4];

// --- Water tessellation cache ------------------------------------------------------
//
// The tessellated output of a draw is a pure function of its source vertices
// (the DL cache's static buffers), the tessellation level and the water
// settings: the waves animate in the vertex shader and the caustic table is
// static, so nothing in the generated grid changes per frame. Regenerating
// it every frame was the render thread's biggest single cost (~20 ms at the
// pond: 12k grid triangles, drawc's "water" step). The cache keys on the
// owner (the DL entry - its pointer identity is invalidated when the entry
// is freed), the source buffers, the level and the config generation, and
// replays the stored grid instead.
#define WT_CACHE_MAX 64
#define WT_POOL_BYTES (1024 * 1024)

typedef struct {
    const void* owner;
    const void* verts;   // Source vertex buffer pointer (part of the key)
    const u16* indices;  // Source index buffer pointer
    u32 tris;            // Input triangle count (batched calls vary it)
    bool packed;
    int n;
    u32 gen;      // cfg_gen at insert: a live settings change invalidates all
    u32 count;    // Output index count
    u8* ov;       // Output vertices in the pool
    u16* oi;      // Output indices in the pool
} WtEntry;

static WtEntry s_wt_cache[WT_CACHE_MAX];
static u8* s_wt_pool;       // linearAlloc'd on first use
static u32 s_wt_pool_used;
static u32 s_wt_flushed;    // Cache-flush watermark (gx_render_flush_new_writes)
static bool s_wt_reset_next;  // Pool/slots full: reset at the next frame begin

// The pool is a bump allocator: a reset frees everything. It only happens at
// a frame begin (set by s_wt_reset_next), after waiting for all submitted GPU
// work: the pipelined queue can still be drawing the previous frame's
// tessellations when this frame's begin runs, so a plain reset could free
// vertices the GPU is still reading.
void gx_water_tess_invalidate(const void* owner) {
    for (int i = 0; i < WT_CACHE_MAX; i++)
        if (s_wt_cache[i].owner == owner) memset(&s_wt_cache[i], 0, sizeof(WtEntry));
}

static void water_tess_frame_begin(void) {
    if (!s_wt_reset_next || !s_wt_pool) return;
    gx_gpu_wait_idle();  // Rare: only when the pool or the slots filled up
    s_wt_reset_next = false;
    s_wt_pool_used = 0;
    s_wt_flushed = 0;
    memset(s_wt_cache, 0, sizeof(s_wt_cache));
}

static bool water_tessellate(bool packed, const void* owner, const void** verts, const u16** indices, u32* count,
                             float pos_scale, float tc_scale) {
    u64 w0 = svcGetSystemTick();
    PROF_COUNT("drawc.water_calls", 1);
    if (!water_warp_draw()) {
        s_drawc_warp += svcGetSystemTick() - w0;
        return false;
    }
    s_drawc_warp += svcGetSystemTick() - w0;
    PROF_COUNT("drawc.water_active", 1);
    // From here the water shader reads the vertex colour: no warp unless the
    // offsets get written
    upload_water_decode(0.0f);
    u32 tris = *count / 3;
    if (!tris || s_raw_draw || s_discard) return true;
    if (s_wt_max < 1) {
        // No tessellation (S3D_WATER_TESS=0): bug 33's waves still animate
        // when the ripple is off - water.v.pica's wave mode computes them at
        // the mesh's own vertices (coarse, but moving)
        if (!(s_wt_ripple && s_map.ripple_unit >= 0)) {
            upload_water_waves(s_wt_wave > 0 ? (float)s_wt_wave : 20.0f);
            upload_water_amp();
        }
        return true;
    }
    PROF_COUNT("gx.water_in_tris", tris);
    const u16* idx = *indices;
    const GxPackedVertex* pv = packed ? (const GxPackedVertex*)*verts : NULL;
    const GxOutVertex* sv = packed ? NULL : (const GxOutVertex*)*verts;
    int ru = s_wt_ripple ? s_map.ripple_unit : -1;
    u64 ph0 = svcGetSystemTick();
    float max_len2 = 0.0f, max_uv2 = 0.0f;
    for (u32 t = 0; t < tris; t++) {
        float p[3][3], q[3][2] = { { 0 } };
        for (int k = 0; k < 3; k++) {
            u16 i = idx[t * 3 + k];
            for (int c = 0; c < 3; c++) p[k][c] = packed ? pv[i].pos[c] * pos_scale : sv[i].pos[c];
            if (ru >= 0)
                for (int c = 0; c < 2; c++) q[k][c] = packed ? pv[i].tc[ru][c] * tc_scale : sv[i].tc[ru][c];
        }
        for (int k = 0; k < 3; k++) {
            const float* a = p[k];
            const float* b = p[(k + 1) % 3];
            float d2 = (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
            if (d2 > max_len2) max_len2 = d2;
            float e0 = q[k][0] - q[(k + 1) % 3][0], e1 = q[k][1] - q[(k + 1) % 3][1];
            if (e0 * e0 + e1 * e1 > max_uv2) max_uv2 = e0 * e0 + e1 * e1;
        }
    }
    u64 ph1 = svcGetSystemTick();
    float max_len = sqrtf(max_len2);
    int want = s_wt_step > 0 ? (int)ceilf(max_len / (float)s_wt_step) : 1;
    if (ru >= 0 && s_wt_uv > 0) {
        int nuv = (int)ceilf(sqrtf(max_uv2) * (float)s_wt_uv);
        if (nuv > want) want = nuv;
    }
    int n = water_tess_level(tris, want);
    u32 vper = (u32)((n + 1) * (n + 2) / 2), tper = (u32)(n * n);
    u32 vsize = packed ? sizeof(GxPackedVertex) : sizeof(GxOutVertex);
    // Fallback waves: four grid steps per wave, never shorter than S3D_WATER_WAVE_MIN
    float len = 4.0f * max_len / n, min_len = s_wt_wave > 0 ? (float)s_wt_wave : 20.0f;
    WaterOfs wo;
    water_ofs_setup(&wo, len > min_len ? len : min_len);
    // The caustic has 4 waves per UV unit along v: grid steps longer than
    // ~1.6 of them sample noise (large water within the triangle budget),
    // so the ripple fades out from a step of 0.25 to 0.4 UV
    float uv_step = sqrtf(max_uv2) / n;
    if (wo.ripple && uv_step > 0.25f) {
        float f = uv_step >= 0.4f ? 0.0f : (0.4f - uv_step) / 0.15f;
        wo.su *= f, wo.sv *= f, wo.tu *= f, wo.tv *= f;
        PROF_COUNT("gx.water_ripple_faded", 1);
    }
    // Bug 33's waves can run in the vertex shader (PICA FLR, water.v.pica's
    // wave mode); the caustic ripple cannot, so it keeps the per-vertex CPU
    // offsets in the colour
    bool vsh_waves = !wo.ripple;
    u64 ph2 = svcGetSystemTick();
    // The tessellation cache: the grid is a pure function of the source
    // buffers, the level and the water settings, so a hit replays the stored
    // grid instead of regenerating it (the render thread's biggest single
    // cost at the pond: ~20 ms a frame)
    u32 ni_out = tris * tper * 3;
    if (owner) {
        u32 gen = cfg_gen();
        for (int i = 0; i < WT_CACHE_MAX; i++) {
            WtEntry* w = &s_wt_cache[i];
            if (w->owner == owner && w->verts == *verts && w->indices == idx && w->tris == tris &&
                w->packed == packed && w->n == n && w->gen == gen) {
                s_wt_tris += w->count / 3;
                if (vsh_waves) {
                    upload_water_waves(wo.wave_len);
                    upload_water_amp();
                } else {
                    upload_water_decode(wo.amp4);
                }
                *verts = w->ov;
                *indices = w->oi;
                *count = w->count;
                PROF_COUNT("gx.water_tess_hits", 1);
                return true;
            }
        }
    }
    u8* ov = NULL;
    u16* oi = NULL;
    bool cached = false;
    if (owner) {
        u32 need = tris * vper * vsize + ni_out * 2 + 7;
        if (!s_wt_pool) {
            s_wt_pool = linearAlloc(WT_POOL_BYTES);
            if (s_wt_pool) {
                s_wt_pool_used = 0;
                memset(s_wt_cache, 0, sizeof(s_wt_cache));
            }
        }
        if (s_wt_pool && s_wt_pool_used + need <= WT_POOL_BYTES) {
            ov = (u8*)((uintptr_t)(s_wt_pool + s_wt_pool_used + 7) & ~(uintptr_t)7);
            oi = (u16*)(ov + tris * vper * vsize);
            s_wt_pool_used += need;
            cached = true;
        } else {
            s_wt_reset_next = true;  // Reset at the next frame begin, when the GPU is done
        }
    }
    if (!ov) {
        ov = gx_render_alloc_dynamic(tris * vper * vsize);
        oi = ov ? gx_render_alloc_dynamic(ni_out * 2) : NULL;
    }
    u64 ph3 = svcGetSystemTick();
    if (!ov || !oi) return true;
    u32 nv = 0, ni = 0;
    float inv = 1.0f / (float)n;
    u64 ph4 = svcGetSystemTick();
    for (u32 t = 0; t < tris; t++) {
        u16 ia = idx[t * 3], ib = idx[t * 3 + 1], ic = idx[t * 3 + 2];
        u32 base = nv;
        // Row j (weight of c), column i (weight of b): vertex base + row_start(j) + i
        for (int j = 0; j <= n; j++) {
            for (int i = 0; i + j <= n; i++) {
                float uv[2] = { 0.0f, 0.0f }, p[3];
                if (packed) {
                    GxPackedVertex* o = (GxPackedVertex*)ov + nv;
                    wt_lerp_packed(o, &pv[ia], &pv[ib], &pv[ic], n - i - j, i, j, inv);
                    for (int c = 0; c < 3; c++) p[c] = o->pos[c] * pos_scale;
                    if (vsh_waves) {
                        water_wave_color(&wo, p, o->color);
                    } else {
                        if (ru >= 0) uv[0] = o->tc[ru][0] * tc_scale, uv[1] = o->tc[ru][1] * tc_scale;
                        water_ofs_vertex(&wo, uv, p, o->color);
                    }
                } else {
                    GxOutVertex* o = (GxOutVertex*)ov + nv;
                    wt_lerp_std(o, &sv[ia], &sv[ib], &sv[ic], n - i - j, i, j, inv);
                    if (vsh_waves) {
                        water_wave_color(&wo, o->pos, o->color);
                    } else {
                        if (ru >= 0) uv[0] = o->tc[ru][0], uv[1] = o->tc[ru][1];
                        water_ofs_vertex(&wo, uv, o->pos, o->color);
                    }
                }
                nv++;
            }
        }
#define WT_V(i, j) (u16)(base + (u32)((j) * (2 * n + 3 - (j)) / 2) + (u32)(i))
        for (int j = 0; j < n; j++) {
            for (int i = 0; i + j < n; i++) {
                oi[ni++] = WT_V(i, j);
                oi[ni++] = WT_V(i + 1, j);
                oi[ni++] = WT_V(i, j + 1);
                if (i + j < n - 1) {
                    oi[ni++] = WT_V(i + 1, j);
                    oi[ni++] = WT_V(i + 1, j + 1);
                    oi[ni++] = WT_V(i, j + 1);
                }
            }
        }
#undef WT_V
    }
    s_wt_tris += ni / 3;
    u64 ph5 = svcGetSystemTick();
    // Phase split of an active tessellation call (reported in the drawc
    // window as "wtphase"): scan = edge/uv pass, alloc = dynamic ring,
    // vert = lerp + offsets, idx = index grid emission
    s_wt_ph[0] += ph1 - ph0;
    s_wt_ph[1] += ph3 - ph2;
    s_wt_ph[2] += ph5 - ph4;
    s_wt_ph[3] += ph4 - ph3;
    if (cached) {
        // Store the grid: any free slot (the pool reset clears them all)
        WtEntry* w = NULL;
        for (int i = 0; i < WT_CACHE_MAX; i++) {
            if (!s_wt_cache[i].owner) {
                w = &s_wt_cache[i];
                break;
            }
        }
        if (w) {
            w->owner = owner;
            w->verts = *verts;
            w->indices = idx;
            w->tris = tris;
            w->packed = packed;
            w->n = n;
            w->gen = cfg_gen();
            w->count = ni;
            w->ov = ov;
            w->oi = oi;
        } else {
            s_wt_reset_next = true;
        }
    }
    // Wave mode: the shader multiplies the waves it computes by texsrc[0].w
    // (the amplitude) itself; decode mode carries the offsets in the colour
    if (vsh_waves) {
        upload_water_waves(wo.wave_len);
        upload_water_amp();
    } else {
        upload_water_decode(wo.amp4);
    }
    *verts = ov;
    *indices = oi;
    *count = ni;
    PROF_COUNT("gx.water_tess_draws", 1);
    PROF_COUNT("gx.water_tess_tris", ni / 3);
    if (wo.ripple) PROF_COUNT("gx.water_ripple_draws", 1);
    if (s_dumping)
        LOG_INFO("dump", "  water tess n %d -> %lu triangles, ripple %d (unit %d), uv extent %.2f", n,
                 (unsigned long)(ni / 3), wo.ripple, ru, sqrtf(max_uv2));
    return true;
}

void gx_render_flush(void) {
    u32 icount = s_iused - s_batch_istart;
    if (icount == 0) return;
    GX_PROF_ZONE("gx.flush");
    s_flush_ra = __builtin_return_address(0);

    // CPU caches are flushed once per frame in gx_render_frame_end(): the GPU
    // only reads this data when citro3d submits the frame's command list

    use_layout(LAYOUT_STD, 1.0f, 1.0f);
    const void* verts = &s_vbuf[s_batch_vstart];
    const u16* indices = &s_ibuf[s_batch_istart];
    u32 count = icount;
    bool tess = water_tessellate(false, NULL, &verts, &indices, &count, 1.0f, 1.0f);
    set_vsrc(VS_STD, verts, NULL);
    draw_triangles(count, indices);
    if (tess) water_tess_restore();
    s_drawn = true;
    if (s_dumping) LOG_INFO("dump", "  draw %lu triangles", (unsigned long)(icount / 3));

    PROF_COUNT("gx.draw_calls", 1);
    PROF_COUNT("gx.triangles", icount / 3);
    DRAW_SRC_DRAW_BATCH(icount / 3);
    s_batch_vstart = s_vused;
    s_batch_istart = s_iused;
}

bool gx_render_dumping(void) { return s_dumping; }

u32 gx_render_frame_number(void) { return s_frame_number; }

bool gx_render_can_pipeline(void) { return s_buffers_doubled; }

// Draw pre-decoded vertices from the display list cache (already in linear
// memory and flushed from the CPU cache)
// Draw-cached cost split (S3D_DLCALL_PROFILE): per-step ticks for the cached
// draw paths, reported from gx_dlcache.c's frame window ("drawc" lines)
static int s_drawc_on;
static u64 s_drawc_t[6];  // flush, prepare, layout, water, vsrc, draw
static u32 s_drawc_calls;
// s_drawc_warp is declared above with water_tessellate: it accumulates
// water-tessellate entry + warp check time

void gx_render_drawc_prof_on(void) { s_drawc_on = 1; }

void gx_render_drawc_report(int frames) {
    if (!s_drawc_on || frames <= 0) return;
    double per = 1.0 / (SYSCLOCK_ARM11 / 1000.0) / frames;  // ticks -> ms per frame
    LOG_DEBUG("drawc", "drawc %.2f ms/frame, %u calls: flush %.2f prepare %.2f layout %.2f water %.2f vsrc %.2f "
             "draw %.2f (warp-entry %.2f)",
              (double)(s_drawc_t[0] + s_drawc_t[1] + s_drawc_t[2] + s_drawc_t[3] + s_drawc_t[4] + s_drawc_t[5]) * per,
              (unsigned)s_drawc_calls / (unsigned)frames, (double)s_drawc_t[0] * per, (double)s_drawc_t[1] * per,
              (double)s_drawc_t[2] * per, (double)s_drawc_t[3] * per, (double)s_drawc_t[4] * per,
              (double)s_drawc_t[5] * per, (double)s_drawc_warp * per);
    LOG_DEBUG("drawc", "wtphase %.2f ms/frame: scan %.2f alloc %.2f vert %.2f idx %.2f",
              (double)(s_wt_ph[0] + s_wt_ph[1] + s_wt_ph[2] + s_wt_ph[3]) * per, (double)s_wt_ph[0] * per,
              (double)s_wt_ph[1] * per, (double)s_wt_ph[2] * per, (double)s_wt_ph[3] * per);
    memset(s_drawc_t, 0, sizeof(s_drawc_t));
    memset(s_wt_ph, 0, sizeof(s_wt_ph));
    s_drawc_calls = 0;
    s_drawc_warp = 0;
}

// The four steps around a cached draw (no-op unless profiling)
#define DRAWC_BEGIN() u64 dc_t0 = svcGetSystemTick(), dc_t1, dc_t2, dc_t2b, dc_t2c, dc_t3, dc_t4
#define DRAWC_STEP(a) (a) = svcGetSystemTick()
#define DRAWC_END()                                                             \
    do {                                                                        \
        if (s_drawc_on) {                                                       \
            s_drawc_t[0] += dc_t1 - dc_t0;                                      \
            s_drawc_t[1] += dc_t2 - dc_t1;                                      \
            s_drawc_t[2] += dc_t2b - dc_t2;                                     \
            s_drawc_t[3] += dc_t2c - dc_t2b;                                    \
            s_drawc_t[4] += dc_t3 - dc_t2c;                                     \
            s_drawc_t[5] += dc_t4 - dc_t3;                                      \
            s_drawc_calls++;                                                    \
        }                                                                       \
    } while (0)

void gx_render_draw_cached(const void* owner, const GxOutVertex* verts, const u16* indices, u32 index_count) {
    DRAWC_BEGIN();
    gx_render_flush();  // Keep draw order with pending decoded geometry
    DRAWC_STEP(dc_t1);
    gx_render_prepare();
    DRAWC_STEP(dc_t2);
    use_layout(LAYOUT_STD, 1.0f, 1.0f);
    DRAWC_STEP(dc_t2b);
    const void* v = verts;
    bool tess = water_tessellate(false, owner, &v, &indices, &index_count, 1.0f, 1.0f);
    DRAWC_STEP(dc_t2c);
    set_vsrc(VS_STD, v, NULL);
    DRAWC_STEP(dc_t3);
    draw_triangles(index_count, indices);
    DRAWC_STEP(dc_t4);
    if (tess) water_tess_restore();
    s_drawn = true;
    PROF_COUNT("gx.draw_calls", 1);
    PROF_COUNT("gx.triangles", index_count / 3);
    DRAW_SRC_DRAW(index_count / 3);
    PROF_COUNT("gx.cached_triangles", index_count / 3);
    DRAWC_END();
    if (s_dumping) LOG_INFO("dump", "  cached draw %lu triangles", (unsigned long)(index_count / 3));
}

void* gx_render_alloc_dynamic(u32 bytes) {
    bytes = (bytes + 7) & ~7u;
    if (!s_dynbuf || s_dynused + bytes > FRAME_DYN_BYTES) {
        LOG_ONCE(LOG_LEVEL_WARN, "gx", "frame dynamic vertex buffer full");
        s_dyn_fails++;
        return NULL;
    }
    void* v = s_dynbuf + s_dynused;
    s_dynused += bytes;
    return v;
}

// posscale: x position scale, y normal scale (s16 x16384, or s8 x64 in the
// dyn16 buffer), z texcoord scale, w = 2 turns texgen normals by the vertex's
// position matrix (bug 27, upload_texmtx)

// Draw-cached cost split (S3D_DLCALL_PROFILE): per-step ticks for the cached
// draw paths, reported from gx_dlcache.c's frame window ("drawc" lines)
static void upload_posscale(void) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_posscale, s_cur_pos_scale,
                  s_unif_layout == LAYOUT_DYN16 ? 1.0f / 64.0f : 1.0f / 16384.0f, s_cur_tc_scale,
                  s_cur_nrm_pv ? 2.0f : 0.0f);
}

// The position/normal/texcoord scales of the next draw's vertex layout (its
// C3D_AttrInfo is bound by apply_vsrc)
static void use_layout(int layout, float pos_scale, float tc_scale) {
    bool nrm8 = layout == LAYOUT_DYN16;
    if (pos_scale != s_cur_pos_scale || tc_scale != s_cur_tc_scale || nrm8 != (s_unif_layout == LAYOUT_DYN16)) {
        s_cur_pos_scale = pos_scale;
        s_cur_tc_scale = tc_scale;
        s_unif_layout = layout;
        PROF_COUNT("drawc.layout_switches", 1);
        upload_posscale();
    }
}

// Cached list stored in the packed layout (gx_dlcache.c pack_vertices):
// fixed-point positions and texcoords, scaled back in the shader
void gx_render_draw_cached_packed(const void* owner, const GxPackedVertex* verts, const u16* indices, u32 index_count,
                                  float pos_scale, float tc_scale) {
    DRAWC_BEGIN();
    gx_render_flush();
    DRAWC_STEP(dc_t1);
    gx_render_prepare();
    DRAWC_STEP(dc_t2);
    use_layout(LAYOUT_PACKED, pos_scale, tc_scale);
    DRAWC_STEP(dc_t2b);
    const void* v = verts;
    bool tess = water_tessellate(true, owner, &v, &indices, &index_count, pos_scale, tc_scale);
    DRAWC_STEP(dc_t2c);
    set_vsrc(VS_PACKED, v, NULL);
    DRAWC_STEP(dc_t3);
    draw_triangles(index_count, indices);
    DRAWC_STEP(dc_t4);
    if (tess) water_tess_restore();
    s_drawn = true;
    PROF_COUNT("gx.draw_calls", 1);
    PROF_COUNT("gx.triangles", index_count / 3);
    DRAW_SRC_DRAW(index_count / 3);
    PROF_COUNT("gx.cached_triangles", index_count / 3);
    PROF_COUNT("gx.packed_triangles", index_count / 3);
    DRAWC_END();
    if (s_dumping) {
        LOG_INFO("dump", "  cached packed draw %lu triangles", (unsigned long)(index_count / 3));
        // Vertex colour range (bug 36: invisible C12 door disc)
        if (index_count) {
            u8 clo[4] = { 255, 255, 255, 255 }, chi[4] = { 0, 0, 0, 0 };
            for (u32 i = 0; i < index_count; i++)
                for (int c = 0; c < 4; c++) {
                    u8 v8 = verts[indices[i]].color[c];
                    if (v8 < clo[c]) clo[c] = v8;
                    if (v8 > chi[c]) chi[c] = v8;
                }
            LOG_INFO("dump", "    color r %u..%u g %u..%u b %u..%u a %u..%u", clo[0], chi[0], clo[1], chi[1], clo[2], chi[2],
                     clo[3], chi[3]);
        }
        // Texcoord range per unit (PICA texcoord precision, C12 wall streaks)
        for (int u = 0; u < GX_PICA_TEX_UNITS && index_count; u++) {
            int lo[2] = { 32767, 32767 }, hi[2] = { -32768, -32768 };
            for (u32 i = 0; i < index_count; i++) {
                const GxPackedVertex* pv = &verts[indices[i]];
                for (int c = 0; c < 2; c++) {
                    if (pv->tc[u][c] < lo[c]) lo[c] = pv->tc[u][c];
                    if (pv->tc[u][c] > hi[c]) hi[c] = pv->tc[u][c];
                }
            }
            if (lo[0] == 0 && hi[0] == 0 && lo[1] == 0 && hi[1] == 0) continue;
            LOG_INFO("dump", "    tc%d s %.3f..%.3f t %.3f..%.3f (scale 1/%.0f)", u, lo[0] * tc_scale, hi[0] * tc_scale,
                     lo[1] * tc_scale, hi[1] * tc_scale, 1.0f / tc_scale);
        }
    }
}

void gx_render_draw_cached_dynamic16(const GxOutVertex* verts, const GxDyn16Vertex* dyn, const u16* indices,
                                     u32 index_count, float pos_scale) {
    DRAWC_BEGIN();
    gx_render_flush();
    DRAWC_STEP(dc_t1);
    gx_render_prepare();
    DRAWC_STEP(dc_t2);
    use_layout(LAYOUT_DYN16, pos_scale, 1.0f);
    DRAWC_STEP(dc_t2b);
    DRAWC_STEP(dc_t2c);
    set_vsrc(VS_DYN16, verts, dyn);  // Position and normal from dyn
    DRAWC_STEP(dc_t3);
    draw_triangles(index_count, indices);
    DRAWC_STEP(dc_t4);
    s_drawn = true;
    PROF_COUNT("gx.draw_calls", 1);
    PROF_COUNT("gx.triangles", index_count / 3);
    DRAW_SRC_DRAW(index_count / 3);
    PROF_COUNT("gx.cached_dyn_triangles", index_count / 3);
    DRAWC_END();
}

// Partially cached list: static attributes from the cache, position and
// normal from this frame's dynamic buffer (two vertex buffers)
void gx_render_draw_cached_dynamic(const GxOutVertex* verts, const GxDynVertex* dyn, const u16* indices,
                                   u32 index_count) {
    DRAWC_BEGIN();
    gx_render_flush();
    DRAWC_STEP(dc_t1);
    gx_render_prepare();
    DRAWC_STEP(dc_t2);
    use_layout(LAYOUT_STD, 1.0f, 1.0f);
    DRAWC_STEP(dc_t2b);
    DRAWC_STEP(dc_t2c);
    set_vsrc(VS_DYN, verts, dyn);
    DRAWC_STEP(dc_t3);
    draw_triangles(index_count, indices);
    DRAWC_STEP(dc_t4);
    s_drawn = true;
    PROF_COUNT("gx.draw_calls", 1);
    PROF_COUNT("gx.triangles", index_count / 3);
    DRAW_SRC_DRAW(index_count / 3);
    PROF_COUNT("gx.cached_dyn_triangles", index_count / 3);
    DRAWC_END();
}

void gx_render_flush_cpu_caches(void) {
    gx_dlcache_frame_end();
    if (s_vused) GSPGPU_FlushDataCache(s_vbuf, s_vused * sizeof(GxOutVertex));
    if (s_iused) GSPGPU_FlushDataCache(s_ibuf, s_iused * sizeof(u16));
    if (s_dynused) GSPGPU_FlushDataCache(s_dynbuf, s_dynused);
}

// GPU pipelining: before a mid-frame submit (gx_c3d_queue.c split), write back
// what the CPU wrote since the last one, instead of the whole linear heap
// (~0.4 ms each on a New 3DS, 6 per hub frame). This frame's vertex, index and
// dyn data, and the dyn pool when a draw reads it directly. Everything else
// the GPU reads is flushed where it is written (display list cache entries,
// textures, movie frames); C3D_FrameEnd still flushes the whole heap once
void gx_render_flush_new_writes(void) {
    gx_dlcache_frame_end();
    if (s_vused > s_vflushed)
        GSPGPU_FlushDataCache(&s_vbuf[s_vflushed], (s_vused - s_vflushed) * sizeof(GxOutVertex));
    if (s_iused > s_iflushed) GSPGPU_FlushDataCache(&s_ibuf[s_iflushed], (s_iused - s_iflushed) * sizeof(u16));
    if (s_dynused > s_dynflushed) GSPGPU_FlushDataCache(s_dynbuf + s_dynflushed, s_dynused - s_dynflushed);
    // The water tessellation pool: the CPU writes new grids mid-frame, the
    // GPU reads them as vertex sources. Without this flush the GPU sees
    // stale RAM for the freshly generated ranges.
    if (s_wt_pool && s_wt_pool_used > s_wt_flushed) {
        GSPGPU_FlushDataCache(s_wt_pool + s_wt_flushed, s_wt_pool_used - s_wt_flushed);
        s_wt_flushed = s_wt_pool_used;
    }
    s_vflushed = s_vused;
    s_iflushed = s_iused;
    s_dynflushed = s_dynused;
}

void gx_render_frame_end(void) {
    report_vertex_work();
    gx_render_flush();
    gx_copy_frame_end();
    reuse_check();
    gx_render_flush_cpu_caches();
}

GxOutVertex* gx_render_alloc_vertices(u32 count, u32 index_count, u16** indices_out, u16* base_out) {
    if (!s_vbuf || s_vused + count > MAX_FRAME_VERTICES || s_iused + index_count > MAX_FRAME_INDICES) {
        LOG_ONCE(LOG_LEVEL_WARN, "gx", "frame vertex/index buffer full, dropping draws");
        PROF_COUNT("gx.dropped_vertices", count);
        s_drop_draws++;
        s_drop_verts += count;
        s_drop_cur++;
        return NULL;
    }
    // 16-bit indices are relative to the batch start
    if (s_vused + count - s_batch_vstart > 0xFFFF) {
        gx_render_flush();
    }
    if (s_vused == s_batch_vstart) DRAW_SRC_BATCH_BEGIN();
    GxOutVertex* v = &s_vbuf[s_vused];
    *base_out = (u16)(s_vused - s_batch_vstart);
    *indices_out = &s_ibuf[s_iused];
    s_vused += count;
    s_iused += index_count;
    return v;
}

// --- State translation -----------------------------------------------------------

static GPU_TESTFUNC depth_func(u8 gx) {
    // The PICA depth buffer stores larger values for nearer fragments (the
    // opposite of GX), so ordering comparisons are mirrored
    switch (gx) {
        case GX_NEVER:   return GPU_NEVER;
        case GX_LESS:    return GPU_GREATER;
        case GX_EQUAL:   return GPU_EQUAL;
        case GX_LEQUAL:  return GPU_GEQUAL;
        case GX_GREATER: return GPU_LESS;
        case GX_NEQUAL:  return GPU_NOTEQUAL;
        case GX_GEQUAL:  return GPU_LEQUAL;
        default:         return GPU_ALWAYS;
    }
}

static GPU_TESTFUNC alpha_func(u8 gx) {
    switch (gx) {
        case GX_NEVER:   return GPU_NEVER;
        case GX_LESS:    return GPU_LESS;
        case GX_EQUAL:   return GPU_EQUAL;
        case GX_LEQUAL:  return GPU_LEQUAL;
        case GX_GREATER: return GPU_GREATER;
        case GX_NEQUAL:  return GPU_NOTEQUAL;
        case GX_GEQUAL:  return GPU_GEQUAL;
        default:         return GPU_ALWAYS;
    }
}

static GPU_BLENDFACTOR blend_factor(u8 gx, bool is_src) {
    switch (gx) {
        case GX_BL_ZERO:        return GPU_ZERO;
        case GX_BL_ONE:         return GPU_ONE;
        // In the source slot SRCCLR means the destination color, and vice versa
        case GX_BL_SRCCLR:      return is_src ? GPU_DST_COLOR : GPU_SRC_COLOR;
        case GX_BL_INVSRCCLR:   return is_src ? GPU_ONE_MINUS_DST_COLOR : GPU_ONE_MINUS_SRC_COLOR;
        case GX_BL_SRCALPHA:    return GPU_SRC_ALPHA;
        case GX_BL_INVSRCALPHA: return GPU_ONE_MINUS_SRC_ALPHA;
        case GX_BL_DSTALPHA:    return GPU_DST_ALPHA;
        default:                return GPU_ONE_MINUS_DST_ALPHA;
    }
}

static GPU_LOGICOP logic_op(u8 gx) {
    static const GPU_LOGICOP map[16] = {
        GPU_LOGICOP_CLEAR, GPU_LOGICOP_AND, GPU_LOGICOP_AND_REVERSE, GPU_LOGICOP_COPY,
        GPU_LOGICOP_AND_INVERTED, GPU_LOGICOP_NOOP, GPU_LOGICOP_XOR, GPU_LOGICOP_OR,
        GPU_LOGICOP_NOR, GPU_LOGICOP_EQUIV, GPU_LOGICOP_INVERT, GPU_LOGICOP_OR_REVERSE,
        GPU_LOGICOP_COPY_INVERTED, GPU_LOGICOP_OR_INVERTED, GPU_LOGICOP_NAND, GPU_LOGICOP_SET,
    };
    return map[gx & 15];
}

// Fog LUTs, cached by parameters (the game switches between a few per frame).
//
// Perspective fog does not use the PICA fog unit: its LUT is indexed by z/w
// depth, and with the game's near/far (2.5 / 10000) everything past a few
// hundred units falls into the last one or two of its 128 entries, which fogs
// the whole scene. Instead fragment lighting evaluates it per pixel: one light
// at the eye with only an ambient color, whose distance attenuation LUT (256
// entries, linear in distance) holds the fog factor. The shader's view vector
// is (0, 0, eye z), so the distance is the eye depth the GX fog uses, and one
// extra combiner stage mixes in the fog color. Orthographic fog (linear depth)
// and TEV programs that use all six stages keep the fog unit.
#define FOG_CACHE_SIZE 8
typedef struct {
    u8 type;
    bool has_da;
    float start, end, nearz, farz;
    C3D_FogLut lut;
    C3D_LightLutDA da;
} FogLutEntry;
static FogLutEntry* s_fog_cache;
static u8 s_fog_count, s_fog_next;
static C3D_LightEnv s_fog_env;
static C3D_Light s_fog_light;
static const C3D_LightLutDA* s_fog_da_bound;
static bool s_fog_env_ready;
static bool s_fog_lit;   // The fog light is in use (apply_light_env binds the environment)

// GX fog curve at t = (eye z - start) / (end - start): 0 = no fog, 1 = fog color
static float gx_fog_curve(u8 type, float t) {
    if (t < 0.0f) t = 0.0f;
    float F;
    switch (type & 7) {
        case GX_FOG_PERSP_LIN:     F = t; break;
        case GX_FOG_PERSP_EXP:     F = 1.0f - exp2f(-8.0f * t); break;
        case GX_FOG_PERSP_EXP2:    F = 1.0f - exp2f(-8.0f * t * t); break;
        case GX_FOG_PERSP_REVEXP:  F = exp2f(-8.0f * (1.0f - t)); break;
        case GX_FOG_PERSP_REVEXP2: F = exp2f(-8.0f * (1.0f - t) * (1.0f - t)); break;
        default:                   F = t; break;
    }
    return F < 0.0f ? 0.0f : F > 1.0f ? 1.0f : F;
}

// GX fog factor at screen depth d (0 near .. 1 far)
static float gx_fog_factor(const GxPixelState* px, float d) {
    float n = px->fog_near, f = px->fog_far;
    float ze;
    if (px->fog_type < GX_FOG_ORTHO_LIN) {
        float den = f - d * (f - n);
        ze = den > 1e-6f ? n * f / den : f;
    } else {
        ze = n + d * (f - n);
    }
    float range = px->fog_end - px->fog_start;
    float t = range != 0.0f ? (ze - px->fog_start) / range : (ze >= px->fog_end ? 1.0f : 0.0f);
    return gx_fog_curve(px->fog_type, t);
}

static FogLutEntry* fog_entry(const GxPixelState* px) {
    if (!s_fog_cache) {
        s_fog_cache = calloc(FOG_CACHE_SIZE, sizeof(FogLutEntry));
        if (!s_fog_cache) return NULL;
    }
    for (int i = 0; i < s_fog_count; i++) {
        FogLutEntry* e = &s_fog_cache[i];
        if (e->type == px->fog_type && e->start == px->fog_start && e->end == px->fog_end &&
            e->nearz == px->fog_near && e->farz == px->fog_far) {
            return e;
        }
    }
    FogLutEntry* e = &s_fog_cache[s_fog_next];
    s_fog_next = (s_fog_next + 1) % FOG_CACHE_SIZE;
    if (s_fog_count < FOG_CACHE_SIZE) s_fog_count++;
    if (s_fog_da_bound == &e->da) s_fog_da_bound = NULL;
    e->type = px->fog_type;
    e->start = px->fog_start;
    e->end = px->fog_end;
    e->nearz = px->fog_near;
    e->farz = px->fog_far;

    // The LUT is indexed by PICA depth (1 near .. 0 far, or flipped with zFlip)
    // and holds the share of the fragment color that is kept
    float val[129];
    for (int i = 0; i <= 128; i++) {
        float pica = i / 128.0f;
        float d = s_fog_zflip ? pica : 1.0f - pica;
        val[i] = 1.0f - gx_fog_factor(px, d);
    }
    float data[256];
    for (int i = 0; i < 128; i++) {
        data[i] = val[i];
        data[128 + i] = val[i + 1] - val[i];
    }
    FogLut_FromArray(&e->lut, data);

    // Distance attenuation LUT over start..end holds the fog factor itself
    e->has_da = px->fog_type < GX_FOG_ORTHO_LIN;
    if (e->has_da) {
        float from = px->fog_start, to = px->fog_end;
        if (to <= from) to = from + 1.0f;
        float da[512];
        for (int i = 0; i < 256; i++) da[i] = gx_fog_curve(px->fog_type, i / 255.0f);
        for (int i = 0; i < 255; i++) da[256 + i] = da[i + 1] - da[i];
        da[511] = 0.0f;
        LightLut_FromArray(&e->da.lut, da);
        e->da.scale = 1.0f / (to - from);
        e->da.bias = -from * e->da.scale;
    }
    PROF_COUNT("gx.fog_luts_built", 1);
    return e;
}

static void fog_env_init(void) {
    static const C3D_Material mtl = { .ambient = { 1.0f, 1.0f, 1.0f } };
    C3D_LightEnvInit(&s_fog_env);
    C3D_LightEnvMaterial(&s_fog_env, &mtl);
    C3D_LightEnvAmbient(&s_fog_env, 0.0f, 0.0f, 0.0f);
    C3D_LightInit(&s_fog_light, &s_fog_env);
    C3D_LightAmbient(&s_fog_light, 1.0f, 1.0f, 1.0f);
    C3D_LightColor(&s_fog_light, 0.0f, 0.0f, 0.0f);
    C3D_FVec eye = FVec4_New(0.0f, 0.0f, 0.0f, 1.0f);
    C3D_LightPosition(&s_fog_light, &eye);
    C3D_LightDistAttnEnable(&s_fog_light, true);
    s_fog_env_ready = true;
}

// Runs after the TEV program: needs the pixel state and the combiner stage count
static void apply_fog(const GxPixelState* px) {
    FogLutEntry* e = NULL;
    if (s_fog_on && px->fog_type != GX_FOG_NONE) e = fog_entry(px);
    u32 color = px->fog_color.r | (px->fog_color.g << 8) | (px->fog_color.b << 16);
    s_fog_color = color;
    s_fog_mode = FOG_MODE_NONE;

    if (e && e->has_da && s_fog_frag && s_tev_nout > 0 && s_tev_nout < GX_PICA_TEV_STAGES) {
        if (!s_fog_env_ready) fog_env_init();
        if (s_fog_da_bound != &e->da) {
            C3D_LightDistAttn(&s_fog_light, &e->da);
            s_fog_da_bound = &e->da;
        }
        s_fog_lit = true;
        s_fog_mode = FOG_MODE_FRAG;
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_TexEnv env;
        C3D_TexEnvInit(&env);
        C3D_TexEnvSrc(&env, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_FRAGMENT_PRIMARY_COLOR);
        C3D_TexEnvFunc(&env, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(&env, color);
        gx_tev_hw_stage(s_tev_nout, &env);
        PROF_COUNT("gx.fog_frag", 1);
        return;
    }

    s_fog_lit = false;
    if (s_tev_nout < GX_PICA_TEV_STAGES) {
        C3D_TexEnv env;
        C3D_TexEnvInit(&env);
        gx_tev_hw_stage(s_tev_nout, &env);
    }
    if (!e) {
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        return;
    }
    s_fog_mode = FOG_MODE_LUT;
    C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, s_fog_zflip);
    C3D_FogColor(color);
    C3D_FogLutBind(&e->lut);
}


// Channel 1 specular through fragment lighting (S3D_FRAG_LIGHT). When the TEV
// program reads channel 1 color and channel 0 stays on the vertex color, the
// channel 1 lights go into the fog light environment as specular-only lights
// and their sum reaches the combiners as the secondary fragment color (see
// gx_tev_set_frag_secondary). GX attenuation maps to PICA LUTs:
//   GX_AF_SPEC: a(N.H) / k(N.H) -> spotlight LUT with an N.H input
//   GX_AF_SPOT: a(cos) -> spotlight LUT, 1 / k(distance) -> distance LUT
// and a clamped diffuse function to the D0 LUT (input L.N). Channel 1 ambient
// and alpha, vertex material colors and lights past the seventh are dropped.
#define SPEC_LIGHTS 7
#define SPEC_LUT_CACHE 8
enum { SPEC_LUT_CLAMP, SPEC_LUT_SPEC, SPEC_LUT_SPOT };
typedef struct {
    u8 mode;
    u16 gen;
    float a[3], k[3];
    C3D_LightLut lut;
} SpecLutEntry;
typedef struct {
    u16 gen;
    float k[3];
    C3D_LightLutDA da;
} SpecDaEntry;
static SpecLutEntry* s_spec_sp;
static SpecDaEntry* s_spec_da;
static u8 s_spec_sp_count, s_spec_sp_next, s_spec_da_count, s_spec_da_next;
static C3D_Light s_spec_light[SPEC_LIGHTS];
static const void* s_spec_sp_bound[SPEC_LIGHTS];
static const void* s_spec_da_bound[SPEC_LIGHTS];
static u16 s_spec_sp_gen[SPEC_LIGHTS], s_spec_da_gen[SPEC_LIGHTS];
static u16 s_spec_gen;
static C3D_LightLut s_spec_d0;
static bool s_spec_ready;

static float spec_poly(const float* c, float x) {
    return c[0] + c[1] * x + c[2] * x * x;
}

static float spec_lut_value(u8 mode, const float* a, const float* k, float x) {
    float f;
    if (mode == SPEC_LUT_CLAMP) {
        f = x > 0.0f ? x : 0.0f;
    } else {
        if (mode == SPEC_LUT_SPEC && x < 0.0f) x = 0.0f;
        f = spec_poly(a, x);
        if (f < 0.0f) f = 0.0f;
        if (mode == SPEC_LUT_SPEC) {
            float d = spec_poly(k, x);
            f = d > 1e-4f ? f / d : 0.0f;
        }
    }
    return f > 1.0f ? 1.0f : f;
}

// Signed LUT (input -1..1): entry j holds x = j / 128 for j < 128, (j - 256) / 128 above
static void spec_lut_signed(C3D_LightLut* lut, u8 mode, const float* a, const float* k) {
    float data[512];
    for (int j = 0; j < 256; j++) {
        float x = (j < 128 ? j : j - 256) / 128.0f;
        data[j] = spec_lut_value(mode, a, k, x);
    }
    for (int j = 0; j < 256; j++) {
        // The delta reaches the value one step further along the input
        float x = (j < 128 ? j : j - 256) / 128.0f + 1.0f / 128.0f;
        data[256 + j] = spec_lut_value(mode, a, k, x) - data[j];
    }
    LightLut_FromArray(lut, data);
}

static const C3D_LightLut* spec_sp_entry(u8 mode, const float* a, const float* k) {
    if (!s_spec_sp) {
        s_spec_sp = calloc(SPEC_LUT_CACHE, sizeof(SpecLutEntry));
        if (!s_spec_sp) return NULL;
    }
    bool use_k = mode == SPEC_LUT_SPEC;
    for (int i = 0; i < s_spec_sp_count; i++) {
        SpecLutEntry* e = &s_spec_sp[i];
        if (e->mode == mode && !memcmp(e->a, a, sizeof(e->a)) && (!use_k || !memcmp(e->k, k, sizeof(e->k)))) {
            return &e->lut;
        }
    }
    SpecLutEntry* e = &s_spec_sp[s_spec_sp_next];
    s_spec_sp_next = (s_spec_sp_next + 1) % SPEC_LUT_CACHE;
    if (s_spec_sp_count < SPEC_LUT_CACHE) s_spec_sp_count++;
    e->mode = mode;
    e->gen = ++s_spec_gen;
    memcpy(e->a, a, sizeof(e->a));
    memcpy(e->k, k, sizeof(e->k));
    spec_lut_signed(&e->lut, mode, a, k);
    PROF_COUNT("gx.spec_luts_built", 1);
    return &e->lut;
}

static u16 spec_sp_gen(const C3D_LightLut* lut) {
    return ((const SpecLutEntry*)((const u8*)lut - offsetof(SpecLutEntry, lut)))->gen;
}

// Distance LUT 1 / (k0 + k1 d + k2 d^2) over d = 0 .. the distance where it falls to 1/100
static const C3D_LightLutDA* spec_da_entry(const float* k) {
    if (!s_spec_da) {
        s_spec_da = calloc(SPEC_LUT_CACHE, sizeof(SpecDaEntry));
        if (!s_spec_da) return NULL;
    }
    for (int i = 0; i < s_spec_da_count; i++) {
        if (!memcmp(s_spec_da[i].k, k, sizeof(s_spec_da[i].k))) return &s_spec_da[i].da;
    }
    SpecDaEntry* e = &s_spec_da[s_spec_da_next];
    s_spec_da_next = (s_spec_da_next + 1) % SPEC_LUT_CACHE;
    if (s_spec_da_count < SPEC_LUT_CACHE) s_spec_da_count++;
    e->gen = ++s_spec_gen;
    memcpy(e->k, k, sizeof(e->k));

    float dmax = 1e5f;
    if (k[2] > 0.0f) {
        float disc = k[1] * k[1] - 4.0f * k[2] * (k[0] - 100.0f);
        if (disc > 0.0f) dmax = (-k[1] + sqrtf(disc)) / (2.0f * k[2]);
    } else if (k[1] > 0.0f) {
        dmax = (100.0f - k[0]) / k[1];
    }
    if (!(dmax > 1.0f)) dmax = 1.0f;
    if (dmax > 1e5f) dmax = 1e5f;
    float data[512];
    for (int j = 0; j < 256; j++) {
        float den = spec_poly(k, j / 256.0f * dmax);
        float f = den > 1e-4f ? 1.0f / den : 1.0f;
        data[j] = f > 1.0f ? 1.0f : f;
    }
    for (int j = 0; j < 255; j++) data[256 + j] = data[j + 1] - data[j];
    data[511] = 0.0f;
    LightLut_FromArray(&e->da.lut, data);
    e->da.scale = 1.0f / dmax;
    e->da.bias = 0.0f;
    PROF_COUNT("gx.spec_luts_built", 1);
    return &e->da;
}

static u16 spec_da_gen(const C3D_LightLutDA* da) {
    return ((const SpecDaEntry*)((const u8*)da - offsetof(SpecDaEntry, da)))->gen;
}

static void spec_env_init(void) {
    if (!s_fog_env_ready) fog_env_init();
    for (int i = 0; i < SPEC_LIGHTS; i++) {
        C3D_LightInit(&s_spec_light[i], &s_fog_env);
        C3D_LightAmbient(&s_spec_light[i], 0.0f, 0.0f, 0.0f);
        C3D_LightDiffuse(&s_spec_light[i], 0.0f, 0.0f, 0.0f);
        C3D_LightSpecular1(&s_spec_light[i], 0.0f, 0.0f, 0.0f);
        C3D_LightEnable(&s_spec_light[i], false);
    }
    static const float none[3];
    spec_lut_signed(&s_spec_d0, SPEC_LUT_CLAMP, none, none);
    C3D_LightEnvClampHighlights(&s_fog_env, true);
    s_spec_ready = true;
}

static float color_f(u8 c) {
    return c / 255.0f;
}

static float clamp1(float v) {
    return v > 1.0f ? 1.0f : v;
}

// Lights of channel 1 -> the specular lights (runs when the TEV program or the lights change)
static void setup_spec_lights(void) {
    const GxChannel* ch = &g_gx.chan[1];
    GXColor m = ch->mat_src == GX_SRC_REG ? g_gx.chan_mat[1] : (GXColor){ 255, 255, 255, 255 };
    C3D_Material mtl = {
        .ambient = { 1.0f, 1.0f, 1.0f },
        .specular0 = { color_f(m.r), color_f(m.g), color_f(m.b) },
    };
    C3D_LightEnvMaterial(&s_fog_env, &mtl);
    C3D_LightEnvLut(&s_fog_env, GPU_LUT_D0, GPU_LUTINPUT_LN, true,
                    ch->diff_fn != GX_DF_NONE ? &s_spec_d0 : NULL);
    bool spec = ch->attn_fn == GX_AF_SPEC;
    // The spotlight LUT input is shared by the environment; the attenuation
    // function belongs to the channel, so every light agrees on it
    C3D_LightEnvLut(&s_fog_env, GPU_LUT_SP, spec ? GPU_LUTINPUT_NH : GPU_LUTINPUT_SP, true, NULL);

    int n = 0;
    for (int i = 0; i < GX_NUM_LIGHTS && n < SPEC_LIGHTS; i++) {
        if (!(ch->light_mask & (1 << i))) continue;
        const GxLight* l = &g_gx.lights[i];
        C3D_Light* pl = &s_spec_light[n];
        float scale = 1.0f;
        const C3D_LightLut* sp = NULL;
        const C3D_LightLutDA* da = NULL;
        if (spec) {
            float len = sqrtf(l->pos[0] * l->pos[0] + l->pos[1] * l->pos[1] + l->pos[2] * l->pos[2]);
            float inv = len > 1e-6f ? 1.0f / len : 0.0f;
            C3D_FVec dir = FVec4_New(l->pos[0] * inv, l->pos[1] * inv, l->pos[2] * inv, 0.0f);
            C3D_LightPosition(pl, &dir);
            sp = spec_sp_entry(SPEC_LUT_SPEC, l->a, l->k);
        } else {
            C3D_FVec pos = FVec4_New(l->pos[0], l->pos[1], l->pos[2], 1.0f);
            C3D_LightPosition(pl, &pos);
            if (ch->attn_fn == GX_AF_SPOT) {
                if (l->a[0] != 1.0f || l->a[1] != 0.0f || l->a[2] != 0.0f) {
                    sp = spec_sp_entry(SPEC_LUT_SPOT, l->a, l->k);
                    C3D_LightSpotDir(pl, l->dir[0], l->dir[1], l->dir[2]);
                }
                if (l->k[1] != 0.0f || l->k[2] != 0.0f) {
                    da = spec_da_entry(l->k);
                } else if (l->k[0] > 0.0f) {
                    scale = 1.0f / l->k[0];
                }
            }
        }
        C3D_LightSpecular0(pl, clamp1(color_f(l->color.r) * scale), clamp1(color_f(l->color.g) * scale),
                           clamp1(color_f(l->color.b) * scale));

        C3D_LightSpotEnable(pl, sp != NULL);
        if (sp && (s_spec_sp_bound[n] != sp || s_spec_sp_gen[n] != spec_sp_gen(sp))) {
            C3D_LightSpotLut(pl, (C3D_LightLut*)sp);
            s_spec_sp_bound[n] = sp;
            s_spec_sp_gen[n] = spec_sp_gen(sp);
        }
        C3D_LightDistAttnEnable(pl, da != NULL);
        if (da && (s_spec_da_bound[n] != da || s_spec_da_gen[n] != spec_da_gen(da))) {
            C3D_LightDistAttn(pl, (C3D_LightLutDA*)da);
            s_spec_da_bound[n] = da;
            s_spec_da_gen[n] = spec_da_gen(da);
        }
        C3D_LightEnable(pl, true);
        n++;
    }
    for (; n < SPEC_LIGHTS; n++) C3D_LightEnable(&s_spec_light[n], false);
}

static void disable_spec_lights(void) {
    static const C3D_Material mtl = { .ambient = { 1.0f, 1.0f, 1.0f } };
    C3D_LightEnvMaterial(&s_fog_env, &mtl);
    C3D_LightEnvLut(&s_fog_env, GPU_LUT_D0, GPU_LUTINPUT_LN, true, NULL);
    for (int n = 0; n < SPEC_LIGHTS; n++) C3D_LightEnable(&s_spec_light[n], false);
}

// Binds the fog / specular light environment after apply_fog decided on the fog light
static void apply_light_env(bool lights_changed) {
    static bool s_spec_on;
    if (s_frag_spec) {
        if (!s_spec_ready) spec_env_init();
        if (lights_changed || !s_spec_on) setup_spec_lights();
        s_spec_on = true;
    } else if (s_spec_on) {
        disable_spec_lights();
        s_spec_on = false;
    }
    if (s_fog_env_ready) C3D_LightEnable(&s_fog_light, s_fog_lit);
    C3D_LightEnvBind(s_fog_lit || s_frag_spec ? &s_fog_env : NULL);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_fraglit, s_frag_spec);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_nrmout, false);
}

// Render capture ID / light / normals passes: the last combiner stage outputs
// the pass's color instead (alpha is kept so alpha-tested cutouts stay), opaque
static void capture_override(void) {
    int mode = gx_capture_mode();
    int k = s_tev_nout < GX_PICA_TEV_STAGES ? s_tev_nout : GX_PICA_TEV_STAGES - 1;
    C3D_TexEnv* env = C3D_GetTexEnv(k);
    if (k == s_tev_nout) C3D_TexEnvInit(env);
    GPU_TEVSRC src = GPU_PRIMARY_COLOR;
    if (mode == GX_CAP_ID) {
        u32 i = s_state_index;  // Same encoding as S3D_DBG_IDCOLOR
        u32 color = ((i & 15) * 17) | ((((i >> 4) & 15) * 17) << 8) | ((((i >> 8) & 3) * 85) << 16);
        C3D_TexEnvColor(env, color | 0xFF000000u);
        src = GPU_CONSTANT;
    }
    C3D_TexEnvSrc(env, C3D_RGB, src, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_1);
    for (int st = k + 1; st < GX_PICA_TEV_STAGES; st++) C3D_TexEnvInit(C3D_GetTexEnv(st));
    gx_tev_hw_invalidate();
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    if (mode == GX_CAP_NORMALS) {
        // The vertex shader writes the view-space normal as the vertex color
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_nrmout, true);
        gx_shader_select(GX_VSH_LIT);
    }
    C3D_LightEnvBind(NULL);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_ColorLogicOp(GPU_LOGICOP_COPY);
}
// GX viewport near/far -> PICA depth (PICA depth = 1 - GX depth). The game
// sets a slightly negative near value for decals, transparent and effect
// passes (camera.c), which pulls them toward the viewer; only geometry right
// at the near plane leaves the 0..1 range.
static void apply_depth_map(void) {
    float vn = g_gx.viewport[4], vf = g_gx.viewport[5];
    if (!s_depth_bias || vf <= vn) {
        C3D_DepthMap(true, -1.0f, 0.0f);
        return;
    }
    // z_ndc -1 (near) .. 0 (far): GX depth = vf + (vf - vn) * z_ndc
    C3D_DepthMap(true, -(vf - vn), 1.0f - vf);
}

static void apply_pixel_state(void) {
    const GxPixelState* px = &g_gx.pixel;

    GPU_WRITEMASK mask = 0;
    if (px->color_update) mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
    if (px->alpha_update) mask |= GPU_WRITE_ALPHA;
    if (px->z_enable && px->z_update) mask |= GPU_WRITE_DEPTH;
    C3D_DepthTest(true, px->z_enable ? depth_func(px->z_func) : GPU_ALWAYS, mask);

    switch (px->blend_mode) {
        case GX_BM_BLEND: {
            GPU_BLENDFACTOR s = blend_factor(px->blend_src, true);
            GPU_BLENDFACTOR d = blend_factor(px->blend_dst, false);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, s, d, s, d);
            break;
        }
        case GX_BM_SUBTRACT:
            C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT,
                           GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
            break;
        case GX_BM_LOGIC:
            C3D_ColorLogicOp(logic_op(px->logic_op));
            break;
        default:
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            break;
    }

    // The PICA has a single alpha comparison; pick the one that matters
    u8 comp = px->alpha_comp0, ref = px->alpha_ref0;
    bool enable = true;
    if (px->alpha_op == GX_AOP_AND) {
        if (px->alpha_comp0 == GX_ALWAYS) { comp = px->alpha_comp1; ref = px->alpha_ref1; }
    } else if (px->alpha_op == GX_AOP_OR) {
        if (px->alpha_comp0 == GX_ALWAYS || px->alpha_comp1 == GX_ALWAYS) enable = false;
        else if (px->alpha_comp0 == GX_NEVER) { comp = px->alpha_comp1; ref = px->alpha_ref1; }
    }
    if (comp == GX_ALWAYS) enable = false;
    C3D_AlphaTest(enable, alpha_func(comp), ref);

    int cull = cull_setting();
    GPU_CULLMODE front = cull == 2 ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW;
    GPU_CULLMODE back = cull == 2 ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW;
    if (cull == 0 || px->cull_mode == GX_CULL_NONE || px->cull_mode == GX_CULL_ALL) C3D_CullFace(GPU_CULL_NONE);
    else C3D_CullFace(px->cull_mode == GX_CULL_FRONT ? front : back);
}

// Projection matrix from the screen x/y rows V and the GX z/w rows of P,
// rotated by the screen tilt
static void tilt_projection(const float V[2][4], const float P[4][4], C3D_Mtx* m) {
    for (int c = 0; c < 4; c++) {
        float x = s_tilt[0][0] * V[0][c] + s_tilt[0][1] * V[1][c];
        float y = s_tilt[1][0] * V[0][c] + s_tilt[1][1] * V[1][c];
        float* r0 = c == 0 ? &m->r[0].x : c == 1 ? &m->r[0].y : c == 2 ? &m->r[0].z : &m->r[0].w;
        float* r1 = c == 0 ? &m->r[1].x : c == 1 ? &m->r[1].y : c == 2 ? &m->r[1].z : &m->r[1].w;
        float* r2 = c == 0 ? &m->r[2].x : c == 1 ? &m->r[2].y : c == 2 ? &m->r[2].z : &m->r[2].w;
        float* r3 = c == 0 ? &m->r[3].x : c == 1 ? &m->r[3].y : c == 2 ? &m->r[3].z : &m->r[3].w;
        *r0 = x;
        *r1 = y;
        *r2 = P[2][c];
        *r3 = P[3][c];
    }
}

// Projection uniform: tilt * viewport * GX projection
static void upload_projection(void) {
    const float* p = g_gx.proj;
    float P[4][4] = { { 0 } };
    if (g_gx.proj_type == GX_PERSPECTIVE) {
        P[0][0] = p[0]; P[0][2] = p[1];
        P[1][1] = p[2]; P[1][2] = p[3];
        P[2][2] = p[4]; P[2][3] = p[5];
        P[3][2] = -1.0f;
    } else {
        P[0][0] = p[0]; P[0][3] = p[1];
        P[1][1] = p[2]; P[1][3] = p[3];
        P[2][2] = p[4]; P[2][3] = p[5];
        P[3][3] = 1.0f;
    }

    // GX viewport (EFB pixels, y down) -> clip space of the g_scr_w x 240
    // screen (EFB x maps to screen x = g_efb_x0 + x * g_efb_xs, y likewise)
    const float* vp = g_gx.viewport;
    float hw = 0.5f * g_scr_w;
    float sx = g_efb_xs * vp[2] / g_scr_w;
    float ox = (g_efb_x0 + g_efb_xs * (vp[0] + 0.5f * vp[2])) / hw - 1.0f;
    float sy = g_efb_ys * vp[3] / 240.0f;
    float oy = 1.0f - (g_efb_y0 + g_efb_ys * (vp[1] + 0.5f * vp[3])) / 120.0f;

    float V[2][4];
    for (int c = 0; c < 4; c++) {
        V[0][c] = sx * P[0][c] + ox * P[3][c];
        V[1][c] = sy * P[1][c] + oy * P[3][c];
    }

    C3D_Mtx m;
    tilt_projection(V, P, &m);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_loc_projection, &m);

    // Stereo: x_clip += eye * sep * (w - convergence) for perspective
    // projections. Orthographic passes (HUD, 2D) stay at screen depth.
    s_eye_proj[0] = s_eye_proj[1] = m;
    if (s_eyes == 2 && g_gx.proj_type == GX_PERSPECTIVE) {
        for (int e = 0; e < 2; e++) {
            float k = e ? s_eye_sep : -s_eye_sep;
            float W[2][4];
            memcpy(W, V, sizeof(W));
            for (int c = 0; c < 4; c++) W[0][c] += k * P[3][c];
            W[0][3] -= k * s_eye_conv;
            tilt_projection(W, P, &s_eye_proj[e]);
        }
    } else if (s_eyes == 2 && s_eye_hud != 0.0f) {
        // Optional constant parallax: the 2D layer floats in front of the screen
        for (int e = 0; e < 2; e++) {
            float k = e ? s_eye_sep : -s_eye_sep;
            float W[2][4];
            memcpy(W, V, sizeof(W));
            W[0][3] -= k * s_eye_hud;
            tilt_projection(W, P, &s_eye_proj[e]);
        }
    }
}

// GX scissor (EFB pixels) -> PICA scissor. The EFB maps to the screen
// (g_efb_x0/xs/y0/ys), and the framebuffer is the screen rotated by s_tilt, so the
// rectangle goes EFB -> screen clip space -> framebuffer clip space -> pixels.
static void apply_scissor(void) {
    const u32* sc = g_gx.scissor;
    float hw = 0.5f * g_scr_w;
    float cx[2] = { (g_efb_x0 + sc[0] * g_efb_xs) / hw - 1.0f, (g_efb_x0 + (sc[0] + sc[2]) * g_efb_xs) / hw - 1.0f };
    float cy[2] = { 1.0f - (g_efb_y0 + sc[1] * g_efb_ys) / 120.0f, 1.0f - (g_efb_y0 + (sc[1] + sc[3]) * g_efb_ys) / 120.0f };
    float fx0 = 1e9f, fx1 = -1e9f, fy0 = 1e9f, fy1 = -1e9f;
    for (int i = 0; i < 4; i++) {
        float x = cx[i & 1], y = cy[i >> 1];
        float tx = s_tilt[0][0] * x + s_tilt[0][1] * y;
        float ty = s_tilt[1][0] * x + s_tilt[1][1] * y;
        if (tx < fx0) fx0 = tx;
        if (tx > fx1) fx1 = tx;
        if (ty < fy0) fy0 = ty;
        if (ty > fy1) fy1 = ty;
    }
    float pw = 0.5f * g_fb_w, ph = 0.5f * g_fb_h;
    int l = (int)((fx0 + 1.0f) * pw + 0.5f), r = (int)((fx1 + 1.0f) * pw + 0.5f);
    int t = (int)((fy0 + 1.0f) * ph + 0.5f), b = (int)((fy1 + 1.0f) * ph + 0.5f);
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > g_fb_w) r = g_fb_w;
    if (b > g_fb_h) b = g_fb_h;
    if (r <= l || b <= t) {
        l = t = 0;
        r = b = 1;  // Empty rectangle: keep a 1-pixel box rather than disabling the test
    }
    s_scissor[0] = l; s_scissor[1] = t; s_scissor[2] = r; s_scissor[3] = b;
    C3D_SetScissor(GPU_SCISSOR_NORMAL, l, t, r, b);
}

// Upload only the position matrix rows that changed
static void upload_posmtx(void) {
    u32 rows = g_gx.posmtx_rows;
    while (rows) {
        int r = __builtin_ctz(rows);
        rows &= rows - 1;
        const float* row = g_gx.xf_mtx[r];
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_posmtx + r, row[0], row[1], row[2], row[3]);
    }
    g_gx.posmtx_rows = 0;
}

// Texture matrices: the GX matrix rows (s, t, q), combined with the unit's
// mapping to PICA (u, v) (see bind_textures)
// Vertex shader variant inputs: a lit channel feeds the TEV (upload_lighting),
// some texture slot warps (upload_texmtx)

// Per-vertex texture matrices for normal texgens (bug 27). Skinned models
// (objprint_dolphin.c modelRenderFn_setVtxDescr) send TEXnMTXIDX with every
// vertex, and renderOpMatrix loads joint i's normal matrix into TEXMTX(i) next
// to its position matrix in PNMTX(i): the 3x3 of the position matrix times
// 1 / rootMotionScale (the same uniform scale for all joints). The sphere-map
// lighting (Fox's fur and body, other characters) looks up the vertex's own
// joint matrix. The shader only has the position matrices per vertex, so with
// posscale.w = 2 it turns normals by posmtx[a0.x], and the slot's matrix
// becomes that scale k (the 3x3s must be proportional; checked here on the
// texgen's own matrix). Before this, every vertex used TEXMTX0 (joint 0 of
// the batch): normals turned the wrong way, so the head sampled the edge of the
// lit sphere (ambient only), dark at night. Returns 0 = not a per-vertex normal
// texgen, 1 = per-vertex with scale *k, -1 = per-vertex but not expressible
static int nrm_pervertex(const GxTexGen* tg, int coord, float* k) {
    if (!tg || tg->src != GX_TG_NRM || coord >= 8 || g_gx.vtx_desc[GX_VA_TEX0MTXIDX + coord] == GX_NONE ||
        g_gx.vtx_desc[GX_VA_PNMTXIDX] == GX_NONE)
        return 0;
    int t = tg->mtx, p = tg->mtx - GX_TEXMTX0;
    if (t >= GX_IDENTITY || t + 3 > GX_XF_MTX_ROWS || p < 0 || p % 3 || p + 3 > 30) return -1;
    float tp = 0.0f, pp = 0.0f, tt = 0.0f;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            float a = g_gx.xf_mtx[t + r][c], b = g_gx.xf_mtx[p + r][c];
            tp += a * b; pp += b * b; tt += a * a;
        }
    if (pp <= 0.0f) return -1;
    *k = tp / pp;
    // Residual of tex - k * pos relative to tex: proportional up to rounding
    float res = tt - *k * tp;
    if (res > 1e-4f * tt) {
        static int logged;
        if (logged < 12) {
            logged++;
            const float(*a)[4] = &g_gx.xf_mtx[t];
            const float(*b)[4] = &g_gx.xf_mtx[p];
            const float(*n)[3] = &g_gx.xf_nrm[p];
            LOG_INFO("gx", "per-vertex texmtx %d: not a scaled PNMTX%d (k %.4f, residual %.3g of %.3g) | tex %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f"
                     " | pos %.3f %.3f %.3f / %.3f %.3f %.3f | nrm %.3f %.3f %.3f",
                     t, p / 3, *k, res, tt, a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2],
                     b[0][0], b[0][1], b[0][2], b[1][0], b[1][1], b[1][2], n[0][0], n[0][1], n[0][2]);
        }
        PROF_COUNT("gx.texmtx_pervertex_notscaled", 1);
        return -1;
    }
    return 1;
}

// One unit: texture coordinate `coord` (-1 = none) with warp amplitude `amp`.
// `pv`: normal texgens use the vertex's own position matrix (nrm_pervertex,
// k = its scale). Returns whether the unit warps
static bool upload_unit_texmtx(int s, int coord, float amp, bool pv) {
    static const float ident[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 0, 1 } };
    int base = s_loc_texmtx + s * 3;
    const GxTexGen* tg = coord >= 0 ? &g_gx.texgen[coord] : NULL;

    const float* rows[3] = { ident[0], ident[1], ident[2] };
    float kmtx[3][4];
    float k;
    if (pv && nrm_pervertex(tg, coord, &k) == 1) {
        memset(kmtx, 0, sizeof(kmtx));
        kmtx[0][0] = kmtx[1][1] = k;
        if (tg->func == GX_TG_MTX3x4) kmtx[2][2] = k;
        else kmtx[2][3] = 1.0f;
        rows[0] = kmtx[0]; rows[1] = kmtx[1]; rows[2] = kmtx[2];
    } else if (tg && tg->mtx < GX_IDENTITY && tg->mtx + 3 <= GX_XF_MTX_ROWS) {
        rows[0] = g_gx.xf_mtx[tg->mtx];
        rows[1] = g_gx.xf_mtx[tg->mtx + 1];
        // 2x4 matrices leave q = 1
        if (tg->func == GX_TG_MTX3x4) rows[2] = g_gx.xf_mtx[tg->mtx + 2];
    }
    // Dual texture transform: post * (rows * input, 1). The normalize
    // step between the two is skipped, so they fold into one matrix.
    float post[3][4];
    if (tg && tg->post != GX_PTIDENTITY - GX_PTTEXMTX0) {
        if (tg->normalize) PROF_COUNT("gx.texgen_normalize_skipped", 1);
        const float (*p)[4] = &g_gx.xf_pt[tg->post];
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                post[r][c] = p[r][0] * rows[0][c] + p[r][1] * rows[1][c] + p[r][2] * rows[2][c] +
                             (c == 3 ? p[r][3] : 0.0f);
            }
        }
        rows[0] = post[0]; rows[1] = post[1]; rows[2] = post[2];
    }
    const float* au = s_unit_au[s];
    const float* av = s_unit_av[s];
    float u[4], v[4];
    for (int c = 0; c < 4; c++) {
        u[c] = au[0] * rows[0][c] + au[1] * rows[1][c] + au[2] * rows[2][c];
        v[c] = av[0] * rows[0][c] + av[1] * rows[1][c] + av[2] * rows[2][c];
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, base + 0, u[0], u[1], u[2], u[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, base + 1, v[0], v[1], v[2], v[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, base + 2, rows[2][0], rows[2][1], rows[2][2], rows[2][3]);

    // Texgen input: (s, t, 1, 1) from the vertex, or position / normal
    u8 src = tg ? tg->src : GX_TG_TEX0;
    bool from_tc = src != GX_TG_POS && src != GX_TG_NRM;
    bool skipped = s > 0 && coord < 0 && (s_vsh_lean & 2);
    if (skipped) {
        // Unused slot 1 or 2: y = 2 makes the shader skip it (no real slot has
        // y > 1). A marker instead of a bool: the bools are full up to b14, and
        // b15 reads as set on real hardware
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_texsrc + s, 0.0f, 2.0f, 0.0f, 0.0f);
    } else {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_texsrc + s, from_tc, src == GX_TG_POS, src == GX_TG_NRM, 4.0f * amp);
    }

    // Affine fast path: q is exactly 1 for every input (w = 1), so the divide
    // drops out (the general path's only difference is the rounding of its
    // rcp(1) and multiply by it)
    bool q1 = rows[2][0] == 0.0f && rows[2][1] == 0.0f && rows[2][2] == 0.0f && rows[2][3] == 1.0f;
    s_tc_kind[s] = !(s_vsh_lean & 1)    ? TC_GEN_OFF
                   : amp != 0.0f        ? TC_GEN_WARP
                   : !q1                ? TC_GEN_PROJ
                   : src == GX_TG_POS   ? TC_AFF_POS
                   : src == GX_TG_NRM   ? TC_AFF_NRM
                                        : TC_AFF;
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_tcaff + s, s_tc_kind[s] <= TC_AFF_NRM);
    s_tc_used[s] = coord >= 0;
    return amp != 0.0f;
}

static void upload_texmtx(void) {
    // Per-vertex normal matrices (nrm_pervertex): the shader turns the normal
    // of every slot, so all normal texgens of the draw must qualify
    static int pv_on = -1;
    CFG_LIVE(pv_on, cfg_int("S3D_TEXMTX_PV", 1));
    int pv_ok = 0, pv_bad = 0;
    for (int s = 0; s < s_map.unit_count && pv_on; s++) {
        int coord = s_map.unit_texcoord[s];
        const GxTexGen* tg = coord >= 0 ? &g_gx.texgen[coord] : NULL;
        float k;
        int r = nrm_pervertex(tg, coord, &k);
        if (r > 0) pv_ok++;
        else if (r < 0 || (tg && tg->src == GX_TG_NRM)) pv_bad++;
    }
    bool pv = pv_ok && !pv_bad;
    if (pv) PROF_COUNT("gx.texmtx_pervertex", 1);
    else if (pv_ok) PROF_COUNT("gx.texmtx_pervertex_failed", 1);
    if (pv != s_cur_nrm_pv) {
        s_cur_nrm_pv = pv;
        upload_posscale();
    }

    bool warp = false;
    for (int s = 0; s < GX_PICA_TEX_UNITS; s++) {
        bool used = s < s_map.unit_count;
        warp |= upload_unit_texmtx(s, used ? s_map.unit_texcoord[s] : -1, used ? s_map.unit_warp[s] : 0.0f, pv);
    }

    // Warp waves (indirect stand-in): wavelengths of 48 and 34 units across
    // object x + y and z + y (upload_texwarp)
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_warpon, warp);
    s_vsh_warp = warp;
    if (warp) {
        PROF_COUNT("gx.tex_warp", 1);
        upload_texwarp(48.0f);
        s_wt_waves = false;
    }
}

static GPU_TEXTURE_WRAP_PARAM wrap_mode(u8 gx) {
    switch (gx) {
        case GX_REPEAT: return GPU_REPEAT;
        case GX_MIRROR: return GPU_MIRRORED_REPEAT;
        default:        return GPU_CLAMP_TO_EDGE;
    }
}

// GPU blur (gx_copy.c port_copytex_blur): the blurred mip level bound as a
// texture of its own (level 0 of a view on the chain's memory), so the level
// is exact whatever LOD the sampler would pick. One view per
// unit and eye, as C3D_TexBind keeps the pointer until the draw.
static C3D_Tex s_level_view[2][GX_PICA_TEX_UNITS];

static C3D_Tex* copy_level(C3D_Tex* t, int level, int u, int eye) {
    if (level <= 0) {
        t->lodBias = 0;
        t->minLevel = t->maxLevel = 0;
        return t;
    }
    C3D_Tex* v = &s_level_view[eye][u];
    *v = *t;
    u8* data = t->data;
    u32 w = t->width, h = t->height;
    for (int l = 0; l < level; l++, w /= 2, h /= 2) data += w * h * 4;
    v->data = data;
    v->width = (u16)w;
    v->height = (u16)h;
    v->size = w * h * 4;
    v->lodBias = 0;
    v->minLevel = v->maxLevel = 0;
    C3D_TexSetFilter(v, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetFilterMipmap(v, GPU_NEAREST);
    return v;
}

// A decoded texture (or the white placeholder) on unit u
static void bind_decoded(int u, const GxTexObjData* obj) {
    // u = s, v = 1 - t (PICA samples memory row 0 at v = 1)
    s_unit_au[u][0] = 1.0f; s_unit_au[u][1] = 0.0f; s_unit_au[u][2] = 0.0f;
    s_unit_av[u][0] = 0.0f; s_unit_av[u][1] = -1.0f; s_unit_av[u][2] = 1.0f;
    GxTexInfo ti;
    C3D_Tex* tex;
    {
        GX_PROF_ZONE("gx.bind.texget");
        tex = obj->valid ? gx_tex_get(obj, &ti) : NULL;
    }
    if (!tex) {
        // Placeholder: count why, so missing textures are visible in the profile
        if (!obj->valid) {
            PROF_COUNT("gx.tex_missing_unloaded", 1);
        } else {
            PROF_COUNT("gx.tex_missing_failed", 1);
            LOG_ONCE(LOG_LEVEL_WARN, "gx", "texture map %d not usable: data %p %ux%u fmt 0x%X",
                     (int)(obj - g_gx.tex), obj->data, obj->width, obj->height, obj->format);
        }
        bind_unit(u, &s_white_tex, 0);
        return;
    }
    C3D_TexSetWrap(tex, wrap_mode(obj->wrap_s), wrap_mode(obj->wrap_t));
    bool linear_mag = obj->mag_filter != GX_NEAR;
    bool linear_min = obj->min_filter != GX_NEAR && obj->min_filter != GX_NEAR_MIP_NEAR;
    C3D_TexSetFilter(tex, linear_mag ? GPU_LINEAR : GPU_NEAREST, linear_min ? GPU_LINEAR : GPU_NEAREST);
    // Padded NPOT images cover the top-left uscale x vscale of the texture
    s_unit_au[u][0] = ti.uscale;
    s_unit_av[u][1] = -ti.vscale;
    if (ti.levels > 1 && s_tex_mips) {
        // The game's LOD bias (-2 on most textures) sharpened the GameCube's
        // aggressive LOD choice; the PICA's is already sharp, so it is
        // replaced by S3D_TEX_LODBIAS_X4 (quarter levels)
        C3D_TexSetFilterMipmap(tex, GPU_LINEAR);
        C3D_TexSetLodBias(tex, s_tex_lod_bias);
        tex->maxLevel = (u8)(ti.levels - 1);
        tex->minLevel = 0;
        if (s_dbg_mip > 0) {
            // Debug: one level only, to find which level looks wrong on hardware
            u8 l = (u8)(s_dbg_mip - 1 < ti.levels - 1 ? s_dbg_mip - 1 : ti.levels - 1);
            tex->minLevel = tex->maxLevel = l;
        }
    } else {
        C3D_TexSetFilterMipmap(tex, GPU_NEAREST);
        tex->maxLevel = 0;
    }
    bind_unit(u, tex, 0);
    if (s_dumping && (int)s_state_index == s_dbg_texdump) s_texdump_tex[u] = tex;
}

// EFB copy on unit u: transposed framebuffer rectangle (see gx_copy.c), and
// the right eye's own copy when it has one
static void bind_copy(int u, const GxTexObjData* obj, const GxCopyTex* copy) {
    memcpy(s_unit_au[u], copy->au, sizeof(copy->au));
    memcpy(s_unit_av[u], copy->av, sizeof(copy->av));
    s_map.unit_swizzle[u] = copy->swizzle;
    C3D_Tex* ct = (C3D_Tex*)&copy->tex;
    C3D_TexSetWrap(ct, wrap_mode(obj->wrap_t), wrap_mode(obj->wrap_s));
    bool lmag = obj->mag_filter != GX_NEAR;
    bool lmin = obj->min_filter != GX_NEAR && obj->min_filter != GX_NEAR_MIP_NEAR;
    C3D_TexSetFilter(ct, lmag ? GPU_LINEAR : GPU_NEAREST, lmin ? GPU_LINEAR : GPU_NEAREST);
    ct = copy_level(ct, gx_copytex_blur_level(copy, obj->data, false), u, 0);
    bind_unit(u, ct, 0);
    if (copy->has_r && copy->r_frame == copy->last_frame) {
        C3D_Tex* rt = (C3D_Tex*)&copy->tex_r;
        C3D_TexSetWrap(rt, wrap_mode(obj->wrap_t), wrap_mode(obj->wrap_s));
        C3D_TexSetFilter(rt, lmag ? GPU_LINEAR : GPU_NEAREST, lmin ? GPU_LINEAR : GPU_NEAREST);
        rt = copy_level(rt, gx_copytex_blur_level(copy, obj->data, true), u, 1);
        s_unit_copy[0][u] = ct;
        s_unit_copy[1][u] = rt;
        s_eye_rebind = true;
    }
    PROF_COUNT("gx.copytex_binds", 1);
}

// A copy or a decoded texture on unit u (pass 2 of a split-off term)
static void bind_texobj(int u, const GxTexObjData* obj) {
    const GxCopyTex* copy = obj->valid ? gx_copytex_find(obj->data) : NULL;
    if (copy) bind_copy(u, obj, copy);
    else bind_decoded(u, obj);
}

typedef struct {
    GxTexObjData obj;
    C3D_Tex* tex;  // NULL = empty
    u32 param, lod_param;
    u8 min_level, max_level;
    float uscale, vscale;
    u32 frame, copy_epoch;
} BindMemo;
static BindMemo s_bind_memo[GX_PICA_TEX_UNITS];

static void bind_textures(void) {
    s_eye_rebind = false;
    for (int u = 0; u < GX_PICA_TEX_UNITS; u++) {
        // Decoded textures: u = s, v = 1 - t (PICA samples memory row 0 at v = 1)
        s_unit_au[u][0] = 1.0f; s_unit_au[u][1] = 0.0f; s_unit_au[u][2] = 0.0f;
        s_unit_av[u][0] = 0.0f; s_unit_av[u][1] = -1.0f; s_unit_av[u][2] = 1.0f;
        s_map.unit_swizzle[u] = 0;
        s_unit_copy[0][u] = s_unit_copy[1][u] = NULL;
        if (u >= s_map.unit_count) {
            // Unused unit: the combiners do not sample it, but the GPU fetches
            // every bound unit, so a stale binding (citro2d's system font sheet
            // after the touch UI, bug 12) must not stay enabled
            gx_c3d_tex_unbind(u);
            continue;
        }
        const GxTexObjData* obj = &g_gx.tex[s_map.unit_texmap[u]];
#if GX_PROFILE_DETAIL
        {
            // Units that get the same texture object as at their last bind
            static GxTexObjData s_last[GX_PICA_TEX_UNITS];
            PROF_COUNT("prep.bind_units", 1);
            if (memcmp(&s_last[u], obj, sizeof(*obj)) == 0) PROF_COUNT("prep.bind_same", 1);
            s_last[u] = *obj;
        }
#endif

        // Same texture object as this unit's last bind this frame
        // (S3D_PREP_FAST bit 2): the decoded texture, its sampler settings and
        // the unit binding all still stand. gx_tex_get already verified the
        // texture this frame, and no copy destination moved since
        BindMemo* m = &s_bind_memo[u];
        if ((s_prep_fast & 2) && m->tex && m->frame == s_frame_number && m->copy_epoch == g_gx_copy_epoch &&
            __C3D_Context.tex[u] == m->tex && m->tex->param == m->param && m->tex->lodParam == m->lod_param &&
            m->tex->minLevel == m->min_level && m->tex->maxLevel == m->max_level && !s_dumping &&
            memcmp(&m->obj, obj, sizeof(*obj)) == 0) {
            s_unit_au[u][0] = m->uscale;
            s_unit_av[u][1] = -m->vscale;
            PROF_COUNT("gx.bind_kept", 1);
            continue;
        }
        m->tex = NULL;

        const GxCopyTex* copy;
        {
            GX_PROF_ZONE("gx.bind.copyfind");
            copy = obj->valid ? gx_copytex_find(obj->data) : NULL;
        }
        if (copy) {
            bind_copy(u, obj, copy);
            continue;
        }

        C3D_Tex* movie_tex = NULL;
        int plane = obj->valid ? movie_plane(obj->data, &movie_tex) : MOVIE_PLANE_NONE;
        if (plane != MOVIE_PLANE_NONE) {
            // Attract movie (port/movie_3ds.c): the Y plane is the RGB frame,
            // U/V are not sampled by the rewritten TEV program (gx_tev.c)
            C3D_Tex* mt = plane == MOVIE_PLANE_Y ? movie_tex : &s_white_tex;
            C3D_TexSetWrap(mt, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
            C3D_TexSetFilter(mt, GPU_LINEAR, GPU_LINEAR);
            bind_unit(u, mt, 0);
            continue;
        }

        bind_decoded(u, obj);
        C3D_Tex* t = __C3D_Context.tex[u];
        if (t && t != &s_white_tex) {
            m->obj = *obj;
            m->tex = t;
            m->param = t->param;
            m->lod_param = t->lodParam;
            m->min_level = t->minLevel;
            m->max_level = t->maxLevel;
            m->uscale = s_unit_au[u][0];
            m->vscale = -s_unit_av[u][1];
            m->frame = s_frame_number;
            m->copy_epoch = g_gx_copy_epoch;
        }
    }
}

static void set_unif_color(int loc, GXColor c) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, loc, c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
}

// GX lighting channel 0 or 1 (s_light_chan, see gx_tev_ras_channel) -> vertex
// shader uniforms (see the "light" procedure in default.v.pica).
// The color and alpha parts keep their own light masks; each light takes the
// diffuse and attenuation functions of its part (color's when it is in both).
static int s_light_chan;

static void upload_lighting(void) {
    int n = s_light_chan;
    const GxChannel* cc = &g_gx.chan[n];
    const GxChannel* ac = &g_gx.chan[2 + n];
    bool active = g_gx.num_chans > n &&
                  (cc->enable || ac->enable || cc->mat_src == GX_SRC_REG || ac->mat_src == GX_SRC_REG);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_lighting, active);
    s_vsh_lit = active;
    s_light_count = 0;
    if (!active) return;

    set_unif_color(s_loc_matreg, g_gx.chan_mat[n]);
    set_unif_color(s_loc_ambreg, g_gx.chan_amb[n]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_chansel, cc->mat_src == GX_SRC_VTX, ac->mat_src == GX_SRC_VTX,
                  cc->amb_src == GX_SRC_VTX, ac->amb_src == GX_SRC_VTX);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_litsel, cc->enable, ac->enable, 0.0f, 0.0f);

    u8 cmask = cc->enable ? cc->light_mask : 0;
    u8 amask = ac->enable ? ac->light_mask : 0;
    for (int i = 0; i < GX_NUM_LIGHTS; i++) {
        bool in_c = (cmask & (1 << i)) != 0, in_a = (amask & (1 << i)) != 0;
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_lighten + i, in_c || in_a);
        if (!in_c && !in_a) continue;
        s_light_count++;

        const GxChannel* ch = in_c ? cc : ac;
        const GxLight* l = &g_gx.lights[i];
        float spec = ch->attn_fn == GX_AF_SPEC, clamp = ch->diff_fn != GX_DF_NONE;
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_lightpos + i, l->pos[0], l->pos[1], l->pos[2], in_c);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_lightdir + i, l->dir[0], l->dir[1], l->dir[2], in_a);
        set_unif_color(s_loc_lightcol + i, l->color);
        if (ch->attn_fn == GX_AF_NONE) {
            // No attenuation: a = k = (1, 0, 0) makes the factor exactly 1
            C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_lighta + i, 1.0f, 0.0f, 0.0f, 0.0f);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_lightk + i, 1.0f, 0.0f, 0.0f, clamp);
        } else {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_lighta + i, l->a[0], l->a[1], l->a[2], spec);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_lightk + i, l->k[0], l->k[1], l->k[2], clamp);
        }
    }
}

// A texture bind_decoded handles (not a screen copy or a movie plane)
static bool tex_decoded(const GxTexObjData* obj) {
    return obj->valid && !gx_copytex_find(obj->data) && movie_plane(obj->data, NULL) == MOVIE_PLANE_NONE;
}

// Average colour of a texture's level 0 (PICA formats with 8+ bits per texel)
static bool tex_average(const C3D_Tex* t, u8 out[4]) {
    const u8* d = (const u8*)t->data;
    u32 n = (u32)t->width * t->height;
    if (!d || !n) return false;
    u32 sum[4] = { 0, 0, 0, 0 };
    for (u32 i = 0; i < n; i++) {
        u8 c[4];
        u32 v = (u32)(d[i * 2] | d[i * 2 + 1] << 8);
        switch (t->fmt) {
            case GPU_RGBA8: c[0] = d[i * 4 + 3]; c[1] = d[i * 4 + 2]; c[2] = d[i * 4 + 1]; c[3] = d[i * 4]; break;
            case GPU_RGB8: c[0] = d[i * 3 + 2]; c[1] = d[i * 3 + 1]; c[2] = d[i * 3]; c[3] = 255; break;
            case GPU_RGB565: c[0] = (v >> 11) << 3; c[1] = ((v >> 5) & 63) << 2; c[2] = (v & 31) << 3; c[3] = 255; break;
            case GPU_RGBA5551:
                c[0] = (v >> 11) << 3; c[1] = ((v >> 6) & 31) << 3; c[2] = ((v >> 1) & 31) << 3; c[3] = (v & 1) * 255;
                break;
            case GPU_RGBA4:
                c[0] = (v >> 12) * 17; c[1] = ((v >> 8) & 15) * 17; c[2] = ((v >> 4) & 15) * 17; c[3] = (v & 15) * 17;
                break;
            case GPU_LA8: c[0] = c[1] = c[2] = d[i * 2 + 1]; c[3] = d[i * 2]; break;
            case GPU_L8: c[0] = c[1] = c[2] = d[i]; c[3] = 255; break;
            case GPU_LA4: c[0] = c[1] = c[2] = (d[i] >> 4) * 17; c[3] = (d[i] & 15) * 17; break;
            default: return false;
        }
        for (int b = 0; b < 4; b++) sum[b] += c[b];
    }
    for (int b = 0; b < 4; b++) out[b] = (u8)(sum[b] / n);
    return true;
}

// Bug 37: the TEV program reads a dropped texture (s_map.avg_map, the eye
// glow's noise) as its average colour. Computed once per texture
static void update_drop_avg(void) {
    if (s_map.avg_map < 0) return;
    static const void* s_avg_data;
    static u8 s_avg[4] = { 128, 128, 128, 255 };
    const GxTexObjData* obj = &g_gx.tex[s_map.avg_map];
    if (obj->valid && obj->data != s_avg_data && tex_decoded(obj)) {
        GxTexInfo ti;
        C3D_Tex* t = gx_tex_get(obj, &ti);
        if (t && tex_average(t, s_avg)) {
            s_avg_data = obj->data;
            LOG_ONCE(LOG_LEVEL_INFO, "tev", "dropped texture average %02X%02X%02X%02X", s_avg[0], s_avg[1], s_avg[2],
                     s_avg[3]);
        }
    }
    gx_tev_set_drop_avg(s_avg);
}

// Whether the current state draws a second pass for a dropped texture. Only
// opaque, depth-writing draws: pass 2 matches pass 1's pixels by depth and
// combines with the blender. Runs after the combiners and fog are set, which
// it snapshots for the restore after pass 2
static void multipass_prepare(void) {
    s_mp_on = false;
    s_mp_split = s_mp_proj = false;
    const GxPixelState* px = &g_gx.pixel;
    if (!s_mp_enabled || s_dbg_idcolor || s_raw_draw || gx_capture_replaces()) return;
    if (px->blend_mode != GX_BM_NONE || !px->z_enable || !px->z_update || !px->color_update) return;
    if (!gx_tev_drops_texture(&s_map)) return;
    bool split = s_map.proj_split >= 0;
    u8 unit_ok = 0;
    for (int u = 0; u < s_map.unit_count; u++) {
        if (tex_decoded(&g_gx.tex[s_map.unit_texmap[u]])) unit_ok |= 1u << u;
    }
    // Bug 32: the split-off projected term goes on unit 0 whatever it holds
    // (draw_second_pass saves and restores the binding)
    if (split) unit_ok = 1;
    if (!gx_tev_multipass(&s_map, unit_ok, split ? 0 : -1, &s_mp)) return;
    const GxTexObjData* obj = &g_gx.tex[s_mp.texmap];
    if (split) {
        if (s_mp.texmap != s_map.proj_split || !obj->valid) return;
        // Pass 2's program reads the color unswizzled
        const GxCopyTex* copy = gx_copytex_find(obj->data);
        if (copy ? copy->swizzle != 0 : movie_plane(obj->data, NULL) != MOVIE_PLANE_NONE) return;
        // Per fragment with the water shader, which turns no normals and
        // feeds no fragment lighting; else per vertex as before
        // Debug: S3D_DBG_REFL_PV=1 samples per vertex anyway (A/B of the same pass)
        static int dbg_pv = -1;
        CFG_LIVE(dbg_pv, cfg_int("S3D_DBG_REFL_PV", 0));
        s_mp_split = true;
        s_mp_proj = !s_frag_spec && !s_cur_nrm_pv && !dbg_pv;
        PROF_COUNT(s_mp_proj ? "gx.proj_split_states" : "gx.proj_split_pervertex", 1);
    } else if (!tex_decoded(obj)) {
        return;
    }
    for (int i = 0; i < GX_PICA_TEV_STAGES; i++) s_mp_main_env[i] = *C3D_GetTexEnv(i);
    gx_tev_last_buf(s_mp_main_buf);
    s_mp_on = true;
}

// Pass 1 replaces the program: reflection * (1 - ka), alpha from the vertex
// color, blended as the state says. Runs after the combiners and fog are set
static void water_split_setup(void) {
    s_ws_on = false;
    const GxPixelState* px = &g_gx.pixel;
    if (!s_ws_enabled || s_dbg_idcolor || s_raw_draw || s_mp_on || gx_capture_replaces()) return;
    if (px->blend_mode != GX_BM_BLEND || px->blend_src != GX_BL_SRCALPHA || px->blend_dst != GX_BL_INVSRCALPHA ||
        px->z_update || !px->color_update)
        return;
    if (!gx_tev_water_split(&s_map, &s_ws)) return;
    bool frag_fog = s_fog_mode == FOG_MODE_FRAG;
    for (int p = 0; p < 2; p++) {
        C3D_TexEnv* env = s_ws_env[p];
        for (int i = 0; i < GX_PICA_TEV_STAGES; i++) C3D_TexEnvInit(&env[i]);
        C3D_TexEnvSrc(&env[0], C3D_RGB, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
        C3D_TexEnvSrc(&env[0], C3D_Alpha, GPU_PRIMARY_COLOR, GPU_CONSTANT, GPU_PRIMARY_COLOR);
        C3D_TexEnvColor(&env[0], s_ws.konst);
        if (p == 0) {
            C3D_TexEnvOpRgb(&env[0], GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA, GPU_TEVOP_RGB_SRC_COLOR);
            C3D_TexEnvFunc(&env[0], C3D_RGB, GPU_MODULATE);
            C3D_TexEnvFunc(&env[0], C3D_Alpha, GPU_REPLACE);
        } else {
            C3D_TexEnvFunc(&env[0], C3D_RGB, GPU_ADD);
            C3D_TexEnvFunc(&env[0], C3D_Alpha, GPU_MODULATE);
        }
        if (frag_fog) {
            C3D_TexEnvSrc(&env[1], C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_FRAGMENT_PRIMARY_COLOR);
            C3D_TexEnvFunc(&env[1], C3D_RGB, GPU_INTERPOLATE);
            C3D_TexEnvColor(&env[1], p == 0 ? s_fog_color : 0);
        }
    }
    static const u8 buf[2] = { 0, 0 };
    set_combiners(s_ws_env[0], buf);
    s_ws_on = true;
    PROF_COUNT("gx.water_split_states", 1);
}

// The water shader passes the vertex alpha through and computes no lighting,
// and its only texgen input is the position, so it only takes splits whose
// channel alpha is the unlit vertex alpha and whose two coordinates are
// generated from the position (the game's water: GX_TG_MTX3x4 of GX_TG_POS)
static bool water_proj_ok(void) {
    static int on = -1;  // S3D_WATER_PROJ
    CFG_LIVE(on, cfg_bool("S3D_WATER_PROJ", true) ? 1 : 0);
    if (!on || s_frag_spec || s_cur_nrm_pv) return false;
    int c[2] = { s_map.unit_texcoord[0], s_ws.refr_texcoord };
    for (int i = 0; i < 2; i++)
        if (c[i] < 0 || c[i] >= g_gx.num_texgens || g_gx.texgen[c[i]].src != GX_TG_POS) return false;
    int n = s_light_chan;
    if (g_gx.num_chans <= n) return true;
    const GxChannel* ac = &g_gx.chan[2 + n];
    return !ac->enable && ac->mat_src == GX_SRC_VTX;
}

static void water_split_prepare(void) {
    water_split_setup();
    set_ws_proj(s_ws_on && water_proj_ok());
}

// --- Render census (mark snapshot) ---------------------------------------------
// diag_mark() asks for one; the next whole frame counts every state change by
// depth/blend/alpha/cull/projection/shader/combiner setup, collects the
// distinct depth mappings (viewport near/far + projection z terms) with their
// draws and triangles, and keeps the first CENSUS_LIST states as one-liners.
// Logged (tag "census") at the following frame_begin.

#define CENSUS_LIST   48
#define CENSUS_DEPTHS 8

typedef struct {
    u16 index, draws;
    u8 z_en, z_func, z_upd, blend, bsrc, bdst, acmp0, aref0, aop, acmp1, aref1, cull, cmask;
    u8 persp, lit, warp, nout, units, mp, ws, fog, spec, off;
    float vn, vf, p4, p5;
    u32 tris;
} CensusState;

typedef struct {
    float vn, vf, p4, p5;
    u8 persp;
    u32 states, tris;
} CensusDepth;

static struct {
    u32 states, draws, tris;
    u32 z_off, z_upd[8], z_noupd[8];
    u32 blend[4];
    u32 atest, cull[4], persp, ortho;
    u32 lit, warp, nout[8], units[4];
    u32 mp, ws, fog[3], spec, off, nocolor, raw;
    u32 path[3], path_tris[3];  // By vertex layout: std, dyn16, packed
    u32 tris_zoff, tris_blend;
    u32 depth_more;  // States whose depth mapping did not fit in depths[]
    float vp_zmin, vp_zmax;
    CensusDepth depths[CENSUS_DEPTHS];
    u32 ndepths;
    u32 cur_depth;   // depths[] slot of the current state (CENSUS_DEPTHS: none)
    u32 copies, copy_skips, offscreen_fails;  // Counter values at the census start
    CensusState list[CENSUS_LIST];
} s_cen;
static bool s_census_req;
static u32 s_census_frame;

u32 g_gx_copies, g_gx_copy_skips, g_gx_offscreen_fails;

void gx_render_census_request(void) { s_census_req = true; }

void gx_render_get_draw_stats(GxDrawStats* out) {
    out->drops = s_drop_draws;
    out->drop_verts = s_drop_verts;
    out->drop_frames = s_drop_frames;
    out->last_drop_frame = s_drop_last;
    out->vtx_peak = s_vtx_peak;
    out->vtx_max = MAX_FRAME_VERTICES;
    out->idx_peak = s_idx_peak;
    out->idx_max = MAX_FRAME_INDICES;
    out->dyn_peak_kb = s_dyn_peak >> 10;
    out->dyn_max_kb = FRAME_DYN_BYTES >> 10;
    out->dyn_fails = s_dyn_fails;
    out->copies = g_gx_copies;
    out->copy_skips = g_gx_copy_skips;
    out->offscreen_fails = g_gx_offscreen_fails;
}

static void census_state(void) {
    const GxPixelState* px = &g_gx.pixel;
    s_cen.states++;
    if (!px->z_enable) s_cen.z_off++;
    else if (px->z_update) s_cen.z_upd[px->z_func & 7]++;
    else s_cen.z_noupd[px->z_func & 7]++;
    s_cen.blend[px->blend_mode & 3]++;
    if (!(px->alpha_comp0 == GX_ALWAYS && px->alpha_comp1 == GX_ALWAYS)) s_cen.atest++;
    s_cen.cull[px->cull_mode & 3]++;
    bool persp = g_gx.proj_type == GX_PERSPECTIVE;
    if (persp) s_cen.persp++;
    else s_cen.ortho++;
    if (s_vsh_lit) s_cen.lit++;
    if (s_vsh_warp) s_cen.warp++;
    s_cen.nout[s_tev_nout & 7]++;
    s_cen.units[s_map.unit_count & 3]++;
    if (s_mp_on) s_cen.mp++;
    if (s_ws_on) s_cen.ws++;
    if (s_fog_mode < 3) s_cen.fog[s_fog_mode]++;
    if (s_frag_spec) s_cen.spec++;
    bool off = gx_offscreen_active();
    if (off) s_cen.off++;
    if (!px->color_update) s_cen.nocolor++;

    float vn = g_gx.viewport[4], vf = g_gx.viewport[5];
    if (s_cen.states == 1 || vn < s_cen.vp_zmin) s_cen.vp_zmin = vn;
    if (s_cen.states == 1 || vf > s_cen.vp_zmax) s_cen.vp_zmax = vf;
    u32 d;
    for (d = 0; d < s_cen.ndepths; d++) {
        const CensusDepth* e = &s_cen.depths[d];
        if (e->vn == vn && e->vf == vf && e->p4 == g_gx.proj[4] && e->p5 == g_gx.proj[5] && e->persp == persp) break;
    }
    if (d == s_cen.ndepths && d < CENSUS_DEPTHS) {
        CensusDepth* e = &s_cen.depths[s_cen.ndepths++];
        e->vn = vn;
        e->vf = vf;
        e->p4 = g_gx.proj[4];
        e->p5 = g_gx.proj[5];
        e->persp = persp;
    }
    if (d < CENSUS_DEPTHS) s_cen.depths[d].states++;
    else s_cen.depth_more++;
    s_cen.cur_depth = d;

    if (s_state_index < CENSUS_LIST) {
        CensusState* c = &s_cen.list[s_state_index];
        c->index = (u16)s_state_index;
        c->z_en = px->z_enable;
        c->z_func = px->z_func;
        c->z_upd = px->z_update;
        c->blend = px->blend_mode;
        c->bsrc = px->blend_src;
        c->bdst = px->blend_dst;
        c->acmp0 = px->alpha_comp0;
        c->aref0 = px->alpha_ref0;
        c->aop = px->alpha_op;
        c->acmp1 = px->alpha_comp1;
        c->aref1 = px->alpha_ref1;
        c->cull = px->cull_mode;
        c->cmask = (px->color_update ? 1 : 0) | (px->alpha_update ? 2 : 0);
        c->persp = persp;
        c->lit = s_vsh_lit;
        c->warp = s_vsh_warp;
        c->nout = s_tev_nout;
        c->units = (u8)s_map.unit_count;
        c->mp = s_mp_on;
        c->ws = s_ws_on;
        c->fog = s_fog_mode;
        c->spec = s_frag_spec;
        c->off = off;
        c->vn = vn;
        c->vf = vf;
        c->p4 = g_gx.proj[4];
        c->p5 = g_gx.proj[5];
    }
}

static void census_draw(u32 count) {
    u32 tris = count / 3;
    s_cen.draws++;
    s_cen.tris += tris;
    if (s_raw_draw) {
        s_cen.raw++;
        return;
    }
    int p = s_attr_layout == LAYOUT_DYN16 ? 1 : s_attr_layout == LAYOUT_PACKED ? 2 : 0;
    s_cen.path[p]++;
    s_cen.path_tris[p] += tris;
    if (!g_gx.pixel.z_enable) s_cen.tris_zoff += tris;
    if (g_gx.pixel.blend_mode == GX_BM_BLEND) s_cen.tris_blend += tris;
    if (s_cen.cur_depth < CENSUS_DEPTHS) s_cen.depths[s_cen.cur_depth].tris += tris;
    if (s_state_index > 0 && s_state_index - 1 < CENSUS_LIST) {
        s_cen.list[s_state_index - 1].draws++;
        s_cen.list[s_state_index - 1].tris += tris;
    }
}

static const char* const s_cmp_names[8] = { "nev", "lt", "eq", "le", "gt", "ne", "ge", "alw" };

static void census_report(void) {
    LOG_INFO("census", "frame %lu: %lu states %lu draws %lu tris | std %lu/%lu dyn16 %lu/%lu packed %lu/%lu raw %lu "
             "(draws/tris) | tris z-off %lu blended %lu",
             (unsigned long)s_census_frame, (unsigned long)s_cen.states, (unsigned long)s_cen.draws,
             (unsigned long)s_cen.tris, (unsigned long)s_cen.path[0], (unsigned long)s_cen.path_tris[0],
             (unsigned long)s_cen.path[1], (unsigned long)s_cen.path_tris[1], (unsigned long)s_cen.path[2],
             (unsigned long)s_cen.path_tris[2], (unsigned long)s_cen.raw, (unsigned long)s_cen.tris_zoff,
             (unsigned long)s_cen.tris_blend);
    LOG_INFO("census", "buffers: vertices %lu/%lu indices %lu/%lu dyn %lu/%lu KB | copies %lu skipped %lu "
             "| offscreen fails %lu",
             (unsigned long)s_vused, (unsigned long)MAX_FRAME_VERTICES, (unsigned long)s_iused,
             (unsigned long)MAX_FRAME_INDICES, (unsigned long)(s_dynused >> 10),
             (unsigned long)(FRAME_DYN_BYTES >> 10), (unsigned long)(g_gx_copies - s_cen.copies),
             (unsigned long)(g_gx_copy_skips - s_cen.copy_skips),
             (unsigned long)(g_gx_offscreen_fails - s_cen.offscreen_fails));
    char zs[200];
    int n = snprintf(zs, sizeof(zs), "off %lu", (unsigned long)s_cen.z_off);
    for (int f = 0; f < 8; f++) {
        if (s_cen.z_upd[f] && n < (int)sizeof(zs))
            n += snprintf(zs + n, sizeof(zs) - n, " %s+w %lu", s_cmp_names[f], (unsigned long)s_cen.z_upd[f]);
        if (s_cen.z_noupd[f] && n < (int)sizeof(zs))
            n += snprintf(zs + n, sizeof(zs) - n, " %s %lu", s_cmp_names[f], (unsigned long)s_cen.z_noupd[f]);
    }
    LOG_INFO("census", "z: %s | depth bias %d vp z %.4g..%.4g", zs, (int)s_depth_bias, s_cen.vp_zmin, s_cen.vp_zmax);
    LOG_INFO("census", "blend none %lu blend %lu logic %lu sub %lu | atest %lu | cull none %lu front %lu back %lu "
             "all %lu | persp %lu ortho %lu | no color %lu offscreen %lu",
             (unsigned long)s_cen.blend[0], (unsigned long)s_cen.blend[1], (unsigned long)s_cen.blend[2],
             (unsigned long)s_cen.blend[3], (unsigned long)s_cen.atest, (unsigned long)s_cen.cull[0],
             (unsigned long)s_cen.cull[1], (unsigned long)s_cen.cull[2], (unsigned long)s_cen.cull[3],
             (unsigned long)s_cen.persp, (unsigned long)s_cen.ortho, (unsigned long)s_cen.nocolor,
             (unsigned long)s_cen.off);
    LOG_INFO("census", "vsh lit %lu warp %lu | spec %lu | combiners 0..7: %lu %lu %lu %lu %lu %lu %lu %lu "
             "| units 0..3: %lu %lu %lu %lu | 2-pass %lu water %lu | fog none %lu lut %lu frag %lu",
             (unsigned long)s_cen.lit, (unsigned long)s_cen.warp, (unsigned long)s_cen.spec,
             (unsigned long)s_cen.nout[0], (unsigned long)s_cen.nout[1], (unsigned long)s_cen.nout[2],
             (unsigned long)s_cen.nout[3], (unsigned long)s_cen.nout[4], (unsigned long)s_cen.nout[5],
             (unsigned long)s_cen.nout[6], (unsigned long)s_cen.nout[7], (unsigned long)s_cen.units[0],
             (unsigned long)s_cen.units[1], (unsigned long)s_cen.units[2], (unsigned long)s_cen.units[3],
             (unsigned long)s_cen.mp, (unsigned long)s_cen.ws, (unsigned long)s_cen.fog[0],
             (unsigned long)s_cen.fog[1], (unsigned long)s_cen.fog[2]);
    for (u32 d = 0; d < s_cen.ndepths; d++) {
        const CensusDepth* e = &s_cen.depths[d];
        // The C3D_DepthMap apply_depth_map sets for this viewport
        bool map = s_depth_bias && e->vf > e->vn;
        LOG_INFO("census", "depth%lu: vp %.5g..%.5g %s pz %.6g %.6g -> map %.5g %.5g | %lu states %lu tris",
                 (unsigned long)d, e->vn, e->vf, e->persp ? "persp" : "ortho", e->p4, e->p5,
                 map ? -(e->vf - e->vn) : -1.0f, map ? 1.0f - e->vf : 0.0f, (unsigned long)e->states,
                 (unsigned long)e->tris);
    }
    if (s_cen.depth_more) LOG_INFO("census", "depth: %lu more states with other mappings", (unsigned long)s_cen.depth_more);
    u32 shown = s_cen.states < CENSUS_LIST ? s_cen.states : CENSUS_LIST;
    for (u32 i = 0; i < shown; i++) {
        const CensusState* c = &s_cen.list[i];
        LOG_INFO("census", "#%u z%c%s%s b%u %u/%u a %s%u op%u %s%u cull%u w%x %s vp %.4g..%.4g pz %.4g %.4g%s%s "
                 "c%u u%u%s%s f%u%s%s | %u draws %lu tris",
                 c->index, c->z_en ? '+' : '-', s_cmp_names[c->z_func & 7], c->z_upd ? "+w" : "", c->blend, c->bsrc,
                 c->bdst, s_cmp_names[c->acmp0 & 7], c->aref0, c->aop, s_cmp_names[c->acmp1 & 7], c->aref1, c->cull,
                 c->cmask, c->persp ? "P" : "O", c->vn, c->vf, c->p4, c->p5, c->lit ? " lit" : "",
                 c->warp ? " warp" : "", c->nout, c->units, c->mp ? " 2p" : "", c->ws ? " ws" : "", c->fog,
                 c->spec ? " spec" : "", c->off ? " OFF" : "", c->draws, (unsigned long)c->tris);
    }
    if (s_cen.states > shown) LOG_INFO("census", "(%lu more states)", (unsigned long)(s_cen.states - shown));
    LOG_INFO("census", "--- end census ---");
    log_flush();
}

// Frame begin: per-frame peaks of the frame that just ended, the census
// report, and starting a requested census
static void census_frame_begin(void) {
    if (s_vused > s_vtx_peak) s_vtx_peak = s_vused;
    if (s_iused > s_idx_peak) s_idx_peak = s_iused;
    if (s_dynused > s_dyn_peak) s_dyn_peak = s_dynused;
    if (s_drop_cur) {
        s_drop_frames++;
        s_drop_last = s_frame_number - 1;
        s_drop_cur = 0;
    }
    if (s_census_on) {
        census_report();
        s_census_on = false;
    }
    if (s_census_req) {
        s_census_req = false;
        memset(&s_cen, 0, sizeof(s_cen));
        s_cen.cur_depth = CENSUS_DEPTHS;
        s_cen.copies = g_gx_copies;
        s_cen.copy_skips = g_gx_copy_skips;
        s_cen.offscreen_fails = g_gx_offscreen_fails;
        s_census_on = true;
        s_census_frame = s_frame_number;
    }
}

// State census (S3D_STATE_CENSUS=1, DRAWCALL_PLAN.md "Draw state memo"): at
// every state setup, hashes of the whole material state (no matrices or light
// values) and counts how often it repeats, to size a memo of translated state.
// Keys: mat = TEV, texgen setup, channel setup, pixel state, bound textures;
// col = mat + material/ambient colors; all = col + projection, viewport and
// scissor; lit = all + light values
u32 gx_tev_state_hash(u32 h, int part);
u32 gx_tev_texmap_mask(void);

static u32 sc_hash(u32 h, const void* p, u32 bytes);

static u32 sc_tex_hash(u32 h) {
    u32 m = gx_tev_texmap_mask();
    h = sc_hash(h, &m, sizeof(m));
    for (int i = 0; i < GX_NUM_TEXMAP; i++)
        if (m & (1u << i)) h = sc_hash(h, &g_gx.tex[i], sizeof(g_gx.tex[i]));
    return h;
}
enum { SC_KEYS = 10, SC_SLOTS = 16384 };
static struct {
    u32 key;
    u32 frame;
} s_sc_tab[SC_KEYS][SC_SLOTS];
static u32 s_sc_last[SC_KEYS];
// Material key of the current state (S3D_STATE_CENSUS on; gx_dlcache.c map census)
u32 gx_render_census_key(void) { return s_sc_last[0]; }

static u32 sc_hash(u32 h, const void* p, u32 bytes) {
    const u8* b = p;
    for (u32 i = 0; i < bytes; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

static void state_census(u32 dirty) {
    static int ids[SC_KEYS][4];
    static bool registered;
    if (!registered) {
        static const char* const names[SC_KEYS] = { "mat", "col", "all", "lit", "p_tev", "p_kcol", "p_texgen", "p_chan", "p_pixel", "p_tex" };
        static const char* const kinds[4] = { "same", "new", "prevframe", "unique" };
        static char buf[SC_KEYS][4][24];
        for (int i = 0; i < SC_KEYS; i++)
            for (int j = 0; j < 4; j++) {
                snprintf(buf[i][j], sizeof(buf[i][j]), "sc.%s_%s", names[i], kinds[j]);
                ids[i][j] = prof_register_counter(buf[i][j]);
            }
        registered = true;
    }
    u32 k[SC_KEYS];
    u32 h0 = 2166136261u;
    k[4] = gx_tev_state_hash(h0, 1);
    k[5] = gx_tev_state_hash(h0, 2);
    k[6] = sc_hash(sc_hash(h0, &g_gx.num_texgens, 1), g_gx.texgen, sizeof(g_gx.texgen));
    k[7] = sc_hash(sc_hash(h0, &g_gx.num_chans, 1), g_gx.chan, sizeof(g_gx.chan));
    k[8] = sc_hash(h0, &g_gx.pixel, sizeof(g_gx.pixel));
    k[9] = sc_tex_hash(h0);
    u32 h = gx_tev_state_hash(2166136261u, 1);  // Register/konst colors are per-draw values
    h = sc_hash(h, &g_gx.num_texgens, 1);
    h = sc_hash(h, g_gx.texgen, sizeof(GxTexGen) * (g_gx.num_texgens < GX_NUM_TEXCOORD ? g_gx.num_texgens : GX_NUM_TEXCOORD));
    h = sc_hash(h, &g_gx.num_chans, 1);
    h = sc_hash(h, g_gx.chan, sizeof(g_gx.chan));
    h = sc_hash(h, &g_gx.pixel, sizeof(g_gx.pixel));
    h = sc_tex_hash(h);
    k[0] = h;
    h = sc_hash(h, g_gx.chan_mat, sizeof(g_gx.chan_mat));
    h = sc_hash(h, g_gx.chan_amb, sizeof(g_gx.chan_amb));
    k[1] = h;
    h = sc_hash(h, &g_gx.proj_type, 1);
    h = sc_hash(h, g_gx.proj, sizeof(g_gx.proj));
    h = sc_hash(h, g_gx.viewport, sizeof(g_gx.viewport));
    h = sc_hash(h, g_gx.scissor, sizeof(g_gx.scissor));
    k[2] = h;
    h = sc_hash(h, g_gx.lights, sizeof(g_gx.lights));
    k[3] = h;
    u32 frame = s_frame_number + 1;  // 0 = empty slot
    PROF_COUNT("sc.prepares", 1);
    if (k[0] == s_sc_last[0]) {
        // Same material as the last setup: which dirty bits made it run
        if (dirty & GX_DIRTY_TEV) PROF_COUNT("sc.same_d_tev", 1);
        if (dirty & GX_DIRTY_TEX) PROF_COUNT("sc.same_d_tex", 1);
        if (dirty & GX_DIRTY_TEXMTX) PROF_COUNT("sc.same_d_texmtx", 1);
        if (dirty & GX_DIRTY_PIXEL) PROF_COUNT("sc.same_d_pixel", 1);
        if (dirty & GX_DIRTY_LIGHT) PROF_COUNT("sc.same_d_light", 1);
        if (dirty & GX_DIRTY_PROJ) PROF_COUNT("sc.same_d_proj", 1);
        if (dirty & GX_DIRTY_POSMTX) PROF_COUNT("sc.same_d_posmtx", 1);
        if (!(dirty & ~(GX_DIRTY_POSMTX | GX_DIRTY_PROJ))) PROF_COUNT("sc.same_cheap", 1);
        if (k[5] != s_sc_last[5]) PROF_COUNT("sc.same_kcol_changed", 1);
    } else {
        if (dirty & GX_DIRTY_TEV) PROF_COUNT("sc.diff_d_tev", 1);
        if (dirty & GX_DIRTY_TEX) PROF_COUNT("sc.diff_d_tex", 1);
        if (dirty & GX_DIRTY_TEXMTX) PROF_COUNT("sc.diff_d_texmtx", 1);
        if (dirty & GX_DIRTY_PIXEL) PROF_COUNT("sc.diff_d_pixel", 1);
        if (dirty & GX_DIRTY_LIGHT) PROF_COUNT("sc.diff_d_light", 1);
    }
    for (int i = 0; i < SC_KEYS; i++) {
        if (k[i] == s_sc_last[i]) prof_add(ids[i][0], 1);
        s_sc_last[i] = k[i];
        u32 slot = (k[i] ^ (k[i] >> 15)) % SC_SLOTS;
        for (u32 n = 0; n < 32; n++, slot = (slot + 1) % SC_SLOTS) {
            if (s_sc_tab[i][slot].frame && s_sc_tab[i][slot].key != k[i]) {
                if (frame - s_sc_tab[i][slot].frame > 30) {
                    s_sc_tab[i][slot].frame = 0;  // Stale: reuse
                } else {
                    continue;
                }
            }
            if (!s_sc_tab[i][slot].frame) {
                prof_add(ids[i][1], 1);
                prof_add(ids[i][3], 1);
            } else if (s_sc_tab[i][slot].frame != frame) {
                prof_add(ids[i][2], 1);
                prof_add(ids[i][3], 1);
            }
            s_sc_tab[i][slot].key = k[i];
            s_sc_tab[i][slot].frame = frame;
            break;
        }
    }
}

const GxUnitMap* gx_render_unit_map(void) { return &s_map; }

const GxUnitMap* gx_render_prepare(void) {
    u32 dirty = g_gx.dirty;
    if (!dirty) return &s_map;
    GX_PROF_ZONE("gx.prepare");
    static int s_sc_on = -1;
    CFG_LIVE(s_sc_on, cfg_int("S3D_STATE_CENSUS", 0));
    if (s_sc_on) state_census(dirty);
#if GX_PROFILE_DETAIL
    // Why state setup runs (MULTICORE_PLAN.md, "Render thread: redundant work")
    if (dirty & GX_DIRTY_TEV) PROF_COUNT("prep.d_tev", 1);
    else if (dirty & GX_DIRTY_TEX) PROF_COUNT("prep.d_tex_only", 1);  // Textures without TEV
    else if (dirty & GX_DIRTY_TEXMTX) PROF_COUNT("prep.d_texmtx_only", 1);
    if (dirty & GX_DIRTY_PIXEL) PROF_COUNT("prep.d_pixel", 1);
    if (dirty & GX_DIRTY_LIGHT) PROF_COUNT("prep.d_light", 1);
    if (dirty & GX_DIRTY_PROJ) PROF_COUNT("prep.d_proj", 1);
    if (dirty & GX_DIRTY_POSMTX) PROF_COUNT("prep.d_posmtx", 1);
    if (dirty == GX_DIRTY_POSMTX) PROF_COUNT("prep.d_posmtx_only", 1);
#endif
    // Only register/konst colors changed (S3D_PREP_FAST bit 1): refresh the
    // constant colors of the applied program. Everything else still stands,
    // unless a second pass or water split copied the combiners
    if (dirty & GX_DIRTY_KCOL) {
        PROF_COUNT("prep.kcol", 1);
        if (dirty & (GX_DIRTY_TEV | GX_DIRTY_TEX | GX_DIRTY_TEXMTX)) PROF_COUNT("prep.kcol_with_tev_tex", 1);
        if (s_mp_on || s_ws_on) PROF_COUNT("prep.kcol_mp_ws", 1);
        if ((s_prep_fast & 1) && !(dirty & (GX_DIRTY_TEV | GX_DIRTY_TEX | GX_DIRTY_TEXMTX)) && !s_mp_on &&
            !s_ws_on && gx_tev_repatch()) {
            dirty &= ~GX_DIRTY_KCOL;
        } else {
            dirty |= GX_DIRTY_TEV;
        }
    }
    // A water split replaced the combiners: any change recompiles them
    if (s_ws_on) dirty |= GX_DIRTY_TEV;
    // Bug 32: a projected added term splits off into a second pass
    // (gx_tev_set_proj_split) only where multipass_prepare draws one, opaque
    // depth-writing draws. When that changes, the unit map is rebuilt
    {
        static int split_on = -1;  // S3D_REFLECT_SPLIT
        CFG_LIVE(split_on, cfg_bool("S3D_REFLECT_SPLIT", true) ? 1 : 0);
        const GxPixelState* px = &g_gx.pixel;
        bool allow = split_on && s_mp_enabled && !s_dbg_idcolor && px->blend_mode == GX_BM_NONE && px->z_enable &&
                     px->z_update && px->color_update;
        if (allow != gx_tev_proj_split_allowed()) {
            gx_tev_set_proj_split(allow);
            dirty |= GX_DIRTY_TEV;
        }
    }

    if (dirty & GX_DIRTY_PROJ) {
        GX_PROF_ZONE("gx.prep.proj");
        upload_projection();
        apply_scissor();
        apply_depth_map();
    }
    if (dirty & GX_DIRTY_POSMTX) {
        GX_PROF_ZONE("gx.prep.posmtx");
        upload_posmtx();
    }
    if (dirty & GX_DIRTY_TEV) gx_tev_touched();
    if ((dirty & (GX_DIRTY_TEV | GX_DIRTY_TEX)) == 0 && (dirty & GX_DIRTY_TEXMTX)) {
        // Only matrix values changed (texgen changes also mark TEV): the
        // unit map, textures, TEV program, fog and light env all stand
        GX_PROF_ZONE("gx.prep.texmtx");
        upload_texmtx();
        dirty &= ~GX_DIRTY_TEXMTX;
    }
    if (dirty & (GX_DIRTY_TEV | GX_DIRTY_TEX | GX_DIRTY_TEXMTX)) {
        GX_PROF_ZONE("gx.prep.tev");
        // The unit map reads only TEV and texgen state, whose changes all mark
        // TEV (texgens too), so a texture-only change keeps it. Exception: the
        // attract movie, recognised by its textures (gx_tev_movie_dependent)
        if ((dirty & GX_DIRTY_TEV) || !(s_rt_dedup & 1) || gx_tev_movie_dependent()) {
#if GX_PROFILE_DETAIL
            GxUnitMap before = s_map;
#endif
            {
                GX_PROF_ZONE("gx.tev.unitmap");
                gx_tev_build_unit_map(&s_map);
            }
#if GX_PROFILE_DETAIL
            // bind_textures fills the swizzles later: compare the rest
            memcpy(before.unit_swizzle, s_map.unit_swizzle, sizeof(before.unit_swizzle));
            if (memcmp(&s_map, &before, sizeof(s_map)) == 0) PROF_COUNT("prep.unitmap_same", 1);
#endif
        } else {
            GX_PROF_COUNT("prep.unitmap_kept", 1);
        }
        {
            GX_PROF_ZONE("gx.prep.bindtex");
            bind_textures();
            update_drop_avg();
        }
        {
            GX_PROF_ZONE("gx.tev.texmtx");
            upload_texmtx();
        }
        {
            GX_PROF_ZONE("gx.tev.apply");
            s_tev_nout = gx_tev_apply(&s_map);
        }
        int ch = gx_tev_ras_channel();
        if (ch != s_light_chan) {
            s_light_chan = ch;
            dirty |= GX_DIRTY_LIGHT;
        }
    }
    if (dirty & (GX_DIRTY_TEV | GX_DIRTY_TEX | GX_DIRTY_TEXMTX | GX_DIRTY_LIGHT)) {
        bool fs = s_frag_light && gx_tev_reads_ch1_color() && s_light_chan == 0 && g_gx.num_chans > 1 &&
                  g_gx.chan[1].enable && g_gx.chan[1].light_mask;
        if (fs != s_frag_spec) {
            s_frag_spec = fs;
            gx_tev_set_frag_secondary(fs);
            s_tev_nout = gx_tev_apply(&s_map);
            dirty |= GX_DIRTY_TEV | GX_DIRTY_LIGHT;
        }
    }
    if (dirty & GX_DIRTY_PIXEL) {
        GX_PROF_ZONE("gx.prep.pixel");
        apply_pixel_state();
    }
    // Texture-only change that kept the combiner program (S3D_PREP_FAST bit 4):
    // the fog stage after it, the fog mode and the light environment stand
    bool env_kept = (s_prep_fast & 4) && !(dirty & (GX_DIRTY_TEV | GX_DIRTY_PIXEL | GX_DIRTY_LIGHT)) &&
                    (dirty & (GX_DIRTY_TEX | GX_DIRTY_TEXMTX)) && gx_tev_apply_kept() && !s_mp_on && !s_ws_on;
    if (env_kept) PROF_COUNT("prep.env_kept", 1);
    if (!env_kept && (dirty & (GX_DIRTY_TEV | GX_DIRTY_TEX | GX_DIRTY_TEXMTX | GX_DIRTY_PIXEL))) {
        GX_PROF_ZONE("gx.prep.fog");
        apply_fog(&g_gx.pixel);
    }
    if (dirty & GX_DIRTY_LIGHT) {
        GX_PROF_ZONE("gx.prep.light");
        upload_lighting();
    }
    if (dirty & (GX_DIRTY_TEV | GX_DIRTY_TEX | GX_DIRTY_TEXMTX | GX_DIRTY_PIXEL | GX_DIRTY_LIGHT)) {
        GX_PROF_ZONE("gx.prep.lightenv");
        if (!env_kept) apply_light_env((dirty & GX_DIRTY_LIGHT) != 0);
        // A new texture can still start or end a second pass or water split
        multipass_prepare();
        water_split_prepare();
    }
    gx_shader_select(state_vsh());
    if (s_dbg_idcolor) {
        // Index bits 0-3 -> red, 4-7 -> green, 8-9 -> blue, in steps of 17 (85 for blue)
        u32 i = s_state_index;
        u32 color = ((i & 15) * 17) | ((((i >> 4) & 15) * 17) << 8) | ((((i >> 8) & 3) * 85) << 16);
        C3D_LightEnvBind(NULL);
        C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
        C3D_TexEnv* env = C3D_GetTexEnv(0);
        C3D_TexEnvInit(env);
        C3D_TexEnvSrc(env, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvColor(env, color | 0xFF000000u);
        for (int st = 1; st < GX_PICA_TEV_STAGES; st++) C3D_TexEnvInit(C3D_GetTexEnv(st));
        gx_tev_hw_invalidate();
        // Opaque output so the screenshot shows the exact index color
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
    }
    if (gx_capture_replaces()) capture_override();

    if (s_dumping) {
        static char buf[1024];
        gx_tev_describe(buf, sizeof(buf));
        // Log lines are limited to 512 bytes
        int len = (int)strlen(buf);
        LOG_INFO("dump", "state #%lu: %.400s", (unsigned long)s_state_index, buf);
        for (int at = 400; at < len; at += 400) LOG_INFO("dump", "  ...%.400s", buf + at);
        gx_tev_dump_program(&s_map);
        LOG_INFO("dump", "  proj %s %.4g %.4g %.4g %.4g %.4g %.4g vp %.0f %.0f %.0f %.0f %.2f %.2f",
                 g_gx.proj_type == GX_PERSPECTIVE ? "persp" : "ortho", g_gx.proj[0], g_gx.proj[1], g_gx.proj[2],
                 g_gx.proj[3], g_gx.proj[4], g_gx.proj[5], g_gx.viewport[0], g_gx.viewport[1], g_gx.viewport[2],
                 g_gx.viewport[3], g_gx.viewport[4], g_gx.viewport[5]);
        for (int u = 0; u < s_map.unit_count; u++) {
            const GxTexObjData* t = &g_gx.tex[s_map.unit_texmap[u]];
            const GxTexGen* tg = &g_gx.texgen[s_map.unit_texcoord[u]];
            LOG_INFO("dump", "  unit%d map%d tc%d: %ux%u fmt 0x%X wrap %u/%u texgen %u src %u mtx %u post %u%s%s swz %02X @%p",
                     u, s_map.unit_texmap[u], s_map.unit_texcoord[u], t->width, t->height, t->format, t->wrap_s,
                     t->wrap_t, tg->func, tg->src, tg->mtx, tg->post + GX_PTTEXMTX0, tg->normalize ? " norm" : "",
                     gx_copytex_find(t->data) ? " COPY" : "", s_map.unit_swizzle[u], t->data);
            // What the GPU samples: PICA format, mip levels, bias, address
            const C3D_Tex* ct = __C3D_Context.tex[u];
            // S3D_CAP_TEXDUMP=N: the capture also saves the textures of every
            // state with at least N GX stages (default 7: character eyes)
            static int cap_texdump = -1;
            if (cap_texdump < 0) cap_texdump = cfg_int("S3D_CAP_TEXDUMP", 7);
            if (ct && s_cap_dump && cap_texdump > 0 && atoi(buf) >= cap_texdump) dump_tex_raw(ct, s_state_index, u);
            if (ct) {
                u32 pa = osConvertVirtToPhys(ct->data);
                LOG_INFO("dump", "    pica %ux%u fmt %d levels %u-%u bias %d pa %08lX", ct->width, ct->height, (int)ct->fmt,
                         ct->minLevel, ct->maxLevel, (int)(s16)(ct->lodBias << 3) >> 3, (unsigned long)pa);
                // Level 0 channel ranges of RGBA8 textures (PICA stores ABGR bytes)
                if (ct->fmt == GPU_RGBA8 && ct->data) {
                    const u8* px = (const u8*)ct->data;
                    u8 lo[4] = { 255, 255, 255, 255 }, hi[4] = { 0, 0, 0, 0 };
                    u32 sum[4] = { 0, 0, 0, 0 }, n = (u32)ct->width * ct->height;
                    for (u32 i = 0; i < n; i++)
                        for (int c = 0; c < 4; c++) {
                            u8 v8 = px[i * 4 + 3 - c];  // c: r g b a
                            if (v8 < lo[c]) lo[c] = v8;
                            if (v8 > hi[c]) hi[c] = v8;
                            sum[c] += v8;
                        }
                    LOG_INFO("dump", "    texels r %u..%u (%lu) g %u..%u (%lu) b %u..%u (%lu) a %u..%u (%lu)", lo[0], hi[0],
                             (unsigned long)(sum[0] / n), lo[1], hi[1], (unsigned long)(sum[1] / n), lo[2], hi[2],
                             (unsigned long)(sum[2] / n), lo[3], hi[3], (unsigned long)(sum[3] / n));
                }
            }
            if (tg->post != GX_PTIDENTITY - GX_PTTEXMTX0) {
                const float (*p)[4] = &g_gx.xf_pt[tg->post];
                LOG_INFO("dump", "    post %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g", p[0][0], p[0][1],
                         p[0][2], p[0][3], p[1][0], p[1][1], p[1][2], p[1][3], p[2][0], p[2][1], p[2][2], p[2][3]);
            }
        }
        LOG_INFO("dump", "  vd tex0 %u tex1 %u tex2 %u", g_gx.vtx_desc[GX_VA_TEX0], g_gx.vtx_desc[GX_VA_TEX1],
                 g_gx.vtx_desc[GX_VA_TEX2]);
        // Vertex colour source (bug 36): CLR0 type per vertex format, array and its first bytes
        if (g_gx.vtx_desc[GX_VA_CLR0] != GX_NONE) {
            const GxArray* ca = &g_gx.arrays[GX_VA_CLR0];
            const u8* cb = ca->data;
            LOG_INFO("dump", "  clr0 desc %u types %u%u%u%u%u%u%u%u array %p stride %u le %u: %02X%02X %02X%02X %02X%02X %02X%02X",
                     g_gx.vtx_desc[GX_VA_CLR0], g_gx.vtx_fmt[0][GX_VA_CLR0].type, g_gx.vtx_fmt[1][GX_VA_CLR0].type,
                     g_gx.vtx_fmt[2][GX_VA_CLR0].type, g_gx.vtx_fmt[3][GX_VA_CLR0].type, g_gx.vtx_fmt[4][GX_VA_CLR0].type,
                     g_gx.vtx_fmt[5][GX_VA_CLR0].type, g_gx.vtx_fmt[6][GX_VA_CLR0].type, g_gx.vtx_fmt[7][GX_VA_CLR0].type,
                     cb, ca->stride, ca->le, cb ? cb[0] : 0, cb ? cb[1] : 0, cb ? cb[2] : 0, cb ? cb[3] : 0,
                     cb ? cb[4] : 0, cb ? cb[5] : 0, cb ? cb[6] : 0, cb ? cb[7] : 0);
            // Whole array, 32 entries per line (2-byte colours only)
            if (cb && ca->stride == 2) {
                u32 n = ca->size / 2 < 256 ? ca->size / 2 : 256;
                for (u32 i0 = 0; i0 < n; i0 += 32) {
                    char hex[32 * 5 + 1];
                    int k = 0;
                    for (u32 i = i0; i < n && i < i0 + 32; i++) k += snprintf(hex + k, sizeof(hex) - k, "%02X%02X ", cb[i * 2], cb[i * 2 + 1]);
                    LOG_INFO("dump", "    clr0[%lu..] %s", (unsigned long)i0, hex);
                }
            }
        }
        const GxPixelState* px = &g_gx.pixel;
        const GxChannel* c0 = &g_gx.chan[0];
        LOG_INFO("dump", "  blend %u %u/%u z %u/%u/%u acmp %u %u %u %u %u chans %u en %u/%u src m%u/%u a%u mat %02X%02X%02X%02X amb %02X%02X%02X%02X mask %02X",
                 px->blend_mode, px->blend_src, px->blend_dst, px->z_enable, px->z_func, px->z_update,
                 px->alpha_comp0, px->alpha_ref0, px->alpha_op, px->alpha_comp1, px->alpha_ref1, g_gx.num_chans,
                 c0->enable, g_gx.chan[2].enable, c0->mat_src, g_gx.chan[2].mat_src, c0->amb_src,
                 g_gx.chan_mat[0].r, g_gx.chan_mat[0].g, g_gx.chan_mat[0].b, g_gx.chan_mat[0].a,
                 g_gx.chan_amb[0].r, g_gx.chan_amb[0].g, g_gx.chan_amb[0].b, g_gx.chan_amb[0].a, c0->light_mask);
        if (g_gx.num_chans > 0 && (c0->enable || g_gx.chan[2].enable)) {
            const GxChannel* a0 = &g_gx.chan[2];
            LOG_INFO("dump", "  light c mask %02X df %u af %u | a mask %02X df %u af %u", c0->light_mask, c0->diff_fn,
                     c0->attn_fn, a0->light_mask, a0->diff_fn, a0->attn_fn);
            u8 mask = (c0->enable ? c0->light_mask : 0) | (a0->enable ? a0->light_mask : 0);
            for (int i = 0; i < GX_NUM_LIGHTS; i++) {
                if (!(mask & (1 << i))) continue;
                const GxLight* l = &g_gx.lights[i];
                LOG_INFO("dump", "  light%d col %02X%02X%02X%02X pos %.3g %.3g %.3g dir %.3g %.3g %.3g a %.3g %.3g %.3g k %.3g %.3g %.3g",
                         i, l->color.r, l->color.g, l->color.b, l->color.a, l->pos[0], l->pos[1], l->pos[2], l->dir[0],
                         l->dir[1], l->dir[2], l->a[0], l->a[1], l->a[2], l->k[0], l->k[1], l->k[2]);
            }
        }
    }

    // ID colors keep TEV/pixel dirty so every draw gets its own index
    g_gx.dirty = s_dbg_idcolor || gx_capture_replaces() ? (GX_DIRTY_TEV | GX_DIRTY_PIXEL) : 0;
    if (s_census_on) census_state();
    s_state_index++;
    PROF_COUNT("gx.state_changes", 1);
    return &s_map;
}

// Shadow caster passes whose texture is reused this frame (S3D_SHADOW_REFRESH,
// patches/main/newshadows.c.sed) still run on the game thread: the first render
// of a model in a frame computes its joint matrices and later renders reuse
// them (bufferFlags 8), and the game relies on the shadow pass being first (the
// staff exploded in cutscenes without it). Their draws are dropped here
void gx_draw_discard(bool on) {
    if (on == s_discard) return;
    gx_dlbatch_flush();  // Held instances belong to the side they were called on
    gx_render_flush();
    s_discard = on;
}

// Clear an EFB rectangle to the copy clear color/depth (GXCopyTex with clear),
// honoring the color/alpha/depth update flags like the GX does. Drawn as a
// quad with its own GPU state; everything is re-applied on the next draw.
void gx_render_clear_efb_rect(u32 left, u32 top, u32 wd, u32 ht) {
    const GxPixelState* px = &g_gx.pixel;
    GPU_WRITEMASK mask = 0;
    if (px->color_update) mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
    if (px->alpha_update) mask |= GPU_WRITE_ALPHA;
    if (px->z_update) mask |= GPU_WRITE_DEPTH;
    if (!mask || !wd || !ht) return;

    gx_render_flush();
    u16* idx;
    u16 base;
    GxOutVertex* v = gx_render_alloc_vertices(4, 6, &idx, &base);
    if (!v) return;

    // Screen clip coordinates of the rectangle
    float hw = 0.5f * g_scr_w;
    float x0 = (g_efb_x0 + left * g_efb_xs) / hw - 1.0f, x1 = (g_efb_x0 + (left + wd) * g_efb_xs) / hw - 1.0f;
    float y0 = 1.0f - (g_efb_y0 + top * g_efb_ys) / 120.0f, y1 = 1.0f - (g_efb_y0 + (top + ht) * g_efb_ys) / 120.0f;
    // PICA depth = 1 - GX depth = -z (w = 1); keep inside the clip volume
    float z = (float)(g_gx.clear_z & 0xFFFFFF) / 16777215.0f - 1.0f;
    if (z > -1.0f / 65536.0f) z = -1.0f / 65536.0f;
    GXColor c = g_gx.clear_color;
    memset(v, 0, 4 * sizeof(*v));
    float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    for (int i = 0; i < 4; i++) {
        v[i].pos[0] = xs[i]; v[i].pos[1] = ys[i]; v[i].pos[2] = z;
        v[i].color[0] = c.r; v[i].color[1] = c.g; v[i].color[2] = c.b; v[i].color[3] = c.a;
    }
    idx[0] = base; idx[1] = base + 1; idx[2] = base + 2;
    idx[3] = base; idx[4] = base + 2; idx[5] = base + 3;

    // Transform: matrix 0 = identity, projection = screen rotation only
    for (int r = 0; r < 3; r++)
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_posmtx + r, r == 0, r == 1, r == 2, 0.0f);
    C3D_Mtx m;
    Mtx_Identity(&m);
    m.r[0].x = s_tilt[0][0]; m.r[0].y = s_tilt[0][1];
    m.r[1].x = s_tilt[1][0]; m.r[1].y = s_tilt[1][1];
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_loc_projection, &m);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_lighting, false);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_fraglit, false);
    C3D_BoolUnifSet(GPU_VERTEX_SHADER, s_loc_nrmout, false);
    gx_shader_select(GX_VSH_UNLIT);

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    gx_tev_hw_invalidate();

    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_DepthTest(true, GPU_ALWAYS, mask);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthMap(true, -1.0f, 0.0f);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
    C3D_LightEnvBind(NULL);

    s_raw_draw = true;  // Both eyes in stereo
    gx_render_flush();
    s_raw_draw = false;
    PROF_COUNT("gx.copy_clears", 1);

    // Re-apply the game's state on the next draw
    g_gx.dirty = GX_DIRTY_ALL;
    g_gx.posmtx_rows |= 7;
}
