// Display list cache.
//
// Decoding GX display lists on the CPU every frame is the most expensive part
// of rendering. Most lists (model and map geometry) are static, so the first
// call decodes a list into linear memory once and records its non-draw
// commands; later calls only replay the matrix loads and draw the cached
// vertices directly.
//
// An entry is reused when the list (address, size, sampled contents), the
// vertex state (formats, arrays, matrix index, texture coordinate setup) and
// the sampled contents of every vertex array it reads are unchanged. Lists
// whose arrays keep changing (CPU-animated meshes) are marked dynamic and
// decoded every time instead.

#include <malloc.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "dl_guard.h"
#include "gx_diag.h"
#include "gx_internal.h"
#include "heaptrack_3ds.h"
#include "log.h"

// Every side-table allocation in this file goes through the guarded heap
// (dl_guard.c): blocks carry magic head/tail words checked at free time, so
// the corrupted-heap crash mode (bug 39) reports which table and entry the
// corruption belongs to instead of faulting inside _free_r. free_entry calls
// dl_free directly with the table names; everything else reports "dl".
#define malloc(n) dl_alloc(n)
#define calloc(n, sz) dl_calloc((n), (sz))
#define realloc(p, n) dl_realloc((p), (n))
#define free(p) dl_free((p), "dl", NULL)
#include "profiler.h"

#define DLCACHE_SLOTS       2048
#define DLCACHE_BUDGET      (12 * 1024 * 1024)  // Linear memory for cached vertices/indices
#define DLCACHE_MAX_ARRAYS  8
#define DLCACHE_DYNAMIC_AFTER 4               // Re-recordings before a list is marked dynamic
// The entries' side tables (segments, ops, gather indices, incremental maps
// and snapshots) live in the C heap. Unbudgeted they ate the whole heap over a
// session (bug 30: 9.6 -> 1.5 MB free, then a map load failed), so they get a
// budget too (S3D_DL_HEAP_KB) and a floor of free heap left for everyone else
#define DLCACHE_HEAP_KB_DEFAULT 6144
#define DLCACHE_HEAP_FLOOR (2 * 1024 * 1024)
#define DLCACHE_HEAP_MIN_AGE 2                // Frames unused before an entry may go (shadow refresh 2)

typedef struct {
    u32 vstart;   // First vertex of the segment
    u32 istart;   // First index (indices are relative to vstart)
    u32 icount;
} DlSegment;

typedef struct {
    u32 offset;   // Command offset in the list, or segment number
    bool is_segment;
} DlOp;

typedef struct {
    const void* data;
    u32 size;
    u32 hash;
    u8 attr;
} DlArrayRef;

typedef struct {
    const void* list;
    u32 nbytes;
    u32 list_hash;
    u32 state_hash;
    DlArrayRef arrays[DLCACHE_MAX_ARRAYS];
    u8 array_count;
    u8 rerecords;
    bool dynamic;
    bool valid;
    void* mem;           // Linear block: vertices then indices
    GxOutVertex* verts;
    GxPackedVertex* pverts;  // Packed vertices instead of verts (pack_vertices), or NULL
    float pack_pos_scale, pack_tc_scale;
    bool no_pack;        // Needed gather after being packed: recorded unpacked from now on
    bool pack_retry;     // Re-recording only to drop the packed layout (not a content change)
    u16* indices;
    u32 vcount;
    // Partial caching of CPU-animated meshes: positions/normals are gathered
    // from the live arrays each frame using the recorded source indices
    u16* pos_idx;
    u16* nrm_idx;
    GxGatherRun* runs;
    u16 run_count;
    bool can_gather_pos, can_gather_nrm;
    bool gather;
    bool gather16;       // Fast path: s16 positions / s8 normals (see gx_vtx_gather16)
    bool gather16_nrm;
    float gather16_scale;
    // Incremental refresh (gather16 entries): persistent per-entry buffer in
    // the dynamic pool, snapshots of the source arrays and source -> output
    // vertex maps, so only vertices whose source changed are rewritten
    GxDyn16Vertex* inc_dyn;
    u32 inc_epoch;
    u8* pos_snap;        // 6 bytes per source position (as in the array)
    u8* nrm_snap;        // 3 bytes per source normal
    u32 pos_srcs, nrm_srcs;
    u32* pos_map_off;    // CSR: outputs of source s are map[off[s] .. off[s+1])
    u16* pos_map;
    u32* nrm_map_off;
    u16* nrm_map;
    u16* pos_refs;       // Distinct source positions/normals this list uses
    u16* nrm_refs;
    u32 pos_ref_count, nrm_ref_count;
    // Sources seen changing ("hot"): between periodic full scans only these
    // are compared, since most vertices of a skinned model never change
    u8* pos_hot;         // Per pos_refs entry
    u8* nrm_hot;
    u16* pos_hot_list;   // Indices into pos_refs
    u16* nrm_hot_list;
    u32 pos_hot_count, nrm_hot_count;
    u16 scans;           // Full scans done so far
    u16 pos_cursor, nrm_cursor;  // Next ref of the rotating partial scan
    DlSegment* segs;
    DlOp* ops;
    u16 seg_count;
    u16 op_count;
    u32 bytes;
    u32 heap_bytes;  // C heap held by the side tables (entry_heap_bytes)
    u32 last_used;
    // Vertex reuse statistics (weld_stats, counters dl.vs_*): indices, output
    // vertices, vertices left after merging identical ones per segment, and
    // simulated post-transform cache misses (FIFO of 4 and 16) for the current
    // and the merged indices
    u32 st_idx, st_vtx, st_weld, st_miss4, st_miss16, st_wmiss4, st_wmiss16;
    // Batching (dlb_eligible / bt_build): 0 not checked, 1 batchable, 2 not.
    // bt_slot: the position matrix slots the vertices use (rank order);
    // bt_mem: bt_k copies of the vertices, copy c using palette slots
    // c * bt_s .. c * bt_s + bt_s - 1, then the indices of every copy
    u8 bt_state;
    u8 bt_s, bt_k;
    u8 bt_miss;  // Holds in a row that ended with one instance (dlb_try)
    u8 bt_slot[10];
    u8 bt_rank[10];
    void* bt_mem;
    u16* bt_idx;
    u32 trust_until;  // Content hashes skipped until this frame (S3D_DL_TRUST)
} DlEntry;

struct DlRecorder {
    GxOutVertex* verts;
    u32 vcount, vcap;
    u16* indices;
    u32 icount, icap;
    DlSegment* segs;
    u32 seg_count, seg_cap;
    DlOp* ops;
    u32 op_count, op_cap;
    u16* pos_idx;
    u16* nrm_idx;
    u32 pidx_cap, nidx_cap;
    GxGatherRun* runs;
    u32 run_count, run_cap;
    bool seg_open;
    bool failed;
    u8 arrays[DLCACHE_MAX_ARRAYS];
    u8 array_count;
};

static DlEntry s_entries[DLCACHE_SLOTS];
static u32 s_cache_bytes;
static u32 s_table_full, s_pool_resets, s_rehashes, s_rehash_dropped;
static u32 s_slot_evicts, s_cold_drops, s_last_pool_reset;
static u32 s_heap_bytes, s_heap_evicts;
static DlRecorder s_rec;

// Held instances of one batchable entry (gx_dlbatch_flush): the matrices of
// its bt_s slots per instance, copied at each call
#define DLB_MAX 64
bool g_dlb_pending;
static struct {
    DlEntry* e;
    u32 count;
    float mats[DLB_MAX * 10][3][4];
} s_dlb;

// --- Hashing ---------------------------------------------------------------------

static inline u32 mix(u32 h, u32 v) { return (h ^ v) * 16777619u; }

static u32 hash_bytes_sampled(const void* data, u32 size, u32 samples) {
    const u8* p = data;
    u32 h = mix(2166136261u, size);
    if (size < 4) {
        for (u32 i = 0; i < size; i++) h = mix(h, p[i]);
        return h;
    }
    u32 words = size / 4;
    u32 step = words > samples ? words / samples : 1;
    for (u32 i = 0; i < words; i += step) {
        u32 w;
        memcpy(&w, p + i * 4, 4);
        h = mix(h, w);
    }
    u32 last;
    memcpy(&last, p + size - 4, 4);
    return mix(h, last);
}

// Hash of the vertex state that decoding depends on (memoized per generation)
static u32 vertex_state_hash(void) {
    static u32 cached_gen = 0xFFFFFFFF;
    static u32 cached_hash;
    if (cached_gen == g_gx.vtx_gen) return cached_hash;

    u32 h = 2166136261u;
    for (int i = 0; i < GX_NUM_ATTR; i++) h = mix(h, g_gx.vtx_desc[i]);
    // Vertex formats hashed a word at a time (sizeof is a multiple of 4)
    const u8* fmt = (const u8*)g_gx.vtx_fmt;
    for (u32 i = 0; i + 4 <= sizeof(g_gx.vtx_fmt); i += 4) {
        u32 w;
        memcpy(&w, fmt + i, 4);
        h = mix(h, w);
    }
    for (int a = GX_VA_POS; a <= GX_VA_TEX7; a++) {
        const GxArray* arr = &g_gx.arrays[a];
        // Position/normal arrays of animated models are double-buffered by
        // the game; their pointers are validated per array instead, so one
        // entry serves both buffers
        if (a != GX_VA_POS && a != GX_VA_NRM) h = mix(h, (u32)(uintptr_t)arr->data);
        h = mix(h, arr->size);
        h = mix(h, arr->stride | (arr->le << 8));
    }
    h = mix(h, g_gx.current_pn_mtx);
    cached_gen = g_gx.vtx_gen;
    cached_hash = h;
    return h;
}

// Texture coordinate setup baked into the cached vertices
static u32 unit_map_hash(const GxUnitMap* map) {
    u32 h = mix(2166136261u, map->unit_count);
    for (int u = 0; u < map->unit_count; u++) {
        int coord = map->unit_texcoord[u];
        h = mix(h, coord);
        h = mix(h, coord >= 0 && coord < g_gx.num_texgens ? g_gx.texgen[coord].src : 0xFF);
    }
    return h;
}

u32 gx_dl_list_hash(const void* list, u32 nbytes) {
    return hash_bytes_sampled(list, nbytes, 32);
}

