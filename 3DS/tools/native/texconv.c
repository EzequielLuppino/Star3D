// Build-time texture converter (GRAPHICS_PLAN.md section 2), loaded by
// tools/tex_convert.py through ctypes. Host C (built by the Makefile with the
// host compiler), no dependencies.
//
// Input: one game texture as stored in TEX0/TEX1/TEXPRE (0x60-byte header +
// GameCube pixel data, big-endian, GX tiled). Output: the same header followed
// by PICA-ready data that the 3DS GPU samples directly:
//
//   payload + 0x00  PicaTexHeader (32 bytes, see gx/gx_pica_tex.h)
//   payload + 0x20  level 0, level 1, ... in PICA 8x8 tile order
//   [secondary image: another PicaTexHeader + levels, at the new imageOffset]
//
// Width/height/format in the game header are left alone (game logic reads
// them); the runtime recognises converted data by the PicaTexHeader magic.
// Non-power-of-two images are resampled (repeat/mirror wrap) or padded
// (clamp, with a UV scale in the header). Mipmaps are generated down to 8x8
// with gamma-correct filtering. Formats are chosen per texture from its pixels
// (see choose_format), ETC1 only when it meets the PSNR threshold.

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

// --- Shared definitions (must match gx/gx_pica_tex.h) -----------------------

#define PT_MAGIC      0x31543350u  // "P3T1" little-endian
#define PT_KEY        0x51A3F0C9u
#define PT_HDR_SIZE   32

enum {  // GPU_TEXCOLOR
    PF_RGBA8 = 0x0, PF_RGB8 = 0x1, PF_RGBA5551 = 0x2, PF_RGB565 = 0x3, PF_RGBA4 = 0x4, PF_LA8 = 0x5,
    PF_HILO8 = 0x6, PF_L8 = 0x7, PF_A8 = 0x8, PF_LA4 = 0x9, PF_L4 = 0xA, PF_A4 = 0xB, PF_ETC1 = 0xC,
    PF_ETC1A4 = 0xD,
};

enum { GX_I4 = 0, GX_I8 = 1, GX_IA4 = 2, GX_IA8 = 3, GX_RGB565 = 4, GX_RGB5A3 = 5, GX_RGBA8 = 6, GX_CMPR = 14 };
enum { WRAP_CLAMP = 0, WRAP_REPEAT = 1, WRAP_MIRROR = 2 };

#define PTF_PADDED   0x01  // NPOT image padded: only uscale x vscale of the texture is the image
#define PTF_RESAMPLED 0x02

typedef struct {
    u32 magic, key;
    u16 width, height;  // Stored (power-of-two) size
    u8 format;          // GPU_TEXCOLOR
    u8 levels;          // Mip levels stored (1 = no mipmaps)
    u8 flags;
    u8 swizzle;         // Reserved (0)
    float uscale, vscale;
    u32 data_offset;    // From the start of this header to level 0
    u32 data_size;      // All levels
} PicaTexHeader;

_Static_assert(sizeof(PicaTexHeader) == PT_HDR_SIZE, "header size");

typedef struct {
    int quality;        // 0 = high (no lossy recompression of non-CMPR), 1 = balanced, 2 = small
    int mipmaps;        // Generate mip levels
    int max_size;       // Clamp the stored size (power of two, <= 1024)
} TcOptions;

typedef struct {
    u32 converted, skipped;
    u32 by_format[16];
    double worst_psnr[16];
    u32 npot_padded, npot_resampled;
    u64 in_bytes, out_bytes;
} TcStats;

// --- Helpers -------------------------------------------------------------------

