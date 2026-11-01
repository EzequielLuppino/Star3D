// Producer-side state deltas for the GX recorder (S3D_RT_DELTA, default 1).
//
// In delta mode the game thread does not record one command per state setter.
// It applies each setter to a shadow copy of the GX state (the shadow lives
// here, as do the bookkeeping copies the recorder already keeps) and marks the
// changed state regions. Before the next draw (or any other recorded command)
// the pending changes are written to the ring as one GXREC_OP_DELTA command,
// and the replay applies them by copying the changed fields into g_gx and
// marking the dirty bits, exactly where the individual setters would have.
//
// Matrix loads are batched the same way into one GXREC_OP_MBLOCK command whose
// replay calls the real load functions. Setters that a display list could
// overwrite behind the recorder's back (CP / XF registers) are either always
// included (vertex descriptors, formats, arrays) or stay recorded commands
// (GXSetCurrentMtx, GXSetTexCoordGen2), so the shadow never guesses.
//
// Order: a state setter flushes a pending matrix block first and a matrix load
// flushes a pending delta first, so delta and block commands keep the stream
// order the setter sequence had. That order matters for the display-list
// batch holds, which are drawn with the state in place at the flush point.
//
// Verify mode (S3D_RT_DELTA=2, PROFILE=1): every delta setter also records
// the real command; the replay checks each delta's payload against the state
// the recorded commands produced and logs the first mismatches.

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "gx_internal.h"
#include "gx_rec.h"
#include "gxrec_gen.h"
#include "config.h"
#include "log.h"
#include "profiler.h"
#include "ptag.h"

// --- Region table ---------------------------------------------------------------
// One entry per state field (or field group) a setter can change. The delta
// payload is the concatenation of the changed regions in table order.

enum {
    RD_VTX_DESC,
    RD_VTX_FMT,
    // GxArray arrays[GX_NUM_ARRAYS] in three blocks (GX_NUM_ARRAYS = 25)
    RD_ARRAY0,   // arrays[0..7]
    RD_ARRAY1,   // arrays[8..15]
    RD_ARRAY2,   // arrays[16..24]
    RD_PROJ,
    RD_PROJ_TYPE,
    RD_VIEWPORT,
    RD_SCISSOR,
    RD_NUM_TEXGENS,
    RD_NUM_CHANS,
    RD_CHAN,
    RD_CHAN_MAT,
    RD_CHAN_AMB,
    RD_LIGHT0,
    RD_LIGHT1,
    RD_LIGHT2,
    RD_LIGHT3,
    RD_LIGHT4,
    RD_LIGHT5,
    RD_LIGHT6,
    RD_LIGHT7,
    RD_LIGHT_MAX = RD_LIGHT7,
    RD_PIXEL,
    RD_CLEAR,
    RD_LINE_POINT,
    RD_TEX_OFFSET_EN,
    RD_TEX0,
    RD_TEX1,
    RD_TEX2,
    RD_TEX3,
    RD_TEX4,
    RD_TEX5,
    RD_TEX6,
    RD_TEX7,
    RD_TEX_MAX = RD_TEX7,
    RD_TEXCOPY,
    RD_TEV_STAGES,
    RD_TEV_REGS,
    RD_TEV_KCOLORS,
    RD_TEV_SWAP,
    RD_TEV_NUM,
    RD_TEV_WARP,
    RD_TEV_IND,
    RD_TEV_IND_MAP,
    RD_TEV_RIPPLE,
    RD_TEV_NUM_IND,
    RD_COUNT
};

_Static_assert(RD_COUNT <= 64, "region bitmap fits u64");

typedef struct {
    u16 off, size;
} DeltaRegion;

#define RDREG(field) { (u16)offsetof(GxState, field), (u16)sizeof(((GxState*)0)->field) }
#define RDREGS(field, size) { (u16)offsetof(GxState, field), (u16)(size) }

static const DeltaRegion s_region_table[RD_COUNT] = {
    [RD_VTX_DESC] = RDREG(vtx_desc),
    [RD_VTX_FMT] = RDREG(vtx_fmt),
    [RD_ARRAY0] = RDREGS(arrays[0], 8 * sizeof(((GxState*)0)->arrays[0])),
    [RD_ARRAY1] = RDREGS(arrays[8], 8 * sizeof(((GxState*)0)->arrays[0])),
    [RD_ARRAY2] = RDREGS(arrays[16], 9 * sizeof(((GxState*)0)->arrays[0])),
    [RD_PROJ] = RDREG(proj),
    [RD_PROJ_TYPE] = RDREG(proj_type),
    [RD_VIEWPORT] = RDREG(viewport),
    [RD_SCISSOR] = RDREG(scissor),
    [RD_NUM_TEXGENS] = RDREG(num_texgens),
    [RD_NUM_CHANS] = RDREG(num_chans),
    [RD_CHAN] = RDREG(chan),
    [RD_CHAN_MAT] = RDREG(chan_mat),
    [RD_CHAN_AMB] = RDREG(chan_amb),
    [RD_LIGHT0] = RDREG(lights[0]),
    [RD_LIGHT1] = RDREG(lights[1]),
    [RD_LIGHT2] = RDREG(lights[2]),
    [RD_LIGHT3] = RDREG(lights[3]),
    [RD_LIGHT4] = RDREG(lights[4]),
    [RD_LIGHT5] = RDREG(lights[5]),
    [RD_LIGHT6] = RDREG(lights[6]),
    [RD_LIGHT7] = RDREG(lights[7]),
    [RD_PIXEL] = RDREG(pixel),
    // clear_color followed by clear_z (both written by GXSetCopyClear)
    [RD_CLEAR] = RDREGS(clear_color, sizeof(((GxState*)0)->clear_color) + sizeof(((GxState*)0)->clear_z)),
    // line_width, point_size, line_tex_offset, point_tex_offset (u8 each)
    [RD_LINE_POINT] = RDREGS(line_width, 4),
    [RD_TEX_OFFSET_EN] = RDREG(tex_offset_enable),
    [RD_TEX0] = RDREG(tex[0]),
    [RD_TEX1] = RDREG(tex[1]),
    [RD_TEX2] = RDREG(tex[2]),
    [RD_TEX3] = RDREG(tex[3]),
    [RD_TEX4] = RDREG(tex[4]),
    [RD_TEX5] = RDREG(tex[5]),
    [RD_TEX6] = RDREG(tex[6]),
    [RD_TEX7] = RDREG(tex[7]),
    // texcopy_src[4] followed by texcopy_dst_fmt
    [RD_TEXCOPY] = RDREGS(texcopy_src, sizeof(((GxState*)0)->texcopy_src) + sizeof(((GxState*)0)->texcopy_dst_fmt)),
    [RD_TEV_STAGES] = RDREG(tev.stages),
    [RD_TEV_REGS] = RDREG(tev.regs),
    [RD_TEV_KCOLORS] = RDREG(tev.kcolors),
    [RD_TEV_SWAP] = RDREG(tev.swap_tables),
    [RD_TEV_NUM] = RDREG(tev.num_stages),
    [RD_TEV_WARP] = RDREG(tev.stage_warp),
    // stage_ind followed by stage_bump_sel
    [RD_TEV_IND] = RDREGS(tev.stage_ind,
                          sizeof(((GxState*)0)->tev.stage_ind) + sizeof(((GxState*)0)->tev.stage_bump_sel)),
    // ind_coord followed by ind_map
    [RD_TEV_IND_MAP] = RDREGS(tev.ind_coord,
                              sizeof(((GxState*)0)->tev.ind_coord) + sizeof(((GxState*)0)->tev.ind_map)),
    [RD_TEV_RIPPLE] = RDREG(tev.stage_ripple),
    [RD_TEV_NUM_IND] = RDREG(tev.num_ind_stages),
};

