// Soak diagnostics for long sessions (FPS degrading, shadows disappearing,
// vertex explosions after a while in the hub):
// - every DIAG_PERIOD frames a "diag" line with cache fill (dlcache, texture
//   cache), free memory and which render targets exist, plus the LOG_ONCE
//   sites whose hit counts grew ("once" tag)
// - diag_mark(): a "mark" snapshot to tie a glitch on screen to the log: the
//   same stats, every LOG_ONCE count, player position and the last huge/NaN
//   vertices or matrices. L+R+SELECT, Settings > "Log snapshot (mark)" or the
//   remote "mark" command.
#include "diag_3ds.h"
#include <3ds.h>
#include <stdio.h>
#include <malloc.h>
#include "config.h"
#include "gx_capture.h"
#include "gx_diag.h"
#include "gx_rec.h"
#include "heaptrack_3ds.h"
#include "frametrace_3ds.h"
#include "log.h"
#include "profiler.h"
#include "savestate_3ds.h"
#include "snapmem_3ds.h"

#define DIAG_PERIOD 600  // ~10 s at 60 Hz retrace

static u32 s_frames, s_marks;
static u32 s_vram_min = 0xFFFFFFFF;  // VRAM free low-water mark, sampled every 60 frames

static void sample_vram(void) {
    u32 v = vramSpaceFree();
    if (v < s_vram_min) s_vram_min = v;
}

static void log_stats(const char* tag) {
    GxDlStats dl;
    GxTexStats tex;
    GxRenderStats rs;
    GxDrawStats ds;
    sample_vram();
    gx_dlcache_get_stats(&dl);
    gx_tex_get_stats(&tex);
    gx_render_get_stats(&rs);
    gx_render_get_draw_stats(&ds);

    u32 heap_free = heaptrack_heap_free();

    LOG_INFO(tag, "dl %lu/%lu used (valid %lu dyn %lu tomb %lu) %lu KB full %lu poolreset %lu (last f%lu) rehash %lu/%lu dropped "
             "evict %lu cold %lu | heap %lu KB, heap evict %lu",
             (unsigned long)dl.used, (unsigned long)dl.slots, (unsigned long)dl.valid, (unsigned long)dl.dynamic,
             (unsigned long)dl.tombs, (unsigned long)dl.kb, (unsigned long)dl.table_full,
             (unsigned long)dl.pool_resets, (unsigned long)dl.last_pool_reset, (unsigned long)dl.rehashes,
             (unsigned long)dl.dropped, (unsigned long)dl.evicts, (unsigned long)dl.cold,
             (unsigned long)dl.heap_kb, (unsigned long)dl.heap_evicts);
    LOG_INFO(tag, "tex %lu/%lu (tomb %lu, trimmed %lu) %lu KB | free: linear %lu KB vram %lu KB (min %lu) heap %lu KB",
             (unsigned long)tex.entries, (unsigned long)tex.slots, (unsigned long)tex.tombs,
             (unsigned long)tex.trimmed, (unsigned long)tex.kb, (unsigned long)(linearSpaceFree() >> 10),
             (unsigned long)(vramSpaceFree() >> 10), (unsigned long)(s_vram_min >> 10), (unsigned long)(heap_free >> 10));
    LOG_INFO(tag, "targets: offscreen ok %x failed %x, lowres %d, shadows %d", rs.offscreen_ok, rs.offscreen_failed,
             rs.lowres, rs.shadows);
    heaptrack_report(tag);
    LOG_INFO(tag, "draw: drops %lu (%lu verts, %lu frames, last f%lu) | peak vtx %lu/%lu idx %lu/%lu dyn %lu/%lu KB "
             "(fails %lu) | copies %lu skipped %lu | offscreen fails %lu | posmtx culls %lu",
             (unsigned long)ds.drops, (unsigned long)ds.drop_verts, (unsigned long)ds.drop_frames,
             (unsigned long)ds.last_drop_frame, (unsigned long)ds.vtx_peak, (unsigned long)ds.vtx_max,
             (unsigned long)ds.idx_peak, (unsigned long)ds.idx_max, (unsigned long)ds.dyn_peak_kb,
             (unsigned long)ds.dyn_max_kb, (unsigned long)ds.dyn_fails, (unsigned long)ds.copies,
             (unsigned long)ds.copy_skips, (unsigned long)ds.offscreen_fails, (unsigned long)g_gx_posmtx_culls);
}