static inline u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
static inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline void put_be32(u8* p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline u8 c4to8(u32 v) { return (u8)(v * 17); }
static inline u8 c5to8(u32 v) { return (u8)((v << 3) | (v >> 2)); }
static inline u8 c6to8(u32 v) { return (u8)((v << 2) | (v >> 4)); }
static inline u8 c3to8(u32 v) { return (u8)((v << 5) | (v << 2) | (v >> 1)); }

static u32 pow2_ceil(u32 v) { u32 p = 1; while (p < v) p <<= 1; return p; }
static u32 pow2_round(u32 v) {
    u32 hi = pow2_ceil(v), lo = hi >> 1;
    if (!lo) return hi;
    return (double)v / lo < (double)hi / v ? lo : hi;
}

// --- GameCube decode (to RGBA8, rows top to bottom) --------------------------

static void fmt_tile(int fmt, int* tw, int* th, int* bpp) {
    switch (fmt) {
        case GX_I4: case GX_CMPR: *tw = 8; *th = 8; *bpp = 4; break;
        case GX_I8: case GX_IA4: *tw = 8; *th = 4; *bpp = 8; break;
        case GX_RGBA8: *tw = 4; *th = 4; *bpp = 32; break;
        default: *tw = 4; *th = 4; *bpp = 16; break;
    }
}

static u32 gx_level_size(int fmt, u32 w, u32 h) {
    int tw, th, bpp;
    fmt_tile(fmt, &tw, &th, &bpp);
    return ((w + tw - 1) / tw) * ((h + th - 1) / th) * tw * th * bpp / 8;
}

static inline void put(u8* dst, u32 w, u32 h, u32 x, u32 y, u8 r, u8 g, u8 b, u8 a) {
    if (x >= w || y >= h) return;
    u8* p = dst + (y * w + x) * 4;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static void rgb5a3(u16 v, u8* o) {
    if (v & 0x8000) {
        o[0] = c5to8((v >> 10) & 31); o[1] = c5to8((v >> 5) & 31); o[2] = c5to8(v & 31); o[3] = 255;
    } else {
        o[0] = c4to8((v >> 8) & 15); o[1] = c4to8((v >> 4) & 15); o[2] = c4to8(v & 15); o[3] = c3to8((v >> 12) & 7);
    }
}

static int gx_decode(const u8* src, u32 avail, int fmt, u32 w, u32 h, u8* dst) {
    int tw, th, bpp;
    fmt_tile(fmt, &tw, &th, &bpp);
    if (gx_level_size(fmt, w, h) > avail) return 0;
    u32 tx_n = (w + tw - 1) / tw, ty_n = (h + th - 1) / th;
    for (u32 ty = 0; ty < ty_n; ty++) {
        for (u32 tx = 0; tx < tx_n; tx++) {
            u32 bx = tx * tw, by = ty * th;
            switch (fmt) {
                case GX_I4:
                    for (int y = 0; y < 8; y++)
                        for (int x = 0; x < 8; x += 2) {
                            u8 b = *src++;
                            u8 i0 = c4to8(b >> 4), i1 = c4to8(b & 15);
                            put(dst, w, h, bx + x, by + y, i0, i0, i0, i0);
                            put(dst, w, h, bx + x + 1, by + y, i1, i1, i1, i1);
                        }
                    break;
                case GX_I8:
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 8; x++) { u8 i = *src++; put(dst, w, h, bx + x, by + y, i, i, i, i); }
                    break;
                case GX_IA4:
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 8; x++) {
                            u8 b = *src++;
                            u8 a = c4to8(b >> 4), i = c4to8(b & 15);
                            put(dst, w, h, bx + x, by + y, i, i, i, a);
                        }
                    break;
                case GX_IA8:
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++, src += 2) put(dst, w, h, bx + x, by + y, src[1], src[1], src[1], src[0]);
                    break;
                case GX_RGB565:
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++, src += 2) {
                            u16 v = be16(src);
                            put(dst, w, h, bx + x, by + y, c5to8(v >> 11), c6to8((v >> 5) & 63), c5to8(v & 31), 255);
                        }
                    break;
                case GX_RGB5A3:
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++, src += 2) {
                            u8 c[4];
                            rgb5a3(be16(src), c);
                            put(dst, w, h, bx + x, by + y, c[0], c[1], c[2], c[3]);
                        }
                    break;
                case GX_RGBA8:
                    for (int i = 0; i < 16; i++) {
                        const u8* ar = src + i * 2;
                        const u8* gb = src + 32 + i * 2;
                        put(dst, w, h, bx + (i & 3), by + (i >> 2), ar[1], gb[0], gb[1], ar[0]);
                    }
                    src += 64;
                    break;
                case GX_CMPR:
                    for (int sb = 0; sb < 4; sb++, src += 8) {
                        u16 c0 = be16(src), c1 = be16(src + 2);
                        u8 pal[4][4];
                        pal[0][0] = c5to8(c0 >> 11); pal[0][1] = c6to8((c0 >> 5) & 63); pal[0][2] = c5to8(c0 & 31);
                        pal[1][0] = c5to8(c1 >> 11); pal[1][1] = c6to8((c1 >> 5) & 63); pal[1][2] = c5to8(c1 & 31);
                        pal[0][3] = pal[1][3] = 255;
                        for (int c = 0; c < 3; c++) {
                            if (c0 > c1) {
                                pal[2][c] = (u8)((2 * pal[0][c] + pal[1][c]) / 3);
                                pal[3][c] = (u8)((pal[0][c] + 2 * pal[1][c]) / 3);
                            } else {
                                pal[2][c] = (u8)((pal[0][c] + pal[1][c]) / 2);
                                pal[3][c] = 0;
                            }
                        }
                        pal[2][3] = 255;
                        pal[3][3] = c0 > c1 ? 255 : 0;
                        u32 sx = bx + (sb & 1) * 4, sy = by + (sb >> 1) * 4;
                        for (int y = 0; y < 4; y++) {
                            u8 bits = src[4 + y];
                            for (int x = 0; x < 4; x++) {
                                const u8* c = pal[(bits >> (6 - 2 * x)) & 3];
                                put(dst, w, h, sx + x, sy + y, c[0], c[1], c[2], c[3]);
                            }
                        }
                    }
                    break;
                default:
                    return 0;
            }
        }
    }
    return 1;
}

// --- Resampling and mipmaps (in linear light for color, linear for alpha) ---

static float s_to_lin[256];
static u8 lin_to_s(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 1.0f) return 255;
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
    return (u8)(s * 255.0f + 0.5f);
}
static void init_tables(void) {
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < 256; i++) {
        float s = i / 255.0f;
        s_to_lin[i] = s <= 0.04045f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
    }
}

static float lanczos2(float x) {
    x = fabsf(x);
    if (x < 1e-6f) return 1.0f;
    if (x >= 2.0f) return 0.0f;
    float px = (float)M_PI * x;
    return 2.0f * sinf(px) * sinf(px * 0.5f) / (px * px);
}

static int wrap_coord(int i, int n, int mode) {
    if (mode == WRAP_REPEAT) { i %= n; return i < 0 ? i + n : i; }
    if (mode == WRAP_MIRROR) {
        int p = 2 * n;
        i %= p;
        if (i < 0) i += p;
        return i < n ? i : p - 1 - i;
    }
    return clampi(i, 0, n - 1);
}

