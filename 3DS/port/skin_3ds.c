// CPU skinning on two cores (MULTICORE_PLAN.md step 1).
//
// The game blends skinned positions and normals per model with
// ObjModel_BlendVertexStream and ObjModel_BlendNormalStream (main/model.c),
// always back to back (objprint_dolphin.c, both call sites). Each stream is a
// list of 0x74-byte chunk records; a chunk copies one block of the source into
// the output and blends a range of it between two joint matrices. The game
// does this through the GameCube's locked cache (copyToCache / memcpyToCache)
// and the global GQR7 scale, so its own functions cannot run on two threads.
//
// Here every chunk is a job for helper_parallel (port/helper.c): the game
// thread and the core 1 helper take chunks until none are left. The vertex
// call only queues its chunks; the normal call that follows adds its own and
// runs both streams as one batch, so a model costs one fork-join. The math is
// the game's (same operations in the same order), with the scale passed in
// instead of read from GQR7.
//
// The block copies run first, on the game thread and in the game's order
// (blocks are rounded up to 32 bytes, so a block can overlap the next one, and
// the later chunk wins there as in the game); then the blends run in
// parallel. In place (output block == source block, the double-buffered
// vtxBuf case) nothing is copied. A blend reads only its own source range and
// writes only its own output range, so the blends are independent unless the
// outputs overlap the sources of a stream that is not in place; then the
// game's function runs unchanged. Small models (under SKIN_MIN_VERTS) run
// inline: the fork-join costs more than it saves. S3D_SKIN_JOBS=0 runs the
// game's functions.
//
// Counters: skin.models_parallel, skin.models_inline, skin.models_game,
// helper.join_wait_us. Zone: skin.blend (both streams of a model).

#include <3ds.h>
#include <string.h>
#include "foxhollow_compat.h"
#include <dolphin/os/OSFastCast.h>
#include "main/model.h"
#include "config.h"
#include "helper.h"
#include "profiler.h"
#include "ptag.h"

#define SKIN_MIN_VERTS 192   // Below this, both streams run inline
#define MAX_JOBS 256         // Chunks per model (both streams)

void __real_ObjModel_BlendVertexStream(u8* mtxs, ModelFileHeader* model, u8* animData, int* dstOffsets, u8* dstBase);
void __real_ObjModel_BlendNormalStream(u8* mtxs, ModelFileHeader* model, u8* animData, u8** outs, int quad);
void memcpyToCache(void* dst, void* src, u32 count);
void setGQR7Packed(int a, int b, int c, int d);

enum { JOB_POS, JOB_NRM, JOB_NRM_QUAD };

typedef struct {
    u8 kind;
    u32 words;          // Block size in 32-byte units (0 = 0x1000 bytes), as memcpyToCache
    const u8* src;      // Source block
    u8* dst;            // Output block
    u32 off;            // Blended range: from this byte offset in the block...
    int count;          // ...this many vertices (normal triples for JOB_NRM_QUAD)
    const u8* weights;  // Two u8 weights per vertex
    const f32* ma;
    const f32* mb;
    f32 scale;
} SkinJob;

static SkinJob s_jobs[MAX_JOBS];
static int s_njobs;

// The vertex stream queued by ObjModel_BlendVertexStream, run with the normals
static struct {
    bool pending;
    ModelFileHeader* model;
    int verts;
} s_pos;

static int s_enabled = -1;

static u32 block_bytes(u32 words) { return words ? words << 5 : 0x1000; }

static void blend_pos(const SkinJob* j) {
    const f32* ma = j->ma;
    const f32* mb = j->mb;
    const u8* w = j->weights;
    const s16* in = (const s16*)(j->src + j->off);
    s16* out = (s16*)(j->dst + j->off);
    f32 scale = j->scale;
    f32 invScale = 1.0f / scale;
    for (int i = 0; i < j->count; i++) {
        f32 w0 = (f32)w[0] * (1.0f / 128.0f);
        f32 w1 = (f32)w[1] * (1.0f / 128.0f);
        w += 2;
        f32 x = (f32)fhReadBES16(&in[0]) * invScale;
        f32 y = (f32)fhReadBES16(&in[1]) * invScale;
        f32 z = (f32)fhReadBES16(&in[2]) * invScale;
        in += 3;
        f32 ox = (ma[0] * x + ma[3] * y + ma[6] * z + ma[9]) * w0 + (mb[0] * x + mb[3] * y + mb[6] * z + mb[9]) * w1;
        f32 oy = (ma[1] * x + ma[4] * y + ma[7] * z + ma[10]) * w0 + (mb[1] * x + mb[4] * y + mb[7] * z + mb[10]) * w1;
        f32 oz = (ma[2] * x + ma[5] * y + ma[8] * z + ma[11]) * w0 + (mb[2] * x + mb[5] * y + mb[8] * z + mb[11]) * w1;
        out[0] = (s16)fhSwap16((u16)__OSf32tos16(ox * scale));
        out[1] = (s16)fhSwap16((u16)__OSf32tos16(oy * scale));
        out[2] = (s16)fhSwap16((u16)__OSf32tos16(oz * scale));
        out += 3;
    }
}

