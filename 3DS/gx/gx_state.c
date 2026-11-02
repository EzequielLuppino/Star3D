// GX state: vertex format, transform unit, lighting, pixel pipeline and the
// many GX calls that have no 3DS equivalent (FIFO, copy filters...).
//
// State that is baked into vertices at decode time (vertex format, current
// matrix index) does not end a batch. State read by the
// GPU at draw time (uploaded matrices, textures, TEV, blend/depth) flushes the
// pending batch first via gx_state_changed().

#include <math.h>
#include <string.h>
#include "gx_diag.h"
#include "gx_internal.h"
#include "log.h"
#include "profiler.h"

GxState g_gx;
u32 g_gx_change_seq;

#if GX_PROFILE_DETAIL
void gxrec_dd_check_change(void* where);
#endif

void gx_state_changed(u32 dirty_bits) {
    // Held display-list instances draw with the state they were called with;
    // position matrices are copied per instance, so loads keep them held
    if (g_dlb_pending && dirty_bits != GX_DIRTY_POSMTX) gx_dlbatch_flush();
    gx_render_flush();
    g_gx.dirty |= dirty_bits;
    g_gx_change_seq++;
#if GX_PROFILE_DETAIL
    gxrec_dd_check_change(__builtin_return_address(0));
#endif
}

static void set_identity_rows(float (*rows)[4], int n) {
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 4; j++) rows[i][j] = (i == j) ? 1.0f : 0.0f;
    }
}

void gx_state_reset(void) {
    u32 gen = g_gx.vtx_gen;  // Keep generations unique across resets
    memset(&g_gx, 0, sizeof(g_gx));
    g_gx.vtx_gen = gen + 1;
    gx_vtx_invalidate_layouts();

    for (int m = 0; m < GX_XF_MTX_ROWS; m += 3) set_identity_rows(&g_gx.xf_mtx[m], 3);
    for (int m = 0; m + 3 <= GX_XF_PT_ROWS; m += 3) set_identity_rows(&g_gx.xf_pt[m], 3);
    set_identity_rows(&g_gx.xf_pt[GX_PTIDENTITY - GX_PTTEXMTX0], 3);
    for (int m = 0; m < GX_XF_NRM_ROWS - 2; m += 3) {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) g_gx.xf_nrm[m + i][j] = (i == j) ? 1.0f : 0.0f;
    }
    g_gx.current_pn_mtx = GX_PNMTX0;

    // Identity orthographic projection, full 640x480 viewport
    g_gx.proj[0] = 1.0f;
    g_gx.proj[2] = 1.0f;
    g_gx.proj[4] = -1.0f;
    g_gx.proj_type = GX_ORTHOGRAPHIC;
    float vp[6] = { 0, 0, 640, 480, 0, 1 };
    memcpy(g_gx.viewport, vp, sizeof(vp));
    u32 sc[4] = { 0, 0, 640, 480 };
    memcpy(g_gx.scissor, sc, sizeof(sc));

    g_gx.num_texgens = 1;
    for (int i = 0; i < GX_NUM_TEXCOORD; i++) {
        g_gx.texgen[i].func = GX_TG_MTX2x4;
        g_gx.texgen[i].src = GX_TG_TEX0 + i;
        g_gx.texgen[i].mtx = GX_IDENTITY;
        g_gx.texgen[i].post = GX_PTIDENTITY - GX_PTTEXMTX0;
    }

    g_gx.num_chans = 0;
    for (int i = 0; i < 2; i++) {
        g_gx.chan_mat[i] = (GXColor){ 255, 255, 255, 255 };
        g_gx.chan_amb[i] = (GXColor){ 0, 0, 0, 0 };
    }
    for (int i = 0; i < 4; i++) {
        g_gx.chan[i].mat_src = GX_SRC_VTX;
    }

    GxPixelState* px = &g_gx.pixel;
    px->blend_mode = GX_BM_NONE;
    px->blend_src = GX_BL_ONE;
    px->blend_dst = GX_BL_ZERO;
    px->z_enable = true;
    px->z_update = true;
    px->z_func = GX_LEQUAL;
    px->color_update = true;
    px->alpha_update = true;
    px->fog_type = GX_FOG_NONE;
    g_gx.line_width = 6;
    g_gx.point_size = 6;
    px->cull_mode = GX_CULL_BACK;
    px->alpha_comp0 = GX_ALWAYS;
    px->alpha_comp1 = GX_ALWAYS;

    g_gx.clear_color = (GXColor){ 0, 0, 0, 255 };
    g_gx.clear_z = 0xFFFFFF;

    gx_tev_reset();
    g_gx.dirty = GX_DIRTY_ALL;
}