// Separable Lanczos-2 resample of a float RGBA image (premultiplied by alpha
// so transparent texels do not bleed their color)
static void resample_axis(const float* src, int sw, int sh, float* dst, int dw, int horizontal, int wrap) {
    int sn = horizontal ? sw : sh, dn = dw;
    int lines = horizontal ? sh : sw;
    float scale = (float)sn / dn;
    float support = 2.0f * (scale > 1.0f ? scale : 1.0f);
    float fscale = scale > 1.0f ? 1.0f / scale : 1.0f;
    for (int d = 0; d < dn; d++) {
        float center = (d + 0.5f) * scale - 0.5f;
        int i0 = (int)floorf(center - support), i1 = (int)ceilf(center + support);
        float wsum = 0;
        for (int line = 0; line < lines; line++) {
            float c[4] = { 0, 0, 0, 0 };
            wsum = 0;
            for (int i = i0; i <= i1; i++) {
                float wgt = lanczos2((i - center) * fscale);
                if (wgt == 0.0f) continue;
                int si = wrap_coord(i, sn, wrap);
                const float* p = horizontal ? &src[(line * sw + si) * 4] : &src[(si * sw + line) * 4];
                for (int k = 0; k < 4; k++) c[k] += p[k] * wgt;
                wsum += wgt;
            }
            float* o = horizontal ? &dst[(line * dw + d) * 4] : &dst[(d * sw + line) * 4];
            for (int k = 0; k < 4; k++) o[k] = wsum != 0.0f ? c[k] / wsum : 0.0f;
        }
    }
}

static float* to_float_premul(const u8* rgba, int n) {
    float* f = malloc((size_t)n * 16);
    for (int i = 0; i < n; i++) {
        float a = rgba[i * 4 + 3] / 255.0f;
        for (int c = 0; c < 3; c++) f[i * 4 + c] = s_to_lin[rgba[i * 4 + c]] * a;
        f[i * 4 + 3] = a;
    }
    return f;
}

static void from_float_premul(const float* f, int n, u8* rgba) {
    for (int i = 0; i < n; i++) {
        float a = f[i * 4 + 3];
        if (a < 0) a = 0;
        if (a > 1) a = 1;
        for (int c = 0; c < 3; c++) rgba[i * 4 + c] = a > 1e-5f ? lin_to_s(f[i * 4 + c] / a) : 0;
        rgba[i * 4 + 3] = (u8)(a * 255.0f + 0.5f);
    }
}

static u8* resample(const u8* rgba, int sw, int sh, int dw, int dh, int wrap_s, int wrap_t) {
    float* f = to_float_premul(rgba, sw * sh);
    float* tmp = malloc((size_t)dw * sh * 16);
    float* out = malloc((size_t)dw * dh * 16);
    resample_axis(f, sw, sh, tmp, dw, 1, wrap_s);
    resample_axis(tmp, dw, sh, out, dh, 0, wrap_t);
    u8* res = malloc((size_t)dw * dh * 4);
    from_float_premul(out, dw * dh, res);
    free(f);
    free(tmp);
    free(out);
    return res;
}

// Pad to dw x dh, replicating the last column/row (clamp-to-edge sampling of
// the padded texture then matches clamping the original)
static u8* pad(const u8* rgba, int sw, int sh, int dw, int dh) {
    u8* res = malloc((size_t)dw * dh * 4);
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++)
            memcpy(&res[(y * dw + x) * 4], &rgba[((y < sh ? y : sh - 1) * sw + (x < sw ? x : sw - 1)) * 4], 4);
    return res;
}

// 2x2 box filter in linear light, alpha-weighted
static u8* half_size(const u8* rgba, int w, int h) {
    int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    u8* res = malloc((size_t)nw * nh * 4);
    for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
            float c[3] = { 0, 0, 0 }, a = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    const u8* p = &rgba[((clampi(y * 2 + dy, 0, h - 1)) * w + clampi(x * 2 + dx, 0, w - 1)) * 4];
                    float pa = p[3] / 255.0f;
                    for (int k = 0; k < 3; k++) c[k] += s_to_lin[p[k]] * pa;
                    a += pa;
                }
            u8* o = &res[(y * nw + x) * 4];
            for (int k = 0; k < 3; k++) o[k] = a > 1e-5f ? lin_to_s(c[k] / a) : 0;
            o[3] = (u8)(a / 4.0f * 255.0f + 0.5f);
        }
    }
    return res;
}

// --- PICA encoders --------------------------------------------------------------

static inline u32 morton(u32 x, u32 y) {
    u32 i = 0;
    for (int b = 0; b < 3; b++) i |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    return i;
}

static int fmt_bpp(int pf) {
    switch (pf) {
        case PF_RGBA8: return 32;
        case PF_RGB8: return 24;
        case PF_RGBA5551: case PF_RGB565: case PF_RGBA4: case PF_LA8: case PF_HILO8: return 16;
        case PF_L8: case PF_A8: case PF_LA4: case PF_ETC1A4: return 8;
        default: return 4;
    }
}

static u32 pica_level_size(int pf, u32 w, u32 h) { return w * h * fmt_bpp(pf) / 8; }

static void encode_plain(const u8* rgba, u32 w, u32 h, int pf, u8* out) {
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            const u8* c = &rgba[(y * w + x) * 4];
            u32 t = ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7);
            u16 v;
            switch (pf) {
                case PF_RGBA8: out[t * 4] = c[3]; out[t * 4 + 1] = c[2]; out[t * 4 + 2] = c[1]; out[t * 4 + 3] = c[0]; break;
                case PF_RGB565:
                    v = (u16)(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255));
                    out[t * 2] = v & 0xFF; out[t * 2 + 1] = v >> 8; break;
                case PF_RGBA5551:
                    v = (u16)(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 31 + 127) / 255) << 6 |
                              ((c[2] * 31 + 127) / 255) << 1 | (c[3] >= 128));
                    out[t * 2] = v & 0xFF; out[t * 2 + 1] = v >> 8; break;
                case PF_RGBA4:
                    v = (u16)(((c[0] * 15 + 127) / 255) << 12 | ((c[1] * 15 + 127) / 255) << 8 |
                              ((c[2] * 15 + 127) / 255) << 4 | ((c[3] * 15 + 127) / 255));
                    out[t * 2] = v & 0xFF; out[t * 2 + 1] = v >> 8; break;
                case PF_LA8: out[t * 2] = c[3]; out[t * 2 + 1] = c[0]; break;
                case PF_L8: out[t] = c[0]; break;
                case PF_A8: out[t] = c[3]; break;
                case PF_LA4: out[t] = (u8)(((c[0] * 15 + 127) / 255) << 4 | ((c[3] * 15 + 127) / 255)); break;
                default: break;
            }
        }
    }
}