void diag_stats(void) {
    log_stats("diag");
    port_musyx_pool_log();
    log_once_report(false);
}

static void texsweep_mark(void);

void diag_mark(void) {
    texsweep_mark();
    s_marks++;
    LOG_WARN("mark", "--- mark %lu ---", (unsigned long)s_marks);
    log_stats("mark");
    if (!savestate_log_where("mark")) LOG_INFO("mark", "not in game");
    u32 bad = gx_diag_dump_bad_values();
    LOG_INFO("mark", "bad vertices/matrices total %lu", (unsigned long)bad);
    log_once_report(true);
    // The census covers the next whole frame and logs itself (tag "census")
    gx_render_census_request();
    gx_capture_request();  // Render capture folder (gx_capture.h)
    LOG_WARN("mark", "--- end mark %lu ---", (unsigned long)s_marks);
    log_flush();
}

// --- GPU cost sweep (S3D_GPU_SWEEP=<map id>) ---------------------------------
// One standing-still run measures what each render feature costs: 10 s after
// that map loads, every SWEEP_FRAMES frames one setting changes from the
// baseline (the run's own values) and is put back, logged as "sweep phase N:
// name" right after a forced profiler report, so report windows line up with
// the phases. Read gpu.p3d_us / ppf_us / psc_us and the frame time per phase.
#define SWEEP_FRAMES 180
#define SWEEP_DELAY 600
static const struct {
    const char* name;
    const char* key[2];
    int value[2];
} k_sweep[] = {
    { "baseline", { NULL }, { 0 } },
    { "no grass layers", { "S3D_GRASS_BUDGET" }, { 0 } },
    { "no shadows", { "S3D_SHADOWS" }, { 0 } },
    { "no fragment lighting", { "S3D_FRAG_LIGHT" }, { 0 } },
    { "no fog", { "S3D_FOG" }, { 0 } },
    { "no water ripples / caustics", { "S3D_WATER_WARP", "S3D_CAUSTIC_WARP" }, { 0, 0 } },
    { "no GPU blur", { "S3D_GPU_BLUR" }, { 0 } },
    { "no heat haze", { "S3D_SCREEN_WARP" }, { 0 } },
    { "texture LOD bias +1", { "S3D_TEX_LODBIAS_X4" }, { 4 } },
    { "baseline again", { NULL }, { 0 } },
};
#define SWEEP_PHASES ((int)(sizeof(k_sweep) / sizeof(k_sweep[0])))
static int s_sweep_map = -2;   // Map that starts it (-1 off, -2 not read yet)
static int s_sweep_phase = -1; // Current phase, -1 not started
static int s_sweep_countdown;  // Frames until the next phase (0 = idle)
static int s_sweep_saved[2];

static void sweep_set(int phase, bool apply) {
    for (int k = 0; k < 2 && k_sweep[phase].key[k]; k++) {
        const char* key = k_sweep[phase].key[k];
        if (apply) {
            s_sweep_saved[k] = cfg_int(key, 0);
            cfg_set(key, k_sweep[phase].value[k]);
        } else {
            cfg_set(key, s_sweep_saved[k]);
        }
    }
}

