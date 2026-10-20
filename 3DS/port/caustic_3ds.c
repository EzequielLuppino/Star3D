// Floor caustics of underwater-looking rooms (OPEN_BUGS.md bug 35).
//
// Map shaders with flag 0x40 (Krazoa Palace's lower rooms, save C12) add
// addWarpedRingTevStages: the game's ring texture (128 x 128 I8, a soft
// annulus of intensity <= 16) is projected from world position twice
// (scales 0.008 and 0.005, along two tilted axes) and added as
// vertex alpha * ring. On the GameCube two indirect stages offset both reads
// per pixel by the caustic texture (matrix 0.5, scale 2^-1, bias on T only:
// S shifts 0..63 texels, T +-32), which breaks the rings into a moving
// caustic net over floor and walls. The PICA has no indirect stage, and the
// per-vertex swirl (gx_tev.c s_warp_sites) did nothing on the floors' 6-7
// big triangles: the floor showed plain faint rings, or nothing.
//
// The port bakes the GameCube's result into the ring texture itself (it has
// no other user), in the first read's coordinate space (tc0): the texture
// covers 2 x 2 ring tiles, and the other three projections are expressed in
// tc0 with their floor-plane relations rounded to halves so the bake tiles
// (tc1 = [.5 0; .5 .5] tc0, caustic reads [1 -.5; .5 0] tc0 and
// [1 .5; 1 -1] tc0; exact on floors: tc1 [.39 .04; .46 .22], [1.19 -.70;
// .74 -.09], [1.07 .60; .83 -1.08]). Offline check against a per-pixel
// simulation: tmp_scripts/bug35/ring_sim*.py. Both TEV reads then sample
// this one net through the first projection, as two copies that drift
// against each other with the game's caustic scroll
// (port_warped_ring_scroll), so it shimmers. A sum of two copies is greyer
// than the GameCube's single net, so each copy is sharpened (power 1.5,
// peak 20 of the GameCube's 32 for both reads).
// S3D_RING_BAKE=0 keeps the plain rings (read at startup); S3D_RING_GAIN
// (percent) scales the baked intensity. The GameCube's own strength (100)
// adds at most ~12% and was hard to see in Azahar on the 3DS screen (the
// net's cells are 2-4 pixels there), so the default is 200.
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "log.h"

void newshadows_getReflectionScrollOffsets(float* outScrollX, float* outScrollY);

static int s_ring_baked = -1;

int port_ring_baked(void) { return s_ring_baked > 0; }

// Byte offset of texel (x, y) in a GameCube I8 texture 128 wide (8 x 4 tiles)
static inline int i8_offset(int x, int y) { return (x & 7) + (x >> 3) * 0x20 + (y & 3) * 8 + (y >> 2) * 0x200; }

// The game's caustic texture (newshadows.c), 64 x 64: [0] = alpha (indirect
// S), [1] = intensity (indirect T)
static void caustic_texel(int x, int y, float out[2]) {
    float xp = (6.284f / 64.0f) * (x & 63), yp = (6.284f / 16.0f) * (y & 63);
    float wave = cosf(0.5f * sinf(yp) + xp), carrier = cosf(yp);
    out[0] = (float)(int)(127.0f * wave * carrier + 127.0f);
    out[1] = (float)(int)(127.0f * wave + 127.0f);
}

// Ring texture value at ring texel coordinates (bilinear, repeating)
static float ring_at(const unsigned char* ring, float x, float y) {
    x -= 0.5f;
    y -= 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float ax = x - x0, ay = y - y0;
    float r00 = ring[(y0 & 127) * 128 + (x0 & 127)], r10 = ring[(y0 & 127) * 128 + ((x0 + 1) & 127)];
    float r01 = ring[((y0 + 1) & 127) * 128 + (x0 & 127)], r11 = ring[((y0 + 1) & 127) * 128 + ((x0 + 1) & 127)];
    return (r00 * (1 - ax) + r10 * ax) * (1 - ay) + (r01 * (1 - ax) + r11 * ax) * ay;
}

// Indirect offset in ring texels at caustic coordinate (s, t): matrix 0.5,
// scale 2^-1, bias on T only
static void caustic_offset(float s, float t, float out[2]) {
    float cx = s * 64.0f - 0.5f, cy = t * 64.0f - 0.5f;
    int x0 = (int)floorf(cx), y0 = (int)floorf(cy);
    float ax = cx - x0, ay = cy - y0, c00[2], c10[2], c01[2], c11[2];
    caustic_texel(x0, y0, c00);
    caustic_texel(x0 + 1, y0, c10);
    caustic_texel(x0, y0 + 1, c01);
    caustic_texel(x0 + 1, y0 + 1, c11);
    for (int k = 0; k < 2; k++) {
        float c = (c00[k] * (1 - ax) + c10[k] * ax) * (1 - ay) + (c01[k] * (1 - ax) + c11[k] * ax) * ay;
        out[k] = 0.25f * (k ? c - 128.0f : c);
    }
}

void port_warped_ring_bake(unsigned char* data) {
    s_ring_baked = cfg_bool("S3D_RING_BAKE", true) ? 1 : 0;
    if (!s_ring_baked) return;
    int gain = cfg_int("S3D_RING_GAIN", 200);
    static unsigned char ring[128 * 128];
    for (int y = 0; y < 128; y++)
        for (int x = 0; x < 128; x++) ring[y * 128 + x] = data[i8_offset(x, y)];
    int peak = 0;
    for (int y = 0; y < 128; y++) {
        for (int x = 0; x < 128; x++) {
            float s0 = (x + 0.5f) / 64.0f, t0 = (y + 0.5f) / 64.0f;  // tc0, 2 x 2 tiles
            float o0[2], o1[2];
            caustic_offset(s0 - 0.5f * t0, 0.5f * s0, o0);
            caustic_offset(s0 + 0.5f * t0, s0 - t0, o1);
            float s1 = 0.5f * s0, t1 = 0.5f * s0 + 0.5f * t0;
            float v = ring_at(ring, s0 * 128.0f + o0[0], t0 * 128.0f + o0[1]) +
                      ring_at(ring, s1 * 128.0f + o0[0] + o1[0], t1 * 128.0f + o0[1] + o1[1]);
            float n = v / 32.0f;
            int iv = (int)(20.0f * n * sqrtf(n) * gain / 100.0f + 0.5f);
            if (iv > 255) iv = 255;
            if (iv > peak) peak = iv;
            data[i8_offset(x, y)] = (unsigned char)iv;
        }
    }
    LOG_INFO("caustic", "warped ring baked (gain %d%%, peak %d)", gain, peak);
}

// Called with the two ring projection matrices (rows: world -> s, t) before
// they are loaded. The second read takes the first one's projection: the
// bake lives in that space (2 ring tiles per texture repeat). The drifts are
// whole repeats when the scroll wraps at 256
void port_warped_ring_scroll(float (*m0)[4], float (*m1)[4]) {
    if (s_ring_baked <= 0) return;
    float rx, ry;
    newshadows_getReflectionScrollOffsets(&rx, &ry);
    (void)ry;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 4; c++) m1[r][c] = m0[r][c] * 0.5f;
    for (int c = 0; c < 4; c++) m0[0][c] *= 0.5f, m0[1][c] *= 0.5f;
    // In texture repeats (= 2 ring tiles)
    m0[0][3] += 0.0625f * rx;
    m0[1][3] += 0.03125f * rx;
    m1[0][3] += 0.37f - 0.09375f * rx;
    m1[1][3] += 0.71f + 0.0625f * rx;
}