// ETC1 -------------------------------------------------------------------------

static const int etc_tables[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 },
                                      { 18, 60 }, { 24, 80 }, { 33, 106 }, { 47, 183 } };
// Perceptual weights (sum 16)
#define WR 5
#define WG 9
#define WB 2

typedef struct {
    u8 px[8][3];
    int n;
} SubBlock;

// Best table and per-pixel modifiers for base color (r,g,b): returns error
static u32 eval_sub(const SubBlock* sb, int r, int g, int b, int* best_table, u8 sel[8]) {
    u32 best = 0xFFFFFFFF;
    for (int t = 0; t < 8; t++) {
        int mods[4] = { etc_tables[t][0], etc_tables[t][1], -etc_tables[t][0], -etc_tables[t][1] };
        u32 err = 0;
        u8 s[8];
        for (int i = 0; i < sb->n && err < best; i++) {
            u32 be = 0xFFFFFFFF;
            for (int m = 0; m < 4; m++) {
                int dr = clampi(r + mods[m], 0, 255) - sb->px[i][0];
                int dg = clampi(g + mods[m], 0, 255) - sb->px[i][1];
                int db = clampi(b + mods[m], 0, 255) - sb->px[i][2];
                u32 e = (u32)(WR * dr * dr + WG * dg * dg + WB * db * db);
                if (e < be) { be = e; s[i] = (u8)m; }
            }
            err += be;
        }
        if (err < best) {
            best = err;
            *best_table = t;
            memcpy(sel, s, 8);
        }
    }
    return best;
}

static void sub_average(const SubBlock* sb, float avg[3]) {
    avg[0] = avg[1] = avg[2] = 0;
    for (int i = 0; i < sb->n; i++)
        for (int c = 0; c < 3; c++) avg[c] += sb->px[i][c];
    for (int c = 0; c < 3; c++) avg[c] /= sb->n;
}

typedef struct {
    u32 err;
    int q[3];   // Quantized base color (4 or 5 bits)
    int table;
    u8 sel[8];
} SubResult;

// Search quantized base colors around the average (bits = 4 or 5)
static void search_sub(const SubBlock* sb, int bits, SubResult* res) {
    float avg[3];
    sub_average(sb, avg);
    int maxq = (1 << bits) - 1;
    int q0[3];
    for (int c = 0; c < 3; c++) q0[c] = clampi((int)(avg[c] * maxq / 255.0f + 0.5f), 0, maxq);
    res->err = 0xFFFFFFFF;
    for (int dr = -1; dr <= 1; dr++)
        for (int dg = -1; dg <= 1; dg++)
            for (int db = -1; db <= 1; db++) {
                int q[3] = { q0[0] + dr, q0[1] + dg, q0[2] + db };
                if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] > maxq || q[1] > maxq || q[2] > maxq) continue;
                int r = bits == 4 ? c4to8(q[0]) : c5to8(q[0]);
                int g = bits == 4 ? c4to8(q[1]) : c5to8(q[1]);
                int b = bits == 4 ? c4to8(q[2]) : c5to8(q[2]);
                int t;
                u8 sel[8];
                u32 e = eval_sub(sb, r, g, b, &t, sel);
                if (e < res->err) {
                    res->err = e;
                    memcpy(res->q, q, sizeof(q));
                    res->table = t;
                    memcpy(res->sel, sel, 8);
                }
            }
}

// Evaluate a fixed 5-bit base (differential mode second subblock)
static void eval_fixed5(const SubBlock* sb, const int q[3], SubResult* res) {
    res->err = eval_sub(sb, c5to8(q[0]), c5to8(q[1]), c5to8(q[2]), &res->table, res->sel);
    memcpy(res->q, q, sizeof(int) * 3);
}

// Pixel order inside an ETC1 block: index = x * 4 + y
static void etc1_encode_block(const u8 block[16][4], u8 out[8], u64* sq_err) {
    u64 best_err = ~0ull;
    u64 best_word = 0;
    for (int flip = 0; flip < 2; flip++) {
        SubBlock sb[2];
        int idx[2][8];
        sb[0].n = sb[1].n = 0;
        for (int x = 0; x < 4; x++)
            for (int y = 0; y < 4; y++) {
                int s = flip ? (y >= 2) : (x >= 2);
                memcpy(sb[s].px[sb[s].n], block[y * 4 + x], 3);
                idx[s][sb[s].n] = x * 4 + y;
                sb[s].n++;
            }
        // Individual mode (4-bit bases)
        SubResult a, b;
        search_sub(&sb[0], 4, &a);
        search_sub(&sb[1], 4, &b);
        u64 err_ind = (u64)a.err + b.err;
        // Differential mode (5-bit base, second within -4..3)
        SubResult da, dbr;
        search_sub(&sb[0], 5, &da);
        search_sub(&sb[1], 5, &dbr);
        int ok = 1;
        for (int c = 0; c < 3; c++) {
            int d = dbr.q[c] - da.q[c];
            if (d < -4 || d > 3) ok = 0;
        }
        if (!ok) {
            // Clamp the second base into range of the first
            int q[3];
            for (int c = 0; c < 3; c++) q[c] = clampi(dbr.q[c], da.q[c] - 4, da.q[c] + 3);
            for (int c = 0; c < 3; c++) q[c] = clampi(q[c], 0, 31);
            eval_fixed5(&sb[1], q, &dbr);
        }
        u64 err_diff = (u64)da.err + dbr.err;

        int diff = err_diff < err_ind;
        const SubResult* s0 = diff ? &da : &a;
        const SubResult* s1 = diff ? &dbr : &b;
        u64 err = diff ? err_diff : err_ind;
        if (err >= best_err) continue;
        best_err = err;

        u64 w = 0;
        if (diff) {
            for (int c = 0; c < 3; c++) {
                int d = s1->q[c] - s0->q[c];
                w |= (u64)((s0->q[c] << 3) | (d & 7)) << (56 - 8 * c);
            }
        } else {
            for (int c = 0; c < 3; c++) w |= (u64)((s0->q[c] << 4) | s1->q[c]) << (56 - 8 * c);
        }
        w |= (u64)s0->table << 37 | (u64)s1->table << 34 | (u64)diff << 33 | (u64)flip << 32;
        // Modifier index m (0: +a, 1: +b, 2: -a, 3: -b) -> pixel code (msb, lsb)
        static const u8 code[4] = { 0, 1, 2, 3 };
        for (int s = 0; s < 2; s++) {
            const SubResult* r = s ? s1 : s0;
            for (int i = 0; i < sb[s].n; i++) {
                int p = idx[s][i];
                u8 cd = code[r->sel[i]];
                w |= (u64)(cd >> 1) << (16 + p);
                w |= (u64)(cd & 1) << p;
            }
        }
        best_word = w;
    }
    // PICA reads the 64-bit block little-endian
    for (int i = 0; i < 8; i++) out[i] = (u8)(best_word >> (8 * i));
    if (sq_err) *sq_err += best_err;
}

