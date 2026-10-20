// On-device benchmarks (GRAPHICS_PLAN.md section 1.1). Azahar does not model
// the New 3DS clock boost or GPU timing, so design decisions (texture
// compression, stored vs compressed assets, fur shell counts, stereo budget)
// need numbers from real hardware. They are measured once at boot and written
// to Star3D.log ("bench" lines). Set S3D_BENCH=0 in env.txt to skip
// them (about 2 seconds).

#include "bench.h"
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "bench_shbin.h"
#include "config.h"
#include "log.h"
#include "worker.h"

static double ms_since(u64 t0) {
    return (double)(svcGetSystemTick() - t0) / CPU_TICKS_PER_MSEC;
}

// --- Storage ---------------------------------------------------------------------

static const char* const s_bench_files[] = {
    "romfs:/files/TEX1.bin", "romfs:/files/MODELS.bin", "romfs:/files/streams/sfx_0a.adp",
};

static void bench_io(void) {
    FILE* f = NULL;
    const char* path = NULL;
    long size = 0;
    for (unsigned i = 0; i < sizeof(s_bench_files) / sizeof(s_bench_files[0]) && !f; i++) {
        f = fopen(s_bench_files[i], "rb");
        path = s_bench_files[i];
    }
    if (!f) {
        LOG_WARN("bench", "io: no RomFS file to read");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    u8* buf = malloc(1 << 20);
    if (!buf || size < (2 << 20)) {
        LOG_WARN("bench", "io: %s too small (%ld bytes) or no memory", path, size);
        free(buf);
        fclose(f);
        return;
    }
    static const struct { u32 bytes; u32 count; bool random; } tests[] = {
        { 4096, 256, false }, { 65536, 32, false }, { 1 << 20, 4, false },
        { 4096, 256, true }, { 65536, 32, true },
    };
    u32 rng = 12345;
    for (unsigned t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
        u32 bytes = tests[t].bytes;
        long span = size - bytes;
        u64 t0 = svcGetSystemTick();
        long pos = 0;
        for (u32 i = 0; i < tests[t].count; i++) {
            if (tests[t].random) {
                rng = rng * 1103515245u + 12345u;
                pos = (long)((u64)rng * (u64)span >> 32) & ~4095L;
            } else if (pos + (long)bytes > size) {
                pos = 0;
            }
            fseek(f, pos, SEEK_SET);
            fread(buf, 1, bytes, f);
            if (!tests[t].random) pos += bytes;
        }
        double ms = ms_since(t0);
        double mb = (double)bytes * tests[t].count / (1 << 20);
        LOG_INFO("bench", "io %-10s %7lu B x %3lu: %7.2f MB/s, %8.1f us/read", tests[t].random ? "random" : "sequential",
                 (unsigned long)bytes, (unsigned long)tests[t].count, mb / (ms / 1000.0),
                 ms * 1000.0 / tests[t].count);
    }
    free(buf);
    fclose(f);
}

// --- Decompression ---------------------------------------------------------------------
// Minimal LZ4 block format (greedy compressor, standard decoder), to compare
// against zlib before deciding how build-time assets are stored.

static int lz4_compress(const u8* src, int n, u8* dst, int cap) {
    enum { HASH_BITS = 14 };
    static u32 table[1 << HASH_BITS];
    memset(table, 0xFF, sizeof(table));
    int ip = 0, anchor = 0, op = 0;
    while (ip + 12 < n) {
        u32 seq;
        memcpy(&seq, src + ip, 4);
        u32 h = (seq * 2654435761u) >> (32 - HASH_BITS);
        u32 ref = table[h];
        table[h] = ip;
        u32 cand = 0;
        if (ref != 0xFFFFFFFF) memcpy(&cand, src + ref, 4);
        if (ref == 0xFFFFFFFF || ip - (int)ref > 65535 || cand != seq) {
            ip++;
            continue;
        }
        int len = 4;
        while (ip + len < n - 5 && src[ref + len] == src[ip + len]) len++;
        int lit = ip - anchor;
        if (op + lit + 16 + lit / 255 > cap) return -1;
        u8* token = &dst[op++];
        *token = (u8)((lit >= 15 ? 15 : lit) << 4);
        if (lit >= 15) { int l = lit - 15; while (l >= 255) { dst[op++] = 255; l -= 255; } dst[op++] = (u8)l; }
        memcpy(dst + op, src + anchor, lit);
        op += lit;
        u16 off = (u16)(ip - ref);
        dst[op++] = off & 0xFF;
        dst[op++] = off >> 8;
        int ml = len - 4;
        *token |= (u8)(ml >= 15 ? 15 : ml);
        if (ml >= 15) { int l = ml - 15; while (l >= 255) { dst[op++] = 255; l -= 255; } dst[op++] = (u8)l; }
        ip += len;
        anchor = ip;
    }
    int lit = n - anchor;
    if (op + lit + 16 + lit / 255 > cap) return -1;
    dst[op++] = (u8)((lit >= 15 ? 15 : lit) << 4);
    if (lit >= 15) { int l = lit - 15; while (l >= 255) { dst[op++] = 255; l -= 255; } dst[op++] = (u8)l; }
    memcpy(dst + op, src + anchor, lit);
    return op + lit;
}

static int lz4_decompress(const u8* src, int n, u8* dst, int cap) {
    const u8* ip = src;
    const u8* end = src + n;
    u8* op = dst;
    u8* oend = dst + cap;
    while (ip < end) {
        u8 token = *ip++;
        int lit = token >> 4;
        if (lit == 15) { u8 b; do { b = *ip++; lit += b; } while (b == 255); }
        if (op + lit > oend) return -1;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip >= end) break;
        int off = ip[0] | (ip[1] << 8);
        ip += 2;
        int ml = (token & 15) + 4;
        if ((token & 15) == 15) { u8 b; do { b = *ip++; ml += b; } while (b == 255); }
        const u8* ref = op - off;
        if (ref < dst || op + ml > oend) return -1;
        while (ml--) *op++ = *ref++;
    }
    return op - dst;
}