// --- Init / FIFO / copies (no 3DS equivalent) --------------------------------

static GXFifoObj s_fifo;

GXFifoObj* GXInit(void* base, u32 size) {
    (void)base;
    (void)size;
    gx_render_flush();
    gx_state_reset();
    LOG_INFO("gx", "GXInit: state reset");
    return &s_fifo;
}

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) { (void)fifo; (void)base; (void)size; }
void GXSetCPUFifo(GXFifoObj* fifo) { (void)fifo; }
void GXSetGPFifo(GXFifoObj* fifo) { (void)fifo; }
void GXGetFifoPtrs(GXFifoObj* fifo, void** readPtr, void** writePtr) {
    (void)fifo;
    if (readPtr) *readPtr = NULL;
    if (writePtr) *writePtr = NULL;
}
void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    if (overhi) *overhi = GX_FALSE;
    if (underlow) *underlow = GX_TRUE;
    if (readIdle) *readIdle = GX_TRUE;
    if (cmdIdle) *cmdIdle = GX_TRUE;
    if (brkpt) *brkpt = GX_FALSE;
}
void GXFlush(void) { gx_render_flush(); }
void GXPixModeSync(void) {}
void GXTexModeSync(void) {}
void GXInvalidateVtxCache(void) {}
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) { (void)left; (void)top; (void)wd; (void)ht; }
void GXSetDispCopyDst(u16 wd, u16 ht) { (void)wd; (void)ht; }
u32 GXSetDispCopyYScale(f32 vscale) { (void)vscale; return 480; }
void GXSetDispCopyGamma(GXGamma gamma) { (void)gamma; }
void GXSetCopyFilter(GXBool aa, u8 sample_pattern[12][2], GXBool vf, u8 vfilter[7]) {
    (void)aa; (void)sample_pattern; (void)vf; (void)vfilter;
}
void GXSetFieldMode(GXBool field_mode, GXBool half_aspect_ratio) { (void)field_mode; (void)half_aspect_ratio; }
void GXSetPixelFmt(GXPixelFmt pix_fmt, GXZFmt16 z_fmt) { (void)pix_fmt; (void)z_fmt; }
void GXSetDither(GXBool dither) { (void)dither; }
void GXSetZCompLoc(GXBool before_tex) { (void)before_tex; }
void GXEnableTexOffsets(GXTexCoordID coord, GXBool line_enable, GXBool point_enable) {
    if (coord >= GX_NUM_TEXCOORD) return;
    u16 bits = (u16)((1u << coord) | (1u << (coord + 8)));
    u16 set = (u16)((line_enable ? 1u << coord : 0) | (point_enable ? 1u << (coord + 8) : 0));
    g_gx.tex_offset_enable = (u16)((g_gx.tex_offset_enable & ~bits) | set);
}
// Lines and points are expanded to quads on the CPU (gx_vtx.c)
void GXSetLineWidth(u8 width, GXTexOffset texOffsets) {
    g_gx.line_width = width;
    g_gx.line_tex_offset = (u8)texOffsets;
}
void GXSetPointSize(u8 pointSize, GXTexOffset texOffsets) {
    g_gx.point_size = pointSize;
    g_gx.point_tex_offset = (u8)texOffsets;
}

void GXCopyDisp(void* dest, GXBool clear) {
    (void)dest;
    (void)clear;
    gx_render_flush();  // Frame is presented at VIWaitForRetrace
}

void GXSetCopyClear(GXColor clear_clr, u32 clear_z) {
    g_gx.clear_color = clear_clr;
    g_gx.clear_z = clear_z;
}

void GXDrawTorus(f32 rc, u8 numc, u8 numt) { (void)rc; (void)numc; (void)numt; }

// --- Vertex format -------------------------------------------------------------

// Texture matrix index attributes pick per-vertex normal texgens
// (gx_render.c nrm_pervertex): a change re-runs upload_texmtx
static void texmtxidx_changed(void) {
    gx_state_changed(GX_DIRTY_TEXMTX);
}