// Mapping of the stored pixel codes: msb/lsb planes encode 00 = +a, 01 = +b,
// 10 = -a, 11 = -b. (ETC1: msb set = negative, lsb set = large modifier)
static void etc1_decode_block(const u8 in[8], u8 out[16][4]) {
    u64 w = 0;
    for (int i = 0; i < 8; i++) w |= (u64)in[i] << (8 * i);
    int diff = (w >> 33) & 1, flip = (w >> 32) & 1;
    int base[2][3];
    for (int c = 0; c < 3; c++) {
        u32 byte = (w >> (56 - 8 * c)) & 0xFF;
        if (diff) {
            int b5 = byte >> 3;
            int d = (int)(byte & 7);
            if (d >= 4) d -= 8;
            base[0][c] = c5to8(b5);
            base[1][c] = c5to8((u32)clampi(b5 + d, 0, 31));
        } else {
            base[0][c] = c4to8(byte >> 4);
            base[1][c] = c4to8(byte & 15);
        }
    }
    int tab[2] = { (int)((w >> 37) & 7), (int)((w >> 34) & 7) };
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            int p = x * 4 + y;
            int s = flip ? (y >= 2) : (x >= 2);
            int msb = (w >> (16 + p)) & 1, lsb = (w >> p) & 1;
            int mag = etc_tables[tab[s]][lsb];
            int mod = msb ? -mag : mag;
            for (int c = 0; c < 3; c++) out[y * 4 + x][c] = (u8)clampi(base[s][c] + mod, 0, 255);
            out[y * 4 + x][3] = 255;
        }
}

// Returns the color PSNR of the encoding
static double encode_etc(const u8* rgba, u32 w, u32 h, int with_alpha, u8* out) {
    double sq = 0;
    u32 blk_bytes = with_alpha ? 16 : 8;
    u8* o = out;
    for (u32 ty = 0; ty < h; ty += 8)
        for (u32 tx = 0; tx < w; tx += 8)
            for (int sb = 0; sb < 4; sb++) {
                u32 bx = tx + (sb & 1) * 4, by = ty + (sb >> 1) * 4;
                u8 block[16][4];
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) memcpy(block[y * 4 + x], &rgba[((by + y) * w + bx + x) * 4], 4);
                if (with_alpha) {
                    u64 a = 0;
                    for (int x = 0; x < 4; x++)
                        for (int y = 0; y < 4; y++)
                            a |= (u64)((block[y * 4 + x][3] * 15 + 127) / 255) << (4 * (x * 4 + y));
                    for (int i = 0; i < 8; i++) o[i] = (u8)(a >> (8 * i));
                }
                u8* e = o + (with_alpha ? 8 : 0);
                etc1_encode_block(block, e, NULL);
                u8 dec[16][4];
                etc1_decode_block(e, dec);
                for (int i = 0; i < 16; i++)
                    for (int c = 0; c < 3; c++) {
                        double d = (double)dec[i][c] - block[i][c];
                        sq += d * d;
                    }
                o += blk_bytes;
            }
    double mse = sq / ((double)w * h * 3);
    return mse <= 1e-9 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

// PSNR of a plain format's round trip (color channels only)
static double plain_psnr(const u8* rgba, u32 n, int pf) {
    double sq = 0;
    for (u32 i = 0; i < n; i++) {
        const u8* c = &rgba[i * 4];
        for (int k = 0; k < 3; k++) {
            int v = c[k], r;
            switch (pf) {
                case PF_RGB565: r = k == 1 ? c6to8((v * 63 + 127) / 255) : c5to8((v * 31 + 127) / 255); break;
                case PF_RGBA5551: r = c5to8((v * 31 + 127) / 255); break;
                case PF_RGBA4: r = c4to8((v * 15 + 127) / 255); break;
                default: r = v; break;
            }
            sq += (double)(r - v) * (r - v);
        }
    }
    double mse = sq / ((double)n * 3);
    return mse <= 1e-9 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
}

// --- Format policy --------------------------------------------------------------------

typedef struct {
    int opaque, binary_alpha;
    int gray;  // r == g == b everywhere
} Analysis;

