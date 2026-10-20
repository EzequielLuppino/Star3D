// Screen warps (GRAPHICS_PLAN.md 5.2). doHeatEffect and doDistortionFilter
// draw the screen copy over a full-screen quad and bend it with an indirect
// stage (the scrolling caustic texture for the heat shimmer, the radial
// vector texture rotated by an angle for the distortion shockwave). The PICA
// has no indirect stage, so the patched functions draw a screen-space grid
// instead, and each vertex gets the offset the indirect read would have added
// at that point, computed analytically on the CPU. The grid uses GX_VTXFMT1
// (positions s16 with 2 fraction bits, float texture coordinates), so the
// small offsets survive. Counter: gx.screen_warp_verts. S3D_SCREEN_WARP=0
// draws the game's plain quad.

#include <math.h>
#include <stdbool.h>
#include "config.h"
#include "gx_internal.h"
#include "profiler.h"

#define WARP_COLS 40
#define WARP_ROWS 24
#define SCREEN_W 640
#define SCREEN_H 480

// Heat: amplitude of the shimmer in texture units, and the waves across the
// screen (the caustic texture repeats 7 times over it)
#define HEAT_AMP 0.0035f
#define HEAT_WAVES 7.0f
// Distortion: the largest push toward the centre in texture units, at the
// game's strongest indirect scale (0.5)
#define DIST_AMP 0.06f

bool port_screen_warp_enabled(void) {
    static int enabled = -1;
    CFG_LIVE(enabled, cfg_bool("S3D_SCREEN_WARP", true));
    return enabled;
}

typedef struct {
    int kind;  // 0 heat, 1 distortion
    float a, b, c, d, e;
} WarpParams;

static void warp_offset(const WarpParams* p, float u, float v, float* du, float* dv) {
    if (p->kind == 0) {
        // a, b = caustic scroll (the game's fA, fB); c, d = rotation of the
        // indirect matrix (cos, sin), already halved by the game
        float tau = 6.2831853f;
        float x = sinf(tau * (HEAT_WAVES * u + p->a)) + 0.5f * sinf(tau * (2.3f * HEAT_WAVES * v - p->b) + 1.3f);
        float y = sinf(tau * (HEAT_WAVES * v - p->b) + 0.7f) + 0.5f * sinf(tau * (1.7f * HEAT_WAVES * u + p->a));
        *du = HEAT_AMP * 2.0f * (p->c * x + p->d * y);
        *dv = HEAT_AMP * 2.0f * (-p->d * x + p->c * y);
    } else {
        // a, b = centre in texture units; c = indirect scale (0..0.5);
        // d = cos, e = sin of the angle. The vector texture holds the radial
        // direction scaled by (100.8 - 0.9 r) / 128 inside r <= 112 texels,
        // and the indirect coordinates are 0.8 * 256 texels per screen unit
        float x = u - p->a, y = v - p->b;
        float rx = p->d * x - p->e * y, ry = p->e * x + p->d * y;
        float r = 204.8f * sqrtf(rx * rx + ry * ry);
        if (r > 112.0f || r < 1e-3f) { *du = *dv = 0.0f; return; }
        float s = (100.8f - 0.9f * r) / 100.8f * (p->c / 0.5f) * DIST_AMP;
        float inv = 204.8f / r;
        // The offset is applied in screen orientation without undoing the
        // rotation, so the push swirls by the angle
        *du = rx * inv * s;
        *dv = ry * inv * s;
    }
}

static void draw_grid(const WarpParams* p, bool color, u8 a_top, u8 a_bot) {
    for (int row = 0; row < WARP_ROWS; row++) {
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT1, (WARP_COLS + 1) * 2);
        for (int col = 0; col <= WARP_COLS; col++) {
            for (int k = 0; k < 2; k++) {
                int r = row + k;
                float u = (float)col / WARP_COLS, v = (float)r / WARP_ROWS;
                float du, dv;
                warp_offset(p, u, v, &du, &dv);
                // The screen edges keep their coordinate so the copy never
                // reads outside itself
                if (col == 0 || col == WARP_COLS) du = 0.0f;
                if (r == 0 || r == WARP_ROWS) dv = 0.0f;
                GXPosition3s16((s16)(col * SCREEN_W * 4 / WARP_COLS), (s16)(r * SCREEN_H * 4 / WARP_ROWS), -32);
                if (color) GXColor4u8(0, 0, 0, (u8)(a_top + (a_bot - a_top) * r / WARP_ROWS));
                GXTexCoord2f32(u + du, v + dv);
            }
        }
    }
    PROF_COUNT("gx.screen_warp_verts", WARP_ROWS * (WARP_COLS + 1) * 2);
}

void port_heat_grid(float fA, float fB, float rot_c, float rot_s, u8 a_top, u8 a_bot) {
    WarpParams p = { 0, fA, fB, rot_c, rot_s, 0.0f };
    draw_grid(&p, true, a_top, a_bot);
}

void port_distortion_grid(float proj5, float proj4, float ind_s, float angle) {
    // TEXCOORD3 = T(0.5) * R(angle) * S(0.8) * (uv - centre), with the centre
    // at (0.5 + 0.5 proj5, 0.5 - 0.5 proj4)
    WarpParams p = { 1, 0.5f + 0.5f * proj5, 0.5f - 0.5f * proj4, ind_s, cosf(angle), sinf(angle) };
    draw_grid(&p, false, 0, 0);
}
