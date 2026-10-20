// Optional timing wrappers around game functions (make PERF_WRAP=1).
// The linker's --wrap redirects calls from other translation units to
// __wrap_<name>; __real_<name> is the original. Each wrapper adds the time
// spent to a profiler counter in CPU ticks (268 ticks = 1 us).

#include <3ds.h>
#include "profiler.h"
#include "draw_src.h"

typedef struct ModelFileHeader ModelFileHeader;

#define TIMED(counter, call) do { \
        u64 t0_ = svcGetSystemTick(); \
        call; \
        PROF_COUNT(counter, svcGetSystemTick() - t0_); \
    } while (0)

// ObjModel_BlendVertexStream / ObjModel_BlendNormalStream are wrapped in every
// build by port/skin_3ds.c (zone skin.blend), sceneRender by
// port/frame_zones.c, GXCallDisplayList / GXEnd by gx/gx_rec.c

void __real_modelAnimBuildJointMatrices(int* out, u8* dst, void* animState, u8* jointData, int jointCount,
                                        u8* jointScratch, int flags, int mode);
void __wrap_modelAnimBuildJointMatrices(int* out, u8* dst, void* animState, u8* jointData, int jointCount,
                                        u8* jointScratch, int flags, int mode) {
    TIMED("perf.joint_matrices_ticks",
          __real_modelAnimBuildJointMatrices(out, dst, animState, jointData, jointCount, jointScratch, flags, mode));
}

// Main loop pieces (gameloop_main.c -> other translation units)
#define WRAP_VOID(name, params, args) \
    void __real_##name params; \
    void __wrap_##name params { TIMED("perf." #name, __real_##name args); }

// Same, and charge the draws and time inside to a draw source (draw_src.h)
#define WRAP_TAG(name, tag, params, args) \
    void __real_##name params; \
    void __wrap_##name params { \
        draw_src_push(tag); \
        TIMED("perf." #name, __real_##name args); \
        draw_src_pop(); \
    }

WRAP_VOID(Obj_UpdateAllObjects, (u8 flags), (flags))
WRAP_TAG(gameTextRun, DSRC_HUD, (void), ())
WRAP_TAG(mainLoopDoGameText, DSRC_HUD, (void), ())
WRAP_TAG(curUiDllDraw, DSRC_HUD, (int a, int b, int c, int d), (a, b, c, d))
WRAP_VOID(updateEnvironment, (int mode), (mode))
WRAP_VOID(waterFxUpdate, (float step), (step))
WRAP_VOID(trackIntersect, (void), ())
WRAP_VOID(doPendingMapLoads, (void), ())

// Inside sceneRender
typedef struct GameObject GameObject;
struct MapBlockData;
WRAP_TAG(objRender, DSRC_OBJ, (int a, int b, int c, int d, GameObject* obj, int flag), (a, b, c, d, obj, flag))
WRAP_TAG(renderMapBlock, DSRC_MAP, (struct MapBlockData* block, u8 type), (block, type))

// Draw sources only reachable through these (sceneDrawTransparentPolys queue)
typedef struct ObjModel ObjModel;
WRAP_TAG(sceneDrawTransparentPolys, DSRC_TRANSP, (void), ())
WRAP_TAG(mapBlockRender_drawLightmapIndirectPasses, DSRC_MAPPASS,
         (struct MapBlockData* block, void* shader, void* state, float (*viewMtx)[4]),
         (block, shader, state, viewMtx))
WRAP_TAG(objRenderFuzz, DSRC_FUR, (GameObject* obj), (obj))
WRAP_TAG(playerRenderFuzz, DSRC_FUR, (GameObject* obj, int p2, int pass), (obj, p2, pass))
WRAP_TAG(objDrawGroundShadow, DSRC_SHADOW, (GameObject* obj, ObjModel* model), (obj, model))
WRAP_TAG(expgfx_renderSourcePools, DSRC_EXPGFX, (uintptr_t sourceId, int mode), (sourceId, mode))
WRAP_TAG(drawGlow, DSRC_EXPGFX, (uintptr_t base, int pool), (base, pool))
WRAP_TAG(waterFxDraw, DSRC_WATER, (void), ())
int __real_objShadowRender(GameObject* obj, int mode, int unused, int frames);
int __wrap_objShadowRender(GameObject* obj, int mode, int unused, int frames) {
    int r;
    draw_src_push(DSRC_SHADOW);
    TIMED("perf.objShadowRender", r = __real_objShadowRender(obj, mode, unused, frames));
    draw_src_pop();
    return r;
}

// Inside sceneDraw
WRAP_TAG(renderClouds, DSRC_SKY, (int a, int b, int c, int d), (a, b, c, d))
WRAP_TAG(drawSkyStars, DSRC_SKY, (void), ())
WRAP_TAG(renderParticles, DSRC_EXPGFX, (void), ())
WRAP_TAG(updateReflectionTextures, DSRC_REFL, (void), ())
WRAP_TAG(drawReflectionTexture, DSRC_REFL, (void), ())
WRAP_TAG(Rcp_UpdateDistortionTextures, DSRC_POSTFX, (void), ())
WRAP_TAG(lightningRenderActive, DSRC_POSTFX, (void), ())
WRAP_VOID(updateLights, (void), ())
WRAP_TAG(screenImageDraw, DSRC_POSTFX, (u8 alpha), (alpha))
WRAP_TAG(renderMotionBlur, DSRC_POSTFX, (float alpha), (alpha))
WRAP_TAG(doHeatEffect, DSRC_POSTFX, (u8 alpha), (alpha))
WRAP_TAG(doBlurFilter, DSRC_POSTFX, (float wx, float wy, float wz, u8 p4, u8 p5), (wx, wy, wz, p4, p5))

