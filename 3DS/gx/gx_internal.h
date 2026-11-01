#ifndef GX_INTERNAL_H
#define GX_INTERNAL_H

// Internal state of the GX (GameCube graphics) emulation on top of citro3d.
//
//   gx_state.c   GX state setters (vertex format, matrices, pixel state...)
//   gx_vtx.c     vertex decoding: immediate mode, display lists
//   gx_render.c  batching and submission to citro3d
//   gx_tex.c     texture objects, format conversion, texture cache
//   gx_tev.c     TEV (texture environment) -> PICA200 combiners

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <dolphin/gx.h>
#include "profiler.h"
#include "watchdog.h"

// Fine-grained profiling zones in hot paths. Each zone costs two
// svcGetSystemTick calls (~1.4 us each under Azahar), so they are off by default.
// In normal builds a zone names the thread's phase (watchdog.h) for the rest
// of the block and then puts the old one back: two stores, no timer. The
// probe's sampler (S3D_PROBE bit 2, port/probe_3ds.c) adds up the render
// thread's time per phase, so the zones split its replay into steps
// (MULTICORE_PLAN.md, "Render-thread steps").
#ifndef GX_PROFILE_DETAIL
#define GX_PROFILE_DETAIL 0
#endif
#if GX_PROFILE_DETAIL
#define GX_PROF_ZONE(name) PROF_ZONE(name)
#define GX_PROF_COUNT(name, value) PROF_COUNT(name, value)
#else
static inline void gx_phase_restore_(volatile const char** prev) { g_main_phase = *prev; }
#define GX_PROF_ZONE(name) \
    volatile const char* PROF_CAT(gx_phase_prev_, __LINE__) __attribute__((cleanup(gx_phase_restore_))) = \
        g_main_phase; \
    g_main_phase = (name)
#define GX_PROF_COUNT(name, value) do { } while (0)
#endif

// PICA200 limits
#define GX_PICA_TEX_UNITS  3
#define GX_PICA_TEV_STAGES 6

#define GX_NUM_VTXFMT      8
#define GX_NUM_ATTR        (GX_VA_TEX7 + 1)  // PNMTXIDX..TEX7
#define GX_NUM_ARRAYS      (GX_LIGHT_ARRAY + 1)
#define GX_NUM_TEXCOORD    8
#define GX_NUM_TEXMAP      8
#define GX_NUM_LIGHTS      8

// XF memory: 64 rows of 4 floats for position/texture matrices, 32 rows of 3
// floats for normal matrices (row index = GX matrix id)
#define GX_XF_MTX_ROWS     64
#define GX_XF_NRM_ROWS     32
// Post-transform (dual texture) matrices: 64 rows of 4 floats at XF address
// 0x500 (row index = GX_PTTEXMTXn - GX_PTTEXMTX0)
#define GX_XF_PT_ROWS      64

typedef struct {
    u8 cnt;
    u8 type;
    u8 frac;
} GxAttrFmt;

typedef struct {
    const u8* data;
    u32 size;
    u8 stride;
    bool le;  // Data already little-endian (foxhollow extension)
} GxArray;

typedef struct {
    u8 func;  // GXTexGenType
    u8 src;   // GXTexGenSrc
    u8 mtx;   // GX texture matrix id (GX_TEXMTX0.. or GX_IDENTITY)
    u8 post;  // Post-transform matrix row (GX_PTTEXMTXn - GX_PTTEXMTX0)
    bool normalize;
} GxTexGen;

typedef struct {
    bool enable;
    u8 amb_src;
    u8 mat_src;
    u8 light_mask;
    u8 diff_fn;
    u8 attn_fn;
} GxChannel;

typedef struct {
    float pos[3];
    float dir[3];
    float a[3];  // Angle attenuation
    float k[3];  // Distance attenuation
    GXColor color;
} GxLight;

// What the game stores in a GXTexObj (16 words available on TARGET_PC)
typedef struct {
    const void* data;
    u16 width;
    u16 height;
    u8 format;
    u8 wrap_s;
    u8 wrap_t;
    u8 mipmap;
    u8 min_filter;
    u8 mag_filter;
    u8 valid;
    u8 pad;
    float max_lod;
    void* user_data;
    float min_lod;
    float lod_bias;
} GxTexObjData;

