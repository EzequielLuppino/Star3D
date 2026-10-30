// EFB -> texture copies (GXCopyTex).
//
// The EFB is the top screen's render target, so a copy is a GPU texture copy
// (TextureCopy DMA) of a rectangle of the framebuffer into a texture, queued
// after the draws issued so far (the frame's command list is split there).
// No CPU readback and no stall.
//
// Framebuffer layout (verified by gx_selftest.c): 240x400, 8x8 tiles, EFB
// pixel (x, y) at memory column 240 - y/2 and row g_efb_x0 + x * g_efb_xs
// (40 + x/2 for the pillarboxed 4:3 image). Wide mode (240x800) and AA
// (480x800) scale rows and/or columns by 2 (g_fb_w/g_fb_h). A copied
// rectangle is therefore transposed relative to the GX texture; the texture
// matrix compensates (gx_copytex_find returns the mapping), and formats that
// read one channel (intensity, depth) are remapped in the combiners.
//
// Copies are keyed by destination address: GXInitTexObj on that address
// binds the copied texture instead of decoding memory.
//
// GXPeekZ (lens flare and sun visibility) reads the depth buffer one frame
// late: while the game peeks, the whole depth buffer is copied to linear
// memory at the end of each frame, and peeks read last frame's copy.

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "gx_init.h"
#include "gx_diag.h"
#include "gx_internal.h"
#include "log.h"
#include "profiler.h"

#define MAX_COPIES        64
#define EXPIRE_FRAMES     300  // Copies not refreshed for this long are freed
// citro3d's GX command queue holds 32 entries and panics when full; the frame
// itself needs a few (clear, command list, display transfer)
#define QUEUE_BUDGET      24


static GxCopyTex s_copies[MAX_COPIES];
// s_copies[i].dest, packed (256 bytes): gx_copytex_find runs for every bound
// texture unit, and scanning the ~150-byte entries cost up to 64 cache misses
// each (MULTICORE_PLAN.md, "Render-thread memos"). Kept in step with every
// write of dest (set_dest / the memsets)
static const void* s_copy_dest[MAX_COPIES];
// g_gx_copy_epoch changes whenever a copy's destination address does, so the
// render side's bind memo (gx_render.c bind_textures) knows "not a copy" still holds
u32 g_gx_copy_epoch;
static inline void sync_dest(const GxCopyTex* c) {
    s_copy_dest[c - s_copies] = c->dest;
    g_gx_copy_epoch++;
}
static u32 s_frame;
static int s_queue_used;

// GXPeekZ readback. Two copies used in turn: GXPeekZ reads the newest one
// the GPU has finished (gx_gpu_frame_done), which with GPU pipelining is two
// frames old, while the GPU may be writing the other.
// GXPeekZ is not recorded (gx_rec.c): the game thread calls it from the
// frame-end breakpoint callback while the render thread may still be running
// that frame and its frame end (gx_copy_frame_end), so both take s_peekz_lock
// and a copy keeps the EFB-to-screen mapping of its own frame.
#define PEEKZ_IDLE_FRAMES 60  // Stop copying when the game stops peeking
static int s_peekz_on = -1;   // S3D_PEEKZ
static struct {
    u8* buf;           // Copy of the depth buffer (same tiled layout)
    u32 size;          // Pixels buf holds
    u32 copied;        // Frame whose depth is in buf (0 = none)
    u32 gpu_frame;     // gx_render_frame_number of that copy
    u32 synced;        // Frame the CPU cache was last invalidated
    u32 w, h;          // Framebuffer size of the copy
    float scr_w, efb_x0, efb_xs;  // EFB-to-screen mapping when it was copied
} s_peekz[2];
static int s_peekz_next;      // Copy the next frame end writes
static volatile u32 s_peekz_last_use;  // Frame of the last GXPeekZ
static LightLock s_peekz_lock = 1;     // Unlocked (LightLock_Init stores 1)

#define s_src g_gx.texcopy_src
#define s_dst_fmt g_gx.texcopy_dst_fmt

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    s_src[0] = left; s_src[1] = top; s_src[2] = wd; s_src[3] = ht;
}

void GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
    (void)wd; (void)ht; (void)mipmap;  // The copy keeps the framebuffer's resolution
    s_dst_fmt = (u8)fmt;
}

// A copy texture may be in use by GPU work of this frame or (pipelined) the
// previous one: the fence frees it once this frame is done
static void free_later(C3D_Tex* tex) {
    gx_gpu_free_tex(tex, gx_render_frame_number());
}

// Debug: create sdmc:/3ds/star3d/copydump to write every copy texture (as
// last frame left it, in GX orientation) to sdmc:/3ds/star3d/copyN.ppm
#define COPYDUMP_TRIGGER "sdmc:/3ds/star3d/copydump"

