// Coarse game function timers in every build (S3D_PROBE=1 turns them on;
// MULTICORE_PLAN.md "Where the game thread's time goes"). Unlike PERF_WRAP
// (perf_wrap.c) there are no per-draw, matrix or malloc wrappers: only
// functions called up to a few hundred times per frame, so the frame time
// stays that of a normal build. Off, each call costs one branch.
// Counters perf.* hold CPU ticks (268,112 per ms). Nested timers overlap:
// objRender also runs inside sceneDrawTransparentPolys, the fur and shadow
// timers inside objRender. PERF_WRAP builds use perf_wrap.c instead.
// Each timer also sets the probe sampler's scope (port/ptag.h) for its call.

#include <3ds.h>
#include <stdint.h>
#include "config.h"
#include "profiler.h"
#include "ptag.h"

bool g_perf_lite;  // S3D_PROBE (port/probe_3ds.c)

#define TIMED(counter, call) do { \
        if (!g_perf_lite) { call; break; } \
        u64 t0_ = svcGetSystemTick(); \
        call; \
        PROF_COUNT(counter, svcGetSystemTick() - t0_); \
    } while (0)

// Same, and sets the sampler's scope (port/ptag.h) for the call
#define TIMED_SCOPE(counter, scope, call) do { \
        if (!g_perf_lite) { call; break; } \
        u8 ps_ = g_ptag_scope[THREAD_SLOT_GAME]; \
        g_ptag_scope[THREAD_SLOT_GAME] = (scope); \
        u64 t0_ = svcGetSystemTick(); \
        call; \
        PROF_COUNT(counter, svcGetSystemTick() - t0_); \
        g_ptag_scope[THREAD_SLOT_GAME] = ps_; \
    } while (0)

#define WRAP_VOID(name, scope, params, args) \
    void __real_##name params; \
    void __wrap_##name params { TIMED_SCOPE("perf." #name, scope, __real_##name args); }

typedef struct GameObject GameObject;
typedef struct ObjModel ObjModel;
struct MapBlockData;

// Logic
WRAP_VOID(Obj_UpdateAllObjects, PS_LOGIC, (u8 flags), (flags))
WRAP_VOID(updateLights, PS_LIGHTS, (void), ())

// Inside sceneRender
// objRender inside the transparent pass gets its own scope
void __real_objRender(int a, int b, int c, int d, GameObject* obj, int flag);
void __wrap_objRender(int a, int b, int c, int d, GameObject* obj, int flag) {
    u8 parent = g_ptag_scope[THREAD_SLOT_GAME];
    TIMED_SCOPE("perf.objRender", parent == PS_TRANSP || parent == PS_OBJ_T ? PS_OBJ_T : PS_OBJ,
                __real_objRender(a, b, c, d, obj, flag));
}
WRAP_VOID(renderMapBlock, PS_MAP, (struct MapBlockData* block, u8 type), (block, type))
WRAP_VOID(sceneDrawTransparentPolys, PS_TRANSP, (void), ())
WRAP_VOID(objRenderFuzz, PS_FUR, (GameObject* obj), (obj))
WRAP_VOID(playerRenderFuzz, PS_FUR, (GameObject* obj, int p2, int pass), (obj, p2, pass))
WRAP_VOID(objDrawGroundShadow, PS_GSHADOW, (GameObject* obj, ObjModel* model), (obj, model))
WRAP_VOID(expgfx_renderSourcePools, PS_EXPGFX, (uintptr_t sourceId, int mode), (sourceId, mode))
WRAP_VOID(drawGlow, PS_GLOW, (uintptr_t base, int pool), (base, pool))
WRAP_VOID(waterFxDraw, PS_WATER, (void), ())
WRAP_VOID(renderClouds, PS_SKY, (int a, int b, int c, int d), (a, b, c, d))
WRAP_VOID(drawSkyStars, PS_SKY, (void), ())
WRAP_VOID(renderParticles, PS_PARTICLES, (void), ())
WRAP_VOID(updateReflectionTextures, PS_REFL, (void), ())
WRAP_VOID(drawReflectionTexture, PS_REFL, (void), ())
WRAP_VOID(Rcp_UpdateDistortionTextures, PS_RCP, (void), ())

int __real_objShadowRender(GameObject* obj, int mode, int unused, int frames);
int __wrap_objShadowRender(GameObject* obj, int mode, int unused, int frames) {
    int r;
    TIMED_SCOPE("perf.objShadowRender", PS_SHADOW, r = __real_objShadowRender(obj, mode, unused, frames));
    return r;
}

void __real_modelAnimBuildJointMatrices(int* out, u8* dst, void* animState, u8* jointData, int jointCount,
                                        u8* jointScratch, int flags, int mode);
void __wrap_modelAnimBuildJointMatrices(int* out, u8* dst, void* animState, u8* jointData, int jointCount,
                                        u8* jointScratch, int flags, int mode) {
    PTAG_LEAF(PL_JOINTS);
    TIMED("perf.joint_matrices_ticks",
          __real_modelAnimBuildJointMatrices(out, dst, animState, jointData, jointCount, jointScratch, flags, mode));
}