u32 gx_dl_array_hash(const void* data, u32 size) {
    return hash_bytes_sampled(data, size, 128);
}

u32 gx_dl_array_key(u32 content_hash, const void* data, u32 size) {
    return mix(mix(content_hash, (u32)(uintptr_t)data), size);
}

// Hashes of the current call computed by the GX recorder on the game thread
// (gx_dl_call_hashed): [0] the list, then one gx_dl_array_key per bit of mask
// (attributes in increasing order) for the arrays its vertex descriptors made
// indexed when it was recorded. A display list can change those descriptors
// and arrays itself, so a key that does not match is checked again here.
static struct {
    const void* list;
    u32 mask;
    const u32* h;
} s_pre;

// Sampled content hash of an array, memoized for the current frame
static u32 array_content_hash(const void* data, u32 size) {
    enum { MEMO = 64 };
    static struct { const void* data; u32 size; u32 frame; u32 hash; } memo[MEMO];
    u32 frame = gx_render_frame_number();
    u32 slot = ((uintptr_t)data >> 4) % MEMO;
    if (memo[slot].data == data && memo[slot].size == size && memo[slot].frame == frame) return memo[slot].hash;
    u32 h = hash_bytes_sampled(data, size, 128);
    memo[slot].data = data;
    memo[slot].size = size;
    memo[slot].frame = frame;
    memo[slot].hash = h;
    return h;
}

// --- Recorder (called from gx_vtx.c while a list runs in record mode) --------------

static bool grow(void** buf, u32* cap, u32 need, u32 elem) {
    if (need <= *cap) return true;
    u32 ncap = *cap ? *cap : 256;
    while (ncap < need) ncap *= 2;
    void* n = realloc(*buf, ncap * elem);
    if (!n) return false;
    *buf = n;
    *cap = ncap;
    return true;
}

static void close_segment(DlRecorder* rec) {
    if (!rec->seg_open) return;
    DlSegment* seg = &rec->segs[rec->seg_count - 1];
    seg->icount = rec->icount - seg->istart;
    rec->seg_open = false;
}

static bool push_op(DlRecorder* rec, u32 offset, bool is_segment) {
    if (!grow((void**)&rec->ops, &rec->op_cap, rec->op_count + 1, sizeof(DlOp))) return false;
    rec->ops[rec->op_count].offset = offset;
    rec->ops[rec->op_count].is_segment = is_segment;
    rec->op_count++;
    return true;
}

static bool open_segment(DlRecorder* rec) {
    if (!grow((void**)&rec->segs, &rec->seg_cap, rec->seg_count + 1, sizeof(DlSegment))) return false;
    DlSegment* seg = &rec->segs[rec->seg_count];
    seg->vstart = rec->vcount;
    seg->istart = rec->icount;
    seg->icount = 0;
    rec->seg_open = true;
    return push_op(rec, rec->seg_count++, true);
}

GxOutVertex* gx_dlrec_alloc(DlRecorder* rec, u32 count, u32 index_count, u16** indices_out, u16* base_out,
                            const GxGatherRun* run, u16** pos_idx_out, u16** nrm_idx_out) {
    if (rec->failed) return NULL;
    // 16-bit indices are relative to the segment start
    if (rec->seg_open && rec->vcount + count - rec->segs[rec->seg_count - 1].vstart > 0xFFFF) close_segment(rec);
    if (!rec->seg_open && !open_segment(rec)) goto fail;
    if (!grow((void**)&rec->verts, &rec->vcap, rec->vcount + count, sizeof(GxOutVertex))) goto fail;
    if (!grow((void**)&rec->indices, &rec->icap, rec->icount + index_count, sizeof(u16))) goto fail;
    if (!grow((void**)&rec->pos_idx, &rec->pidx_cap, rec->vcount + count, sizeof(u16))) goto fail;
    if (!grow((void**)&rec->nrm_idx, &rec->nidx_cap, rec->vcount + count, sizeof(u16))) goto fail;

    // Gather runs: merge with the previous run when the encoding is the same
    GxGatherRun* last = rec->run_count ? &rec->runs[rec->run_count - 1] : NULL;
    if (last && last->vstart + last->count == rec->vcount && last->pos_desc == run->pos_desc &&
        last->pos_cnt == run->pos_cnt && last->pos_type == run->pos_type && last->pos_frac == run->pos_frac &&
        last->nrm_desc == run->nrm_desc && last->nrm_type == run->nrm_type) {
        last->count += count;
    } else {
        if (!grow((void**)&rec->runs, &rec->run_cap, rec->run_count + 1, sizeof(GxGatherRun))) goto fail;
        rec->runs[rec->run_count] = *run;
        rec->runs[rec->run_count].vstart = rec->vcount;
        rec->runs[rec->run_count].count = count;
        rec->run_count++;
    }

    memset(&rec->pos_idx[rec->vcount], 0, count * sizeof(u16));
    memset(&rec->nrm_idx[rec->vcount], 0, count * sizeof(u16));
    *pos_idx_out = &rec->pos_idx[rec->vcount];
    *nrm_idx_out = &rec->nrm_idx[rec->vcount];
    *base_out = (u16)(rec->vcount - rec->segs[rec->seg_count - 1].vstart);
    *indices_out = &rec->indices[rec->icount];
    GxOutVertex* v = &rec->verts[rec->vcount];
    rec->vcount += count;
    rec->icount += index_count;
    return v;

fail:
    rec->failed = true;
    return NULL;
}

void gx_dlrec_command(DlRecorder* rec, u32 offset, bool splits_draws) {
    if (rec->failed) return;
    if (splits_draws) close_segment(rec);
    if (!push_op(rec, offset, false)) rec->failed = true;
}

void gx_dlrec_array(DlRecorder* rec, int attr) {
    for (int i = 0; i < rec->array_count; i++) {
        if (rec->arrays[i] == attr) return;
    }
    if (rec->array_count < DLCACHE_MAX_ARRAYS) rec->arrays[rec->array_count++] = (u8)attr;
    else rec->failed = true;  // Cannot validate: do not cache
}

// --- Entries ------------------------------------------------------------------------

// Bug 17: an entry's memory must outlive every GPU frame that drew it, or a
// new recording (or any other linearAlloc) reuses it and an earlier draw reads
// garbage indices; on a real New 3DS that hung the GPU in the hub. With GPU
// pipelining that can be the previous frame too, so the fence decides
// (gx_gpu_free_linear, gx_init.c)
static void free_entry(DlEntry* e) {
    gx_water_tess_invalidate(e);  // Its cached tessellations key on this entry
    if (e->mem) gx_gpu_free_linear(e->mem, e->last_used);
    if (e == s_dlb.e) {  // Not expected: held entries were used this frame
        PROF_COUNT("dlb.dropped", 1);
        s_dlb.e = NULL;
        g_dlb_pending = false;
    }
    if (e->bt_mem) gx_gpu_free_linear(e->bt_mem, e->last_used);
    e->bt_mem = NULL;
    e->bt_idx = NULL;
    e->bt_state = 0;
    e->bt_miss = 0;
    dl_free(e->segs, "segs", e);
    dl_free(e->ops, "ops", e);
    dl_free(e->pos_idx, "pos_idx", e);
    dl_free(e->nrm_idx, "nrm_idx", e);
    dl_free(e->runs, "runs", e);
    dl_free(e->pos_snap, "pos_snap", e);
    dl_free(e->nrm_snap, "nrm_snap", e);
    dl_free(e->pos_map_off, "pos_map_off", e);
    dl_free(e->pos_map, "pos_map", e);
    dl_free(e->nrm_map_off, "nrm_map_off", e);
    dl_free(e->nrm_map, "nrm_map", e);
    dl_free(e->pos_refs, "pos_refs", e);
    dl_free(e->nrm_refs, "nrm_refs", e);
    dl_free(e->pos_hot, "pos_hot", e);
    dl_free(e->nrm_hot, "nrm_hot", e);
    dl_free(e->pos_hot_list, "pos_hot_list", e);
    dl_free(e->nrm_hot_list, "nrm_hot_list", e);
    e->pos_refs = e->nrm_refs = NULL;
    e->pos_hot = e->nrm_hot = NULL;
    e->pos_hot_list = e->nrm_hot_list = NULL;
    e->pos_hot_count = e->nrm_hot_count = 0;
    e->scans = 0;
    e->pos_snap = e->nrm_snap = NULL;
    e->pos_map_off = e->nrm_map_off = NULL;
    e->pos_map = e->nrm_map = NULL;
    e->inc_dyn = NULL;
    s_cache_bytes -= e->bytes;
    s_heap_bytes -= e->heap_bytes;
    e->heap_bytes = 0;
    e->mem = NULL;
    e->verts = NULL;
    e->pverts = NULL;
    e->segs = NULL;
    e->ops = NULL;
    e->pos_idx = NULL;
    e->nrm_idx = NULL;
    e->runs = NULL;
    e->gather = false;
    e->bytes = 0;
    e->valid = false;
}

// Free least recently used entries (not used this frame) until 'needed' fits
static void make_room(u32 needed) {
    u32 frame = gx_render_frame_number();
    while (s_cache_bytes + needed > DLCACHE_BUDGET) {
        DlEntry* oldest = NULL;
        for (int i = 0; i < DLCACHE_SLOTS; i++) {
            DlEntry* e = &s_entries[i];
            if (e->valid && e->last_used != frame && (!oldest || e->last_used < oldest->last_used)) oldest = e;
        }
        if (!oldest) return;
        free_entry(oldest);
        PROF_COUNT("dl.evicted", 1);
    }
}

static u32 entry_heap_bytes(const DlEntry* e) {
    const void* p[] = {e->segs, e->ops, e->pos_idx, e->nrm_idx, e->runs, e->pos_snap, e->nrm_snap,
                       e->pos_map_off, e->pos_map, e->nrm_map_off, e->nrm_map, e->pos_refs, e->nrm_refs,
                       e->pos_hot, e->nrm_hot, e->pos_hot_list, e->nrm_hot_list};
    u32 n = 0;
    for (u32 i = 0; i < sizeof(p) / sizeof(p[0]); i++)
        if (p[i]) n += malloc_usable_size((void*)p[i]);
    return n;
}