void gx_copy_dump_all(void) { gx_copy_dump_dir("sdmc:/3ds/star3d"); }

void gx_copy_dump_dir(const char* dir) {
    for (int i = 0; i < MAX_COPIES; i++) {
        const GxCopyTex* c = &s_copies[i];
        if (!c->dest) continue;
        u32 w = c->src_w / 2 ? c->src_w / 2 : 1, h = c->src_h / 2 ? c->src_h / 2 : 1;
        char path[160];
        snprintf(path, sizeof(path), "%s/copy%d.ppm", dir, i);
        FILE* f = fopen(path, "wb");
        if (!f) continue;
        fprintf(f, "P6\n%lu %lu\n255\n", (unsigned long)w, (unsigned long)h);
        const u32* texels = (const u32*)c->tex.data;
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) {
                float s = (x + 0.5f) / w, t = (y + 0.5f) / h;
                float u = c->au[0] * s + c->au[1] * t + c->au[2];
                float v = c->av[0] * s + c->av[1] * t + c->av[2];
                int tx = (int)(u * c->tex.width), ty = (int)((1.0f - v) * c->tex.height);
                if (tx < 0) tx = 0;
                if (ty < 0) ty = 0;
                if (tx >= c->tex.width) tx = c->tex.width - 1;
                if (ty >= c->tex.height) ty = c->tex.height - 1;
                u32 m = 0;
                for (int b = 0; b < 3; b++) m |= ((tx >> b) & 1) << (2 * b) | ((ty >> b) & 1) << (2 * b + 1);
                u32 px = texels[((ty >> 3) * (c->tex.width >> 3) + (tx >> 3)) * 64 + m];
                u8 rgb[3] = { px >> 24, px >> 16, px >> 8 };
                fwrite(rgb, 1, 3, f);
            }
        }
        fclose(f);
        // Mip levels as stored (texture orientation, row 0 at the bottom)
        const u8* lv = c->tex.data;
        u32 lw = c->tex.width, lh = c->tex.height;
        for (int l = 0; l <= c->mip_levels && lw >= 8 && lh >= 8; l++) {
            snprintf(path, sizeof(path), "%s/copy%d_m%d.ppm", dir, i, l);
            FILE* mf = fopen(path, "wb");
            if (mf) {
                fprintf(mf, "P6\n%lu %lu\n255\n", (unsigned long)lw, (unsigned long)lh);
                const u32* tx32 = (const u32*)lv;
                for (u32 y = lh; y-- > 0;) {
                    for (u32 x = 0; x < lw; x++) {
                        u32 m = 0;
                        for (int b = 0; b < 3; b++) m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
                        u32 px = tx32[((y >> 3) * (lw >> 3) + (x >> 3)) * 64 + m];
                        u8 rgb[3] = { px >> 24, px >> 16, px >> 8 };
                        fwrite(rgb, 1, 3, mf);
                    }
                }
                fclose(mf);
            }
            lv += lw * lh * 4;
            lw /= 2;
            lh /= 2;
        }
        LOG_INFO("copy", "dumped copy%d: dest %p %ux%u tex %ux%u mips %u (frame %lu) swz %02X last %lu/%lu", i, c->dest, c->src_w,
                 c->src_h, c->tex.width, c->tex.height, c->mip_levels, (unsigned long)c->mip_frame, c->swizzle,
                 (unsigned long)c->last_frame, (unsigned long)s_frame);
    }
}

void gx_copy_frame_begin(void) {
    s_frame++;
    if (s_frame % 60 == 30 && remove(COPYDUMP_TRIGGER) == 0) {
        gx_gpu_wait_idle();  // The copies of the last frame, finished
        gx_copy_dump_all();
    }
    s_queue_used = 0;
    for (int i = 0; i < MAX_COPIES; i++) {
        GxCopyTex* c = &s_copies[i];
        if (c->dest && s_frame - c->last_frame > EXPIRE_FRAMES) {
            free_later(&c->tex);
            if (c->has_r) free_later(&c->tex_r);
            memset(c, 0, sizeof(*c));
            sync_dest(c);
        }
    }
}

// Mip aliases (port_copytex_mip_alias): a texture the game fills with a
// downscaled copy of another copy is bound as that copy at a coarser level
#define MAX_ALIASES 4
static struct { const void* dest; const void* src; u8 level; u32 frame; } s_alias[MAX_ALIASES];

static int alias_index(const void* data) {
    for (int i = 0; i < MAX_ALIASES; i++) {
        if (s_alias[i].dest == data && s_frame - s_alias[i].frame <= EXPIRE_FRAMES) return i;
    }
    return -1;
}