void GXClearVtxDesc(void) {
    gx_vtx_invalidate_layouts();
    for (int a = GX_VA_TEX0MTXIDX; a <= GX_VA_TEX7MTXIDX; a++)
        if (g_gx.vtx_desc[a] != GX_NONE) {
            texmtxidx_changed();
            break;
        }
    memset(g_gx.vtx_desc, GX_NONE, sizeof(g_gx.vtx_desc));
}

void GXSetVtxDesc(GXAttr attr, GXAttrType type) {
#if GX_PROFILE_DETAIL
    {
        GXAttr a = attr == GX_VA_NBT ? GX_VA_NRM : attr;
        if (a < GX_NUM_ATTR && g_gx.vtx_desc[a] == type) PROF_COUNT("gx.vtxdesc_same", 1);
    }
#endif
    gx_vtx_invalidate_layouts();
    if (attr < GX_NUM_ATTR) {
        if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX && g_gx.vtx_desc[attr] != type) texmtxidx_changed();
        g_gx.vtx_desc[attr] = type;
    } else if (attr == GX_VA_NBT) {
        g_gx.vtx_desc[GX_VA_NRM] = type;
    }
}

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    if (vtxfmt >= GX_NUM_VTXFMT) return;
    if (attr == GX_VA_NBT) attr = GX_VA_NRM;
    if (attr >= GX_NUM_ATTR) return;
    {
        const GxAttrFmt* f = &g_gx.vtx_fmt[vtxfmt][attr];
        if (f->cnt == cnt && f->type == type && f->frac == frac) GX_PROF_COUNT("gx.vtxfmt_same", 1);
    }
    gx_vtx_invalidate_layouts();
    g_gx.vtx_fmt[vtxfmt][attr].cnt = cnt;
    g_gx.vtx_fmt[vtxfmt][attr].type = type;
    g_gx.vtx_fmt[vtxfmt][attr].frac = frac;
}

void GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool le) {
    if (attr == GX_VA_NBT) attr = GX_VA_NRM;
    if (attr >= GX_NUM_ARRAYS) return;
    g_gx.vtx_gen++;
    g_gx.arrays[attr].data = data;
    g_gx.arrays[attr].size = size;
    g_gx.arrays[attr].stride = stride;
    g_gx.arrays[attr].le = le;
}

// --- Transform -----------------------------------------------------------------

u32 g_gx_posmtx_culls;

void GXLoadPosMtxImm(const void* mtx, u32 id) {
    if (id + 3 > GX_XF_MTX_ROWS) return;
    if (memcmp(g_gx.xf_mtx[id], mtx, 12 * sizeof(float)) == 0) return;
    const float* m = mtx;
    if (gx_diag_bad3(m, GX_DIAG_MTX_LIMIT) || gx_diag_bad3(m + 4, GX_DIAG_MTX_LIMIT) ||
        gx_diag_bad3(m + 8, GX_DIAG_MTX_LIMIT) || !(__builtin_fabsf(m[3]) + __builtin_fabsf(m[7]) + __builtin_fabsf(m[11]) < GX_DIAG_MTX_LIMIT)) {
        float t[3] = { m[3], m[7], m[11] };
        gx_diag_bad_value("posmtx", t, 3, id, __builtin_return_address(0));
    }
    gx_state_changed(GX_DIRTY_POSMTX);
    if (!(__builtin_fabsf(m[3]) + __builtin_fabsf(m[7]) + __builtin_fabsf(m[11]) < GX_POSMTX_CULL)) {
        // Far beyond any visible distance (runaway particles, NaN): a zero
        // matrix makes every triangle degenerate, so nothing is rasterized
        // instead of near-plane-clipped slivers across the screen
        memset(g_gx.xf_mtx[id], 0, 12 * sizeof(float));
        g_gx_posmtx_culls++;
    } else {
        memcpy(g_gx.xf_mtx[id], mtx, 12 * sizeof(float));
    }
    if (id < 30) g_gx.posmtx_rows |= 7u << id;
}

void GXLoadNrmMtxImm(const void* mtx, u32 id) {
    if (id + 3 > GX_XF_NRM_ROWS) return;
    // Stored for completeness; the vertex shader transforms normals with the
    // position matrix (exact for rotations and uniform scale), so no flush
    const float (*m)[4] = mtx;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) g_gx.xf_nrm[id + i][j] = m[i][j];
    }
}

void GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type) {
    const float* m = mtx;
    if (id >= GX_PTTEXMTX0) {
        // Post-transform matrices are always 3x4
        u32 row = id - GX_PTTEXMTX0;
        if (row + 3 > GX_XF_PT_ROWS || memcmp(g_gx.xf_pt[row], m, 12 * sizeof(float)) == 0) return;
        gx_state_changed(GX_DIRTY_TEXMTX);
        memcpy(g_gx.xf_pt[row], m, 12 * sizeof(float));
        return;
    }
    if (id >= GX_IDENTITY || id + 3 > GX_XF_MTX_ROWS) return;
    int rows = (type == GX_MTX2x4) ? 2 : 3;
    if (memcmp(g_gx.xf_mtx[id], m, rows * 4 * sizeof(float)) == 0 &&
        (rows == 3 || (g_gx.xf_mtx[id + 2][0] == 0.0f && g_gx.xf_mtx[id + 2][1] == 0.0f &&
                       g_gx.xf_mtx[id + 2][2] == 0.0f && g_gx.xf_mtx[id + 2][3] == 1.0f)))
        return;
    // Models load a texture matrix per joint (TEXMTX0-9, bug 27) but texgens
    // read only TEXMTX0 (no per-vertex TEXnMTXIDX here): a slot no texgen
    // reads changes nothing until a texgen points at it, which marks dirty
    bool read = false;
    for (int c = 0; c < GX_NUM_TEXCOORD; c++) read |= g_gx.texgen[c].mtx == id;
    if (read) gx_state_changed(GX_DIRTY_TEXMTX);
    else GX_PROF_COUNT("gx.texmtx_load_unread", 1);
    memcpy(g_gx.xf_mtx[id], m, rows * 4 * sizeof(float));
    if (rows == 2) {
        // A 2x4 matrix leaves q = 1
        g_gx.xf_mtx[id + 2][0] = 0.0f;
        g_gx.xf_mtx[id + 2][1] = 0.0f;
        g_gx.xf_mtx[id + 2][2] = 0.0f;
        g_gx.xf_mtx[id + 2][3] = 1.0f;
    }
}

void GXSetCurrentMtx(u32 id) {
    if (g_gx.current_pn_mtx != id) g_gx.vtx_gen++;
    g_gx.current_pn_mtx = (u8)id;  // Baked into each vertex
}

void GXSetProjection(const void* mtx, GXProjectionType type) {
    const float (*m)[4] = mtx;
    float p[6];
    p[0] = m[0][0];
    p[2] = m[1][1];
    p[4] = m[2][2];
    p[5] = m[2][3];
    if (type == GX_PERSPECTIVE) {
        p[1] = m[0][2];
        p[3] = m[1][2];
    } else {
        p[1] = m[0][3];
        p[3] = m[1][3];
    }
    // The game sets the same projection before many draws (~210 calls per hub frame)
    if (g_gx.proj_type == type && memcmp(g_gx.proj, p, sizeof(p)) == 0) return;
    gx_state_changed(GX_DIRTY_PROJ);
    g_gx.proj_type = (u8)type;
    memcpy(g_gx.proj, p, sizeof(p));
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    float vp[6] = { left, top, wd, ht, nearz, farz };
    if (memcmp(vp, g_gx.viewport, sizeof(vp)) == 0) return;
    gx_state_changed(GX_DIRTY_PROJ);
    memcpy(g_gx.viewport, vp, sizeof(vp));
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    (void)field;
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    u32 sc[4] = { left, top, wd, ht };
    if (memcmp(sc, g_gx.scissor, sizeof(sc)) == 0) return;
    gx_state_changed(GX_DIRTY_PROJ);
    memcpy(g_gx.scissor, sc, sizeof(sc));
}
void GXGetScissor(u32* left, u32* top, u32* wd, u32* ht) {
    if (left) *left = g_gx.scissor[0];
    if (top) *top = g_gx.scissor[1];
    if (wd) *wd = g_gx.scissor[2];
    if (ht) *ht = g_gx.scissor[3];
}

// --- Texture coordinate generation ------------------------------------------------