static Analysis analyse(const u8* rgba, u32 n) {
    Analysis a = { 1, 1, 1 };
    for (u32 i = 0; i < n; i++) {
        const u8* c = &rgba[i * 4];
        if (c[3] != 255) a.opaque = 0;
        if (c[3] != 255 && c[3] != 0) a.binary_alpha = 0;
        if (c[0] != c[1] || c[1] != c[2]) a.gray = 0;
    }
    return a;
}

static double etc_threshold(int quality) { return quality == 0 ? 1e9 : quality == 1 ? 38.0 : 32.0; }

// Picks the PICA format. 'level0' is the stored-size level 0 image.
static int choose_format(int gx_fmt, const u8* level0, u32 w, u32 h, const TcOptions* opt, u8* scratch,
                         double* psnr_out) {
    Analysis a = analyse(level0, w * h);
    *psnr_out = 99.0;
    switch (gx_fmt) {
        case GX_I4: case GX_IA4: return PF_LA4;
        case GX_I8: case GX_IA8: return PF_LA8;
        case GX_RGB565: return PF_RGB565;
        case GX_CMPR:
            // Already block-compressed: ETC1 keeps the size
            if (a.opaque) { *psnr_out = encode_etc(level0, w, h, 0, scratch); return PF_ETC1; }
            *psnr_out = encode_etc(level0, w, h, 1, scratch);
            return PF_ETC1A4;
        case GX_RGB5A3:
            if (a.opaque) {
                double p = encode_etc(level0, w, h, 0, scratch);
                if (p >= etc_threshold(opt->quality)) { *psnr_out = p; return PF_ETC1; }
                *psnr_out = plain_psnr(level0, w * h, PF_RGB565);
                return PF_RGB565;
            }
            if (a.binary_alpha) { *psnr_out = plain_psnr(level0, w * h, PF_RGBA5551); return PF_RGBA5551; }
            {
                double p = encode_etc(level0, w, h, 1, scratch);
                if (p >= etc_threshold(opt->quality)) { *psnr_out = p; return PF_ETC1A4; }
            }
            *psnr_out = plain_psnr(level0, w * h, PF_RGBA4);
            return PF_RGBA4;
        case GX_RGBA8:
        default:
            if (a.opaque) {
                double p = encode_etc(level0, w, h, 0, scratch);
                if (p >= etc_threshold(opt->quality)) { *psnr_out = p; return PF_ETC1; }
                p = plain_psnr(level0, w * h, PF_RGB565);
                if (opt->quality > 0 && p >= 40.0) { *psnr_out = p; return PF_RGB565; }
                return PF_RGBA8;
            }
            {
                double p = encode_etc(level0, w, h, 1, scratch);
                if (p >= etc_threshold(opt->quality)) { *psnr_out = p; return PF_ETC1A4; }
                p = plain_psnr(level0, w * h, PF_RGBA4);
                if (opt->quality > 0 && p >= 36.0) { *psnr_out = p; return PF_RGBA4; }
            }
            return PF_RGBA8;
    }
}

// --- One image -> PicaTexHeader + levels ---------------------------------------

// Converts a decoded RGBA image. Returns bytes written to out (0 on failure).
static u32 build_image(const u8* rgba, u32 w, u32 h, int gx_fmt, int wrap_s, int wrap_t, const TcOptions* opt,
                       u8* out, u32 cap, TcStats* st) {
    u32 tw, th;
    int flags = 0;
    float us = 1.0f, vs = 1.0f;
    u8* img = NULL;
    int is_pot = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
    if (is_pot && w >= 8 && h >= 8) {
        tw = w; th = h;
        img = malloc((size_t)w * h * 4);
        memcpy(img, rgba, (size_t)w * h * 4);
    } else if (wrap_s == WRAP_CLAMP && wrap_t == WRAP_CLAMP) {
        tw = pow2_ceil(w); th = pow2_ceil(h);
        if (tw < 8) tw = 8;
        if (th < 8) th = 8;
        img = pad(rgba, w, h, tw, th);
        us = (float)w / tw;
        vs = (float)h / th;
        flags |= PTF_PADDED;
        if (st) st->npot_padded++;
    } else {
        // Repeating textures must tile exactly: resample each axis to a power
        // of two (clamped axes of a mixed texture are padded instead)
        tw = wrap_s == WRAP_CLAMP ? pow2_ceil(w) : pow2_round(w);
        th = wrap_t == WRAP_CLAMP ? pow2_ceil(h) : pow2_round(h);
        if (tw < 8) tw = 8;
        if (th < 8) th = 8;
        u32 rw = wrap_s == WRAP_CLAMP ? w : tw, rh = wrap_t == WRAP_CLAMP ? h : th;
        u8* r = resample(rgba, w, h, rw, rh, wrap_s == WRAP_CLAMP ? WRAP_CLAMP : wrap_s,
                         wrap_t == WRAP_CLAMP ? WRAP_CLAMP : wrap_t);
        img = pad(r, rw, rh, tw, th);
        free(r);
        us = (float)rw / tw;
        vs = (float)rh / th;
        if (rw != tw || rh != th) flags |= PTF_PADDED;
        flags |= PTF_RESAMPLED;
        if (st) st->npot_resampled++;
    }
    while ((int)tw > opt->max_size || (int)th > opt->max_size) {
        u8* hs = half_size(img, tw, th);
        free(img);
        img = hs;
        tw = tw > 8 ? tw / 2 : tw;
        th = th > 8 ? th / 2 : th;
    }

    int levels = 1;
    if (opt->mipmaps) {
        u32 m = tw < th ? tw : th;
        while ((m >> levels) >= 8) levels++;
    }

    u8* scratch = malloc((size_t)tw * th * 4 + 64);
    double psnr;
    int pf = choose_format(gx_fmt, img, tw, th, opt, scratch, &psnr);
    free(scratch);

    u32 total = 0;
    for (int l = 0; l < levels; l++) total += pica_level_size(pf, tw >> l, th >> l);
    if (PT_HDR_SIZE + total > cap) {
        free(img);
        return 0;
    }

    PicaTexHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = PT_MAGIC;
    hdr.key = PT_KEY;
    hdr.width = (u16)tw;
    hdr.height = (u16)th;
    hdr.format = (u8)pf;
    hdr.levels = (u8)levels;
    hdr.flags = (u8)flags;
    hdr.uscale = us;
    hdr.vscale = vs;
    hdr.data_offset = PT_HDR_SIZE;
    hdr.data_size = total;
    memcpy(out, &hdr, sizeof(hdr));

    u8* dst = out + PT_HDR_SIZE;
    u8* level = img;
    u32 lw = tw, lh = th;
    for (int l = 0; l < levels; l++) {
        if (pf == PF_ETC1 || pf == PF_ETC1A4) encode_etc(level, lw, lh, pf == PF_ETC1A4, dst);
        else encode_plain(level, lw, lh, pf, dst);
        dst += pica_level_size(pf, lw, lh);
        if (l + 1 < levels) {
            u8* next = half_size(level, lw, lh);
            if (level != img) free(level);
            level = next;
            lw /= 2;
            lh /= 2;
        }
    }
    if (level != img) free(level);
    free(img);

    if (st) {
        st->by_format[pf]++;
        if (st->worst_psnr[pf] == 0 || psnr < st->worst_psnr[pf]) st->worst_psnr[pf] = psnr;
    }
    return PT_HDR_SIZE + total;
}