const GxCopyTex* gx_copytex_find(const void* data) {
    if (!data) return NULL;
    // An alias wins over an older real copy into the same texture
    int a = alias_index(data);
    if (a >= 0) data = s_alias[a].src;
    if (g_rt_memo & 1) {
        for (int i = 0; i < MAX_COPIES; i++) {
            if (s_copy_dest[i] == data) return &s_copies[i];
        }
        return NULL;
    }
    for (int i = 0; i < MAX_COPIES; i++) {
        if (s_copies[i].dest == data) return &s_copies[i];
    }
    return NULL;
}

// Copy texture in VRAM (linear memory when VRAM is full), with mip levels
// 1..levels for the GPU blur
static bool alloc_tex(C3D_Tex* tex, u16 w, u16 h, u8 levels) {
    C3D_TexInitParams p = { w, h, levels, GPU_RGBA8, GPU_TEX_2D, true };
    if (C3D_TexInitWithParams(tex, NULL, p)) return true;
    p.onVram = false;
    return C3D_TexInitWithParams(tex, NULL, p);
}

// Box-filter the texture's mip chain from level 0 with the transfer engine's
// 2x2 downscale, queued after the copy that filled level 0. Tiled -> tiled is
// BIT(5) alone (adding OUT_TILED makes the input linear, which scaling does
// not support), and the output dimensions are given unscaled: the engine
// halves them itself. Either mistake scrambles the level.
static bool gen_mips(C3D_Tex* tex, u8 levels) {
    u8* src = tex->data;
    u32 w = tex->width, h = tex->height;
    for (int l = 1; l <= levels; l++) {
        u8* dst = src + w * h * 4;
        GX_DisplayTransfer((u32*)src, GX_BUFFER_DIM(w, h), (u32*)dst, GX_BUFFER_DIM(w, h),
                           BIT(5) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                               GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_XY));
        s_queue_used++;
        gx_gpu_trace("mip transfer (w h level)", w, h, l);
        src = dst;
        w /= 2;
        h /= 2;
    }
    return true;
}

// Frames a copy keeps its mip chain after the last blur scope (DOF) used it
#define SCOPE_KEEP_FRAMES 60

// Bug 21 (hardware only): a tiled -> tiled transfer needs an input of at least
// 64x32 (3dbrew, GPU/External Registers). The pause menu's 128x128 slot shadow
// asked for level 3, whose 32x32 input hung the GPU; Azahar runs it. Level l
// reads level l-1, so l is allowed while (w >> (l-1)) >= 64 and (h >> (l-1)) >= 32
#define MIP_SRC_MIN_W 64
#define MIP_SRC_MIN_H 32

static u8 mip_levels_for(const GxCopyTex* e, u16 w, u16 h) {
    int max = C3D_TexCalcMaxLevel(w, h);
    int fit = 0;
    while (fit < max && (w >> fit) >= MIP_SRC_MIN_W && (h >> fit) >= MIP_SRC_MIN_H) fit++;
    if (max > fit) max = fit;
    int want = e->blur_level;
    if (e->scope_level > want && e->scope_frame && s_frame - e->scope_frame < SCOPE_KEEP_FRAMES)
        want = e->scope_level;
    if (e->alias_level > want && e->alias_frame && s_frame - e->alias_frame < SCOPE_KEEP_FRAMES)
        want = e->alias_level;
    if (want > fit && fit < C3D_TexCalcMaxLevel(w, h)) {
        LOG_ONCE(LOG_LEVEL_INFO, "gx", "copy %ux%u: mip levels %d -> %d (transfer minimum 64x32, bug 21)", w, h, want,
                 fit);
        PROF_COUNT("gx.copytex_mip_clamped", 1);
    }
    return (u8)(want < max ? want : max);
}

// Bug 28: a copy fills cols x rows of its power-of-two texture and the rest
// stays black (240 x 400 of 256 x 512 for a screen copy). The GameCube's
// copy is exactly the source size, so clamped coordinates past its edge
// repeat the edge pixels; here they read the padding, and bilinear filtering
// darkens the last texel. The water's warped reflection reaches up to about
// 0.012 past the screen edge (gx_tev.c s_warp_sites), under 8 texels, so one
// tile of edge pixels is enough: the last tile row copied into the next one,
// then the last tile column into the next one (corner included). Color
// copies only (S3D_COPY_EDGE, live)
static int s_edge_on = -1;  // S3D_COPY_EDGE
#define EDGE_RESERVE 2      // Queue entries left for later copies in the frame

static int edge_transfers(u32 cols, u32 rows, u16 tw, u16 th) {
    return (rows < th) + (cols < tw);
}

