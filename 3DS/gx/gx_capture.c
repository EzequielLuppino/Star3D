// Render capture: see gx_capture.h. Runs at frame begin, where C3D_FrameBegin
// has waited for the GPU, so the previous frame's color and depth buffers are
// complete and not cleared yet.
//
// All capture files are written by a low-priority writer thread: the render
// thread only copies pixels and stages whole-file buffers (the "capmem:"
// in-memory files), so a mark never stalls the frame for the SD card. Bug 40:
// the capture used to write everything inline on the render thread (per-line
// state dumps, per-state texture dumps, BMPs, copy PPMs) while the game
// streamed gamedata/romfs.bin and the log writer flushed; the SD card's commits stall
// for seconds each, the frame froze for up to 85 s, and the kernel took a
// deterministic prefetch abort while the fs module was current (9 identical
// dumps). See BUG40_CAPTURE_CRASH.md.
#include "gx_capture.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <time.h>
#include "config.h"
#include "gx_internal.h"
#include "log.h"

#define CAPTURE_ROOT "sdmc:/3ds/star3d/captures"

int g_gx_cap_mode;
static volatile bool s_requested;
static char s_dir[96];
static FILE* s_states;

void gx_capture_request(void) { s_requested = true; }
const char* gx_capture_dir(void) { return s_dir; }

// --- Capture writer thread -----------------------------------------------------
// One low-priority thread owns the SD for the capture: jobs are whole files
// (name + heap buffer), taken in order. The render thread blocks only while
// more than CAP_QUEUE_BYTES are staged (the SD card is slow, but a bounded
// stall is better than the old unbounded one, and in practice the queue
// stays well below the bound).

#define CAP_QUEUE_BYTES (2 * 1024 * 1024)
#define CAP_JOB_MAX 64
#define CAP_JOB_NAME 96

typedef struct {
    char name[CAP_JOB_NAME];
    u8* data;
    u32 size;
    bool append;  // A later chunk of a chunked file: open in "ab"
} CapJob;

static CapJob s_job[CAP_JOB_MAX];
static u32 s_job_head, s_job_tail;  // tail..head, free-running
static u32 s_job_bytes;             // Staged bytes
static LightLock s_cap_lock = 1;
static LightEvent s_cap_ev;
static Thread s_cap_thread;
static volatile bool s_cap_quit;
static volatile bool s_writer_wedged;  // A file write hung: drop new files at once
static volatile u32 s_cap_drops;       // Files dropped because the writer could not keep up
static volatile bool s_made_dir;       // The writer created this capture's folder
// The file the writer is working on (diagnostics; logged from the game side,
// never from the writer itself: a LOG call from the writer or under s_mem_lock
// can route through the states.txt tee back into capmem and self-deadlock)
static volatile char s_cur_file[CAP_JOB_NAME];
static volatile u64 s_cur_t0;

static void cap_write_inline(const char* name, const void* data, u32 size, bool append) {
    // The folder is created here, on the writer thread, right before the first
    // file's fopen: mkdir + fopen + write + close stay sequential on one
    // thread, like the old inline capture (a concurrent fopen into a
    // freshly-created folder is one of the fs-wedge suspects, bug 40)
    if (!s_made_dir) {
        s_made_dir = true;
        mkdir(s_dir, 0777);
    }
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    // Default stdio buffering: the fs sees small ~1 KB writes, the pattern the
    // old inline capture used for months. Large buffered writes (32 KB) wedged
    // the fs module on the console's slow SD card (bug 40, round 2).
    FILE* f = fopen(path, append ? "ab" : "wb");
    if (f) {
        fwrite(data, 1, size, f);
        fclose(f);
    }
}