// After an entry allocated side tables: account them, then free least
// recently used entries while the tables are over budget or the heap is low.
// 'e' itself is in use (or not valid yet) and is never chosen.
static void heap_account(DlEntry* e) {
    u32 n = entry_heap_bytes(e);
    s_heap_bytes += n - e->heap_bytes;
    e->heap_bytes = n;

    static int budget_kb;
    CFG_LIVE(budget_kb, cfg_int("S3D_DL_HEAP_KB", DLCACHE_HEAP_KB_DEFAULT));
    u32 budget = budget_kb > 0 ? (u32)budget_kb << 10 : 0xFFFFFFFFu;
    u32 frame = gx_render_frame_number();
    while (s_heap_bytes > budget || heaptrack_heap_free() < DLCACHE_HEAP_FLOOR) {
        DlEntry* oldest = NULL;
        for (int i = 0; i < DLCACHE_SLOTS; i++) {
            DlEntry* c = &s_entries[i];
            if (c->valid && c->heap_bytes && c != e && frame - c->last_used > DLCACHE_HEAP_MIN_AGE &&
                (!oldest || c->last_used < oldest->last_used))
                oldest = c;
        }
        if (!oldest) return;
        free_entry(oldest);
        s_heap_evicts++;
        PROF_COUNT("dl.heap_evicts", 1);
    }
}

// Entries are keyed by (list, state): the same list drawn with different
// vertex/texture setups (e.g. multi-pass effects) gets one entry per setup.
// When the table has no free slot or tombstone, the least recently used entry
// (not used this frame) is evicted, so a table full of cold lists from areas
// visited earlier does not force every new list to draw uncached
static DlEntry* find_slot(const void* list, u32 state_hash) {
    u32 start = (((uintptr_t)list >> 5) ^ state_hash) % DLCACHE_SLOTS;
    u32 frame = gx_render_frame_number();
    DlEntry* free_slot = NULL;
    DlEntry* oldest = NULL;
    for (u32 i = 0; i < DLCACHE_SLOTS; i++) {
        DlEntry* e = &s_entries[(start + i) % DLCACHE_SLOTS];
        if (e->list == list && e->state_hash == state_hash) return e;
        if (!e->list) return free_slot ? free_slot : e;  // End of probe chain
        if (!e->valid && !e->dynamic && !free_slot) free_slot = e;
        if (e->last_used != frame && (!oldest || e->last_used < oldest->last_used)) oldest = e;
    }
    if (free_slot || !oldest) return free_slot;
    if (oldest->valid) free_entry(oldest);
    oldest->dynamic = false;  // The caller resets the key (list differs)
    s_slot_evicts++;
    PROF_COUNT("dl.slot_evicts", 1);
    return oldest;
}

// Check the vertex arrays the list reads. If only positions/normals changed
// and the entry can refresh them, switch it to gather mode instead of failing.
static bool arrays_unchanged(DlEntry* e, const u32* pre, u32 pre_mask) {
    for (int i = 0; i < e->array_count; i++) {
        const DlArrayRef* a = &e->arrays[i];
        bool gatherable = (a->attr == GX_VA_POS && e->can_gather_pos) || (a->attr == GX_VA_NRM && e->can_gather_nrm);
        if (gatherable && e->gather) continue;
        const GxArray* cur = &g_gx.arrays[a->attr];
        if (cur->data == a->data && cur->size == a->size) {
            u32 bit = 1u << a->attr;
            if (pre && (pre_mask & bit) &&
                pre[1 + __builtin_popcount(pre_mask & (bit - 1))] == gx_dl_array_key(a->hash, a->data, a->size))
                continue;
            if (pre) PROF_COUNT("dl.prehash_rechecks", 1);
            if (array_content_hash(cur->data, cur->size) == a->hash) continue;
        }
        if (!gatherable) return false;
        if (e->pverts) {
            // Gather reads GxOutVertex: record this entry unpacked from now on
            e->no_pack = true;
            e->pack_retry = true;
            PROF_COUNT("dl.unpacked_for_gather", 1);
            return false;
        }
        e->gather = true;
        PROF_COUNT("dl.became_gather", 1);
    }
    return true;
}

// --- Packed layout -------------------------------------------------------------------

// Fraction bits a value needs to be an exact s16 fixed-point number, or -1
static int exact_frac(float v) {
    for (int f = 0; f <= 15; f++) {
        if (!(fabsf(v) <= 32767.0f)) return -1;  // Also rejects NaN
        if (v == truncf(v)) return f;
        v *= 2.0f;
    }
    return -1;
}

// Common fraction bits for a strided set of floats, or -1 if some value has no
// exact s16 representation at that precision
static int common_frac(const float* first, u32 count, u32 stride_floats, u32 comps) {
    int frac = 0;
    for (u32 i = 0; i < count; i++) {
        const float* v = first + i * stride_floats;
        for (u32 c = 0; c < comps; c++) {
            int f = exact_frac(v[c]);
            if (f < 0) return -1;
            if (f > frac) frac = f;
        }
    }
    float s = (float)(1 << frac);
    for (u32 i = 0; i < count; i++) {
        const float* v = first + i * stride_floats;
        for (u32 c = 0; c < comps; c++) {
            if (!(fabsf(v[c] * s) <= 32767.0f)) return -1;
        }
    }
    return frac;
}

// Find exact fixed-point scales for the recorded positions and texcoords (most
// models come from s16 arrays); false if the packed layout would lose precision
static bool can_pack(const DlRecorder* rec, int* pos_frac, int* tc_frac) {
    static int s_on = -1;  // S3D_DL_PACK
    if (s_on < 0) s_on = cfg_bool("S3D_DL_PACK", true) ? 1 : 0;
    if (!s_on || !rec->vcount) return false;
    const u32 stride = sizeof(GxOutVertex) / sizeof(float);
    *pos_frac = common_frac(rec->verts[0].pos, rec->vcount, stride, 3);
    if (*pos_frac < 0) return false;
    *tc_frac = common_frac(&rec->verts[0].tc[0][0], rec->vcount, stride, GX_PICA_TEX_UNITS * 2);
    return *tc_frac >= 0;
}

static void pack_vertices(const GxOutVertex* in, u32 count, int pos_frac, int tc_frac, GxPackedVertex* out) {
    float ps = (float)(1 << pos_frac), ts = (float)(1 << tc_frac);
    for (u32 i = 0; i < count; i++) {
        const GxOutVertex* v = &in[i];
        GxPackedVertex* p = &out[i];
        for (int c = 0; c < 3; c++) p->pos[c] = (s16)(v->pos[c] * ps);
        p->pos[3] = 0;
        memcpy(p->color, v->color, 4);
        memcpy(p->idx, v->idx, 4);
        for (int t = 0; t < GX_PICA_TEX_UNITS; t++) {
            p->tc[t][0] = (s16)(v->tc[t][0] * ts);
            p->tc[t][1] = (s16)(v->tc[t][1] * ts);
        }
        memcpy(p->nrm, v->nrm, sizeof(p->nrm));
    }
}

// --- Vertex reuse statistics (MULTICORE_PLAN.md: the GPU is vertex-bound) ---------
// Each GX primitive is recorded as its own output vertices, so vertices shared
// between primitives are shaded again. Per segment (one draw): how many
// output vertices are identical (same decoded vertex and the same gather
// sources), and how many vertex shader runs a small FIFO post-transform cache
// would leave with the current indices and with identical vertices merged.
// Computed once when the list is recorded; costs nothing per frame.
static u32 s_vs_frame[7];  // Sums over the entries played this frame (st_* order)

static u32 fifo_misses(const u16* idx, u32 n, const u16* remap, int k) {
    u16 c[16];
    int len = 0, head = 0;
    u32 miss = 0;
    for (u32 i = 0; i < n; i++) {
        u16 v = remap ? remap[idx[i]] : idx[i];
        bool hit = false;
        for (int j = 0; j < len; j++) {
            if (c[j] == v) {
                hit = true;
                break;
            }
        }
        if (hit) continue;
        miss++;
        if (len < k) {
            c[len++] = v;
        } else {
            c[head] = v;
            head = (head + 1) % k;
        }
    }
    return miss;
}

static u32 weld_key(const DlRecorder* rec, u32 v) {
    const u8* p = (const u8*)&rec->verts[v];
    u32 h = 2166136261u;
    for (u32 i = 0; i < sizeof(GxOutVertex); i++) h = (h ^ p[i]) * 16777619u;
    if (rec->pos_idx) h = (h ^ rec->pos_idx[v]) * 16777619u;
    if (rec->nrm_idx) h = (h ^ rec->nrm_idx[v]) * 16777619u;
    return h;
}

static bool weld_same(const DlRecorder* rec, u32 a, u32 b) {
    if (memcmp(&rec->verts[a], &rec->verts[b], sizeof(GxOutVertex))) return false;
    if (rec->pos_idx && rec->pos_idx[a] != rec->pos_idx[b]) return false;
    if (rec->nrm_idx && rec->nrm_idx[a] != rec->nrm_idx[b]) return false;
    return true;
}

static void weld_stats(DlEntry* e, const DlRecorder* rec) {
    static u16* remap;
    static s32* table;
    static u32 remap_cap, table_cap;
    e->st_idx = e->st_vtx = e->st_weld = e->st_miss4 = e->st_miss16 = e->st_wmiss4 = e->st_wmiss16 = 0;
    for (u32 s = 0; s < rec->seg_count; s++) {
        const DlSegment* seg = &rec->segs[s];
        if (!seg->icount) continue;
        const u16* idx = &rec->indices[seg->istart];
        u32 nv = 0;
        for (u32 i = 0; i < seg->icount; i++)
            if (idx[i] + 1u > nv) nv = idx[i] + 1u;
        if (seg->vstart + nv > rec->vcount) return;  // Should not happen: no statistics for this entry
        u32 tsize = 16;
        while (tsize < nv * 2) tsize <<= 1;
        if (nv > remap_cap) {
            u16* r = realloc(remap, nv * sizeof(u16));
            if (!r) return;
            remap = r;
            remap_cap = nv;
        }
        if (tsize > table_cap) {
            s32* t = realloc(table, tsize * sizeof(s32));
            if (!t) return;
            table = t;
            table_cap = tsize;
        }
        for (u32 i = 0; i < tsize; i++) table[i] = -1;
        u32 unique = 0;
        for (u32 i = 0; i < nv; i++) {
            u32 v = seg->vstart + i;
            u32 slot = weld_key(rec, v) & (tsize - 1);
            while (table[slot] >= 0 && !weld_same(rec, seg->vstart + (u32)table[slot], v)) slot = (slot + 1) & (tsize - 1);
            if (table[slot] < 0) {
                table[slot] = (s32)i;
                unique++;
            }
            remap[i] = (u16)table[slot];
        }
        e->st_idx += seg->icount;
        e->st_vtx += nv;
        e->st_weld += unique;
        e->st_miss4 += fifo_misses(idx, seg->icount, NULL, 4);
        e->st_miss16 += fifo_misses(idx, seg->icount, NULL, 16);
        e->st_wmiss4 += fifo_misses(idx, seg->icount, remap, 4);
        e->st_wmiss16 += fifo_misses(idx, seg->icount, remap, 16);
    }
}