void GXSetNumTexGens(u8 nTexGens) {
    if (nTexGens == g_gx.num_texgens) return;
    gx_state_changed(GX_DIRTY_TEXMTX | GX_DIRTY_TEV);
    g_gx.num_texgens = nTexGens;
}

void GXSetTexCoordGen2(GXTexCoordID dst_coord, GXTexGenType func, GXTexGenSrc src_param, u32 mtx,
                       GXBool normalize, u32 postmtx) {
    if (dst_coord >= GX_NUM_TEXCOORD) return;
    GxTexGen* tg = &g_gx.texgen[dst_coord];
    u8 post = postmtx >= GX_PTTEXMTX0 && postmtx - GX_PTTEXMTX0 + 3 <= GX_XF_PT_ROWS
                  ? (u8)(postmtx - GX_PTTEXMTX0) : (u8)(GX_PTIDENTITY - GX_PTTEXMTX0);
    bool norm = normalize != 0;
    if (tg->func == func && tg->src == src_param && tg->mtx == mtx && tg->post == post && tg->normalize == norm)
        return;
    // TEV too: the texgen source feeds the unit map's multipass choice, so
    // TEXMTX alone means only matrix values changed (gx_render_prepare)
    gx_state_changed(GX_DIRTY_TEXMTX | GX_DIRTY_TEV);
    tg->func = func;
    tg->src = src_param;
    tg->mtx = (u8)mtx;
    tg->post = post;
    tg->normalize = norm;
    if (func != GX_TG_MTX2x4 && func != GX_TG_MTX3x4) {
        LOG_ONCE(LOG_LEVEL_WARN, "gx", "texgen type %d (bump/SRTG) not supported", func);
    }
}

// --- Lighting channels (evaluated in the vertex shader) -----------------------------

void GXSetNumChans(u8 nChans) {
    if (nChans == g_gx.num_chans) return;
    gx_state_changed(GX_DIRTY_LIGHT);
    g_gx.num_chans = nChans;
}

static void set_channel(int i, const GxChannel* c) {
    if (memcmp(&g_gx.chan[i], c, sizeof(*c)) == 0) return;
    gx_state_changed(GX_DIRTY_LIGHT);
    g_gx.chan[i] = *c;
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask,
                   GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    GxChannel c = { enable != 0, (u8)amb_src, (u8)mat_src, (u8)light_mask, (u8)diff_fn, (u8)attn_fn };
    switch (chan) {
        case GX_COLOR0:   set_channel(0, &c); break;
        case GX_COLOR1:   set_channel(1, &c); break;
        case GX_ALPHA0:   set_channel(2, &c); break;
        case GX_ALPHA1:   set_channel(3, &c); break;
        case GX_COLOR0A0: set_channel(0, &c); set_channel(2, &c); break;
        case GX_COLOR1A1: set_channel(1, &c); set_channel(3, &c); break;
        default: break;
    }
}

static int chan_color_index(GXChannelID chan) {
    switch (chan) {
        case GX_COLOR0: case GX_ALPHA0: case GX_COLOR0A0: return 0;
        case GX_COLOR1: case GX_ALPHA1: case GX_COLOR1A1: return 1;
        default: return -1;
    }
}

// Apply a color/alpha/both register update; marks lighting dirty if it changed
static void set_chan_color(GXColor* reg, GXChannelID chan, GXColor c, int index) {
    GXColor n = *reg;
    if (chan == GX_ALPHA0 || chan == GX_ALPHA1) {
        n.a = c.a;
    } else if (chan == GX_COLOR0 || chan == GX_COLOR1) {
        n.r = c.r; n.g = c.g; n.b = c.b;
    } else {
        n = c;
    }
    if (memcmp(reg, &n, sizeof(n)) == 0) return;
    gx_state_changed(GX_DIRTY_LIGHT);
    *reg = n;
}

void GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) {
    int i = chan_color_index(chan);
    if (i < 0) return;
    set_chan_color(&g_gx.chan_amb[i], chan, amb_color, i);
}

void GXSetChanMatColor(GXChannelID chan, GXColor mat_color) {
    int i = chan_color_index(chan);
    if (i < 0) return;
    set_chan_color(&g_gx.chan_mat[i], chan, mat_color, i);
}

// --- Light objects ------------------------------------------------------------------
// The game owns GXLightObj storage (64 bytes); a GxLight is stored inside it.