static void cap_writer(void* arg) {
    (void)arg;
    while (!s_cap_quit) {
        LightEvent_Wait(&s_cap_ev);
        for (;;) {
            LightLock_Lock(&s_cap_lock);
            if (s_job_head == s_job_tail) {
                LightLock_Unlock(&s_cap_lock);
                break;
            }
            CapJob j = s_job[s_job_tail % CAP_JOB_MAX];
            s_job[s_job_tail % CAP_JOB_MAX].data = NULL;
            s_job_bytes -= j.size;
            s_job_tail++;
            LightLock_Unlock(&s_cap_lock);
            snprintf((char*)s_cur_file, sizeof(s_cur_file), "%s", j.name);
            s_cur_t0 = svcGetSystemTick();
            cap_write_inline(j.name, j.data, j.size, j.append);
            free(j.data);
            s_cur_file[0] = '\0';
            s_writer_wedged = false;  // A file went through: the SD is alive
        }
    }
    // Shutdown: write whatever is left
    for (;;) {
        LightLock_Lock(&s_cap_lock);
        if (s_job_head == s_job_tail) {
            LightLock_Unlock(&s_cap_lock);
            break;
        }
        CapJob j = s_job[s_job_tail % CAP_JOB_MAX];
        s_job[s_job_tail % CAP_JOB_MAX].data = NULL;
        s_job_bytes -= j.size;
        s_job_tail++;
        LightLock_Unlock(&s_cap_lock);
        cap_write_inline(j.name, j.data, j.size, j.append);
        free(j.data);
        s_writer_wedged = false;
    }
}

// Stage a chunk of a file: the buffer is taken over (and freed after writing).
// Without a writer thread (creation failed, or shutdown) it is written inline.
// If the writer cannot keep up (a wedged SD write) and block is false, the
// buffer is NOT handed over (the caller keeps it and retries later); with
// block the call waits up to ~10 s and then drops the file. The capture loses
// a file but the game never stalls behind it (bug 40).
// Returns false when the buffer was not staged (dropped or kept by the caller).
// No LOG calls in here: callers may hold log_lock or s_mem_lock, and a WARN
// could route through the states.txt tee back into capmem and self-deadlock.
static bool cap_write_take(const char* name, u8* data, u32 size, bool append, bool block) {
    if (!data) return true;
    if (!s_cap_thread) {
        cap_write_inline(name, data, size, append);
        free(data);
        return true;
    }
    if (s_writer_wedged) {  // A write already hung: refuse new files at once
        s_cap_drops++;
        return false;
    }
    u32 waited = 0;
    for (;;) {
        LightLock_Lock(&s_cap_lock);
        if (s_cap_quit || s_job_head - s_job_tail >= CAP_JOB_MAX - 1 || s_job_bytes + size > CAP_QUEUE_BYTES) {
            LightLock_Unlock(&s_cap_lock);
            if (s_cap_quit) {
                cap_write_inline(name, data, size, append);
                free(data);
                return true;
            }
            if (!block) return false;  // Caller keeps the buffer
            if (++waited > 5000) {     // ~10 s: give up on this file
                s_writer_wedged = true;
                s_cap_drops++;
                return false;
            }
            LightEvent_Signal(&s_cap_ev);
            svcSleepThread(2 * 1000 * 1000LL);
            continue;
        }
        CapJob* j = &s_job[s_job_head % CAP_JOB_MAX];
        snprintf(j->name, sizeof(j->name), "%s", name);
        j->data = data;
        j->size = size;
        j->append = append;
        s_job_bytes += size;
        s_job_head++;
        LightLock_Unlock(&s_cap_lock);
        LightEvent_Signal(&s_cap_ev);
        return true;
    }
}

// Wait until every staged file is written (a new capture starts only then, so
// the files of two captures never mix in one folder)
static void cap_drain(void) {
    if (!s_cap_thread || s_writer_wedged) return;
    int waited = 0;
    while (s_job_head != s_job_tail) {
        LightEvent_Signal(&s_cap_ev);
        svcSleepThread(5 * 1000 * 1000LL);
        if (++waited > 12000) {  // ~60 s: give up, the writer is stuck
            LOG_WARN("capture", "writer still busy after 60 s (on \"%s\", %lu KB staged), starting anyway",
                     (const char*)s_cur_file, (unsigned long)(s_job_bytes / 1024));
            break;
        }
    }
}

