// GX textures: GXTexObj handling, conversion of GameCube texture formats to
// PICA200 formats, and a texture cache.
//
// Textures from the TEX banks are converted at build time (tools/tex_convert.py)
// into PICA formats with mipmaps; their data starts with a PicaTexHeader
// (gx_pica_tex.h). Those are bound as they are: zero-copy when the data lies
// in linear memory (S3D_TEX_ZEROCOPY), otherwise copied once into linear
// memory. S3D_TEX_PICA=0 ignores them (only useful for A/B tests with an
// unconverted RomFS).
//
// Everything else (textures the game builds at runtime, builds without the
// converter) is GameCube tiled big-endian data (I4, I8, IA4, IA8, RGB565,
// RGB5A3, RGBA8, CMPR). Those are decoded to RGBA, resampled to power-of-two
// sizes if needed, and stored in the PICA's 8x8 Morton-order tiled layout.
// The cache is keyed by (data pointer, format, size); a sampled fingerprint is
// re-checked after GXInvalidateTexAll to catch textures rewritten in place.

#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "gx_diag.h"
#include "gx_init.h"
#include "gx_internal.h"
#include "gx_pica_tex.h"
#include "log.h"
#include "profiler.h"

#define TEX_CACHE_SIZE   512
#define TEX_CACHE_BUDGET (20 * 1024 * 1024)  // Linear memory for converted textures

typedef struct {
    const void* data;
    u16 width, height;
    u8 format;
    bool used;
    bool zero_copy;  // tex.data points into the game's memory (not owned)
    bool pica;       // Converted at build time
    u8 levels;
    float uscale, vscale;
    u32 fingerprint;
    u32 fingerprint8;  // 8-sample fingerprint (S3D_TEX_VERIFY 3)
    u32 hdr_hash;      // Build-time header words (S3D_TEX_VERIFY 2)
    u32 verified_generation;
    u32 last_used_frame;
    u32 gpu_frame;  // gx_render_frame_number of the last use (GPU fence)
    u32 bytes;
    C3D_Tex tex;
    u32 lvl_hash[8];  // S3D_DBG_TEXCHECK: hash of each mip level after conversion
    bool hashed;
} TexCacheEntry;

static int s_use_pica = -1, s_zero_copy = -1;

static TexCacheEntry s_cache[TEX_CACHE_SIZE];
static u32 s_cache_bytes;
static u32 s_frame;
static u32 s_generation = 1;  // Bumped by GXInvalidateTexAll
static u32 s_trimmed;
static int s_tex_verify = 2;  // S3D_TEX_VERIFY, see entry_changed

// --- GXTexObj ------------------------------------------------------------------

_Static_assert(sizeof(GxTexObjData) <= sizeof(GXTexObj), "GxTexObjData must fit in GXTexObj");

static GxTexObjData* tex_of(GXTexObj* obj) { return (GxTexObjData*)obj; }

void GXInitTexObj(GXTexObj* obj, const void* data, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrapS,
                  GXTexWrapMode wrapT, GXBool mipmap) {
    GxTexObjData* t = tex_of(obj);
    void* user = t->user_data;
    memset(t, 0, sizeof(*t));
    t->user_data = user;
    t->data = data;
    t->width = width;
    t->height = height;
    t->format = format;
    t->wrap_s = wrapS;
    t->wrap_t = wrapT;
    t->mipmap = mipmap;
    t->min_filter = mipmap ? GX_LIN_MIP_LIN : GX_LINEAR;
    t->mag_filter = GX_LINEAR;
    t->valid = 1;
}

void GXInitTexObjLOD(GXTexObj* obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod,
                     f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod, GXAnisotropy max_aniso) {
    (void)bias_clamp; (void)do_edge_lod; (void)max_aniso;
    GxTexObjData* t = tex_of(obj);
    t->min_filter = min_filt;
    t->mag_filter = mag_filt;
    t->min_lod = min_lod;
    t->max_lod = max_lod;
    t->lod_bias = lod_bias;
}

void GXInitTexObjUserData(GXTexObj* obj, void* user_data) { tex_of(obj)->user_data = user_data; }
void* GXGetTexObjUserData(const GXTexObj* obj) { return ((const GxTexObjData*)obj)->user_data; }
GXTexFmt GXGetTexObjFmt(const GXTexObj* obj) { return ((const GxTexObjData*)obj)->format; }
u16 GXGetTexObjWidth(const GXTexObj* obj) { return ((const GxTexObjData*)obj)->width; }
u16 GXGetTexObjHeight(const GXTexObj* obj) { return ((const GxTexObjData*)obj)->height; }

void GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    if (id >= GX_NUM_TEXMAP) return;
    const GxTexObjData* t = tex_of(obj);
    if (memcmp(&g_gx.tex[id], t, sizeof(*t)) == 0) return;
    gx_state_changed(GX_DIRTY_TEX);
    g_gx.tex[id] = *t;
}

void GXInvalidateTexAll(void) {
    gx_state_changed(GX_DIRTY_TEX);
    s_generation++;
    static unsigned gen;
    if (gen != cfg_gen()) {
        gen = cfg_gen();
        s_tex_verify = cfg_int("S3D_TEX_VERIFY", 2);
    }
}

// Tile size and bits per pixel of a GX texture format
static void format_info(u32 fmt, int* tw, int* th, int* bpp) {
    switch (fmt & 0xF) {
        case GX_TF_I4: case GX_TF_CMPR: case GX_TF_C4:
            *tw = 8; *th = 8; *bpp = 4; break;
        case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8:
            *tw = 8; *th = 4; *bpp = 8; break;
        case GX_TF_RGBA8:
            *tw = 4; *th = 4; *bpp = 32; break;
        default:  // IA8, RGB565, RGB5A3, C14X2, Z16...
            *tw = 4; *th = 4; *bpp = 16; break;
    }
}