// Matrix library call counts (no timing: these are called thousands of times)
typedef float Mtx[3][4];
typedef struct { float x, y, z; } Vec;
#define COUNT_VOID(name, params, args) \
    void __real_##name params; \
    void __wrap_##name params { PROF_COUNT("calls." #name, 1); __real_##name args; }
#define COUNT_U32(name, params, args) \
    u32 __real_##name params; \
    u32 __wrap_##name params { PROF_COUNT("calls." #name, 1); return __real_##name args; }

COUNT_VOID(C_MTXConcat, (const Mtx a, const Mtx b, Mtx ab), (a, b, ab))
COUNT_VOID(C_MTXCopy, (const Mtx a, Mtx b), (a, b))
COUNT_U32(C_MTXInverse, (const Mtx a, Mtx b), (a, b))
COUNT_U32(C_MTXInvXpose, (const Mtx a, Mtx b), (a, b))
COUNT_VOID(C_MTXMultVec, (const Mtx m, const Vec* s, Vec* d), (m, s, d))
COUNT_VOID(C_MTXMultVecSR, (const Mtx m, const Vec* s, Vec* d), (m, s, d))
COUNT_VOID(C_MTXIdentity, (Mtx m), (m))
COUNT_VOID(C_MTXTrans, (Mtx m, float x, float y, float z), (m, x, y, z))
COUNT_VOID(C_MTXScale, (Mtx m, float x, float y, float z), (m, x, y, z))
COUNT_VOID(C_MTXRotRad, (Mtx m, char axis, float r), (m, axis, r))

// THP movie decoding (aurora's THPDec.cpp)
s32 __real_THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void* work);
s32 __wrap_THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void* work) {
    s32 r;
    TIMED("perf.THPVideoDecode", r = __real_THPVideoDecode(file, tileY, tileU, tileV, work));
    return r;
}

// Heap accounting: live bytes per caller (malloc family), dumped by
// perf_heap_report() (called from the frame probe)
#include <stdlib.h>
#include <string.h>
#include "log.h"

#define TRACK_SLOTS 65536
typedef struct { void* p; u32 size; void* caller; } TrackEntry;
static TrackEntry s_track[TRACK_SLOTS];
static u32 track_hash(void* p) { return ((uintptr_t)p >> 3) * 2654435761u % TRACK_SLOTS; }
static void track_add(void* p, size_t n, void* caller) {
    if (!p) return;
    u32 h = track_hash(p);
    for (u32 i = 0; i < TRACK_SLOTS; i++, h = (h + 1) % TRACK_SLOTS) {
        if (!s_track[h].p || s_track[h].p == (void*)1) { s_track[h].p = p; s_track[h].size = n; s_track[h].caller = caller; return; }
    }
}
static void track_del(void* p) {
    if (!p) return;
    u32 h = track_hash(p);
    for (u32 i = 0; i < TRACK_SLOTS; i++, h = (h + 1) % TRACK_SLOTS) {
        if (!s_track[h].p) return;
        if (s_track[h].p == p) { s_track[h].p = (void*)1; return; }
    }
}
void perf_heap_report(void) {
    struct { void* caller; u32 bytes, count; } top[64];
    int n = 0;
    for (u32 i = 0; i < TRACK_SLOTS; i++) {
        if (!s_track[i].p || s_track[i].p == (void*)1) continue;
        int j;
        for (j = 0; j < n && top[j].caller != s_track[i].caller; j++) {}
        if (j == n) { if (n == 64) continue; top[n].caller = s_track[i].caller; top[n].bytes = 0; top[n].count = 0; n++; }
        top[j].bytes += s_track[i].size; top[j].count++;
    }
    for (int k = 0; k < 12 && k < n; k++) {
        int best = k;
        for (int j = k + 1; j < n; j++) if (top[j].bytes > top[best].bytes) best = j;
        typeof(top[0]) t = top[k]; top[k] = top[best]; top[best] = t;
        LOG_INFO("heap", "  %p: %lu KB in %lu blocks", top[k].caller, (unsigned long)(top[k].bytes / 1024), (unsigned long)top[k].count);
    }
}

void* __real_malloc(size_t n);
void* __wrap_malloc(size_t n) {
    void* p = __real_malloc(n);
    track_add(p, n, __builtin_return_address(0));
    return p;
}
void* __real_calloc(size_t a, size_t b);
void* __wrap_calloc(size_t a, size_t b) {
    void* p = __real_calloc(a, b);
    track_add(p, a * b, __builtin_return_address(0));
    return p;
}
void* __real_memalign(size_t al, size_t n);
void* __wrap_memalign(size_t al, size_t n) {
    void* p = __real_memalign(al, n);
    track_add(p, n, __builtin_return_address(0));
    return p;
}
void* __real_realloc(void* old, size_t n);
void* __wrap_realloc(void* old, size_t n) {
    void* p = __real_realloc(old, n);
    if (p) { track_del(old); track_add(p, n, __builtin_return_address(0)); }
    return p;
}
void __real_free(void* p);
void __wrap_free(void* p) {
    track_del(p);
    __real_free(p);
}