static void cap_writer_start(void) {
    if (s_cap_thread) return;
    // One-time parent folders (SD ops, done here once instead of per capture)
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/star3d", 0777);
    mkdir(CAPTURE_ROOT, 0777);
    if (AddDevice(&gx_capmem_devoptab) < 0) {
        LOG_WARN("capture", "no devoptab slot for capmem:, capture files are written inline");
    }
    LightEvent_Init(&s_cap_ev, RESET_ONESHOT);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    s_cap_thread = threadCreate(cap_writer, NULL, 16 * 1024, prio < 0x3F ? prio + 1 : prio, -2, false);
    if (!s_cap_thread) LOG_WARN("capture", "no capture writer thread: capture files are written inline");
}

void gx_capture_shutdown(void) {
    if (s_states) {  // A capture was cut short: close the open tee first
        log_set_tee(NULL);
        fclose(s_states);
        s_states = NULL;
    }
    if (!s_cap_thread) return;
    s_cap_quit = true;
    LightEvent_Signal(&s_cap_ev);
    threadJoin(s_cap_thread, U64_MAX);
    threadFree(s_cap_thread);
    s_cap_thread = NULL;
}

// --- capmem: in-memory files ----------------------------------------------------
// "capmem:/<name>" files buffer in the heap; closing one hands the buffer to
// the capture writer (or writes it inline without a writer thread). The
// render thread's capture files (states.txt tee, tex dumps, info.txt, copy
// PPMs) never touch the SD.
//
// Big files are chunked: every CAP_MEM_CHUNK bytes the buffer is staged as an
// append job and the file continues, so the heap cost of an open file stays
// bounded and the writer's backpressure spreads over the whole pass instead
// of one big enqueue at close.

#define CAP_MEM_MAX 16
#define CAP_MEM_CHUNK (512 * 1024)
#define CAP_MEM_MAXSIZE (2 * 1024 * 1024)  // Hard cap while the writer is behind: writes then fail

typedef struct {
    bool used;
    char name[CAP_JOB_NAME];
    u8* data;
    u32 size, cap, pos;  // Buffer-relative; the logical file offset is base + pos
    u32 base;            // Bytes already staged to the writer as chunks
    bool first_chunk;
} CapMemFile;

static CapMemFile s_mem[CAP_MEM_MAX];
static LightLock s_mem_lock = 1;

typedef struct { int slot; } CapMemFD;

// Try to hand the buffer to the writer (append unless it is the file's first
// chunk). block: wait up to ~10 s for space (the final close); without it the
// buffer stays here and the caller retries later. Returns false if the buffer
// was NOT handed over (still held by the memfile).
static bool capmem_flush_chunk(CapMemFile* m, bool block) {
    if (!m->size) return true;
    if (!cap_write_take(m->name, m->data, m->size, !m->first_chunk, block)) return false;
    m->first_chunk = false;
    m->base += m->size;
    m->data = NULL;
    m->size = m->cap = m->pos = 0;
    return true;
}

static int capmem_open(struct _reent* r, void* fd, const char* path, int flags, int mode) {
    (void)r; (void)flags; (void)mode;
    if (!path || strncmp(path, "capmem:/", 8) != 0 || !path[8]) return -1;
    LightLock_Lock(&s_mem_lock);
    int slot = -1;
    for (int i = 0; i < CAP_MEM_MAX; i++)
        if (!s_mem[i].used) { slot = i; break; }
    if (slot < 0) {
        LightLock_Unlock(&s_mem_lock);
        return -1;
    }
    CapMemFile* m = &s_mem[slot];
    m->used = true;
    snprintf(m->name, sizeof(m->name), "%s", path + 8);
    m->data = NULL;
    m->size = m->cap = m->pos = m->base = 0;
    m->first_chunk = true;
    ((CapMemFD*)fd)->slot = slot;
    LightLock_Unlock(&s_mem_lock);
    return 0;
}