// Verify the hand-merged multi-field regions really are contiguous
_Static_assert(offsetof(GxState, clear_z) == offsetof(GxState, clear_color) + sizeof(((GxState*)0)->clear_color),
               "clear_z follows clear_color");
_Static_assert(offsetof(GxState, point_size) == offsetof(GxState, line_width) + 1 &&
               offsetof(GxState, line_tex_offset) == offsetof(GxState, line_width) + 2 &&
               offsetof(GxState, point_tex_offset) == offsetof(GxState, line_width) + 3,
               "line/point fields are contiguous");
_Static_assert(offsetof(GxState, texcopy_dst_fmt) ==
               offsetof(GxState, texcopy_src) + sizeof(((GxState*)0)->texcopy_src),
               "texcopy_dst_fmt follows texcopy_src");
_Static_assert(offsetof(GxState, tev.stage_bump_sel) ==
               offsetof(GxState, tev.stage_ind) + sizeof(((GxState*)0)->tev.stage_ind),
               "stage_bump_sel follows stage_ind");
_Static_assert(offsetof(GxState, tev.ind_map) ==
               offsetof(GxState, tev.ind_coord) + sizeof(((GxState*)0)->tev.ind_coord),
               "ind_map follows ind_coord");

// --- Producer state --------------------------------------------------------------

static bool s_on;        // Delta mode for this frame (S3D_RT_DELTA non-zero)
static bool s_verify;    // PROFILE=1, S3D_RT_DELTA=2: also record the real commands
static GxState s_shadow; // Game thread's mirror of g_gx (fields setters change)
// Bookkeeping the recorder used to keep in gx_rec.c: which arrays a display
// list call can read (its hashes), and the scissor answered to GXGetScissor
static u8 s_vtx_desc[GX_NUM_ATTR];
static struct {
    const void* data;
    u32 size;
} s_array[GX_NUM_ARRAYS];
static u32 s_scissor[4];

// Pending delta (flushed by gxrec_delta_flush)
static u64 s_regions;    // Region bitmap
static u32 s_flags;      // DELTA_FLAG_*
static u32 s_dirty;      // Dirty bit union
static u32 s_vtx_gen;    // vtx_gen bumps to add at apply time
#define DELTA_FLAG_LAYOUTS 1u

// Pending matrix block
#define MBL_MAX 96
#define MBL_ENTRY_WORDS 14  // 1 header + up to 12 words (48-byte matrices)
static u32 s_mblk[2 + MBL_MAX * MBL_ENTRY_WORDS];
static u32 s_mblk_count;
enum { MB_POS = 0, MB_NRM = 1, MB_TEX = 2 };

static bool s_flushing;  // gxrec_delta_flush in progress (it records commands)

#if GX_PROFILE_DETAIL
#define D_VERIFY(expr) do { if (s_verify) { expr; } } while (0)
#else
#define D_VERIFY(expr) do { } while (0)
#endif

static inline void mark_region(int r) { s_regions |= 1ull << r; }

// --- Matrix block ----------------------------------------------------------------

// Flush the pending matrix block as one GXREC_OP_MBLOCK command (the caller
// owns the s_flushing guard: gxrec_begin inside hooks back into the flush)
static void mblk_flush_body(void) {
    PTAG_SUB(PSUB_FILTER);
    u32 words = 1 + s_mblk_count * (1 + 12);
    u32* p = gxrec_begin(GXREC_OP_MBLOCK, words);
    p[0] = s_mblk_count;
    u32* q = p + 1;
    for (u32 i = 0; i < s_mblk_count; i++) {
        const u32* e = &s_mblk[2 + i * MBL_ENTRY_WORDS];
        u32 nw = e[0] & 0xFF;
        memcpy(q, e, (1 + nw) * 4);
        q += 1 + nw;
    }
    s_mblk_count = 0;
    gxrec_end(p + words);
}

static void mblk_flush(void) {
    if (!s_mblk_count || s_flushing) return;
    s_flushing = true;
    mblk_flush_body();
    s_flushing = false;
}

static void mblk_append(u32 kind, u32 id, u32 type, const void* mtx, u32 mtx_words) {
    if (s_mblk_count == MBL_MAX) mblk_flush();
    u32* e = &s_mblk[2 + s_mblk_count * MBL_ENTRY_WORDS];
    e[0] = (kind << 24) | (type << 16) | (id << 8) | mtx_words;
    memcpy(e + 1, mtx, mtx_words * 4);
    s_mblk_count++;
}

// A state setter arrives: the matrix block's loads came first in the stream
static inline void d_pre_setter(void) {
    if (s_flushing) return;
    if (s_mblk_count) mblk_flush();
}

// A matrix load arrives: the pending delta's setters came first in the stream
static inline void d_pre_matrix(void) {
    if (s_flushing) return;
    if (s_regions || s_flags || s_dirty || s_vtx_gen) gxrec_delta_flush();
}

// --- Delta packet ----------------------------------------------------------------

static u32 delta_payload_words(u64 bm) {
    u32 n = 0;
    for (int i = 0; i < RD_COUNT; i++)
        if (bm & (1ull << i)) n += (s_region_table[i].size + 3) / 4;
    return n;
}

// Verify mode: a hash over everything the shadow controls, so a setter change
// the compare logic wrongly dropped shows up even though its region is not in
// the payload. Computed on the shadow at flush time and on g_gx at apply time
// (after the recorded commands replayed), so the two must agree.
static u32 shadow_hash(const GxState* s) {
    u32 h = 2166136261u;
    for (int i = 0; i < RD_COUNT; i++) {
        const u8* p = (const u8*)s + s_region_table[i].off;
        for (u32 j = 0; j < s_region_table[i].size; j++) h = (h ^ p[j]) * 16777619u;
    }
    return h;
}

void gxrec_delta_flush(void) {
    if (s_flushing) return;
    if (!s_on) return;
    s_flushing = true;
    if (s_mblk_count) mblk_flush_body();
    if (s_regions || s_flags || s_dirty || s_vtx_gen || s_verify) {
        PTAG_SUB(PSUB_FILTER);
        u32 words = 6 + delta_payload_words(s_regions);
        u32* p = gxrec_begin(GXREC_OP_DELTA, words);
        p[0] = (u32)s_regions;
        p[1] = (u32)(s_regions >> 32);
        p[2] = s_flags;
        p[3] = s_dirty;
        p[4] = s_vtx_gen;
        p[5] = s_verify ? shadow_hash(&s_shadow) : 0;
        u8* dst = (u8*)(p + 6);
        for (int i = 0; i < RD_COUNT; i++)
            if (s_regions & (1ull << i)) {
                memcpy(dst, (const u8*)&s_shadow + s_region_table[i].off, s_region_table[i].size);
                dst += s_region_table[i].size;
            }
        s_regions = 0;
        s_flags = 0;
        s_dirty = 0;
        s_vtx_gen = 0;
        gxrec_end(p + words);
        GX_PROF_COUNT("rec.delta_flushed", 1);
    }
    s_flushing = false;
}