_Static_assert(sizeof(GxLight) <= sizeof(GXLightObj), "GxLight must fit in GXLightObj");

static GxLight* light_of(GXLightObj* obj) { return (GxLight*)obj; }

void GXInitLightPos(GXLightObj* lt_obj, f32 x, f32 y, f32 z) {
    GxLight* l = light_of(lt_obj);
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z;
}

void GXInitLightDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    GxLight* l = light_of(lt_obj);
    l->dir[0] = nx; l->dir[1] = ny; l->dir[2] = nz;
}

void GXInitSpecularDir(GXLightObj* lt_obj, f32 nx, f32 ny, f32 nz) {
    // Specular lights are placed "infinitely" far against the direction; the
    // light direction holds the half-angle vector with the view axis (0, 0, 1)
    GxLight* l = light_of(lt_obj);
    f32 hx = -nx, hy = -ny, hz = 1.0f - nz;
    f32 mag = sqrtf(hx * hx + hy * hy + hz * hz);
    mag = mag > 0.0f ? 1.0f / mag : 0.0f;
    l->pos[0] = -nx * 1048576.0f; l->pos[1] = -ny * 1048576.0f; l->pos[2] = -nz * 1048576.0f;
    l->dir[0] = hx * mag; l->dir[1] = hy * mag; l->dir[2] = hz * mag;
}

void GXInitLightColor(GXLightObj* lt_obj, GXColor color) { light_of(lt_obj)->color = color; }

void GXInitLightAttn(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    GxLight* l = light_of(lt_obj);
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}

void GXInitLightAttnA(GXLightObj* lt_obj, f32 a0, f32 a1, f32 a2) {
    GxLight* l = light_of(lt_obj);
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2;
}

void GXInitLightAttnK(GXLightObj* lt_obj, f32 k0, f32 k1, f32 k2) {
    GxLight* l = light_of(lt_obj);
    l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}

void GXGetLightAttnK(GXLightObj* lt_obj, f32* k0, f32* k1, f32* k2) {
    const GxLight* l = (const GxLight*)lt_obj;
    if (k0) *k0 = l->k[0];
    if (k1) *k1 = l->k[1];
    if (k2) *k2 = l->k[2];
}

// Same formulas as the Dolphin SDK
void GXInitLightSpot(GXLightObj* lt_obj, f32 cutoff, GXSpotFn spot_func) {
    float a0 = 1.0f, a1 = 0.0f, a2 = 0.0f;
    if (cutoff > 0.0f && cutoff <= 90.0f) {
        float cr = cosf(cutoff * 3.14159265f / 180.0f);
        float d;
        switch (spot_func) {
            case GX_SP_FLAT:  a0 = -1000.0f * cr; a1 = 1000.0f; a2 = 0.0f; break;
            case GX_SP_COS:   a0 = -cr / (1.0f - cr); a1 = 1.0f / (1.0f - cr); a2 = 0.0f; break;
            case GX_SP_COS2:  a0 = 0.0f; a1 = -cr / (1.0f - cr); a2 = 1.0f / (1.0f - cr); break;
            case GX_SP_SHARP: d = (1.0f - cr) * (1.0f - cr);
                              a0 = cr * (cr - 2.0f) / d; a1 = 2.0f / d; a2 = -1.0f / d; break;
            case GX_SP_RING1: d = (1.0f - cr) * (1.0f - cr);
                              a0 = -4.0f * cr / d; a1 = 4.0f * (1.0f + cr) / d; a2 = -4.0f / d; break;
            case GX_SP_RING2: d = (1.0f - cr) * (1.0f - cr);
                              a0 = 1.0f - 2.0f * cr * cr / d; a1 = 4.0f * cr / d; a2 = -2.0f / d; break;
            default: break;
        }
    }
    GXInitLightAttnA(lt_obj, a0, a1, a2);
}