// Move the recording into linear memory as the entry's contents
static bool store_recording(DlEntry* e, const DlRecorder* rec) {
    int pos_frac = 0, tc_frac = 0;
    bool packed = !e->no_pack && can_pack(rec, &pos_frac, &tc_frac);
    u32 vbytes = rec->vcount * (packed ? sizeof(GxPackedVertex) : sizeof(GxOutVertex));
    vbytes = (vbytes + 3) & ~3u;
    u32 ibytes = rec->icount * sizeof(u16);
    u32 bytes = vbytes + ((ibytes + 15) & ~15u);
    if (bytes > DLCACHE_BUDGET / 4) return false;

    // Lists without drawable geometry (state only, lines/points) keep just their ops
    void* mem = NULL;
    if (bytes) {
        make_room(bytes);
        mem = linearAlloc(bytes);
        if (!mem) {
            make_room(DLCACHE_BUDGET);
            mem = linearAlloc(bytes);
            // Memory freed this frame or last waits for the GPU (gx_gpu_free_linear)
            if (!mem && gx_gpu_reclaim()) mem = linearAlloc(bytes);
            if (!mem) return false;
        }
    }

    e->segs = malloc(rec->seg_count * sizeof(DlSegment) + 1);
    e->ops = malloc(rec->op_count * sizeof(DlOp) + 1);
    if (!e->segs || !e->ops) {
        if (mem) linearFree(mem);
        free(e->segs);
        free(e->ops);
        e->segs = NULL;
        e->ops = NULL;
        return false;
    }

    e->mem = mem;
    e->verts = packed ? NULL : mem;
    e->pverts = packed ? mem : NULL;
    e->indices = (u16*)((u8*)mem + vbytes);
    if (packed) {
        pack_vertices(rec->verts, rec->vcount, pos_frac, tc_frac, e->pverts);
        e->pack_pos_scale = 1.0f / (float)(1 << pos_frac);
        e->pack_tc_scale = 1.0f / (float)(1 << tc_frac);
        PROF_COUNT("dl.packed_entries", 1);
        PROF_COUNT("dl.packed_saved_kb", rec->vcount * (sizeof(GxOutVertex) - sizeof(GxPackedVertex)) / 1024);
    } else if (rec->vcount) {
        memcpy(e->verts, rec->verts, rec->vcount * sizeof(GxOutVertex));
    }
    memcpy(e->indices, rec->indices, ibytes);
    if (mem) GSPGPU_FlushDataCache(mem, bytes);
    memcpy(e->segs, rec->segs, rec->seg_count * sizeof(DlSegment));
    memcpy(e->ops, rec->ops, rec->op_count * sizeof(DlOp));
    e->seg_count = rec->seg_count;
    e->op_count = rec->op_count;
    e->vcount = rec->vcount;
    weld_stats(e, rec);

    // Keep source indices only if positions/normals are indexed somewhere
    for (u32 r = 0; r < rec->run_count; r++) {
        const GxGatherRun* run = &rec->runs[r];
        if (run->pos_desc == GX_INDEX8 || run->pos_desc == GX_INDEX16) e->can_gather_pos = true;
        if (run->nrm_desc == GX_INDEX8 || run->nrm_desc == GX_INDEX16) e->can_gather_nrm = true;
    }
    if (e->can_gather_pos || e->can_gather_nrm) {
        e->pos_idx = malloc(rec->vcount * sizeof(u16) + 1);
        e->nrm_idx = malloc(rec->vcount * sizeof(u16) + 1);
        e->runs = malloc(rec->run_count * sizeof(GxGatherRun) + 1);
        if (e->pos_idx && e->nrm_idx && e->runs) {
            memcpy(e->pos_idx, rec->pos_idx, rec->vcount * sizeof(u16));
            memcpy(e->nrm_idx, rec->nrm_idx, rec->vcount * sizeof(u16));
            memcpy(e->runs, rec->runs, rec->run_count * sizeof(GxGatherRun));
            e->run_count = rec->run_count;

            // Fast path if every run uses indexed s16 XYZ positions with one
            // fixed-point scale and indexed s8 normals (or no normals)
            const GxGatherRun* r0 = &rec->runs[0];
            bool fast = rec->run_count > 0;
            bool nrm = r0->nrm_desc == GX_INDEX8 || r0->nrm_desc == GX_INDEX16;
            for (u32 r = 0; r < rec->run_count && fast; r++) {
                const GxGatherRun* run = &rec->runs[r];
                bool pidx = run->pos_desc == GX_INDEX8 || run->pos_desc == GX_INDEX16;
                bool nidx = run->nrm_desc == GX_INDEX8 || run->nrm_desc == GX_INDEX16;
                fast = pidx && run->pos_type == GX_S16 && run->pos_cnt == GX_POS_XYZ &&
                       run->pos_frac == r0->pos_frac && nidx == nrm && (!nidx || run->nrm_type == GX_S8) &&
                       (nidx || run->nrm_desc == GX_NONE);
            }
            e->gather16 = fast;
            e->gather16_nrm = nrm;
            e->gather16_scale = 1.0f / (float)(1 << r0->pos_frac);
        } else {
            free(e->pos_idx);
            free(e->nrm_idx);
            free(e->runs);
            e->pos_idx = e->nrm_idx = NULL;
            e->runs = NULL;
            e->can_gather_pos = e->can_gather_nrm = false;
        }
    }
    e->bytes = bytes;
    s_cache_bytes += bytes;
    heap_account(e);
    return true;
}

// --- Incremental refresh of CPU-animated meshes --------------------------------------

// Persistent dynamic buffers live in one linear pool (bump allocated) so the
// CPU cache is flushed once per frame. When the pool fills up it is reset and
// entries rebuild their buffers (detected through the epoch).
#define DYN_POOL_BYTES (3 * 1024 * 1024)
static u8* s_dyn_pool;
static u32 s_dyn_pool_used;
static u32 s_dyn_pool_epoch = 1;
static bool s_dyn_pool_dirty;
static bool s_dyn_pool_reset_pending;
static u32 s_dyn_pool_drawn;  // Last frame that drew a pool buffer directly (0 = none since the reset)

static void* dyn_pool_alloc(u32 bytes) {
    bytes = (bytes + 15) & ~15u;
    if (!s_dyn_pool) {
        s_dyn_pool = linearAlloc(DYN_POOL_BYTES);
        LOG_INFO("dl", "dyn pool %p-%p", (void*)s_dyn_pool, (void*)(s_dyn_pool + DYN_POOL_BYTES));
        if (!s_dyn_pool) return NULL;
    }
    if (s_dyn_pool_used + bytes > DYN_POOL_BYTES) {
        // Buffers drawn earlier in this frame are read by the GPU only when
        // the frame is submitted, so the pool is reset at the next frame start
        s_dyn_pool_reset_pending = true;
        return NULL;
    }
    void* p = s_dyn_pool + s_dyn_pool_used;
    s_dyn_pool_used += bytes;
    return p;
}

// Build a CSR map from source index -> output vertices
static bool build_map(const u16* idx, u32 vcount, u32* srcs_out, u32** off_out, u16** map_out) {
    u32 srcs = 0;
    for (u32 i = 0; i < vcount; i++) if (idx[i] + 1u > srcs) srcs = idx[i] + 1u;
    u32* off = calloc(srcs + 1, sizeof(u32));
    u16* map = malloc(vcount * sizeof(u16) + 1);
    if (!off || !map) { free(off); free(map); return false; }
    for (u32 i = 0; i < vcount; i++) off[idx[i] + 1]++;
    for (u32 s = 0; s < srcs; s++) off[s + 1] += off[s];
    u32* fill = malloc((srcs + 1) * sizeof(u32));
    if (!fill) { free(off); free(map); return false; }
    memcpy(fill, off, (srcs + 1) * sizeof(u32));
    for (u32 i = 0; i < vcount; i++) map[fill[idx[i]]++] = (u16)i;
    free(fill);
    *srcs_out = srcs;
    *off_out = off;
    *map_out = map;
    return true;
}

static inline s16 be_s16(const u8* p, bool le) {
    return le ? (s16)(p[0] | (p[1] << 8)) : (s16)((p[0] << 8) | p[1]);
}

// Sources that at least one output vertex uses (so updates skip the rest)
static bool collect_refs(const u32* off, u32 srcs, u16** refs_out, u32* count_out) {
    u32 n = 0;
    for (u32 s = 0; s < srcs; s++) n += off[s + 1] > off[s];
    u16* refs = malloc(n * sizeof(u16) + 1);
    if (!refs) return false;
    n = 0;
    for (u32 s = 0; s < srcs; s++) {
        if (off[s + 1] > off[s]) refs[n++] = (u16)s;
    }
    *refs_out = refs;
    *count_out = n;
    return true;
}