// The model-draw seam (gx_rec.c port_model_draw): instead of emitting
// separate MBLOCK and DELTA ops before the draw, the pending matrix block
// and state delta are serialized here and carried inside the model-draw op
// (RENDERER_CHANNEL.md stage A/B). Layout: [mblk body][delta body], sizes
// returned in mwords/dwords.
const u32* gxrec_delta_take(u32* mwords, u32* dwords) {
    static u32 buf[1 + MBL_MAX * MBL_ENTRY_WORDS + 6 + (sizeof(s_shadow) / 4)];
    u32* q = buf;
    if (s_mblk_count) {
        *q++ = s_mblk_count;
        for (u32 i = 0; i < s_mblk_count; i++) {
            const u32* e = &s_mblk[2 + i * MBL_ENTRY_WORDS];
            u32 nw = e[0] & 0xFF;
            memcpy(q, e, (1 + nw) * 4);
            q += 1 + nw;
        }
        s_mblk_count = 0;
    }
    *mwords = (u32)(q - buf);
    if (s_regions || s_flags || s_dirty || s_vtx_gen || s_verify) {
        *q++ = (u32)s_regions;
        *q++ = (u32)(s_regions >> 32);
        *q++ = s_flags;
        *q++ = s_dirty;
        *q++ = s_vtx_gen;
        *q++ = s_verify ? shadow_hash(&s_shadow) : 0;
        for (int i = 0; i < RD_COUNT; i++)
            if (s_regions & (1ull << i)) {
                memcpy(q, (const u8*)&s_shadow + s_region_table[i].off, s_region_table[i].size);
                q += s_region_table[i].size / 4;
            }
        s_regions = 0;
        s_flags = 0;
        s_dirty = 0;
        s_vtx_gen = 0;
        GX_PROF_COUNT("rec.delta_taken", 1);
    }
    *dwords = (u32)(q - buf) - *mwords;
    return buf;
}

// The consumer side: apply one recorded delta packet
void gxrec_delta_apply(const u32* p, u32 words) {
    (void)words;
    u64 bm = p[0] | ((u64)p[1] << 32);
    u32 flags = p[2], dirty = p[3], vtx_gen = p[4];
    const u8* src = (const u8*)(p + 6);
    if (flags & DELTA_FLAG_LAYOUTS) gx_vtx_invalidate_layouts();
    if (dirty) {
        // The same order as gx_state_changed: the holds and the pending
        // immediate-mode batch draw with the state before these setters
        if (g_dlb_pending && dirty != GX_DIRTY_POSMTX) gx_dlbatch_flush();
        gx_render_flush();
    }
#if GX_PROFILE_DETAIL
    if (s_verify) {
        // The recorded commands were replayed first: the fields must match
        const u8* chk = src;
        for (int i = 0; i < RD_COUNT; i++)
            if (bm & (1ull << i)) {
                if (memcmp(chk, (const u8*)&g_gx + s_region_table[i].off, s_region_table[i].size) != 0) {
                    PROF_COUNT("rec.delta_verify_fail", 1);
                    static u32 reported;
                    if (reported++ < 5) LOG_WARN("rt", "delta verify: region %d differs from the replayed setters", i);
                }
                chk += s_region_table[i].size;
            }
        // The whole shadow must agree: catches changes the compares dropped
        if (p[5] && p[5] != shadow_hash(&g_gx)) {
            PROF_COUNT("rec.delta_verify_hash_fail", 1);
            static u32 reported;
            if (reported++ < 5) LOG_WARN("rt", "delta verify: shadow hash %08lX differs from the replayed state",
                                         (unsigned long)p[5]);
        }
    }
#endif
    for (int i = 0; i < RD_COUNT; i++)
        if (bm & (1ull << i)) {
            memcpy((u8*)&g_gx + s_region_table[i].off, src, s_region_table[i].size);
            src += s_region_table[i].size;
        }
    if (vtx_gen) g_gx.vtx_gen += vtx_gen;
    if (dirty) {
        g_gx.dirty |= dirty;
        g_gx_change_seq++;
    }
    GX_PROF_COUNT("rec.delta_applied", 1);
    GX_PROF_COUNT("rec.delta_bytes", (u32)(src - (const u8*)(p + 6)));
}

// The consumer side: apply one recorded matrix block
static const char* const s_mblk_phases[8] = { "mblk 0", "mblk 1", "mblk 2", "mblk 3",
                                              "mblk 4", "mblk 5", "mblk 6", "mblk 7" };
void gxrec_delta_mblk_apply(const u32* p, u32 words) {
    u32 n = p[0];
    if (n > MBL_MAX) {
        LOG_WARN("rt", "matrix block packet corrupt: %lu entries (%lu words)", (unsigned long)n, (unsigned long)words);
        return;
    }
    const u32* q = p + 1;
    for (u32 i = 0; i < n; i++) {
        g_main_phase = s_mblk_phases[i & 7];
        u32 h = q[0];
        u32 kind = h >> 24, type = (h >> 16) & 0xFF, id = (h >> 8) & 0xFF, nw = h & 0xFF;
        if (nw > 12) nw = 12;
        const float* m = (const float*)(q + 1);
        if (kind == MB_POS)
            __real_GXLoadPosMtxImm(m, id);
        else if (kind == MB_NRM)
            __real_GXLoadNrmMtxImm(m, id);
        else
            __real_GXLoadTexMtxImm(m, id, (GXTexMtxType)type);
        q += 1 + nw;
    }
    GX_PROF_COUNT("rec.mblk_applied", 1);
    GX_PROF_COUNT("rec.mblk_matrices", n);
}

// --- Mode and lifecycle ----------------------------------------------------------

bool gxrec_delta_on(void) { return s_on; }
bool gxrec_delta_verify(void) { return s_verify; }
static bool s_broken;  // Region table incomplete: delta stays off
int gxrec_delta_want(void) { return s_broken ? 0 : cfg_int("S3D_RT_DELTA", 1); }
// S3D_RT_DELTA differs from what the current frame applies: the recorder must
// drain and refresh the shadow before recording continues
bool gxrec_delta_switch_pending(void) {
    int want = gxrec_delta_want();
    bool on = want != 0;
#if GX_PROFILE_DETAIL
    return s_on != on || s_verify != (on && (want & 2) != 0);
#else
    return s_on != on;
#endif
}
int gxrec_dedup_mode(void);  // gx_rec.c

void gxrec_delta_frame_begin(void) {
    int want = gxrec_delta_want();
    s_on = want != 0;
#if GX_PROFILE_DETAIL
    s_verify = s_on && (want & 2) != 0;
#else
    s_verify = false;
#endif
    // The region table must cover every region id with real bytes: a missing
    // entry is a silent zero-sized copy that drops state (RD_TEXn / RD_LIGHTn
    // once shipped as {0,0}). Check once and refuse to run broken.
    static bool checked;
    if (!checked) {
        checked = true;
        bool bad = false;
        for (int i = 0; i < RD_COUNT; i++) {
            if (s_region_table[i].size == 0 || s_region_table[i].off + s_region_table[i].size > sizeof(GxState)) {
                LOG_ERROR("rt", "delta region table incomplete at %d: off %u size %u", i,
                          (unsigned)s_region_table[i].off, (unsigned)s_region_table[i].size);
                bad = true;
            }
        }
        if (bad) {
            LOG_ERROR("rt", "delta mode disabled: fix the region table");
            s_broken = true;
            s_on = s_verify = false;
        }
    }
}

