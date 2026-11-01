// The object material cache (RENDERER_CHANNEL.md, the fork). The game
// rebuilds every object's material (the TEV stages, the textures, the
// texgens, the lights, the fog - objSetupRenderOpGxState's ~82 GX calls and
// its stage builders) every frame, although it is static per object for the
// non-animated ones. The cached path runs the setup once per (object, model,
// position, instruction content, animation, lights) and restores the stored
// state afterwards: gxrec_delta_restore feeds the shadow (the render thread
// receives the state through the normal delta flush) and the game-side g_gx.
// The key is deliberately conservative: any input the setup may read that is
// not in it just costs cache misses, never a wrong material - except the
// animation's *content*, which the key sees only as the pointer (the
// animated objects' materials are then stale: known limitation, the first
// slice covers the static objects).

#include <3ds.h>
#include <string.h>
#include "config.h"
#include "gx_internal.h"
#include "log.h"
#include "profiler.h"
#include "matcache_3ds.h"

static u32 light_hash(void) {
    // Sampled words of the light objects (the per-frame content)
    const u8* p = (const u8*)g_gx.lights;
    u32 h = 0x811C9DC5u;
    for (int i = 0; i < 16; i++) {
        u32 w;
        memcpy(&w, p + (i * 31), 4);
        h = (h ^ w) * 0x01000193u;
    }
    return h;
}

// The game's stage counters (shader_dolphin.c)
extern int gRcpNumTevStages, gRcpNumTexGens, gRcpNumIndStages;
extern int gRcpNextTevStage, gRcpNextTexCoord, gRcpNextTexMap, gRcpNextPostTexMtx;

typedef struct {
    void* obj;
    void* m;
    void* am;
    u32 bs_pos;
    u32 bs_hash;
    void* anim;
    int lights;
    u32 idx;
    u32 advance;
    u32 lh;
    int rcp[7];
    GxState state;
    u32 last_used;
    u8 used;
} MatEntry;

static MatEntry s_entries[256];
static u32 s_last_advance;

static u32 bs_hash(const void* data, u32 pos) {
    // Sampled words from the instruction stream at the setup's position
    const u8* p = (const u8*)data + (pos >> 3);
    u32 h = 0x811C9DC5u;
    for (int i = 0; i < 16; i++) {
        u32 w = p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
        h = (h ^ w) * 0x01000193u;
        p += 4;
    }
    return h;
}

static void rcp_save(MatEntry* e) {
    e->rcp[0] = gRcpNumTevStages;
    e->rcp[1] = gRcpNumTexGens;
    e->rcp[2] = gRcpNumIndStages;
    e->rcp[3] = gRcpNextTevStage;
    e->rcp[4] = gRcpNextTexCoord;
    e->rcp[5] = gRcpNextTexMap;
    e->rcp[6] = gRcpNextPostTexMtx;
}

static void rcp_load(const MatEntry* e) {
    gRcpNumTevStages = e->rcp[0];
    gRcpNumTexGens = e->rcp[1];
    gRcpNumIndStages = e->rcp[2];
    gRcpNextTevStage = e->rcp[3];
    gRcpNextTexCoord = e->rcp[4];
    gRcpNextTexMap = e->rcp[5];
    gRcpNextPostTexMtx = e->rcp[6];
}

// The patched call site's cache lookup: a hit restores the material and the
// game's stage counters and returns the render-op index; 0xFFFFFFFF = miss
// (the patch then runs the setup and calls port_mat_end)
u32 port_mat_setup(void* obj, void* m, void* am, u32 bs_pos, const void* bs_data, void* anim, int lights) {
    s_last_advance = 0;
    // The cache is opt-in while the correctness work continues (the first
    // Azahar run showed stale-state artifacts - the shadow's region coverage
    // and the key's inputs are being tightened)
    static int on = -1;
    CFG_LIVE(on, cfg_int("S3D_MAT_CACHE", 0));
    if (!on) return 0xFFFFFFFFu;
    u32 key_hash = bs_hash(bs_data, bs_pos);
    u32 lh = light_hash();
    for (int i = 0; i < 256; i++) {
        MatEntry* e = &s_entries[i];
        if (!e->used) continue;
        if (e->obj == obj && e->m == m && e->am == am && e->bs_pos == bs_pos && e->bs_hash == key_hash &&
            e->anim == anim && e->lights == lights && e->lh == lh) {
            e->last_used = prof_frame_count();
            gxrec_delta_restore(&e->state);
            rcp_load(e);
            s_last_advance = e->advance;
            PROF_COUNT("mat.hits", 1);
            return e->idx;
        }
    }
    // The miss diagnostic: against this object's own entry, which key part
    // changed? (same instruction content + differing anim/lights = the
    // fixable inputs; a differing hash = the instructions themselves moved)
    {
        bool same_bs = false, diff_anim = false, diff_lights = false;
        for (int i = 0; i < 256; i++) {
            MatEntry* e = &s_entries[i];
            if (!e->used) continue;
            if (e->obj == obj && e->m == m && e->am == am && e->bs_pos == bs_pos) {
                if (e->bs_hash == key_hash) {
                    same_bs = true;
                    if (e->anim != anim) diff_anim = true;
                    if (e->lights != lights) diff_lights = true;
                }
            }
        }
        if (same_bs) {
            PROF_COUNT("mat.miss_samebs", 1);
            if (diff_anim) PROF_COUNT("mat.miss_anim", 1);
            if (diff_lights) PROF_COUNT("mat.miss_lights", 1);
            if (!diff_anim && !diff_lights) PROF_COUNT("mat.miss_stable", 1);
        } else {
            PROF_COUNT("mat.miss_other", 1);
        }
    }
    PROF_COUNT("mat.misses", 1);
    return 0xFFFFFFFFu;
}

// The patched call site's store (after the setup ran): the shadow holds the
// material state; the game's stage counters and the bit-stream advance are
// saved for the hit path
u32 port_mat_end(void* obj, void* m, void* am, u32 bs_pos, const void* bs_data, void* anim, int lights,
                 u32 idx, u32 advance) {
    u32 key_hash = bs_hash(bs_data, bs_pos);
    u32 lh = light_hash();
    MatEntry* slot = NULL;
    u32 oldest = 0xFFFFFFFFu;
    for (int i = 0; i < 256; i++) {
        if (!s_entries[i].used) {
            slot = &s_entries[i];
            break;
        }
        if (s_entries[i].last_used < oldest) {
            oldest = s_entries[i].last_used;
            slot = &s_entries[i];
        }
    }
    slot->used = 1;
    slot->obj = obj;
    slot->m = m;
    slot->am = am;
    slot->bs_pos = bs_pos;
    slot->bs_hash = key_hash;
    slot->anim = anim;
    slot->lights = lights;
    slot->idx = idx;
    slot->advance = advance;
    slot->lh = lh;
    slot->last_used = prof_frame_count();
    rcp_save(slot);
    gxrec_delta_snapshot(&slot->state);
    return idx;
}

u32 port_mat_advance(void) { return s_last_advance; }