// Bug 34: the game asked for a NULL texture (shader_dolphin.c selectTexture),
// so the unit keeps the previous material's texture. First 16 calls logged
// with the caller (addr2line on out/star3d.elf), then only counted
void port_null_texture(int mapId, void* caller) {
    static int logged;
    PROF_COUNT("game.null_texture", 1);
    if (logged >= 16) return;
    logged++;
    LOG_WARN("tex", "selectTexture(NULL) for map %d from %p (report %d/16)", mapId, caller, logged);
}

// Bug 34: a texture reference that is a pointer below 0x80000000 (textureIdxToPtr
// took those for slot handles and returned NULL before the texture.c.sed fix).
// Counted every time, the first 16 distinct ones logged
void port_tex_ptr_ref(void* tex) {
    static void* seen[16];
    static int n;
    PROF_COUNT("game.tex_ptr_refs", 1);
    for (int i = 0; i < n; i++)
        if (seen[i] == tex) return;
    if (n >= 16) return;
    seen[n++] = tex;
    LOG_WARN("tex", "texture reference %p is a pointer (was NULL before the bug 34 fix, report %d/16)", tex, n);
}

// --- Texture suspect sweep (S3D_TEX_SWEEP=1, bug 34) ---------------------------
// HW-only wrong/flickering map textures (C12 room) that Azahar doesn't show.
// The first mark starts it (its own capture is the baseline); then every phase
// switches one suspect setting, lets it settle and takes a render capture,
// logged as "texsweep phase N: name". Compare each capture's walls with the
// Azahar reference. The setting goes back to its default before the next phase
// (S3D_RT_STEPS -1 = the built-in steps).
#define TEXSWEEP_SETTLE 90
#define TEXSWEEP_FRAMES 260
static const struct {
    const char *name, *key;
    int value, def;
} k_texsweep[] = {
    // Round 2 (the switch faces sample the previous draw's copy texture): what
    // can change TEXMAP0 between the game's batched objects behind its texture
    // cache (gObjCachedTexture, objprint_dolphin.c)
    { "display-list batching off", "S3D_DL_BATCH", 0, 1 },
    { "shadow refresh every frame", "S3D_SHADOW_REFRESH", 1, 2 },
    { "shadows off", "S3D_SHADOWS", 0, 1 },
    { "animation LOD off", "S3D_ANIM_LOD", 0, 2 },
    { "map material dedup off", "S3D_MAP_DEDUP", 0, 3 },
    { "render thread off (replay on the game thread)", "S3D_RENDER_THREAD", 1, 2 },
    { "render-thread dedup off", "S3D_RT_DEDUP", 0, 5 },
    { "prep fast paths off", "S3D_PREP_FAST", 0, 7 },
    { "render-thread memos off", "S3D_RT_MEMO", 0, 15 },
    { "baseline again", NULL, 0, 0 },
};
#define TEXSWEEP_PHASES ((int)(sizeof(k_texsweep) / sizeof(k_texsweep[0])))
static int s_texsweep = -1;       // -1 not read, 0 off, 1 armed (waits for a mark), 2 running
static int s_texsweep_phase = -1;
static int s_texsweep_count;

static void texsweep_set(int phase, bool apply) {
    const char* key = k_texsweep[phase].key;
    if (!key) return;
    cfg_set(key, apply ? k_texsweep[phase].value : k_texsweep[phase].def);
}

static void texsweep_mark(void) {
    if (s_texsweep < 0) s_texsweep = cfg_bool("S3D_TEX_SWEEP", false) ? 1 : 0;
    if (s_texsweep != 1) return;
    s_texsweep = 2;
    s_texsweep_phase = -1;
    s_texsweep_count = TEXSWEEP_FRAMES;  // The mark's own capture first
    LOG_WARN("texsweep", "texture sweep started: this mark's capture = baseline, then %d phases of %d frames; "
             "stand still", TEXSWEEP_PHASES, TEXSWEEP_FRAMES);
}