static void fill_edges(C3D_Tex* tex, u32 cols, u32 rows) {
    u8* data = tex->data;
    u32 row_bytes = tex->width * 32;  // One row of 8x8 RGBA8 tiles
    u32 tile_rows = rows / 8;
    if (rows < tex->height) {
        u8* src = data + (tile_rows - 1) * row_bytes;
        GX_TextureCopy((u32*)src, GX_BUFFER_DIM(row_bytes >> 4, 0), (u32*)(src + row_bytes),
                       GX_BUFFER_DIM(row_bytes >> 4, 0), row_bytes, GX_TRANSFER_RAW_COPY(1));
        s_queue_used++;
        tile_rows++;
    }
    if (cols < tex->width) {
        u8* src = data + (cols / 8 - 1) * 256;
        u32 gap = row_bytes - 256;
        GX_TextureCopy((u32*)src, GX_BUFFER_DIM(256 >> 4, gap >> 4), (u32*)(src + 256),
                       GX_BUFFER_DIM(256 >> 4, gap >> 4), 256 * tile_rows, GX_TRANSFER_RAW_COPY(1));
        s_queue_used++;
    }
    gx_gpu_trace("copytex edge fill (cols rows)", cols, rows, 0);
    PROF_COUNT("gx.copytex_edge_fill", 1);
}

static u32 pow2_at_least(u32 v) {
    u32 p = 8;
    while (p < v) p <<= 1;
    return p;
}

// Source buffer and channel remapping for a destination format
static bool format_source(u8 fmt, bool* depth, u8* swizzle) {
    *depth = false;
    switch (fmt) {
        case GX_TF_RGB565: case GX_TF_RGB5A3: case GX_TF_RGBA8:
        case GX_CTF_RG8: case GX_CTF_GB8:
            *swizzle = 0; return true;
        case GX_TF_I4: case GX_TF_I8:  // Intensity: green approximates luma
        case GX_CTF_G8:
            *swizzle = GX_SWZ(GX_SWZ_G, GX_SWZ_A_G, 0); return true;
        case GX_TF_IA4: case GX_TF_IA8:
            *swizzle = GX_SWZ(GX_SWZ_G, GX_SWZ_A_A, 0); return true;
        case GX_CTF_R4: case GX_CTF_R8:
            *swizzle = GX_SWZ(GX_SWZ_R, GX_SWZ_A_R, 0); return true;
        case GX_CTF_RA4: case GX_CTF_RA8:
            *swizzle = GX_SWZ(GX_SWZ_R, GX_SWZ_A_A, 0); return true;
        case GX_CTF_A8:
            *swizzle = GX_SWZ(GX_SWZ_A, GX_SWZ_A_A, 0); return true;
        case GX_CTF_B8:
            *swizzle = GX_SWZ(GX_SWZ_B, GX_SWZ_A_B, 0); return true;
        // Depth (D24S8 read as RGBA8: A = bits 0-7, B = 8-15, G = 16-23).
        // PICA depth is 1 - GX depth, hence the inversion.
        case GX_TF_Z8: case GX_TF_Z16: case GX_TF_Z24X8: case GX_CTF_Z4:
            *depth = true; *swizzle = GX_SWZ(GX_SWZ_G, GX_SWZ_A_G, 1); return true;
        case GX_CTF_Z8M: case GX_CTF_Z16L:
            *depth = true; *swizzle = GX_SWZ(GX_SWZ_B, GX_SWZ_A_B, 1); return true;
        case GX_CTF_Z8L:
            *depth = true; *swizzle = GX_SWZ(GX_SWZ_A, GX_SWZ_A_A, 1); return true;
        default:
            return false;
    }
}

static GxCopyTex* entry_for(const void* dest) {
    GxCopyTex* free_entry = NULL;
    for (int i = 0; i < MAX_COPIES; i++) {
        if (s_copies[i].dest == dest) return &s_copies[i];
        if (!s_copies[i].dest && !free_entry) free_entry = &s_copies[i];
    }
    if (!free_entry) {
        // Reuse the least recently copied entry
        free_entry = &s_copies[0];
        for (int i = 1; i < MAX_COPIES; i++) {
            if (s_copies[i].last_frame < free_entry->last_frame) free_entry = &s_copies[i];
        }
        free_later(&free_entry->tex);
        if (free_entry->has_r) free_later(&free_entry->tex_r);
        memset(free_entry, 0, sizeof(*free_entry));
        sync_dest(free_entry);
    }
    return free_entry;
}

