// Map sort (DRAWCALL_PLAN.md "Map sort"). renderSceneGeometry's opaque pass
// draws the map block by block, and each block material by material, so
// consecutive map draws almost never share a material and the render thread
// sets up its state again for nearly every one. With S3D_MAP_SORT the pass
// collects the visible blocks and draws them grouped by material
// (patches/main/tex_dolphin_mapsort.c.inc, appended to tex_dolphin.c). Bit
// mask, any non-zero value sorts:
//   1  grouped by material; every piece still runs its full material setup
//      (the setters skip unchanged values, so the render thread only sees
//      the changes). Each block keeps its own piece order
//   2  a piece skips its material setup when the piece before it used a
//      shader with the same content and added nothing to it
//   4  plain opaque pieces of a block may swap among themselves (alpha-tested,
//      blended and other order-sensitive pieces keep their place in the block)
// Counters (per frame): map_sort.units / map_sort.blocks (collected),
// map_sort.keys_distinct (shader contents), map_sort.units_nokey and
// map_sort.units_ordered, map_sort.groups (material groups drawn; the best
// case is keys_distinct), map_sort.block_setups, map_sort.shader_skipped,
// map_sort.vtx_replayed, map_sort.overflow (blocks drawn the old way because
// the tables were full), map_sort.stuck (units the merge left; should be 0).

#include "config.h"
#include "log.h"
#include "profiler.h"

int port_map_sort(void) {
    static int mode = -1;
    CFG_LIVE(mode, cfg_int("S3D_MAP_SORT", 0));
    return mode;
}

void port_map_sort_note(int what, int n) {
    switch (what) {
    case 0: PROF_COUNT("map_sort.units", n); break;
    case 1: PROF_COUNT("map_sort.groups", n); break;
    case 2: PROF_COUNT("map_sort.block_setups", n); break;
    case 3: PROF_COUNT("map_sort.shader_skipped", n); break;
    case 4: PROF_COUNT("map_sort.vtx_replayed", n); break;
    case 5: PROF_COUNT("map_sort.overflow", n); break;
    case 6: PROF_COUNT("map_sort.blocks", n); break;
    case 7: PROF_COUNT("map_sort.units_nokey", n); break;
    case 8: PROF_COUNT("map_sort.keys_distinct", n); break;
    case 9: PROF_COUNT("map_sort.units_ordered", n); break;
    default: PROF_COUNT("map_sort.stuck", n); break;
    }
}

// Map material dedup (DRAWCALL_PLAN.md "Map material dedup",
// patches/main/tex_dolphin_mapdedup.c.inc). Keeps the game's draw order;
// mapBlockRender_setShader skips the setters whose state the last map shader
// of the pass already set. Default 3 (HW 3AF2B4BF: frame -0.45 ms, render
// thread -0.4 ms, verify run clean; DRAWCALL_PLAN.md). Bit mask, read live:
//   1  fog, blend / z / alpha compare, channel and cull, each on its own
//   2  the whole TEV / texture setup when the shader has the same content as
//      the last one set up and no point-light stages were added since
//   4  verify (needs S3D_RENDER_THREAD=0): nothing is skipped; a group bit 1
//      would have skipped is run anyway, and if it changed GX state anyway
//      (g_gx_change_seq) that is logged and counted as map_dd.mismatch
// Counters (per frame): map_dd.calls (TEV setups reached), map_dd.tev (of
// them skipped), map_dd.fog / map_dd.pe / map_dd.chan / map_dd.cull (setter
// groups skipped).
int port_map_dedup(void) {
    static int mode = -1;
    CFG_LIVE(mode, cfg_int("S3D_MAP_DEDUP", 3));
    return mode;
}

void port_map_dedup_note(int what, int n) {
    switch (what) {
    case 0: PROF_COUNT("map_dd.fog", n); break;
    case 1: PROF_COUNT("map_dd.pe", n); break;
    case 2: PROF_COUNT("map_dd.chan", n); break;
    case 3: PROF_COUNT("map_dd.cull", n); break;
    case 4: PROF_COUNT("map_dd.tev", n); break;
    case 6: PROF_COUNT("map_dd.mismatch", n); break;
    default: PROF_COUNT("map_dd.calls", n); break;
    }
}

// Verify mode: a group the cache called unchanged did change GX state
void port_map_dedup_mismatch(int what, unsigned flags, int cached, int now) {
    static const char* const k_names[] = { "fog", "blend/z/alpha", "channel", "cull" };
    static int logged;
    port_map_dedup_note(6, 1);
    if (logged >= 40) return;
    logged++;
    LOG_WARN("mapdd", "mismatch: %s group changed state although the cache had it (shader flags %08X, value %d)",
             what >= 0 && what < 4 ? k_names[what] : "?", flags, cached);
    (void)now;
}