static void texsweep_frame(void) {
    if (s_texsweep != 2) return;
    // Captures take 5 frames: never switch while one runs
    if (gx_capture_mode() != GX_CAP_NONE) return;
    --s_texsweep_count;
    if ((s_texsweep_count == TEXSWEEP_FRAMES - TEXSWEEP_SETTLE || s_texsweep_count == TEXSWEEP_FRAMES - TEXSWEEP_SETTLE - 61) &&
        s_texsweep_phase >= 0) {
        // Two captures 61 frames apart (odd: catches every-other-frame flips)
        LOG_WARN("texsweep", "phase %d capture: %s", s_texsweep_phase, k_texsweep[s_texsweep_phase].name);
        savestate_log_where("texsweep");
        gx_capture_request();
        return;
    }
    if (s_texsweep_count > 0) return;
    if (s_texsweep_phase >= 0) texsweep_set(s_texsweep_phase, false);
    if (++s_texsweep_phase >= TEXSWEEP_PHASES) {
        s_texsweep = 0;
        LOG_WARN("texsweep", "texture sweep done");
        return;
    }
    texsweep_set(s_texsweep_phase, true);
    LOG_WARN("texsweep", "texsweep phase %d: %s", s_texsweep_phase, k_texsweep[s_texsweep_phase].name);
    s_texsweep_count = TEXSWEEP_FRAMES;
}

static void ab_map_loaded(int map_id);

void diag_map_loaded(int map_id) {
    ab_map_loaded(map_id);
    if (s_sweep_map == -2) s_sweep_map = cfg_int("S3D_GPU_SWEEP", -1);
    if (s_sweep_map < 0 || map_id != s_sweep_map || s_sweep_phase >= 0 || s_sweep_countdown) return;
    s_sweep_countdown = SWEEP_DELAY;
    LOG_INFO("sweep", "map %d loaded: GPU sweep starts in %d frames, %d phases of %d frames; stand still", map_id,
             SWEEP_DELAY, SWEEP_PHASES, SWEEP_FRAMES);
}

static void sweep_frame(void) {
    if (!s_sweep_countdown || --s_sweep_countdown) return;
    // Settings change between frames with the render thread idle, and the
    // report closes the previous phase's window
    gxrec_report_drain();
    prof_report();
    if (s_sweep_phase >= 0) sweep_set(s_sweep_phase, false);
    s_sweep_phase++;
    if (s_sweep_phase >= SWEEP_PHASES) {
        LOG_INFO("sweep", "GPU sweep done");
        return;
    }
    sweep_set(s_sweep_phase, true);
    LOG_INFO("sweep", "sweep phase %d: %s", s_sweep_phase, k_sweep[s_sweep_phase].name);
    s_sweep_countdown = SWEEP_FRAMES;
}

