// Where-am-I tags for the probe's sampler (port/probe_3ds.c, S3D_PROBE bit 2;
// MULTICORE_PLAN.md "Inside the game thread"). Each thread slot keeps two
// bytes: the scope (innermost coarse game function, set by perf/perf_lite.c
// and frame_zones.c, only while the probe is on) and the leaf (what the code is
// doing inside it: game code, GX recording, display-list hashing, skinning,
// waits; set in every build, a load and two stores). A thread on core 2 reads
// the game thread's pair every S3D_PROBE_SAMPLE_US and adds the elapsed time
// to that cell, so the game thread pays no timer calls.

#ifndef PTAG_H
#define PTAG_H

#include <3ds.h>
#include "profiler.h"

enum {
    PS_OUT,        // Outside every scope below (frame start/end, loads, text, audio)
    PS_LOGIC,      // Obj_UpdateAllObjects
    PS_LIGHTS,     // updateLights
    PS_SCENE,      // sceneRender, outside the scopes below
    PS_OBJ,        // objRender (opaque pass)
    PS_OBJ_T,      // objRender inside sceneDrawTransparentPolys
    PS_MAP,        // renderMapBlock
    PS_TRANSP,     // sceneDrawTransparentPolys, outside objRender
    PS_FUR,        // objRenderFuzz / playerRenderFuzz
    PS_GSHADOW,    // objDrawGroundShadow
    PS_SHADOW,     // objShadowRender
    PS_GLOW,       // drawGlow
    PS_EXPGFX,     // expgfx_renderSourcePools
    PS_WATER,      // waterFxDraw
    PS_SKY,        // renderClouds, drawSkyStars
    PS_PARTICLES,  // renderParticles
    PS_REFL,       // updateReflectionTextures, drawReflectionTexture
    PS_RCP,        // Rcp_UpdateDistortionTextures
    PS_COUNT
};

enum {
    PL_GAME,     // Game code (traversal, matrices, culling, ...)
    PL_GXCMD,    // GX state call recorded into the ring (or run directly)
    PL_GXVTX,    // Immediate-mode vertex attribute call (GXPosition3f32, ...)
    PL_GXDRAW,   // GXBegin / GXEnd / GXCallDisplayList, outside the hashing
    PL_DLHASH,   // Display list and vertex array hashing (rt.dl_prehash)
    PL_SKIN,     // Skinning wrappers (port/skin_3ds.c)
    PL_JOINTS,   // modelAnimBuildJointMatrices
    PL_WAIT,     // Waiting for the render thread (ring space, syncs)
    PL_PROBE,    // The probe's own slices
    PL_COUNT
};

extern volatile u8 g_ptag_scope[THREAD_SLOTS];
extern volatile u8 g_ptag_leaf[THREAD_SLOTS];

static inline void ptag_leaf_restore_(u8* prev) { g_ptag_leaf[t_thread_slot] = *prev; }

// Sets the leaf for the rest of the enclosing block, then puts back the old one
#define PTAG_LEAF(l) \
    u8 PROF_CAT(ptag_prev_leaf_, __LINE__) __attribute__((cleanup(ptag_leaf_restore_))) = \
        g_ptag_leaf[t_thread_slot]; \
    g_ptag_leaf[t_thread_slot] = (l)

// Recorder detail (game thread only: only the producer records). The opcode
// being recorded, calls per opcode, and the recorder step
enum {
    PSUB_NONE,     // Wrapper and command encoding
    PSUB_RESERVE,  // Ring space (reserve)
    PSUB_FILTER,   // Record-time redundant-command filter (dd_redundant)
    PSUB_PUBLISH,  // Publishing to the render thread (and waking it)
    PSUB_IMM,      // Opening / closing immediate-mode chunks
    PSUB_COUNT
};
extern volatile u8 g_ptag_op, g_ptag_sub;
extern u32 g_ptag_op_calls[256];

static inline void ptag_sub_restore_(u8* prev) { g_ptag_sub = *prev; }

// Sets the recorder step for the rest of the enclosing block
#define PTAG_SUB(st) \
    u8 PROF_CAT(ptag_prev_sub_, __LINE__) __attribute__((cleanup(ptag_sub_restore_))) = g_ptag_sub; \
    g_ptag_sub = (st)

// Per-scope draw counters (game thread, read by the probe report)
extern u32 g_ptag_imm_calls[PS_COUNT];  // Immediate-mode vertex attribute calls
extern u32 g_ptag_begin_verts[PS_COUNT];  // GXBegin vertex counts (immediate mode and display lists being built)
extern u32 g_ptag_dl_calls[PS_COUNT];
extern u32 g_ptag_dl_bytes[PS_COUNT];

#endif // PTAG_H