// Extra information about a bound texture (gx_tex_get)
typedef struct {
    float uscale, vscale;  // Part of the stored texture covered by the GX image
    u8 levels;             // Mip levels available
    bool pica;             // Converted at build time
} GxTexInfo;

// Pixel pipeline state
typedef struct {
    u8 blend_mode, blend_src, blend_dst, logic_op;
    bool z_enable, z_update;
    u8 z_func;
    bool color_update, alpha_update;
    u8 cull_mode;
    u8 alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    // Fog (GXSetFog); fog_type GX_FOG_NONE disables it
    u8 fog_type;
    float fog_start, fog_end, fog_near, fog_far;
    GXColor fog_color;
} GxPixelState;

// One GX TEV stage (gx_tev.c)
typedef struct {
    u8 color_in[4];
    u8 alpha_in[4];
    u8 color_op, alpha_op;
    u8 color_bias, alpha_bias;
    u8 color_scale, alpha_scale;
    u8 color_reg, alpha_reg;  // Output registers (GX_TEVPREV..GX_TEVREG2)
    u8 texcoord, texmap, channel;
    u8 kcsel, kasel;
    u8 ras_swap, tex_swap;    // Swap tables applied to rasterized / texture colors
} GxTevStage;

// TEV and indirect state (was file-local in gx_tev.c)
typedef struct {
    GxTevStage stages[GX_MAX_TEVSTAGE];
    GXColor regs[GX_MAX_TEVREG];       // Initial values of PREV, REG0..REG2
    GXColor kcolors[GX_MAX_KCOLOR];
    u8 swap_tables[GX_MAX_TEVSWAP][4]; // Source channel (GX_CH_*) of r, g, b, a
    u8 num_stages;
    // Indirect stand-ins (no PICA equivalent; the warp is per vertex)
    float stage_warp[GX_MAX_TEVSTAGE];
    u8 stage_ind[GX_MAX_TEVSTAGE], stage_bump_sel[GX_MAX_TEVSTAGE];
    u8 ind_coord[GX_MAX_INDTEXSTAGE], ind_map[GX_MAX_INDTEXSTAGE];
    u8 stage_ripple[GX_MAX_TEVSTAGE];
    u8 num_ind_stages;
} GxTevState;

enum {
    GX_DIRTY_POSMTX = 1 << 0,  // Position matrices / projection / viewport
    GX_DIRTY_TEXMTX = 1 << 1,  // Texture matrices or texgens
    GX_DIRTY_TEV    = 1 << 2,  // TEV stages, konst colors, tev order
    GX_DIRTY_TEX    = 1 << 3,  // Bound textures
    GX_DIRTY_PIXEL  = 1 << 4,  // Blend, depth, cull, alpha test, masks
    GX_DIRTY_LIGHT  = 1 << 5,  // Lighting channels, material/ambient colors, lights
    GX_DIRTY_PROJ   = 1 << 6,  // Projection or viewport
    GX_DIRTY_KCOL   = 1 << 7,  // TEV register / konst color values only (gx_tev_repatch)
    GX_DIRTY_ALL    = 0xFF,
};

typedef struct {
    // Vertex setup
    u8 vtx_desc[GX_NUM_ATTR];
    GxAttrFmt vtx_fmt[GX_NUM_VTXFMT][GX_NUM_ATTR];
    GxArray arrays[GX_NUM_ARRAYS];

    // Transform unit
    float xf_mtx[GX_XF_MTX_ROWS][4];
    float xf_nrm[GX_XF_NRM_ROWS][3];
    float xf_pt[GX_XF_PT_ROWS][4];
    u8 current_pn_mtx;  // Row index of the default position matrix
    float proj[6];
    u8 proj_type;
    float viewport[6];  // left, top, width, height, near, far (EFB pixels)
    u32 scissor[4];     // left, top, width, height (EFB pixels)

    u8 num_texgens;
    GxTexGen texgen[GX_NUM_TEXCOORD];

    u8 num_chans;
    GxChannel chan[4];  // COLOR0, COLOR1, ALPHA0, ALPHA1
    GXColor chan_mat[2];
    GXColor chan_amb[2];
    GxLight lights[GX_NUM_LIGHTS];

    GxPixelState pixel;
    GXColor clear_color;
    u32 clear_z;

    // Line and point rasterization (expanded to quads on the CPU)
    u8 line_width;   // 1/6 pixel units
    u8 point_size;   // 1/6 pixel units
    u8 line_tex_offset, point_tex_offset;  // GXTexOffset
    u16 tex_offset_enable;  // Bit per texcoord: lines in bits 0-7, points in bits 8-15

    GxTexObjData tex[GX_NUM_TEXMAP];  // Bound per texture map

    // Texture copy setup (gx_copy.c): no dirty bits, read at GXCopyTex
    u16 texcopy_src[4];  // left, top, width, height (EFB pixels)
    u8 texcopy_dst_fmt;

    GxTevState tev;  // TEV and indirect stage state (gx_tev.c)

    u32 dirty;
    u32 posmtx_rows;  // Position matrix rows (0-29) changed since the last upload
    u32 vtx_gen;  // Bumped on any vertex format/array/matrix index change
} GxState;

