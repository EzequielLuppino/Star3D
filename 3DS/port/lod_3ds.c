// Scene detail levers for the hub (MULTICORE_PLAN.md, "Hub: fewer objects").
// Called from objUpdateOpacity (patches/main/shader.c.sed).

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "log.h"
#include "profiler.h"
#include <3ds/types.h>
#include <stdint.h>

// Object census: every object reaching the distance test on one frame, with
// its def id, class id and ObjDef name (empty in this data), camera distance, draw range and projected size, logged
// per name. S3D_OBJ_CENSUS=<frames after the A/B start> (port/diag_3ds.c), 0 = off
#define CENSUS_MAX 768
typedef struct {
    const void* obj;
    char name[12];
    int def_id, class_id;
    float d, range, sz;  // sz < 0: not reached (past the range)
} CensusObj;
static CensusObj s_census[CENSUS_MAX];
static int s_census_n;
static int s_census_state;  // 0 idle, 1 armed (counting down), 2 collecting, 3 done
static int s_census_delay;

void lod_census_request(int delay) {
    if (delay <= 0 || s_census_state != 0) return;
    s_census_state = 1;
    s_census_delay = delay;
}

static CensusObj* census_find(const void* obj) {
    for (int i = s_census_n - 1; i >= 0; i--)
        if (s_census[i].obj == obj) return &s_census[i];
    return NULL;
}

// S3D_OBJ_DIST (percent, default 100): scales every object's draw range. The
// range comes from the object's placement (cullDistance2 = placement byte
// << 3); objects past it aren't drawn, and the last 100 units fade out.
// Scaled ranges stay at least 140 so nearby objects (and the fade band) stay
// Plants: DLL 685 (SoftBody: grass clumps, flowers, bulrushes) and 687 (fern
// trees). The hub has ~180 of them. All plants of a kind sway with one shared
// animation phase (685.c), yet each one is skinned on its own every frame.
// S3D_PLANTS: 1 normal (default), 0 hidden, 2 frozen (skinned once, then the
// first pose is kept: measures what re-skinning costs)
static int s_plants = 1;
static unsigned s_lod_frame;

static inline bool is_plant(int dll_id) { return dll_id == 685 || dll_id == 687; }

// Camera distance of each object this frame (from the distance test), for the
// animation LOD. Open addressing on the object address, cleared lazily by frame
#define DIST_SLOTS 2048
static struct {
    const void* obj;
    float d;
    unsigned frame;
} s_dist[DIST_SLOTS];

static void dist_store(const void* obj, float d) {
    u32 h = ((uintptr_t)obj >> 4) % DIST_SLOTS;
    for (u32 i = 0; i < 16; i++, h = (h + 1) % DIST_SLOTS) {
        if (s_dist[h].obj == obj || s_dist[h].frame + 1 < s_lod_frame || !s_dist[h].obj) {
            s_dist[h].obj = obj;
            s_dist[h].d = d;
            s_dist[h].frame = s_lod_frame;
            return;
        }
    }
}

// Distance seen this frame or the last one, or -1
static float dist_lookup(const void* obj) {
    u32 h = ((uintptr_t)obj >> 4) % DIST_SLOTS;
    for (u32 i = 0; i < 16; i++, h = (h + 1) % DIST_SLOTS)
        if (s_dist[h].obj == obj) return s_dist[h].frame + 1 >= s_lod_frame ? s_dist[h].d : -1.0f;
    return -1.0f;
}

