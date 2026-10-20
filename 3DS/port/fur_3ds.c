// Fur wind (GRAPHICS_PLAN.md 5.4). The game sways fur by warping each shell's
// noise coordinates with an indirect read of the scrolling caustic texture,
// scaled by the shell height; the PICA has no indirect stage. Instead the
// noise post-matrix (GX_PTTEXMTX0, in noise tiles) gets a uniform offset that
// grows with the shell height and swings with the same scroll the caustic
// uses, so outer shells lean further. objFuzzShellRenderCb and objFuzzRenderCb
// are patched to call this before loading the matrix. S3D_FUR_WIND=0 turns it
// off.
// It also scales the game's 37.5x noise tiling by S3D_FUR_TILE percent
// (default 50). The 64x64 noise has no mipmaps (none on GameCube either), and
// at 400x240 every texel covers about twice the screen span it does at
// 640x480, so the full tiling aliased into speckle in dialog close-ups
// (OPEN_BUGS.md 2). Half the tiling keeps the GameCube's texels per pixel.

#include <math.h>
#include <string.h>
#include "config.h"
#include "gx_internal.h"
#include "profiler.h"
#include <3ds.h>
#include "log.h"

#define FUR_WIND_AMP 0.6f  // Offset at fz = 1, in noise tiles

void port_fur_wind(float m[3][4], float fz, float sx, float sy) {
    static int enabled = -1, tile = -1;
    CFG_LIVE(enabled, cfg_bool("S3D_FUR_WIND", true));
    CFG_LIVE(tile, cfg_int("S3D_FUR_TILE", 50));
    if (tile > 0) {
        m[0][0] *= (float)tile / 100.0f;
        m[1][1] *= (float)tile / 100.0f;
    }
    if (!enabled) return;
    // The scroll advances about 0.5/s in x and 0.18/s in y: a 2 s sway with
    // a slower drift across it
    float a = FUR_WIND_AMP * fz;
    m[0][3] += a * sinf(6.2831853f * sx);
    m[1][3] += a * 0.5f * sinf(6.2831853f * sy + 1.0f);
}

// Fur shell budget (GRAPHICS_PLAN.md 5.4). objRenderFuzz draws up to 15
// shells for Fox and Krystal, every one a full model pass, and stereo draws
// each twice. The patched loop asks for a layer step instead: shells are
// spread over the same height (gObjFuzzStep, which scales the joints per
// pass, is multiplied by the step) but at most 8 are drawn close up, 4 at mid
// range and 2 far away; far objects lose their fur in stereo. "3D: reduced"
// (S3D_STEREO_REDUCED) halves the budget in stereo. S3D_FUR_BUDGET=0 restores
// the game's counts. cnt is the game's distance measure (16 - cnt shells
// before its own cap).

bool port_stereo_active(void);   // gx_render.c
bool port_stereo_reduced(void);  // gx_render.c

static int s_fur_layers = 1;  // Layers in the current objRenderFuzz loop, for port_fur_shade

int port_fur_step(int n, int cnt) {
    static int enabled = -1;
    CFG_LIVE(enabled, cfg_bool("S3D_FUR_BUDGET", true));
    s_fur_layers = n;
    if (!enabled) return 1;
    bool stereo = port_stereo_active();
    int budget = cnt <= 4 ? 8 : cnt <= 9 ? 4 : stereo ? 0 : 2;
    if (port_stereo_reduced()) budget /= 2;
    if (budget <= 0) {
        PROF_COUNT("fur.objects_skipped", 1);
        return 0;
    }
    int step = (n + budget - 1) / budget;
    PROF_COUNT("fur.shells", (n + step - 1) / step);
    return step;
}

// Per-layer shade (GRAPHICS_PLAN.md 5.4): with fewer shells the fur reads
// flat, so inner shells are drawn darker (60% at the root, full at the tips)
// as a cheap stand-in for self-shadowing. Multiplies the shell's base texture
// in objFuzzRenderCb. S3D_FUR_SHADE=0 turns it off.
unsigned char port_fur_shade(int layer) {
    static int enabled = -1;
    CFG_LIVE(enabled, cfg_bool("S3D_FUR_SHADE", true));
    if (!enabled || s_fur_layers <= 1) return 0xff;
    float t = (float)layer / (float)(s_fur_layers - 1);
    if (t > 1.0f) t = 1.0f;
    return (unsigned char)(255.0f * (0.6f + 0.4f * t));
}