extern GxState g_gx;

// EFB x -> top screen x (pixels of the 400x240 screen): the 4:3 image is
// pillarboxed (40 + x/2); in the game's 16:9 mode it fills the screen
// (x * 400/640). Updated at frame start (gx_init.c). EFB y -> screen y is
// g_efb_y0 + y * g_efb_ys, and g_scr_w is the target screen's width (400, or
// 320 while the bottom screen HUD is drawn, gx_hud_begin).
extern float g_efb_x0, g_efb_xs, g_efb_y0, g_efb_ys, g_scr_w;
// Size of the framebuffer being drawn (memory columns x rows: 240x400, or
// 240x800 in wide mode, 480x800 with AA, 240x320 for the bottom screen).
// Screen pixel (x, y) covers framebuffer rows x * g_fb_h / g_scr_w and
// columns g_fb_w - y * g_fb_w / 240.
extern int g_fb_w, g_fb_h;
// S3D_RT_MEMO (MULTICORE_PLAN.md, "Render-thread memos"), read at the frame
// start (gx_render.c): 1 compact EFB-copy address index (gx_copy.c), 2
// texture lookup memo (gx_tex.c), 4 two-way TEV program cache (gx_tev.c),
// 8 touch menu text cache (port/touchui_3ds.c). Default 15
extern int g_rt_memo;

// gx_state.c
void gx_state_reset(void);
void gx_state_changed(u32 dirty_bits);  // Flushes the pending batch first
// Bumped by every gx_state_changed: a command that leaves it alone changed no
// render state (PROFILE=1 "unchanged" column of the rtops report)
extern u32 g_gx_change_seq;

// gx_rec_delta.c: producer-side state deltas (S3D_RT_DELTA). The game thread
// applies state setters to a shadow copy of g_gx and records one delta per
// draw instead of one command per setter; the replay applies the delta by
// copying the changed fields and marking the dirty bits, like the individual
// setters would have. Matrix loads are batched into one command instead.
bool gxrec_delta_on(void);       // Set at the frame start from S3D_RT_DELTA
int gxrec_delta_want(void);      // The raw S3D_RT_DELTA value (0 off, 1 on, 2 = on + verify)
bool gxrec_delta_switch_pending(void);  // S3D_RT_DELTA changed since the frame start
bool gxrec_delta_dl_recording(void);  // Inside GXBeginDisplayList (gx_rec.c)
bool gxrec_delta_verify(void);   // S3D_RT_DELTA value 2: check deltas against the real state (PROFILE=1)
// Recorder-side bookkeeping (gx_rec.c): vertex descriptors and arrays for the
// display-list hashes, the scissor for GXGetScissor
u8 gxrec_delta_vtx_desc(int attr);
const void* gxrec_delta_array_data(int attr);
u32 gxrec_delta_array_size(int attr);
void gxrec_delta_get_scissor(u32 out[4]);
// Recording side: flush the pending delta / matrix block before another
// command is recorded (gx_rec.c calls this from gxrec_begin and sync_all),
// apply a recorded delta or matrix block (replay side), refresh the shadow
// from the real state, and switch the mode at a frame start
void gxrec_delta_flush(void);
void gxrec_delta_apply(const u32* p, u32 words);
// Serializes the pending matrix block and state delta into a static buffer
// (the same bodies the MBLOCK/DELTA ops carry, without the op headers) and
// clears the pending state; returns the word counts. Single-threaded: the
// buffer stays valid until the next take or flush.
const u32* gxrec_delta_take(u32* mwords, u32* dwords);
// The material recorder (RENDERER_CHANNEL.md): snapshot/restore the shadow's
// material regions (the game-side fork stores the material per render-op)
void gxrec_delta_snapshot(GxState* out);
void gxrec_delta_restore(const GxState* in);
void gxrec_delta_mblk_apply(const u32* p, u32 words);
void gxrec_delta_refresh(void);
void gxrec_delta_frame_begin(void);
// A state setter applied to the shadow (returns false when not in delta mode:
// the caller records the command). All return whether they handled the call.
bool gxrec_delta_GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac);
bool gxrec_delta_GXSetVtxDesc(GXAttr attr, GXAttrType type);
bool gxrec_delta_GXClearVtxDesc(void);
bool gxrec_delta_GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool le);
bool gxrec_delta_GXSetScissor(u32 left, u32 top, u32 wd, u32 ht);
bool gxrec_delta_GXLoadPosMtxImm(const void* mtx, u32 id);
bool gxrec_delta_GXLoadNrmMtxImm(const void* mtx, u32 id);
bool gxrec_delta_GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type);
bool gxrec_delta_GXSetProjection(const void* mtx, GXProjectionType type);
bool gxrec_delta_GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz);
bool gxrec_delta_GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field);
bool gxrec_delta_GXSetNumTexGens(u8 nTexGens);
bool gxrec_delta_GXSetNumChans(u8 nChans);
bool gxrec_delta_GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask,
                               GXDiffuseFn diff_fn, GXAttnFn attn_fn);