float port_obj_range(const void* obj, const char* name, int def_id, int class_id, int dll_id, float range, float d) {
    CFG_LIVE(s_plants, cfg_int("S3D_PLANTS", 1));
    if (s_plants == 0 && is_plant(dll_id)) {
        PROF_COUNT("plant.hidden", 1);
        return 0.0f;
    }
    dist_store(obj, d);
    static int pct = -1;
    CFG_LIVE(pct, cfg_int("S3D_OBJ_DIST", 100));
    if (s_census_state == 2 && s_census_n < CENSUS_MAX && !census_find(obj)) {
        CensusObj* c = &s_census[s_census_n++];
        c->obj = obj;
        memset(c->name, 0, sizeof(c->name));
        if (name) memcpy(c->name, name, 11);
        for (int i = 0; i < 11; i++)
            if (c->name[i] && (c->name[i] < 32 || c->name[i] > 126)) c->name[i] = '?';
        if (!c->name[0]) strcpy(c->name, "-");
        c->def_id = def_id;
        c->class_id = class_id;
        c->d = d;
        c->range = range;
        c->sz = -1.0f;
    }
    if (pct != 100) {
        float r = range * (float)pct / 100.0f;
        if (r < 140.0f) r = range < 140.0f ? range : 140.0f;
        range = r;
    }
    if (d > range) {
        PROF_COUNT("obj.dist_culled", 1);
    } else {
        PROF_COUNT("obj.dist_kept", 1);
    }
    return range;
}

// S3D_OBJ_MINPX (default 10 = the game's own cut): objects whose projected
// size (objUpdateOpacity's sz, ~pixels) is below this aren't drawn. The game
// fades 10..15 itself; above that this is a hard cut
int port_obj_small(const void* obj, float sz) {
    static int minpx = -1;
    CFG_LIVE(minpx, cfg_int("S3D_OBJ_MINPX", 10));
    if (s_census_state == 2) {
        CensusObj* c = census_find(obj);
        if (c) c->sz = sz;
    }
    if (sz < (float)minpx && sz >= 10.0f) {
        PROF_COUNT("obj.size_culled", 1);
        return 1;
    }
    return 0;
}

typedef struct {
    const char* name;
    int def_id, class_id;
    int n, reached;
    float dmin, dsum, dmax, rmin, rmax, szmin, szsum;
    int cut[4];  // In range but culled at distance 75 / 50 / 30 % (floor 140), or below 20 px
} CensusRow;

static int row_cmp(const void* a, const void* b) {
    const CensusRow *x = a, *y = b;
    return y->n != x->n ? y->n - x->n : x->def_id - y->def_id;
}

static bool cut_at(const CensusObj* c, int pct) {
    float r = c->range * pct / 100.0f;
    if (r < 140.0f) r = c->range < 140.0f ? c->range : 140.0f;
    return c->d > r;
}

static void census_dump(void) {
    static const int pcts[3] = { 75, 50, 30 };
    static CensusRow rows[CENSUS_MAX];
    int nr = 0, reached = 0, kept[4] = { 0 };
    for (int i = 0; i < s_census_n; i++) {
        const CensusObj* c = &s_census[i];
        CensusRow* r = NULL;
        for (int k = 0; k < nr && !r; k++)
            if (rows[k].def_id == c->def_id && rows[k].class_id == c->class_id) r = &rows[k];
        if (!r) {
            r = &rows[nr++];
            memset(r, 0, sizeof(*r));
            r->name = c->name;
            r->def_id = c->def_id;
            r->class_id = c->class_id;
            r->dmin = r->rmin = r->szmin = 1e9f;
        }
        r->n++;
        r->dsum += c->d;
        if (c->d < r->dmin) r->dmin = c->d;
        if (c->d > r->dmax) r->dmax = c->d;
        if (c->range < r->rmin) r->rmin = c->range;
        if (c->range > r->rmax) r->rmax = c->range;
        if (c->sz < 0.0f) continue;
        r->reached++;
        reached++;
        r->szsum += c->sz;
        if (c->sz < r->szmin) r->szmin = c->sz;
        for (int p = 0; p < 3; p++) {
            if (cut_at(c, pcts[p])) r->cut[p]++;
            else kept[p]++;
        }
        if (c->sz < 20.0f) r->cut[3]++;
        else kept[3]++;
    }
    qsort(rows, nr, sizeof(rows[0]), row_cmp);
    LOG_INFO("objcensus", "objects at the distance test: %d (%d kinds), %d within range; kept at 75/50/30%%: %d / %d / %d, "
             "kept at >= 20 px: %d", s_census_n, nr, reached, kept[0], kept[1], kept[2], kept[3]);
    LOG_INFO("objcensus", "columns: def class name | count, in range | distance min/avg/max | range min/max | size min/avg (in "
             "range) | in range but cut at 75/50/30%%, <20 px");
    for (int k = 0; k < nr; k++) {
        const CensusRow* r = &rows[k];
        LOG_INFO("objcensus", "%4d %4d %-11s | %3d %3d | %5.0f %5.0f %5.0f | %5.0f %5.0f | %6.1f %6.1f | %3d %3d %3d %3d", r->def_id, r->class_id,
                 r->name, r->n, r->reached, r->dmin, r->dsum / r->n, r->dmax, r->rmin, r->rmax,
                 r->reached ? r->szmin : -1.0f, r->reached ? r->szsum / r->reached : -1.0f, r->cut[0], r->cut[1],
                 r->cut[2], r->cut[3]);
    }
}