void GXCopyTex(void* dest, GXBool clear) {
    C3D_RenderTarget* target = gx_copy_source_target();
    if (!dest || !target || !gx_frame_is_active()) return;
    u32 L = s_src[0], T = s_src[1], W = s_src[2], H = s_src[3];

    bool depth;
    u8 swizzle;
    if (!format_source(s_dst_fmt, &depth, &swizzle)) {
        LOG_ONCE(LOG_LEVEL_WARN, "gx", "GXCopyTex: unsupported format 0x%X", s_dst_fmt);
        PROF_COUNT("gx.copytex_skipped", 1);
        g_gx_copy_skips++;
        goto do_clear;
    }

    // Framebuffer rectangle (memory columns/rows), widened to whole tiles.
    // kx/ky: framebuffer pixels per screen pixel (2 with AA, kx 2 in wide
    // mode). The source is the top screen, or the scratch target of an
    // offscreen pass (6.1), which maps the EFB with its own g_efb_*
    // fb_w/fb_h map the EFB (the drawn area); stride is the memory layout
    int fb_w, fb_h, stride = target->frameBuf.width;
    gx_copy_source_size(&fb_w, &fb_h);
    float kx = fb_h / g_scr_w, ky = fb_w / 240.0f;
    float ys = g_efb_ys * ky, y0 = g_efb_y0 * ky;
    float xs = g_efb_xs * kx, x0 = g_efb_x0 * kx;
    int c0 = (fb_w - (int)(y0 + (T + H) * ys)) & ~7;
    int c1 = (fb_w - (int)(y0 + T * ys) + 7) & ~7;
    int r0 = (int)(x0 + L * xs) & ~7;
    int r1 = ((int)(x0 + (L + W) * xs + 0.99f) + 7) & ~7;
    if (c0 < 0) c0 = 0;
    if (c1 > fb_w) c1 = fb_w;
    if (r0 < 0) r0 = 0;
    if (r1 > fb_h) r1 = fb_h;
    u32 cols = c1 > c0 ? c1 - c0 : 0, rows = r1 > r0 ? r1 - r0 : 0;
    if (cols < 8 || rows < 8) goto do_clear;

    gx_render_flush();
    bool split = gx_render_has_drawn();
    if (s_queue_used + 1 + split > QUEUE_BUDGET) {
        LOG_ONCE(LOG_LEVEL_WARN, "gx", "GXCopyTex: too many copies in one frame (skipped)");
        PROF_COUNT("gx.copytex_over_budget", 1);
        g_gx_copy_skips++;
        goto do_clear;
    }

    GxCopyTex* e = entry_for(dest);
    u16 tw = (u16)pow2_at_least(cols), th = (u16)pow2_at_least(rows);
    u8 levels = mip_levels_for(e, tw, th);
    if (!e->dest || e->tex.width != tw || e->tex.height != th || e->mip_levels != levels) {
        u8 blur = e->blur_level, scope = e->scope_level;
        u32 scope_frame = e->scope_frame, alias_frame = e->alias_frame;
        u8 alias = e->alias_level;
        if (e->dest) free_later(&e->tex);
        if (e->has_r) free_later(&e->tex_r);
        memset(e, 0, sizeof(*e));
        sync_dest(e);
        e->blur_level = blur;
        e->scope_level = scope;
        e->scope_frame = scope_frame;
        e->alias_level = alias;
        e->alias_frame = alias_frame;
        e->mip_levels = levels;
        if (!alloc_tex(&e->tex, tw, th, levels)) {
            LOG_ONCE(LOG_LEVEL_ERROR, "gx", "GXCopyTex: out of memory for %ux%u", tw, th);
            g_gx_copy_skips++;
            goto do_clear;
        }
        e->dest = dest;
        sync_dest(e);
    }

    // Queue the copy after everything drawn so far
    if (split) {
        gx_gpu_trace_before_split();
        C3D_FrameSplit(0);
        gx_render_clear_drawn();
        s_queue_used++;
    }
    u8* fb = depth ? (u8*)target->frameBuf.depthBuf : (u8*)target->frameBuf.colorBuf;
    u8* src = fb + ((r0 / 8) * (stride / 8) + c0 / 8) * 256;
    u32 line = cols * 32, in_gap = (stride - cols) * 32, out_gap = (tw - cols) * 32;
    gx_c3d_tex_invalidate();
    GX_TextureCopy((u32*)src, GX_BUFFER_DIM(line >> 4, in_gap >> 4), (u32*)e->tex.data,
                   GX_BUFFER_DIM(line >> 4, out_gap >> 4), line * (rows / 8), GX_TRANSFER_RAW_COPY(1));
    s_queue_used++;
    gx_gpu_trace("copytex (tex wxh, cols rows, levels)", (tw << 16) | th, (cols << 16) | rows, levels);
    // Edge fill before the mips, keeping room for the mips and later copies
    CFG_LIVE(s_edge_on, cfg_bool("S3D_COPY_EDGE", true) ? 1 : 0);
    int edge = s_edge_on && !depth ? edge_transfers(cols, rows, tw, th) : 0;
    // (Azahar, hub area: the 640x480 reflection copy comes at queue 10-16 of 24)
    if (edge && s_queue_used + edge + levels + EDGE_RESERVE <= QUEUE_BUDGET) fill_edges(&e->tex, cols, rows);
    else if (edge) {
        PROF_COUNT("gx.copytex_edge_over_budget", 1);
        static u32 s_logged;
        if (s_logged < 8 && s_frame % 64 == 0) {
            s_logged++;
            LOG_INFO("gx", "copy edge fill skipped: dest %p %lux%lu (tex %ux%u, levels %u), queue %d of %d", dest,
                     (unsigned long)W, (unsigned long)H, tw, th, levels, s_queue_used, QUEUE_BUDGET);
        }
    }
    if (levels && s_queue_used + levels <= QUEUE_BUDGET && gen_mips(&e->tex, levels)) {
        e->mip_frame = s_frame;
        PROF_COUNT("gx.copytex_blur", 1);
    }

    // Stereo: the same rectangle of the right eye's half into tex_r, so
    // effects that sample the EFB (heat haze, water, blur) match each eye.
    // Over budget, the right eye falls back to the left copy.
    u8 *r_color, *r_depth;
    if (gx_right_eye_buffers(&r_color, &r_depth) && s_queue_used + 1 <= QUEUE_BUDGET) {
        if (e->has_r && (e->tex_r.width != tw || e->tex_r.height != th)) {
            free_later(&e->tex_r);
            memset(&e->tex_r, 0, sizeof(e->tex_r));
            e->has_r = false;
        }
        if (!e->has_r) e->has_r = alloc_tex(&e->tex_r, tw, th, levels);
        if (e->has_r) {
            u8* rsrc = (depth ? r_depth : r_color) + ((r0 / 8) * (stride / 8) + c0 / 8) * 256;
            GX_TextureCopy((u32*)rsrc, GX_BUFFER_DIM(line >> 4, in_gap >> 4), (u32*)e->tex_r.data,
                           GX_BUFFER_DIM(line >> 4, out_gap >> 4), line * (rows / 8), GX_TRANSFER_RAW_COPY(1));
            s_queue_used++;
            gx_gpu_trace("copytex right eye", (tw << 16) | th, (cols << 16) | rows, levels);
            if (edge && s_queue_used + edge + levels + EDGE_RESERVE <= QUEUE_BUDGET) fill_edges(&e->tex_r, cols, rows);
            e->r_frame = s_frame;
            if (levels && s_queue_used + levels <= QUEUE_BUDGET && gen_mips(&e->tex_r, levels)) {
                e->mip_frame_r = s_frame;
            }
            PROF_COUNT("gx.copytex_right", 1);
        }
    } else if (gx_right_eye_buffers(&r_color, &r_depth)) {
        PROF_COUNT("gx.copytex_right_over_budget", 1);
    }

    // Texture coordinates: u follows framebuffer columns (EFB y, reversed),
    // v follows rows (EFB x); PICA samples memory row 0 at v = 1
    float A = fb_w - y0 - T * ys - c0;
    float B = x0 + L * xs - r0;
    e->au[0] = 0.0f;                   // s
    e->au[1] = -(H * ys) / tw;         // t
    e->au[2] = A / tw;                 // q
    e->av[0] = -(W * xs) / th;
    e->av[1] = 0.0f;
    e->av[2] = 1.0f - B / th;
    e->swizzle = swizzle;
    e->src_w = (u16)W;
    e->src_h = (u16)H;
    e->fb_scale = ys;
    e->last_frame = s_frame;
    PROF_COUNT("gx.copytex", 1);
    g_gx_copies++;

do_clear:
    if (clear) gx_render_clear_efb_rect(L, T, W, H);
}