bool gxrec_delta_GXSetChanAmbColor(GXChannelID chan, GXColor amb_color);
bool gxrec_delta_GXSetChanMatColor(GXChannelID chan, GXColor mat_color);
bool gxrec_delta_GXLoadLightObjImm(GXLightObj* lt_obj, GXLightID light);
bool gxrec_delta_GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op);
bool gxrec_delta_GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable);
bool gxrec_delta_GXSetColorUpdate(GXBool update_enable);
bool gxrec_delta_GXSetAlphaUpdate(GXBool update_enable);
bool gxrec_delta_GXSetCullMode(GXCullMode mode);
bool gxrec_delta_GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1);
bool gxrec_delta_GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color);
bool gxrec_delta_GXSetCopyClear(GXColor clear_clr, u32 clear_z);
bool gxrec_delta_GXEnableTexOffsets(GXTexCoordID coord, GXBool line_enable, GXBool point_enable);
bool gxrec_delta_GXSetLineWidth(u8 width, GXTexOffset texOffsets);
bool gxrec_delta_GXSetPointSize(u8 pointSize, GXTexOffset texOffsets);
bool gxrec_delta_GXSetNumTevStages(u8 nStages);
bool gxrec_delta_GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color);
bool gxrec_delta_GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c,
                                 GXTevColorArg d);
bool gxrec_delta_GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c,
                                 GXTevAlphaArg d);
bool gxrec_delta_GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                 GXTevRegID out_reg);
bool gxrec_delta_GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                 GXTevRegID out_reg);
bool gxrec_delta_GXSetTevColor(GXTevRegID id, GXColor color);
bool gxrec_delta_GXSetTevColorS10(GXTevRegID id, GXColorS10 color);
bool gxrec_delta_GXSetTevKColor(GXTevKColorID id, GXColor color);
bool gxrec_delta_GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel);
bool gxrec_delta_GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel);
bool gxrec_delta_GXSetTevOp(GXTevStageID id, GXTevMode mode);
bool gxrec_delta_GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel);
bool gxrec_delta_GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green,
                                       GXTevColorChan blue, GXTevColorChan alpha);
bool gxrec_delta_GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map);
bool gxrec_delta_GXSetNumIndStages(u8 nIndStages);
bool gxrec_delta_GXSetTevDirect(GXTevStageID tev_stage);
bool gxrec_delta_GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,
                                  GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s,
                                  GXIndTexWrap wrap_t, GXBool add_prev, GXBool ind_lod, GXIndTexAlphaSel alpha_sel);