static u32 level_size(u32 fmt, u32 w, u32 h) {
    int tw, th, bpp;
    format_info(fmt, &tw, &th, &bpp);
    u32 wt = (w + tw - 1) / tw;
    u32 ht = (h + th - 1) / th;
    return wt * ht * tw * th * bpp / 8;
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 max_lod) {
    u32 size = 0;
    u32 levels = mipmap ? max_lod : 1;
    if (levels == 0) levels = 1;
    for (u32 i = 0; i < levels; i++) {
        size += level_size(format, width, height);
        if (width == 1 && height == 1) break;
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
    return size;
}

// --- Decoding GameCube formats to RGBA8 --------------------------------------------

static inline u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

static inline void put_rgba(u8* dst, u32 w, u32 h, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) {
    if (x >= w || y >= h) return;
    u8* px = dst + (y * w + x) * 4;
    px[0] = r; px[1] = g; px[2] = b; px[3] = a;
}

static void rgb5a3_to_rgba(u16 v, u8* out) {
    if (v & 0x8000) {
        out[0] = ((v >> 10) & 0x1F) * 255 / 31;
        out[1] = ((v >> 5) & 0x1F) * 255 / 31;
        out[2] = (v & 0x1F) * 255 / 31;
        out[3] = 255;
    } else {
        out[0] = ((v >> 8) & 0xF) * 17;
        out[1] = ((v >> 4) & 0xF) * 17;
        out[2] = (v & 0xF) * 17;
        out[3] = ((v >> 12) & 0x7) * 255 / 7;
    }
}

static void decode_cmpr_block(const u8* src, u8 colors[4][4]) {
    u16 c0 = be16(src), c1 = be16(src + 2);
    u8 r0 = ((c0 >> 11) & 0x1F) * 255 / 31, g0 = ((c0 >> 5) & 0x3F) * 255 / 63, b0 = (c0 & 0x1F) * 255 / 31;
    u8 r1 = ((c1 >> 11) & 0x1F) * 255 / 31, g1 = ((c1 >> 5) & 0x3F) * 255 / 63, b1 = (c1 & 0x1F) * 255 / 31;
    colors[0][0] = r0; colors[0][1] = g0; colors[0][2] = b0; colors[0][3] = 255;
    colors[1][0] = r1; colors[1][1] = g1; colors[1][2] = b1; colors[1][3] = 255;
    if (c0 > c1) {
        colors[2][0] = (2 * r0 + r1) / 3; colors[2][1] = (2 * g0 + g1) / 3; colors[2][2] = (2 * b0 + b1) / 3;
        colors[3][0] = (r0 + 2 * r1) / 3; colors[3][1] = (g0 + 2 * g1) / 3; colors[3][2] = (b0 + 2 * b1) / 3;
        colors[2][3] = colors[3][3] = 255;
    } else {
        colors[2][0] = (r0 + r1) / 2; colors[2][1] = (g0 + g1) / 2; colors[2][2] = (b0 + b1) / 2;
        colors[2][3] = 255;
        colors[3][0] = colors[3][1] = colors[3][2] = 0;
        colors[3][3] = 0;
    }
}

// Decode level 0 of a GX texture into a w*h RGBA8 buffer
static bool decode_gx_texture(const u8* src, u32 fmt, u32 w, u32 h, u8* dst) {
    int tw, th, bpp;
    format_info(fmt, &tw, &th, &bpp);
    u32 tiles_x = (w + tw - 1) / tw, tiles_y = (h + th - 1) / th;

    switch (fmt) {
        case GX_TF_I4:
            for (u32 ty = 0; ty < tiles_y; ty++)
                for (u32 tx = 0; tx < tiles_x; tx++)
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x += 2) {
                            u8 b = *src++;
                            u8 i0 = (b >> 4) * 17, i1 = (b & 0xF) * 17;
                            put_rgba(dst, w, h, tx * 8 + x, ty * 8 + y, i0, i0, i0, i0);
                            put_rgba(dst, w, h, tx * 8 + x + 1, ty * 8 + y, i1, i1, i1, i1);
                        }
            return true;
        case GX_TF_I8:
            for (u32 ty = 0; ty < tiles_y; ty++)
                for (u32 tx = 0; tx < tiles_x; tx++)
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 8; x++) {
                            u8 i = *src++;
                            put_rgba(dst, w, h, tx * 8 + x, ty * 4 + y, i, i, i, i);
                        }
            return true;
        case GX_TF_IA4:
            for (u32 ty = 0; ty < tiles_y; ty++)
                for (u32 tx = 0; tx < tiles_x; tx++)
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 8; x++) {
                            u8 b = *src++;
                            u8 a = (b >> 4) * 17, i = (b & 0xF) * 17;
                            put_rgba(dst, w, h, tx * 8 + x, ty * 4 + y, i, i, i, a);
                        }
            return true;
        case GX_TF_IA8:
            for (u32 ty = 0; ty < tiles_y; ty++)
                for (u32 tx = 0; tx < tiles_x; tx++)
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++, src += 2)
                            put_rgba(dst, w, h, tx * 4 + x, ty * 4 + y, src[1], src[1], src[1], src[0]);
            return true;
        case GX_TF_RGB565:
            for (u32 ty = 0; ty < tiles_y; ty++)
                for (u32 tx = 0; tx < tiles_x; tx++)
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++, src += 2) {
                            u16 v = be16(src);
                            put_rgba(dst, w, h, tx * 4 + x, ty * 4 + y, ((v >> 11) & 0x1F) * 255 / 31,
                                     ((v >> 5) & 0x3F) * 255 / 63, (v & 0x1F) * 255 / 31, 255);
                        }
            return true;
        case GX_TF_RGB5A3:
            for (u32 ty = 0; ty < tiles_y; ty++)
                for (u32 tx = 0; tx < tiles_x; tx++)
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++, src += 2) {
                            u8 c[4];
                            rgb5a3_to_rgba(be16(src), c);
                            put_rgba(dst, w, h, tx * 4 + x, ty * 4 + y, c[0], c[1], c[2], c[3]);
                        }
            return true;
        case GX_TF_RGBA8:
            // Each 4x4 tile: 16 AR pairs followed by 16 GB pairs
            for (u32 ty = 0; ty < tiles_y; ty++)
                for (u32 tx = 0; tx < tiles_x; tx++, src += 64)
                    for (int i = 0; i < 16; i++) {
                        const u8* ar = src + i * 2;
                        const u8* gb = src + 32 + i * 2;
                        put_rgba(dst, w, h, tx * 4 + (i & 3), ty * 4 + (i >> 2), ar[1], gb[0], gb[1], ar[0]);
                    }
            return true;
        case GX_TF_CMPR:
            // 8x8 tiles of four 4x4 DXT1-style sub-blocks
            for (u32 ty = 0; ty < tiles_y; ty++)
                for (u32 tx = 0; tx < tiles_x; tx++)
                    for (int sb = 0; sb < 4; sb++, src += 8) {
                        u8 colors[4][4];
                        decode_cmpr_block(src, colors);
                        u32 bx = tx * 8 + (sb & 1) * 4, by = ty * 8 + (sb >> 1) * 4;
                        for (int y = 0; y < 4; y++) {
                            u8 bits = src[4 + y];
                            for (int x = 0; x < 4; x++) {
                                const u8* c = colors[(bits >> (6 - x * 2)) & 3];
                                put_rgba(dst, w, h, bx + x, by + y, c[0], c[1], c[2], c[3]);
                            }
                        }
                    }
            return true;
        default:
            return false;
    }
}