// Copy the render-side state into the shadow (frame start, direct sections)
void gxrec_delta_refresh(void) {
    memcpy(&s_shadow, &g_gx, sizeof(s_shadow));
    memcpy(s_vtx_desc, g_gx.vtx_desc, sizeof(s_vtx_desc));
    for (int a = 0; a < GX_NUM_ARRAYS; a++) {
        s_array[a].data = g_gx.arrays[a].data;
        s_array[a].size = g_gx.arrays[a].size;
    }
    memcpy(s_scissor, g_gx.scissor, sizeof(s_scissor));
    s_regions = 0;
    s_flags = 0;
    s_dirty = 0;
    s_vtx_gen = 0;
    s_mblk_count = 0;
}

// Bookkeeping accessors (gx_rec.c hashing and GXGetScissor answers)
u8 gxrec_delta_vtx_desc(int attr) { return attr >= 0 && attr < GX_NUM_ATTR ? s_vtx_desc[attr] : GX_NONE; }
const void* gxrec_delta_array_data(int attr) { return attr >= 0 && attr < GX_NUM_ARRAYS ? s_array[attr].data : NULL; }
u32 gxrec_delta_array_size(int attr) { return attr >= 0 && attr < GX_NUM_ARRAYS ? s_array[attr].size : 0; }
void gxrec_delta_get_scissor(u32 out[4]) { memcpy(out, s_scissor, sizeof(s_scissor)); }

// --- Shadow apply helpers --------------------------------------------------------

static inline bool d_valid_stage(GXTevStageID s) { return s < GX_MAX_TEVSTAGE; }

// GX_VA_TEX0MTXIDX..GX_VA_TEX7MTXIDX: set -> texgen matrices pick per-vertex
// slots, so a descriptor change there re-runs upload_texmtx (gx_state.c)
static void d_texmtxidx_changed(void) { s_dirty |= GX_DIRTY_TEXMTX; }

// TEV helpers (mirror gx_tev.c set_stage_warp / set_stage_bump)
static void d_set_stage_warp(int stage, float amp) {
    if (stage < 0 || stage >= GX_MAX_TEVSTAGE || s_shadow.tev.stage_warp[stage] == amp) return;
    s_shadow.tev.stage_warp[stage] = amp;
    s_dirty |= GX_DIRTY_TEV;
    mark_region(RD_TEV_WARP);
}

static void d_set_stage_bump(int stage, int ind, int sel) {
    if (stage < 0 || stage >= GX_MAX_TEVSTAGE) return;
    if (s_shadow.tev.stage_ind[stage] == ind && s_shadow.tev.stage_bump_sel[stage] == sel) return;
    s_shadow.tev.stage_ind[stage] = (u8)ind;
    s_shadow.tev.stage_bump_sel[stage] = (u8)sel;
    s_dirty |= GX_DIRTY_TEV;
    mark_region(RD_TEV_IND);
}

static void d_set_stage_ripple(int stage, bool on) {
    if (stage < 0 || stage >= GX_MAX_TEVSTAGE || s_shadow.tev.stage_ripple[stage] == (u8)on) return;
    s_shadow.tev.stage_ripple[stage] = (u8)on;
    mark_region(RD_TEV_RIPPLE);
}

// Profiling counters of the indirect sites, registered on the game thread
#define D_IND_MAX 48
static const char* d_ind_funcs[D_IND_MAX];
static char d_ind_names[D_IND_MAX][48];
static int d_ind_ids[D_IND_MAX];
static int d_ind_count;

static void d_ind_count_site(const char* func) {
    int i = 0;
    while (i < d_ind_count && d_ind_funcs[i] != func) i++;
    if (i == d_ind_count) {
        if (d_ind_count == D_IND_MAX) i = D_IND_MAX - 1;
        else {
            d_ind_funcs[i] = func;
            snprintf(d_ind_names[i], sizeof(d_ind_names[i]), "ind.%s", func);
            d_ind_ids[i] = prof_register_counter(d_ind_names[i]);
            d_ind_count++;
        }
    }
    prof_add(d_ind_ids[i], 1);
}

// --- Vertex format and arrays (always included; no value compare) ------------------

bool gxrec_delta_GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetVtxAttrFmt(vtxfmt, attr, cnt, type, frac));
    d_pre_setter();
    s_flags |= DELTA_FLAG_LAYOUTS;
    if (vtxfmt >= GX_NUM_VTXFMT) return true;
    if (attr == GX_VA_NBT) attr = GX_VA_NRM;
    if (attr >= GX_NUM_ATTR) return true;
    GxAttrFmt* f = &s_shadow.vtx_fmt[vtxfmt][attr];
    f->cnt = cnt;
    f->type = type;
    f->frac = frac;
    mark_region(RD_VTX_FMT);
    return true;
}

bool gxrec_delta_GXSetVtxDesc(GXAttr attr, GXAttrType type) {
    // Bookkeeping for the DL hashing runs in every recorded mode, not just
    // delta mode (the recorder answers from these copies immediately)
    GXAttr a = attr == GX_VA_NBT ? GX_VA_NRM : attr;
    if (a < GX_NUM_ATTR) {
        if (s_on && a >= GX_VA_TEX0MTXIDX && a <= GX_VA_TEX7MTXIDX && s_vtx_desc[a] != type) d_texmtxidx_changed();
        s_vtx_desc[a] = (u8)type;
    }
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetVtxDesc(attr, type));
    d_pre_setter();
    s_flags |= DELTA_FLAG_LAYOUTS;
    if (a < GX_NUM_ATTR) {
        s_shadow.vtx_desc[a] = (u8)type;
        mark_region(RD_VTX_DESC);
    }
    return true;
}

bool gxrec_delta_GXClearVtxDesc(void) {
    if (s_on) {
        for (int a = GX_VA_TEX0MTXIDX; a <= GX_VA_TEX7MTXIDX; a++)
            if (s_vtx_desc[a] != GX_NONE) {
                d_texmtxidx_changed();
                break;
            }
    }
    memset(s_vtx_desc, GX_NONE, sizeof(s_vtx_desc));
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXClearVtxDesc());
    d_pre_setter();
    s_flags |= DELTA_FLAG_LAYOUTS;
    memset(s_shadow.vtx_desc, GX_NONE, sizeof(s_shadow.vtx_desc));
    mark_region(RD_VTX_DESC);
    return true;
}

bool gxrec_delta_GXSetArray(GXAttr attr, const void* data, u32 size, u8 stride, bool le) {
    GXAttr a = attr == GX_VA_NBT ? GX_VA_NRM : attr;
    if (a < GX_NUM_ARRAYS) {
        s_array[a].data = data;
        s_array[a].size = size;
    }
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetArray(attr, data, size, stride, le));
    d_pre_setter();
    if (a < GX_NUM_ARRAYS) {
        s_vtx_gen++;
        s_shadow.vtx_gen++;
        s_shadow.arrays[a].data = data;
        s_shadow.arrays[a].size = size;
        s_shadow.arrays[a].stride = stride;
        s_shadow.arrays[a].le = le;
        mark_region(RD_ARRAY0 + a / 8);
    }
    return true;
}