// --- GPU blur ----------------------------------------------------------------

// Replaces a CPU box blur of a copy texture's memory (newshadows.c
// boxBlurTexture), which the bound copy texture ignores. The copy gets a mip
// chain generated by the transfer engine after each refresh, and binds sample
// a coarser level with bilinear filtering: level n averages 2^n framebuffer
// pixels, magnified back as a tent of twice that, so a window of w pixels maps
// to level log2(w) - 1. Level 0 stays sharp, so repeated blurs do not
// accumulate. The level takes effect from the next copy into dest.
// Returns false (the game blurs memory) when disabled or dest is not a copy.
bool port_copytex_blur(const void* dest, int window) {
    static int s_on = -1;  // S3D_GPU_BLUR
    CFG_LIVE(s_on, cfg_bool("S3D_GPU_BLUR", true) ? 1 : 0);
    if (!s_on || !dest) return false;
    GxCopyTex* e = (GxCopyTex*)gx_copytex_find(dest);
    if (!e) return false;
    float px = window * (e->fb_scale > 0.0f ? e->fb_scale : 0.5f);
    int level = 0;
    while ((2 << level) * 2 <= px + 0.5f) level++;  // log2(px) - 1, rounded down
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    e->blur_level = (u8)level;
    PROF_COUNT("gx.gpu_blur_calls", 1);
    return true;
}