// --- Encoding to PICA tiled formats --------------------------------------------------

static inline u32 morton(u32 x, u32 y) {
    // Interleave the low 3 bits of x and y (x in the even bits)
    u32 i = 0;
    for (int b = 0; b < 3; b++) {
        i |= ((x >> b) & 1) << (2 * b);
        i |= ((y >> b) & 1) << (2 * b + 1);
    }
    return i;
}

static u32 next_pow2(u32 v) {
    u32 p = 8;
    while (p < v && p < 1024) p <<= 1;
    return p;
}

static GPU_TEXCOLOR pica_format_for(u32 gx_fmt) {
    switch (gx_fmt) {
        case GX_TF_I4: case GX_TF_IA4: return GPU_LA4;
        case GX_TF_I8: case GX_TF_IA8: return GPU_LA8;
        case GX_TF_RGB565:             return GPU_RGB565;
        default:                       return GPU_RGBA8;
    }
}

// Encode an RGBA8 image (rgba, sw x sh) into one tiled PICA level (out,
// tw x th), nearest-resampling when the sizes differ
static void encode_level(const u8* rgba, u32 sw, u32 sh, u8* out, u32 tw, u32 th, GPU_TEXCOLOR fmt) {

    for (u32 y = 0; y < th; y++) {
        u32 sy = y * sh / th;
        for (u32 x = 0; x < tw; x++) {
            u32 sx = x * sw / tw;
            const u8* c = rgba + (sy * sw + sx) * 4;
            u32 texel = ((y >> 3) * (tw >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7);

            switch (fmt) {
                case GPU_RGBA8: {
                    u8* d = out + texel * 4;
                    d[0] = c[3]; d[1] = c[2]; d[2] = c[1]; d[3] = c[0];
                    break;
                }
                case GPU_RGB565: {
                    u16 v = ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3);
                    ((u16*)out)[texel] = v;
                    break;
                }
                case GPU_LA8: {
                    u8* d = out + texel * 2;
                    d[0] = c[3]; d[1] = c[0];
                    break;
                }
                case GPU_LA4:
                    out[texel] = (c[0] & 0xF0) | (c[3] >> 4);
                    break;
                default:
                    break;
            }
        }
    }
}

static void encode_pica(const u8* rgba, u32 sw, u32 sh, C3D_Tex* tex) {
    encode_level(rgba, sw, sh, tex->data, tex->width, tex->height, tex->fmt);
}

// Runtime textures that get a box-filtered mip chain on upload (OPEN_BUGS.md
// 2). The fur noise is 64x64 IA8 with no mip levels (none on GameCube either)
// and is tiled 37.5x over the shells; at 400x240 a screen pixel spans several
// texels, so it sampled as high-contrast speckle in dialog close-ups. The
// game fills the noise frames once (newshadows.c) and the patch registers each
// one after its DCFlushRange. Levels stop at 8x8, the PICA's smallest.
// S3D_FUR_MIPS=0 turns it off.
#define MIP_WANTED_MAX 32
static const void* s_mip_wanted[MIP_WANTED_MAX];
static int s_mip_wanted_count;

void port_tex_want_mips(const void* data) {
    for (int i = 0; i < s_mip_wanted_count; i++)
        if (s_mip_wanted[i] == data) return;
    if (s_mip_wanted_count < MIP_WANTED_MAX) s_mip_wanted[s_mip_wanted_count++] = data;
    else LOG_ONCE(LOG_LEVEL_WARN, "gx", "mip registry full");
}

static bool mips_wanted(const void* data) {
    static int enabled = -1;
    if (enabled < 0) enabled = cfg_bool("S3D_FUR_MIPS", true);
    if (!enabled) return false;
    for (int i = 0; i < s_mip_wanted_count; i++)
        if (s_mip_wanted[i] == data) return true;
    return false;
}