// Grass pass budget (GRAPHICS_PLAN.md step 11). Map blocks with grass draw the
// block again for every layer (4, 8 or 16 passes, each raised a little and
// masked by its own noise texture) in mapBlockRender_drawLightmapIndirectPasses.
// The patched loop walks the passes with this step, ending on the top one so
// the blades keep their full height (gx.prof counter grass.passes). At most S3D_GRASS_BUDGET passes (default
// 8) are drawn, half that in "3D: reduced" stereo. 0 draws no grass passes (only the ground under them).
int port_grass_step(int passCount) {
    static int budget = -1;
    CFG_LIVE(budget, cfg_int("S3D_GRASS_BUDGET", 8));
    if (budget < 1) return passCount + 1;  // the loop starts past the last pass
    int b = budget;
    if (port_stereo_reduced() && b > 1) b /= 2;
    int step = (passCount + b - 1) / b;
    if (step < 1) step = 1;
    PROF_COUNT("grass.passes", (passCount + step - 1) / step);
    return step;
}

// Fur noise over projected light (GRAPHICS_PLAN.md 4.4). A shell lit by a
// projected light texture (torches, caustics) reads four textures: base,
// sphere-map, light and the layer's noise mask, one more than the PICA has.
// The unit map keeps the first three in stage order, so the noise was dropped
// and every shell drew solid. objFuzzRenderCb now skips the light texture
// stage instead (the shell keeps its vertex lighting). The proctex unit cannot
// stand in for the noise: it reads one of the three units' coordinates, and
// those carry no 37.5x tiling. S3D_FUR_PROJ_LIGHT=1 restores the light stage.
int port_fur_proj_light(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = cfg_bool("S3D_FUR_PROJ_LIGHT", false);
    if (!enabled) PROF_COUNT("fur.proj_light_skipped", 1);
    return enabled;
}

// Gameplay fur (OPEN_BUGS.md 2). objRenderFuzz only gives the "strong" distance
// falloff to riding objects, dialog cameras (mapEventSlot 0x3f) and the death
// models; anything else counts 5.25 times faster, so Fox at the gameplay camera
// distance gets 16 - cnt <= 0 shells and no fur at all. For the player
// (classId 1) this recomputes cnt the strong way, keeping the game's weak
// look (at most 3 shells, double spacing). Called right after n = 16 - cnt;
// returns the new n and updates cnt for port_fur_step. Logs the inputs about
// once a second (tag fur). S3D_FUR_MODE picks it: 0 no fur shells at all (any
// object, cutscenes too), 1 the game's own fur (cutscenes and dialogs, the
// default), 2 also in gameplay. Mode 2 is
// not the default:
// the GameCube draws no shells at the gameplay camera, and the forced shells
// tile the 64x64 noise (no mipmaps, also on GameCube) 37.5 times over a Fox
// ~45 px tall, which aliases into speckle (capture 20261011_002136_f2383).
int port_fur_gameplay(int strong, int classId, float dist, float scale, int slot, unsigned flags, int* cnt, int n) {
    static int enabled = -1;
    static u64 last_log;
    CFG_LIVE(enabled, cfg_int("S3D_FUR_MODE", 1));
    if (enabled == 0) return 0;
    int n2 = n;
    if (enabled == 2 && !strong && classId == 1 && scale > 0.0f) {
        *cnt = (int)((2.0f * dist) / scale);
        n2 = 16 - *cnt;
    }
    if (classId == 1) {
        u64 now = osGetTime();
        if (now - last_log >= 1000) {
            last_log = now;
            LOG_INFO("fur", "player strong %d dist %.1f scale %.2f slot %d flags %x n %d -> %d", strong, dist, scale,
                     slot, flags, n, n2);
        }
    }
    return n2;
}

// Fur bump mask and sphere map (OPEN_BUGS.md 2). A shell reads four textures
// (base, sphere map, bump mask, noise) and the PICA has three. The sphere map
// on stage 1 lights the fur (REG1 = RASC + sphere * C2, and RASC is black with
// the unlit channel), the bump mask only limits where the noise shows
// (GX_ALPHA_BUMPN on the noise stage). By default objFuzzRenderCb takes the
// game's own no-mask path for the noise stage (port_fur_bump returns 0), so
// map2 is never read and the sphere map keeps its unit. S3D_FUR_BUMP=1 keeps
// the mask; the sphere map is then read as white (REG1 = RASC + C2), which
// draws the fur unlit and too bright in dialogs (capture
// 20261011_003922_f2261). port_fur_sphere returns stage 1's b input:
// S3D_FUR_SPHERE 0 TEXC, 1 ONE, 2 HALF, default TEXC without the mask and
// ONE with it.
int port_fur_bump(void) {
    static int enabled = -1;
    CFG_LIVE(enabled, cfg_bool("S3D_FUR_BUMP", false));
    return enabled;
}

int port_fur_sphere(void) {
    static int mode = -1;
    CFG_LIVE(mode, cfg_int("S3D_FUR_SPHERE", -1));
    int m = mode < 0 ? (port_fur_bump() ? 1 : 0) : mode;
    if (m == 1) return 12;  // GX_CC_ONE
    if (m == 2) return 13;  // GX_CC_HALF
    return 8;               // GX_CC_TEXC
}

