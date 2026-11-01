// GX vertex pipeline: immediate mode (GXBegin/GXPosition.../GXEnd), display
// list recording (GXBeginDisplayList) and playback (GXCallDisplayList).
//
// Everything funnels into gx_vtx_draw_stream(), which decodes a GX vertex
// stream (the same big-endian byte layout the GameCube FIFO uses, with direct
// or indexed attributes) into PICA vertices, applies lighting on the CPU and
// emits indexed triangles into the current batch. Immediate-mode calls are
// encoded into that byte layout first, so all paths share one decoder.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gx_diag.h"
#include "gx_internal.h"
#include "gx_rec.h"
#include "log.h"
#include "profiler.h"

// --- Byte stream helpers -----------------------------------------------------

static inline u16 rd16(const u8* p, bool le) {
    return le ? (u16)(p[0] | (p[1] << 8)) : (u16)((p[0] << 8) | p[1]);
}

static inline u32 rd32(const u8* p, bool le) {
    return le ? (u32)(p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24))
              : (u32)(((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static inline float rdf32(const u8* p, bool le) {
    u32 bits = rd32(p, le);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static DlRecorder* s_rec;  // Non-NULL while a display list is recorded for the cache

// --- Lines and points ------------------------------------------------------------
//
// The PICA only rasterizes triangles. Points become screen-aligned squares and
// lines screen-space quads of the GX width (1/6 pixel units, in EFB pixels).
// The corners stay in the vertex's model space, offset along the inverse of
// the position matrix, so they go through the normal shader path.

#define LP_MAX_VERTS 1024
static GxOutVertex s_lp_verts[LP_MAX_VERTS];

static const float k_tex_offsets[6] = { 0.0f, 1.0f / 16, 1.0f / 8, 1.0f / 4, 1.0f / 2, 1.0f };

// Position of a decoded vertex in view space
static void lp_view_pos(const GxOutVertex* v, float out[3]) {
    const float(*m)[4] = &g_gx.xf_mtx[v->idx[0]];
    for (int r = 0; r < 3; r++) out[r] = m[r][0] * v->pos[0] + m[r][1] * v->pos[1] + m[r][2] * v->pos[2] + m[r][3];
}

// View-space offset of (px, py) EFB pixels at view position e; false if
// the point is behind the camera
static bool lp_pixels_to_view(const float e[3], float px, float py, float out[2]) {
    const float* p = g_gx.proj;
    float vw = g_gx.viewport[2], vh = g_gx.viewport[3];
    if (vw == 0.0f || vh == 0.0f || p[0] == 0.0f || p[2] == 0.0f) return false;
    float w = 1.0f;
    if (g_gx.proj_type == GX_PERSPECTIVE) {
        w = -e[2];
        if (w <= 1e-4f) return false;
    }
    out[0] = px * 2.0f / vw * w / p[0];
    out[1] = py * 2.0f / vh * w / p[2];
    return true;
}

// Model-space vector for a view-space (dx, dy, 0) offset. Uses the matrix
// rows as axes, which is exact for rotation plus per-axis scale.
static void lp_view_to_model(const GxOutVertex* v, float dx, float dy, float out[3]) {
    const float(*m)[4] = &g_gx.xf_mtx[v->idx[0]];
    float l0 = m[0][0] * m[0][0] + m[0][1] * m[0][1] + m[0][2] * m[0][2];
    float l1 = m[1][0] * m[1][0] + m[1][1] * m[1][1] + m[1][2] * m[1][2];
    float s0 = l0 > 1e-12f ? dx / l0 : 0.0f, s1 = l1 > 1e-12f ? dy / l1 : 0.0f;
    for (int c = 0; c < 3; c++) out[c] = m[0][c] * s0 + m[1][c] * s1;
}

// Corner texture offsets: enabled texcoords get (cs, ct) * offset added
static void lp_tex_offset(GxOutVertex* o, const GxUnitMap* map, u8 enable, float offset, float cs, float ct) {
    if (!enable || offset == 0.0f) return;
    for (int s = 0; s < map->unit_count; s++) {
        int coord = map->unit_texcoord[s];
        if (coord < 0 || coord >= GX_NUM_TEXCOORD || !(enable & (1u << coord))) continue;
        o->tc[s][0] += cs * offset;
        o->tc[s][1] += ct * offset;
    }
}

// Emits one quad: corners a - da, a + da, b + db, b - db
static bool lp_quad(const GxUnitMap* map, const GxOutVertex* a, const GxOutVertex* b, const float da[3],
                    const float db[3], u8 tex_enable, float tex_offset) {
    u16* idx;
    u16 base;
    GxOutVertex* o = gx_render_alloc_vertices(4, 6, &idx, &base);
    if (!o) return false;
    static const float corner_s[4] = { 0, 0, 1, 1 }, corner_t[4] = { 0, 1, 1, 0 };
    for (int i = 0; i < 4; i++) {
        const GxOutVertex* src = i < 2 ? a : b;
        const float* d = i < 2 ? da : db;
        float sign = (i == 0 || i == 3) ? -1.0f : 1.0f;
        o[i] = *src;
        for (int c = 0; c < 3; c++) o[i].pos[c] += sign * d[c];
        lp_tex_offset(&o[i], map, tex_enable, tex_offset, corner_s[i], corner_t[i]);
    }
    idx[0] = base; idx[1] = base + 1; idx[2] = base + 2;
    idx[3] = base; idx[4] = base + 2; idx[5] = base + 3;
    return true;
}

static void expand_lines_points(GXPrimitive prim, const GxUnitMap* map, const GxOutVertex* v, u32 count) {
    u32 quads = 0;
    if (prim == GX_POINTS) {
        float half = g_gx.point_size / 12.0f;  // Half the size in EFB pixels
        u8 en = (u8)(g_gx.tex_offset_enable >> 8);
        float toff = k_tex_offsets[g_gx.point_tex_offset < 6 ? g_gx.point_tex_offset : 0];
        for (u32 i = 0; i < count; i++) {
            float e[3], d[2], mx[3], my[3];
            lp_view_pos(&v[i], e);
            if (!lp_pixels_to_view(e, half, half, d)) continue;
            lp_view_to_model(&v[i], d[0], 0.0f, mx);
            lp_view_to_model(&v[i], 0.0f, d[1], my);
            // A square is a quad between the left and right edge centers
            GxOutVertex a = v[i], b = v[i];
            for (int c = 0; c < 3; c++) { a.pos[c] -= mx[c]; b.pos[c] += mx[c]; }
            if (!lp_quad(map, &a, &b, my, my, en, toff)) break;
            quads++;
        }
    } else {
        float half = g_gx.line_width / 12.0f;
        u8 en = (u8)(g_gx.tex_offset_enable & 0xFF);
        float toff = k_tex_offsets[g_gx.line_tex_offset < 6 ? g_gx.line_tex_offset : 0];
        const float* p = g_gx.proj;
        bool persp = g_gx.proj_type == GX_PERSPECTIVE;
        u32 step = prim == GX_LINES ? 2 : 1;
        for (u32 i = 0; i + 1 < count; i += step) {
            const GxOutVertex* a = &v[i];
            const GxOutVertex* b = &v[i + 1];
            float ea[3], eb[3];
            lp_view_pos(a, ea);
            lp_view_pos(b, eb);
            float wa = persp ? -ea[2] : 1.0f, wb = persp ? -eb[2] : 1.0f;
            if (wa <= 1e-4f || wb <= 1e-4f) continue;
            // Screen direction in EFB pixels
            float sx = (p[0] * eb[0] / wb - p[0] * ea[0] / wa) * g_gx.viewport[2] * 0.5f;
            float sy = (p[2] * eb[1] / wb - p[2] * ea[1] / wa) * g_gx.viewport[3] * 0.5f;
            float len = sqrtf(sx * sx + sy * sy);
            float nx, ny;
            if (len < 1e-6f) { nx = 0.0f; ny = half; }
            else { nx = -sy / len * half; ny = sx / len * half; }
            float da2[2], db2[2], da[3], db[3];
            if (!lp_pixels_to_view(ea, nx, ny, da2) || !lp_pixels_to_view(eb, nx, ny, db2)) continue;
            lp_view_to_model(a, da2[0], da2[1], da);
            lp_view_to_model(b, db2[0], db2[1], db);
            if (!lp_quad(map, a, b, da, db, en, toff)) break;
            quads++;
        }
    }
    PROF_COUNT("gx.lines_points_expanded", quads);
    PROF_COUNT("gx.vertices", quads * 4);
}

// --- Immediate mode / display list recording -------------------------------------

#define IMM_BUFFER_SIZE (256 * 1024)
static u8 s_imm_buf[IMM_BUFFER_SIZE];
static u32 s_imm_len;
static bool s_imm_active;
static GXPrimitive s_imm_prim;
static u8 s_imm_fmt;
static u16 s_imm_count;
static u32 s_imm_expected;  // Bytes of the primitive started by GXBegin

static int layout_size(u8 vtxfmt);
static void imm_submit(void);

// The emission (RENDERER_CHANNEL.md stage C): while recording, the game
// thread runs the immediate-stream decode into the frame batch instead of
// shipping the raw bytes to the render thread, so the map lookup stays pure
// (the render thread applies the state itself when it flushes the batch)
static bool s_game_decode;

static u8* s_dl_buf;
static u32 s_dl_size;
static u32 s_dl_len;
static bool s_dl_recording;
static bool s_dl_overflow;

static inline void emit(const void* data, u32 n) {
    if (s_dl_recording) {
        if (s_dl_len + n > s_dl_size) {
            s_dl_overflow = true;
            return;
        }
        memcpy(s_dl_buf + s_dl_len, data, n);
        s_dl_len += n;
    } else {
        if (s_imm_len + n > IMM_BUFFER_SIZE) {
            LOG_ONCE(LOG_LEVEL_ERROR, "gx", "immediate-mode vertex buffer overflow");
            return;
        }
        memcpy(s_imm_buf + s_imm_len, data, n);
        s_imm_len += n;
        // The GX draws as soon as the declared vertex data has arrived; the
        // game relies on that and never calls GXEnd (a no-op on the console)
        if (s_imm_active && s_imm_len >= s_imm_expected) imm_submit();
    }
}

static inline void emit_u8(u8 v) { emit(&v, 1); }
static inline void emit_u16(u16 v) { u8 b[2] = { v >> 8, v & 0xFF }; emit(b, 2); }
static inline void emit_u32(u32 v) { u8 b[4] = { v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF }; emit(b, 4); }
static inline void emit_f32(f32 f) { u32 v; memcpy(&v, &f, 4); emit_u32(v); }

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts) {
    if (s_dl_recording) {
        emit_u8((u8)type | (vtxfmt & 7));
        emit_u16(nverts);
        return;
    }
    s_imm_prim = type;
    s_imm_fmt = vtxfmt & 7;
    s_imm_count = nverts;
    s_imm_len = 0;
    s_imm_expected = (u32)layout_size(s_imm_fmt) * nverts;
    s_imm_active = s_imm_expected != 0;
}

static void imm_submit(void) {
    GX_PROF_ZONE("gx.immediate");
    s_imm_active = false;
    if (gxrec_recording()) {
        // The emission: decode on the game thread into the frame batch (the
        // render thread flushes it in order); the expand path and the DL
        // capture keep the raw-byte recording
        if (s_imm_prim == GX_LINES || s_imm_prim == GX_LINESTRIP || s_imm_prim == GX_POINTS) {
            gxrec_put_imm_expand(s_imm_prim, s_imm_fmt, s_imm_count, s_imm_buf, s_imm_len);
            return;
        }
        s_game_decode = true;
        gx_vtx_draw_stream(s_imm_prim, s_imm_fmt, s_imm_count, s_imm_buf, s_imm_len, NULL);
        s_game_decode = false;
        return;
    }
    gx_vtx_draw_stream(s_imm_prim, s_imm_fmt, s_imm_count, s_imm_buf, s_imm_len, NULL);
}

void GXEnd(void) {
    if (s_dl_recording || !s_imm_active) return;
    imm_submit();
}

// Replay of recorded immediate-mode vertex bytes (gx_rec.c): the same bytes
// the GXPosition* / GXColor* / ... calls below would have emitted
void gx_vtx_imm_bytes(const void* data, u32 n) { emit(data, n); }

void GXPosition3f32(f32 x, f32 y, f32 z) { emit_f32(x); emit_f32(y); emit_f32(z); }
void GXPosition3u16(u16 x, u16 y, u16 z) { emit_u16(x); emit_u16(y); emit_u16(z); }
void GXPosition3s16(s16 x, s16 y, s16 z) { emit_u16(x); emit_u16(y); emit_u16(z); }
void GXPosition3u8(u8 x, u8 y, u8 z) { emit_u8(x); emit_u8(y); emit_u8(z); }
void GXPosition3s8(s8 x, s8 y, s8 z) { emit_u8(x); emit_u8(y); emit_u8(z); }
void GXPosition2f32(f32 x, f32 y) { emit_f32(x); emit_f32(y); }
void GXPosition2u16(u16 x, u16 y) { emit_u16(x); emit_u16(y); }
void GXPosition2s16(s16 x, s16 y) { emit_u16(x); emit_u16(y); }
void GXPosition2u8(u8 x, u8 y) { emit_u8(x); emit_u8(y); }
void GXPosition2s8(s8 x, s8 y) { emit_u8(x); emit_u8(y); }
void GXPosition1x16(u16 index) { emit_u16(index); }
void GXPosition1x8(u8 index) { emit_u8(index); }
void GXNormal3f32(f32 x, f32 y, f32 z) { emit_f32(x); emit_f32(y); emit_f32(z); }
void GXNormal3s16(s16 x, s16 y, s16 z) { emit_u16(x); emit_u16(y); emit_u16(z); }
void GXNormal3s8(s8 x, s8 y, s8 z) { emit_u8(x); emit_u8(y); emit_u8(z); }
void GXNormal1x16(u16 index) { emit_u16(index); }
void GXNormal1x8(u8 index) { emit_u8(index); }
void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { emit_u8(r); emit_u8(g); emit_u8(b); emit_u8(a); }
void GXColor3u8(u8 r, u8 g, u8 b) { emit_u8(r); emit_u8(g); emit_u8(b); }
void GXColor1u32(u32 clr) { emit_u32(clr); }
void GXColor1u16(u16 clr) { emit_u16(clr); }
void GXColor1x16(u16 index) { emit_u16(index); }
void GXColor1x8(u8 index) { emit_u8(index); }
void GXTexCoord2f32(f32 s, f32 t) { emit_f32(s); emit_f32(t); }
void GXTexCoord2u16(u16 s, u16 t) { emit_u16(s); emit_u16(t); }
void GXTexCoord2s16(s16 s, s16 t) { emit_u16(s); emit_u16(t); }
void GXTexCoord2u8(u8 s, u8 t) { emit_u8(s); emit_u8(t); }
void GXTexCoord2s8(s8 s, s8 t) { emit_u8(s); emit_u8(t); }
void GXTexCoord1f32(f32 s) { emit_f32(s); }
void GXTexCoord1u16(u16 s) { emit_u16(s); }
void GXTexCoord1s16(s16 s) { emit_u16(s); }
void GXTexCoord1u8(u8 s) { emit_u8(s); }
void GXTexCoord1s8(s8 s) { emit_u8(s); }
void GXTexCoord1x16(u16 index) { emit_u16(index); }
void GXTexCoord1x8(u8 index) { emit_u8(index); }

void GXBeginDisplayList(void* list, u32 size) {
    s_dl_buf = list;
    s_dl_size = size;
    s_dl_len = 0;
    s_dl_recording = true;
    s_dl_overflow = false;
}

u32 GXEndDisplayList(void) {
    s_dl_recording = false;
    if (s_dl_overflow) {
        LOG_WARN("gx", "display list overflow (buffer 0x%lX bytes)", (unsigned long)s_dl_size);
        return 0;
    }
    // Pad with NOPs to a 32-byte multiple like the hardware FIFO
    while ((s_dl_len & 31) && s_dl_len < s_dl_size) s_dl_buf[s_dl_len++] = 0;
    return s_dl_len;
}

// --- Vertex decoding ---------------------------------------------------------------

typedef struct {
    u8 attr;
    u8 desc;
    u8 type;
    u8 ncomp;      // Components to read (direct data)
    u8 size;       // Bytes per attribute in the array/stream data
    float scale;   // Fixed-point scale (1 / 2^frac)
} AttrDecoder;

static int comp_size(u8 type) {
    switch (type) {
        case GX_U8: case GX_S8: return 1;
        case GX_U16: case GX_S16: return 2;
        default: return 4;
    }
}

static int color_size(u8 type) {
    switch (type) {
        case GX_RGB565: case GX_RGBA4: return 2;
        case GX_RGB8: case GX_RGBA6: return 3;
        default: return 4;
    }
}

// Build the attribute list for a vertex format; returns stream bytes/vertex
static int build_layout(u8 vtxfmt, AttrDecoder* out, int* count) {
    int n = 0, stream_size = 0;
    for (int a = 0; a < GX_NUM_ATTR; a++) {
        u8 desc = g_gx.vtx_desc[a];
        if (desc == GX_NONE) continue;

        const GxAttrFmt* f = &g_gx.vtx_fmt[vtxfmt][a];
        AttrDecoder* d = &out[n++];
        d->attr = a;
        d->desc = desc;
        d->type = f->type;
        d->scale = 1.0f;

        if (a <= GX_VA_TEX7MTXIDX) {
            d->ncomp = 1;
            d->size = 1;
        } else if (a == GX_VA_POS) {
            d->ncomp = f->cnt == GX_POS_XY ? 2 : 3;
            d->size = d->ncomp * comp_size(f->type);
            d->scale = 1.0f / (float)(1 << f->frac);
        } else if (a == GX_VA_NRM) {
            d->ncomp = 3;
            // NBT carries normal, binormal and tangent; only the normal is used
            int vectors = f->cnt == GX_NRM_XYZ ? 1 : 3;
            d->size = 3 * vectors * comp_size(f->type);
            d->scale = f->type == GX_S8 ? 1.0f / 64.0f : f->type == GX_S16 ? 1.0f / 16384.0f : 1.0f;
        } else if (a == GX_VA_CLR0 || a == GX_VA_CLR1) {
            d->ncomp = 1;
            d->size = color_size(f->type);
        } else {
            d->ncomp = f->cnt == GX_TEX_S ? 1 : 2;
            d->size = d->ncomp * comp_size(f->type);
            d->scale = 1.0f / (float)(1 << f->frac);
        }

        if (desc == GX_DIRECT) stream_size += d->size;
        else {
            // NBT3 normals use three indices (normal, binormal, tangent)
            int nidx = (a == GX_VA_NRM && f->cnt == GX_NRM_NBT3) ? 3 : 1;
            stream_size += nidx * (desc == GX_INDEX8 ? 1 : 2);
        }
    }
    *count = n;
    return stream_size;
}

static inline float read_comp(const u8* p, u8 type, bool le) {
    switch (type) {
        case GX_U8:  return (float)p[0];
        case GX_S8:  return (float)(s8)p[0];
        case GX_U16: return (float)rd16(p, le);
        case GX_S16: return (float)(s16)rd16(p, le);
        default:     return rdf32(p, le);
    }
}

static inline void read_color(const u8* p, u8 type, bool le, u8 out[4]) {
    switch (type) {
        case GX_RGB565: {
            u16 v = rd16(p, le);
            out[0] = ((v >> 11) & 0x1F) * 255 / 31;
            out[1] = ((v >> 5) & 0x3F) * 255 / 63;
            out[2] = (v & 0x1F) * 255 / 31;
            out[3] = 255;
            break;
        }
        case GX_RGB8:
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
            break;
        case GX_RGBX8:
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
            break;
        case GX_RGBA4: {
            u16 v = rd16(p, le);
            out[0] = ((v >> 12) & 0xF) * 17;
            out[1] = ((v >> 8) & 0xF) * 17;
            out[2] = ((v >> 4) & 0xF) * 17;
            out[3] = (v & 0xF) * 17;
            break;
        }
        case GX_RGBA6: {
            u32 v = ((u32)p[0] << 16) | (p[1] << 8) | p[2];
            out[0] = ((v >> 18) & 0x3F) * 255 / 63;
            out[1] = ((v >> 12) & 0x3F) * 255 / 63;
            out[2] = ((v >> 6) & 0x3F) * 255 / 63;
            out[3] = (v & 0x3F) * 255 / 63;
            break;
        }
        default:  // RGBA8 (byte order is the same in both endiannesses)
            out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
            break;
    }
}

typedef struct {
    float pos[3];
    float nrm[3];
    u8 clr[2][4];
    float tex[GX_NUM_TEXCOORD][2];
    u8 pn_idx;
    bool has_clr[2];
    bool has_nrm;
} DecodedVertex;

// --- Vertex color --------------------------------------------------------------------
// Lighting (material/ambient selection and the lights themselves) runs in the
// vertex shader; the CPU only forwards the vertex color (white if absent).

static inline void vertex_color(const DecodedVertex* v, u8 out[4]) {
    if (v->has_clr[0]) {
        memcpy(out, v->clr[0], 4);
    } else {
        out[0] = out[1] = out[2] = out[3] = 255;
    }
}

// --- Draw ---------------------------------------------------------------------------

static u32 index_count_for(GXPrimitive prim, u32 n) {
    switch (prim) {
        case GX_TRIANGLES:     return n / 3 * 3;
        case GX_TRIANGLESTRIP:
        case GX_TRIANGLEFAN:   return n >= 3 ? (n - 2) * 3 : 0;
        case GX_QUADS:         return n / 4 * 6;
        default:               return 0;
    }
}

static void emit_indices(GXPrimitive prim, u32 n, u16 base, u16* idx) {
    switch (prim) {
        case GX_TRIANGLES:
            for (u32 i = 0; i + 2 < n; i += 3) {
                *idx++ = base + i; *idx++ = base + i + 1; *idx++ = base + i + 2;
            }
            break;
        case GX_TRIANGLESTRIP:
            for (u32 i = 0; i + 2 < n; i++) {
                // Keep a consistent winding for every other triangle
                if (i & 1) { *idx++ = base + i + 1; *idx++ = base + i; }
                else       { *idx++ = base + i; *idx++ = base + i + 1; }
                *idx++ = base + i + 2;
            }
            break;
        case GX_TRIANGLEFAN:
            for (u32 i = 1; i + 1 < n; i++) {
                *idx++ = base; *idx++ = base + i; *idx++ = base + i + 1;
            }
            break;
        case GX_QUADS:
            for (u32 i = 0; i + 3 < n; i += 4) {
                *idx++ = base + i; *idx++ = base + i + 1; *idx++ = base + i + 2;
                *idx++ = base + i; *idx++ = base + i + 2; *idx++ = base + i + 3;
            }
            break;
        default:
            break;
    }
}

// Log the vertex descriptor/format and the bytes at a decode failure
static void dump_decode_state(const char* what, u8 vtxfmt, const u8* data, u32 avail) {
    char line[160];
    int n = 0;
    for (int a = 0; a < GX_NUM_ATTR && n < (int)sizeof(line) - 24; a++) {
        if (g_gx.vtx_desc[a] == GX_NONE) continue;
        const GxAttrFmt* f = &g_gx.vtx_fmt[vtxfmt][a];
        n += snprintf(line + n, sizeof(line) - n, " a%d:d%u/c%u/t%u/f%u", a, g_gx.vtx_desc[a], f->cnt, f->type, f->frac);
    }
    LOG_ERROR("gx", "%s: fmt %u desc%s", what, vtxfmt, line);
    n = 0;
    for (u32 i = 0; i < avail && i < 32; i++) n += snprintf(line + n, sizeof(line) - n, "%02x ", data[i]);
    LOG_ERROR("gx", "  bytes: %s", line);
}

// Decoded layouts per vertex format, rebuilt after vertex descriptor/format changes
static AttrDecoder s_layouts[GX_NUM_VTXFMT][GX_NUM_ATTR];
static int s_layout_count[GX_NUM_VTXFMT];
static int s_layout_size[GX_NUM_VTXFMT];
static u8 s_layout_valid;

void gx_vtx_invalidate_layouts(void) {
    s_layout_valid = 0;
    g_gx.vtx_gen++;  // Vertex state changed (display list cache keys)
}

static int layout_size(u8 vtxfmt) {
    if (!(s_layout_valid & (1 << vtxfmt))) {
        s_layout_size[vtxfmt] = build_layout(vtxfmt, s_layouts[vtxfmt], &s_layout_count[vtxfmt]);
        s_layout_valid |= 1 << vtxfmt;
    }
    return s_layout_size[vtxfmt];
}

void gx_vtx_draw_stream(GXPrimitive prim, u8 vtxfmt, u16 count, const u8* data, u32 size, u32* consumed) {
    layout_size(vtxfmt);
    const AttrDecoder* layout = s_layouts[vtxfmt];
    int nattr = s_layout_count[vtxfmt];
    int vsize = s_layout_size[vtxfmt];
    u32 total = (u32)vsize * count;
    if (consumed) *consumed = total;

    if (total > size) {
        static int reported;
        if (reported++ < 3) {
            LOG_ERROR("gx", "vertex stream overrun: %u verts x %d bytes > %lu bytes", count, vsize, (unsigned long)size);
            dump_decode_state("overrun", vtxfmt, data, size);
        }
        // consumed stays at the full size so the caller sees the overrun
        return;
    }

    u32 nidx = index_count_for(prim, count);
    bool expand = prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS;
    if (expand) {
        // Expanded to quads from the current matrices, so not cacheable
        if (s_rec) {
            gx_dlrec_fail(s_rec);
            return;
        }
        if (count > LP_MAX_VERTS) {
            LOG_ONCE(LOG_LEVEL_WARN, "gx", "line/point primitive with %u vertices truncated", count);
            PROF_COUNT("gx.lines_points_skipped", count - LP_MAX_VERTS);
            count = LP_MAX_VERTS;
        }
    } else if (nidx == 0) {
        return;
    }

    const GxUnitMap* map = s_game_decode ? gx_render_unit_map() : gx_render_prepare();
    u16* indices;
    u16 base;
    GxOutVertex* out;
    u16* pos_idx = NULL;
    u16* nrm_idx = NULL;
    if (s_rec) {
        GxGatherRun run = { 0, count, g_gx.vtx_desc[GX_VA_POS], g_gx.vtx_fmt[vtxfmt][GX_VA_POS].cnt,
                            g_gx.vtx_fmt[vtxfmt][GX_VA_POS].type, g_gx.vtx_fmt[vtxfmt][GX_VA_POS].frac,
                            g_gx.vtx_desc[GX_VA_NRM], g_gx.vtx_fmt[vtxfmt][GX_VA_NRM].type };
        out = gx_dlrec_alloc(s_rec, count, nidx, &indices, &base, &run, &pos_idx, &nrm_idx);
        for (int ai = 0; ai < nattr; ai++) {
            if (layout[ai].desc != GX_DIRECT && layout[ai].attr >= GX_VA_POS) gx_dlrec_array(s_rec, layout[ai].attr);
        }
    } else if (expand) {
        out = s_lp_verts;
        indices = NULL;
        base = 0;
    } else {
        out = gx_render_alloc_vertices(count, nidx, &indices, &base);
    }
    if (!out) return;

    const u8* p = data;
    // Dump frames: CLR0 index and alpha ranges of this draw (bug 36)
    bool dump_clr = gx_render_dumping() && g_gx.vtx_desc[GX_VA_CLR0] != GX_NONE;
    u32 clr_ilo = 0xFFFF, clr_ihi = 0;
    u8 clr_alo = 255, clr_ahi = 0;
    for (u32 vi = 0; vi < count; vi++) {
        DecodedVertex v;
        v.pos[0] = v.pos[1] = v.pos[2] = 0.0f;
        v.nrm[0] = v.nrm[1] = 0.0f;
        v.nrm[2] = 1.0f;
        v.has_clr[0] = v.has_clr[1] = false;
        v.has_nrm = false;
        if (map->unit_count) memset(v.tex, 0, sizeof(v.tex));
        v.pn_idx = g_gx.current_pn_mtx;

        for (int ai = 0; ai < nattr; ai++) {
            const AttrDecoder* d = &layout[ai];
            const u8* src;
            bool le = false;

            if (d->desc == GX_DIRECT) {
                src = p;
                p += d->size;
            } else {
                u32 index = d->desc == GX_INDEX8 ? p[0] : rd16(p, false);
                p += d->desc == GX_INDEX8 ? 1 : 2;
                // Recording: remember where positions/normals came from so
                // animated meshes can refresh just those each frame
                if (pos_idx && d->attr == GX_VA_POS) pos_idx[vi] = (u16)index;
                if (nrm_idx && d->attr == GX_VA_NRM) nrm_idx[vi] = (u16)index;
                if (dump_clr && d->attr == GX_VA_CLR0) {
                    if (index < clr_ilo) clr_ilo = index;
                    if (index > clr_ihi) clr_ihi = index;
                }
                const GxArray* arr = &g_gx.arrays[d->attr];
                u32 offset = index * arr->stride;
                if (!arr->data || offset + d->size > arr->size) {
                    PROF_COUNT("gx.bad_index", 1);
                    continue;
                }
                src = arr->data + offset;
                le = arr->le;
            }

            switch (d->attr) {
                case GX_VA_PNMTXIDX:
                    v.pn_idx = src[0] < 30 ? src[0] : 0;
                    break;
                case GX_VA_TEX0MTXIDX: case GX_VA_TEX1MTXIDX: case GX_VA_TEX2MTXIDX: case GX_VA_TEX3MTXIDX:
                case GX_VA_TEX4MTXIDX: case GX_VA_TEX5MTXIDX: case GX_VA_TEX6MTXIDX: case GX_VA_TEX7MTXIDX:
                    // Not stored: gx_render.c upload_unit_texmtx assumes the vertex's texture
                    // matrix is TEXMTX(n) for position matrix PNMTX(n) (models: renderOpMatrix
                    // loads them in pairs, bug 27). Count whether that holds
                    if (src[0] == v.pn_idx + GX_TEXMTX0) {
                        PROF_COUNT("gx.texmtxidx_paired", 1);
                    } else {
                        PROF_COUNT("gx.texmtxidx_other", 1);
                        LOG_ONCE(LOG_LEVEL_INFO, "gx", "TEX%dMTXIDX %u with PNMTXIDX %u (not paired)",
                                 d->attr - GX_VA_TEX0MTXIDX, src[0], v.pn_idx);
                    }
                    break;
                case GX_VA_POS: {
                    int cs = comp_size(d->type);
                    for (int c = 0; c < d->ncomp; c++) v.pos[c] = read_comp(src + c * cs, d->type, le) * d->scale;
                    break;
                }
                case GX_VA_NRM: {
                    int cs = comp_size(d->type);
                    for (int c = 0; c < 3; c++) v.nrm[c] = read_comp(src + c * cs, d->type, le) * d->scale;
                    v.has_nrm = true;
                    break;
                }
                case GX_VA_CLR0:
                case GX_VA_CLR1: {
                    int ci = d->attr - GX_VA_CLR0;
                    read_color(src, d->type, le, v.clr[ci]);
                    v.has_clr[ci] = true;
                    break;
                }
                default:
                    if (d->attr >= GX_VA_TEX0) {
                        int t = d->attr - GX_VA_TEX0;
                        int cs = comp_size(d->type);
                        for (int c = 0; c < d->ncomp; c++) v.tex[t][c] = read_comp(src + c * cs, d->type, le) * d->scale;
                    }
                    break;  // TEXnMTXIDX: per-vertex texture matrices not supported
            }
        }

        GxOutVertex* o = &out[vi];
        o->pos[0] = v.pos[0];
        o->pos[1] = v.pos[1];
        o->pos[2] = v.pos[2];
        if (gx_diag_bad3(v.pos, GX_DIAG_POS_LIMIT))
            gx_diag_bad_value(s_rec ? "dl-vtx" : "imm-vtx", v.pos, 3, vtxfmt, __builtin_return_address(0));
        o->idx[0] = v.pn_idx < 30 ? v.pn_idx : 0;
        o->idx[1] = o->idx[2] = o->idx[3] = 0;
        vertex_color(&v, o->color);
        if (dump_clr) {
            if (o->color[3] < clr_alo) clr_alo = o->color[3];
            if (o->color[3] > clr_ahi) clr_ahi = o->color[3];
        }
        // Normals as 16-bit fixed point (x16384); the shader renormalizes
        for (int c = 0; c < 3; c++) {
            float n = v.nrm[c] * 16384.0f;
            o->nrm[c] = (s16)(n > 32767.0f ? 32767.0f : n < -32768.0f ? -32768.0f : n);
        }
        o->nrm[3] = 0;

        // Texture coordinate inputs; position/normal sources are read by the
        // shader from the position/normal attributes instead (see texsrc)
        for (int s = 0; s < map->unit_count; s++) {
            int coord = map->unit_texcoord[s];
            float* tc = o->tc[s];
            tc[0] = tc[1] = 0.0f;
            if (coord < 0 || coord >= g_gx.num_texgens) continue;

            u8 src = g_gx.texgen[coord].src;
            if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) {
                tc[0] = v.tex[src - GX_TG_TEX0][0];
                tc[1] = v.tex[src - GX_TG_TEX0][1];
            } else if (src >= GX_TG_TEXCOORD0 && src <= GX_TG_TEXCOORD6) {
                tc[0] = v.tex[src - GX_TG_TEXCOORD0][0];
                tc[1] = v.tex[src - GX_TG_TEXCOORD0][1];
            } else if (src == GX_TG_COLOR0) {
                tc[0] = o->color[0] / 255.0f;
                tc[1] = o->color[1] / 255.0f;
            }
        }
    }

    if (dump_clr && count) {
        const GxAttrFmt* cf = &g_gx.vtx_fmt[vtxfmt][GX_VA_CLR0];
        LOG_INFO("dump", "    vtx fmt %u clr0 desc %u cnt %u type %u idx %lu..%lu alpha %u..%u (%lu verts, %s)", vtxfmt,
                 g_gx.vtx_desc[GX_VA_CLR0], cf->cnt, cf->type, (unsigned long)clr_ilo, (unsigned long)clr_ihi, clr_alo,
                 clr_ahi, (unsigned long)count, s_rec ? "recording" : "live");
        char lay[160], raw[100];
        int k = 0;
        for (int ai = 0; ai < nattr && k < 150; ai++)
            k += snprintf(lay + k, sizeof(lay) - k, " a%u/d%u/s%u", layout[ai].attr, layout[ai].desc, layout[ai].size);
        u32 vb = (u32)(p - data) / count;  // Stream bytes per vertex
        k = 0;
        for (u32 i = 0; i < vb * 3 && i < (u32)(p - data) && k < 96; i++) k += snprintf(raw + k, sizeof(raw) - k, "%02X", data[i]);
        LOG_INFO("dump", "      layout%s | %lu B/vtx: %s", lay, (unsigned long)vb, raw);
    }
    if (expand) {
        expand_lines_points(prim, map, out, count);
        return;
    }
    emit_indices(prim, count, base, indices);
    PROF_COUNT("gx.vertices", count);
}

// --- Display lists --------------------------------------------------------------------

#define GX_CMD_NOP          0x00
#define GX_CMD_LOAD_CP      0x08
#define GX_CMD_LOAD_XF      0x10
#define GX_CMD_LOAD_INDX_A  0x20
#define GX_CMD_LOAD_INDX_D  0x38
#define GX_CMD_CALL_DL      0x40
#define GX_CMD_INVAL_VTX    0x48
#define GX_CMD_LOAD_BP      0x60

static inline u32 bits(u32 value, int count, int shift) {
    return (value >> shift) & ((1u << count) - 1);
}

static void set_tex_mtx_index(int coord, u32 id) {
    if (g_gx.texgen[coord].mtx == id) return;
    gx_state_changed(GX_DIRTY_TEXMTX);
    g_gx.texgen[coord].mtx = (u8)id;
}

// CP registers written by display lists: vertex descriptor, vertex attribute
// formats, array strides and the matrix index registers
static void handle_cp(u8 addr, u32 value) {
    u8* vd = g_gx.vtx_desc;
    if (addr >= 0x50) gx_vtx_invalidate_layouts();
    if (addr == 0x30) {
        if (g_gx.current_pn_mtx != bits(value, 6, 0)) g_gx.vtx_gen++;
        g_gx.current_pn_mtx = (u8)bits(value, 6, 0);
        for (int i = 0; i < 4; i++) set_tex_mtx_index(i, bits(value, 6, 6 + i * 6));
    } else if (addr == 0x40) {
        for (int i = 0; i < 4; i++) set_tex_mtx_index(4 + i, bits(value, 6, i * 6));
    } else if (addr == 0x50) {
        for (int i = 0; i < 9; i++) {
            // Texture matrix indices pick per-vertex normal texgens (gx_render.c nrm_pervertex)
            if (i && vd[GX_VA_PNMTXIDX + i] != bits(value, 1, i)) gx_state_changed(GX_DIRTY_TEXMTX);
            vd[GX_VA_PNMTXIDX + i] = (u8)bits(value, 1, i);
        }
        vd[GX_VA_POS] = (u8)bits(value, 2, 9);
        vd[GX_VA_NRM] = (u8)bits(value, 2, 11);
        vd[GX_VA_CLR0] = (u8)bits(value, 2, 13);
        vd[GX_VA_CLR1] = (u8)bits(value, 2, 15);
    } else if (addr == 0x60) {
        for (int i = 0; i < 8; i++) vd[GX_VA_TEX0 + i] = (u8)bits(value, 2, i * 2);
    } else if (addr >= 0x70 && addr <= 0x77) {
        GxAttrFmt* f = g_gx.vtx_fmt[addr - 0x70];
        f[GX_VA_POS] = (GxAttrFmt){ (u8)bits(value, 1, 0), (u8)bits(value, 3, 1), (u8)bits(value, 5, 4) };
        u8 nrm_cnt = bits(value, 1, 31) ? GX_NRM_NBT3 : bits(value, 1, 9) ? GX_NRM_NBT : GX_NRM_XYZ;
        f[GX_VA_NRM] = (GxAttrFmt){ nrm_cnt, (u8)bits(value, 3, 10), 0 };
        f[GX_VA_CLR0] = (GxAttrFmt){ (u8)bits(value, 1, 13), (u8)bits(value, 3, 14), 0 };
        f[GX_VA_CLR1] = (GxAttrFmt){ (u8)bits(value, 1, 17), (u8)bits(value, 3, 18), 0 };
        f[GX_VA_TEX0] = (GxAttrFmt){ (u8)bits(value, 1, 21), (u8)bits(value, 3, 22), (u8)bits(value, 5, 25) };
    } else if (addr >= 0x80 && addr <= 0x87) {
        GxAttrFmt* f = g_gx.vtx_fmt[addr - 0x80];
        f[GX_VA_TEX1] = (GxAttrFmt){ (u8)bits(value, 1, 0), (u8)bits(value, 3, 1), (u8)bits(value, 5, 4) };
        f[GX_VA_TEX2] = (GxAttrFmt){ (u8)bits(value, 1, 9), (u8)bits(value, 3, 10), (u8)bits(value, 5, 13) };
        f[GX_VA_TEX3] = (GxAttrFmt){ (u8)bits(value, 1, 18), (u8)bits(value, 3, 19), (u8)bits(value, 5, 22) };
        f[GX_VA_TEX4].cnt = (u8)bits(value, 1, 27);
        f[GX_VA_TEX4].type = (u8)bits(value, 3, 28);
    } else if (addr >= 0x90 && addr <= 0x97) {
        GxAttrFmt* f = g_gx.vtx_fmt[addr - 0x90];
        f[GX_VA_TEX4].frac = (u8)bits(value, 5, 0);
        f[GX_VA_TEX5] = (GxAttrFmt){ (u8)bits(value, 1, 5), (u8)bits(value, 3, 6), (u8)bits(value, 5, 9) };
        f[GX_VA_TEX6] = (GxAttrFmt){ (u8)bits(value, 1, 14), (u8)bits(value, 3, 15), (u8)bits(value, 5, 18) };
        f[GX_VA_TEX7] = (GxAttrFmt){ (u8)bits(value, 1, 23), (u8)bits(value, 3, 24), (u8)bits(value, 5, 27) };
    } else if (addr >= 0xB0 && addr <= 0xBF) {
        u32 attr = addr - 0xB0 + GX_VA_POS;
        if (attr < GX_NUM_ARRAYS) g_gx.arrays[attr].stride = (u8)value;
    } else {
        LOG_ONCE(LOG_LEVEL_WARN, "gx", "display list CP register 0x%02X not handled", addr);
        PROF_COUNT("gx.dl_cp_unhandled", 1);
    }
}

// Write words into XF memory (matrix memory and normal matrices)
static void xf_write(u32 addr, const u8* data, u32 words, bool le) {
    for (u32 i = 0; i < words; i++, addr++) {
        float f = rdf32(data + i * 4, le);
        if (addr < GX_XF_MTX_ROWS * 4) {
            if (g_gx.xf_mtx[addr / 4][addr % 4] == f) continue;
            gx_state_changed(addr < 30 * 4 ? GX_DIRTY_POSMTX : GX_DIRTY_TEXMTX);
            g_gx.xf_mtx[addr / 4][addr % 4] = f;
            if (addr < 30 * 4) g_gx.posmtx_rows |= 1u << (addr / 4);
        } else if (addr >= 0x400 && addr < 0x400 + GX_XF_NRM_ROWS * 3) {
            u32 n = addr - 0x400;
            g_gx.xf_nrm[n / 3][n % 3] = f;
        } else if (addr >= 0x500 && addr < 0x500 + GX_XF_PT_ROWS * 4) {
            u32 n = addr - 0x500;
            if (g_gx.xf_pt[n / 4][n % 4] == f) continue;
            gx_state_changed(GX_DIRTY_TEXMTX);
            g_gx.xf_pt[n / 4][n % 4] = f;
        } else {
            PROF_COUNT("gx.dl_xf_other", 1);
        }
    }
}

static int s_dl_trace;        // >0: log every command (debugging aid)
static bool s_dl_failed;

void gx_vtx_set_trace(bool on) { s_dl_trace = on; }

// Execute one non-draw command; p points just after the command byte.
// Returns the position after the command, or NULL to end the list.
static const u8* exec_command(u8 cmd, const u8* p, const u8* list, const u8* end) {
    switch (cmd & 0xF8) {  // Only the top 5 bits select the command
        case GX_CMD_NOP:
        case GX_CMD_INVAL_VTX:
            return p;
        case GX_CMD_LOAD_CP:
            handle_cp(p[0], rd32(p + 1, false));
            return p + 5;
        case GX_CMD_LOAD_XF: {
            u32 header = rd32(p, false);
            u32 words = ((header >> 16) & 0xFFFF) + 1;
            xf_write(header & 0xFFFF, p + 4, words, false);
            return p + 4 + words * 4;
        }
        case GX_CMD_LOAD_INDX_A:
        case GX_CMD_LOAD_INDX_A + 0x08:
        case GX_CMD_LOAD_INDX_A + 0x10:
        case GX_CMD_LOAD_INDX_D: {
            u16 index = rd16(p, false);
            u16 addr_len = rd16(p + 2, false);
            const GxArray* arr = &g_gx.arrays[GX_POS_MTX_ARRAY + ((cmd & 0xF8) - GX_CMD_LOAD_INDX_A) / 8];
            u32 words = (addr_len >> 12) + 1;
            u32 offset = index * arr->stride;
            if (arr->data && offset + words * 4 <= arr->size) {
                xf_write(addr_len & 0xFFF, arr->data + offset, words, arr->le);
            } else {
                PROF_COUNT("gx.bad_index", 1);
            }
            return p + 4;
        }
        case GX_CMD_CALL_DL:
            LOG_ONCE(LOG_LEVEL_WARN, "gx", "nested display list call ignored");
            return p + 8;
        case GX_CMD_LOAD_BP: {
            u8 reg = p[0];
            // BP "draw done" ends the command stream; the rest of the buffer
            // is not part of the list (Aurora stops here too)
            if (reg == 0x45) return NULL;
            PROF_COUNT("gx.dl_bp", 1);
            LOG_ONCE(LOG_LEVEL_WARN, "gx", "display list BP register 0x%02X ignored", reg);
            return p + 4;
        }
        default: {
            static int reported;
            if (reported++ < 3) {
                LOG_ERROR("gx", "unknown display list command 0x%02X at offset %ld, rest skipped", cmd,
                          (long)(p - 1 - list));
                const u8* start = (p - 1 - list) > 16 ? p - 17 : list;
                dump_decode_state("display list (bytes from 16 before the command)", 0, start, end - start);
            }
            s_dl_failed = true;
            return NULL;
        }
    }
}

void gx_vtx_exec_command(const void* list, u32 nbytes, u32 offset) {
    const u8* base = list;
    exec_command(base[offset], base + offset + 1, base, base + nbytes);
}

bool gx_vtx_run_display_list(const void* list, u32 nbytes, DlRecorder* rec) {
    const u8* base = list;
    const u8* p = base;
    const u8* end = p + nbytes;
    // The GPU fetches display lists in 32-byte blocks, so data up to the next
    // 32-byte boundary is part of the list even if nbytes stops short of it
    const u8* fetch_end = (const u8*)(((uintptr_t)end + 31) & ~(uintptr_t)31);

    s_dl_failed = false;
    s_rec = rec;
    while (p < end) {
        u8 cmd = *p++;
        if (cmd >= 0x80) {
            if (p + 2 > fetch_end) break;
            u16 count = rd16(p, false);
            p += 2;
            u32 used;
            u32 offset = p - 3 - base;
            if (s_dl_trace) {
                LOG_INFO("dltrace", "+%04lX draw prim 0x%02X fmt %u count %u", (unsigned long)offset, cmd & 0xF8, cmd & 7, count);
            }
            gx_vtx_draw_stream((GXPrimitive)(cmd & 0xF8), cmd & 7, count, p, fetch_end - p, &used);
            if (p + used > fetch_end) {
                s_dl_failed = true;
                static int reported;
                if (reported++ < 3)
                    LOG_ERROR("gx", "  (draw at offset %lu of a %lu-byte display list)", (unsigned long)offset,
                              (unsigned long)nbytes);
            }
            p += used;
            continue;
        }

        if (cmd == 0) continue;
        u32 offset = p - 1 - base;
        if (s_dl_trace) {
            LOG_INFO("dltrace", "+%04lX cmd 0x%02X %02X %02X %02X %02X %02X", (unsigned long)offset, cmd, p[0], p[1],
                     p[2], p[3], p[4]);
        }
        if (rec) {
            u8 op = cmd & 0xF8;
            // Matrix loads change shader uniforms, so they split cached draws;
            // CP loads only change decoding, which the recording already baked in
            if (op == GX_CMD_LOAD_XF || (op >= GX_CMD_LOAD_INDX_A && op <= GX_CMD_LOAD_INDX_D)) {
                gx_dlrec_command(rec, offset, true);
            } else if (op == GX_CMD_LOAD_CP) {
                gx_dlrec_command(rec, offset, false);
            }
        }
        p = exec_command(cmd, p, base, end);
        if (!p) break;
    }
    s_rec = NULL;
    return !s_dl_failed;
}

// Refresh positions/normals of a partially cached list from the current
// vertex arrays (same encoding as when it was recorded)
void gx_vtx_gather(const GxGatherRun* runs, u32 run_count, const u16* pos_idx, const u16* nrm_idx,
                   const GxOutVertex* recorded, GxDynVertex* out) {
    const GxArray* parr = &g_gx.arrays[GX_VA_POS];
    const GxArray* narr = &g_gx.arrays[GX_VA_NRM];

    for (u32 r = 0; r < run_count; r++) {
        const GxGatherRun* run = &runs[r];
        bool pos_indexed = run->pos_desc == GX_INDEX8 || run->pos_desc == GX_INDEX16;
        bool nrm_indexed = run->nrm_desc == GX_INDEX8 || run->nrm_desc == GX_INDEX16;
        int pcs = comp_size(run->pos_type), ncs = comp_size(run->nrm_type);
        int pcomps = run->pos_cnt == GX_POS_XY ? 2 : 3;
        u32 psize = pcomps * pcs, nsize = 3 * ncs;
        float pscale = 1.0f / (float)(1 << run->pos_frac);
        float nscale = run->nrm_type == GX_S8 ? 1.0f / 64.0f : run->nrm_type == GX_S16 ? 1.0f / 16384.0f : 1.0f;

        for (u32 i = run->vstart; i < run->vstart + run->count; i++) {
            const GxOutVertex* rv = &recorded[i];
            GxDynVertex* o = &out[i];

            u32 off = pos_idx[i] * parr->stride;
            if (pos_indexed && parr->data && off + psize <= parr->size) {
                const u8* src = parr->data + off;
                o->pos[0] = read_comp(src, run->pos_type, parr->le) * pscale;
                o->pos[1] = read_comp(src + pcs, run->pos_type, parr->le) * pscale;
                o->pos[2] = pcomps == 3 ? read_comp(src + 2 * pcs, run->pos_type, parr->le) * pscale : 0.0f;
            } else {
                o->pos[0] = rv->pos[0]; o->pos[1] = rv->pos[1]; o->pos[2] = rv->pos[2];
            }
            if (gx_diag_bad3(o->pos, GX_DIAG_POS_LIMIT)) gx_diag_bad_value("gather-vtx", o->pos, 3, i, NULL);

            off = nrm_idx[i] * narr->stride;
            if (nrm_indexed && narr->data && off + nsize <= narr->size) {
                const u8* src = narr->data + off;
                for (int c = 0; c < 3; c++) {
                    float n = read_comp(src + c * ncs, run->nrm_type, narr->le) * nscale * 16384.0f;
                    o->nrm[c] = (s16)(n > 32767.0f ? 32767.0f : n < -32768.0f ? -32768.0f : n);
                }
            } else {
                o->nrm[0] = rv->nrm[0]; o->nrm[1] = rv->nrm[1]; o->nrm[2] = rv->nrm[2];
            }
            o->nrm[3] = 0;
        }
    }
}

// Fast path: positions are indexed big-endian s16 XYZ, normals indexed s8
// (checked when the list was recorded). Copies raw values; the shader scales.
void gx_vtx_gather16(const u16* pos_idx, const u16* nrm_idx, u32 count, bool has_nrm, GxDyn16Vertex* out) {
    const GxArray* parr = &g_gx.arrays[GX_VA_POS];
    const GxArray* narr = &g_gx.arrays[GX_VA_NRM];
    const u8* pdata = parr->data;
    const u8* ndata = narr->data;
    u32 pstride = parr->stride, nstride = narr->stride;
    u32 pmax = pstride ? parr->size / pstride : 0;
    u32 nmax = nstride ? narr->size / nstride : 0;
    bool ple = parr->le, nle_unused = narr->le;
    (void)nle_unused;

    for (u32 i = 0; i < count; i++) {
        GxDyn16Vertex* o = &out[i];
        u32 pi = pos_idx[i];
        if (pi < pmax) {
            const u8* p = pdata + pi * pstride;
            if (ple) {
                o->pos[0] = (s16)(p[0] | (p[1] << 8));
                o->pos[1] = (s16)(p[2] | (p[3] << 8));
                o->pos[2] = (s16)(p[4] | (p[5] << 8));
            } else {
                o->pos[0] = (s16)((p[0] << 8) | p[1]);
                o->pos[1] = (s16)((p[2] << 8) | p[3]);
                o->pos[2] = (s16)((p[4] << 8) | p[5]);
            }
        } else {
            o->pos[0] = o->pos[1] = o->pos[2] = 0;
        }
        o->pos[3] = 0;

        u32 ni = nrm_idx[i];
        if (has_nrm && ni < nmax) {
            const s8* n = (const s8*)(ndata + ni * nstride);
            o->nrm[0] = n[0];
            o->nrm[1] = n[1];
            o->nrm[2] = n[2];
        } else {
            o->nrm[0] = o->nrm[1] = 0;
            o->nrm[2] = 64;
        }
        o->nrm[3] = 0;
    }
}