typedef struct {
    const u8* zdata; uLong zsize;
    const u8* sdata; uLong ssize;  // zlib "stored" (level 0)
    const u8* ldata; int lsize;
    u8* out; u32 n;
    double zlib_ms, stored_ms, lz4_ms;
} InflateJob;

static void run_inflate(void* arg) {
    InflateJob* j = arg;
    enum { REPS = 3 };
    u64 t0 = svcGetSystemTick();
    for (int r = 0; r < REPS; r++) { uLongf len = j->n; uncompress(j->out, &len, j->zdata, j->zsize); }
    j->zlib_ms = ms_since(t0) / REPS;
    t0 = svcGetSystemTick();
    for (int r = 0; r < REPS; r++) { uLongf len = j->n; uncompress(j->out, &len, j->sdata, j->ssize); }
    j->stored_ms = ms_since(t0) / REPS;
    t0 = svcGetSystemTick();
    for (int r = 0; r < REPS; r++) lz4_decompress(j->ldata, j->lsize, j->out, j->n);
    j->lz4_ms = ms_since(t0) / REPS;
}

static void bench_decompress(void) {
    const u32 n = 1 << 20;
    u8* src = malloc(n);
    uLong zcap = compressBound(n);
    u8* z = malloc(zcap);
    u8* st = malloc(zcap);
    u8* l = malloc(n + n / 128 + 64);
    u8* out = malloc(n);
    if (!src || !z || !st || !l || !out) goto done;

    // Texture-like data: smooth gradients, repeated tiles and some noise
    u32 rng = 1;
    for (u32 i = 0; i < n; i++) {
        rng = rng * 1664525u + 1013904223u;
        u32 x = i & 255, y = (i >> 8) & 255;
        src[i] = (u8)((x + y) / 2 + ((rng >> 28) & 3) + (((i >> 12) & 7) == 3 ? (rng >> 24) : 0));
    }
    uLong zsize = zcap, ssize = zcap;
    compress2(z, &zsize, src, n, 6);
    compress2(st, &ssize, src, n, 0);
    int lsize = lz4_compress(src, n, l, n + n / 128 + 64);
    if (lsize < 0) goto done;

    InflateJob job = { z, zsize, st, ssize, l, lsize, out, n, 0, 0, 0 };
    run_inflate(&job);
    LOG_INFO("bench", "decompress core 0: zlib %.1f MB/s (ratio %.2f), zlib-stored %.1f MB/s, lz4 %.1f MB/s (ratio %.2f)",
             1000.0 / job.zlib_ms, (double)zsize / n, 1000.0 / job.stored_ms, 1000.0 / job.lz4_ms, (double)lsize / n);
    if (worker_core() >= 0) {
        InflateJob job2 = job;
        WorkerJob wj;
        worker_submit(&wj, run_inflate, &job2);
        worker_wait(&wj);
        LOG_INFO("bench", "decompress core %d: zlib %.1f MB/s, zlib-stored %.1f MB/s, lz4 %.1f MB/s", worker_core(),
                 1000.0 / job2.zlib_ms, 1000.0 / job2.stored_ms, 1000.0 / job2.lz4_ms);
    }

    // memcpy bandwidth (the texture upload path before unified memory)
    u8* lin_a = linearAlloc(n);
    u8* lin_b = linearAlloc(n);
    if (lin_a && lin_b) {
        u64 t0 = svcGetSystemTick();
        for (int r = 0; r < 8; r++) memcpy(out, src, n);
        double heap_ms = ms_since(t0) / 8;
        t0 = svcGetSystemTick();
        for (int r = 0; r < 8; r++) memcpy(lin_a, src, n);
        double h2l_ms = ms_since(t0) / 8;
        t0 = svcGetSystemTick();
        for (int r = 0; r < 8; r++) memcpy(lin_b, lin_a, n);
        double l2l_ms = ms_since(t0) / 8;
        t0 = svcGetSystemTick();
        for (int r = 0; r < 8; r++) GSPGPU_FlushDataCache(lin_b, n);
        double flush_ms = ms_since(t0) / 8;
        LOG_INFO("bench", "memcpy heap->heap %.0f MB/s, heap->linear %.0f MB/s, linear->linear %.0f MB/s, "
                 "dcache flush %.0f MB/s", 1000.0 / heap_ms, 1000.0 / h2l_ms, 1000.0 / l2l_ms, 1000.0 / flush_ms);
    }
    if (lin_a) linearFree(lin_a);
    if (lin_b) linearFree(lin_b);

done:
    free(src);
    free(z);
    free(st);
    free(l);
    free(out);
}

