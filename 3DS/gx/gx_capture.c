// Render capture: see gx_capture.h. Runs at frame begin, where C3D_FrameBegin
// has waited for the GPU, so the previous frame's color and depth buffers are
// complete and not cleared yet.
#include "gx_capture.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "gx_internal.h"
#include "log.h"

#define CAPTURE_ROOT "sdmc:/3ds/star3d/captures"

int g_gx_cap_mode;
static volatile bool s_requested;
static char s_dir[96];
static FILE* s_states;

void gx_capture_request(void) { s_requested = true; }
const char* gx_capture_dir(void) { return s_dir; }

// Byte offset of fb pixel (x, y) in the 8x8 tiled buffer, in pixels
static u32 tiled_index(u32 x, u32 y, u32 w) {
    u32 m = 0;
    for (int b = 0; b < 3; b++) m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    return ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + m;
}

// The buffers are portrait (fb width = screen height); images are landscape.
// Landscape (X, Y), Y = 0 at the top, -> fb (x, y)
static void land_to_fb(const C3D_FrameBuf* fb, u32 X, u32 Y, u32* x, u32* y) {
    *x = fb->width - 1 - Y;
    *y = X;
}

static void color_at(const C3D_FrameBuf* fb, u32 x, u32 y, u8 rgba[4]) {
    u32 i = tiled_index(x, y, fb->width);
    const u8* p = (const u8*)fb->colorBuf;
    switch (fb->colorFmt) {
    case GPU_RB_RGBA8: {
        u32 v = ((const u32*)p)[i];
        rgba[0] = v >> 24, rgba[1] = v >> 16, rgba[2] = v >> 8, rgba[3] = v;
        break;
    }
    case GPU_RB_RGB8:
        rgba[0] = p[i * 3 + 2], rgba[1] = p[i * 3 + 1], rgba[2] = p[i * 3], rgba[3] = 255;
        break;
    case GPU_RB_RGBA5551: {
        u16 v = ((const u16*)p)[i];
        rgba[0] = ((v >> 11) & 31) * 255 / 31, rgba[1] = ((v >> 6) & 31) * 255 / 31;
        rgba[2] = ((v >> 1) & 31) * 255 / 31, rgba[3] = (v & 1) ? 255 : 0;
        break;
    }
    case GPU_RB_RGB565: {
        u16 v = ((const u16*)p)[i];
        rgba[0] = (v >> 11) * 255 / 31, rgba[1] = ((v >> 5) & 63) * 255 / 63, rgba[2] = (v & 31) * 255 / 31;
        rgba[3] = 255;
        break;
    }
    default: {  // RGBA4
        u16 v = ((const u16*)p)[i];
        rgba[0] = (v >> 12) * 17, rgba[1] = ((v >> 8) & 15) * 17, rgba[2] = ((v >> 4) & 15) * 17, rgba[3] = (v & 15) * 17;
        break;
    }
    }
}

static u32 depth_at(const C3D_FrameBuf* fb, u32 x, u32 y) {
    u32 i = tiled_index(x, y, fb->width);
    const u8* p = (const u8*)fb->depthBuf;
    switch (fb->depthFmt) {
    case GPU_RB_DEPTH16: return ((const u16*)p)[i];
    case GPU_RB_DEPTH24: return p[i * 3] | (p[i * 3 + 1] << 8) | (p[i * 3 + 2] << 16);
    default: return ((const u32*)p)[i] & 0xFFFFFF;  // D24S8
    }
}

static int bpp_color(int fmt) { return fmt == GPU_RB_RGBA8 ? 4 : fmt == GPU_RB_RGB8 ? 3 : 2; }
static int bpp_depth(int fmt) { return fmt == GPU_RB_DEPTH16 ? 2 : fmt == GPU_RB_DEPTH24 ? 3 : 4; }

// Buffers outside VRAM can sit in the data cache
static void invalidate(const void* p, u32 size) {
    u32 a = (u32)p;
    if (a < 0x1F000000 || a >= 0x1F600000) GSPGPU_InvalidateDataCache(p, size);
}