bool gxrec_delta_port_tev_indirect(const char* func, GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                                   GXIndTexFormat format, GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel,
                                   GXIndTexWrap wrap_s, GXIndTexWrap wrap_t, GXBool add_prev, GXBool ind_lod,
                                   GXIndTexAlphaSel alpha_sel);
bool gxrec_delta_GXLoadTexObj(GXTexObj* obj, GXTexMapID id);
bool gxrec_delta_GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht);
bool gxrec_delta_GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap);

// gx_dlcache.c: display-list batching (S3D_DL_BATCH, DRAWCALL_PLAN.md "Plant
// batching"). During a replay, consecutive calls of the same cached list whose
// only change in between is position matrices are held back and drawn
// together through the matrix palette. Any other state change, draw or
// non-setter command draws the held instances first (gx_state_changed,
// gx_rec.c replay_until)
extern bool g_dlb_pending;
extern bool g_gxrec_replaying;  // gx_rec.c: inside replay_until (batching allowed)
void gx_dlbatch_flush(void);
void gxrec_dlb_flushed(void);  // gx_rec.c: counts what ended the batch
void gxrec_dlb_log(void);      // gx_rec.c: logs those counts every 600 frames

// gx_render.c
void gx_render_init(void);
void gx_render_deinit(void);
void gx_render_frame_begin(void);
void gx_render_set_stereo(bool on, float separation);  // Per frame, before gx_render_frame_begin
void gx_render_flush(void);
void gx_render_frame_end(void);  // Flushes CPU caches for this frame's vertex data
void gx_render_flush_cpu_caches(void);  // The cache part of gx_render_frame_end

// gx_init.c: S3D_GPU_TRACE (bug 12). Logs the operation, submits everything so
// far and waits for the GPU; no-op unless tracing this frame
bool gx_gpu_trace_on(void);
void gx_gpu_trace_before_split(void);  // S3D_GPU_DUMP in S3D_GPU_SYNC mode
int gx_gpu_sync(void);  // S3D_GPU_SYNC: quiet trace variants (bug 17)
void gx_gpu_sync_before_draw(void);  // S3D_GPU_SYNC bit 16 + S3D_GPU_SYNC_FLAGS
void gx_gpu_trace(const char* what, u32 a, u32 b, u32 c);
bool gx_gpu_trace_skip_draw(void);  // S3D_GPU_SKIP
bool gx_render_set_shader(shaderInstance_s* vsh);  // Looks up uniform locations

// gx_init.c: vertex shader variants, in default.shbin's DVLE order
// (GX_VSH_WATER: projective unit 0 for the water split, bug 28)
enum { GX_VSH_LIT, GX_VSH_UNLIT, GX_VSH_WARP, GX_VSH_WATER, GX_VSH_COUNT };
void gx_shader_select(int variant);

typedef struct {
    float pos[3];
    u8 color[4];
    u8 idx[4];       // [0] = position matrix row
    float tc[GX_PICA_TEX_UNITS][2];
    s16 nrm[4];      // x16384, [3] unused (padding)
} GxOutVertex;       // 52 bytes

// Cached list vertices when positions and texcoords convert to fixed point
// without loss (gx_dlcache.c pack_vertices); the shader scales them back
typedef struct {
    s16 pos[4];      // [3] unused
    u8 color[4];
    u8 idx[4];
    s16 tc[GX_PICA_TEX_UNITS][2];
    s16 nrm[4];      // x16384, [3] unused
} GxPackedVertex;    // 36 bytes

// Per-frame position/normal for partially cached (CPU-animated) lists
typedef struct {
    float pos[3];
    s16 nrm[4];
} GxDynVertex;  // 20 bytes

// Compact per-frame position/normal when the game's arrays hold 16-bit
// positions and 8-bit normals (read natively by the GPU, scaled in the shader)
typedef struct {
    s16 pos[4];
    s8 nrm[4];
} GxDyn16Vertex;  // 12 bytes

// How positions/normals of a run of recorded vertices were encoded
typedef struct {
    u32 vstart, count;    // Vertices in the recording
    u8 pos_desc, pos_cnt, pos_type, pos_frac;
    u8 nrm_desc, nrm_type;
} GxGatherRun;