// --- GPU -------------------------------------------------------------------------------

typedef struct {
    float pos[3];
    u8 color[4];
    float tc[2];
} BenchVtx;  // 24 bytes

typedef struct {
    s16 pos[4];
    u8 color[4];
    s16 tc[2];
} BenchVtx16;  // 16 bytes

#define QUADS_PER_TEST 16
#define TRI_VERTS      30000

static DVLB_s* s_dvlb;
static shaderProgram_s s_prog;
static int s_loc_posscale;
static C3D_RenderTarget* s_target;
static BenchVtx* s_quad;      // QUADS_PER_TEST full-screen quads (6 vertices each)
static BenchVtx* s_tris;      // TRI_VERTS tiny triangles
static BenchVtx16* s_tris16;
static C3D_Tex s_tex_rgba8, s_tex_etc1, s_tex_rgb565;
static C3D_AttrInfo s_attr_f, s_attr_s;

static void set_quads(void) {
    for (int q = 0; q < QUADS_PER_TEST; q++) {
        static const float xy[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
        for (int i = 0; i < 6; i++) {
            BenchVtx* v = &s_quad[q * 6 + i];
            v->pos[0] = xy[i][0];
            v->pos[1] = xy[i][1];
            v->pos[2] = -0.5f;
            v->color[0] = 200; v->color[1] = 120; v->color[2] = 60; v->color[3] = 128;
            v->tc[0] = (xy[i][0] + 1) * 0.5f;
            v->tc[1] = (xy[i][1] + 1) * 0.5f;
        }
    }
    u32 rng = 7;
    for (int i = 0; i < TRI_VERTS; i++) {
        if (i % 3 == 0) rng = rng * 1103515245u + 12345u;
        float cx = ((rng >> 8) & 1023) / 512.0f - 1.0f, cy = ((rng >> 18) & 1023) / 512.0f - 1.0f;
        float dx = (i % 3 == 1) ? 0.01f : 0.0f, dy = (i % 3 == 2) ? 0.01f : 0.0f;
        BenchVtx* v = &s_tris[i];
        v->pos[0] = cx + dx; v->pos[1] = cy + dy; v->pos[2] = -0.5f;
        memset(v->color, 255, 4);
        v->tc[0] = v->tc[1] = 0;
        BenchVtx16* w = &s_tris16[i];
        w->pos[0] = (s16)(v->pos[0] * 32767); w->pos[1] = (s16)(v->pos[1] * 32767); w->pos[2] = (s16)(-0.5f * 32767);
        w->pos[3] = 0;
        memset(w->color, 255, 4);
        w->tc[0] = w->tc[1] = 0;
    }
    GSPGPU_FlushDataCache(s_quad, sizeof(BenchVtx) * QUADS_PER_TEST * 6);
    GSPGPU_FlushDataCache(s_tris, sizeof(BenchVtx) * TRI_VERTS);
    GSPGPU_FlushDataCache(s_tris16, sizeof(BenchVtx16) * TRI_VERTS);
}

static bool gpu_setup(void) {
    s_dvlb = DVLB_ParseFile((u32*)bench_shbin, bench_shbin_size);
    if (!s_dvlb) return false;
    shaderProgramInit(&s_prog);
    shaderProgramSetVsh(&s_prog, &s_dvlb->DVLE[0]);
    s_loc_posscale = shaderInstanceGetUniformLocation(s_prog.vertexShader, "posscale");
    s_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    s_quad = linearAlloc(sizeof(BenchVtx) * QUADS_PER_TEST * 6);
    s_tris = linearAlloc(sizeof(BenchVtx) * TRI_VERTS);
    s_tris16 = linearAlloc(sizeof(BenchVtx16) * TRI_VERTS);
    if (!s_target || !s_quad || !s_tris || !s_tris16) return false;
    if (!C3D_TexInit(&s_tex_rgba8, 256, 256, GPU_RGBA8) || !C3D_TexInit(&s_tex_etc1, 256, 256, GPU_ETC1) ||
        !C3D_TexInit(&s_tex_rgb565, 256, 256, GPU_RGB565))
        return false;
    u32 rng = 99;
    C3D_Tex* texs[3] = { &s_tex_rgba8, &s_tex_etc1, &s_tex_rgb565 };
    for (int t = 0; t < 3; t++) {
        u8* d = texs[t]->data;
        for (u32 i = 0; i < texs[t]->size; i++) { rng = rng * 1664525u + 1013904223u; d[i] = rng >> 24; }
        C3D_TexFlush(texs[t]);
        C3D_TexSetFilter(texs[t], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(texs[t], GPU_REPEAT, GPU_REPEAT);
    }
    set_quads();
    AttrInfo_Init(&s_attr_f);
    AttrInfo_AddLoader(&s_attr_f, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&s_attr_f, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&s_attr_f, 2, GPU_FLOAT, 2);
    AttrInfo_Init(&s_attr_s);
    AttrInfo_AddLoader(&s_attr_s, 0, GPU_SHORT, 4);
    AttrInfo_AddLoader(&s_attr_s, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&s_attr_s, 2, GPU_SHORT, 2);
    return true;
}

static void gpu_teardown(void) {
    C3D_TexDelete(&s_tex_rgba8);
    C3D_TexDelete(&s_tex_etc1);
    C3D_TexDelete(&s_tex_rgb565);
    if (s_quad) linearFree(s_quad);
    if (s_tris) linearFree(s_tris);
    if (s_tris16) linearFree(s_tris16);
    if (s_target) C3D_RenderTargetDelete(s_target);
    if (s_dvlb) {
        shaderProgramFree(&s_prog);
        DVLB_Free(s_dvlb);
    }
    s_quad = NULL;
    s_tris = NULL;
    s_tris16 = NULL;
    s_target = NULL;
    s_dvlb = NULL;
}

enum {
    T_FLAT, T_RGBA8, T_ETC1, T_RGB565, T_3TEX, T_6STAGES, T_BLEND, T_LIGHT1, T_LIGHT4, T_LIGHT8, T_PROCTEX,
    T_VTX_FLOAT, T_VTX_S16, T_COUNT
};
static const char* const s_test_names[T_COUNT] = {
    "flat color", "texture RGBA8", "texture ETC1", "texture RGB565", "3 textures, 3 stages", "6 combiner stages",
    "alpha blending", "fragment light x1", "fragment light x4", "fragment light x8", "procedural texture",
    "vertices float (24 B)", "vertices s16 (16 B)",
};

static C3D_LightEnv s_lenv;
static C3D_Light s_lights[8];
static C3D_LightLut s_lut_phong;
static C3D_ProcTex s_pt;
static C3D_ProcTexLut s_pt_noise;
static C3D_ProcTexColorLut s_pt_color;

static void stage(int i, GPU_TEVSRC a, GPU_TEVSRC b, GPU_COMBINEFUNC f) {
    C3D_TexEnv* env = C3D_GetTexEnv(i);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, a, b, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(env, C3D_Both, f);
}

static void setup_test(int t) {
    for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_LightEnvBind(NULL);
    switch (t) {
        case T_FLAT: case T_VTX_FLOAT: case T_VTX_S16:
            stage(0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_REPLACE);
            break;
        case T_RGBA8: case T_ETC1: case T_RGB565:
            C3D_TexBind(0, t == T_RGBA8 ? &s_tex_rgba8 : t == T_ETC1 ? &s_tex_etc1 : &s_tex_rgb565);
            stage(0, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_MODULATE);
            break;
        case T_3TEX:
            C3D_TexBind(0, &s_tex_rgba8);
            C3D_TexBind(1, &s_tex_etc1);
            C3D_TexBind(2, &s_tex_rgb565);
            stage(0, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_MODULATE);
            stage(1, GPU_TEXTURE1, GPU_PREVIOUS, GPU_MODULATE);
            stage(2, GPU_TEXTURE2, GPU_PREVIOUS, GPU_ADD);
            break;
        case T_6STAGES:
            C3D_TexBind(0, &s_tex_rgba8);
            stage(0, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_MODULATE);
            for (int i = 1; i < 6; i++) stage(i, GPU_PREVIOUS, GPU_TEXTURE0, i & 1 ? GPU_ADD : GPU_MODULATE);
            break;
        case T_BLEND:
            stage(0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_REPLACE);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
                           GPU_ONE_MINUS_SRC_ALPHA);
            break;
        case T_LIGHT1: case T_LIGHT4: case T_LIGHT8: {
            int n = t == T_LIGHT1 ? 1 : t == T_LIGHT4 ? 4 : 8;
            C3D_LightEnvInit(&s_lenv);
            C3D_LightEnvBind(&s_lenv);
            LightLut_Phong(&s_lut_phong, 20.0f);
            C3D_LightEnvLut(&s_lenv, GPU_LUT_D0, GPU_LUTINPUT_NH, false, &s_lut_phong);
            static const C3D_Material mat = { { 0.1f, 0.1f, 0.1f }, { 0.6f, 0.6f, 0.6f }, { 0.5f, 0.5f, 0.5f },
                                              { 0, 0, 0 }, { 0, 0, 0 } };
            C3D_LightEnvMaterial(&s_lenv, &mat);
            for (int i = 0; i < n; i++) {
                C3D_LightInit(&s_lights[i], &s_lenv);
                C3D_LightColor(&s_lights[i], 1.0f, 0.9f, 0.8f);
                C3D_FVec pos = FVec4_New(i * 0.3f - 1.0f, 0.5f, 1.0f, 1.0f);
                C3D_LightPosition(&s_lights[i], &pos);
            }
            stage(0, GPU_FRAGMENT_PRIMARY_COLOR, GPU_FRAGMENT_SECONDARY_COLOR, GPU_ADD);
            break;
        }
        case T_PROCTEX: {
            C3D_ProcTexInit(&s_pt, 0, 64);
            C3D_ProcTexClamp(&s_pt, GPU_PT_MIRRORED_REPEAT, GPU_PT_MIRRORED_REPEAT);
            C3D_ProcTexNoiseCoefs(&s_pt, C3D_ProcTex_UV, 0.3f, 0.2f, 0.1f);
            C3D_ProcTexCombiner(&s_pt, false, GPU_PT_SQRT2, GPU_PT_SQRT2);
            C3D_ProcTexFilter(&s_pt, GPU_PT_LINEAR);
            C3D_ProcTexBind(0, &s_pt);
            float noise[129];
            for (int i = 0; i <= 128; i++) noise[i] = (float)((i * 37) % 128) / 128.0f;
            ProcTexLut_FromArray(&s_pt_noise, noise);
            C3D_ProcTexLutBind(GPU_LUT_NOISE, &s_pt_noise);
            float ramp[129];
            for (int i = 0; i <= 128; i++) ramp[i] = i / 128.0f;
            ProcTexLut_FromArray(&s_pt_noise, ramp);
            C3D_ProcTexLutBind(GPU_LUT_RGBMAP, &s_pt_noise);
            u32 colors[64];
            for (int i = 0; i < 64; i++) colors[i] = 0xFF000000 | (i * 4) << 16 | (255 - i * 4) << 8 | 128;
            ProcTexColorLut_Write(&s_pt_color, colors, 0, 64);
            C3D_ProcTexColorLutBind(&s_pt_color);
            stage(0, GPU_TEXTURE3, GPU_PRIMARY_COLOR, GPU_MODULATE);
            break;
        }
        default:
            break;
    }
}

static void draw_test(int t) {
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    if (t == T_VTX_S16) {
        C3D_SetAttrInfo(&s_attr_s);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_posscale, 1.0f / 32767.0f, 0, 0, 0);
        BufInfo_Add(buf, s_tris16, sizeof(BenchVtx16), 3, 0x210);
        C3D_DrawArrays(GPU_TRIANGLES, 0, TRI_VERTS);
        return;
    }
    C3D_SetAttrInfo(&s_attr_f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_posscale, 1.0f, 0, 0, 0);
    if (t == T_VTX_FLOAT) {
        BufInfo_Add(buf, s_tris, sizeof(BenchVtx), 3, 0x210);
        C3D_DrawArrays(GPU_TRIANGLES, 0, TRI_VERTS);
        return;
    }
    BufInfo_Add(buf, s_quad, sizeof(BenchVtx), 3, 0x210);
    C3D_DrawArrays(GPU_TRIANGLES, 0, QUADS_PER_TEST * 6);
}