// Halve an RGBA8 image in place (2x2 box filter); w and h are even
static void box_halve(u8* rgba, u32 w, u32 h) {
    u32 hw = w / 2, hh = h / 2;
    for (u32 y = 0; y < hh; y++)
        for (u32 x = 0; x < hw; x++) {
            const u8* a = rgba + ((2 * y) * w + 2 * x) * 4;
            const u8* b = a + w * 4;
            u8* d = rgba + (y * hw + x) * 4;
            for (int c = 0; c < 4; c++) d[c] = (u8)((a[c] + a[c + 4] + b[c] + b[c + 4] + 2) >> 2);
        }
}

// --- Texture integrity check (S3D_DBG_TEXCHECK, OPEN_BUGS.md bug 23) ---------------
// Mipmapped textures turn white on hardware beyond a certain distance, only
// after the LOD bias was set to -2, and stay white until the level reloads:
// suspected corruption of the small mip levels in memory. With the check on,
// each mipmapped texture's levels are hashed when converted and re-read in
// the background (data cache invalidated first, so GPU writes are seen); a
// change is logged with the levels hit and what the bytes look like now.
static int s_texcheck = -1;
static u32 s_check_pos, s_check_logs;

static u32 pica_bpp(GPU_TEXCOLOR fmt);

static u32 level_bytes(const C3D_Tex* t, int l) {
    return (u32)(t->width >> l) * (t->height >> l) * pica_bpp(t->fmt) / 8;
}

static void hash_levels(const C3D_Tex* t, int levels, u32* out) {
    const u8* p = (const u8*)t->data;
    for (int l = 0; l < levels && l < 8; l++) {
        u32 n = level_bytes(t, l), h = 2166136261u;
        for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
        out[l] = h;
        p += n;
    }
}

static bool texcheck_on(void) {
    if (s_texcheck < 0) {
        s_texcheck = cfg_bool("S3D_DBG_TEXCHECK", false) ? 1 : 0;
        if (s_texcheck) LOG_INFO("texcheck", "texture integrity check on (mipmapped textures)");
    }
    return s_texcheck > 0;
}

// --- Cache ---------------------------------------------------------------------------

static u32 fingerprint_n(const u8* data, u32 size, u32 samples) {
    // Sample up to 'samples' words spread across the texture
    u32 h = 2166136261u;
    u32 words = size / 4;
    u32 step = words > samples ? words / samples : 1;
    for (u32 i = 0; i < words; i += step) {
        u32 w;
        memcpy(&w, data + i * 4, 4);
        h = (h ^ w) * 16777619u;
    }
    return h;
}

static u32 fingerprint(const u8* data, u32 size) { return fingerprint_n(data, size, 64); }

static u32 header_hash(const PicaTexHeader* ph) { return fingerprint_n((const u8*)ph, sizeof(*ph), 8); }

static void evict(TexCacheEntry* e) {
    if (!e->used) return;
    // Freed once the GPU is done with the last frame that sampled it
    if (!e->zero_copy) gx_gpu_free_tex(&e->tex, e->gpu_frame);
    s_cache_bytes -= e->bytes;
    e->used = false;
    e->zero_copy = false;
}

// Free least recently used textures (not used this frame) until 'needed' fits
static void make_room(u32 needed) {
    while (s_cache_bytes + needed > TEX_CACHE_BUDGET) {
        TexCacheEntry* oldest = NULL;
        for (int i = 0; i < TEX_CACHE_SIZE; i++) {
            TexCacheEntry* e = &s_cache[i];
            if (e->used && e->last_used_frame != s_frame && (!oldest || e->last_used_frame < oldest->last_used_frame))
                oldest = e;
        }
        if (!oldest) break;
        evict(oldest);
        PROF_COUNT("gx.tex_evicted", 1);
    }
}

static u32 pica_bpp(GPU_TEXCOLOR fmt) {
    switch (fmt) {
        case GPU_RGBA8: return 32;
        case GPU_RGB8: return 24;
        case GPU_RGBA5551: case GPU_RGB565: case GPU_RGBA4: case GPU_LA8: case GPU_HILO8: return 16;
        case GPU_L8: case GPU_A8: case GPU_LA4: case GPU_ETC1A4: return 8;
        default: return 4;  // L4, A4, ETC1
    }
}