bool gxrec_delta_GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    u32 sc[4] = { left, top, wd, ht };
    memcpy(s_scissor, sc, sizeof(sc));
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetScissor(left, top, wd, ht));
    d_pre_setter();
    if (memcmp(sc, s_shadow.scissor, sizeof(sc)) == 0) return true;
    s_dirty |= GX_DIRTY_PROJ;
    memcpy(s_shadow.scissor, sc, sizeof(sc));
    mark_region(RD_SCISSOR);
    return true;
}

// --- Matrices (batched into one command) -------------------------------------------

bool gxrec_delta_GXLoadPosMtxImm(const void* mtx, u32 id) {
    if (!s_on) return false;
    d_pre_matrix();
    mblk_append(MB_POS, id, 0, mtx, 12);
    GX_PROF_COUNT("rec.mblk_pos", 1);
    return true;
}

bool gxrec_delta_GXLoadNrmMtxImm(const void* mtx, u32 id) {
    if (!s_on) {
        // S3D_RT_DEDUP bit 4 drops these entirely (nothing reads them)
        return (gxrec_dedup_mode() & 4) != 0;
    }
    if (gxrec_dedup_mode() & 4) return true;
    d_pre_matrix();
    mblk_append(MB_NRM, id, 0, mtx, 12);
    return true;
}

bool gxrec_delta_GXLoadTexMtxImm(const void* mtx, u32 id, GXTexMtxType type) {
    if (!s_on) return false;
    d_pre_matrix();
    u32 nw = (id >= GX_PTTEXMTX0 || type != GX_MTX2x4) ? 12 : 8;
    mblk_append(MB_TEX, id, (u32)type, mtx, nw);
    return true;
}

// --- Transform ----------------------------------------------------------------

bool gxrec_delta_GXSetProjection(const void* mtx, GXProjectionType type) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetProjection(mtx, type));
    d_pre_setter();
    const float (*m)[4] = mtx;
    float p[6];
    p[0] = m[0][0];
    p[2] = m[1][1];
    p[4] = m[2][2];
    p[5] = m[2][3];
    if (type == GX_PERSPECTIVE) {
        p[1] = m[0][2];
        p[3] = m[1][2];
    } else {
        p[1] = m[0][3];
        p[3] = m[1][3];
    }
    if (s_shadow.proj_type == type && memcmp(s_shadow.proj, p, sizeof(p)) == 0) return true;
    s_dirty |= GX_DIRTY_PROJ;
    s_shadow.proj_type = (u8)type;
    memcpy(s_shadow.proj, p, sizeof(p));
    mark_region(RD_PROJ);
    mark_region(RD_PROJ_TYPE);
    return true;
}

static void d_viewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    float vp[6] = { left, top, wd, ht, nearz, farz };
    if (memcmp(vp, s_shadow.viewport, sizeof(vp)) == 0) return;
    s_dirty |= GX_DIRTY_PROJ;
    memcpy(s_shadow.viewport, vp, sizeof(vp));
    mark_region(RD_VIEWPORT);
}

bool gxrec_delta_GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetViewport(left, top, wd, ht, nearz, farz));
    d_pre_setter();
    d_viewport(left, top, wd, ht, nearz, farz);
    return true;
}

bool gxrec_delta_GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetViewportJitter(left, top, wd, ht, nearz, farz, field));
    d_pre_setter();
    (void)field;
    d_viewport(left, top, wd, ht, nearz, farz);
    return true;
}

// --- Texture coordinate generation ----------------------------------------------

bool gxrec_delta_GXSetNumTexGens(u8 nTexGens) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetNumTexGens(nTexGens));
    d_pre_setter();
    if (nTexGens == s_shadow.num_texgens) return true;
    s_dirty |= GX_DIRTY_TEXMTX | GX_DIRTY_TEV;
    s_shadow.num_texgens = nTexGens;
    mark_region(RD_NUM_TEXGENS);
    return true;
}

// --- Lighting channels ----------------------------------------------------------

bool gxrec_delta_GXSetNumChans(u8 nChans) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetNumChans(nChans));
    d_pre_setter();
    if (nChans == s_shadow.num_chans) return true;
    s_dirty |= GX_DIRTY_LIGHT;
    s_shadow.num_chans = nChans;
    mark_region(RD_NUM_CHANS);
    return true;
}

static void d_set_channel(int i, const GxChannel* c) {
    if (memcmp(&s_shadow.chan[i], c, sizeof(*c)) == 0) return;
    s_dirty |= GX_DIRTY_LIGHT;
    s_shadow.chan[i] = *c;
    mark_region(RD_CHAN);
}

bool gxrec_delta_GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask,
                               GXDiffuseFn diff_fn, GXAttnFn attn_fn) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetChanCtrl(chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn));
    d_pre_setter();
    GxChannel c = { enable != 0, (u8)amb_src, (u8)mat_src, (u8)light_mask, (u8)diff_fn, (u8)attn_fn };
    switch (chan) {
        case GX_COLOR0:   d_set_channel(0, &c); break;
        case GX_COLOR1:   d_set_channel(1, &c); break;
        case GX_ALPHA0:   d_set_channel(2, &c); break;
        case GX_ALPHA1:   d_set_channel(3, &c); break;
        case GX_COLOR0A0: d_set_channel(0, &c); d_set_channel(2, &c); break;
        case GX_COLOR1A1: d_set_channel(1, &c); d_set_channel(3, &c); break;
        default: break;
    }
    return true;
}

static int d_chan_color_index(GXChannelID chan) {
    switch (chan) {
        case GX_COLOR0: case GX_ALPHA0: case GX_COLOR0A0: return 0;
        case GX_COLOR1: case GX_ALPHA1: case GX_COLOR1A1: return 1;
        default: return -1;
    }
}

static void d_set_chan_color(GXColor* reg, int region, GXChannelID chan, GXColor c) {
    GXColor n = *reg;
    if (chan == GX_ALPHA0 || chan == GX_ALPHA1) {
        n.a = c.a;
    } else if (chan == GX_COLOR0 || chan == GX_COLOR1) {
        n.r = c.r; n.g = c.g; n.b = c.b;
    } else {
        n = c;
    }
    if (memcmp(reg, &n, sizeof(n)) == 0) return;
    s_dirty |= GX_DIRTY_LIGHT;
    *reg = n;
    mark_region(region);
}

bool gxrec_delta_GXSetChanAmbColor(GXChannelID chan, GXColor amb_color) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetChanAmbColor(chan, amb_color));
    d_pre_setter();
    int i = d_chan_color_index(chan);
    if (i >= 0) d_set_chan_color(&s_shadow.chan_amb[i], RD_CHAN_AMB, chan, amb_color);
    return true;
}

bool gxrec_delta_GXSetChanMatColor(GXChannelID chan, GXColor mat_color) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetChanMatColor(chan, mat_color));
    d_pre_setter();
    int i = d_chan_color_index(chan);
    if (i >= 0) d_set_chan_color(&s_shadow.chan_mat[i], RD_CHAN_MAT, chan, mat_color);
    return true;
}