// --- Setting A/B runs (S3D_VSH_LEAN_AB / S3D_RT_DEDUP_AB / S3D_SHADOWS_AB /
// S3D_FUR_MODE_AB / S3D_SHADOW_TEXTURES_AB / S3D_SHADOW_REFRESH_AB / S3D_CULL_AB /
// S3D_RCP_REFRESH_AB / S3D_RENDER_THREAD_AB / S3D_TOUCHUI_AB / S3D_RT_MEMO_AB / S3D_TEX_VERIFY_AB /
// S3D_OBJ_DIST_AB / S3D_OBJ_MINPX_AB / S3D_PLANTS_AB /
// S3D_C3D_FAST_AB / S3D_ANIM_LOD_AB / S3D_RT_STEPS_AB / S3D_FUR_FAST_AB / S3D_MAP_SORT_AB /
// S3D_JOINT_TEXMTX_AB =
// <frames per phase>)
// Standing still, the run cycles one setting through two to four values every
// N frames and logs each phase's average frame time and GPU time
// (gpu.p3d_us), skipping the first frames after a switch. Same spot, same run,
// so the values compare directly (MULTICORE_PLAN.md, "Leaner vertex shaders",
// "Render thread: redundant work", "Vertex attribute pruning"). The first
// enabled test runs. It starts AB_START_DELAY frames after map S3D_AB_MAP
// loads (default 7, the hub; -1 = at boot), so boot and the title scene stay
// out of it (21DE01AE ran its first two phases there)
#define AB_SETTLE 30
#define AB_START_DELAY 150
extern volatile u32 g_gx_gpu_p3d_last;
extern u32 g_scene_render_ticks, g_scene_own_ticks;  // frame_zones.c
void probe_sample_mark(const char* label);  // probe_3ds.c: sampler windows follow the phases
static const struct {
    const char *env, *var, *tag, *label;
    int n, vals[4];
} k_ab_tests[] = {
    { "S3D_VSH_LEAN_AB", "S3D_VSH_LEAN", "vshab", "lean", 2, { 3, 0 } },
    // 5 / 7: no filter (the default) / record-time filter (bit 2) (gx_rec.c)
    { "S3D_RT_DEDUP_AB", "S3D_RT_DEDUP", "dedupab", "dedup", 2, { 5, 7 } },
    // Price of shadows and of Fur "Always" (vs the default "Cutscene") on the same spot
    { "S3D_SHADOWS_AB", "S3D_SHADOWS", "shadowab", "shadows", 2, { 1, 0 } },
    { "S3D_FUR_MODE_AB", "S3D_FUR_MODE", "furab", "fur", 2, { 2, 1 } },
    // Shadow levers (gx_init.c): texture passes 8 / 3 / 0, refresh every frame / every 2nd
    { "S3D_SHADOW_TEXTURES_AB", "S3D_SHADOW_TEXTURES", "shtexab", "shadow textures", 3, { 8, 3, 0 } },
    { "S3D_SHADOW_REFRESH_AB", "S3D_SHADOW_REFRESH", "shrefab", "shadow refresh", 2, { 1, 2 } },
    // Back-face culling off / on (gx_render.c), Rcp lighting textures every frame / every 2nd (gx_init.c)
    { "S3D_CULL_AB", "S3D_CULL", "cullab", "cull", 2, { 0, 1 } },
    { "S3D_RCP_REFRESH_AB", "S3D_RCP_REFRESH", "rcpab", "rcp refresh", 2, { 1, 2 } },
    // Render thread on core 2 / replay on the game thread: does core 2's replay slow core 0's sceneRender?
    { "S3D_RENDER_THREAD_AB", "S3D_RENDER_THREAD", "rtab", "render thread", 2, { 2, 1 } },
    // Touch menu drawn / not drawn (touchui_3ds.c): its price on the render thread
    { "S3D_TOUCHUI_AB", "S3D_TOUCHUI_DRAW", "touchab", "touch UI", 2, { 1, 0 } },
    // Render-thread memos (gx_internal.h): all / none / copy index + texture memo / + two-way TEV cache
    { "S3D_RT_MEMO_AB", "S3D_RT_MEMO", "memoab", "rt memo", 4, { 15, 0, 3, 7 } },
    // Texture re-checks (gx_tex.c): 64-word fingerprint / header only for build-time textures / 8 words / none
    { "S3D_TEX_VERIFY_AB", "S3D_TEX_VERIFY", "verifyab", "tex verify", 4, { 1, 2, 3, 0 } },
    // Object draw distance in percent (port/lod_3ds.c): what the far objects cost
    { "S3D_OBJ_DIST_AB", "S3D_OBJ_DIST", "objdistab", "obj dist", 4, { 100, 75, 50, 30 } },
    // Minimum object screen size (port/lod_3ds.c): the game's 10 px / 20 / 30 / 45
    { "S3D_OBJ_MINPX_AB", "S3D_OBJ_MINPX", "minpxab", "obj min px", 4, { 10, 20, 30, 45 } },
    // Plants (port/lod_3ds.c): normal / frozen pose (skinned once) / hidden
    { "S3D_PLANTS_AB", "S3D_PLANTS", "plantab", "plants", 3, { 1, 2, 0 } },
    // citro3d fast paths (third_party/citro3d): all / without the uniform value check (bit 4)
    { "S3D_C3D_FAST_AB", "S3D_C3D_FAST", "c3dab", "c3d fast", 2, { 7, 3 } },
    // Animation LOD (port/lod_3ds.c): far models every 4th/8th frame / every frame / every 2nd/4th
    { "S3D_ANIM_LOD_AB", "S3D_ANIM_LOD", "animab", "anim lod", 3, { 2, 0, 1 } },
    // Render-thread steps (gx/gx_render.c S3D_RT_STEPS): all / none / state setup only / uniform check only
    { "S3D_RT_STEPS_AB", "S3D_RT_STEPS", "stepsab", "rt steps", 4, { 15, 0, 7, 8 } },
    // Display-list batching (gx/gx_dlcache.c): same-model objects in one draw / off
    { "S3D_DL_BATCH_AB", "S3D_DL_BATCH", "dlbab", "dl batch", 2, { 1, 0 } },
    // Map sort (port/mapsort_3ds.c): all bits / the game's order / without
    // skipping same-material setups (bit 2)
    { "S3D_MAP_SORT_AB", "S3D_MAP_SORT", "mapsortab", "map sort", 3, { 7, 0, 5 } },
    // Map material dedup (port/mapsort_3ds.c): both bits / off / fixed-function
    // setters only (no TEV setup skip)
    { "S3D_MAP_DEDUP_AB", "S3D_MAP_DEDUP", "mapddab", "map dedup", 3, { 3, 0, 1 } },
    // Fast fur shells (port/fur_3ds.c): the default bits / the game's path /
    // without lights once (bit 8). Fur must be drawn (Fur "Always" in the hub,
    // or the title with S3D_AB_MAP=63)
    { "S3D_FUR_FAST_AB", "S3D_FUR_FAST", "furfastab", "fur fast", 3, { 15, 0, 7 } },
    // Joint texture matrices (bug 27, patches/main/objprint_dolphin.c.sed): loaded
    // (the fix) / dropped as before. The price of the extra TEXMTX loads
    { "S3D_JOINT_TEXMTX_AB", "S3D_JOINT_TEXMTX", "jtexab", "joint texmtx", 2, { 1, 0 } },
};
void lod_frame(void);  // lod_3ds.c
void lod_census_request(int delay);
static int s_ab_test = -1;    // Index in k_ab_tests, -2 = none, -1 = not read yet
static int s_ab_frames;       // Frames per phase
static int s_ab_map = -2;     // S3D_AB_MAP; -2 = not read yet
static int s_ab_wait = -1;    // Frames until the start; -1 = waiting for the map
static int s_ab_count, s_ab_phase, s_ab_n;
static u64 s_ab_tick;
static double s_ab_frame_ms, s_ab_gpu_ms, s_ab_scene_ms, s_ab_own_ms;