// Build-time converted texture: wrap the data in place or copy it once
static bool convert_pica(TexCacheEntry* e, const GxTexObjData* obj, const PicaTexHeader* ph) {
    GPU_TEXCOLOR fmt = (GPU_TEXCOLOR)ph->format;
    int levels = ph->levels;
    const u8* pixels = (const u8*)ph + ph->data_offset;
    u32 bytes = ph->data_size;

    if (s_zero_copy < 0) s_zero_copy = cfg_bool("S3D_TEX_ZEROCOPY", true);
    // Bug 15: the real GPU hangs on texture data that is not 128-byte aligned
    // (self-test step 31: an RGB565 texture at pa % 128 = 0x60). Azahar doesn't
    // care. Unaligned game data is copied into a citro3d buffer (0x80 aligned)
    u32 pa = osConvertVirtToPhys(pixels);
    bool zero_copy = s_zero_copy && pa != 0 && (pa & 0x7F) == 0;
    if (s_zero_copy && pa != 0 && !zero_copy) PROF_COUNT("gx.tex_pica_unaligned", 1);
    if (zero_copy) {
        // The fields C3D_TexInitWithParams would set, without allocating
        memset(&e->tex, 0, sizeof(e->tex));
        e->tex.data = (void*)pixels;
        e->tex.width = ph->width;
        e->tex.height = ph->height;
        e->tex.fmt = fmt;
        e->tex.size = (u32)ph->width * ph->height * pica_bpp(fmt) / 8;
        e->tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
        if (fmt == GPU_ETC1) e->tex.param |= GPU_TEXTURE_ETC1_PARAM;
        e->tex.maxLevel = (u8)(levels - 1);
        e->tex.minLevel = 0;
        GSPGPU_FlushDataCache(pixels, bytes);
        gx_c3d_tex_invalidate();
        e->bytes = 0;  // Game memory: not counted against the cache budget
        PROF_COUNT("gx.tex_pica_zero_copy", 1);
    } else {
        make_room(bytes);
        C3D_TexInitParams p = { ph->width, ph->height, (u8)(levels - 1), fmt, GPU_TEX_2D, false };
        if (!C3D_TexInitWithParams(&e->tex, NULL, p)) {
            make_room(TEX_CACHE_BUDGET);
            if (!C3D_TexInitWithParams(&e->tex, NULL, p) && !(gx_gpu_reclaim() && C3D_TexInitWithParams(&e->tex, NULL, p))) {
                LOG_ONCE(LOG_LEVEL_ERROR, "gx", "out of linear memory for textures");
                return false;
            }
        }
        memcpy(e->tex.data, pixels, bytes);
        GSPGPU_FlushDataCache(e->tex.data, bytes);
        gx_c3d_tex_invalidate();
        e->bytes = bytes;
        PROF_COUNT("gx.tex_pica_copied", 1);
    }
    e->zero_copy = zero_copy;
    e->pica = true;
    e->levels = (u8)levels;
    e->uscale = (ph->flags & PICA_TEXF_PADDED) ? ph->uscale : 1.0f;
    e->vscale = (ph->flags & PICA_TEXF_PADDED) ? ph->vscale : 1.0f;
    e->data = obj->data;
    e->width = obj->width;
    e->height = obj->height;
    e->format = obj->format;
    e->fingerprint = fingerprint(pixels, bytes);
    e->fingerprint8 = fingerprint_n(pixels, bytes, 8);
    e->hdr_hash = header_hash(ph);
    e->verified_generation = s_generation;
    e->used = true;
    s_cache_bytes += e->bytes;
    e->hashed = levels > 1 && texcheck_on();
    if (e->hashed) hash_levels(&e->tex, levels, e->lvl_hash);
    return true;
}

static bool convert_into(TexCacheEntry* e, const GxTexObjData* obj) {
    if (s_use_pica < 0) s_use_pica = cfg_bool("S3D_TEX_PICA", true);
    const PicaTexHeader* ph = s_use_pica ? pica_tex_header(obj->data) : NULL;
    if (ph) return convert_pica(e, obj, ph);
    e->pica = false;
    e->zero_copy = false;
    e->levels = 1;
    e->uscale = e->vscale = 1.0f;

    PROF_ZONE("gx.tex_convert");
    u32 w = obj->width, h = obj->height;
    u32 src_size = level_size(obj->format, w, h);

    u8* rgba = malloc(w * h * 4);
    if (!rgba) return false;
    memset(rgba, 0, w * h * 4);
    if (!decode_gx_texture(obj->data, obj->format, w, h, rgba)) {
        LOG_ONCE(LOG_LEVEL_WARN, "gx", "unsupported texture format 0x%X", obj->format);
        free(rgba);
        return false;
    }

    GPU_TEXCOLOR pfmt = pica_format_for(obj->format);
    u32 tw = next_pow2(w), th = next_pow2(h);
    // Mip chain only for registered power-of-two textures (no resampling)
    int levels = 1;
    if (tw == w && th == h && mips_wanted(obj->data))
        while ((tw >> levels) >= 8 && (th >> levels) >= 8) levels++;
    u32 bytes = tw * th * (pfmt == GPU_RGBA8 ? 4 : pfmt == GPU_LA4 ? 1 : 2);
    if (levels > 1) bytes = bytes * 4 / 3;
    make_room(bytes);

    C3D_TexInitParams params = { (u16)tw, (u16)th, (u8)(levels - 1), pfmt, GPU_TEX_2D, false };
    if (!C3D_TexInitWithParams(&e->tex, NULL, params)) {
        // Linear memory exhausted: drop every texture not used this frame and
        // retry, then wait for the GPU to release what was dropped
        make_room(TEX_CACHE_BUDGET);
        if (!C3D_TexInitWithParams(&e->tex, NULL, params) &&
            !(gx_gpu_reclaim() && C3D_TexInitWithParams(&e->tex, NULL, params))) {
            LOG_ONCE(LOG_LEVEL_ERROR, "gx", "out of linear memory for textures");
            free(rgba);
            return false;
        }
    }

    encode_pica(rgba, w, h, &e->tex);
    for (int l = 1; l < levels; l++) {
        box_halve(rgba, tw >> (l - 1), th >> (l - 1));
        encode_level(rgba, tw >> l, th >> l, C3D_Tex2DGetImagePtr(&e->tex, l, NULL), tw >> l, th >> l, pfmt);
    }
    if (levels > 1) PROF_COUNT("gx.tex_mipped", 1);
    e->levels = (u8)levels;
    free(rgba);
    C3D_TexFlush(&e->tex);
    gx_c3d_tex_invalidate();

    e->data = obj->data;
    e->width = obj->width;
    e->height = obj->height;
    e->format = obj->format;
    e->fingerprint = fingerprint(obj->data, src_size);
    e->fingerprint8 = fingerprint_n(obj->data, src_size, 8);
    e->hdr_hash = 0;
    e->verified_generation = s_generation;
    e->bytes = bytes;
    e->used = true;
    s_cache_bytes += bytes;
    e->hashed = levels > 1 && texcheck_on();
    if (e->hashed) hash_levels(&e->tex, levels, e->lvl_hash);
    PROF_COUNT("gx.tex_converted", 1);
    return true;
}