// Blurred sampling for the draws between two calls, for screen-space passes
// that sample a copy other effects need sharp (intersect_render.c
// doBlurFilter, the DOF filter: its 5-tap cross samples the screen copy at a
// coarser mip level, which turns it into a smooth blur). level > 0 starts the
// scope on dest, 0 ends it. The copy keeps generating mips while scopes use
// it, so the first frame of an effect samples level 0.
static const GxCopyTex* s_scope_copy;
static u8 s_scope_level;

bool port_copytex_blur_scope(const void* dest, int level) {
    static int s_on = -1;  // S3D_GPU_BLUR
    CFG_LIVE(s_on, cfg_bool("S3D_GPU_BLUR", true) ? 1 : 0);
    if (level <= 0 || !s_on || !dest) {
        if (s_scope_copy) gx_state_changed(GX_DIRTY_TEX);
        s_scope_copy = NULL;
        return false;
    }
    GxCopyTex* e = (GxCopyTex*)gx_copytex_find(dest);
    if (!e) return false;
    e->scope_level = (u8)(level > 4 ? 4 : level);
    e->scope_frame = s_frame ? s_frame : 1;
    s_scope_copy = e;
    s_scope_level = e->scope_level;
    gx_state_changed(GX_DIRTY_TEX);
    PROF_COUNT("gx.gpu_blur_scopes", 1);
    return true;
}

// Small copies of a whole copy (newshadows.c drawReflectionTexture, 6.2: the
// 80x60 reflection is the 320x240 reflection copy drawn at a quarter size and
// copied again). dest becomes an alias of src's mip level closest to w x h
// EFB pixels, so the redraw and the copy are skipped. The level takes effect
// from the next copy into src. Returns false (the game draws and copies) when
// disabled (S3D_REFLECT_MIP) or src is not a copy.
bool port_copytex_mip_alias(const void* dest, const void* src, int w) {
    static int s_on = -1;  // S3D_REFLECT_MIP
    if (s_on < 0) s_on = cfg_bool("S3D_REFLECT_MIP", true) ? 1 : 0;
    if (!s_on || !dest || !src || w <= 0) return false;
    GxCopyTex* e = (GxCopyTex*)gx_copytex_find(src);
    if (!e || e->dest != src || !e->src_w) return false;
    // Framebuffer pixels across src, halved per level down to about w
    float px = e->src_w * (e->fb_scale > 0.0f ? e->fb_scale : 0.5f);
    int level = 0;
    while (level < 4 && px >= 1.5f * w) {
        px *= 0.5f;
        level++;
    }
    int a = alias_index(dest);
    if (a < 0) {
        a = 0;
        for (int i = 0; i < MAX_ALIASES; i++) {
            if (!s_alias[i].dest || s_frame - s_alias[i].frame > EXPIRE_FRAMES) { a = i; break; }
        }
    }
    s_alias[a].dest = dest;
    s_alias[a].src = src;
    s_alias[a].level = (u8)level;
    s_alias[a].frame = s_frame;
    if (e->alias_level < level || s_frame - e->alias_frame >= SCOPE_KEEP_FRAMES) e->alias_level = (u8)level;
    e->alias_frame = s_frame ? s_frame : 1;
    PROF_COUNT("gx.copytex_mip_alias", 1);
    return true;
}

// Mip level a bind samples: the scope or blur level when the chain matches
// the current contents of that eye's texture, else level 0
int gx_copytex_blur_level(const GxCopyTex* c, const void* data, bool right) {
    int level = c == s_scope_copy ? s_scope_level : c->blur_level;
    if (data != c->dest) {
        int a = alias_index(data);
        if (a >= 0) level = s_alias[a].level;
    }
    if (!level) return 0;
    u32 mf = right ? c->mip_frame_r : c->mip_frame;
    u32 cf = right ? c->r_frame : c->last_frame;
    if (mf != cf) return 0;
    return level < c->mip_levels ? level : c->mip_levels;
}

// --- GXPeekZ ---------------------------------------------------------------