bool gxrec_delta_GXLoadLightObjImm(GXLightObj* lt_obj, GXLightID light) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXLoadLightObjImm(lt_obj, light));
    d_pre_setter();
    const GxLight* l = (const GxLight*)lt_obj;
    for (int i = 0; i < GX_NUM_LIGHTS; i++) {
        if (light & (1 << i)) {
            if (memcmp(&s_shadow.lights[i], l, sizeof(GxLight)) == 0) return true;
            s_dirty |= GX_DIRTY_LIGHT;
            s_shadow.lights[i] = *l;
            mark_region(RD_LIGHT0 + i);
            return true;
        }
    }
    return true;
}

// --- Pixel pipeline -------------------------------------------------------------

bool gxrec_delta_GXSetBlendMode(GXBlendMode type, GXBlendFactor src_factor, GXBlendFactor dst_factor, GXLogicOp op) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetBlendMode(type, src_factor, dst_factor, op));
    d_pre_setter();
    GxPixelState* px = &s_shadow.pixel;
    if (px->blend_mode == type && px->blend_src == src_factor && px->blend_dst == dst_factor && px->logic_op == op)
        return true;
    s_dirty |= GX_DIRTY_PIXEL;
    px->blend_mode = type;
    px->blend_src = src_factor;
    px->blend_dst = dst_factor;
    px->logic_op = op;
    mark_region(RD_PIXEL);
    return true;
}

bool gxrec_delta_GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetZMode(compare_enable, func, update_enable));
    d_pre_setter();
    GxPixelState* px = &s_shadow.pixel;
    if (px->z_enable == (compare_enable != 0) && px->z_func == func && px->z_update == (update_enable != 0))
        return true;
    s_dirty |= GX_DIRTY_PIXEL;
    px->z_enable = compare_enable != 0;
    px->z_func = func;
    px->z_update = update_enable != 0;
    mark_region(RD_PIXEL);
    return true;
}

bool gxrec_delta_GXSetColorUpdate(GXBool update_enable) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetColorUpdate(update_enable));
    d_pre_setter();
    if (s_shadow.pixel.color_update == (update_enable != 0)) return true;
    s_dirty |= GX_DIRTY_PIXEL;
    s_shadow.pixel.color_update = update_enable != 0;
    mark_region(RD_PIXEL);
    return true;
}

bool gxrec_delta_GXSetAlphaUpdate(GXBool update_enable) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetAlphaUpdate(update_enable));
    d_pre_setter();
    if (s_shadow.pixel.alpha_update == (update_enable != 0)) return true;
    s_dirty |= GX_DIRTY_PIXEL;
    s_shadow.pixel.alpha_update = update_enable != 0;
    mark_region(RD_PIXEL);
    return true;
}

bool gxrec_delta_GXSetCullMode(GXCullMode mode) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetCullMode(mode));
    d_pre_setter();
    if (s_shadow.pixel.cull_mode == mode) return true;
    s_dirty |= GX_DIRTY_PIXEL;
    s_shadow.pixel.cull_mode = mode;
    mark_region(RD_PIXEL);
    return true;
}

bool gxrec_delta_GXSetAlphaCompare(GXCompare comp0, u8 ref0, GXAlphaOp op, GXCompare comp1, u8 ref1) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetAlphaCompare(comp0, ref0, op, comp1, ref1));
    d_pre_setter();
    GxPixelState* px = &s_shadow.pixel;
    if (px->alpha_comp0 == comp0 && px->alpha_ref0 == ref0 && px->alpha_op == op && px->alpha_comp1 == comp1 &&
        px->alpha_ref1 == ref1)
        return true;
    s_dirty |= GX_DIRTY_PIXEL;
    px->alpha_comp0 = comp0;
    px->alpha_ref0 = ref0;
    px->alpha_op = op;
    px->alpha_comp1 = comp1;
    px->alpha_ref1 = ref1;
    mark_region(RD_PIXEL);
    return true;
}

bool gxrec_delta_GXSetFog(GXFogType type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetFog(type, startz, endz, nearz, farz, color));
    d_pre_setter();
    GxPixelState* px = &s_shadow.pixel;
    if (px->fog_type == type && px->fog_start == startz && px->fog_end == endz &&
        px->fog_near == nearz && px->fog_far == farz &&
        memcmp(&px->fog_color, &color, sizeof(color)) == 0)
        return true;
    s_dirty |= GX_DIRTY_PIXEL;
    px->fog_type = (u8)type;
    px->fog_start = startz;
    px->fog_end = endz;
    px->fog_near = nearz;
    px->fog_far = farz;
    px->fog_color = color;
    mark_region(RD_PIXEL);
    return true;
}

// --- Copies, lines, points (plain writes, no dirty bits) -------------------------

bool gxrec_delta_GXSetCopyClear(GXColor clear_clr, u32 clear_z) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetCopyClear(clear_clr, clear_z));
    d_pre_setter();
    s_shadow.clear_color = clear_clr;
    s_shadow.clear_z = clear_z;
    mark_region(RD_CLEAR);
    return true;
}

bool gxrec_delta_GXEnableTexOffsets(GXTexCoordID coord, GXBool line_enable, GXBool point_enable) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXEnableTexOffsets(coord, line_enable, point_enable));
    d_pre_setter();
    if (coord >= GX_NUM_TEXCOORD) return true;
    u16 bits = (u16)((1u << coord) | (1u << (coord + 8)));
    u16 set = (u16)((line_enable ? 1u << coord : 0) | (point_enable ? 1u << (coord + 8) : 0));
    s_shadow.tex_offset_enable = (u16)((s_shadow.tex_offset_enable & ~bits) | set);
    mark_region(RD_TEX_OFFSET_EN);
    return true;
}

bool gxrec_delta_GXSetLineWidth(u8 width, GXTexOffset texOffsets) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetLineWidth(width, texOffsets));
    d_pre_setter();
    s_shadow.line_width = width;
    s_shadow.line_tex_offset = (u8)texOffsets;
    mark_region(RD_LINE_POINT);
    return true;
}

bool gxrec_delta_GXSetPointSize(u8 pointSize, GXTexOffset texOffsets) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetPointSize(pointSize, texOffsets));
    d_pre_setter();
    s_shadow.point_size = pointSize;
    s_shadow.point_tex_offset = (u8)texOffsets;
    mark_region(RD_LINE_POINT);
    return true;
}

// --- TEV -------------------------------------------------------------------------

bool gxrec_delta_GXSetNumTevStages(u8 nStages) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetNumTevStages(nStages));
    d_pre_setter();
    if (nStages == s_shadow.tev.num_stages) return true;
    s_dirty |= GX_DIRTY_TEV;
    s_shadow.tev.num_stages = nStages;
    mark_region(RD_TEV_NUM);
    return true;
}

bool gxrec_delta_GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevOrder(stage, coord, map, color));
    d_pre_setter();
    if (!d_valid_stage(stage)) return true;
    GxTevStage* st = &s_shadow.tev.stages[stage];
    u8 m = (map & GX_TEX_DISABLE) ? GX_TEXMAP_NULL : (u8)map;
    if (st->texcoord == coord && st->texmap == m && st->channel == color) return true;
    s_dirty |= GX_DIRTY_TEV;
    st->texcoord = coord;
    st->texmap = m;
    st->channel = color;
    mark_region(RD_TEV_STAGES);
    return true;
}