// Fast fur shells (FUR_PLAN.md "Fast shells"). Every shell is a full
// modelDoRenderInstrs pass, but only render ops with flag 0x200 (the fur
// material) draw; the others still set up their whole material, vertex format
// and joint matrices for display lists that are then skipped. In
// objRenderFuzz's loop the patched objprint_dolphin.c defers that work and
// applies only what a drawn op can still see. S3D_FUR_FAST is a bit mask:
//   1  matrix loads of skipped ops wait in a table (one per matrix slot) and
//      are sent before the next fur draw; a reload of the same slot replaces
//      the waiting one (needs bit 2, otherwise every material flushes it)
//   2  a skipped op's material setup runs only if it is the last one before a
//      fur op (what the fur op would otherwise inherit) or the last of the loop
//   4  the same for a skipped op's vertex format, applied before a fur draw
//   8  projection, lights, fog and vertex arrays once per object, not per shell
//  16  fur shading once per layer: in a shell, a fur op that follows another
//      fur op's setup with nothing but matrices and vertex formats in between
//      only binds the textures that differ (objFuzzRenderCb's ~60 TEV, texture
//      and pixel commands are the same for all fur ops of the layer). Opt-in:
//      Fox has one fur op per shell, so it never fires for him (Azahar,
//      build AF53D8C4: fur.fast_cb_lite 0 in the hub and on the title)
// 0 is the game's path. The loop end applies whatever is still waiting, so
// the GX state after the fur equals the game's. Counters:
// fur.fast_mtx_deferred / fur.fast_mtx_flushed, fur.fast_mat_skipped /
// fur.fast_mat_run, fur.fast_desc_skipped / fur.fast_desc_run, fur.fast_cb_full /
// fur.fast_cb_lite (fur ops with the full setup / textures only).
int port_fur_fast(void) {
    static int bits = -1;
    CFG_LIVE(bits, cfg_int("S3D_FUR_FAST", 15));
    return bits;
}

void port_fur_fast_note(int what) {
    switch (what) {
    case 0: PROF_COUNT("fur.fast_mat_skipped", 1); break;
    case 1: PROF_COUNT("fur.fast_mat_run", 1); break;
    case 2: PROF_COUNT("fur.fast_desc_skipped", 1); break;
    case 3: PROF_COUNT("fur.fast_desc_run", 1); break;
    case 4: PROF_COUNT("fur.fast_cb_full", 1); break;
    default: PROF_COUNT("fur.fast_cb_lite", 1); break;
    }
}

// Waiting matrix loads: pos, nrm and tex memory, indexed by id / 3 (PNMTX0-9
// are ids 0-27, TEXMTX0-9 30-57). Post-transform ids (64+) never wait
#define FUR_MTX_SLOTS 20
typedef struct {
    float m[12];
    u32 gen;
    u8 type;
    u8 live;
} FurMtx;
static FurMtx s_fur_mtx[3][FUR_MTX_SLOTS];
static u32 s_fur_gen = 1;
static int s_fur_waiting;

static void fur_mtx_send(int kind, const void* m, u32 id, int type) {
    if (kind == 0) GXLoadPosMtxImm(m, id);
    else if (kind == 1) GXLoadNrmMtxImm(m, id);
    else GXLoadTexMtxImm(m, id, (GXTexMtxType)type);
}

// kind 0 pos, 1 nrm, 2 tex. defer: the load belongs to an op whose display
// lists are skipped
void port_fur_mtx(int kind, const void* m, u32 id, int type, int defer) {
    u32 slot = id / 3;
    if (id >= GX_IDENTITY || slot >= FUR_MTX_SLOTS) {
        fur_mtx_send(kind, m, id, type);
        return;
    }
    FurMtx* e = &s_fur_mtx[kind][slot];
    if (defer) {
        if (!e->live) {
            e->live = 1;
            s_fur_waiting++;
        }
        memcpy(e->m, m, (kind == 2 && type == GX_MTX2x4) ? 32 : 48);
        e->type = (u8)type;
        e->gen = s_fur_gen;
        PROF_COUNT("fur.fast_mtx_deferred", 1);
        return;
    }
    if (e->live) {
        e->live = 0;
        s_fur_waiting--;
    }
    fur_mtx_send(kind, m, id, type);
}

// Starts a new generation: loads deferred from now on come after a deferred
// material setup in the game's order
u32 port_fur_mtx_mark(void) {
    return ++s_fur_gen;
}

// Sends the waiting loads older than generation `below` (~0u: all)
void port_fur_mtx_flush(u32 below) {
    if (s_fur_waiting == 0) return;
    int sent = 0;
    for (int k = 0; k < 3; k++) {
        for (int s = 0; s < FUR_MTX_SLOTS; s++) {
            FurMtx* e = &s_fur_mtx[k][s];
            if (!e->live || e->gen >= below) continue;
            e->live = 0;
            s_fur_waiting--;
            fur_mtx_send(k, e->m, (u32)s * 3, e->type);
            sent++;
        }
    }
    if (sent) PROF_COUNT("fur.fast_mtx_flushed", sent);
}