// --- GPU cost per draw (MULTICORE_PLAN.md: in the hub the GPU is draw-bound,
// ~47 us of GPU time per draw) --------------------------------------------------
// DC_DRAWS tiny draws in one frame per test, with one kind of state changed
// between consecutive draws, timed by GSP's performance log (P3D: command list
// execution, exact GPU time). "per draw" minus the "same state" row is what
// that change costs the GPU. 3 rounds, best of each.
#define DC_DRAWS 400
#define DC_BIG_TRIS 100
enum {
    DC_SAME, DC_ELEMENTS, DC_BIG, DC_TEX, DC_TEXENV1, DC_TEXENV6, DC_SHADER, DC_ATTR, DC_BUFFER, DC_UNIFORM,
    DC_BLEND, DC_DEPTH, DC_TYPICAL, DC_COUNT
};
static const char* const s_dc_names[DC_COUNT] = {
    "same state", "same state, indexed", "same state, 100 triangles", "texture change", "combiner change (1 stage)",
    "combiner change (6 stages)", "shader program rebind", "vertex format re-set", "vertex buffer change",
    "uniform matrix upload", "blend mode change", "depth test change", "texture+combiner+buffer+matrix",
};

static shaderProgram_s s_prog2;  // Same code as s_prog: a rebind, like a vertex shader variant switch
static BenchVtx* s_dc_vtx;       // Two copies of DC_BIG_TRIS small triangles
static u16* s_dc_idx;
static C3D_AttrInfo s_attr_f2;   // Same layout as s_attr_f

