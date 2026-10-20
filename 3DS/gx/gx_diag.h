#pragma once
// Cache and renderer statistics for the periodic diagnostics line
// (port/diag_3ds.c). Getters only, cheap enough to call any time.
#include <3ds/types.h>
#include <stdbool.h>

typedef struct {
    u32 slots, used, valid, dynamic, tombs;
    u32 kb;
    u32 table_full;   // Lookups that found no slot (list drawn uncached), total
    u32 pool_resets;  // Dynamic vertex pool resets, total
    u32 rehashes;     // Table rebuilds that dropped tombstones/stale entries
    u32 dropped;      // Entries dropped by those rebuilds, total
    u32 evicts;       // Live entries evicted for a new list when the table was full, total
    u32 cold;         // Entries freed after DL_COLD_FRAMES unused, total
    u32 last_pool_reset;  // Frame of the latest dynamic pool reset
    u32 heap_kb;      // C heap held by the entries' side tables
    u32 heap_evicts;  // Entries freed for the side-table budget or the heap floor, total
} GxDlStats;

typedef struct {
    u32 slots, entries, tombs;
    u32 kb;
    u32 trimmed;   // Tombstones cleared, total
} GxTexStats;

// Dropped draws, skipped copies and buffer overflows are LOG_ONCE sites, which
// count their hits (log_once_report)
typedef struct {
    u8 offscreen_ok;      // Bit i: the (32 << i) square scratch target exists
    u8 offscreen_failed;  // Bit i: creating it failed (no VRAM)
    bool lowres;          // Low-resolution game target exists
    bool shadows;         // port_shadows_enabled()
} GxRenderStats;

// Frame vertex buffers and copies (totals since boot; peaks are per frame)
typedef struct {
    u32 drops;            // Draws dropped because the frame vertex/index buffer was full
    u32 drop_verts;       // Their vertices
    u32 drop_frames;      // Frames with at least one drop
    u32 last_drop_frame;
    u32 vtx_peak, vtx_max, idx_peak, idx_max;
    u32 dyn_peak_kb, dyn_max_kb;
    u32 dyn_fails;        // gx_render_alloc_dynamic calls that found the buffer full
    u32 copies;           // GXCopyTex copies done
    u32 copy_skips;       // GXCopyTex copies skipped (format, budget or memory)
    u32 offscreen_fails;  // gx_offscreen_begin calls that had no target
} GxDrawStats;

void gx_dlcache_get_stats(GxDlStats* out);
void gx_tex_get_stats(GxTexStats* out);
void gx_render_get_stats(GxRenderStats* out);
void gx_render_get_draw_stats(GxDrawStats* out);
// Log a one-frame render census (state counts, depth mappings, the first
// states of the frame) for the next whole frame; tag "census"
void gx_render_census_request(void);

// Counters bumped by gx_copy.c / gx_init.c, read by gx_render_get_draw_stats
extern u32 g_gx_copies, g_gx_copy_skips, g_gx_offscreen_fails;

// --- Vertex/matrix sanity ------------------------------------------------------
// "Vertex explosions" come from huge or NaN positions or matrices. Checks are a
// single compare (NaN fails it too); hits go to gx_diag_bad_value, which logs
// them rate-limited and keeps the last few for the "mark" snapshot.

#define GX_DIAG_POS_LIMIT 1e6f  // Model-space position component
#define GX_DIAG_MTX_LIMIT 1e7f  // Position matrix element
// Position matrices whose view-space translation (|x|+|y|+|z|) passes this are
// replaced with zero (GXLoadPosMtxImm); counted in g_gx_posmtx_culls
#define GX_POSMTX_CULL 1e6f
extern u32 g_gx_posmtx_culls;

static inline bool gx_diag_bad3(const float* p, float limit) {
    return !(__builtin_fabsf(p[0]) + __builtin_fabsf(p[1]) + __builtin_fabsf(p[2]) < limit);
}

void gx_diag_bad_value(const char* what, const float* v, int n, u32 id, const void* caller);
// Logs the remembered bad values (newest last); returns the total count
u32 gx_diag_dump_bad_values(void);