static CapMemFile* capmem_get(void* fd) {
    CapMemFD* f = fd;
    if (!f || f->slot < 0 || f->slot >= CAP_MEM_MAX || !s_mem[f->slot].used) return NULL;
    return &s_mem[f->slot];
}

static ssize_t capmem_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void)r;
    CapMemFile* m = capmem_get(fd);
    if (!m) return -1;
    LightLock_Lock(&s_mem_lock);
    if (m->size + len > m->cap) {
        u32 cap = m->cap ? m->cap * 2 : 16 * 1024;
        while (cap < m->size + (u32)len) cap *= 2;
        u8* p = realloc(m->data, cap);
        if (!p) {
            LightLock_Unlock(&s_mem_lock);
            return -1;
        }
        m->data = p;
        m->cap = cap;
    }
    memcpy(m->data + m->pos, ptr, len);
    m->pos += (u32)len;
    if (m->pos > m->size) m->size = m->pos;
    if (m->size >= CAP_MEM_CHUNK) {
        // Hand the chunk over if the writer has room; otherwise keep buffering
        // here (never block the render thread or the log lock on the SD)
        capmem_flush_chunk(m, false);
        if (m->size >= CAP_MEM_MAXSIZE) {  // Still full: the writer is far behind
            LightLock_Unlock(&s_mem_lock);
            return -1;
        }
    }
    LightLock_Unlock(&s_mem_lock);
    return len;
}

static off_t capmem_seek(struct _reent* r, void* fd, off_t pos, int dir) {
    (void)r;
    CapMemFile* m = capmem_get(fd);
    if (!m) return -1;
    LightLock_Lock(&s_mem_lock);
    off_t logical = dir == SEEK_SET ? 0 : dir == SEEK_CUR ? (off_t)(m->base + m->pos) : (off_t)(m->base + m->size);
    off_t np = logical + pos;
    if (np < 0) np = 0;
    off_t rel = np - (off_t)m->base;
    if (rel < 0) rel = 0;
    m->pos = (u32)rel;
    if (m->pos > m->size) m->size = m->pos;
    LightLock_Unlock(&s_mem_lock);
    return np;
}

static int capmem_close(struct _reent* r, void* fd) {
    (void)r;
    CapMemFile* m = capmem_get(fd);
    if (!m) return -1;
    LightLock_Lock(&s_mem_lock);
    if (!capmem_flush_chunk(m, true)) {  // Even the blocking hand-over failed
        free(m->data);
        m->data = NULL;
    }
    m->used = false;
    LightLock_Unlock(&s_mem_lock);
    return 0;
}

devoptab_t gx_capmem_devoptab = {
    .name = "capmem",
    .structSize = sizeof(CapMemFD),
    .open_r = capmem_open,
    .close_r = capmem_close,
    .write_r = capmem_write,
    .seek_r = capmem_seek,
};

// --- Framebuffer readback -------------------------------------------------------
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

// The GPU wrote the buffers; invalidate the CPU data cache for the whole
// range before reading. VRAM is cached too: skipping it left stale 64-byte
// lines from the previous pass (the 4x4-block artifact in tint/light/normals,
// bug 34's capture note).
static void invalidate(const void* p, u32 size) {
    if (!p || !size) return;
    GSPGPU_InvalidateDataCache(p, size);
}

// 24-bit BMP, rows bottom-up and padded to 4 bytes; rgb is w*h*3 top-down.
// Built in memory and handed to the capture writer.
static void write_bmp(const char* name, const u8* rgb, u32 w, u32 h) {
    u32 row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    u8* out = malloc(size);
    if (!out) {
        LOG_WARN("capture", "no memory for %s", name);
        return;
    }
    memset(out, 0, size);
    out[0] = 'B', out[1] = 'M';
    u32 fields[] = { size, 0, 54, 40, w, h };
    memcpy(out + 2, fields, sizeof(fields));
    out[26] = 1, out[28] = 24;
    u32 img = row * h;
    memcpy(out + 34, &img, 4);
    for (u32 y = 0; y < h; y++) {
        u8* line = out + 54 + (h - 1 - y) * row;
        const u8* s = rgb + y * w * 3;
        for (u32 x = 0; x < w; x++) line[x * 3] = s[x * 3 + 2], line[x * 3 + 1] = s[x * 3 + 1], line[x * 3 + 2] = s[x * 3];
    }
    if (!cap_write_take(name, out, size, false, true)) free(out);
}