// Texture coordinate source for each PICA texture slot, set up per batch
typedef struct {
    int unit_count;
    s8 unit_texmap[GX_PICA_TEX_UNITS];    // GX texmap sampled by each PICA unit
    s8 unit_texcoord[GX_PICA_TEX_UNITS];  // GX texcoord feeding each PICA unit
    s8 texmap_unit[GX_NUM_TEXMAP];        // Reverse mapping (first unit), -1 = not bound
    s8 stage_unit[GX_MAX_TEVSTAGE];       // Unit each TEV stage samples, -1 = none
    s8 stage_bump[GX_MAX_TEVSTAGE];       // Unit read as the stage's bump alpha (RASA), -1 = none
    u8 unit_swizzle[GX_PICA_TEX_UNITS];   // GX_SWZ channel remap (copied textures)
    float unit_warp[GX_PICA_TEX_UNITS];   // Indirect stand-in amplitude (texture units)
    s8 proj_split;                        // Texmap dropped for a projective second pass (bug 32), -1 = none
    s8 ripple_unit;                       // Unit carrying the water ripple's TEX0 coordinate (bug 35), -1 = none
    s8 avg_map;                           // Dropped texmap read as its average colour (bug 37), -1 = none
} GxUnitMap;

// Channel remapping of a texture in the combiners: color from one channel,
// alpha from one channel, optionally inverted (depth copies)
enum { GX_SWZ_RGB, GX_SWZ_R, GX_SWZ_G, GX_SWZ_B, GX_SWZ_A };
enum { GX_SWZ_A_A, GX_SWZ_A_R, GX_SWZ_A_G, GX_SWZ_A_B };
#define GX_SWZ(color, alpha, invert) ((u8)((color) | ((alpha) << 3) | ((invert) << 5)))
#define GX_SWZ_COLOR(s) ((s) & 7)
#define GX_SWZ_ALPHA(s) (((s) >> 3) & 3)
#define GX_SWZ_INVERT(s) (((s) >> 5) & 1)

// EFB copy textures (gx_copy.c). Texture coordinates: u = au . (s, t, q),
// v = av . (s, t, q) in homogeneous texgen output
typedef struct {
    const void* dest;
    C3D_Tex tex;
    C3D_Tex tex_r;     // Right eye (stereo frames only, valid when has_r)
    bool has_r;
    u32 r_frame;       // Frame tex_r was last copied
    float au[3], av[3];
    u8 swizzle;
    u16 src_w, src_h;  // EFB rectangle size
    u32 last_frame;
    float fb_scale;    // Framebuffer pixels per EFB pixel
    u8 blur_level;     // GPU blur (port_copytex_blur): mip level sampled, 0 = sharp
    u8 mip_levels;     // Mip levels allocated past level 0 (tex and tex_r; maxLevel is the bind's clamp)
    u32 mip_frame;     // Frame the mip chain was generated (current when == last_frame)
    u32 mip_frame_r;
    u8 scope_level;    // Mip level wanted by port_copytex_blur_scope (DOF), kept SCOPE_KEEP_FRAMES
    u32 scope_frame;   // Frame of the last scope on this copy
    u8 alias_level;    // Mip level a port_copytex_mip_alias reads, kept SCOPE_KEEP_FRAMES
    u32 alias_frame;   // Frame of the last alias refresh
} GxCopyTex;

const GxCopyTex* gx_copytex_find(const void* data);
extern u32 g_gx_copy_epoch;  // Bumped when a copy destination address changes
// Mip level to sample (GPU blur, or the alias level when data is a mip alias)
int gx_copytex_blur_level(const GxCopyTex* c, const void* data, bool right);
void gx_copy_frame_begin(void);
void gx_etc1_texel(const u8* blk, u32 x, u32 y, u8 rgb[3]);  // One texel of a PICA ETC1 block
void gx_copy_dump_all(void);  // Debug: write every copy texture to sdmc:/3ds/star3d/copyN.ppm
void gx_copy_dump_dir(const char* dir);  // Same, into dir
void gx_copy_frame_end(void);  // Queues the depth readback for GXPeekZ
void gx_render_clear_efb_rect(u32 left, u32 top, u32 wd, u32 ht);  // GX copy clear
bool gx_render_has_drawn(void);   // Draws issued since the last clear_drawn
void gx_render_clear_drawn(void);
bool gx_frame_is_active(void);