static void d_tev_color_in(int stage, const u8 in[4]) {
    if (!d_valid_stage((GXTevStageID)stage) || memcmp(s_shadow.tev.stages[stage].color_in, in, 4) == 0) return;
    s_dirty |= GX_DIRTY_TEV;
    memcpy(s_shadow.tev.stages[stage].color_in, in, 4);
    mark_region(RD_TEV_STAGES);
}

static void d_tev_alpha_in(int stage, const u8 in[4]) {
    if (!d_valid_stage((GXTevStageID)stage) || memcmp(s_shadow.tev.stages[stage].alpha_in, in, 4) == 0) return;
    s_dirty |= GX_DIRTY_TEV;
    memcpy(s_shadow.tev.stages[stage].alpha_in, in, 4);
    mark_region(RD_TEV_STAGES);
}

bool gxrec_delta_GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c,
                                 GXTevColorArg d) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevColorIn(stage, a, b, c, d));
    d_pre_setter();
    u8 in[4] = { a, b, c, d };
    d_tev_color_in(stage, in);
    return true;
}

bool gxrec_delta_GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c,
                                 GXTevAlphaArg d) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevAlphaIn(stage, a, b, c, d));
    d_pre_setter();
    u8 in[4] = { a, b, c, d };
    d_tev_alpha_in(stage, in);
    return true;
}

bool gxrec_delta_GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                 GXTevRegID out_reg) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevColorOp(stage, op, bias, scale, clamp, out_reg));
    d_pre_setter();
    (void)clamp;
    if (!d_valid_stage(stage)) return true;
    GxTevStage* st = &s_shadow.tev.stages[stage];
    if (st->color_op == op && st->color_bias == bias && st->color_scale == scale && st->color_reg == out_reg)
        return true;
    s_dirty |= GX_DIRTY_TEV;
    st->color_op = op;
    st->color_bias = bias;
    st->color_scale = scale;
    st->color_reg = out_reg < GX_MAX_TEVREG ? out_reg : GX_TEVPREV;
    mark_region(RD_TEV_STAGES);
    return true;
}

bool gxrec_delta_GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                                 GXTevRegID out_reg) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevAlphaOp(stage, op, bias, scale, clamp, out_reg));
    d_pre_setter();
    (void)clamp;
    if (!d_valid_stage(stage)) return true;
    GxTevStage* st = &s_shadow.tev.stages[stage];
    if (st->alpha_op == op && st->alpha_bias == bias && st->alpha_scale == scale && st->alpha_reg == out_reg)
        return true;
    s_dirty |= GX_DIRTY_TEV;
    st->alpha_op = op;
    st->alpha_bias = bias;
    st->alpha_scale = scale;
    st->alpha_reg = out_reg < GX_MAX_TEVREG ? out_reg : GX_TEVPREV;
    mark_region(RD_TEV_STAGES);
    return true;
}

static void d_tev_color_reg(GXTevRegID id, GXColor color) {
    if (id >= GX_MAX_TEVREG || memcmp(&s_shadow.tev.regs[id], &color, sizeof(color)) == 0) return;
    s_dirty |= GX_DIRTY_KCOL;
    s_shadow.tev.regs[id] = color;
    mark_region(RD_TEV_REGS);
}

bool gxrec_delta_GXSetTevColor(GXTevRegID id, GXColor color) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevColor(id, color));
    d_pre_setter();
    d_tev_color_reg(id, color);
    return true;
}

bool gxrec_delta_GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevColorS10(id, color));
    d_pre_setter();
    #define D_CLAMP8(v) ((u8)((v) < 0 ? 0 : (v) > 255 ? 255 : (v)))
    GXColor c = { D_CLAMP8(color.r), D_CLAMP8(color.g), D_CLAMP8(color.b), D_CLAMP8(color.a) };
    #undef D_CLAMP8
    d_tev_color_reg(id, c);
    return true;
}

bool gxrec_delta_GXSetTevKColor(GXTevKColorID id, GXColor color) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevKColor(id, color));
    d_pre_setter();
    if (id >= GX_MAX_KCOLOR || memcmp(&s_shadow.tev.kcolors[id], &color, sizeof(color)) == 0) return true;
    s_dirty |= GX_DIRTY_KCOL;
    s_shadow.tev.kcolors[id] = color;
    mark_region(RD_TEV_KCOLORS);
    return true;
}

bool gxrec_delta_GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevKColorSel(stage, sel));
    d_pre_setter();
    if (!d_valid_stage(stage) || s_shadow.tev.stages[stage].kcsel == sel) return true;
    s_dirty |= GX_DIRTY_TEV;
    s_shadow.tev.stages[stage].kcsel = sel;
    mark_region(RD_TEV_STAGES);
    return true;
}

bool gxrec_delta_GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevKAlphaSel(stage, sel));
    d_pre_setter();
    if (!d_valid_stage(stage) || s_shadow.tev.stages[stage].kasel == sel) return true;
    s_dirty |= GX_DIRTY_TEV;
    s_shadow.tev.stages[stage].kasel = sel;
    mark_region(RD_TEV_STAGES);
    return true;
}

bool gxrec_delta_GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevOp(id, mode));
    d_pre_setter();
    if (!d_valid_stage(id)) return true;
    GXTevColorArg carg = id == GX_TEVSTAGE0 ? GX_CC_RASC : GX_CC_CPREV;
    GXTevAlphaArg aarg = id == GX_TEVSTAGE0 ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
        case GX_MODULATE:
            d_tev_color_in(id, (u8[4]){ GX_CC_ZERO, GX_CC_TEXC, carg, GX_CC_ZERO });
            d_tev_alpha_in(id, (u8[4]){ GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO });
            break;
        case GX_DECAL:
            d_tev_color_in(id, (u8[4]){ carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO });
            d_tev_alpha_in(id, (u8[4]){ GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg });
            break;
        case GX_BLEND:
            d_tev_color_in(id, (u8[4]){ carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO });
            d_tev_alpha_in(id, (u8[4]){ GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO });
            break;
        case GX_REPLACE:
            d_tev_color_in(id, (u8[4]){ GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC });
            d_tev_alpha_in(id, (u8[4]){ GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA });
            break;
        default:  // GX_PASSCLR
            d_tev_color_in(id, (u8[4]){ GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, carg });
            d_tev_alpha_in(id, (u8[4]){ GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg });
            break;
    }
    GxTevStage* st = &s_shadow.tev.stages[id];
    if (st->color_op != GX_TEV_ADD || st->color_bias != GX_TB_ZERO || st->color_scale != GX_CS_SCALE_1 ||
        st->color_reg != GX_TEVPREV) {
        s_dirty |= GX_DIRTY_TEV;
        st->color_op = GX_TEV_ADD;
        st->color_bias = GX_TB_ZERO;
        st->color_scale = GX_CS_SCALE_1;
        st->color_reg = GX_TEVPREV;
        mark_region(RD_TEV_STAGES);
    }
    if (st->alpha_op != GX_TEV_ADD || st->alpha_bias != GX_TB_ZERO || st->alpha_scale != GX_CS_SCALE_1 ||
        st->alpha_reg != GX_TEVPREV) {
        s_dirty |= GX_DIRTY_TEV;
        st->alpha_op = GX_TEV_ADD;
        st->alpha_bias = GX_TB_ZERO;
        st->alpha_scale = GX_CS_SCALE_1;
        st->alpha_reg = GX_TEVPREV;
        mark_region(RD_TEV_STAGES);
    }
    return true;
}