static int gx_wrap(u8 v) { return v == 1 ? WRAP_REPEAT : v == 2 ? WRAP_MIRROR : WRAP_CLAMP; }

// --- Entry points (ctypes) -------------------------------------------------------------

// Converts one texture (0x60 header + data). Returns the size written to out,
// 0 to keep the original (unsupported/invalid), or -needed if out is too small.
int tc_convert(const u8* in, u32 in_size, u8* out, u32 out_cap, int quality, int mipmaps, int max_size, int scale,
               TcStats* st) {
    init_tables();
    if (in_size < 0x60) return 0;
    u32 w = be16(in + 0x0A), h = be16(in + 0x0C);
    int fmt = in[0x16];
    u32 img_off = be32(in + 0x50);
    if (w == 0 || h == 0 || w > 1024 || h > 1024) goto skip;
    if (fmt != GX_I4 && fmt != GX_I8 && fmt != GX_IA4 && fmt != GX_IA8 && fmt != GX_RGB565 && fmt != GX_RGB5A3 &&
        fmt != GX_RGBA8 && fmt != GX_CMPR)
        goto skip;
    // Already converted (magic at the start of the payload)
    if (in_size >= 0x60 + 8 && (u32)(in[0x60] | in[0x61] << 8 | in[0x62] << 16 | (u32)in[0x63] << 24) == PT_MAGIC)
        goto skip;
    if (scale < 0 || scale > 4) scale = 0;

    u32 orig_w = w, orig_h = h;
    TcOptions opt = { quality, mipmaps, max_size > 0 ? max_size : 1024 };
    u8* rgba = calloc((size_t)w * h, 4);
    if (!gx_decode(in + 0x60, in_size - 0x60, fmt, w, h, rgba)) {
        free(rgba);
        goto skip;
    }
    // TEX_SCALE: keep only every 2^scale texels (2x2 box in linear light).
    // The game header keeps the original size; the PICA header below carries
    // the reduced one, which is what the renderer uses.
    if (scale > 0) {
        u32 sw = w, sh = h;
        for (int i = 0; i < scale; i++) {
            u8* hs = half_size(rgba, (int)sw, (int)sh);
            free(rgba);
            rgba = hs;
            sw = sw > 1 ? sw / 2 : 1;
            sh = sh > 1 ? sh / 2 : 1;
        }
        w = sw;
        h = sh;
    }
    u32 cap = out_cap > 0x60 ? out_cap - 0x60 : 0;
    u8* tmp = malloc((size_t)8 * 1024 * 1024);
    u32 n = build_image(rgba, w, h, fmt, gx_wrap(in[0x17]), gx_wrap(in[0x18]), &opt, tmp, 8 * 1024 * 1024, st);
    free(rgba);
    if (!n) {
        free(tmp);
        goto skip;
    }
    u32 total = (n + 31) & ~31u;
    u32 sec_off = 0, sec_n = 0;
    u8* sec = NULL;
    if (img_off) {
        // Secondary I4 image at payload + imageOffset (see textureInitSecondaryGXTexObj)
        u32 sec_src = 0x60 + img_off;
        if (sec_src < in_size) {
            u8* srgba = calloc((size_t)orig_w * orig_h, 4);
            if (gx_decode(in + sec_src, in_size - sec_src, GX_I4, orig_w, orig_h, srgba)) {
                u8* shrunk = srgba;
                if (scale > 0) {
                    u32 sw = orig_w, sh = orig_h;
                    for (int i = 0; i < scale; i++) {
                        u8* hs = half_size(shrunk, (int)sw, (int)sh);
                        free(shrunk);
                        shrunk = hs;
                        sw = sw > 1 ? sw / 2 : 1;
                        sh = sh > 1 ? sh / 2 : 1;
                    }
                }
                sec = malloc((size_t)4 * 1024 * 1024);
                sec_n = build_image(shrunk, w, h, GX_I4, gx_wrap(in[0x17]), gx_wrap(in[0x18]), &opt, sec,
                                    4 * 1024 * 1024, st);
                free(shrunk);
                sec_off = total;
                total += (sec_n + 31) & ~31u;
            } else {
                free(srgba);
            }
        }
        if (!sec_n) {
            free(sec);
            free(tmp);
            goto skip;
        }
    }
    if (0x60 + total > out_cap) {
        free(tmp);
        free(sec);
        return -(int)(0x60 + total);
    }
    memcpy(out, in, 0x60);
    memset(out + 0x60, 0, total);
    memcpy(out + 0x60, tmp, n);
    if (sec_n) memcpy(out + 0x60 + sec_off, sec, sec_n);
    put_be32(out + 0x50, sec_off);
    // dataSize/loadedSize are recomputed by the game
    free(tmp);
    free(sec);
    if (st) {
        st->converted++;
        st->in_bytes += in_size;
        st->out_bytes += 0x60 + total;
    }
    return (int)(0x60 + total);

skip:
    if (st) st->skipped++;
    return 0;
}

