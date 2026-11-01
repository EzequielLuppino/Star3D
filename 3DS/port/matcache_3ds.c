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

// The material hash of the CURRENT shadow state (the sampled words over
// the mask regions): a hit whose stored material equals the applied one
// skips the restore (the state is already correct - the common case when
// the consecutive draws share the material)
u32 matcache_shadow_hash(void);

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
    u32 mat_hash;
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
    if (!on || !g_map_loaded) return 0xFFFFFFFFu;  // The title/attract scenes stay uncached
    u32 key_hash = bs_hash(bs_data, bs_pos);
    uintptr_t o = (uintptr_t)obj;
    u32 pair = ((o >> 4) ^ (o >> 12) ^ (o >> 20)) & 127;
    for (int w = 0; w < 2; w++) {
        MatEntry* e = &s_entries[pair * 2 + w];
        if (e->used && e->obj == obj && e->m == m && e->am == am && e->bs_pos == bs_pos && e->bs_hash == key_hash &&
            e->anim == anim && e->lights == lights) {
            e->last_used = prof_frame_count();
            // The material is usually still the applied one (the consecutive
            // draws of the same object): the hash check skips the restore
            if (e->mat_hash != matcache_shadow_hash()) {
                gxrec_delta_restore(&e->state);
                PROF_COUNT("mat.restores", 1);
            }
            rcp_load(e);
            s_last_advance = e->advance;
            PROF_COUNT("mat.hits", 1);
            return e->idx;
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
    uintptr_t o = (uintptr_t)obj;
    u32 pair = ((o >> 4) ^ (o >> 12) ^ (o >> 20)) & 127;
    MatEntry* slot = &s_entries[pair * 2];
    if (slot->used && s_entries[pair * 2 + 1].used == 0) slot = &s_entries[pair * 2 + 1];
    else if (slot->used && s_entries[pair * 2 + 1].used &&
             s_entries[pair * 2 + 1].last_used < slot->last_used)
        slot = &s_entries[pair * 2 + 1];
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
    slot->mat_hash = matcache_shadow_hash();
    slot->last_used = prof_frame_count();
    rcp_save(slot);
    gxrec_delta_snapshot(&slot->state);
    return idx;
}

u32 port_mat_advance(void) { return s_last_advance; }