bool gxrec_delta_GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevSwapMode(stage, ras_sel, tex_sel));
    d_pre_setter();
    if (!d_valid_stage(stage) || ras_sel >= GX_MAX_TEVSWAP || tex_sel >= GX_MAX_TEVSWAP) return true;
    GxTevStage* st = &s_shadow.tev.stages[stage];
    if (st->ras_swap == ras_sel && st->tex_swap == tex_sel) return true;
    s_dirty |= GX_DIRTY_TEV;
    st->ras_swap = (u8)ras_sel;
    st->tex_swap = (u8)tex_sel;
    mark_region(RD_TEV_STAGES);
    return true;
}

bool gxrec_delta_GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green,
                                       GXTevColorChan blue, GXTevColorChan alpha) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevSwapModeTable(table, red, green, blue, alpha));
    d_pre_setter();
    if (table >= GX_MAX_TEVSWAP) return true;
    u8 t[4] = { (u8)red, (u8)green, (u8)blue, (u8)alpha };
    if (memcmp(s_shadow.tev.swap_tables[table], t, 4) == 0) return true;
    s_dirty |= GX_DIRTY_TEV;
    memcpy(s_shadow.tev.swap_tables[table], t, 4);
    mark_region(RD_TEV_SWAP);
    return true;
}

// --- Indirect stages ------------------------------------------------------------

bool gxrec_delta_GXSetIndTexOrder(GXIndTexStageID ind_stage, GXTexCoordID tex_coord, GXTexMapID tex_map) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetIndTexOrder(ind_stage, tex_coord, tex_map));
    d_pre_setter();
    if (ind_stage < 0 || ind_stage >= GX_MAX_INDTEXSTAGE) return true;
    if (s_shadow.tev.ind_coord[ind_stage] == (u8)tex_coord && s_shadow.tev.ind_map[ind_stage] == (u8)tex_map)
        return true;
    s_shadow.tev.ind_coord[ind_stage] = (u8)tex_coord;
    s_shadow.tev.ind_map[ind_stage] = (u8)tex_map;
    s_dirty |= GX_DIRTY_TEV;
    mark_region(RD_TEV_IND_MAP);
    return true;
}

bool gxrec_delta_GXSetNumIndStages(u8 nIndStages) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetNumIndStages(nIndStages));
    d_pre_setter();
    if (nIndStages) LOG_ONCE(LOG_LEVEL_WARN, "gx", "indirect texturing not supported");
    if (s_shadow.tev.num_ind_stages != nIndStages) {
        s_shadow.tev.num_ind_stages = nIndStages;
        mark_region(RD_TEV_NUM_IND);
    }
    if (!nIndStages) {
        for (int s = 0; s < GX_MAX_TEVSTAGE; s++) d_set_stage_warp(s, 0.0f);
        bool ripple = false;
        for (int s = 0; s < GX_MAX_TEVSTAGE; s++) ripple |= s_shadow.tev.stage_ripple[s] != 0;
        if (ripple) {
            memset(s_shadow.tev.stage_ripple, 0, sizeof(s_shadow.tev.stage_ripple));
            mark_region(RD_TEV_RIPPLE);
        }
    }
    return true;
}

bool gxrec_delta_GXSetTevDirect(GXTevStageID tev_stage) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevDirect(tev_stage));
    d_pre_setter();
    d_set_stage_warp(tev_stage, 0.0f);
    d_set_stage_ripple(tev_stage, false);
    d_set_stage_bump(tev_stage, 0, GX_ITBA_OFF);
    return true;
}

static void d_indirect(const char* func, GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                       GXIndTexAlphaSel alpha_sel) {
    d_pre_setter();
    if (!func) func = "other";
    d_ind_count_site(func);
    float amp = gx_tev_warp_for_site(func);
    d_set_stage_warp(tev_stage, amp);
    d_set_stage_ripple(tev_stage, !strcmp(func, "setupWaterCausticTev"));
    d_set_stage_bump(tev_stage, ind_stage, alpha_sel);
}

bool gxrec_delta_port_tev_indirect(const char* func, GXTevStageID tev_stage, GXIndTexStageID ind_stage,
                                   GXIndTexFormat format, GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel,
                                   GXIndTexWrap wrap_s, GXIndTexWrap wrap_t, GXBool add_prev, GXBool ind_lod,
                                   GXIndTexAlphaSel alpha_sel) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_port_tev_indirect(func, tev_stage, ind_stage, format, bias_sel, matrix_sel, wrap_s, wrap_t,
                                         add_prev, ind_lod, alpha_sel));
    (void)format; (void)bias_sel; (void)matrix_sel;
    (void)wrap_s; (void)wrap_t; (void)add_prev; (void)ind_lod;
    d_indirect(func, tev_stage, ind_stage, alpha_sel);
    return true;
}

bool gxrec_delta_GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,
                                  GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s,
                                  GXIndTexWrap wrap_t, GXBool add_prev, GXBool ind_lod, GXIndTexAlphaSel alpha_sel) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTevIndirect(tev_stage, ind_stage, format, bias_sel, matrix_sel, wrap_s, wrap_t, add_prev,
                                        ind_lod, alpha_sel));
    (void)format; (void)bias_sel; (void)matrix_sel;
    (void)wrap_s; (void)wrap_t; (void)add_prev; (void)ind_lod;
    d_indirect(NULL, tev_stage, ind_stage, alpha_sel);
    return true;
}

// --- Textures --------------------------------------------------------------------

bool gxrec_delta_GXLoadTexObj(GXTexObj* obj, GXTexMapID id) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXLoadTexObj(obj, id));
    d_pre_setter();
    if (id >= GX_NUM_TEXMAP) return true;
    const GxTexObjData* t = (const GxTexObjData*)obj;
    if (memcmp(&s_shadow.tex[id], t, sizeof(*t)) == 0) return true;
    s_dirty |= GX_DIRTY_TEX;
    s_shadow.tex[id] = *t;
    mark_region(RD_TEX0 + id);
    return true;
}

bool gxrec_delta_GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTexCopySrc(left, top, wd, ht));
    d_pre_setter();
    s_shadow.texcopy_src[0] = left;
    s_shadow.texcopy_src[1] = top;
    s_shadow.texcopy_src[2] = wd;
    s_shadow.texcopy_src[3] = ht;
    mark_region(RD_TEXCOPY);
    return true;
}

bool gxrec_delta_GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap) {
    if (!s_on) return false;
    D_VERIFY(gxrec_put_GXSetTexCopyDst(wd, ht, fmt, mipmap));
    d_pre_setter();
    (void)wd; (void)ht; (void)mipmap;
    s_shadow.texcopy_dst_fmt = (u8)fmt;
    mark_region(RD_TEXCOPY);
    return true;
}