static u32 dc_p3d_us(void) {
    GSPGPU_PerfLog l;
    if (R_FAILED(GSPGPU_GetPerfLog(&l))) return 0;
    return l.entries[GSPGPU_EVENT_P3D].totalDurationUs;
}

static void dc_texenv(int variant, int stages) {
    for (int i = 0; i < 6; i++) {
        C3D_TexEnv* env = C3D_GetTexEnv(i);
        C3D_TexEnvInit(env);
        if (i >= stages) continue;
        if (i == 0) C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
        else C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env, C3D_Both, variant ? GPU_ADD : GPU_MODULATE);
        C3D_TexEnvColor(env, variant ? 0xFF808080 : 0xFF404040);
    }
}

static void dc_buffer(int copy) {
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, s_dc_vtx + copy * DC_BIG_TRIS * 3, sizeof(BenchVtx), 3, 0x210);
}

static void dc_setup(void) {
    C3D_BindProgram(&s_prog);
    C3D_SetAttrInfo(&s_attr_f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_posscale, 1.0f, 0, 0, 0);
    dc_buffer(0);
    C3D_TexBind(0, &s_tex_rgba8);
    dc_texenv(0, 1);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_LightEnvBind(NULL);
}

// Changes test t's state for draw i (i alternates between two values)
static void dc_change(int t, int i) {
    int v = i & 1;
    switch (t) {
        case DC_TEX: C3D_TexBind(0, v ? &s_tex_etc1 : &s_tex_rgba8); break;
        case DC_TEXENV1: dc_texenv(v, 1); break;
        case DC_TEXENV6: dc_texenv(v, 6); break;
        case DC_SHADER: C3D_BindProgram(v ? &s_prog2 : &s_prog); break;
        case DC_ATTR: C3D_SetAttrInfo(v ? &s_attr_f2 : &s_attr_f); break;
        case DC_BUFFER: dc_buffer(v); break;
        case DC_UNIFORM: {
            C3D_Mtx m;
            Mtx_Identity(&m);
            m.r[0].x = v ? 1.0f : 0.999f;
            C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_loc_posscale, &m);
            C3D_FVUnifSet(GPU_VERTEX_SHADER, s_loc_posscale, 1.0f, 0, 0, 0);
            break;
        }
        case DC_BLEND:
            if (v) C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ZERO);
            else C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            break;
        case DC_DEPTH: C3D_DepthTest(v != 0, GPU_GEQUAL, GPU_WRITE_COLOR); break;
        case DC_TYPICAL:
            C3D_TexBind(0, v ? &s_tex_etc1 : &s_tex_rgba8);
            dc_texenv(v, 2);
            dc_buffer(v);
            dc_change(DC_UNIFORM, i);
            break;
        default: break;
    }
}