const GxUnitMap* gx_render_prepare(void);  // Applies dirty state, returns unit map
const GxUnitMap* gx_render_unit_map(void);  // The unit map as is (no state applied)
u32 gx_render_census_key(void);  // Material key of the last state census (S3D_STATE_CENSUS)
void gx_render_draw_cached(const void* owner, const GxOutVertex* verts, const u16* indices, u32 index_count);
void gx_render_draw_cached_packed(const void* owner, const GxPackedVertex* verts, const u16* indices, u32 index_count, float pos_scale,
                                  float tc_scale);
void gx_render_draw_cached_dynamic(const GxOutVertex* verts, const GxDynVertex* dyn, const u16* indices,
                                   u32 index_count);
void gx_render_draw_cached_dynamic16(const GxOutVertex* verts, const GxDyn16Vertex* dyn, const u16* indices,
                                     u32 index_count, float pos_scale);
void* gx_render_alloc_dynamic(u32 bytes);
u32 gx_render_frame_number(void);
bool gx_render_dumping(void);  // Capture ID pass / batch dump frame

// GPU fence (gx_init.c, PIPELINE_PLAN.md). With S3D_GPU_PIPELINE the GPU can
// still run frame F while the CPU builds F+1, so memory the GPU reads may only
// be freed or rewritten once its last frame is done. Frames are
// gx_render_frame_number values.
bool gx_gpu_frame_done(u32 frame);
void gx_gpu_free_linear(void* p, u32 last_used);         // linearFree once frame last_used is done
void gx_gpu_free_tex(const C3D_Tex* tex, u32 last_used);  // C3D_TexDelete once done (struct copied)
void gx_gpu_wait_idle(void);  // Wait for all submitted GPU work, then free what it released
bool gx_gpu_reclaim(void);    // Out of memory: wait for the GPU and free deferred memory (true if any)
bool gx_gpu_pipelined(void);  // The current frame overlaps the previous one on the GPU
bool gx_render_can_pipeline(void);  // Frame buffers are doubled (gx_render.c)
void gx_render_flush_new_writes(void);  // Mid-frame cache flush hook (gx_render.c)
void gx_gpu_wait_previous(void);  // Wait for the last submitted frame only
GxOutVertex* gx_render_alloc_vertices(u32 count, u32 index_count, u16** indices_out, u16* base_out);

// gx_tev.c
void gx_tev_reset(void);
// Amplitude of a known indirect site (S3D_WATER_WARP etc.), also used by the
// recorder's shadow (gx_rec_delta.c)
float gx_tev_warp_for_site(const char* func);
void gx_tev_build_unit_map(GxUnitMap* map);
// Whether gx_tev_build_unit_map may split off a projected additive term (bug
// 32): the render side allows it for opaque depth-writing draws only
void gx_tev_set_proj_split(bool allow);
bool gx_tev_proj_split_allowed(void);
bool gx_tev_movie_dependent(void);  // The unit map depends on the bound textures (attract movie)
void gx_tev_set_ind_order(int ind_stage, int tex_coord, int tex_map);
// Returns the number of PICA combiner stages used (the rest pass through)
u8 gx_tev_apply(const GxUnitMap* map);
void gx_tev_touched(void);  // TEV state changed: the next gx_tev_apply recompiles or looks up
// Register/konst colors changed, nothing else: refresh the constant colors of
// the program applied last. False when the full gx_tev_apply is needed
bool gx_tev_repatch(void);
// The last gx_tev_apply kept the applied program (texture-only change): the
// combiners, including the fog stage after them, were not rewritten
bool gx_tev_apply_kept(void);
// Combiner buffer masks of the last gx_tev_apply (color, alpha)
void gx_tev_last_buf(u8* buf_mask);
// Combiner writes that skip stages equal to what citro3d already holds;
// gx_tev_hw_invalidate after writing combiners any other way
void gx_tev_hw_write(const C3D_TexEnv* env, const u8* buf_mask);  // all stages + buffer masks (color, alpha)
void gx_tev_hw_stage(int i, const C3D_TexEnv* env);
void gx_tev_hw_invalidate(void);