// First use (or after a pool reset): allocate, build maps, full gather
static bool setup_incremental(DlEntry* e) {
    GxDyn16Vertex* dyn = dyn_pool_alloc(e->vcount * sizeof(GxDyn16Vertex));
    if (!dyn) return false;

    if (!e->pos_map) {
        if (!build_map(e->pos_idx, e->vcount, &e->pos_srcs, &e->pos_map_off, &e->pos_map)) return false;
        if (e->gather16_nrm &&
            !build_map(e->nrm_idx, e->vcount, &e->nrm_srcs, &e->nrm_map_off, &e->nrm_map)) return false;
        e->pos_snap = malloc(e->pos_srcs * 6 + 1);
        e->nrm_snap = e->gather16_nrm ? malloc(e->nrm_srcs * 3 + 1) : NULL;
        if (!e->pos_snap || (e->gather16_nrm && !e->nrm_snap)) return false;
        if (!collect_refs(e->pos_map_off, e->pos_srcs, &e->pos_refs, &e->pos_ref_count)) return false;
        if (e->gather16_nrm && !collect_refs(e->nrm_map_off, e->nrm_srcs, &e->nrm_refs, &e->nrm_ref_count))
            return false;
        e->pos_hot = calloc(e->pos_ref_count + 1, 1);
        e->pos_hot_list = malloc(e->pos_ref_count * sizeof(u16) + 1);
        e->nrm_hot = calloc(e->nrm_ref_count + 1, 1);
        e->nrm_hot_list = malloc(e->nrm_ref_count * sizeof(u16) + 1);
        if (!e->pos_hot || !e->pos_hot_list || !e->nrm_hot || !e->nrm_hot_list) return false;
    }

    // Snapshot the current source data and gather everything once
    const GxArray* parr = &g_gx.arrays[GX_VA_POS];
    const GxArray* narr = &g_gx.arrays[GX_VA_NRM];
    for (u32 s = 0; s < e->pos_srcs; s++) {
        u32 off = s * parr->stride;
        if (off + 6 <= parr->size) memcpy(e->pos_snap + s * 6, parr->data + off, 6);
        else memset(e->pos_snap + s * 6, 0, 6);
    }
    for (u32 s = 0; e->nrm_snap && s < e->nrm_srcs; s++) {
        u32 off = s * narr->stride;
        if (off + 3 <= narr->size) memcpy(e->nrm_snap + s * 3, narr->data + off, 3);
        else memset(e->nrm_snap + s * 3, 0, 3);
    }
    gx_vtx_gather16(e->pos_idx, e->nrm_idx, e->vcount, e->gather16_nrm, dyn);

    e->inc_dyn = dyn;
    e->inc_epoch = s_dyn_pool_epoch;
    PROF_COUNT("dl.gathered_vertices", e->vcount);
    return true;
}

// Positions are 2-byte aligned (stride and offsets are even)
static inline bool update_pos(DlEntry* e, const GxArray* parr, u32 s, GxDyn16Vertex* dyn, u32* updated) {
    const u16* cur = (const u16*)(parr->data + s * parr->stride);
    u16* snap = (u16*)(e->pos_snap + s * 6);
    if (cur[0] == snap[0] && cur[1] == snap[1] && cur[2] == snap[2]) return false;
    snap[0] = cur[0]; snap[1] = cur[1]; snap[2] = cur[2];
    s16 x = parr->le ? (s16)cur[0] : (s16)__builtin_bswap16(cur[0]);
    s16 y = parr->le ? (s16)cur[1] : (s16)__builtin_bswap16(cur[1]);
    s16 z = parr->le ? (s16)cur[2] : (s16)__builtin_bswap16(cur[2]);
    for (u32 k = e->pos_map_off[s]; k < e->pos_map_off[s + 1]; k++) {
        GxDyn16Vertex* o = &dyn[e->pos_map[k]];
        o->pos[0] = x; o->pos[1] = y; o->pos[2] = z;
    }
    *updated += e->pos_map_off[s + 1] - e->pos_map_off[s];
    return true;
}

static inline bool update_nrm(DlEntry* e, const GxArray* narr, u32 s, GxDyn16Vertex* dyn) {
    const u8* cur = narr->data + s * narr->stride;
    u8* snap = e->nrm_snap + s * 3;
    if (cur[0] == snap[0] && cur[1] == snap[1] && cur[2] == snap[2]) return false;
    snap[0] = cur[0]; snap[1] = cur[1]; snap[2] = cur[2];
    for (u32 k = e->nrm_map_off[s]; k < e->nrm_map_off[s + 1]; k++) {
        GxDyn16Vertex* o = &dyn[e->nrm_map[k]];
        o->nrm[0] = (s8)cur[0]; o->nrm[1] = (s8)cur[1]; o->nrm[2] = (s8)cur[2];
    }
    return true;
}

#define HOT_LEARN_SCANS 30   // Full scans before relying on the hot set
#define HOT_RESCAN_EVERY 64  // Frames between full scans afterwards
// Between full scans, also compare 1/HOT_SLICE of the cold refs each frame (a
// rotating window), so a vertex that starts moving after learning is stale
// for at most HOT_SLICE frames instead of HOT_RESCAN_EVERY (stretched
// triangles, "vertex explosions")
#define HOT_SLICE 4

static inline void mark_hot(u8* hot, u16* list, u32* count, u32 r) {
    if (!hot[r]) {
        hot[r] = 1;
        list[(*count)++] = (u16)r;
    }
}

// Rewrite only output vertices whose source position/normal changed
static void update_incremental(DlEntry* e) {
    const GxArray* parr = &g_gx.arrays[GX_VA_POS];
    const GxArray* narr = &g_gx.arrays[GX_VA_NRM];
    GxDyn16Vertex* dyn = e->inc_dyn;
    u32 updated = 0;
    u32 pmax = parr->stride ? parr->size / parr->stride : 0;
    u32 nmax = narr->stride ? narr->size / narr->stride : 0;

    // Full scans while learning, then periodically (staggered per entry)
    u32 frame = gx_render_frame_number();
    bool full = e->scans < HOT_LEARN_SCANS || ((frame + ((uintptr_t)e >> 4)) % HOT_RESCAN_EVERY) == 0;

    if (full) {
        e->scans++;
        for (u32 r = 0; r < e->pos_ref_count; r++) {
            u32 s = e->pos_refs[r];
            if (s < pmax && update_pos(e, parr, s, dyn, &updated))
                mark_hot(e->pos_hot, e->pos_hot_list, &e->pos_hot_count, r);
        }
        for (u32 r = 0; e->nrm_snap && r < e->nrm_ref_count; r++) {
            u32 s = e->nrm_refs[r];
            if (s < nmax && update_nrm(e, narr, s, dyn))
                mark_hot(e->nrm_hot, e->nrm_hot_list, &e->nrm_hot_count, r);
        }
        PROF_COUNT("dl.full_scans", 1);
    } else {
        for (u32 h = 0; h < e->pos_hot_count; h++) {
            u32 s = e->pos_refs[e->pos_hot_list[h]];
            if (s < pmax) update_pos(e, parr, s, dyn, &updated);
        }
        for (u32 h = 0; e->nrm_snap && h < e->nrm_hot_count; h++) {
            u32 s = e->nrm_refs[e->nrm_hot_list[h]];
            if (s < nmax) update_nrm(e, narr, s, dyn);
        }

        // Rotating window over the cold refs
        u32 n = (e->pos_ref_count + HOT_SLICE - 1) / HOT_SLICE;
        u32 r = e->pos_cursor;
        for (u32 i = 0; i < n; i++, r++) {
            if (r >= e->pos_ref_count) r = 0;
            if (e->pos_hot[r]) continue;
            u32 s = e->pos_refs[r];
            if (s < pmax && update_pos(e, parr, s, dyn, &updated)) {
                mark_hot(e->pos_hot, e->pos_hot_list, &e->pos_hot_count, r);
                PROF_COUNT("dl.late_hot", 1);
            }
        }
        e->pos_cursor = (u16)(r >= e->pos_ref_count ? 0 : r);
        if (e->nrm_snap) {
            n = (e->nrm_ref_count + HOT_SLICE - 1) / HOT_SLICE;
            r = e->nrm_cursor;
            for (u32 i = 0; i < n; i++, r++) {
                if (r >= e->nrm_ref_count) r = 0;
                if (e->nrm_hot[r]) continue;
                u32 s = e->nrm_refs[r];
                if (s < nmax && update_nrm(e, narr, s, dyn))
                    mark_hot(e->nrm_hot, e->nrm_hot_list, &e->nrm_hot_count, r);
            }
            e->nrm_cursor = (u16)(r >= e->nrm_ref_count ? 0 : r);
        }
    }

    PROF_COUNT("dl.incremental_vertices", updated);
}

// The persistent buffer is rewritten in place each time its entry plays, so
// the draw reads a copy in frame memory: an earlier draw of the same entry in
// this frame keeps its own pose (bug 18), and with GPU pipelining the previous
// frame's GPU work never sees this frame's update. Out of frame memory, the
// pool buffer itself is drawn (the old behavior; the pool is then flushed and
// its reset waits for the GPU)
static GxDyn16Vertex* frame_copy16(GxDyn16Vertex* src, u32 n) {
    GxDyn16Vertex* d = gx_render_alloc_dynamic(n * sizeof(GxDyn16Vertex));
    if (!d) {
        s_dyn_pool_dirty = true;
        s_dyn_pool_drawn = gx_render_frame_number();
        PROF_COUNT("dl.inc_drawn_in_place", 1);
        return src;
    }
    memcpy(d, src, n * sizeof(GxDyn16Vertex));
    PROF_COUNT("dl.inc_copied_vertices", n);
    return d;
}

// --- DL-call cost split (S3D_DLCALL_PROFILE) --------------------------------------
// The DL-call opcode (gx_dl_call_hashed) is the render thread's biggest single
// cost; its inner zones are hidden by the per-opcode phase in the sampler, so
// these sums split the call into its parts directly. Logged every 120 frames
// as "dlcall" lines: ms/frame per part, per call.
static int s_dlcp_on;
static u64 s_dlcp_t[8];
static u32 s_dlcp_calls;
static u64 s_dlcp_play_gather, s_dlcp_play_draws;
static u32 s_dlcp_frames;

static void dlcp_init(void) {
    static bool done;
    if (done) return;
    done = true;
    s_dlcp_on = cfg_int("S3D_DLCALL_PROFILE", 0) != 0;
}

static void dlcp_add(int part, u64 dt) {
    if (!s_dlcp_on) return;
    s_dlcp_t[part] += dt;
}