static inline u32 cache_slot(const void* data) {
    return ((uintptr_t)data >> 5) % TEX_CACHE_SIZE;
}

// S3D_TEX_VERIFY (MULTICORE_PLAN.md, "Texture re-checks"): how a cached entry
// is checked against its data once per GXInvalidateTexAll (every frame):
// 1 = 64-word fingerprint (the old way), 2 = build-time converted textures
// only compare their header (their pixels come from files and aren't
// rewritten; zero-copy ones are read by the GPU in place anyway), runtime
// textures keep the full fingerprint, 3 = 8-word fingerprint for all,
// 0 = never re-check (measuring only: misses textures rewritten in place).
// Default 2: on HW (FA1AFBED) mode 1 cost 2.6 ms of render thread, mode 2 0.1
// (header only), mode 3 0.44.
// C12 wall textures (OPEN_BUGS.md bug 34): the header holds no content
// identity, so a different texture of the same size and format loaded at a
// cached address passed the header check. Copied entries kept the old pixels,
// and zero-copy ones were never flushed from the CPU cache again (DCFlushRange
// is a no-op). Mode 2 now also compares the 8-word pixel fingerprint of build-time
// textures; 4 = the old header-only check (A/B only).
static void log_same_header(const TexCacheEntry* e, const PicaTexHeader* ph) {
    static int logged;
    PROF_COUNT("gx.tex_same_header", 1);
    if (logged >= 16) return;
    logged++;
    LOG_WARN("gx", "texture changed under the same header: %p %ux%u fmt %u levels %u %s (frame %lu, report %d/16)",
             e->data, ph->width, ph->height, ph->format, ph->levels, e->zero_copy ? "zero-copy" : "copied",
             (unsigned long)s_frame, logged);
}

static bool entry_changed(const TexCacheEntry* e, const GxTexObjData* obj) {
    u32 samples = s_tex_verify == 3 ? 8 : 64;
    const u8* data;
    u32 size;
    if (e->pica) {
        const PicaTexHeader* ph = pica_tex_header(obj->data);
        if (!ph || header_hash(ph) != e->hdr_hash) return true;  // No longer the same converted texture
        data = (const u8*)ph + ph->data_offset;
        size = ph->data_size;
        if (s_tex_verify == 2 || s_tex_verify == 4) {
            PROF_COUNT("gx.tex_verify_header", 1);
            if (s_tex_verify == 4) return false;
            GX_PROF_ZONE("gx.tex.verify");
            if (fingerprint_n(data, size, 8) == e->fingerprint8) return false;
            log_same_header(e, ph);
            return true;
        }
    } else {
        data = obj->data;
        size = level_size(obj->format, obj->width, obj->height);
    }
    GX_PROF_ZONE("gx.tex.verify");
    bool changed = fingerprint_n(data, size, samples) != (samples == 8 ? e->fingerprint8 : e->fingerprint);
    if (changed && e->pica) log_same_header(e, pica_tex_header(obj->data));
    return changed;
}

static C3D_Tex* entry_result(TexCacheEntry* e, GxTexInfo* info) {
    e->last_used_frame = s_frame;
    e->gpu_frame = gx_render_frame_number();
    if (info) {
        info->uscale = e->uscale;
        info->vscale = e->vscale;
        info->levels = e->levels;
        info->pica = e->pica;
    }
    return &e->tex;
}

// Entries live at their hashed slot (linear probing). Evicted entries keep
// their data pointer as a tombstone so probe chains stay intact.
// A cached entry for obj: checked against the data once per generation
static C3D_Tex* entry_hit(TexCacheEntry* e, const GxTexObjData* obj, GxTexInfo* info) {
    if (e->verified_generation != s_generation) {
        e->verified_generation = s_generation;
        if (s_tex_verify == 0) return entry_result(e, info);
        PROF_COUNT("gx.tex_verifies", 1);
        if (e->pica) PROF_COUNT("gx.tex_verifies_pica", 1);
        if (entry_changed(e, obj)) {
            evict(e);
            PROF_COUNT("gx.tex_changed", 1);
            return convert_into(e, obj) ? entry_result(e, info) : NULL;
        }
    }
    return entry_result(e, info);
}

static inline bool entry_matches(const TexCacheEntry* e, const GxTexObjData* obj) {
    return e->used && e->data == obj->data && e->format == obj->format && e->width == obj->width &&
           e->height == obj->height;
}

// S3D_RT_MEMO bit 2 (MULTICORE_PLAN.md, "Render-thread memos"): the entry last
// found for a data address, so repeated binds skip the probe. Entries never
// move and an evicted one fails entry_matches, so a stale memo only misses
#define TEX_MEMO_SIZE 128
static TexCacheEntry* s_memo[TEX_MEMO_SIZE];
static inline u32 memo_slot(const void* data) { return ((uintptr_t)data >> 5) % TEX_MEMO_SIZE; }