// Skinned models: (model instance, last frame skinned, last frame seen).
// Frozen plants keep their first pose; one not seen for a second may be a new
// object at a reused address, so it is skinned again
#define SKIN_SLOTS 1024
static struct {
    const void* am;
    unsigned skinned, seen;
} s_skin[SKIN_SLOTS];

static int skin_slot(const void* am) {
    u32 h = ((uintptr_t)am >> 4) % SKIN_SLOTS;
    for (u32 i = 0; i < SKIN_SLOTS; i++, h = (h + 1) % SKIN_SLOTS) {
        if (s_skin[h].am == am) {
            if (s_lod_frame - s_skin[h].seen > 60) s_skin[h].skinned = s_lod_frame - 1000;
            s_skin[h].seen = s_lod_frame;
            return (int)h;
        }
        if (!s_skin[h].am || s_lod_frame - s_skin[h].seen > 60) {
            s_skin[h].am = am;
            s_skin[h].skinned = s_lod_frame - 1000;
            s_skin[h].seen = s_lod_frame;
            return (int)h;
        }
    }
    return -1;
}

// Animation LOD (S3D_ANIM_LOD, default 2 = Strong): CPU-skinned models far from the
// camera keep last frame's skinned vertices on some frames. Skinning blends in
// model space and the object's world transform is a GPU matrix, so a skipped
// frame only delays the pose; joints, hit spheres and position still update.
// Mode 1: every frame up to S3D_ANIM_LOD_NEAR (400 units), every 2nd frame up
// to S3D_ANIM_LOD_FAR (900), every 4th beyond. Mode 2 doubles the intervals.
// Called only for models that blend vertices and have no morph targets
// (patches/main/objprint_dolphin.c.sed). Returns 1 to skip skinning
// Which model path objects take (objRenderModel, objprint_dolphin.c.sed):
// the game's batchable path (model flag 0x8000: one display list, matrix loads
// only between same-model objects, which gx_dlcache.c batches) or the full one
void port_model_path(int dll_id, int alt) {
    if (is_plant(dll_id)) {
        if (alt) PROF_COUNT("plant.alt_path", 1);
        else PROF_COUNT("plant.full_path", 1);
    } else {
        if (alt) PROF_COUNT("obj.alt_path", 1);
        else PROF_COUNT("obj.full_path", 1);
    }
}

int port_skin_skip(const void* obj, int dll_id, const void* am) {
    CFG_LIVE(s_plants, cfg_int("S3D_PLANTS", 1));
    static int mode = -1, near_d = -1, far_d = -1;
    CFG_LIVE(mode, cfg_int("S3D_ANIM_LOD", 2));
    CFG_LIVE(near_d, cfg_int("S3D_ANIM_LOD_NEAR", 400));
    CFG_LIVE(far_d, cfg_int("S3D_ANIM_LOD_FAR", 900));
    bool frozen = s_plants == 2 && is_plant(dll_id);
    if (!frozen && mode <= 0) {
        PROF_COUNT("anim.skinned", 1);
        return 0;
    }
    int h = skin_slot(am);
    if (h < 0) return 0;
    unsigned interval = 1;
    if (frozen) {
        interval = 1u << 30;
    } else {
        float d = dist_lookup(obj);
        if (d >= (float)far_d) interval = 4;
        else if (d >= (float)near_d) interval = 2;
        if (mode >= 2) interval *= 2;
    }
    if (s_lod_frame - s_skin[h].skinned < interval) {
        PROF_COUNT("anim.skin_skipped", 1);
        return 1;
    }
    s_skin[h].skinned = s_lod_frame;
    PROF_COUNT("anim.skinned", 1);
    return 0;
}