// Decodes a converted level 0 back to RGBA8 (tools/tex_check.py). Returns
// 1 on success; w/h receive the stored size.
int tc_decode_pica(const u8* payload, u32 size, u8* out, u32 out_cap, u32* w_out, u32* h_out, u32* fmt_out) {
    PicaTexHeader hdr;
    if (size < sizeof(hdr)) return 0;
    memcpy(&hdr, payload, sizeof(hdr));
    if (hdr.magic != PT_MAGIC || hdr.key != PT_KEY) return 0;
    u32 w = hdr.width, h = hdr.height;
    *w_out = w;
    *h_out = h;
    *fmt_out = hdr.format;
    if ((u64)w * h * 4 > out_cap) return 0;
    const u8* d = payload + hdr.data_offset;
    int pf = hdr.format;
    if (pf == PF_ETC1 || pf == PF_ETC1A4) {
        const u8* p = d;
        for (u32 ty = 0; ty < h; ty += 8)
            for (u32 tx = 0; tx < w; tx += 8)
                for (int sb = 0; sb < 4; sb++) {
                    u32 bx = tx + (sb & 1) * 4, by = ty + (sb >> 1) * 4;
                    u64 a = ~0ull;
                    if (pf == PF_ETC1A4) {
                        a = 0;
                        for (int i = 0; i < 8; i++) a |= (u64)p[i] << (8 * i);
                        p += 8;
                    }
                    u8 dec[16][4];
                    etc1_decode_block(p, dec);
                    p += 8;
                    for (int x = 0; x < 4; x++)
                        for (int y = 0; y < 4; y++) {
                            u8* o = &out[((by + y) * w + bx + x) * 4];
                            memcpy(o, dec[y * 4 + x], 3);
                            o[3] = pf == PF_ETC1A4 ? c4to8((a >> (4 * (x * 4 + y))) & 15) : 255;
                        }
                }
        return 1;
    }
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u32 t = ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton(x & 7, y & 7);
            u8* o = &out[(y * w + x) * 4];
            u16 v;
            switch (pf) {
                case PF_RGBA8: o[0] = d[t * 4 + 3]; o[1] = d[t * 4 + 2]; o[2] = d[t * 4 + 1]; o[3] = d[t * 4]; break;
                case PF_RGB565: v = d[t * 2] | d[t * 2 + 1] << 8;
                    o[0] = c5to8(v >> 11); o[1] = c6to8((v >> 5) & 63); o[2] = c5to8(v & 31); o[3] = 255; break;
                case PF_RGBA5551: v = d[t * 2] | d[t * 2 + 1] << 8;
                    o[0] = c5to8(v >> 11); o[1] = c5to8((v >> 6) & 31); o[2] = c5to8((v >> 1) & 31); o[3] = (v & 1) * 255; break;
                case PF_RGBA4: v = d[t * 2] | d[t * 2 + 1] << 8;
                    o[0] = c4to8(v >> 12); o[1] = c4to8((v >> 8) & 15); o[2] = c4to8((v >> 4) & 15); o[3] = c4to8(v & 15); break;
                case PF_LA8: o[0] = o[1] = o[2] = d[t * 2 + 1]; o[3] = d[t * 2]; break;
                case PF_LA4: o[0] = o[1] = o[2] = c4to8(d[t] >> 4); o[3] = c4to8(d[t] & 15); break;
                case PF_L8: o[0] = o[1] = o[2] = d[t]; o[3] = 255; break;
                case PF_A8: o[0] = o[1] = o[2] = 0; o[3] = d[t]; break;
                default: return 0;
            }
        }
    return 1;
}

// Decodes a GameCube texture's level 0 (for comparison in tex_check.py)
int tc_decode_gx(const u8* in, u32 in_size, u8* out, u32 out_cap) {
    if (in_size < 0x60) return 0;
    u32 w = be16(in + 0x0A), h = be16(in + 0x0C);
    if ((u64)w * h * 4 > out_cap) return 0;
    return gx_decode(in + 0x60, in_size - 0x60, in[0x16], w, h, out);
}

int tc_stats_size(void) { return (int)sizeof(TcStats); }

// Movie frames (tools/video_convert.py): scale an RGBA image (rows top first)
// to dw x dh and encode it as PICA ETC1. out receives dw * dh / 2 bytes.
// Returns the PSNR of the encoding, or -1 on bad sizes.
double tc_encode_frame_etc1(const u8* rgba, u32 sw, u32 sh, u32 dw, u32 dh, u8* out) {
    if (!sw || !sh || dw < 8 || dh < 8 || (dw & 7) || (dh & 7)) return -1;
    init_tables();
    const u8* img = rgba;
    u8* scaled = NULL;
    if (sw != dw || sh != dh) {
        scaled = resample(rgba, (int)sw, (int)sh, (int)dw, (int)dh, WRAP_CLAMP, WRAP_CLAMP);
        img = scaled;
    }
    double psnr = encode_etc(img, dw, dh, 0, out);
    free(scaled);
    return psnr;
}