static void blend_nrm(const SkinJob* j, int per_vertex) {
    const f32* ma = j->ma;
    const f32* mb = j->mb;
    const u8* w = j->weights;
    const s8* in = (const s8*)(j->src + j->off);
    s8* out = (s8*)(j->dst + j->off);
    f32 scale = j->scale;
    f32 invScale = 1.0f / scale;
    for (int i = 0; i < j->count; i++) {
        f32 w0 = (f32)w[0] * (1.0f / 128.0f);
        f32 w1 = (f32)w[1] * (1.0f / 128.0f);
        w += 2;
        for (int k = 0; k < per_vertex; k++) {
            f32 x = (f32)in[0] * invScale;
            f32 y = (f32)in[1] * invScale;
            f32 z = (f32)in[2] * invScale;
            in += 3;
            f32 ox = (ma[0] * x + ma[3] * y + ma[6] * z) * w0 + (mb[0] * x + mb[3] * y + mb[6] * z) * w1;
            f32 oy = (ma[1] * x + ma[4] * y + ma[7] * z) * w0 + (mb[1] * x + mb[4] * y + mb[7] * z) * w1;
            f32 oz = (ma[2] * x + ma[5] * y + ma[8] * z) * w0 + (mb[2] * x + mb[5] * y + mb[8] * z) * w1;
            out[0] = __OSf32tos8(ox * scale);
            out[1] = __OSf32tos8(oy * scale);
            out[2] = __OSf32tos8(oz * scale);
            out += 3;
        }
    }
}

static void run_job(void* ctx, int index) {
    const SkinJob* j = &((const SkinJob*)ctx)[index];
    if (j->kind == JOB_POS) blend_pos(j);
    else blend_nrm(j, j->kind == JOB_NRM_QUAD ? 3 : 1);
}

// Jobs [first, first + count) can blend in any order: in place, or no output
// block overlaps any source block
static bool jobs_independent(int first, int count) {
    const u8 *src_lo = NULL, *src_hi = NULL, *dst_lo = NULL, *dst_hi = NULL;
    bool in_place = true;
    for (int i = first; i < first + count; i++) {
        const SkinJob* j = &s_jobs[i];
        u32 n = block_bytes(j->words);
        if (j->dst != j->src) in_place = false;
        if (!src_lo || j->src < src_lo) src_lo = j->src;
        if (!src_hi || j->src + n > src_hi) src_hi = j->src + n;
        if (!dst_lo || j->dst < dst_lo) dst_lo = j->dst;
        if (!dst_hi || j->dst + n > dst_hi) dst_hi = j->dst + n;
    }
    return in_place || dst_hi <= src_lo || src_hi <= dst_lo;
}

// Queues one job per chunk of a stream; false when they do not fit or overlap
static bool add_stream(const u8* chunks, int chunk_count, int kind, u8* mtxs, u8* animData, int* dstOffsets,
                       u8* dstBase, u8** outs, f32 scale, int* verts) {
    if (s_njobs + chunk_count > MAX_JOBS) return false;
    int first = s_njobs;
    for (int i = 0; i < chunk_count; i++) {
        const u8* chunk = chunks + i * 0x74;
        SkinJob* j = &s_jobs[s_njobs++];
        j->kind = (u8)kind;
        j->words = (u32)((chunk[0x73] << 5) + 0x1f) >> 5;
        j->src = animData + *(const int*)(chunk + 0x60);
        j->dst = kind == JOB_POS ? dstBase + dstOffsets[i] : outs[i];
        j->off = chunk[0x72];
        j->count = *(const u16*)(chunk + 0x70);
        j->weights = *(u8* const*)(chunk + 0x64);
        j->ma = (const f32*)(mtxs + chunk[0x6c] * 0x30);
        j->mb = (const f32*)(mtxs + chunk[0x6d] * 0x30);
        j->scale = scale;
        *verts += j->count * (kind == JOB_NRM_QUAD ? 3 : 1);
    }
    if (!jobs_independent(first, chunk_count)) {
        s_njobs = first;
        return false;
    }
    return true;
}