static void dlcp_report(int frames) {
    if (!s_dlcp_on || frames <= 0) return;
    double per = 1.0 / (SYSCLOCK_ARM11 / 1000.0) / frames;  // ticks -> ms per frame
    LOG_DEBUG("dlcall", "dlcall %.2f ms/frame, %u calls: prepare %.2f lookup %.2f dlb %.2f validate %.2f gather %.2f "
             "draws %.2f uncached %.2f rest %.2f (play gather %.2f, play draws %.2f)",
             (double)(s_dlcp_t[0] + s_dlcp_t[1] + s_dlcp_t[2] + s_dlcp_t[3] + s_dlcp_t[4] + s_dlcp_t[5] +
                      s_dlcp_t[6] + s_dlcp_t[7]) * per,
             (unsigned)s_dlcp_calls / (unsigned)frames, (double)s_dlcp_t[0] * per, (double)s_dlcp_t[1] * per,
             (double)s_dlcp_t[2] * per, (double)s_dlcp_t[3] * per, (double)s_dlcp_t[4] * per, (double)s_dlcp_t[5] * per,
             (double)s_dlcp_t[6] * per, (double)s_dlcp_t[7] * per, (double)s_dlcp_play_gather * per,
             (double)s_dlcp_play_draws * per);
    memset(s_dlcp_t, 0, sizeof(s_dlcp_t));
    s_dlcp_calls = 0;
    s_dlcp_play_gather = s_dlcp_play_draws = 0;
}

// Called once per frame from the render thread's frame boundary (gx_rec.c):
// the game thread also runs gx_dlcache_frame_end, so the window counter must
// not live there
void gx_dlcache_prof_frame(void) {
    if (!s_dlcp_on) return;
    gx_render_drawc_prof_on();
    if (++s_dlcp_frames >= 120) {
        dlcp_report(120);
        gx_render_drawc_report(120);
        s_dlcp_frames = 0;
    }
}

// Flush the dynamic pool once per frame (called before the frame is submitted)
void gx_dlcache_frame_end(void) {
    gxrec_dlb_log();
    // Cached draws this frame: indices, output vertices, after merging, and
    // simulated vertex shader runs (FIFO cache of 4 / 16) now and after merging
    PROF_COUNT("dl.vs_indices", s_vs_frame[0]);
    PROF_COUNT("dl.vs_vertices", s_vs_frame[1]);
    PROF_COUNT("dl.vs_welded", s_vs_frame[2]);
    PROF_COUNT("dl.vs_runs4", s_vs_frame[3]);
    PROF_COUNT("dl.vs_runs16", s_vs_frame[4]);
    PROF_COUNT("dl.vs_wruns4", s_vs_frame[5]);
    PROF_COUNT("dl.vs_wruns16", s_vs_frame[6]);
    memset(s_vs_frame, 0, sizeof(s_vs_frame));
    if (s_dyn_pool_dirty && s_dyn_pool) {
        GSPGPU_FlushDataCache(s_dyn_pool, DYN_POOL_BYTES);
        s_dyn_pool_dirty = false;
    }
}

static bool play_entry(const DlEntry* e) {
    GxDynVertex* dyn = NULL;
    GxDyn16Vertex* dyn16 = NULL;
    if (s_dlcp_on) s_dlcp_play_gather = s_dlcp_play_draws = 0;
    u64 t_g = svcGetSystemTick();
    s_vs_frame[0] += e->st_idx;
    s_vs_frame[1] += e->st_vtx;
    s_vs_frame[2] += e->st_weld;
    s_vs_frame[3] += e->st_miss4;
    s_vs_frame[4] += e->st_miss16;
    s_vs_frame[5] += e->st_wmiss4;
    s_vs_frame[6] += e->st_wmiss16;
    if (e->gather) {
        GX_PROF_ZONE("gx.gather");
        if (e->gather16) {
            GX_PROF_ZONE("gx.gather16");
            DlEntry* me = (DlEntry*)e;
            if (me->inc_dyn && me->inc_epoch == s_dyn_pool_epoch) {
                update_incremental(me);
                dyn16 = frame_copy16(me->inc_dyn, e->vcount);
            } else if (setup_incremental(me)) {
                heap_account(me);
                dyn16 = frame_copy16(me->inc_dyn, e->vcount);
            } else {
                heap_account(me);  // Keeps what it allocated before failing
                // No persistent buffer this frame: full gather into frame memory
                dyn16 = gx_render_alloc_dynamic(e->vcount * sizeof(GxDyn16Vertex));
                if (!dyn16) return false;
                gx_vtx_gather16(e->pos_idx, e->nrm_idx, e->vcount, e->gather16_nrm, dyn16);
                PROF_COUNT("dl.gathered_vertices", e->vcount);
            }
        } else {
            dyn = gx_render_alloc_dynamic(e->vcount * sizeof(GxDynVertex));
            if (!dyn) return false;
            gx_vtx_gather(e->runs, e->run_count, e->pos_idx, e->nrm_idx, e->verts, dyn);
            PROF_COUNT("dl.gathered_vertices", e->vcount);
        }
    }
    s_dlcp_play_gather += svcGetSystemTick() - t_g;
    u64 t_d = svcGetSystemTick();

    for (u32 i = 0; i < e->op_count; i++) {
        const DlOp* op = &e->ops[i];
        if (op->is_segment) {
            const DlSegment* seg = &e->segs[op->offset];
            if (!seg->icount) continue;
            GX_PROF_ZONE("gx.dl.segment");
            if (dyn16) {
                gx_render_draw_cached_dynamic16(&e->verts[seg->vstart], &dyn16[seg->vstart],
                                                &e->indices[seg->istart], seg->icount, e->gather16_scale);
            } else if (dyn) {
                gx_render_draw_cached_dynamic(&e->verts[seg->vstart], &dyn[seg->vstart], &e->indices[seg->istart],
                                              seg->icount);
            } else {
                if (e->pverts)
                    gx_render_draw_cached_packed(e, &e->pverts[seg->vstart], &e->indices[seg->istart], seg->icount,
                                                 e->pack_pos_scale, e->pack_tc_scale);
                else
                    gx_render_draw_cached(e, &e->verts[seg->vstart], &e->indices[seg->istart], seg->icount);
            }
        } else {
            GX_PROF_ZONE("gx.dl.listcmd");
            gx_vtx_exec_command(e->list, e->nbytes, op->offset);
        }
    }
    s_dlcp_play_draws += svcGetSystemTick() - t_d;
    PROF_COUNT("dl.replay_ops", e->op_count);
    PROF_COUNT("dl.replay_segments", e->seg_count);
    return true;
}

// --- Display-list batching (S3D_DL_BATCH) -------------------------------------------
//
// Objects the game draws back to back with the same model (its "runtime
// batchable" path, model flag 0x8000: grass clumps, flowers, ...; sorted by
// kind in getVisibleObjects) record only matrix loads between their display
// list calls. During a replay such calls are held (dlb_try) and drawn
// together: the entry gets bt_k copies of its vertices, copy c reading
// palette slots c * bt_s.., so one draw covers up to bt_k instances (10 / slots
// used). Anything that would change render state, draw something else or is
// not a plain setter draws the held instances first, with the state they
// were called with (gx_state_changed, gx_rec.c replay_until).

static int s_dlb_mode = -1;  // S3D_DL_BATCH: 0 off, 1 on

// Can this entry be batched? Static vertices, one segment, no list commands,
// and every vertex on a position matrix slot (0-9)
static bool dlb_eligible(DlEntry* e) {
    if (e->bt_state) return e->bt_state == 1;
    e->bt_state = 2;
    if (!e->valid || e->dynamic || e->gather || e->seg_count != 1 || e->op_count != 1 || !e->ops[0].is_segment ||
        e->segs[0].vstart != 0 || e->segs[0].icount == 0 || e->vcount == 0)
        return false;
    u32 used = 0;
    for (u32 v = 0; v < e->vcount; v++) {
        u8 idx = e->pverts ? e->pverts[v].idx[0] : e->verts[v].idx[0];
        if (idx % 3 || idx >= 30) return false;
        used |= 1u << (idx / 3);
    }
    int s = __builtin_popcount(used);
    int k = 10 / s;
    while (k >= 2 && (u32)k * e->vcount > 65535) k--;
    if (k < 2) return false;
    int r = 0;
    memset(e->bt_rank, 0, sizeof(e->bt_rank));
    for (int slot = 0; slot < 10; slot++) {
        if (!(used & (1u << slot))) continue;
        e->bt_slot[r] = (u8)slot;
        e->bt_rank[slot] = (u8)r++;
    }
    e->bt_s = (u8)s;
    e->bt_k = (u8)k;
    e->bt_state = 1;
    return true;
}

// The instanced copies, built the first time the entry draws 2+ instances
static bool bt_build(DlEntry* e) {
    if (e->bt_mem) return true;
    u32 vc = e->vcount, ic = e->segs[0].icount, k = e->bt_k;
    u32 vsz = e->pverts ? sizeof(GxPackedVertex) : sizeof(GxOutVertex);
    u32 vbytes = (k * vc * vsz + 15) & ~15u;
    u32 bytes = vbytes + ((k * ic * sizeof(u16) + 15) & ~15u);
    make_room(bytes);
    u8* mem = linearAlloc(bytes);
    if (!mem && gx_gpu_reclaim()) mem = linearAlloc(bytes);
    if (!mem) {
        PROF_COUNT("dlb.alloc_failed", 1);
        e->bt_state = 2;
        return false;
    }
    const u8* src = e->pverts ? (const u8*)e->pverts : (const u8*)e->verts;
    u32 idx_off = e->pverts ? offsetof(GxPackedVertex, idx) : offsetof(GxOutVertex, idx);
    for (u32 c = 0; c < k; c++) {
        u8* dst = mem + c * vc * vsz;
        memcpy(dst, src, vc * vsz);
        for (u32 v = 0; v < vc; v++) {
            u8* idx = dst + v * vsz + idx_off;
            idx[0] = (u8)(3 * (c * e->bt_s + e->bt_rank[idx[0] / 3]));
        }
    }
    u16* ind = (u16*)(mem + vbytes);
    const u16* si = &e->indices[e->segs[0].istart];
    for (u32 c = 0; c < k; c++) {
        for (u32 j = 0; j < ic; j++) ind[c * ic + j] = (u16)(si[j] + c * vc);
    }
    GSPGPU_FlushDataCache(mem, bytes);
    e->bt_mem = mem;
    e->bt_idx = ind;
    e->bytes += bytes;
    s_cache_bytes += bytes;
    PROF_COUNT("dlb.built_kb", bytes / 1024);
    return true;
}