// 24-bit BMP, rows bottom-up and padded to 4 bytes; rgb is w*h*3 top-down
static void write_bmp(const char* name, const u8* rgb, u32 w, u32 h) {
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) {
        LOG_WARN("capture", "cannot write %s", path);
        return;
    }
    u32 row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    u8 hdr[54] = { 'B', 'M' };
    u32 fields[] = { size, 0, 54, 40, w, h };
    memcpy(hdr + 2, fields, sizeof(fields));
    hdr[26] = 1, hdr[28] = 24;
    u32 img = row * h;
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    u8* line = calloc(1, row);
    for (u32 y = h; y-- > 0;) {
        const u8* s = rgb + y * w * 3;
        for (u32 x = 0; x < w; x++) line[x * 3] = s[x * 3 + 2], line[x * 3 + 1] = s[x * 3 + 1], line[x * 3 + 2] = s[x * 3];
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

static void write_color(const char* name, const C3D_FrameBuf* fb, bool with_alpha) {
    u32 lw = fb->height, lh = fb->width;
    u8* rgb = malloc(lw * lh * 3);
    u8* a = with_alpha ? malloc(lw * lh * 3) : NULL;
    if (!rgb) {
        free(a);
        return;
    }
    for (u32 Y = 0; Y < lh; Y++) {
        for (u32 X = 0; X < lw; X++) {
            u32 x, y;
            land_to_fb(fb, X, Y, &x, &y);
            u8 c[4];
            color_at(fb, x, y, c);
            u8* o = rgb + (Y * lw + X) * 3;
            o[0] = c[0], o[1] = c[1], o[2] = c[2];
            if (a) memset(a + (Y * lw + X) * 3, c[3], 3);
        }
    }
    write_bmp(name, rgb, lw, lh);
    if (a) write_bmp("alpha.bmp", a, lw, lh);
    free(rgb);
    free(a);
}

// depth.bmp: nearest = white, farthest drawn = dark gray, never drawn (0) = black
// (PICA depth clears to 0 = far); depth.bin: raw values, u32 landscape rows
static void write_depth(const C3D_FrameBuf* fb, u32* out_min, u32* out_max) {
    u32 lw = fb->height, lh = fb->width;
    u32* d = malloc(lw * lh * 4);
    u8* rgb = malloc(lw * lh * 3);
    u32 lo = 0xFFFFFFFF, hi = 0;
    if (d && rgb) {
        for (u32 Y = 0; Y < lh; Y++) {
            for (u32 X = 0; X < lw; X++) {
                u32 x, y;
                land_to_fb(fb, X, Y, &x, &y);
                u32 v = depth_at(fb, x, y);
                d[Y * lw + X] = v;
                if (v && v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
        float range = hi > lo ? (float)(hi - lo) : 1.0f;
        for (u32 i = 0; i < lw * lh; i++) {
            u8 g = d[i] ? (u8)(40 + 215.0f * (d[i] - lo) / range) : 0;
            memset(rgb + i * 3, g, 3);
        }
        write_bmp("depth.bmp", rgb, lw, lh);
        char path[160];
        snprintf(path, sizeof(path), "%s/depth.bin", s_dir);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(d, 4, lw * lh, f);
            fclose(f);
        }
    }
    free(d);
    free(rgb);
    *out_min = lo == 0xFFFFFFFF ? 0 : lo;
    *out_max = hi;
}

static void write_info(const C3D_FrameBuf* fb, int top_mode, bool lowres, u32 dmin, u32 dmax) {
    static const char* modes[] = { "mono", "stereo (left eye)", "wide", "antialiased" };
    char path[160];
    snprintf(path, sizeof(path), "%s/info.txt", s_dir);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "frame %lu\nmode %s%s\n", (unsigned long)gx_render_frame_number(),
            top_mode >= 0 && top_mode < 4 ? modes[top_mode] : "?", lowres ? ", low resolution target" : "");
    fprintf(f, "framebuffer %ux%u (portrait; images are %ux%u landscape) color fmt %d depth fmt %d\n", fb->width,
            fb->height, fb->height, fb->width, fb->colorFmt, fb->depthFmt);
    fprintf(f, "depth range drawn %lu..%lu (0 = cleared = far, larger = nearer)\n", (unsigned long)dmin,
            (unsigned long)dmax);
    fputs("\nfiles (frame N = the marked frame; each pass is one later frame):\n"
          "color.bmp     frame N as drawn\n"
          "alpha.bmp     its alpha channel\n"
          "depth.bmp     its depth: white nearest, dark gray farthest drawn, black never drawn\n"
          "depth.bin     raw depth, u32 per pixel, landscape rows top to bottom\n"
          "copyN.ppm     GXCopyTex textures of frame N (copyN_mM: mip levels as stored, upside down)\n"
          "id.bmp        frame N+1, each state in a flat color: index = R/17 + (G/17)*16 + (B/85)*256\n"
          "states.txt    log of frame N+1: per state (\"state #index\") TEV program, units, blend, lights\n"
          "tint.bmp      frame N+2, TEV fallbacks: magenta >6 stages truncated, yellow approximated,\n"
          "              cyan dropped texture, green indirect\n"
          "light.bmp     frame N+3, rasterized vertex color (GX lighting result) only, no textures;\n"
          "              black = channel off (models lit by normal-texgen ramp textures)\n"
          "normals.bmp   frame N+4, view-space normal as color (x right = red, y up = green, z to\n"
          "              viewer = blue; 128 = 0); flat 128 gray/black = no normal\n",
          f);
    fclose(f);
}

static bool make_dir(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/star3d", 0777);
    mkdir(CAPTURE_ROOT, 0777);
    time_t now = time(NULL);
    struct tm* t = gmtime(&now);
    snprintf(s_dir, sizeof(s_dir), CAPTURE_ROOT "/%04d%02d%02d_%02d%02d%02d_f%lu", t->tm_year + 1900, t->tm_mon + 1,
             t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, (unsigned long)gx_render_frame_number());
    if (mkdir(s_dir, 0777) != 0) {
        LOG_WARN("capture", "cannot create %s", s_dir);
        return false;
    }
    return true;
}

void gx_capture_frame(const C3D_FrameBuf* fb, int top_mode, bool lowres) {
    if (!s_requested && g_gx_cap_mode == GX_CAP_NONE) return;
    if (!fb || !fb->colorBuf) return;
    gx_gpu_wait_idle();  // Pipelined, the previous frame may still be drawing
    invalidate(fb->colorBuf, fb->width * fb->height * bpp_color(fb->colorFmt));
    if (fb->depthBuf) invalidate(fb->depthBuf, fb->width * fb->height * bpp_depth(fb->depthFmt));

    int mode = g_gx_cap_mode;
    if (mode == GX_CAP_NONE) {
        s_requested = false;
        if (!make_dir()) return;
        write_color("color.bmp", fb, fb->colorFmt != GPU_RB_RGB8 && fb->colorFmt != GPU_RB_RGB565);
        u32 dmin = 0, dmax = 0;
        if (fb->depthBuf) write_depth(fb, &dmin, &dmax);
        gx_copy_dump_dir(s_dir);
        write_info(fb, top_mode, lowres, dmin, dmax);
        char path[160];
        snprintf(path, sizeof(path), "%s/states.txt", s_dir);
        s_states = fopen(path, "w");
        if (s_states) log_set_tee(s_states);
    } else {
        static const char* names[] = { NULL, "id.bmp", "tint.bmp", "light.bmp", "normals.bmp" };
        write_color(names[mode], fb, false);
        if (mode == GX_CAP_ID && s_states) {
            log_set_tee(NULL);
            fclose(s_states);
            s_states = NULL;
        }
    }
    g_gx_cap_mode = mode == GX_CAP_NORMALS ? GX_CAP_NONE : mode + 1;
    g_gx.dirty = GX_DIRTY_ALL;
    if (g_gx_cap_mode == GX_CAP_NONE) LOG_WARN("capture", "capture written to %s", s_dir);
}