// Scale the game reads back from GQR7 ((a << 8) + b) << 16 | ...: 1 << (a & 63)
static f32 gqr_scale(u8 a) { return (f32)(1 << (a & 0x3f)); }

static void run_batch(int verts) {
    // The game copies each whole block, then blends a range in it
    for (int i = 0; i < s_njobs; i++) {
        const SkinJob* j = &s_jobs[i];
        if (j->dst != j->src) memcpyToCache(j->dst, (void*)j->src, j->words);
    }
    if (verts >= SKIN_MIN_VERTS && helper_available()) {
        helper_parallel(run_job, s_jobs, s_njobs);
        PROF_COUNT("skin.models_parallel", 1);
    } else {
        for (int i = 0; i < s_njobs; i++) run_job(s_jobs, i);
        PROF_COUNT("skin.models_inline", 1);
    }
    s_njobs = 0;
}

static void flush_pending_pos(void) {
    if (!s_pos.pending) return;
    s_pos.pending = false;
    PROF_ZONE("skin.blend");
    run_batch(s_pos.verts);
    setGQR7Packed(s_pos.model->unk8C[2], 7, s_pos.model->unk8C[2], 7);
}

void __wrap_ObjModel_BlendVertexStream(u8* mtxs, ModelFileHeader* model, u8* animData, int* dstOffsets, u8* dstBase) {
    PTAG_LEAF(PL_SKIN);
    if (s_enabled < 0) s_enabled = cfg_bool("S3D_SKIN_JOBS", true);
    flush_pending_pos();
    if (!s_enabled || model->vertexAnimCount == 0) {
        __real_ObjModel_BlendVertexStream(mtxs, model, animData, dstOffsets, dstBase);
        return;
    }
    int verts = 0;
    s_njobs = 0;
    if (!add_stream(model->vertexAnimEntriesRaw, model->vertexAnimCount, JOB_POS, mtxs, animData, dstOffsets,
                    dstBase, NULL, gqr_scale(model->unk8C[2]), &verts)) {
        PROF_COUNT("skin.models_game", 1);
        __real_ObjModel_BlendVertexStream(mtxs, model, animData, dstOffsets, dstBase);
        return;
    }
    s_pos.pending = true;
    s_pos.model = model;
    s_pos.verts = verts;
}

void __wrap_ObjModel_BlendNormalStream(u8* mtxs, ModelFileHeader* model, u8* animData, u8** outs, int quad) {
    PTAG_LEAF(PL_SKIN);
    if (s_enabled < 0) s_enabled = cfg_bool("S3D_SKIN_JOBS", true);
    bool with_pos = s_pos.pending && s_pos.model == model;
    if (!with_pos) flush_pending_pos();
    if (!s_enabled || model->blendAnimCount == 0) {
        flush_pending_pos();
        __real_ObjModel_BlendNormalStream(mtxs, model, animData, outs, quad);
        return;
    }
    PROF_ZONE("skin.blend");
    int verts = with_pos ? s_pos.verts : 0;
    if (!with_pos) s_njobs = 0;
    s_pos.pending = false;
    if (!add_stream(model->blendAnimEntriesRaw, model->blendAnimCount, (u8)quad ? JOB_NRM_QUAD : JOB_NRM, mtxs,
                    animData, NULL, NULL, outs, gqr_scale(model->unkB0[2]), &verts)) {
        if (with_pos) {  // The positions alone
            run_batch(s_pos.verts);
            setGQR7Packed(model->unk8C[2], 7, model->unk8C[2], 7);
        }
        PROF_COUNT("skin.models_game", 1);
        __real_ObjModel_BlendNormalStream(mtxs, model, animData, outs, quad);
        return;
    }
    run_batch(verts);
    // The game leaves GQR7 at the normal stream's scale
    setGQR7Packed(model->unkB0[2], 6, model->unkB0[2], 6);
}

// End of a frame's rendering (vi_3ds.c): a vertex stream never left queued
void port_skin_flush(void) {
    PTAG_LEAF(PL_SKIN);
    flush_pending_pos();
}