static void ab_map_loaded(int map_id) {
    if (s_ab_map == -2) s_ab_map = cfg_int("S3D_AB_MAP", 7);
    if (s_ab_wait == -1 && map_id == s_ab_map) s_ab_wait = AB_START_DELAY;
}

static void ab_frame(void) {
    if (s_ab_test == -1) {
        s_ab_test = -2;
        if (s_ab_map == -2) s_ab_map = cfg_int("S3D_AB_MAP", 7);
        for (int i = 0; i < (int)(sizeof(k_ab_tests) / sizeof(k_ab_tests[0])); i++) {
            int n = cfg_int(k_ab_tests[i].env, 0);
            if (n <= 0) continue;
            s_ab_test = i;
            s_ab_frames = n < AB_SETTLE * 2 ? AB_SETTLE * 2 : n;
            if (s_ab_map < 0) {
                s_ab_wait = 0;
                lod_census_request(cfg_int("S3D_OBJ_CENSUS", 0));
            }
            char vals[48];
            int vl = 0;
            for (int v = 0; v < k_ab_tests[i].n; v++)
                vl += snprintf(vals + vl, sizeof(vals) - vl, "%s%d", v ? " / " : "", k_ab_tests[i].vals[v]);
            LOG_INFO(k_ab_tests[i].tag, "%s A/B: %d frames per phase, %s %s (%d values), starts %d frames "
                     "after map %d loads; stand still", k_ab_tests[i].label, s_ab_frames, k_ab_tests[i].var, vals,
                     k_ab_tests[i].n, AB_START_DELAY, s_ab_map);
            break;
        }
    }
    if (s_ab_test < 0 || s_ab_wait < 0) return;
    if (s_ab_wait > 0) {
        if (--s_ab_wait == 0) {
            s_ab_phase = 0;
            cfg_set(k_ab_tests[s_ab_test].var, k_ab_tests[s_ab_test].vals[0]);
            LOG_INFO(k_ab_tests[s_ab_test].tag, "A/B started");
            lod_census_request(cfg_int("S3D_OBJ_CENSUS", 0));
            s_ab_count = s_ab_n = 0;
            s_ab_tick = 0;
        }
        return;
    }
    u64 now = svcGetSystemTick();
    if (s_ab_tick && s_ab_count >= AB_SETTLE) {
        s_ab_frame_ms += (double)(now - s_ab_tick) / (SYSCLOCK_ARM11 / 1000.0);
        s_ab_gpu_ms += g_gx_gpu_p3d_last / 1000.0;
        s_ab_scene_ms += g_scene_render_ticks / (SYSCLOCK_ARM11 / 1000.0);
        s_ab_own_ms += g_scene_own_ticks / (SYSCLOCK_ARM11 / 1000.0);
        s_ab_n++;
    }
    s_ab_tick = now;
    if (s_ab_count == AB_SETTLE) probe_sample_mark(NULL);
    if (++s_ab_count < s_ab_frames) return;
    const char* tag = k_ab_tests[s_ab_test].tag;
    int val = k_ab_tests[s_ab_test].vals[s_ab_phase];
    if (s_ab_n) {
        LOG_INFO(tag, "%s %d: %d frames, frame %.2f ms, gpu %.2f ms, sceneRender %.2f ms (own %.2f)",
                 k_ab_tests[s_ab_test].label, val, s_ab_n, s_ab_frame_ms / s_ab_n, s_ab_gpu_ms / s_ab_n,
                 s_ab_scene_ms / s_ab_n, s_ab_own_ms / s_ab_n);
        char label[48];
        snprintf(label, sizeof(label), "%s %s=%d", tag, k_ab_tests[s_ab_test].var, val);
        probe_sample_mark(label);
    }
    s_ab_phase = (s_ab_phase + 1) % k_ab_tests[s_ab_test].n;
    cfg_set(k_ab_tests[s_ab_test].var, k_ab_tests[s_ab_test].vals[s_ab_phase]);
    s_ab_count = s_ab_n = 0;
    s_ab_frame_ms = s_ab_gpu_ms = s_ab_scene_ms = s_ab_own_ms = 0.0;
}

void diag_frame(void) {
    lod_frame();
    sweep_frame();
    texsweep_frame();
    ab_frame();
    // L+R+SELECT, once per press (START+SELECT quits, so START stays out)
    u32 combo = KEY_L | KEY_R | KEY_SELECT;
    // With S3D_FRAME_TRACE set, the combo starts a frame trace instead
    if ((hidKeysHeld() & combo) == combo && (hidKeysDown() & combo)) {
        if (ft_enabled()) ft_request();
        else diag_mark();
    }
    if (s_frames % 60 == 0) sample_vram();
    if (++s_frames >= DIAG_PERIOD) {
        s_frames = 0;
        diag_stats();
    }
}