// Second pass drawing the term of a texture dropped for lack of units (4.1)
enum { GX_MULTIPASS_ADD = 1, GX_MULTIPASS_MUL = 2 };
typedef struct {
    u8 kind;                    // GX_MULTIPASS_*
    s8 texmap, texcoord, unit;  // Dropped texture, its coordinates, the unit pass 2 samples it on
    u8 nout, buf_mask[2];
    C3D_TexEnv env[GX_PICA_TEV_STAGES];
} GxMultipass;
bool gx_tev_drops_texture(const GxUnitMap* map);  // A texture stage has no unit
void gx_tev_set_drop_avg(const u8 rgba[4]);      // Average colour of map->avg_map's texture (bug 37)
// After gx_tev_apply: whether the draw needs a second pass, and its program.
// unit_ok has a bit for each unit pass 2 may rebind (0: not allowed);
// force_unit >= 0 makes pass 2 sample on that unit (bug 32: projective unit 0)
bool gx_tev_multipass(const GxUnitMap* map, u8 unit_ok, int force_unit, GxMultipass* out);
// Screen-copy water mixing reflection (unit 0) and refraction + konst by
// the konst alpha: drawn as two projective passes (gx_render.c)
typedef struct {
    u32 konst;                    // Konst color, alpha = mix factor (0xAABBGGRR)
    s8 refr_unit, refr_texcoord;  // Refraction unit in the main program, its coordinates
} GxWaterSplit;
bool gx_tev_water_split(const GxUnitMap* map, GxWaterSplit* out);
int gx_tev_ind_coord(int ind_stage);  // Texcoord of an indirect stage (bug 35)
void gx_tev_describe(char* buf, int size);
void gx_tev_dump_program(const GxUnitMap* map);
// GX lighting channel the vertex color carries (0 or 1)
int gx_tev_ras_channel(void);
void gx_tev_set_frag_secondary(bool on);
bool gx_tev_reads_ch1_color(void);

// gx_tex.c
void gx_tex_init(void);
void gx_tex_deinit(void);
void gx_tex_frame_begin(void);
C3D_Tex* gx_tex_get(const GxTexObjData* obj, GxTexInfo* info);

// gx_dlcache.c: display list cache (GXCallDisplayList)
typedef struct DlRecorder DlRecorder;
void gx_dlcache_init(void);
// Display list cache hashes of game memory, also computed by the GX recorder
// on the game thread (gx_rec.c, gx_dl_call_hashed). Pure functions
u32 gx_dl_list_hash(const void* list, u32 nbytes);
u32 gx_dl_array_hash(const void* data, u32 size);                 // Content only
u32 gx_dl_array_key(u32 content_hash, const void* data, u32 size);  // Content + where
void gx_dlcache_deinit(void);
void gx_dlcache_frame_stats(void);
void gx_dlcache_frame_end(void);  // Flushes per-entry dynamic buffers
void gx_dlcache_prof_frame(void); // gx_rec.c: render-thread frame window for S3D_DLCALL_PROFILE
void gx_render_drawc_prof_on(void);   // gx_render.c: enable the draw-cached cost split
void gx_render_drawc_report(int frames);  // ... and report one window ("drawc" lines)
void gx_water_tess_invalidate(const void* owner);  // ... drop cached water tessellations of a DL entry
GxOutVertex* gx_dlrec_alloc(DlRecorder* rec, u32 count, u32 index_count, u16** indices_out, u16* base_out,
                            const GxGatherRun* run, u16** pos_idx_out, u16** nrm_idx_out);
void gx_dlrec_command(DlRecorder* rec, u32 offset, bool splits_draws);
void gx_dlrec_array(DlRecorder* rec, int attr);
void gx_dlrec_fail(DlRecorder* rec);  // The list cannot be cached (drawn live instead)

// gx_vtx.c
bool gx_vtx_run_display_list(const void* list, u32 nbytes, DlRecorder* rec);
void gx_vtx_exec_command(const void* list, u32 nbytes, u32 offset);
void gx_vtx_set_trace(bool on);
void gx_vtx_gather(const GxGatherRun* runs, u32 run_count, const u16* pos_idx, const u16* nrm_idx,
                   const GxOutVertex* recorded, GxDynVertex* out);
void gx_vtx_gather16(const u16* pos_idx, const u16* nrm_idx, u32 count, bool has_nrm, GxDyn16Vertex* out);
void gx_vtx_invalidate_layouts(void);
void gx_vtx_draw_stream(GXPrimitive prim, u8 vtxfmt, u16 count, const u8* data, u32 size, u32* consumed);

#endif // GX_INTERNAL_H