C3D_Tex* gx_tex_get(const GxTexObjData* obj, GxTexInfo* info) {
    if (!obj->data || obj->width == 0 || obj->height == 0 || obj->width > 1024 || obj->height > 1024)
        return NULL;

    PROF_COUNT("gx.tex_lookups", 1);
    if (g_rt_memo & 2) {
        TexCacheEntry* m = s_memo[memo_slot(obj->data)];
        if (m && entry_matches(m, obj)) {
            PROF_COUNT("gx.tex_memo_hits", 1);
            return entry_hit(m, obj, info);
        }
    }
    u32 start = cache_slot(obj->data);
    TexCacheEntry* free_slot = NULL;
    for (u32 i = 0; i < TEX_CACHE_SIZE; i++) {
        TexCacheEntry* e = &s_cache[(start + i) % TEX_CACHE_SIZE];
        if (!e->used) {
            if (!free_slot) free_slot = e;
            if (!e->data) break;  // Never used: end of the probe chain
            continue;
        }
        if (!entry_matches(e, obj)) continue;
        PROF_COUNT("gx.tex_probe_steps", i + 1);
        s_memo[memo_slot(obj->data)] = e;
        return entry_hit(e, obj, info);
    }

    if (!free_slot) {
        make_room(TEX_CACHE_BUDGET);  // Frees every texture unused this frame
        for (u32 i = 0; i < TEX_CACHE_SIZE && !free_slot; i++) {
            TexCacheEntry* e = &s_cache[(start + i) % TEX_CACHE_SIZE];
            if (!e->used) free_slot = e;
        }
        if (!free_slot) {
            LOG_ONCE(LOG_LEVEL_WARN, "gx", "texture cache full");
            return NULL;
        }
    }
    if (!convert_into(free_slot, obj)) return NULL;
    s_memo[memo_slot(obj->data)] = free_slot;
    return entry_result(free_slot, info);
}