static void dlb_capture(const DlEntry* e) {
    float (*m)[3][4] = &s_dlb.mats[s_dlb.count * e->bt_s];
    for (int r = 0; r < e->bt_s; r++) memcpy(m[r], g_gx.xf_mtx[e->bt_slot[r] * 3], sizeof(m[r]));
    s_dlb.count++;
}

// A cache hit: hold it (true) or let the caller draw it now (false)
static bool dlb_try(DlEntry* e) {
    CFG_LIVE(s_dlb_mode, cfg_int("S3D_DL_BATCH", 1));
    if (s_dlb.e == e && s_dlb.count < DLB_MAX) {
        dlb_capture(e);
        return true;
    }
    if (s_dlb.e) gx_dlbatch_flush();
    if (s_dlb_mode <= 0 || !g_gxrec_replaying || !dlb_eligible(e)) return false;
    // A list that keeps ending up alone (drawn once per frame, or between
    // other objects) is held only every 16th frame, to notice when that changes
    if (e->bt_miss >= 8 && (gx_render_frame_number() & 15)) {
        PROF_COUNT("dlb.not_held", 1);
        return false;
    }
    s_dlb.e = e;
    s_dlb.count = 0;
    g_dlb_pending = true;
    dlb_capture(e);
    return true;
}

void gx_dlbatch_flush(void) {
    DlEntry* e = s_dlb.e;
    if (!e) return;
    s_dlb.e = NULL;
    g_dlb_pending = false;
    gxrec_dlb_flushed();
    GX_PROF_ZONE("gx.dl.batch");
    u32 n = s_dlb.count;
    int s = e->bt_s;
    float saved[30][4];
    memcpy(saved, g_gx.xf_mtx, sizeof(saved));
    u32 touched = 0;
    if (n < 2) {
        if (e->bt_miss < 255) e->bt_miss++;
    } else {
        e->bt_miss = 0;
    }
    if (n < 2 || !bt_build(e)) {
        // One by one, each with its own matrices (rows the stream has not
        // reloaded since stay as they are)
        for (u32 i = 0; i < n; i++) {
            for (int r = 0; r < s; r++) {
                int row = e->bt_slot[r] * 3;
                if (!(touched & (1u << row)) && memcmp(g_gx.xf_mtx[row], s_dlb.mats[i * s + r], 48) == 0) continue;
                memcpy(g_gx.xf_mtx[row], s_dlb.mats[i * s + r], 48);
                touched |= 7u << row;
            }
            g_gx.posmtx_rows |= touched;
            g_gx.dirty |= GX_DIRTY_POSMTX;
            if (!play_entry(e)) PROF_COUNT("dlb.play_failed", 1);
        }
        PROF_COUNT(n < 2 ? "dlb.single" : "dlb.unbuilt", n);
    } else {
        u32 k = e->bt_k, ic = e->segs[0].icount, draws = 0;
        for (u32 i = 0; i < n; i += k) {
            u32 m = n - i < k ? n - i : k;
            for (u32 c = 0; c < m; c++) {
                for (int r = 0; r < s; r++) {
                    int row = (c * s + r) * 3;
                    memcpy(g_gx.xf_mtx[row], s_dlb.mats[(i + c) * s + r], 48);
                    touched |= 7u << row;
                }
            }
            g_gx.posmtx_rows |= touched;
            g_gx.dirty |= GX_DIRTY_POSMTX;
            if (e->pverts)
                gx_render_draw_cached_packed(e, (const GxPackedVertex*)e->bt_mem, e->bt_idx, m * ic,
                                             e->pack_pos_scale, e->pack_tc_scale);
            else
                gx_render_draw_cached(e, (const GxOutVertex*)e->bt_mem, e->bt_idx, m * ic);
            draws++;
        }
        PROF_COUNT("dlb.batches", 1);
        PROF_COUNT("dlb.instances", n);
        PROF_COUNT("dlb.draws", draws);
    }
    // Back to the matrices the stream loaded last
    if (!touched) return;
    for (int row = 0; row < 30; row++) {
        if (touched & (1u << row)) memcpy(g_gx.xf_mtx[row], saved[row], sizeof(saved[row]));
    }
    g_gx.posmtx_rows |= touched;
    g_gx.dirty |= GX_DIRTY_POSMTX;
}

// Run a list without the cache, replaying it with a command trace if it fails
static void run_uncached(const void* list, u32 nbytes) {
    GX_PROF_ZONE("gx.dl.uncached");
    static int traces_left = 2;
    u8 desc[GX_NUM_ATTR];
    GxAttrFmt fmt[GX_NUM_VTXFMT][GX_NUM_ATTR];
    if (traces_left > 0) {
        memcpy(desc, g_gx.vtx_desc, sizeof(desc));
        memcpy(fmt, g_gx.vtx_fmt, sizeof(fmt));
    }

    if (!gx_vtx_run_display_list(list, nbytes, NULL) && traces_left > 0) {
        traces_left--;
        memcpy(g_gx.vtx_desc, desc, sizeof(desc));
        memcpy(g_gx.vtx_fmt, fmt, sizeof(fmt));
        gx_vtx_invalidate_layouts();
        LOG_INFO("dltrace", "--- replaying failed %lu-byte display list ---", (unsigned long)nbytes);
        gx_vtx_set_trace(true);
        gx_vtx_run_display_list(list, nbytes, NULL);
        gx_vtx_set_trace(false);
        LOG_INFO("dltrace", "--- end of trace ---");
    }
}

// Replay of a recorded GXCallDisplayList (gx_rec.c): the hashes point into
// the command ring and stay valid during the call
// Map census (S3D_STATE_CENSUS=1, DRAWCALL_PLAN.md "Map blocks: merge by
// material?"): map-block calls (gx_rec.c tags them with bit 31 of nbytes) per
// frame, how many distinct materials they use, how many follow a call with
// the same material, and their triangles. Says what merging map pieces by
// material could save
static bool s_call_map;
static void map_census(const DlEntry* e) {
    static int on = -1;
    CFG_LIVE(on, cfg_int("S3D_STATE_CENSUS", 0));
    if (!on) return;
    enum { SLOTS = 512 };
    static struct {
        u32 key, frame;
    } tab[SLOTS];
    static u32 last_key, last_frame;
    u32 frame = gx_render_frame_number() + 1;
    u32 key = gx_render_census_key();
    PROF_COUNT("mc.calls", 1);
    if (e && e->valid && e->seg_count) PROF_COUNT("mc.triangles", e->segs[0].icount / 3);
    if (key == last_key && frame == last_frame) PROF_COUNT("mc.same_as_prev", 1);
    last_key = key;
    last_frame = frame;
    u32 slot = (key ^ (key >> 13)) % SLOTS;
    for (int n = 0; n < 64; n++, slot = (slot + 1) % SLOTS) {
        if (tab[slot].frame == frame && tab[slot].key == key) return;
        if (tab[slot].frame != frame) {
            tab[slot].key = key;
            tab[slot].frame = frame;
            PROF_COUNT("mc.materials", 1);
            return;
        }
    }
}

void gx_dl_call_hashed(const void* list, u32 nbytes, u32 mask, const u32* hashes) {
    s_call_map = (nbytes >> 31) != 0;
    nbytes &= 0x7FFFFFFFu;
    if (mask) {
        s_pre.list = list;
        s_pre.mask = mask;
        s_pre.h = hashes;
    } else {
        // No game-thread pre-hashes: the render thread hashes the list and
        // the arrays itself (the game thread records ~750 unique lists a
        // frame; its hashing was ~6 ms while the render thread has CPU
        // headroom under the GPU)
        s_pre.list = NULL;
        s_pre.mask = 0;
    }
    PROF_COUNT("dl.prehashed_calls", 1);
    GXCallDisplayList(list, nbytes);
    s_pre.list = NULL;
    s_call_map = false;
}

