// Star 3D additions to citro3d (MULTICORE_PLAN.md, "Thin emitter").
// Not part of upstream citro3d 1.7.1; every change in this directory is
// marked "S3D:".
#pragma once
#include "../../../os/watchdog.h"

// S3D_C3D_FAST bitmask, set by gx_render.c at the frame start:
// 1 draw packet from a template (C3D_DrawElementsFast), one context update
// 2 uniform upload skips clean words of the dirty flags
// 4 uniform upload skips values the registers already hold (uniforms.c)
extern int g_c3d_fast;
// Uniform upload counts since the last read: vec4 uploaded, vec4 skipped,
// bool writes skipped (uniforms.c, read by gx_render.c)
extern u32 g_c3d_unif_stats[3];

// Sampler phase for the rest of the enclosing block (like GX_PROF_ZONE)
static inline void c3d_phase_restore_(volatile const char** prev) { g_main_phase = *prev; }
#define C3D_PHASE_CAT2(a, b) a##b
#define C3D_PHASE_CAT(a, b) C3D_PHASE_CAT2(a, b)
#define C3D_PHASE(name)                                                                                   \
    volatile const char* C3D_PHASE_CAT(c3d_phase_prev_, __LINE__) __attribute__((cleanup(c3d_phase_restore_))) = \
        g_main_phase;                                                                                     \
    g_main_phase = (name)