static void write_color(const char* name, const C3D_FrameBuf* fb, bool with_alpha) {
    u32 lw = fb->height, lh = fb->width;
    u8* rgb = malloc(lw * lh * 3);
    u8* a = with_alpha ? malloc(lw * lh * 3) : NULL;
    if (!rgb) {
        free(a);
        LOG_WARN("capture", "no memory for %s", name);
        return;
    }
    if (with_alpha && !a) {
        free(rgb);
        LOG_WARN("capture", "no memory for %s", name);
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
        if (!cap_write_take("depth.bin", (u8*)d, lw * lh * 4, false, true)) free(d);
        d = NULL;
    }
    free(d);
    free(rgb);
    *out_min = lo == 0xFFFFFFFF ? 0 : lo;
    *out_max = hi;
}

static void write_info(const C3D_FrameBuf* fb, int top_mode, bool lowres, u32 dmin, u32 dmax) {
    static const char* modes[] = { "mono", "stereo (left eye)", "wide", "antialiased" };
    FILE* f = fopen("capmem:/info.txt", "w");
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
    time_t now = time(NULL);
    struct tm* t = gmtime(&now);
    snprintf(s_dir, sizeof(s_dir), CAPTURE_ROOT "/%04d%02d%02d_%02d%02d%02d_f%lu", t->tm_year + 1900, t->tm_mon + 1,
             t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, (unsigned long)gx_render_frame_number());
    s_made_dir = false;  // The writer creates it (or retries) before its first fopen
    if (mkdir(s_dir, 0777) != 0) {
        // Best effort: the writer thread retries; only a failure there loses files
        LOG_WARN("capture", "cannot create %s yet (writer will retry)", s_dir);
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
        cap_drain();  // The previous capture's files, done: one folder at a time
        cap_writer_start();
        make_dir();
        write_color("color.bmp", fb, fb->colorFmt != GPU_RB_RGB8 && fb->colorFmt != GPU_RB_RGB565);
        u32 dmin = 0, dmax = 0;
        if (fb->depthBuf) write_depth(fb, &dmin, &dmax);
        // The EFB copy PPMs (~2 MB in ~15 files on a slow SD) are opt-in now:
        // S3D_CAP_COPIES=1. The copydump trigger still dumps them any time.
        if (cfg_bool("S3D_CAP_COPIES", false)) gx_copy_dump_capture();
        write_info(fb, top_mode, lowres, dmin, dmax);
        s_states = fopen("capmem:/states.txt", "w");
        if (s_states) log_set_tee(s_states);
    } else {
        static const char* names[] = { NULL, "id.bmp", "tint.bmp", "light.bmp", "normals.bmp" };
        write_color(names[mode], fb, false);
        if (mode == GX_CAP_ID && s_states) {
            log_set_tee(NULL);
            fclose(s_states);  // capmem close: hands the buffered states.txt to the writer
            s_states = NULL;
        }
    }
    g_gx_cap_mode = mode == GX_CAP_NORMALS ? GX_CAP_NONE : mode + 1;
    g_gx.dirty = GX_DIRTY_ALL;
    if (g_gx_cap_mode == GX_CAP_NONE) {
        if (s_cap_drops)
            LOG_WARN("capture", "capture passes done, %lu files dropped (writer on \"%s\"), the rest land in %s "
                     "(background)", (unsigned long)s_cap_drops, (const char*)s_cur_file, s_dir);
        else
            LOG_WARN("capture", "capture passes done, files land in %s (background)", s_dir);
        s_cap_drops = 0;
    }
}