void GXInitLightDistAttn(GXLightObj* lt_obj, f32 ref_dist, f32 ref_br, GXDistAttnFn dist_func) {
    float k0 = 1.0f, k1 = 0.0f, k2 = 0.0f;
    if (ref_dist >= 0.0f && ref_br > 0.0f && ref_br < 1.0f) {
        switch (dist_func) {
            case GX_DA_GENTLE: k1 = (1.0f - ref_br) / (ref_br * ref_dist); break;
            case GX_DA_MEDIUM: k1 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist);
                               k2 = 0.5f * (1.0f - ref_br) / (ref_br * ref_dist * ref_dist); break;
            case GX_DA_STEEP:  k2 = (1.0f - ref_br) / (ref_br * ref_dist * ref_dist); break;
            default: break;
        }
    }
    GXInitLightAttnK(lt_obj, k0, k1, k2);
}

void GXLoadLightObjImm(GXLightObj* lt_obj, GXLightID light) {
    // GXLightID is a bit mask (GX_LIGHT0 = 1)
    for (int i = 0; i < GX_NUM_LIGHTS; i++) {
        if (light & (1 << i)) {
            if (memcmp(&g_gx.lights[i], light_of(lt_obj), sizeof(GxLight)) == 0) return;
            gx_state_changed(GX_DIRTY_LIGHT);
            g_gx.lights[i] = *light_of(lt_obj);
            return;
        }
    }
}

// --- Pixel pipeline ---------------------------------------------------------------

void GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op) {
    GxPixelState* px = &g_gx.pixel;
    if (px->blend_mode == type && px->blend_src == src_factor && px->blend_dst == dst_factor && px->logic_op == op)
        return;
    gx_state_changed(GX_DIRTY_PIXEL);
    px->blend_mode = type;
    px->blend_src = src_factor;
    px->blend_dst = dst_factor;
    px->logic_op = op;
}

void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    GxPixelState* px = &g_gx.pixel;
    if (px->z_enable == (compare_enable != 0) && px->z_func == func && px->z_update == (update_enable != 0)) return;
    gx_state_changed(GX_DIRTY_PIXEL);
    px->z_enable = compare_enable != 0;
    px->z_func = func;
    px->z_update = update_enable != 0;
}

void GXSetColorUpdate(GXBool update_enable) {
    if (g_gx.pixel.color_update == (update_enable != 0)) return;
    gx_state_changed(GX_DIRTY_PIXEL);
    g_gx.pixel.color_update = update_enable != 0;
}

void GXSetAlphaUpdate(GXBool update_enable) {
    if (g_gx.pixel.alpha_update == (update_enable != 0)) return;
    gx_state_changed(GX_DIRTY_PIXEL);
    g_gx.pixel.alpha_update = update_enable != 0;
}

void GXSetCullMode(GXCullMode mode) {
    if (g_gx.pixel.cull_mode == mode) return;
    gx_state_changed(GX_DIRTY_PIXEL);
    g_gx.pixel.cull_mode = mode;
}

void GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    GxPixelState* px = &g_gx.pixel;
    if (px->alpha_comp0 == comp0 && px->alpha_ref0 == ref0 && px->alpha_op == op && px->alpha_comp1 == comp1 &&
        px->alpha_ref1 == ref1)
        return;
    gx_state_changed(GX_DIRTY_PIXEL);
    px->alpha_comp0 = comp0;
    px->alpha_ref0 = ref0;
    px->alpha_op = op;
    px->alpha_comp1 = comp1;
    px->alpha_ref1 = ref1;
}

void GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    GxPixelState* px = &g_gx.pixel;
    if (px->fog_type == type && px->fog_start == startz && px->fog_end == endz &&
        px->fog_near == nearz && px->fog_far == farz &&
        memcmp(&px->fog_color, &color, sizeof(color)) == 0) {
        return;
    }
    gx_state_changed(GX_DIRTY_PIXEL);
    px->fog_type = (u8)type;
    px->fog_start = startz;
    px->fog_end = endz;
    px->fog_near = nearz;
    px->fog_far = farz;
    px->fog_color = color;
}

// --- Indirect texturing (not supported by the PICA200) ------------------------------

void GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map) {
    gx_tev_set_ind_order(ind_stage, tex_coord, tex_map);  // Bump alpha (gx_tev.c)
}
void GXSetIndTexCoordScale(GXIndTexStageID indStage, GXIndTexScale scaleS, GXIndTexScale scaleT) {
    (void)indStage; (void)scaleS; (void)scaleT;
}
void GXSetIndTexMtx(GXIndTexMtxID mtx_sel, const void* offset, s8 scale_exp) {
    (void)mtx_sel; (void)offset; (void)scale_exp;
}