static void bench_drawcost(void) {
    s_dc_vtx = linearAlloc(sizeof(BenchVtx) * DC_BIG_TRIS * 3 * 2);
    s_dc_idx = linearAlloc(sizeof(u16) * 3);
    if (!s_dc_vtx || !s_dc_idx) {
        LOG_WARN("bench", "drawcost: out of memory");
        goto done;
    }
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < DC_BIG_TRIS * 3; i++) {
            BenchVtx* v = &s_dc_vtx[c * DC_BIG_TRIS * 3 + i];
            float cx = (i / 3 % 10) * 0.1f - 0.5f, cy = (i / 30) * 0.1f - 0.5f;
            v->pos[0] = cx + ((i % 3) == 1 ? 0.02f : 0.0f);
            v->pos[1] = cy + ((i % 3) == 2 ? 0.02f : 0.0f);
            v->pos[2] = -0.5f;
            memset(v->color, 200, 4);
            v->tc[0] = v->tc[1] = (i % 3) * 0.5f;
        }
    }
    s_dc_idx[0] = 0;
    s_dc_idx[1] = 1;
    s_dc_idx[2] = 2;
    GSPGPU_FlushDataCache(s_dc_vtx, sizeof(BenchVtx) * DC_BIG_TRIS * 3 * 2);
    GSPGPU_FlushDataCache(s_dc_idx, sizeof(u16) * 3);
    shaderProgramInit(&s_prog2);
    shaderProgramSetVsh(&s_prog2, &s_dvlb->DVLE[0]);
    AttrInfo_Init(&s_attr_f2);
    AttrInfo_AddLoader(&s_attr_f2, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(&s_attr_f2, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(&s_attr_f2, 2, GPU_FLOAT, 2);
    GSPGPU_SetPerfLogMode(true);

    double best[DC_COUNT], cpu[DC_COUNT];
    for (int t = 0; t < DC_COUNT; t++) best[t] = cpu[t] = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        for (int t = 0; t < DC_COUNT; t++) {
            // The next frame begin waits for this one, so the log is complete
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C3D_FrameDrawOn(s_target);
            dc_setup();
            C3D_FrameEnd(0);
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            u32 before = dc_p3d_us();
            C3D_FrameDrawOn(s_target);
            dc_setup();
            u64 c0 = svcGetSystemTick();
            for (int i = 0; i < DC_DRAWS; i++) {
                dc_change(t, i);
                if (t == DC_ELEMENTS) C3D_DrawElements(GPU_TRIANGLES, 3, C3D_UNSIGNED_SHORT, s_dc_idx);
                else C3D_DrawArrays(GPU_TRIANGLES, 0, t == DC_BIG ? DC_BIG_TRIS * 3 : 3);
            }
            double c = ms_since(c0);
            C3D_FrameEnd(0);
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            u32 us = dc_p3d_us() - before;
            C3D_FrameEnd(0);
            if (us > 0 && us < 1000000 && us < best[t]) best[t] = us;
            if (c < cpu[t]) cpu[t] = c;
        }
    }
    LOG_INFO("bench", "gpu per draw (%d draws, GSP P3D time; cost = per draw - same state):", DC_DRAWS);
    for (int t = 0; t < DC_COUNT; t++) {
        if (best[t] >= 1e9 || best[DC_SAME] >= 1e9) {
            LOG_INFO("bench", "  %-32s GPU n/a (no GSP performance log), CPU %5.2f us per draw", s_dc_names[t],
                     cpu[t] * 1000.0 / DC_DRAWS);
            continue;
        }
        double per = best[t] / DC_DRAWS, base = best[DC_SAME] / DC_DRAWS;
        LOG_INFO("bench", "  %-32s %8.0f us GPU, %6.2f us per draw (cost %+6.2f), CPU %5.2f us per draw", s_dc_names[t],
                 best[t], per, per - base, cpu[t] * 1000.0 / DC_DRAWS);
    }
    shaderProgramFree(&s_prog2);
done:
    if (s_dc_vtx) linearFree(s_dc_vtx);
    if (s_dc_idx) linearFree(s_dc_idx);
    s_dc_vtx = NULL;
    s_dc_idx = NULL;
}