// One texel of an ETC1 block (PICA layout: 64-bit little-endian word, pixel
// index x * 4 + y)
void gx_etc1_texel(const u8* blk, u32 x, u32 y, u8 rgb[3]) {
    static const int tables[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 },
                                      { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };
    u64 w = 0;
    for (int i = 0; i < 8; i++) w |= (u64)blk[i] << (8 * i);
    int diff = (w >> 33) & 1, flip = (w >> 32) & 1;
    int s = flip ? (y >= 2) : (x >= 2);
    int p = x * 4 + y;
    int mag = tables[(w >> (s ? 34 : 37)) & 7][(w >> p) & 1];
    int mod = (w >> (16 + p)) & 1 ? -mag : mag;
    for (int c = 0; c < 3; c++) {
        u32 byte = (w >> (56 - 8 * c)) & 0xFF;
        int base;
        if (diff) {
            int b5 = byte >> 3, d = byte & 7;
            if (d >= 4) d -= 8;
            if (s) b5 += d;
            base = (b5 << 3) | (b5 >> 2);
        } else {
            int b4 = s ? byte & 15 : byte >> 4;
            base = b4 * 17;
        }
        int v = base + mod;
        rgb[c] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

// Texel (x, y) of a w x h GX image stored as a build-time converted texture
// (base level). The stored image may be padded or resampled to a power of two.
static void pica_texel(const PicaTexHeader* hdr, u32 x, u32 y, u32 w, u32 h, u8 out[4]) {
    u32 W = hdr->width, H = hdr->height;
    u32 sx = (u32)((x + 0.5f) / w * hdr->uscale * W), sy = (u32)((y + 0.5f) / h * hdr->vscale * H);
    if (sx >= W) sx = W - 1;
    if (sy >= H) sy = H - 1;
    const u8* d = (const u8*)hdr + hdr->data_offset;
    u32 tile = (sy >> 3) * (W >> 3) + (sx >> 3);
    out[0] = out[1] = out[2] = 0;
    out[3] = 255;
    if (hdr->format == GPU_ETC1 || hdr->format == GPU_ETC1A4) {
        u32 bsize = hdr->format == GPU_ETC1A4 ? 16 : 8;
        const u8* blk = d + (tile * 4 + (((sx >> 2) & 1) | (((sy >> 2) & 1) << 1))) * bsize;
        gx_etc1_texel(blk + bsize - 8, sx & 3, sy & 3, out);
        if (bsize == 16) {
            u32 i = (sx & 3) * 4 + (sy & 3);
            out[3] = ((blk[i >> 1] >> ((i & 1) * 4)) & 15) * 17;
        }
        return;
    }
    u32 m = 0;
    for (int b = 0; b < 3; b++) m |= ((sx >> b) & 1) << (2 * b) | ((sy >> b) & 1) << (2 * b + 1);
    u32 i = tile * 64 + m;
    u32 v;
    switch (hdr->format) {
        case GPU_RGBA8: out[0] = d[i * 4 + 3]; out[1] = d[i * 4 + 2]; out[2] = d[i * 4 + 1]; out[3] = d[i * 4]; break;
        case GPU_RGB8: out[0] = d[i * 3 + 2]; out[1] = d[i * 3 + 1]; out[2] = d[i * 3]; break;
        case GPU_RGB565:
            v = d[i * 2] | d[i * 2 + 1] << 8;
            out[0] = (v >> 11) * 255 / 31; out[1] = ((v >> 5) & 63) * 255 / 63; out[2] = (v & 31) * 255 / 31;
            break;
        case GPU_RGBA5551:
            v = d[i * 2] | d[i * 2 + 1] << 8;
            out[0] = (v >> 11) * 255 / 31; out[1] = ((v >> 6) & 31) * 255 / 31; out[2] = ((v >> 1) & 31) * 255 / 31;
            out[3] = (v & 1) * 255;
            break;
        case GPU_RGBA4:
            v = d[i * 2] | d[i * 2 + 1] << 8;
            out[0] = (v >> 12) * 17; out[1] = ((v >> 8) & 15) * 17; out[2] = ((v >> 4) & 15) * 17; out[3] = (v & 15) * 17;
            break;
        case GPU_LA8: out[0] = out[1] = out[2] = d[i * 2 + 1]; out[3] = d[i * 2]; break;
        case GPU_L8: out[0] = out[1] = out[2] = d[i]; break;
        case GPU_A8: out[3] = d[i]; break;
        case GPU_LA4: out[0] = out[1] = out[2] = (d[i] >> 4) * 17; out[3] = (d[i] & 15) * 17; break;
        default: break;
    }
}

// blendTextures (newshadows.c) mixes two loaded textures on the CPU into a
// runtime GX texture. Loaded textures are converted at build time, so the
// game's GX-layout loop would read PICA data; this does the blend from the
// converted sources instead. Returns 0 when neither source is converted.
int port_blend_textures(const void* src1, const void* src2, u32 w, u32 h, u32 fmt, float blend, void* dst) {
    const PicaTexHeader* ha = pica_tex_header(src1);
    const PicaTexHeader* hb = pica_tex_header(src2);
    if (!ha && !hb) return 0;
    if (!ha || !hb || (fmt != GX_TF_RGBA8 && fmt != GX_TF_RGB565) || (w & 3) || (h & 3)) return 1;
    u32 wa = (u32)(255.0f * blend) & 0xFF, wb = 0xFF - wa;
    u8* out = (u8*)dst;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            u8 a[4], b[4], c[3];
            pica_texel(ha, x, y, w, h, a);
            pica_texel(hb, x, y, w, h, b);
            for (int k = 0; k < 3; k++) c[k] = (u8)(((a[k] * wa) >> 8) + ((b[k] * wb) >> 8));
            u32 tile = (y >> 2) * (w >> 2) + (x >> 2), i = (y & 3) * 4 + (x & 3);
            if (fmt == GX_TF_RGBA8) {
                // Alpha 0, as the game's loop writes it
                u8* t = out + tile * 64;
                t[i * 2] = 0; t[i * 2 + 1] = c[0];
                t[32 + i * 2] = c[1]; t[32 + i * 2 + 1] = c[2];
            } else {
                u16 v = (u16)((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | (c[2] >> 3));
                u8* t = out + tile * 32 + i * 2;
                t[0] = (u8)(v >> 8); t[1] = (u8)v;
            }
        }
    }
    PROF_COUNT("gx.tex_cpu_blends", 1);
    return 1;
}

void gx_tex_init(void) {
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_bytes = 0;
}

void gx_tex_deinit(void) {
    for (int i = 0; i < TEX_CACHE_SIZE; i++) evict(&s_cache[i]);
}

// A tombstone followed by a never-used slot ends its probe chain anyway, so it
// can become never-used too. Entries never move (C3D may hold &e->tex).
static void trim_tombstones(void) {
    bool changed = true;
    for (int pass = 0; pass < 4 && changed; pass++) {
        changed = false;
        for (int i = TEX_CACHE_SIZE - 1; i >= 0; i--) {
            TexCacheEntry* e = &s_cache[i];
            if (e->used || !e->data || s_cache[(i + 1) % TEX_CACHE_SIZE].data) continue;
            e->data = NULL;
            s_trimmed++;
            changed = true;
        }
    }
}

void gx_tex_get_stats(GxTexStats* out) {
    memset(out, 0, sizeof(*out));
    out->slots = TEX_CACHE_SIZE;
    for (int i = 0; i < TEX_CACHE_SIZE; i++) {
        if (s_cache[i].used) out->entries++;
        else if (s_cache[i].data) out->tombs++;
    }
    out->kb = s_cache_bytes / 1024;
    out->trimmed = s_trimmed;
}

// Re-hashes up to ~256 KB of mipmapped textures per frame, round robin
static void texcheck_frame(void) {
    if (!texcheck_on()) return;
    u32 budget = 256 * 1024;
    for (int n = 0; n < TEX_CACHE_SIZE && budget > 0; n++) {
        TexCacheEntry* e = &s_cache[s_check_pos];
        s_check_pos = (s_check_pos + 1) % TEX_CACHE_SIZE;
        if (!e->used || !e->hashed) continue;
        u32 total = 0;
        for (int l = 0; l < e->levels && l < 8; l++) total += level_bytes(&e->tex, l);
        budget = budget > total ? budget - total : 0;
        GSPGPU_InvalidateDataCache(e->tex.data, total);
        u32 now[8];
        hash_levels(&e->tex, e->levels, now);
        u32 mask = 0;
        for (int l = 0; l < e->levels && l < 8; l++)
            if (now[l] != e->lvl_hash[l]) mask |= 1u << l;
        PROF_COUNT("gx.texcheck_kb", total / 1024);
        if (!mask) continue;
        PROF_COUNT("gx.texcheck_changed", 1);
        if (s_check_logs < 64) {
            s_check_logs++;
            LOG_WARN("texcheck", "CHANGED tex %p (game %p) %ux%u pica fmt %d levels %u %s pa %08lX: levels mask %02lX",
                     e->tex.data, e->data, e->tex.width, e->tex.height, (int)e->tex.fmt, e->levels,
                     e->zero_copy ? "zero-copy" : (e->pica ? "copied" : "runtime"),
                     (unsigned long)osConvertVirtToPhys(e->tex.data), (unsigned long)mask);
            const u8* p = (const u8*)e->tex.data;
            for (int l = 0; l < e->levels && l < 8; l++) {
                u32 nb = level_bytes(&e->tex, l);
                if (mask & (1u << l)) {
                    u32 ff = 0, zero = 0;
                    for (u32 i = 0; i < nb; i++) {
                        ff += p[i] == 0xFF;
                        zero += p[i] == 0;
                    }
                    LOG_WARN("texcheck", "  level %d (%lu B): %lu%% 0xFF, %lu%% 0x00, first %02X%02X%02X%02X %02X%02X%02X%02X", l,
                             (unsigned long)nb, (unsigned long)(ff * 100 / nb), (unsigned long)(zero * 100 / nb), p[0],
                             p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                }
                p += nb;
            }
        }
        memcpy(e->lvl_hash, now, sizeof(now));  // Report only further changes
    }
}

void gx_tex_frame_begin(void) {
    s_frame++;
    texcheck_frame();
    if ((s_frame & 63) == 0) trim_tombstones();
    PROF_COUNT("gx.tex_cache_kb", s_cache_bytes / 1024);
}