// Objects drawn with fur: their fur passes scale the joint matrices in place,
// which only works if the matrices are rebuilt every frame
static struct {
    const void* obj;
    unsigned frame;
} s_fur[256];

void port_fur_mark(const void* obj) {
    u32 h = ((uintptr_t)obj >> 4) % 256;
    for (u32 i = 0; i < 8; i++, h = (h + 1) % 256)
        if (s_fur[h].obj == obj || !s_fur[h].obj || s_lod_frame - s_fur[h].frame > 60) {
            s_fur[h].obj = obj;
            s_fur[h].frame = s_lod_frame;
            return;
        }
}

static bool has_fur(const void* obj) {
    u32 h = ((uintptr_t)obj >> 4) % 256;
    for (u32 i = 0; i < 8; i++, h = (h + 1) % 256)
        if (s_fur[h].obj == obj) return s_lod_frame - s_fur[h].frame <= 60;
    return false;
}

// Joint matrices of models that don't blend vertices (plants, props): far
// ones keep last frame's matrices on some frames (same S3D_ANIM_LOD tiers).
// Their world transform is inside the matrices, so this only happens while
// the object stands still (world matrix unchanged). The call site leaves out
// objects with a bone callback or root motion; objects with fur are left out
// here. Returns 1 to skip ObjModel_UpdateAnimMatrices
#define POSE_SLOTS 1024
static struct {
    const void* am;
    unsigned updated, seen;
    float wm[12];
} s_pose[POSE_SLOTS];

int port_anim_skip(const void* obj, int dll_id, const void* am, const float* wm) {
    (void)dll_id;
    static int mode = -1, near_d = -1, far_d = -1;
    CFG_LIVE(mode, cfg_int("S3D_ANIM_LOD", 2));
    CFG_LIVE(near_d, cfg_int("S3D_ANIM_LOD_NEAR", 400));
    CFG_LIVE(far_d, cfg_int("S3D_ANIM_LOD_FAR", 900));
    if (mode <= 0 || has_fur(obj)) return 0;
    float d = dist_lookup(obj);
    unsigned interval = d >= (float)far_d ? 4 : d >= (float)near_d ? 2 : 1;
    if (mode >= 2) interval *= 2;
    if (interval == 1) return 0;
    u32 h = ((uintptr_t)am >> 4) % POSE_SLOTS;
    int slot = -1;
    for (u32 i = 0; i < 16; i++, h = (h + 1) % POSE_SLOTS) {
        if (s_pose[h].am == am) { slot = (int)h; break; }
        if (slot < 0 && (!s_pose[h].am || s_lod_frame - s_pose[h].seen > 60)) slot = (int)h;
    }
    if (slot < 0) return 0;
    bool known = s_pose[slot].am == am && s_lod_frame - s_pose[slot].seen <= 60;
    s_pose[slot].seen = s_lod_frame;
    if (known && memcmp(s_pose[slot].wm, wm, sizeof(s_pose[slot].wm)) == 0 &&
        s_lod_frame - s_pose[slot].updated < interval) {
        PROF_COUNT("anim.pose_skipped", 1);
        return 1;
    }
    s_pose[slot].am = am;
    s_pose[slot].updated = s_lod_frame;
    memcpy(s_pose[slot].wm, wm, sizeof(s_pose[slot].wm));
    PROF_COUNT("anim.pose_updated", 1);
    return 0;
}

// Once per frame (diag_frame)
void lod_frame(void) {
    s_lod_frame++;
    if (s_census_state == 1) {
        if (--s_census_delay > 0) return;
        s_census_state = 2;
        s_census_n = 0;
    } else if (s_census_state == 2) {
        s_census_state = 3;  // One census per run
        census_dump();
    }
}