void gx_copy_frame_end(void) {
    if (s_peekz_on != 1 || !s_peekz_last_use || s_frame - s_peekz_last_use > PEEKZ_IDLE_FRAMES) return;
    C3D_RenderTarget* target = gx_top_target();
    if (!target || !gx_frame_is_active()) return;
    u32 fb_w = target->frameBuf.width, fb_h = target->frameBuf.height;
    // This buffer's last copy ran two frames ago at most: done before this
    // frame's copy can run (C3D_FrameEnd waits for the previous frame)
    LightLock_Lock(&s_peekz_lock);
    typeof(s_peekz[0])* pz = &s_peekz[s_peekz_next];
    if (pz->buf && fb_w * fb_h > pz->size) {
        gx_gpu_free_linear(pz->buf, pz->gpu_frame);
        pz->buf = NULL;
        pz->copied = 0;
    }
    if (!pz->buf) {
        pz->size = fb_w * fb_h;
        pz->buf = linearAlloc(pz->size * 4);
        if (!pz->buf) {
            LOG_ONCE(LOG_LEVEL_WARN, "gx", "GXPeekZ: no linear memory for the depth copy");
            s_peekz_on = 0;
            LightLock_Unlock(&s_peekz_lock);
            return;
        }
    }
    bool split = gx_render_has_drawn();
    if (s_queue_used + 1 + split > QUEUE_BUDGET) {
        PROF_COUNT("gx.peekz_over_budget", 1);
        LightLock_Unlock(&s_peekz_lock);
        return;
    }
    if (split) {
        gx_gpu_trace_before_split();
        C3D_FrameSplit(0);
        gx_render_clear_drawn();
        s_queue_used++;
    }
    u32 line = fb_w * 32;  // One row of tiles
    GX_TextureCopy((u32*)target->frameBuf.depthBuf, GX_BUFFER_DIM(line >> 4, 0), (u32*)pz->buf,
                   GX_BUFFER_DIM(line >> 4, 0), line * (fb_h / 8), GX_TRANSFER_RAW_COPY(1));
    s_queue_used++;
    gx_gpu_trace("peekz depth copy", fb_w, fb_h, 0);
    pz->copied = s_frame;
    pz->gpu_frame = gx_render_frame_number();
    pz->synced = 0;
    pz->w = fb_w;
    pz->h = fb_h;
    pz->scr_w = g_scr_w;
    pz->efb_x0 = g_efb_x0;
    pz->efb_xs = g_efb_xs;
    s_peekz_next ^= 1;
    LightLock_Unlock(&s_peekz_lock);
    PROF_COUNT("gx.peekz_copies", 1);
}

void GXPeekZ(u16 x, u16 y, u32* z) {
    if (!z) return;
    *z = 0xFFFFFF;  // "Far": nothing in front
    if (s_peekz_on < 0) s_peekz_on = cfg_bool("S3D_PEEKZ", true) ? 1 : 0;
    if (s_peekz_on != 1) return;
    s_peekz_last_use = s_frame;
    PROF_COUNT("gx.peekz_reads", 1);
    if (x >= 640 || y >= 480) return;
    // Only a copy the GPU has finished: from the previous frame, or two
    // frames back with GPU pipelining
    LightLock_Lock(&s_peekz_lock);
    typeof(s_peekz[0])* pz = NULL;
    for (int i = 0; i < 2; i++) {
        typeof(s_peekz[0])* c = &s_peekz[i];
        if (c->buf && c->copied && c->copied != s_frame && gx_gpu_frame_done(c->gpu_frame) &&
            (!pz || c->copied > pz->copied))
            pz = c;
    }
    if (!pz) {
        LightLock_Unlock(&s_peekz_lock);
        return;
    }
    if (pz->synced != s_frame) {
        GSPGPU_InvalidateDataCache(pz->buf, pz->w * pz->h * 4);
        pz->synced = s_frame;
    }
    float kx = pz->h / pz->scr_w, ky = pz->w / 240.0f;
    u32 col = pz->w - 1 - (u32)(y * 0.5f * ky), row = (u32)((pz->efb_x0 + x * pz->efb_xs) * kx);
    if (row >= pz->h) {
        LightLock_Unlock(&s_peekz_lock);
        return;
    }
    u32 tile = (row / 8) * (pz->w / 8) + col / 8;
    u32 m = 0;
    for (int b = 0; b < 3; b++) {
        m |= ((col >> b) & 1) << (2 * b);
        m |= ((row >> b) & 1) << (2 * b + 1);
    }
    const u8* px = pz->buf + (tile * 64 + m) * 4;
    u32 pica = px[0] | (px[1] << 8) | (px[2] << 16);
    LightLock_Unlock(&s_peekz_lock);
    *z = 0xFFFFFF - pica;
}