void GXCallDisplayList(const void* list, u32 nbytes) {
    if (!list || nbytes == 0) return;
    GX_PROF_ZONE("gx.displaylist");
    dlcp_init();
    u64 t0 = svcGetSystemTick(), t1 = t0, t2 = t0, t3 = t0, t4 = t0;

    // While instances are held only matrices can have changed, which leave the
    // unit map alone: the draw that needs them uploads them (gx_dlbatch_flush)
    const GxUnitMap* map =
        g_dlb_pending && g_gx.dirty == GX_DIRTY_POSMTX ? gx_render_unit_map() : gx_render_prepare();
    t1 = svcGetSystemTick();
    dlcp_add(0, t1 - t0);
    u32 state_hash, list_hash;
    DlEntry* e;
    const u32* pre = s_pre.list == list ? s_pre.h : NULL;
    u32 pre_mask = s_pre.mask;
    s_pre.list = NULL;
    {
        GX_PROF_ZONE("gx.dl.lookup");
        state_hash = mix(vertex_state_hash(), unit_map_hash(map));
        e = find_slot(list, state_hash);
    }
    t2 = svcGetSystemTick();
    dlcp_add(1, t2 - t1);
    if (s_call_map) map_census(e);
    u32 frame = gx_render_frame_number();
    // S3D_DL_CACHE=0: decode every list live (debug, bug 36 stale vertex colours)
    static int cache_on = 1;
    CFG_LIVE(cache_on, cfg_int("S3D_DL_CACHE", 1));
    if (!cache_on) {
        if (s_dlb.e) gx_dlbatch_flush();
        run_uncached(list, nbytes);
        dlcp_add(6, svcGetSystemTick() - t2);
        return;
    }

    if (!e) {
        if (s_dlb.e) gx_dlbatch_flush();
        s_table_full++;
        PROF_COUNT("dl.table_full", 1);
        run_uncached(list, nbytes);
        dlcp_add(6, svcGetSystemTick() - t2);
        return;
    }

    if (e->list == list && e->state_hash == state_hash && e->dynamic) {
        if (s_dlb.e) gx_dlbatch_flush();
        e->last_used = frame;
        PROF_COUNT("dl.dynamic", 1);
        run_uncached(list, nbytes);
        dlcp_add(6, svcGetSystemTick() - t2);
        return;
    }

    bool same_key = e && e->list == list && e->state_hash == state_hash;
    bool hit = false, trusted = false;
    // S3D_DL_TRUST: a static (non-dynamic) entry that validated unchanged
    // skips the content hashes until trust_until. The game never mutates
    // these lists; the periodic re-check (a hashed hit every TRUST_WINDOW
    // frames) bounds any surprise, and the dynamic lists never trust.
    static int trust_on = 1;
    CFG_LIVE(trust_on, cfg_int("S3D_DL_TRUST", 1));
    if (trust_on && same_key && e->valid && !e->dynamic && e->nbytes == nbytes &&
        (s32)(frame - e->trust_until) < 0) {
        trusted = true;
        PROF_COUNT("dl.trusted", 1);
    } else {
        list_hash = pre ? pre[0] : hash_bytes_sampled(list, nbytes, 32);
        GX_PROF_ZONE("gx.dl.validate");
        hit = same_key && e->valid && e->nbytes == nbytes && e->list_hash == list_hash && arrays_unchanged(e, pre, pre_mask);
    }
    t3 = svcGetSystemTick();
    dlcp_add(3, t3 - t2);
    if (trusted) hit = true;
    if (hit) {
        e->last_used = frame;
        if (!trusted) e->trust_until = frame + 512;  // The re-check renews it
        PROF_COUNT("dl.hits", 1);
        if (dlb_try(e)) {
            dlcp_add(2, svcGetSystemTick() - t3);
            return;
        }
        t4 = svcGetSystemTick();
        dlcp_add(2, t4 - t3);
        GX_PROF_ZONE("gx.dl.play");
        if (!play_entry(e)) run_uncached(list, nbytes);
        {
            u64 play_total = svcGetSystemTick() - t4;
            u64 g = s_dlcp_play_gather, d = s_dlcp_play_draws;  // This call's (play_entry resets them)
            dlcp_add(4, g);
            dlcp_add(5, d);
            dlcp_add(7, play_total - g - d);
            s_dlcp_calls++;
        }
        return;
    }

    // Miss: (re)record this list. Same key with a valid entry means the list
    // or its vertex arrays changed since it was recorded.
    if (s_dlb.e) gx_dlbatch_flush();
    bool rerecord = same_key && e->valid;
    if (e->valid) free_entry(e);
    if (!same_key) {
        memset(e, 0, sizeof(*e));
        e->list = list;
        e->state_hash = state_hash;
    } else if (e->pack_retry) {
        e->pack_retry = false;  // Layout change only, not a content change
    } else if (rerecord && ++e->rerecords >= DLCACHE_DYNAMIC_AFTER) {
        // Keeps changing (animated vertex data): stop caching it
        e->dynamic = true;
        PROF_COUNT("dl.became_dynamic", 1);
        static int logged;
        if (logged++ < 12) {
            char changed[64] = "";
            int n = 0;
            for (int i = 0; i < e->array_count && n < 56; i++) {
                if (array_content_hash(e->arrays[i].data, e->arrays[i].size) != e->arrays[i].hash)
                    n += snprintf(changed + n, sizeof(changed) - n, " attr%u", e->arrays[i].attr);
            }
            LOG_INFO("dl", "list %p (%lu bytes) is dynamic: list %s, changed arrays:%s", list, (unsigned long)nbytes,
                     e->list_hash != list_hash ? "changed" : "same", changed);
        }
        run_uncached(list, nbytes);
        return;
    }

    DlRecorder* rec = &s_rec;
    rec->vcount = rec->icount = rec->seg_count = rec->op_count = 0;
    rec->seg_open = false;
    rec->failed = false;
    rec->array_count = 0;
    rec->run_count = 0;

    bool ok;
    {
        GX_PROF_ZONE("gx.dl.record");
        ok = gx_vtx_run_display_list(list, nbytes, rec);
        close_segment(rec);
    }
    PROF_COUNT("dl.misses", 1);

    if (ok && !rec->failed && store_recording(e, rec)) {
        e->nbytes = nbytes;
        e->list_hash = list_hash;
        e->state_hash = state_hash;
        e->array_count = rec->array_count;
        for (int i = 0; i < rec->array_count; i++) {
            const GxArray* arr = &g_gx.arrays[rec->arrays[i]];
            e->arrays[i].data = arr->data;
            e->arrays[i].size = arr->size;
            e->arrays[i].hash = array_content_hash(arr->data, arr->size);
            e->arrays[i].attr = rec->arrays[i];
        }
        e->valid = true;
        e->last_used = frame;
        // The recording pass only decoded; replay the list from the cache.
        // Its CP/XF commands already ran during recording and are idempotent.
        if (!play_entry(e)) run_uncached(list, nbytes);
    } else {
        // Could not cache: draw it the normal way. Lists that cannot be
        // recorded (errors, too many arrays) are not retried.
        if (!ok || rec->failed) e->dynamic = true;
        PROF_COUNT("dl.uncacheable", 1);
        run_uncached(list, nbytes);
    }
}

void gx_dlcache_init(void) {
    memset(s_entries, 0, sizeof(s_entries));
    s_cache_bytes = 0;
}

void gx_dlcache_deinit(void) {
    for (int i = 0; i < DLCACHE_SLOTS; i++) {
        if (s_entries[i].valid) free_entry(&s_entries[i]);
    }
    free(s_rec.verts);
    free(s_rec.indices);
    free(s_rec.segs);
    free(s_rec.ops);
    free(s_rec.pos_idx);
    free(s_rec.nrm_idx);
    free(s_rec.runs);
    memset(&s_rec, 0, sizeof(s_rec));
}

// --- Table maintenance ---------------------------------------------------------------
// Freed entries stay in the table as tombstones (probe chains run through
// them) and dynamic lists keep a permanent marker. Over a long session they
// pile up: lookups for new lists walk ever longer chains and eventually the
// table has no free slot, so lists are drawn uncached. Every so often the
// table is rebuilt with only the live entries. Nothing keeps DlEntry pointers
// across frames, so entries can move.

#define DL_MAINT_FRAMES 64
#define DL_STALE_FRAMES 1800  // Dynamic markers unused this long are dropped
#define DL_COLD_FRAMES  1800  // Cached lists unused this long are freed

static bool entry_live(const DlEntry* e, u32 frame) {
    if (!e->list) return false;
    if (e->valid) return true;
    return e->dynamic && frame - e->last_used < DL_STALE_FRAMES;
}

static void maintain_table(u32 frame) {
    u32 used = 0, live = 0;
    for (int i = 0; i < DLCACHE_SLOTS; i++) {
        DlEntry* e = &s_entries[i];
        if (!e->list) continue;
        if (e->valid && frame - e->last_used >= DL_COLD_FRAMES) {
            free_entry(e);  // Leaves a tombstone, dropped by the rebuild below
            s_cold_drops++;
        }
        used++;
        if (entry_live(e, frame)) live++;
    }
    // Rebuild when a quarter of the table is dead weight or it is nearly full
    if (used - live < DLCACHE_SLOTS / 4 && used < DLCACHE_SLOTS * 7 / 8) return;
    if (used == live) return;  // Full of live (recently used) entries: nothing to gain
    DlEntry* tmp = malloc(live * sizeof(DlEntry) + 1);
    if (!tmp) return;
    u32 n = 0;
    for (int i = 0; i < DLCACHE_SLOTS; i++) {
        if (entry_live(&s_entries[i], frame)) tmp[n++] = s_entries[i];
    }
    memset(s_entries, 0, sizeof(s_entries));
    for (u32 k = 0; k < n; k++) {
        u32 start = (((uintptr_t)tmp[k].list >> 5) ^ tmp[k].state_hash) % DLCACHE_SLOTS;
        for (u32 i = 0; i < DLCACHE_SLOTS; i++) {
            DlEntry* e = &s_entries[(start + i) % DLCACHE_SLOTS];
            if (!e->list) {
                *e = tmp[k];
                break;
            }
        }
    }
    free(tmp);
    s_rehashes++;
    s_rehash_dropped += used - live;
    PROF_COUNT("dl.rehash", 1);
}

void gx_dlcache_get_stats(GxDlStats* out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < DLCACHE_SLOTS; i++) {
        const DlEntry* e = &s_entries[i];
        if (!e->list) continue;
        out->used++;
        if (e->valid) out->valid++;
        else if (e->dynamic) out->dynamic++;
        else out->tombs++;
    }
    out->slots = DLCACHE_SLOTS;
    out->kb = s_cache_bytes / 1024;
    out->heap_kb = s_heap_bytes / 1024;
    out->heap_evicts = s_heap_evicts;
    out->table_full = s_table_full;
    out->pool_resets = s_pool_resets;
    out->rehashes = s_rehashes;
    out->dropped = s_rehash_dropped;
    out->evicts = s_slot_evicts;
    out->cold = s_cold_drops;
    out->last_pool_reset = s_last_pool_reset;
}

// Called once per frame from the renderer: report cache usage
void gx_dlcache_frame_stats(void) {
    PROF_COUNT("dl.cache_kb", s_cache_bytes / 1024);
    PROF_COUNT("dl.heap_kb", s_heap_bytes / 1024);
    if (s_dyn_pool_reset_pending) {
        // Pool buffers drawn directly (play_entry's fallback) may still be in
        // use by the previous frame's GPU work when pipelined
        if (s_dyn_pool_drawn && !gx_gpu_frame_done(s_dyn_pool_drawn)) gx_gpu_wait_idle();
        s_dyn_pool_drawn = 0;
        s_dyn_pool_reset_pending = false;
        s_dyn_pool_used = 0;
        s_dyn_pool_epoch++;  // Entries rebuild their buffers on next use
        s_pool_resets++;
        s_last_pool_reset = gx_render_frame_number();
        LOG_INFO("diag", "dl poolreset #%lu at frame %lu", (unsigned long)s_pool_resets,
                 (unsigned long)s_last_pool_reset);
        PROF_COUNT("dl.dyn_pool_reset", 1);
    }
    u32 frame = gx_render_frame_number();
    if (frame % DL_MAINT_FRAMES == 0) maintain_table(frame);
}

void gx_dlrec_fail(DlRecorder* rec) {
    rec->failed = true;
}