static void bench_gpu(void) {
    if (!gpu_setup()) {
        LOG_WARN("bench", "gpu: setup failed (out of memory?)");
        gpu_teardown();
        return;
    }
    C3D_BindProgram(&s_prog);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);

    // Each test is drawn in its own frame; its GPU time is read once the next
    // frame has started (C3D_GetDrawingTime reports the last finished frame)
    float best[T_COUNT];
    for (int t = 0; t < T_COUNT; t++) best[t] = 1e9f;
    for (int rep = 0; rep < 3; rep++) {
        for (int t = 0; t <= T_COUNT; t++) {
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            if (t > 0) {
                float ms = C3D_GetDrawingTime();
                if (ms > 0 && ms < best[t - 1]) best[t - 1] = ms;
            }
            if (t == T_COUNT) {
                C3D_FrameEnd(0);
                break;
            }
            C3D_RenderTargetClear(s_target, C3D_CLEAR_ALL, 0x000000FF, 0);
            C3D_FrameDrawOn(s_target);
            setup_test(t);
            draw_test(t);
            C3D_FrameEnd(0);
        }
    }
    C3D_LightEnvBind(NULL);

    const double pixels = 400.0 * 240.0 * QUADS_PER_TEST;
    for (int t = 0; t < T_COUNT; t++) {
        if (t == T_VTX_FLOAT || t == T_VTX_S16) {
            LOG_INFO("bench", "gpu %-24s %7.3f ms for %d vertices: %6.2f Mvert/s", s_test_names[t], best[t],
                     TRI_VERTS, TRI_VERTS / (best[t] * 1000.0));
        } else {
            LOG_INFO("bench", "gpu %-24s %7.3f ms for %d full-screen quads: %7.1f Mpixel/s (%.3f ms per 400x240)",
                     s_test_names[t], best[t], QUADS_PER_TEST, pixels / (best[t] * 1000.0), best[t] / QUADS_PER_TEST);
        }
    }
    bench_drawcost();
    gpu_teardown();
}

static double cpu_loop_ms(void) {
    volatile u32 acc = 1;
    u64 t0 = svcGetSystemTick();
    for (u32 i = 0; i < 4000000; i++) acc = acc * 1664525u + i;
    return ms_since(t0);
}

static void bench_cpu_clock(void) {
    double fast = cpu_loop_ms();
    osSetSpeedupEnable(false);
    double slow = cpu_loop_ms();
    osSetSpeedupEnable(true);
    double again = cpu_loop_ms();
    LOG_INFO("bench", "cpu loop speedup on %.2f ms, off %.2f ms, on %.2f ms", fast, slow, again);
}

void bench_run(void) {
    if (!cfg_bool("S3D_BENCH", true)) return;
    LOG_INFO("bench", "--- hardware benchmarks (S3D_BENCH=0 in env.txt skips them) ---");
    u64 t0 = svcGetSystemTick();
    bench_cpu_clock();
    bench_io();
    bench_decompress();
    if (cfg_bool("S3D_BENCH_GPU", true)) bench_gpu();
    LOG_INFO("bench", "--- benchmarks done in %.0f ms ---", ms_since(t0));
    log_flush();
}
