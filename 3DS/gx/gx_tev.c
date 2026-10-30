// TEV (GX texture environment) -> PICA200 texture combiners.
//
// A GX TEV program has up to 16 stages. Each computes, for color and alpha:
//     reg = (d +/- lerp(a, b, c) + bias) * scale,   lerp = a * (1 - c) + b * c
// and writes the result to one of four registers (PREV, REG0-2). Later stages
// read any register; the final pixel is PREV.
//
// The PICA200 has 6 combiner stages, each computing one function of three
// sources, and only two places to keep values: the previous stage's output and
// a combiner buffer that can latch the output of stages 0-3.
//
// gx_tev_apply() compiles the GX program onto that hardware:
//   - each GX stage becomes one PICA stage, or two when it needs both a lerp
//     and an add/subtract or a DIVIDE_2 scale, or up to four for a compare
//     op (if the program fits in 6 stages that way);
//   - the location of every GX register value (constant, previous output,
//     combiner buffer) is tracked per channel; values still needed later are
//     latched into the buffer before the previous output is overwritten;
//   - a final copy stage moves PREV into place if the last stage wrote
//     another register.
// Programs that do not fit are compiled again with single-stage
// approximations. State setters only record state.

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "gx_capture.h"
#include "gx_internal.h"
#include "log.h"
#include "movie_3ds.h"
#include "profiler.h"

typedef GxTevStage tev_stage_t;

// TEV state lives in g_gx.tev (gx_internal.h) so the recorder's shadow can
// mirror it; these names keep the rest of the file unchanged
#define s_stages g_gx.tev.stages
#define s_regs g_gx.tev.regs
#define s_kcolors g_gx.tev.kcolors
#define s_swap_tables g_gx.tev.swap_tables
#define s_num_stages g_gx.tev.num_stages

// --- State setters -------------------------------------------------------------

static inline bool valid_stage(GXTevStageID s) { return s < GX_MAX_TEVSTAGE; }

void gx_tev_reset(void) {
    memset(s_stages, 0, sizeof(s_stages));
    memset(s_regs, 0xFF, sizeof(s_regs));
    memset(s_kcolors, 0xFF, sizeof(s_kcolors));
    for (int i = 0; i < GX_MAX_TEVSTAGE; i++) {
        tev_stage_t* st = &s_stages[i];
        st->texcoord = i < GX_NUM_TEXCOORD ? i : GX_TEXCOORD_NULL;
        st->texmap = i < GX_NUM_TEXMAP ? i : GX_TEXMAP_NULL;
        st->channel = GX_COLOR0A0;
        st->color_in[0] = st->color_in[1] = st->color_in[2] = GX_CC_ZERO;
        st->color_in[3] = i == 0 ? GX_CC_RASC : GX_CC_CPREV;
        st->alpha_in[0] = st->alpha_in[1] = st->alpha_in[2] = GX_CA_ZERO;
        st->alpha_in[3] = i == 0 ? GX_CA_RASA : GX_CA_APREV;
        st->color_reg = st->alpha_reg = GX_TEVPREV;
        st->kcsel = GX_TEV_KCSEL_1;
        st->kasel = GX_TEV_KASEL_1;
    }
    s_num_stages = 1;
    // GXInit defaults: RGBA, RRRA, GGGA, BBBA
    for (int t = 0; t < GX_MAX_TEVSWAP; t++) {
        for (int c = 0; c < 3; c++) s_swap_tables[t][c] = t == 0 ? c : t - 1;
        s_swap_tables[t][3] = GX_CH_ALPHA;
    }
}

void GXSetNumTevStages(u8 nStages) {
    if (nStages == s_num_stages) return;
    gx_state_changed(GX_DIRTY_TEV);
    s_num_stages = nStages;
}

void GXSetTevOrder(GXTevStageID stage, GXTexCoordID coord, GXTexMapID map, GXChannelID color) {
    if (!valid_stage(stage)) return;
    tev_stage_t* st = &s_stages[stage];
    u8 m = (map & GX_TEX_DISABLE) ? GX_TEXMAP_NULL : (u8)map;
    if (st->texcoord == coord && st->texmap == m && st->channel == color) return;
    gx_state_changed(GX_DIRTY_TEV);
    st->texcoord = coord;
    st->texmap = m;
    st->channel = color;
}

void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d) {
    if (!valid_stage(stage)) return;
    u8 in[4] = { a, b, c, d };
    if (memcmp(s_stages[stage].color_in, in, 4) == 0) return;
    gx_state_changed(GX_DIRTY_TEV);
    memcpy(s_stages[stage].color_in, in, 4);
}

void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d) {
    if (!valid_stage(stage)) return;
    u8 in[4] = { a, b, c, d };
    if (memcmp(s_stages[stage].alpha_in, in, 4) == 0) return;
    gx_state_changed(GX_DIRTY_TEV);
    memcpy(s_stages[stage].alpha_in, in, 4);
}

// The PICA always clamps, so the clamp flag is ignored
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                     GXTevRegID out_reg) {
    (void)clamp;
    if (!valid_stage(stage)) return;
    tev_stage_t* st = &s_stages[stage];
    if (st->color_op == op && st->color_bias == bias && st->color_scale == scale && st->color_reg == out_reg) return;
    gx_state_changed(GX_DIRTY_TEV);
    st->color_op = op;
    st->color_bias = bias;
    st->color_scale = scale;
    st->color_reg = out_reg < GX_MAX_TEVREG ? out_reg : GX_TEVPREV;
}

void GXSetTevAlphaOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp,
                     GXTevRegID out_reg) {
    (void)clamp;
    if (!valid_stage(stage)) return;
    tev_stage_t* st = &s_stages[stage];
    if (st->alpha_op == op && st->alpha_bias == bias && st->alpha_scale == scale && st->alpha_reg == out_reg) return;
    gx_state_changed(GX_DIRTY_TEV);
    st->alpha_op = op;
    st->alpha_bias = bias;
    st->alpha_scale = scale;
    st->alpha_reg = out_reg < GX_MAX_TEVREG ? out_reg : GX_TEVPREV;
}

void GXSetTevColor(GXTevRegID id, GXColor color) {
    if (id >= GX_MAX_TEVREG || memcmp(&s_regs[id], &color, sizeof(color)) == 0) return;
    gx_state_changed(GX_DIRTY_KCOL);
    s_regs[id] = color;
}

void GXSetTevColorS10(GXTevRegID id, GXColorS10 color) {
    // 10-bit signed register values, clamped to the 8-bit range
    #define CLAMP8(v) ((u8)((v) < 0 ? 0 : (v) > 255 ? 255 : (v)))
    GXColor c = { CLAMP8(color.r), CLAMP8(color.g), CLAMP8(color.b), CLAMP8(color.a) };
    #undef CLAMP8
    GXSetTevColor(id, c);
}

void GXSetTevKColor(GXTevKColorID id, GXColor color) {
    if (id >= GX_MAX_KCOLOR || memcmp(&s_kcolors[id], &color, sizeof(color)) == 0) return;
    gx_state_changed(GX_DIRTY_KCOL);
    s_kcolors[id] = color;
}

void GXSetTevKColorSel(GXTevStageID stage, GXTevKColorSel sel) {
    if (!valid_stage(stage) || s_stages[stage].kcsel == sel) return;
    gx_state_changed(GX_DIRTY_TEV);
    s_stages[stage].kcsel = sel;
}

void GXSetTevKAlphaSel(GXTevStageID stage, GXTevKAlphaSel sel) {
    if (!valid_stage(stage) || s_stages[stage].kasel == sel) return;
    gx_state_changed(GX_DIRTY_TEV);
    s_stages[stage].kasel = sel;
}

// Presets, as defined by the Dolphin SDK (stages after the first read the
// previous stage instead of the rasterized color)
void GXSetTevOp(GXTevStageID id, GXTevMode mode) {
    GXTevColorArg carg = id == GX_TEVSTAGE0 ? GX_CC_RASC : GX_CC_CPREV;
    GXTevAlphaArg aarg = id == GX_TEVSTAGE0 ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
        case GX_MODULATE:
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, carg, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
            break;
        case GX_DECAL:
            GXSetTevColorIn(id, carg, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
            break;
        case GX_BLEND:
            GXSetTevColorIn(id, carg, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, aarg, GX_CA_ZERO);
            break;
        case GX_REPLACE:
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
            break;
        default:  // GX_PASSCLR
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, carg);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, aarg);
            break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

// Swap tables map to PICA operands that read single channels (see
// channel_op); permutations that mix channels are not representable
void GXSetTevSwapMode(GXTevStageID stage, GXTevSwapSel ras_sel, GXTevSwapSel tex_sel) {
    if (!valid_stage(stage) || ras_sel >= GX_MAX_TEVSWAP || tex_sel >= GX_MAX_TEVSWAP) return;
    tev_stage_t* st = &s_stages[stage];
    if (st->ras_swap == ras_sel && st->tex_swap == tex_sel) return;
    gx_state_changed(GX_DIRTY_TEV);
    st->ras_swap = (u8)ras_sel;
    st->tex_swap = (u8)tex_sel;
}
void GXSetTevSwapModeTable(GXTevSwapSel table, GXTevColorChan red, GXTevColorChan green, GXTevColorChan blue,
                           GXTevColorChan alpha) {
    if (table >= GX_MAX_TEVSWAP) return;
    u8 t[4] = { (u8)red, (u8)green, (u8)blue, (u8)alpha };
    if (memcmp(s_swap_tables[table], t, 4) == 0) return;
    gx_state_changed(GX_DIRTY_TEV);
    memcpy(s_swap_tables[table], t, 4);
}

// Indirect texturing has no PICA equivalent: the warp is dropped and the
// stage samples its texture directly. The game's GXSetTevIndirect calls go
// through port_tev_indirect (sed patches pass __func__), which counts them
// per calling building block as "ind.<function>" so the remaining gaps show
// up in the profiler report. Known blocks get a stand-in: the stage's texture
// slot is offset by analytic waves in the vertex shader (s_stage_warp, the
// amplitude in texture units; see texwarp in default.v.pica).
#define IND_MAX_SITES 48
#define s_stage_warp g_gx.tev.stage_warp
#define s_num_ind_stages g_gx.tev.num_ind_stages

// Bump alpha (GX_ALPHA_BUMPN): a stage with an indirect alpha select reads,
// as its rasterized alpha, one component of the indirect texture (S = alpha,
// T = blue, U = green) at the indirect stage's coordinate. The fur shells
// mask their noise with it, so it is emulated by sampling the indirect
// texture in a regular unit (see gx_tev_build_unit_map)
#define s_stage_ind g_gx.tev.stage_ind
#define s_stage_bump_sel g_gx.tev.stage_bump_sel
#define s_ind_coord g_gx.tev.ind_coord
#define s_ind_map g_gx.tev.ind_map
// Stages set up by setupWaterCausticTev: the water ripple (bug 35) reads the
// caustic texture at TEX0, so the unit map carries TEX0 in a spare unit
#define s_stage_ripple g_gx.tev.stage_ripple

void gx_tev_set_ind_order(int ind_stage, int tex_coord, int tex_map) {
    if (ind_stage < 0 || ind_stage >= GX_MAX_INDTEXSTAGE) return;
    if (s_ind_coord[ind_stage] == (u8)tex_coord && s_ind_map[ind_stage] == (u8)tex_map) return;
    s_ind_coord[ind_stage] = (u8)tex_coord;
    s_ind_map[ind_stage] = (u8)tex_map;
    gx_state_changed(GX_DIRTY_TEV);
}

int gx_tev_ind_coord(int ind_stage) {
    return ind_stage >= 0 && ind_stage < GX_MAX_INDTEXSTAGE ? s_ind_coord[ind_stage] : -1;
}

static void set_stage_bump(int stage, int ind, int sel) {
    if (stage < 0 || stage >= GX_MAX_TEVSTAGE) return;
    if (s_stage_ind[stage] == ind && s_stage_bump_sel[stage] == sel) return;
    s_stage_ind[stage] = (u8)ind;
    s_stage_bump_sel[stage] = (u8)sel;
    gx_state_changed(GX_DIRTY_TEV);
}


// Each site's amplitude is the texture-unit swing of the warp: about
// 255 * m * 2^scale_exp texels of the indirect read, softened where the
// per-vertex stand-in would tear. The caustic, noise and ring warps (5.3)
// read textures the game fills at runtime from its RNG and project them from
// world position, so a build-time flipbook can't reproduce them; they get the
// same analytic swirl as the water.
static const struct { const char* func; float amp; int group; } s_warp_sites[] = {
    // Water reflection ripple (GRAPHICS_PLAN.md 5.1): the caustic texture
    // displaces the screen-projected reflection by up to about 0.016
    { "setupWaterCausticTev", 0.012f, 0 },
    // Heavy fog (5.5): the caustic texture (matrix 0.5, scale -2) shifts the
    // fog texture's depth/height coordinate by up to about 16 of its 64
    // texels; the stand-in uses a softer swirl so the fog bank's edge rolls
    // without tearing
    { "renderHeavyFog", 0.08f, 0 },
    // 5.3: caustic base (world-projected caustic over the base texture),
    // wavy caustic (scale -3), warped noise (scale -4 on a 32x32 noise) and
    // the warped ring (scale -1, two added reads; the strongest)
    { "setupCausticBaseTevStages", 0.02f, 1 },
    { "addWavyCausticTevStage", 0.03f, 1 },
    { "addWarpedNoiseTevStages", 0.03f, 1 },
    { "addWarpedRingTevStages", 0.06f, 1 },
};

// S3D_WATER_WARP_PCT scales the water ripple (setupWaterCausticTev; bug 33: at 100 the Krazoa wall
// panes looked like clear glass next to the GameCube's strong refraction,
// whose caustic offsets reach ~16 texels of the 320 wide copy)
int port_ring_baked(void);  // port/caustic_3ds.c

float gx_tev_warp_for_site(const char* func) {
    static int water = -1, caustic = -1, water_pct = -1;
    CFG_LIVE(water, cfg_bool("S3D_WATER_WARP", true));
    CFG_LIVE(caustic, cfg_bool("S3D_CAUSTIC_WARP", true));
    CFG_LIVE(water_pct, cfg_int("S3D_WATER_WARP_PCT", 300));
    int enabled[2] = { water, caustic };
    for (size_t i = 0; i < sizeof(s_warp_sites) / sizeof(s_warp_sites[0]); i++) {
        if (strcmp(func, s_warp_sites[i].func)) continue;
        if (!enabled[s_warp_sites[i].group]) return 0.0f;
        if (!strcmp(func, "setupWaterCausticTev")) return s_warp_sites[i].amp * (float)water_pct / 100.0f;
        // Bug 35: the warped ring's net is baked into its texture
        if (!strcmp(func, "addWarpedRingTevStages") && port_ring_baked()) return 0.0f;
        return s_warp_sites[i].amp;
    }
    return 0.0f;
}

// Env map bump (GRAPHICS_PLAN.md 5.6). addEnvMapBumpStages bends the sphere
// map coordinate with two indirect stages that read the bump texture through
// the binormal and tangent. Without indirect stages those two stages only
// pass the colour through, and the bump texture coordinate is never read. So
// by default the game's own bump-off path (lbl_803DB5E8 bit 0 clear) runs
// instead: the plain sphere map, with 2 TEV stages, 3 texgens and a texmap
// fewer. Counter: env.bump_fallback. S3D_ENV_BUMP_SKIP=0 keeps the stages.
int port_env_bump_skip(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = cfg_bool("S3D_ENV_BUMP_SKIP", true);
    if (enabled) PROF_COUNT("env.bump_fallback", 1);
    return enabled;
}

static void set_stage_warp(int stage, float amp) {
    if (stage < 0 || stage >= GX_MAX_TEVSTAGE || s_stage_warp[stage] == amp) return;
    s_stage_warp[stage] = amp;
    gx_state_changed(GX_DIRTY_TEV);
}

void GXSetNumIndStages(u8 nIndStages) {
    if (nIndStages) LOG_ONCE(LOG_LEVEL_WARN, "gx", "indirect texturing not supported");
    s_num_ind_stages = nIndStages;
    if (!nIndStages) {
        for (int s = 0; s < GX_MAX_TEVSTAGE; s++) set_stage_warp(s, 0.0f);
        memset(s_stage_ripple, 0, sizeof(s_stage_ripple));
    }
}
void GXSetTevDirect(GXTevStageID tev_stage) {
    set_stage_warp(tev_stage, 0.0f);
    if (tev_stage < GX_MAX_TEVSTAGE) s_stage_ripple[tev_stage] = 0;
    set_stage_bump(tev_stage, 0, GX_ITBA_OFF);
}

void port_tev_indirect(const char* func, GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,
                       GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,
                       GXBool add_prev, GXBool ind_lod, GXIndTexAlphaSel alpha_sel) {
    static const char* funcs[IND_MAX_SITES];
    static char names[IND_MAX_SITES][48];
    static int ids[IND_MAX_SITES], count;
    static float amps[IND_MAX_SITES];
    static unsigned amps_gen;
    (void)ind_stage; (void)format; (void)bias_sel; (void)matrix_sel;
    (void)wrap_s; (void)wrap_t; (void)add_prev; (void)ind_lod;
    if (!func) func = "other";
    int i = 0;
    while (i < count && funcs[i] != func) i++;
    if (i == count) {
        if (count == IND_MAX_SITES) i = IND_MAX_SITES - 1;
        else {
            funcs[i] = func;
            snprintf(names[i], sizeof(names[i]), "ind.%s", func);
            ids[i] = prof_register_counter(names[i]);
            amps[i] = gx_tev_warp_for_site(func);
            count++;
        }
    }
    // The amplitudes follow live config changes (S3D_WATER_WARP etc.)
    if (amps_gen != cfg_gen()) {
        amps_gen = cfg_gen();
        for (int j = 0; j < count; j++) amps[j] = gx_tev_warp_for_site(funcs[j]);
    }
    prof_add(ids[i], 1);
    set_stage_warp(tev_stage, amps[i]);
    if (tev_stage < GX_MAX_TEVSTAGE) s_stage_ripple[tev_stage] = !strcmp(func, "setupWaterCausticTev");
    set_stage_bump(tev_stage, ind_stage, alpha_sel);
}
void GXSetTevIndirect(GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,
                      GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,
                      GXBool add_prev, GXBool ind_lod, GXIndTexAlphaSel alpha_sel) {
    port_tev_indirect(NULL, tev_stage, ind_stage, format, bias_sel, matrix_sel, wrap_s, wrap_t, add_prev, ind_lod,
                      alpha_sel);
}
void GXSetTevIndRepeat(GXTevStageID tev_stage) { (void)tev_stage; }

// --- Lighting channel -------------------------------------------------------------------

static int gx_stage_count(void);

static bool stage_reads_ras(const tev_stage_t* st) {
    for (int i = 0; i < 4; i++) {
        if (st->color_in[i] == GX_CC_RASC || st->color_in[i] == GX_CC_RASA || st->alpha_in[i] == GX_CA_RASA)
            return true;
    }
    return false;
}

// The PICA has one vertex color, so only one GX lighting channel is computed:
// channel 0 when any stage reads it, else channel 1 when a stage reads that
// (specular-only passes). Stages on the other channel read zero.
int gx_tev_ras_channel(void) {
    bool c1 = false;
    for (int s = 0; s < gx_stage_count(); s++) {
        const tev_stage_t* st = &s_stages[s];
        if (!stage_reads_ras(st)) continue;
        if (st->channel == GX_COLOR0 || st->channel == GX_ALPHA0 || st->channel == GX_COLOR0A0) return 0;
        if (st->channel == GX_COLOR1 || st->channel == GX_ALPHA1 || st->channel == GX_COLOR1A1) c1 = true;
    }
    return c1 ? 1 : 0;
}

// Channel 1 color as the PICA secondary fragment color (fragment specular):
// with it set, stages on COLOR1 / COLOR1A1 read GPU_FRAGMENT_SECONDARY_COLOR
// while channel 0 keeps the vertex color. Channel 1 alpha still reads zero.
static bool s_frag_sec;

void gx_tev_set_frag_secondary(bool on) { s_frag_sec = on; }

bool gx_tev_reads_ch1_color(void) {
    for (int s = 0; s < gx_stage_count(); s++) {
        const tev_stage_t* st = &s_stages[s];
        if (st->channel != GX_COLOR1 && st->channel != GX_COLOR1A1) continue;
        for (int i = 0; i < 4; i++)
            if (st->color_in[i] == GX_CC_RASC) return true;
    }
    return false;
}

// --- Texture unit assignment ---------------------------------------------------------

static int gx_stage_count(void) {
    return s_num_stages < GX_MAX_TEVSTAGE ? s_num_stages : GX_MAX_TEVSTAGE;
}

// The program compiled for the current draw: the game's stages, or a rewritten
// copy (movie frames)
static const tev_stage_t* s_cur = s_stages;
static int s_cur_n = 1;
static tev_stage_t s_rewritten[GX_MAX_TEVSTAGE];

static bool stage_uses_texture(const tev_stage_t* st);

static int stage_movie_plane(const tev_stage_t* st) {
    if (st->texmap >= GX_NUM_TEXMAP || !g_gx.tex[st->texmap].valid) return MOVIE_PLANE_NONE;
    return movie_plane(g_gx.tex[st->texmap].data, NULL);
}

// The game draws the attract movie from Y/U/V planes with a YUV->RGB program
// (addYUVVideoTevStages, THPPlayerDrawCurrentFrame): stages sampling U and V,
// one sampling Y, then stages combining the partial sums, the last of which
// writes PREV. With build-time RGB frames bound on the Y plane, that block
// becomes one stage: PREV.rgb = texture, alpha kept from before the block.
static void prepare_program(void) {
    s_cur = s_stages;
    s_cur_n = gx_stage_count();
    if (!movie_active()) return;

    int first = -1, ystage = -1;
    for (int s = 0; s < s_cur_n; s++) {
        int plane = stage_uses_texture(&s_stages[s]) ? stage_movie_plane(&s_stages[s]) : MOVIE_PLANE_NONE;
        if (plane == MOVIE_PLANE_NONE) continue;
        if (first < 0) first = s;
        if (plane == MOVIE_PLANE_Y) {
            ystage = s;
            break;
        }
    }
    if (ystage < 0) return;
    int last = s_cur_n - 1;
    for (int s = ystage + 1; s < s_cur_n; s++) {
        if (s_stages[s].color_reg == GX_TEVPREV) {
            last = s;
            break;
        }
    }

    int n = 0;
    for (int s = 0; s < first; s++) s_rewritten[n++] = s_stages[s];
    tev_stage_t st = s_stages[ystage];
    static const u8 color_in[4] = { GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC };
    memcpy(st.color_in, color_in, 4);
    st.alpha_in[0] = st.alpha_in[1] = st.alpha_in[2] = GX_CA_ZERO;
    st.alpha_in[3] = first > 0 ? GX_CA_APREV : GX_CA_TEXA;
    st.color_op = st.alpha_op = GX_TEV_ADD;
    st.color_bias = st.alpha_bias = GX_TB_ZERO;
    st.color_scale = st.alpha_scale = GX_CS_SCALE_1;
    st.color_reg = st.alpha_reg = GX_TEVPREV;
    st.tex_swap = GX_TEV_SWAP0;
    s_rewritten[n++] = st;
    for (int s = last + 1; s < s_cur_n; s++) s_rewritten[n++] = s_stages[s];
    s_cur = s_rewritten;
    s_cur_n = n;
    PROF_COUNT("tev.movie_rewrites", 1);
}

// The unit map depends on the bound textures, not only on TEV state: the
// current program was rewritten for the movie, or a stage samples a movie
// plane (gx_render_prepare keeps the map on texture-only changes otherwise)
bool gx_tev_movie_dependent(void) {
    if (!movie_active()) return false;
    if (s_cur != s_stages) return true;
    int n = gx_stage_count();
    for (int s = 0; s < n; s++)
        if (stage_uses_texture(&s_stages[s]) && stage_movie_plane(&s_stages[s]) != MOVIE_PLANE_NONE) return true;
    return false;
}

static bool is_tex_carg(u8 c) { return c == GX_CC_TEXC || c == GX_CC_TEXA; }

// d + tex * w with a constant weight (konst, 1, 1/2), alpha not sampling the
// texture: one tap of a blur (gx_tev_build_unit_map)
static bool stage_is_weighted_tap(const tev_stage_t* st) {
    if (st->color_op != GX_TEV_ADD || st->color_in[0] != GX_CC_ZERO || is_tex_carg(st->color_in[3])) return false;
    u8 b = st->color_in[1], c = st->color_in[2];
    u8 w = is_tex_carg(b) ? c : is_tex_carg(c) ? b : 0xFF;
    if (w != GX_CC_KONST && w != GX_CC_ONE && w != GX_CC_HALF) return false;
    for (int i = 0; i < 4; i++)
        if (st->alpha_in[i] == GX_CA_TEXA) return false;
    return true;
}

static bool stage_uses_texture(const tev_stage_t* st) {
    for (int i = 0; i < 4; i++) {
        u8 c = st->color_in[i];
        if (c == GX_CC_TEXC || c == GX_CC_TEXA) return true;
        if (st->alpha_in[i] == GX_CA_TEXA) return true;
    }
    return false;
}

// How much a texture matters to the picture, judged from the first stage that
// samples it (higher is kept first when a draw has more textures than units,
// 4.1). GX computes d + (1-c)*a + c*b
static int texmap_priority(const tev_stage_t* st, bool first) {
    if (first) return 100;  // The base texture
    const u8* in = st->color_in;
    bool ctex = false;
    for (int i = 0; i < 4; i++) ctex |= in[i] == GX_CC_TEXC || in[i] == GX_CC_TEXA;
    if (!ctex) return 70;  // Alpha only: cutouts and masks show badly when lost
    bool t[4];
    for (int i = 0; i < 4; i++) t[i] = in[i] == GX_CC_TEXC || in[i] == GX_CC_TEXA;
    if (t[3] && !t[0] && !t[1] && !t[2]) return 30;  // prev + tex: additive reflection or glow
    if (in[0] == GX_CC_ZERO && (t[1] != t[2])) {
        // tex * x (+ d): a modulation (light or shadow map) unless d carries
        // the surface, which makes it an additive term
        return in[3] == GX_CC_ZERO ? 60 : 30;
    }
    if ((t[0] || t[1]) && !t[2] && in[0] != GX_CC_ZERO && in[1] != GX_CC_ZERO) {
        // lerp(tex, x) by a computed alpha (prev / rasterized): a decal layer
        // such as an eye or a painted marking; by a constant: a fade
        bool computed = in[2] == GX_CC_APREV || in[2] == GX_CC_CPREV || in[2] == GX_CC_RASA || in[2] == GX_CC_RASC;
        return computed ? 80 : 20;
    }
    return 50;
}

// The (texmap, texcoord) a stage reads as bump alpha, or false
static bool stage_bump_source(int s, const tev_stage_t* st, u8* map, u8* coord) {
    if (s_cur != s_stages || st->channel != GX_ALPHA_BUMPN || s_stage_bump_sel[s] == GX_ITBA_OFF) return false;
    int ind = s_stage_ind[s];
    if (ind >= s_num_ind_stages || ind >= GX_MAX_INDTEXSTAGE) return false;
    // Bug 37: the eye glow (addWarpedNoiseTevStages) weights its colour lerp
    // by the bump alpha, so colour RASA counts too
    bool rasa = false;
    for (int i = 0; i < 4; i++) rasa |= st->alpha_in[i] == GX_CA_RASA || st->color_in[i] == GX_CC_RASA;
    if (!rasa || s_ind_map[ind] >= GX_NUM_TEXMAP || s_ind_coord[ind] >= GX_NUM_TEXCOORD) return false;
    *map = s_ind_map[ind];
    *coord = s_ind_coord[ind];
    return true;
}

// Bug 37: a texture that is only a colour lerp operand weighted by the bump
// alpha, lerp(a, b, RASA) with the texture in a or b and alpha not sampling
// it: Krystal's Krazoa eye glow, lerp(K, noise, eyeball texture), where the
// noise matters far less than the bump texture that shapes iris and pupil.
// Dropped, it reads as its average colour (map->avg_map, gx_tev_set_drop_avg)
static bool stage_tex_averageable(int s, const tev_stage_t* st) {
    u8 bm, bc;
    if (!stage_bump_source(s, st, &bm, &bc) || bm == st->texmap) return false;
    const u8* in = st->color_in;
    if (in[2] != GX_CC_RASA || is_tex_carg(in[3])) return false;
    if (!is_tex_carg(in[0]) && !is_tex_carg(in[1])) return false;
    for (int i = 0; i < 4; i++)
        if (st->alpha_in[i] == GX_CA_TEXA) return false;
    // Every stage sampling the texture must be such a lerp
    for (int t = 0; t < s_cur_n; t++)
        if (t != s && s_cur[t].texmap == st->texmap && stage_uses_texture(&s_cur[t])) return false;
    return true;
}

static bool s_applied_valid;  // gx_tev_apply's last program is current (defined below)
static u8 s_drop_avg[4] = { 128, 128, 128, 255 };

void gx_tev_set_drop_avg(const u8 rgba[4]) {
    if (memcmp(s_drop_avg, rgba, 4) == 0) return;
    memcpy(s_drop_avg, rgba, 4);
    s_applied_valid = false;  // Texture-only changes keep the program: patch it again
}

// Bug 32: a screen-projected texture added as the last term, PREV + tex * x
// through a GX_TG_MTX3x4 position texgen: the maps' small reflection
// (shader_dolphin.c addSmallReflectionTevStage, floors whose shader has flag
// 0x100, e.g. Krazoa Palace). Units divide s/q per vertex, which bends the
// reflection along the floor's large triangles and breaks it where a vertex
// is behind the camera (as the water did, bug 28). Only unit 0 in projection
// mode divides per fragment, with the water shader, which computes slot 0
// alone. So the term is left out of the program (compile keeps PREV for a
// dropped added term) and the multipass second pass draws it projectively on
// unit 0 (gx_render.c multipass_prepare). Decided from TEV and texgen state
// only, like the rest of the unit map; the render side allows it by pixel
// state (gx_tev_set_proj_split, S3D_REFLECT_SPLIT)
static bool s_proj_split_allow;
void gx_tev_set_proj_split(bool allow) { s_proj_split_allow = allow; }
bool gx_tev_proj_split_allowed(void) { return s_proj_split_allow; }

static bool mp_arg_ok(u8 arg, int s_d);

static int proj_split_texmap(void) {
    if (!s_proj_split_allow || s_num_ind_stages || s_cur_n < 1) return -1;
    int l = s_cur_n - 1;
    const tev_stage_t* st = &s_cur[l];
    const u8* in = st->color_in;
    if (st->texmap >= GX_NUM_TEXMAP || st->texcoord >= g_gx.num_texgens || !stage_uses_texture(st)) return -1;
    const GxTexGen* tg = &g_gx.texgen[st->texcoord];
    if (tg->func != GX_TG_MTX3x4 || tg->src != GX_TG_POS) return -1;
    if (st->color_op != GX_TEV_ADD || st->color_bias != GX_TB_ZERO || st->color_scale != GX_CS_SCALE_1 ||
        st->color_reg != GX_TEVPREV || in[0] != GX_CC_ZERO || in[3] != GX_CC_CPREV)
        return -1;
    if (!(in[1] == GX_CC_TEXC && mp_arg_ok(in[2], l)) && !(in[2] == GX_CC_TEXC && mp_arg_ok(in[1], l))) return -1;
    for (int i = 0; i < 4; i++)
        if (st->alpha_in[i] == GX_CA_TEXA) return -1;
    for (int s = 0; s < l; s++)
        if (s_cur[s].texmap == st->texmap && stage_uses_texture(&s_cur[s])) return -1;
    return st->texmap;
}

void gx_tev_build_unit_map(GxUnitMap* map) {
    memset(map, -1, sizeof(*map));
    map->unit_count = 0;
    prepare_program();

    // More textures than units: keep the most important ones (texmap_priority,
    // ties to the earlier stage) instead of the first three. S3D_TEV_DROP_PRIORITY=0
    // keeps the first three
    static int prio_on = -1;
    if (prio_on < 0) prio_on = cfg_bool("S3D_TEV_DROP_PRIORITY", true) ? 1 : 0;
    bool keep[GX_NUM_TEXMAP];
    memset(keep, 1, sizeof(keep));
    if (prio_on) {
        int score[GX_NUM_TEXMAP], n = 0;
        bool seen[GX_NUM_TEXMAP] = { 0 };
        for (int s = 0; s < s_cur_n; s++) {
            const tev_stage_t* st = &s_cur[s];
            if (st->texmap >= GX_NUM_TEXMAP || st->texcoord >= GX_NUM_TEXCOORD || !stage_uses_texture(st)) continue;
            // Every further stage that samples a texture makes it matter more
            u8 bm, bc;
            if (stage_bump_source(s, st, &bm, &bc)) {
                // The bump alpha mask: without it every fur shell is opaque
                if (seen[bm]) score[bm] += 10 * 16;
                else { seen[bm] = true; score[bm] = 75 * 16 - s; n++; }
            }
            if (seen[st->texmap]) { score[st->texmap] += 10 * 16; continue; }
            seen[st->texmap] = true;
            score[st->texmap] = texmap_priority(st, n == 0) * 16 - s;
            static int avg_on = -1;  // S3D_TEV_DROP_AVG=0: no average-colour stand-ins
            if (avg_on < 0) avg_on = cfg_bool("S3D_TEV_DROP_AVG", true) ? 1 : 0;
            if (avg_on && n > 0 && stage_tex_averageable(s, st)) score[st->texmap] = 40 * 16 - s;
            // The texgen tells more than the stage: a texture read through the
            // model's own UVs is surface detail (the staff's base texture,
            // blended in at the last stage), one projected from position is a
            // light or shadow overlay whose loss only drops a darkening term
            if (n > 0 && st->texcoord < g_gx.num_texgens) {
                const u8 func = g_gx.texgen[st->texcoord].func, src = g_gx.texgen[st->texcoord].src;
                if (src >= GX_TG_TEX0 && src <= GX_TG_TEX7) score[st->texmap] += 20 * 16;
                else if (func == GX_TG_MTX3x4 && src == GX_TG_POS) score[st->texmap] -= 20 * 16;
            }
            n++;
        }
        if (n > GX_PICA_TEX_UNITS) {
            memset(keep, 0, sizeof(keep));
            for (int k = 0; k < GX_PICA_TEX_UNITS; k++) {
                int best = -1;
                for (int m = 0; m < GX_NUM_TEXMAP; m++)
                    if (seen[m] && !keep[m] && (best < 0 || score[m] > score[best])) best = m;
                keep[best] = true;
            }
            PROF_COUNT("tev.priority_drops", n - GX_PICA_TEX_UNITS);
        }
    }
    map->avg_map = -1;
    for (int s = 0; s < s_cur_n && prio_on; s++) {
        const tev_stage_t* st = &s_cur[s];
        if (st->texmap < GX_NUM_TEXMAP && !keep[st->texmap] && stage_uses_texture(st) && stage_tex_averageable(s, st)) {
            static int avg_on = -1;
            if (avg_on < 0) avg_on = cfg_bool("S3D_TEV_DROP_AVG", true) ? 1 : 0;
            if (avg_on) map->avg_map = (s8)st->texmap;
            break;
        }
    }
    map->proj_split = (s8)proj_split_texmap();
    if (map->proj_split >= 0) {
        keep[map->proj_split] = false;
        PROF_COUNT("tev.proj_splits", 1);
    }

    for (int s = 0; s < s_cur_n; s++) {
        const tev_stage_t* st = &s_cur[s];
        if (st->texmap >= GX_NUM_TEXMAP || st->texcoord >= GX_NUM_TEXCOORD || !stage_uses_texture(st)) continue;
        u8 bm, bc;
        if (stage_bump_source(s, st, &bm, &bc) && keep[bm]) {
            int u = -1;
            for (int k = 0; k < map->unit_count; k++)
                if (map->unit_texmap[k] == bm && map->unit_texcoord[k] == bc) u = k;
            if (u < 0 && map->unit_count < GX_PICA_TEX_UNITS) {
                u = map->unit_count++;
                map->unit_texmap[u] = bm;
                map->unit_texcoord[u] = bc;
                if (map->texmap_unit[bm] < 0) map->texmap_unit[bm] = u;
                map->unit_warp[u] = 0.0f;
            }
            if (u >= 0) {
                map->stage_bump[s] = (s8)u;
                PROF_COUNT("tev.bump_alpha", 1);
            }
        }
        if (!keep[st->texmap]) continue;
        // One unit per (texmap, texcoord) pair: GX may sample one texture
        // with two coordinates (water: screen copy plain and mirrored)
        int found = -1;
        for (int u = 0; u < map->unit_count; u++)
            if (map->unit_texmap[u] == st->texmap && map->unit_texcoord[u] == st->texcoord) found = u;
        if (found >= 0) {
            map->stage_unit[s] = (s8)found;
            continue;
        }
        if (map->unit_count >= GX_PICA_TEX_UNITS) {
            // Bug 26: a blur adds N taps of one texture, PREV + tex(coord i) *
            // konst (the combat depth-of-field: 5 taps of the screen copy,
            // then halved). Dropping taps darkened the blurred background to
            // 40% (black rocks). A pure weighted tap whose texture is already
            // bound with another coordinate samples that unit instead: less
            // spread, right brightness. Not for products with registers (the
            // light pairs). S3D_TEV_TAP_REUSE=0 turns it off
            static int tap_reuse = -1;
            if (tap_reuse < 0) tap_reuse = cfg_bool("S3D_TEV_TAP_REUSE", true) ? 1 : 0;
            if (tap_reuse && stage_is_weighted_tap(st)) {
                int same = -1;
                for (int u = 0; u < map->unit_count; u++)
                    if (map->unit_texmap[u] == st->texmap) same = u;
                if (same >= 0) {
                    map->stage_unit[s] = (s8)same;
                    PROF_COUNT("tev.tap_reuse", 1);
                    continue;
                }
            }
            LOG_ONCE(LOG_LEVEL_WARN, "tev", "more than %d textures in one draw (extra textures dropped)",
                     GX_PICA_TEX_UNITS);
            continue;
        }
        int u = map->unit_count++;
        map->unit_texmap[u] = st->texmap;
        map->unit_texcoord[u] = st->texcoord;
        if (map->texmap_unit[st->texmap] >= 0) PROF_COUNT("tev.texmap_multi_coord", 1);
        else map->texmap_unit[st->texmap] = u;
        map->stage_unit[s] = (s8)u;
        map->unit_warp[u] = s_num_ind_stages && s_cur == s_stages ? s_stage_warp[s] : 0.0f;
    }

    // Only PICA unit 0 can divide by q per fragment (projective texturing);
    // units 1 and 2 divide per vertex, which smears screen-projected
    // coordinates (water reflection / refraction) across large triangles.
    // Move the last projected coordinate (the one the final stages blend in)
    // to unit 0. Only water splits use the projective mode so far
    // (gx_render.c set_ws_proj, bug 28); elsewhere unit 0 divides per vertex
    // like the others (bug 12)
    int proj = -1;
    for (int u = 0; u < map->unit_count; u++) {
        int c = map->unit_texcoord[u];
        if (c >= 0 && c < g_gx.num_texgens && g_gx.texgen[c].func == GX_TG_MTX3x4) proj = u;
    }
    if (proj > 0) {
        PROF_COUNT("tev.proj_unit0_swaps", 1);
        s8 t;
        t = map->unit_texmap[0]; map->unit_texmap[0] = map->unit_texmap[proj]; map->unit_texmap[proj] = t;
        t = map->unit_texcoord[0]; map->unit_texcoord[0] = map->unit_texcoord[proj]; map->unit_texcoord[proj] = t;
        float w = map->unit_warp[0]; map->unit_warp[0] = map->unit_warp[proj]; map->unit_warp[proj] = w;
        for (int s = 0; s < GX_MAX_TEVSTAGE; s++) {
            if (map->stage_unit[s] == 0) map->stage_unit[s] = (s8)proj;
            else if (map->stage_unit[s] == proj) map->stage_unit[s] = 0;
            if (map->stage_bump[s] == 0) map->stage_bump[s] = (s8)proj;
            else if (map->stage_bump[s] == proj) map->stage_bump[s] = 0;
        }
        for (int m = 0; m < GX_NUM_TEXMAP; m++) map->texmap_unit[m] = -1;
        for (int u = 0; u < map->unit_count; u++)
            if (map->texmap_unit[map->unit_texmap[u]] < 0) map->texmap_unit[map->unit_texmap[u]] = (s8)u;
    }

    // Water ripple (bug 35): the GameCube offsets the water's screen
    // coordinates per pixel by the caustic texture read at TEX0 (indirect
    // stage 0's coordinate). A spare unit that no stage samples carries that
    // coordinate, so the vertices hold TEX0 and gx_render.c (water_tessellate)
    // computes the offsets per vertex on the CPU
    bool ripple = false;
    for (int s = 0; s < s_cur_n && s_num_ind_stages && s_cur == s_stages; s++)
        if (s_stage_ripple[s] && s_stage_warp[s] > 0.0f) ripple = true;
    if (ripple && map->unit_count < GX_PICA_TEX_UNITS && s_ind_coord[0] < g_gx.num_texgens &&
        s_ind_map[0] < GX_NUM_TEXMAP) {
        int u = map->unit_count++;
        map->unit_texmap[u] = (s8)s_ind_map[0];
        map->unit_texcoord[u] = (s8)s_ind_coord[0];
        map->unit_warp[u] = 0.0f;
        map->ripple_unit = (s8)u;
    }
}

// --- Values -----------------------------------------------------------------------------

enum { V_ZERO, V_ONE, V_CONST, V_TEX, V_RAS, V_REG, V_STAGEPREV };
enum { CH_COLOR = 0, CH_ALPHA = 1 };

typedef struct {
    u8 kind;
    u8 op;           // PICA operand: GPU_TEVOP_RGB_* for color, GPU_TEVOP_A_* for alpha
    u8 reg;          // V_REG: GX register
    u8 regch;        // V_REG: which channel of the register is read
    GPU_TEVSRC tex;  // V_TEX
    u8 cval[4];      // V_CONST
    u8 swap;         // V_TEX / V_RAS: swap table
    u8 csrc[4];      // V_CONST: where each cval byte comes from (KSRC_*)
} Val;

// Source of a constant byte. Register and konst values are not compiled into
// the program: it records where each constant byte comes from and the bytes
// are filled in from the current values whenever it is used (patch_konst)
#define KSRC_LITERAL 0                                 // The byte in cval / the program
#define KSRC_REG(i) (1 + (i))                          // Byte i of s_regs (reg * 4 + comp)
#define KSRC_KCOLOR(i) (1 + 4 * GX_MAX_TEVREG + (i))   // Byte i of s_kcolors
#define KSRC_DROPAVG(i) (1 + 4 * GX_MAX_TEVREG + 4 * GX_MAX_KCOLOR + (i))  // Byte i of s_drop_avg
#define KSRC_HALF 0x80                                 // The value halved

static Val val_kind(u8 kind) {
    Val v = { kind, 0, 0, 0, 0, { 0 } };
    return v;
}

static Val val_const(u8 op, GXColor c) {
    Val v = { V_CONST, op, 0, 0, 0, { c.r, c.g, c.b, c.a } };
    return v;
}

static Val val_reg(u8 reg, u8 regch, u8 op) {
    Val v = { V_REG, op, reg, regch, 0, { 0 } };
    return v;
}

static Val val_src(u8 kind, GPU_TEVSRC tex, u8 op) {
    Val v = { kind, op, 0, 0, tex, { 0 } };
    return v;
}

// Value selected by a konst color/alpha selector
static Val konst_value(u8 op, u8 sel, bool alpha) {
    if (sel <= GX_TEV_KCSEL_1_8) {
        u8 v = (u8)((8 - sel) * 255 / 8);
        return val_const(op, (GXColor){ v, v, v, v });
    }
    Val r = val_const(op, (GXColor){ 255, 255, 255, 255 });
    int k = -1, comp = -1;  // Konst color, and the component splatted (-1 = none)
    if (sel >= GX_TEV_KCSEL_K0 && sel <= GX_TEV_KCSEL_K3) {
        k = sel - GX_TEV_KCSEL_K0;
        if (alpha) comp = 3;
    } else if (sel >= GX_TEV_KCSEL_K0_R && sel <= GX_TEV_KCSEL_K3_A) {
        k = (sel - GX_TEV_KCSEL_K0_R) & 3;
        comp = (sel - GX_TEV_KCSEL_K0_R) >> 2;
    }
    if (k < 0) return r;
    for (int i = 0; i < 4; i++) {
        int b = k * 4 + (comp < 0 ? i : comp);
        r.cval[i] = ((const u8*)s_kcolors)[b];
        r.csrc[i] = KSRC_KCOLOR(b);
    }
    return r;
}

typedef struct {
    GPU_TEVSRC tex;   // Texture source of this stage, or 0 if none
    GPU_TEVSRC bump;  // Texture read as bump alpha (RASA), or 0
    u8 bump_op;       // Its alpha operand (component)
    bool ras_zero;    // Rasterized color is zero
    bool ras_sec;     // Rasterized color is channel 1 from fragment lighting
    u8 kcsel, kasel;
    u8 ras_swap, tex_swap;
    u8 bump_cop;  // The bump component as a colour operand (colour RASA); last: positional initializers
} stage_ctx_t;

static Val with_swap(Val v, const stage_ctx_t* ctx) {
    if (v.kind == V_TEX) v.swap = ctx->tex_swap;
    if (v.kind == V_RAS) v.swap = ctx->ras_swap;
    return v;
}

static Val decode_color_arg_raw(u8 arg, const stage_ctx_t* ctx) {
    switch (arg) {
        case GX_CC_CPREV: return val_reg(GX_TEVPREV, CH_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        case GX_CC_APREV: return val_reg(GX_TEVPREV, CH_ALPHA, GPU_TEVOP_RGB_SRC_ALPHA);
        case GX_CC_C0:    return val_reg(GX_TEVREG0, CH_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        case GX_CC_A0:    return val_reg(GX_TEVREG0, CH_ALPHA, GPU_TEVOP_RGB_SRC_ALPHA);
        case GX_CC_C1:    return val_reg(GX_TEVREG1, CH_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        case GX_CC_A1:    return val_reg(GX_TEVREG1, CH_ALPHA, GPU_TEVOP_RGB_SRC_ALPHA);
        case GX_CC_C2:    return val_reg(GX_TEVREG2, CH_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        case GX_CC_A2:    return val_reg(GX_TEVREG2, CH_ALPHA, GPU_TEVOP_RGB_SRC_ALPHA);
        case GX_CC_TEXC:  return ctx->tex ? val_src(V_TEX, ctx->tex, GPU_TEVOP_RGB_SRC_COLOR) : val_kind(V_ONE);
        case GX_CC_TEXA:  return ctx->tex ? val_src(V_TEX, ctx->tex, GPU_TEVOP_RGB_SRC_ALPHA) : val_kind(V_ONE);
        case GX_CC_RASC:  return ctx->ras_sec ? val_src(V_RAS, GPU_FRAGMENT_SECONDARY_COLOR, GPU_TEVOP_RGB_SRC_COLOR)
                               : ctx->ras_zero ? val_kind(V_ZERO) : val_src(V_RAS, 0, GPU_TEVOP_RGB_SRC_COLOR);
        case GX_CC_RASA:
            if (ctx->bump) return val_src(V_TEX, ctx->bump, ctx->bump_cop);
            return ctx->ras_zero ? val_kind(V_ZERO) : val_src(V_RAS, 0, GPU_TEVOP_RGB_SRC_ALPHA);
        case GX_CC_ONE:   return val_kind(V_ONE);
        case GX_CC_HALF:  return val_const(GPU_TEVOP_RGB_SRC_COLOR, (GXColor){ 128, 128, 128, 128 });
        case GX_CC_KONST: return konst_value(GPU_TEVOP_RGB_SRC_COLOR, ctx->kcsel, false);
        default:          return val_kind(V_ZERO);
    }
}

static Val decode_color_arg(u8 arg, const stage_ctx_t* ctx) {
    return with_swap(decode_color_arg_raw(arg, ctx), ctx);
}

static Val decode_alpha_arg_raw(u8 arg, const stage_ctx_t* ctx) {
    switch (arg) {
        case GX_CA_APREV: return val_reg(GX_TEVPREV, CH_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
        case GX_CA_A0:    return val_reg(GX_TEVREG0, CH_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
        case GX_CA_A1:    return val_reg(GX_TEVREG1, CH_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
        case GX_CA_A2:    return val_reg(GX_TEVREG2, CH_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
        case GX_CA_TEXA:  return ctx->tex ? val_src(V_TEX, ctx->tex, GPU_TEVOP_A_SRC_ALPHA) : val_kind(V_ONE);
        case GX_CA_RASA:  return ctx->ras_zero ? val_kind(V_ZERO) : val_src(V_RAS, 0, GPU_TEVOP_A_SRC_ALPHA);
        case GX_CA_KONST: return konst_value(GPU_TEVOP_A_SRC_ALPHA, ctx->kasel, true);
        default:          return val_kind(V_ZERO);
    }
}

static Val decode_alpha_arg(u8 arg, const stage_ctx_t* ctx) {
    if (arg == GX_CA_RASA && ctx->bump) return val_src(V_TEX, ctx->bump, ctx->bump_op);
    return with_swap(decode_alpha_arg_raw(arg, ctx), ctx);
}

static Val invert(Val v) {
    if (v.kind == V_ZERO) return val_kind(V_ONE);
    if (v.kind == V_ONE) return val_kind(V_ZERO);
    v.op ^= 1;  // Every PICA operand has its "1 - x" variant at op | 1
    return v;
}

// --- Plans: a GX stage channel as one to four PICA combiner steps -------------------

typedef struct {
    GPU_COMBINEFUNC func;
    Val src[3];
    u8 nsrc;
    GPU_TEVSCALE scale;
} Step;

#define PLAN_MAX_STEPS 4

typedef struct {
    Step step[PLAN_MAX_STEPS];
    u8 n;
    bool own_scale;  // Steps carry their own scales; the GX stage scale is not applied
} Plan;

static Step step1(GPU_COMBINEFUNC f, Val a) { Step s = { f, { a }, 1 }; return s; }
static Step step2(GPU_COMBINEFUNC f, Val a, Val b) { Step s = { f, { a, b }, 2 }; return s; }
static Step step3(GPU_COMBINEFUNC f, Val a, Val b, Val c) { Step s = { f, { a, b, c }, 3 }; return s; }

// d +/- lerp(a, b, c), with lerp = a*(1-c) + b*c. With allow_two, cases that
// need two PICA functions are split; otherwise they are approximated.
static void build_arith_plan(const Val in[4], u8 op, u8 bias, bool allow_two, Plan* out) {
    Val a = in[0], b = in[1], c = in[2], d = in[3];
    Val zero = val_kind(V_ZERO);
    Val prev = val_kind(V_STAGEPREV);
    bool sub = op == GX_TEV_SUB;

    // Reduce lerp(a, b, c) to a single term
    enum { T_ZERO, T_ONE_VAL, T_MUL, T_LERP } term;
    Val t0 = zero, t1 = zero, t2 = zero;
    if (c.kind == V_ZERO) { term = T_ONE_VAL; t0 = a; }
    else if (c.kind == V_ONE) { term = T_ONE_VAL; t0 = b; }
    else if (a.kind == V_ZERO && b.kind == V_ZERO) { term = T_ZERO; }
    else if (a.kind == V_ZERO) {
        if (b.kind == V_ONE) { term = T_ONE_VAL; t0 = c; }
        else { term = T_MUL; t0 = b; t1 = c; }
    } else if (b.kind == V_ZERO) { term = T_MUL; t0 = a; t1 = invert(c); }
    else { term = T_LERP; t0 = b; t1 = a; t2 = c; }  // b*c + a*(1-c)
    if (term == T_ONE_VAL && t0.kind == V_ZERO) term = T_ZERO;

    out->n = 1;
    GPU_COMBINEFUNC add = (bias == GX_TB_SUBHALF) ? GPU_ADD_SIGNED : GPU_ADD;

    if (sub && bias == GX_TB_ADDHALF && term != T_ZERO) {
        // d - x + 0.5 = ADD_SIGNED(d, 1 - x), exact
        if (term == T_ONE_VAL) {
            out->step[0] = step2(GPU_ADD_SIGNED, d, invert(t0));
        } else if (allow_two) {
            out->n = 2;
            out->step[0] = term == T_MUL ? step2(GPU_MODULATE, t0, t1) : step3(GPU_INTERPOLATE, t0, t1, t2);
            out->step[1] = step2(GPU_ADD_SIGNED, d, invert(prev));
        } else {
            out->step[0] = step1(GPU_REPLACE, d);  // d - x*y + 0.5 approximated as d
        }
        return;
    }

    if (term == T_ZERO) {
        out->step[0] = step1(GPU_REPLACE, d);
    } else if (d.kind == V_ZERO) {
        if (sub)                  out->step[0] = step1(GPU_REPLACE, zero);  // 0 - x clamps to 0
        else if (term == T_ONE_VAL) out->step[0] = step1(GPU_REPLACE, t0);
        else if (term == T_MUL)   out->step[0] = step2(GPU_MODULATE, t0, t1);
        else                      out->step[0] = step3(GPU_INTERPOLATE, t0, t1, t2);
    } else if (term == T_ONE_VAL) {
        out->step[0] = sub ? step2(GPU_SUBTRACT, d, t0) : step2(add, t0, d);
    } else if (term == T_MUL && !sub) {
        out->step[0] = step3(GPU_MULTIPLY_ADD, t0, t1, d);
    } else if (allow_two) {
        // Two steps: compute the product/lerp, then combine with d
        out->n = 2;
        out->step[0] = term == T_MUL ? step2(GPU_MODULATE, t0, t1) : step3(GPU_INTERPOLATE, t0, t1, t2);
        out->step[1] = sub ? step2(GPU_SUBTRACT, d, prev) : step2(add, prev, d);
    } else if (term == T_MUL) {
        out->step[0] = step1(GPU_REPLACE, d);  // d - x*y approximated as d
    } else {
        out->step[0] = step3(GPU_INTERPOLATE, t0, t1, t2);  // lerp + d: d dropped
    }
}

static Val half_val(int ch) {
    return val_const(ch == CH_COLOR ? GPU_TEVOP_RGB_SRC_COLOR : GPU_TEVOP_A_SRC_ALPHA, (GXColor){ 128, 128, 128, 128 });
}

// Compare ops: d + (a > b ? c : 0) (or ==). R8 compares red, GR16 / BGR24
// compare 16 / 24-bit values (approximated by their high byte), RGB8
// compares per component and, in the alpha channel, A8 compares alpha.
// GT is a step function of a - b: the difference is at least 1/255 when
// a > b, so a clamped gain of 256 (x4, then ADD(prev, prev) at x4 twice)
// turns it into exactly 0 or 1. EQ needs |a - b|, which takes two
// temporaries the combiners do not have, so it is approximated by d.
static void build_compare_plan(const Val in[4], u8 op, int ch, bool allow_two, Plan* out) {
    Val a = in[0], b = in[1], c = in[2], d = in[3];
    Val prev = val_kind(V_STAGEPREV);
    bool eq = (op & 1) != 0;
    out->own_scale = true;

    if (ch == CH_COLOR && op < GX_TEV_COMP_RGB8_GT) {
        // Compare one channel, replicated to all three
        static const u8 chan_op[3] = { GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B };
        u8 cop = chan_op[(op - GX_TEV_COMP_R8_GT) >> 1];
        if (a.op == GPU_TEVOP_RGB_SRC_COLOR) a.op = cop;
        if (b.op == GPU_TEVOP_RGB_SRC_COLOR) b.op = cop;
    }

    bool c_one = c.kind == V_ONE, d_zero = d.kind == V_ZERO;
    if (c.kind == V_ZERO || eq || !allow_two) {
        // Approximations: d alone, or a soft step of a - b when d is zero
        out->n = 1;
        if (c.kind != V_ZERO && !eq && d_zero) {
            out->step[0] = step2(GPU_SUBTRACT, a, b);
            out->step[0].scale = GPU_TEVSCALE_4;
            if (!c_one) out->own_scale = false;
        } else {
            out->step[0] = step1(GPU_REPLACE, d);
        }
        if (c.kind != V_ZERO) PROF_COUNT("tev.compare_approx", 1);
        return;
    }

    int n = 0;
    out->step[n] = step2(GPU_SUBTRACT, a, b);
    out->step[n++].scale = GPU_TEVSCALE_4;
    out->step[n] = step2(GPU_ADD, prev, prev);
    out->step[n++].scale = GPU_TEVSCALE_4;
    out->step[n] = step2(GPU_ADD, prev, prev);
    out->step[n++].scale = GPU_TEVSCALE_4;
    if (!(c_one && d_zero)) {
        if (d_zero)     out->step[n++] = step2(GPU_MODULATE, prev, c);
        else if (c_one) out->step[n++] = step2(GPU_ADD, prev, d);
        else            out->step[n++] = step3(GPU_MULTIPLY_ADD, prev, c, d);
    }
    out->n = n;
}

// d +/- lerp(a, b, c) + bias, then the stage scale. DIVIDE_2 has no PICA
// scale: (x + d) / 2 is lerp(x, d, 1/2) in one step; otherwise the result is
// halved by a following step (exact except that the PICA clamps the
// intermediate value to 1 before halving).
// A PICA stage has one constant colour, so a lerp between two constants
// keeps only one of them (the last resolved). Bug 37: the eye glow is
// lerp(K, noise stand-in, bump) with K = 282000 (near black): approximate the
// darker constant as black, lerp(a, b, c) -> b * c, or a * (1 - c). Splitting
// it into two stages would push the program's last buffer read to stage 5,
// which the console does not honour (bug 26). Decided from the values at
// compile time; the kept constant is still patched on every use
static void fix_const_lerps(Plan* p) {
    for (int k = 0; k < p->n; k++) {
        Step* st = &p->step[k];
        if (st->func != GPU_INTERPOLATE || st->src[0].kind != V_CONST || st->src[1].kind != V_CONST) continue;
        const Val* a = &st->src[0];
        const Val* b = &st->src[1];
        if (memcmp(a->cval, b->cval, 4) == 0 && memcmp(a->csrc, b->csrc, 4) == 0 && a->op == b->op) continue;
        int sa = a->cval[0] + a->cval[1] + a->cval[2], sb = b->cval[0] + b->cval[1] + b->cval[2];
        // INTERPOLATE(s0, s1, s2) = s0 * s2 + s1 * (1 - s2)
        Val keep = sa >= sb ? *a : *b;
        Val w = sa >= sb ? st->src[2] : invert(st->src[2]);
        *st = (Step){ GPU_MODULATE, { keep, w }, 2, st->scale };
        PROF_COUNT("tev.const_lerps", 1);
    }
}

static void build_plan(const Val in[4], u8 op, u8 bias, u8 scale, int ch, bool allow_two, Plan* out) {
    out->own_scale = false;
    if (op >= GX_TEV_COMP_R8_GT && op <= GX_TEV_COMP_RGB8_EQ) {
        build_compare_plan(in, op, ch, allow_two, out);
        return;
    }
    build_arith_plan(in, op, bias, allow_two, out);
    if (scale != GX_CS_DIVIDE_2) return;

    Val half = half_val(ch);
    Step* last = &out->step[out->n - 1];
    const Val* d = &in[3];
    if (op == GX_TEV_ADD && bias == GX_TB_ZERO && out->n == 1 && last->func == GPU_ADD && d->kind != V_ZERO) {
        // ADD(t, d) -> INTERPOLATE(t, d, 1/2)
        *last = step3(GPU_INTERPOLATE, last->src[0], last->src[1], half);
    } else if (last->func == GPU_REPLACE) {
        Val x = last->src[0];
        if (x.kind == V_ZERO) return;
        if (x.kind == V_ONE) *last = step1(GPU_REPLACE, half);
        else if (x.kind == V_CONST) {
            for (int i = 0; i < 4; i++) {
                x.cval[i] = (u8)(x.cval[i] >> 1);
                if (x.csrc[i] != KSRC_LITERAL) x.csrc[i] |= KSRC_HALF;
            }
            *last = step1(GPU_REPLACE, x);
        } else {
            *last = step2(GPU_MODULATE, x, half);
        }
    } else if (allow_two && out->n < PLAN_MAX_STEPS) {
        out->step[out->n++] = step2(GPU_MODULATE, val_kind(V_STAGEPREV), half);
    } else {
        PROF_COUNT("tev.divide2_approx", 1);
    }
}

// --- Compiler -----------------------------------------------------------------------------

enum { L_CONST, L_PREV, L_BUF, L_LOST };  // Where a GX register's value lives

typedef struct {
    GPU_COMBINEFUNC func[2];   // [channel]
    GPU_TEVSRC src[2][3];
    u8 op[2][3];
    u8 konst[4];
    u8 konst_src[4];  // KSRC_* per byte of konst
    bool konst_used[2];
    GPU_TEVSCALE scale[2];
} PicaStage;

typedef struct {
    PicaStage out[GX_PICA_TEV_STAGES];
    int nout;
    bool overflow;
    u8 loc[GX_MAX_TEVREG][2];
    s8 prev_holds[2];   // GX register whose value is the previous output, -1 = none/temp
    s8 buf_holds[2];    // GX register latched in the combiner buffer, -1 = none
    s8 last_writer[2];  // PICA stage that produced the previous output
    s8 buf_last_read[2];
    s8 buf_latch[2];    // PICA stage that last latched the buffer (seen from stage + 2)
    u8 buf_mask[2];    // PICA stages (0-3) that update the buffer
    const GxUnitMap* map;
    // Per GX stage: plans and contexts
    Plan plan[GX_MAX_TEVSTAGE][2];
    bool passthru[GX_MAX_TEVSTAGE][2];  // Channel keeps its output register (texture dropped)
    // Bug 37: writes whose value is lost before any read (compile_pruned):
    // they emit nothing, and the register reads as lost afterwards
    bool lost_write[GX_MAX_TEVSTAGE][2];
    bool used[GX_MAX_TEVSTAGE][2];  // The stage channel's value was read from PREV / the buffer
    s8 producer[GX_MAX_TEVREG][2];  // GX stage whose result a register holds, -1 = none
    int nstages;
} Compiler;

static bool val_reads(const Val* v, u8 reg, u8 ch) {
    return v->kind == V_REG && v->reg == reg && v->regch == ch;
}

static bool step_reads(const Step* st, u8 reg, u8 ch) {
    for (int i = 0; i < st->nsrc; i++) {
        if (val_reads(&st->src[i], reg, ch)) return true;
    }
    return false;
}

// Is GX register (reg, ch) read before being overwritten, looking from PICA
// step 'from_step' of GX stage 'from_stage' onwards? PREV is always needed at
// the end (it is the final color).
static bool needed(const Compiler* c, u8 reg, u8 ch, int from_stage, int from_step) {
    for (int s = from_stage; s < c->nstages; s++) {
        int first = s == from_stage ? from_step : 0;
        for (int k = first; k < PLAN_MAX_STEPS; k++) {
            for (int chan = 0; chan < 2; chan++) {
                const Plan* p = &c->plan[s][chan];
                if (k < p->n && step_reads(&p->step[k], reg, ch)) return true;
            }
        }
        // A passthru channel rewrites its register with its own value
        const tev_stage_t* st = &s_cur[s];
        if (!c->passthru[s][ch] && (ch == CH_COLOR ? st->color_reg : st->alpha_reg) == reg) return false;
    }
    return reg == GX_TEVPREV;
}

// Keep the previous output's value (register r, channel ch) by latching it
// into the combiner buffer from the PICA stage that produced it
static void save_or_lose(Compiler* c, int ch, int cur_stage, int cur_step) {
    s8 r = c->prev_holds[ch];
    if (r < 0 || c->loc[r][ch] != L_PREV) return;
    c->prev_holds[ch] = -1;
    // Read only by the step about to be emitted: it reads GPU_PREVIOUS, and
    // the value is invalidated once the step is emitted
    if (!needed(c, r, ch, cur_stage, cur_step)) return;

    int p = c->last_writer[ch];
    s8 old = c->buf_holds[ch];
    bool buffer_free = old < 0 || c->loc[old][ch] != L_BUF || !needed(c, old, ch, cur_stage, cur_step);
    // Latching at stage p changes what stages p+1.. read from the buffer, so
    // nothing after p may have read it already
    if (p >= 0 && p <= 3 && buffer_free && c->buf_last_read[ch] <= p) {
        c->buf_mask[ch] |= 1 << p;
        if (old >= 0 && c->loc[old][ch] == L_BUF) c->loc[old][ch] = L_LOST;
        c->buf_holds[ch] = r;
        c->buf_latch[ch] = p;
        c->loc[r][ch] = L_BUF;
        PROF_COUNT("tev.buffer_saves", 1);
    } else {
        c->loc[r][ch] = L_LOST;
        PROF_COUNT("tev.values_lost", 1);
    }
}

// Operand reading a texture / rasterized color through a swap table and a
// texture's channel remapping (GX_SWZ, for EFB copies). PICA operands can read
// the color, the alpha, or one channel replicated, so swap tables that
// replicate one channel (and all alpha selections) are exact.
static u8 channel_op(const u8 table[4], u8 swz, int ch, u8 op) {
    if (op > 3 || (ch == CH_ALPHA && op > 1)) return op;
    // PICA channel (0-3 = R, G, B, A) holding each GX channel
    u8 base[4] = { 0, 1, 2, 3 };
    if (swz) {
        static const u8 color_ch[5] = { 0xFF, 0, 1, 2, 3 };
        static const u8 alpha_ch[4] = { 3, 0, 1, 2 };
        u8 c = color_ch[GX_SWZ_COLOR(swz)];
        if (c != 0xFF) base[0] = base[1] = base[2] = c;
        base[3] = alpha_ch[GX_SWZ_ALPHA(swz)];
    }
    u8 eff[4];
    for (int i = 0; i < 4; i++) eff[i] = base[table[i] & 3];
    u8 inv = (op & 1) ^ GX_SWZ_INVERT(swz);

    static const u8 rgb_from[4] = { GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B,
                                    GPU_TEVOP_RGB_SRC_ALPHA };
    static const u8 a_from[4] = { GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_B, GPU_TEVOP_A_SRC_ALPHA };
    if (ch == CH_ALPHA) return a_from[eff[3]] | inv;
    if (op >= 2) return rgb_from[eff[3]] | inv;  // Alpha replicated as color
    if (eff[0] == 0 && eff[1] == 1 && eff[2] == 2) return GPU_TEVOP_RGB_SRC_COLOR | inv;
    if (eff[0] == eff[1] && eff[1] == eff[2]) return rgb_from[eff[0]] | inv;
    PROF_COUNT("tev.swap_unsupported", 1);
    return GPU_TEVOP_RGB_SRC_COLOR | inv;
}

// Map a value to a PICA source for channel ch of PICA stage ps
static void resolve(Compiler* c, PicaStage* ps, int ch, const Val* v, bool addend, GPU_TEVSRC* src, u8* op) {
    *op = v->op;
    switch (v->kind) {
        case V_TEX:
            *src = v->tex;
            *op = channel_op(s_swap_tables[v->swap], c->map->unit_swizzle[v->tex - GPU_TEXTURE0], ch, v->op);
            return;
        case V_RAS:
            *src = v->tex ? v->tex : GPU_PRIMARY_COLOR;
            *op = channel_op(s_swap_tables[v->swap], 0, ch, v->op);
            return;
        case V_STAGEPREV: *src = GPU_PREVIOUS; return;
        default: break;
    }

    u8 cval[4], csrc[4] = { KSRC_LITERAL, KSRC_LITERAL, KSRC_LITERAL, KSRC_LITERAL };
    if (v->kind == V_REG) {
        u8 l = c->loc[v->reg][v->regch];
        if ((l == L_PREV || l == L_BUF) && c->producer[v->reg][v->regch] >= 0)
            c->used[c->producer[v->reg][v->regch]][v->regch] = true;
        if (l == L_PREV) { *src = GPU_PREVIOUS; return; }
        if (l == L_BUF) {
            // The buffer lags one stage: a value latched at stage p is read
            // from p + 2 on, and stage p + 1 still finds it in the previous output
            if (c->nout == c->buf_latch[v->regch] + 1) { *src = GPU_PREVIOUS; return; }
            *src = GPU_PREVIOUS_BUFFER;
            if (c->nout > c->buf_last_read[v->regch]) c->buf_last_read[v->regch] = c->nout;
            return;
        }
        // Constant (initial register value) or lost (approximated by it, or
        // by zero when it is only added)
        GXColor r = s_regs[v->reg];
        if (l == L_LOST && addend) r = (GXColor){ 0, 0, 0, 0 };
        cval[0] = r.r; cval[1] = r.g; cval[2] = r.b; cval[3] = r.a;
        if (!(l == L_LOST && addend))
            for (int i = 0; i < 4; i++) csrc[i] = KSRC_REG(v->reg * 4 + i);
        if (l == L_LOST) PROF_COUNT("tev.lost_reads", 1);
    } else if (v->kind == V_CONST) {
        memcpy(cval, v->cval, 4);
        memcpy(csrc, v->csrc, 4);
    } else {
        u8 x = v->kind == V_ONE ? 0xFF : 0x00;
        cval[0] = cval[1] = cval[2] = cval[3] = x;
        *op = 0;
    }

    // One constant color per PICA stage: conflicts keep the last value
    *src = GPU_CONSTANT;
    if (ch == CH_ALPHA) {
        ps->konst[3] = cval[3];
        ps->konst_src[3] = csrc[3];
    } else {
        for (int i = 0; i < 3; i++) {
            ps->konst[i] = cval[i];
            ps->konst_src[i] = csrc[i];
        }
        if (*op == GPU_TEVOP_RGB_SRC_ALPHA || *op == GPU_TEVOP_RGB_ONE_MINUS_SRC_ALPHA) {
            ps->konst[3] = cval[3];
            ps->konst_src[3] = csrc[3];
        }
    }
    ps->konst_used[ch] = true;
}

static GPU_TEVSCALE map_scale(u8 scale) {
    switch (scale) {
        case GX_CS_SCALE_2: return GPU_TEVSCALE_2;
        case GX_CS_SCALE_4: return GPU_TEVSCALE_4;
        default:            return GPU_TEVSCALE_1;  // DIVIDE_2 is a plan step
    }
}

static PicaStage* new_stage(Compiler* c) {
    if (c->nout >= GX_PICA_TEV_STAGES) {
        c->overflow = true;
        return NULL;
    }
    PicaStage* ps = &c->out[c->nout];
    memset(ps, 0, sizeof(*ps));
    for (int ch = 0; ch < 2; ch++) {
        ps->func[ch] = GPU_REPLACE;  // Pass-through of the previous output
        ps->src[ch][0] = ps->src[ch][1] = ps->src[ch][2] = GPU_PREVIOUS;
        ps->scale[ch] = GPU_TEVSCALE_1;
    }
    ps->konst[0] = ps->konst[1] = ps->konst[2] = ps->konst[3] = 0xFF;
    return ps;
}

static void emit_step(Compiler* c, PicaStage* ps, int ch, const Step* st) {
    ps->func[ch] = st->func;
    for (int i = 0; i < 3; i++) {
        if (i < st->nsrc) {
            bool addend = st->func == GPU_ADD || (st->func == GPU_MULTIPLY_ADD && i == 2);
            resolve(c, ps, ch, &st->src[i], addend, &ps->src[ch][i], &ps->op[ch][i]);
        } else {
            ps->src[ch][i] = GPU_PRIMARY_COLOR;
            ps->op[ch][i] = 0;
        }
    }
}

// The register channel a stage channel copies unchanged (one plain REPLACE of
// the same channel at scale 1), or NULL
static const Val* copy_source(const Compiler* c, int s, int ch) {
    const Plan* p = &c->plan[s][ch];
    const tev_stage_t* st = &s_cur[s];
    const u8 scale = ch == CH_COLOR ? st->color_scale : st->alpha_scale;
    if (c->passthru[s][ch] || p->n != 1 || p->own_scale || scale != GX_CS_SCALE_1) return NULL;
    const Step* s0 = &p->step[0];
    const Val* v = &s0->src[0];
    const u8 plain = ch == CH_COLOR ? GPU_TEVOP_RGB_SRC_COLOR : GPU_TEVOP_A_SRC_ALPHA;
    if (s0->func != GPU_REPLACE || v->kind != V_REG || v->regch != ch || v->op != plain) return NULL;
    return v;
}

static void compile(Compiler* c, const GxUnitMap* map, bool allow_two) {
    c->map = map;
    c->nout = 0;
    c->overflow = false;
    memset(c->loc, L_CONST, sizeof(c->loc));
    memset(c->used, 0, sizeof(c->used));
    memset(c->producer, -1, sizeof(c->producer));
    for (int ch = 0; ch < 2; ch++) {
        c->prev_holds[ch] = -1;
        c->buf_holds[ch] = -1;
        c->last_writer[ch] = -1;
        c->buf_last_read[ch] = -1;
        c->buf_latch[ch] = -1;
        c->buf_mask[ch] = 0;
    }
    c->nstages = s_cur_n;
    bool ch1 = gx_tev_ras_channel() == 1;

    // Bug 26: a blur sums N taps into PREV and halves the last stage
    // (DIVIDE_2). The halving costs a PICA stage, which pushed the 5-tap
    // combat depth-of-field over 6 and the approximation dropped it (blur 2x
    // too bright). The sum is linear, so halve every tap's konst weight
    // instead: exact, no extra stage. S3D_TEV_HALVE_TAPS=0 turns it off
    static int halve_on = -1;
    if (halve_on < 0) halve_on = cfg_bool("S3D_TEV_HALVE_TAPS", true) ? 1 : 0;
    u32 halve = 0;
    for (int last = c->nstages - 1; halve_on && last >= 0; last--) {
        if (s_cur[last].color_scale != GX_CS_DIVIDE_2) continue;
        u32 m = 0;
        for (int j = last; j >= 0; j--) {
            const tev_stage_t* t = &s_cur[j];
            bool tap = stage_is_weighted_tap(t) && t->color_bias == GX_TB_ZERO && t->color_reg == GX_TEVPREV &&
                       t->color_scale == (j == last ? GX_CS_DIVIDE_2 : GX_CS_SCALE_1) &&
                       (t->color_in[1] == GX_CC_KONST || t->color_in[2] == GX_CC_KONST);
            if (!tap) break;
            m |= 1u << j;
            if (t->color_in[3] == GX_CC_ZERO) {  // Start of the sum
                halve = m;
                break;
            }
            if (t->color_in[3] != GX_CC_CPREV) break;
        }
        break;
    }
    if (halve) PROF_COUNT("tev.halved_taps", 1);

    // Plans for every GX stage first (liveness looks ahead)
    for (int s = 0; s < c->nstages; s++) {
        const tev_stage_t* st = &s_cur[s];
        stage_ctx_t ctx = { 0, 0, 0, false, false, st->kcsel, st->kasel, st->ras_swap, st->tex_swap };
        if (map->stage_bump[s] >= 0) {
            static const u8 comp[4] = { GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_B, GPU_TEVOP_A_SRC_G };
            ctx.bump = (GPU_TEVSRC)(GPU_TEXTURE0 + map->stage_bump[s]);
            ctx.bump_op = comp[s_stage_bump_sel[s] & 3];
            static const u8 ccomp[4] = { GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_ALPHA, GPU_TEVOP_RGB_SRC_B,
                                         GPU_TEVOP_RGB_SRC_G };
            ctx.bump_cop = ccomp[s_stage_bump_sel[s] & 3];
        }
        if (st->texmap < GX_NUM_TEXMAP && map->stage_unit[s] >= 0) {
            ctx.tex = (GPU_TEVSRC)(GPU_TEXTURE0 + map->stage_unit[s]);
        }
        // Only one channel is computed (see gx_tev_ras_channel); the other reads as zero
        bool on1 = st->channel == GX_COLOR1 || st->channel == GX_ALPHA1 || st->channel == GX_COLOR1A1;
        ctx.ras_zero = st->channel == GX_COLOR_ZERO || st->channel == GX_COLOR_NULL || on1 != ch1;
        ctx.ras_sec = s_frag_sec && on1 && !ch1;
        Val cin[4], ain[4];
        for (int i = 0; i < 4; i++) {
            cin[i] = decode_color_arg(st->color_in[i], &ctx);
            ain[i] = decode_alpha_arg(st->alpha_in[i], &ctx);
        }
        // A texture dropped for lack of units: a channel that samples it
        // keeps its output register unchanged (reading it as white would
        // wash the surface out)
        if (!ctx.tex && st->texmap < GX_NUM_TEXMAP && st->texmap == map->avg_map && stage_uses_texture(st)) {
            // Bug 37: the dropped texture reads as its average colour,
            // patched in on every use (gx_tev_set_drop_avg)
            Val avg = val_const(GPU_TEVOP_RGB_SRC_COLOR, (GXColor){ s_drop_avg[0], s_drop_avg[1], s_drop_avg[2], s_drop_avg[3] });
            for (int b = 0; b < 4; b++) avg.csrc[b] = KSRC_DROPAVG(b);
            for (int i = 0; i < 4; i++) {
                if (st->color_in[i] == GX_CC_TEXC) cin[i] = avg;
                if (st->color_in[i] == GX_CC_TEXA) { cin[i] = avg; cin[i].op = GPU_TEVOP_RGB_SRC_ALPHA; }
            }
            c->passthru[s][CH_COLOR] = c->passthru[s][CH_ALPHA] = false;
            PROF_COUNT("tev.drop_avgs", 1);
        } else if (!ctx.tex && st->texmap < GX_NUM_TEXMAP && stage_uses_texture(st)) {
            static const u8 creg_arg[GX_MAX_TEVREG] = { GX_CC_CPREV, GX_CC_C0, GX_CC_C1, GX_CC_C2 };
            static const u8 areg_arg[GX_MAX_TEVREG] = { GX_CA_APREV, GX_CA_A0, GX_CA_A1, GX_CA_A2 };
            bool ctex = false, atex = false;
            for (int i = 0; i < 4; i++) {
                ctex |= st->color_in[i] == GX_CC_TEXC || st->color_in[i] == GX_CC_TEXA;
                atex |= st->alpha_in[i] == GX_CA_TEXA;
            }
            // A plain modulation (tex * x) reads the texture as white instead,
            // which keeps x when the output goes to another register
            bool cmod = ctex && st->color_op == GX_TEV_ADD && st->color_in[0] == GX_CC_ZERO &&
                        st->color_in[3] == GX_CC_ZERO && !is_tex_carg(st->color_in[1]) != !is_tex_carg(st->color_in[2]);
            bool amod = atex && st->alpha_op == GX_TEV_ADD && st->alpha_in[0] == GX_CA_ZERO &&
                        st->alpha_in[3] == GX_CA_ZERO && (st->alpha_in[1] == GX_CA_TEXA) != (st->alpha_in[2] == GX_CA_TEXA);
            // An added term (d + tex * x) drops the product instead, which
            // keeps d: the fur's REG1 = RASC + TEX1 * C2 would otherwise keep
            // the stale register value (green on the death screen)
            bool cadd = ctex && !cmod && st->color_op == GX_TEV_ADD && st->color_in[0] == GX_CC_ZERO &&
                        !is_tex_carg(st->color_in[3]) && !is_tex_carg(st->color_in[1]) != !is_tex_carg(st->color_in[2]);
            bool aadd = atex && !amod && st->alpha_op == GX_TEV_ADD && st->alpha_in[0] == GX_CA_ZERO &&
                        st->alpha_in[3] != GX_CA_TEXA && (st->alpha_in[1] == GX_CA_TEXA) != (st->alpha_in[2] == GX_CA_TEXA);
            if (cmod) {
                cin[is_tex_carg(st->color_in[1]) ? 1 : 2] = val_kind(V_ONE);
                ctex = false;
            }
            if (amod) {
                ain[st->alpha_in[1] == GX_CA_TEXA ? 1 : 2] = val_kind(V_ONE);
                atex = false;
            }
            if (cadd) {
                cin[1] = cin[2] = val_kind(V_ZERO);
                ctex = false;
            }
            if (aadd) {
                ain[1] = ain[2] = val_kind(V_ZERO);
                atex = false;
            }
            if (ctex) {
                cin[0] = cin[1] = cin[2] = val_kind(V_ZERO);
                cin[3] = decode_color_arg(creg_arg[st->color_reg], &ctx);
            }
            if (atex) {
                ain[0] = ain[1] = ain[2] = val_kind(V_ZERO);
                ain[3] = decode_alpha_arg(areg_arg[st->alpha_reg], &ctx);
            }
            c->passthru[s][CH_COLOR] = ctex;
            c->passthru[s][CH_ALPHA] = atex;
            PROF_COUNT("tev.dropped_tex_stages", 1);
        } else {
            c->passthru[s][CH_COLOR] = c->passthru[s][CH_ALPHA] = false;
        }
        u8 cscale = st->color_scale;
        if (halve & (1u << s)) {
            cscale = GX_CS_SCALE_1;
            for (int i = 1; i <= 2; i++) {
                if (cin[i].kind != V_CONST) continue;
                for (int b = 0; b < 4; b++) {
                    cin[i].cval[b] = (u8)(cin[i].cval[b] >> 1);
                    if (cin[i].csrc[b] != KSRC_LITERAL) cin[i].csrc[b] |= KSRC_HALF;
                }
            }
        }
        build_plan(cin, c->passthru[s][CH_COLOR] ? GX_TEV_ADD : st->color_op,
                   c->passthru[s][CH_COLOR] ? GX_TB_ZERO : st->color_bias,
                   c->passthru[s][CH_COLOR] ? GX_CS_SCALE_1 : cscale, CH_COLOR, allow_two, &c->plan[s][CH_COLOR]);
        build_plan(ain, c->passthru[s][CH_ALPHA] ? GX_TEV_ADD : st->alpha_op,
                   c->passthru[s][CH_ALPHA] ? GX_TB_ZERO : st->alpha_bias,
                   c->passthru[s][CH_ALPHA] ? GX_CS_SCALE_1 : st->alpha_scale, CH_ALPHA, allow_two, &c->plan[s][CH_ALPHA]);
        fix_const_lerps(&c->plan[s][CH_COLOR]);
        fix_const_lerps(&c->plan[s][CH_ALPHA]);
        // A passthru channel rewrites its register with its own value: emit
        // nothing for it, so stages that only sampled dropped textures cost
        // no PICA stages (the eye and fur programs fit in 6 that way)
        for (int ch = 0; ch < 2; ch++) {
            if (c->passthru[s][ch]) c->plan[s][ch].n = 0;
        }
    }

    // Bug 37: Krystal's eyes (8 GX stages, 5 textures) dropped map1, whose
    // modulation then reads it as white: REG2 = C1 became a plain copy that
    // took a PICA stage, the program overflowed 6 and the truncation cut the
    // last stage, lerp(REG0, PREV, APREV), so only the glow (REG0) showed.
    // A copy a = b emits nothing when every later read of a, up to a's next
    // write, can read b instead (b unchanged in between): the reads are
    // renamed and the copy is dead. S3D_TEV_COPY_PROP=0 turns it off
    static int copy_on = -1;
    if (copy_on < 0) copy_on = cfg_bool("S3D_TEV_COPY_PROP", true) ? 1 : 0;
    for (int s = 0; copy_on && s < c->nstages; s++) {
        const tev_stage_t* st = &s_cur[s];
        for (int ch = 0; ch < 2; ch++) {
            const u8 a = ch == CH_COLOR ? st->color_reg : st->alpha_reg;
            const Val* src = copy_source(c, s, ch);
            if (!src || src->reg == a) continue;
            const u8 b = src->reg;
            // Check first, rename after: reads of a may only see b unchanged
            // (self copies leave it unchanged), and a must be rewritten
            // before the end unless it is not PREV
            bool ok = false, b_dirty = false;
            int end = c->nstages;
            for (int t = s + 1; t < c->nstages; t++) {
                bool reads = false;
                for (int k = 0; k < 2; k++)
                    for (int i = 0; i < c->plan[t][k].n; i++) reads |= step_reads(&c->plan[t][k].step[i], a, ch);
                if (reads && b_dirty) break;
                const tev_stage_t* tt = &s_cur[t];
                const u8 w = ch == CH_COLOR ? tt->color_reg : tt->alpha_reg;
                const Val* tcp = copy_source(c, t, ch);
                if (!c->passthru[t][ch] && c->plan[t][ch].n > 0 && !(tcp && tcp->reg == w)) {
                    if (w == a) { ok = true; end = t + 1; break; }
                    if (w == b) b_dirty = true;
                }
                if (t == c->nstages - 1) ok = a != GX_TEVPREV;
            }
            if (!ok) continue;
            for (int t = s + 1; t < end; t++)
                for (int k = 0; k < 2; k++)
                    for (int i = 0; i < c->plan[t][k].n; i++) {
                        Step* sp = &c->plan[t][k].step[i];
                        for (int j = 0; j < sp->nsrc; j++)
                            if (val_reads(&sp->src[j], a, ch)) sp->src[j].reg = b;
                    }
            c->passthru[s][ch] = true;
            c->plan[s][ch].n = 0;
            PROF_COUNT("tev.copy_props", 1);
        }
    }

    // Bug 25: a projected light on the map adds two stages per light,
    // REG1 = konst * light tex, PREV += REG1 * light tex. Lights beyond the 3
    // units drop their textures: the add then reduces to PREV = PREV and the
    // konst write is never read, but both still took PICA stages, the program
    // overflowed 6 and the truncation cut the final copy of PREV, so the floor
    // showed the light colour (or black) whenever a hit spawned a light.
    // Channels that only copy their own register, and writes nobody reads
    // before the next write, emit nothing (like passthru). Backwards, so a
    // write only read by a dead one dies too. S3D_TEV_DEAD=0 turns it off
    static int dead_on = -1;
    if (dead_on < 0) dead_on = cfg_bool("S3D_TEV_DEAD", true) ? 1 : 0;
    if (dead_on) {
        for (int s = c->nstages - 1; s >= 0; s--) {
            const tev_stage_t* st = &s_cur[s];
            const u8 regs[2] = { st->color_reg, st->alpha_reg };
            for (int ch = 0; ch < 2; ch++) {
                Plan* p = &c->plan[s][ch];
                if (c->passthru[s][ch] || p->n == 0) continue;
                const Val* cp = copy_source(c, s, ch);
                bool self = cp && cp->reg == regs[ch];
                if (!self && needed(c, regs[ch], ch, s + 1, 0)) continue;
                c->passthru[s][ch] = true;
                p->n = 0;
                // PROF_COUNT keeps the name of its first call per site
                if (self) PROF_COUNT("tev.self_copies", 1);
                else PROF_COUNT("tev.dead_writes", 1);
            }
        }
    }

    for (int s = 0; s < c->nstages; s++)
        for (int ch = 0; ch < 2; ch++)
            if (c->lost_write[s][ch] && !c->passthru[s][ch]) c->plan[s][ch].n = 0;

    for (int s = 0; s < c->nstages; s++) {
        const tev_stage_t* st = &s_cur[s];
        const Plan* pc = &c->plan[s][CH_COLOR];
        const Plan* pa = &c->plan[s][CH_ALPHA];
        int n = pc->n > pa->n ? pc->n : pa->n;

        for (int k = 0; k < n; k++) {
            // Values in the previous output that this step overwrites
            for (int ch = 0; ch < 2; ch++) {
                const Plan* p = ch == CH_COLOR ? pc : pa;
                if (k < p->n) save_or_lose(c, ch, s, k + 1);
            }

            PicaStage* ps = new_stage(c);
            if (!ps) return;
            for (int ch = 0; ch < 2; ch++) {
                const Plan* p = ch == CH_COLOR ? pc : pa;
                if (k < p->n) {
                    emit_step(c, ps, ch, &p->step[k]);
                    c->last_writer[ch] = c->nout;
                    c->prev_holds[ch] = -1;
                    for (int r = 0; r < GX_MAX_TEVREG; r++) {
                        if (c->loc[r][ch] == L_PREV) c->loc[r][ch] = L_LOST;
                    }
                }
                if (p->own_scale) {
                    if (k < p->n) ps->scale[ch] = p->step[k].scale;
                } else if (k == n - 1 && !c->passthru[s][ch] && !c->lost_write[s][ch])
                    ps->scale[ch] = map_scale(ch == CH_COLOR ? st->color_scale : st->alpha_scale);
            }
            c->nout++;
        }

        // The stage's results are now the previous output
        u8 regs[2] = { st->color_reg, st->alpha_reg };
        for (int ch = 0; ch < 2; ch++) {
            if (c->passthru[s][ch]) continue;
            u8 w = regs[ch];
            if (c->buf_holds[ch] == w) c->buf_holds[ch] = -1;
            if (c->lost_write[s][ch]) {
                if (c->prev_holds[ch] == w) c->prev_holds[ch] = -1;
                c->loc[w][ch] = L_LOST;
                c->producer[w][ch] = -1;
                continue;
            }
            c->loc[w][ch] = L_PREV;
            c->prev_holds[ch] = w;
            c->producer[w][ch] = (s8)s;
        }
    }

    // The pixel is PREV: copy it into place if the last stage wrote elsewhere
    for (int ch = 0; ch < 2; ch++)
        if (c->loc[GX_TEVPREV][ch] == L_PREV && c->producer[GX_TEVPREV][ch] >= 0)
            c->used[c->producer[GX_TEVPREV][ch]][ch] = true;
    bool need_copy = c->loc[GX_TEVPREV][CH_COLOR] != L_PREV || c->loc[GX_TEVPREV][CH_ALPHA] != L_PREV;
    if (need_copy) {
        PicaStage* ps = new_stage(c);
        if (!ps) return;
        for (int ch = 0; ch < 2; ch++) {
            if (c->loc[GX_TEVPREV][ch] == L_PREV) continue;
            Val v = val_reg(GX_TEVPREV, ch, ch == CH_COLOR ? GPU_TEVOP_RGB_SRC_COLOR : GPU_TEVOP_A_SRC_ALPHA);
            Step cp = step1(GPU_REPLACE, v);
            emit_step(c, ps, ch, &cp);
        }
        c->nout++;
    }
}

// Bug 37: a value that had to leave PREV while the combiner buffer was busy
// is lost, and its readers get zero (an added term) or the register's
// initial value. The stages that computed it were wasted: Krystal's eye
// spent one on REG2 = REG1 * base, which pushed its final lerp to a buffer
// read at p5 of a latch at p3 (bug 26: unreliable on hardware). Compile,
// mark the stage channels whose result nobody read from PREV or the buffer,
// and compile again with those writes dropped (the register then reads as
// lost, as before), until nothing changes. Only complete compiles are
// judged. S3D_TEV_PRUNE_LOST=0 turns it off
static void compile_pruned(Compiler* c, const GxUnitMap* map, bool allow_two) {
    static int prune_on = -1;
    if (prune_on < 0) prune_on = cfg_bool("S3D_TEV_PRUNE_LOST", true) ? 1 : 0;
    memset(c->lost_write, 0, sizeof(c->lost_write));
    compile(c, map, allow_two);
    for (int iter = 0; prune_on && iter < 4 && !c->overflow; iter++) {
        bool any = false;
        for (int s = 0; s < c->nstages; s++)
            for (int ch = 0; ch < 2; ch++)
                if (c->plan[s][ch].n > 0 && !c->passthru[s][ch] && !c->used[s][ch]) {
                    c->lost_write[s][ch] = true;
                    any = true;
                }
        if (!any) break;
        PROF_COUNT("tev.lost_prunes", 1);
        compile(c, map, allow_two);
    }
}

static Compiler s_compiler;

// Compiled program cache (GRAPHICS_PLAN.md 4.1). A frame draws the same few
// hundred materials over and over, so a program is compiled once per distinct
// input: the stages, the swap tables and the texture unit map. Register and
// konst values only feed the PICA constant colors, which are filled in from
// the current values on every use (patch_konst), so animated colors do not
// recompile. S3D_TEV_CACHE=0 turns
// it off (compile on every TEV state change, as before).
// S3D_RT_MEMO bit 4 (gx_internal.h) makes it two-way: the hub kept 12 pairs
// of programs evicting each other every frame (5C202467), and each eviction
// is a recompile. Way 0 is the old direct-mapped table, way 1 the second half
#define TEV_CACHE_SIZE 256

typedef struct {
    u32 n;
    s8 stage_unit[GX_MAX_TEVSTAGE];
    s8 stage_bump[GX_MAX_TEVSTAGE];
    u8 unit_swizzle[GX_PICA_TEX_UNITS];
    u8 pad;
    u8 swaps[GX_MAX_TEVSWAP][4];
    tev_stage_t stages[GX_MAX_TEVSTAGE];
} TevKey;

typedef struct {
    u8 nout;
    u8 buf_mask[2];
    u8 dyn_mask;  // PICA stages whose constant color reads register/konst values
    u8 konst_src[GX_PICA_TEV_STAGES][4];
    u8 fallback;  // TEV_FB_* bits: how the compile fell short
} TevProgInfo;

// Compile shortfalls, shown by the S3D_TEV_TINT_FALLBACK debug tint
enum { TEV_FB_APPROX = 1, TEV_FB_TRUNCATED = 2 };

typedef struct {
    TevKey key;
    u32 hash;
    u32 stamp;  // Last use (two-way replacement)
    bool valid;
    TevProgInfo info;
    C3D_TexEnv env[GX_PICA_TEV_STAGES];
} TevCacheEntry;

static TevCacheEntry* s_tev_cache;
static int s_tev_cache_on = -1;
// Entry of the last get_program call and its hash (an entry can be replaced
// later, gx_tev_multipass also compiles), for gx_tev_repatch
static TevCacheEntry* s_last_entry;
static u32 s_last_entry_hash;
static TevKey s_key;

static size_t s_key_len;

static u32 build_key(const GxUnitMap* map) {
    s_key.n = s_cur_n;
    s_key.pad = s_frag_sec;
    memcpy(s_key.stage_unit, map->stage_unit, sizeof(s_key.stage_unit));
    memcpy(s_key.stage_bump, map->stage_bump, sizeof(s_key.stage_bump));
    for (int u = 0; u < GX_PICA_TEX_UNITS; u++) s_key.unit_swizzle[u] = u < map->unit_count ? map->unit_swizzle[u] : 0;
    memcpy(s_key.swaps, s_swap_tables, sizeof(s_swap_tables));
    // Only the stages in use are keyed, up to a whole zero-padded word
    size_t len = offsetof(TevKey, stages) + sizeof(tev_stage_t) * s_cur_n;
    size_t len4 = (len + 3) & ~(size_t)3;
    memcpy(s_key.stages, s_cur, sizeof(tev_stage_t) * s_cur_n);
    memset((u8*)&s_key + len, 0, len4 - len);
    s_key_len = len4;
    const u32* w = (const u32*)&s_key;
    u32 h = 2166136261u;
    for (size_t i = 0; i < len4 / 4; i++) h = (h ^ w[i]) * 16777619u;
    // Final mix: the multiplies only carry differences upward, so without it
    // keys differing in a word's top byte share a cache slot
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    return h ^ (h >> 16);
}

// Fill the constant colors of a program with the current register/konst values
static void patch_konst(C3D_TexEnv* env, const TevProgInfo* info) {
    for (int i = 0; i < GX_PICA_TEV_STAGES; i++) {
        if (!(info->dyn_mask & (1 << i))) continue;
        u32 color = env[i].color;
        for (int b = 0; b < 4; b++) {
            u8 src = info->konst_src[i][b];
            if (src == KSRC_LITERAL) continue;
            int idx = (src & ~KSRC_HALF) - 1;
            u8 v = idx < 4 * GX_MAX_TEVREG                       ? ((const u8*)s_regs)[idx]
                   : idx < 4 * GX_MAX_TEVREG + 4 * GX_MAX_KCOLOR ? ((const u8*)s_kcolors)[idx - 4 * GX_MAX_TEVREG]
                                                                  : s_drop_avg[idx - 4 * GX_MAX_TEVREG - 4 * GX_MAX_KCOLOR];
            if (src & KSRC_HALF) v >>= 1;
            color = (color & ~(0xFFu << (8 * b))) | ((u32)v << (8 * b));
        }
        env[i].color = color;
    }
}

static void compile_program(const GxUnitMap* map, C3D_TexEnv* env, TevProgInfo* info) {
    Compiler* c = &s_compiler;
    info->fallback = 0;
    compile_pruned(c, map, true);
    if (c->overflow) {
        compile_pruned(c, map, false);  // Single-stage approximations
        PROF_COUNT("tev.fallback_compiles", 1);
        info->fallback |= TEV_FB_APPROX;
        if (c->overflow) {
            info->fallback |= TEV_FB_TRUNCATED;
            LOG_ONCE(LOG_LEVEL_WARN, "tev", "TEV program needs more than %d PICA stages (truncated)",
                     GX_PICA_TEV_STAGES);
            PROF_COUNT("tev.truncated", 1);
        }
    }

    for (int i = 0; i < GX_PICA_TEV_STAGES; i++) {
        C3D_TexEnv* e = &env[i];
        C3D_TexEnvInit(e);  // Pass the previous stage through unchanged
        if (i >= c->nout) continue;

        const PicaStage* ps = &c->out[i];
        C3D_TexEnvSrc(e, C3D_RGB, ps->src[0][0], ps->src[0][1], ps->src[0][2]);
        C3D_TexEnvSrc(e, C3D_Alpha, ps->src[1][0], ps->src[1][1], ps->src[1][2]);
        C3D_TexEnvOpRgb(e, (GPU_TEVOP_RGB)ps->op[0][0], (GPU_TEVOP_RGB)ps->op[0][1], (GPU_TEVOP_RGB)ps->op[0][2]);
        C3D_TexEnvOpAlpha(e, (GPU_TEVOP_A)ps->op[1][0], (GPU_TEVOP_A)ps->op[1][1], (GPU_TEVOP_A)ps->op[1][2]);
        C3D_TexEnvFunc(e, C3D_RGB, ps->func[0]);
        C3D_TexEnvFunc(e, C3D_Alpha, ps->func[1]);
        C3D_TexEnvScale(e, C3D_RGB, ps->scale[0]);
        C3D_TexEnvScale(e, C3D_Alpha, ps->scale[1]);
        // Constant color is packed as 0xAABBGGRR
        const u8* k = ps->konst;
        C3D_TexEnvColor(e, k[0] | (k[1] << 8) | (k[2] << 16) | ((u32)k[3] << 24));
    }
    info->nout = (u8)c->nout;
    info->buf_mask[CH_COLOR] = c->buf_mask[CH_COLOR];
    info->buf_mask[CH_ALPHA] = c->buf_mask[CH_ALPHA];
    info->dyn_mask = 0;
    for (int i = 0; i < GX_PICA_TEV_STAGES; i++) {
        for (int b = 0; b < 4; b++) {
            u8 src = i < c->nout ? c->out[i].konst_src[b] : KSRC_LITERAL;
            info->konst_src[i][b] = src;
            if (src != KSRC_LITERAL) info->dyn_mask |= (u8)(1 << i);
        }
    }

    // Debug (bug 25): S3D_TEV_LOG_FALLBACK=N logs the first N compiles that
    // dropped a texture or fell back, with the GX stages, the unit map and
    // the PICA program (gx_tev_dump_program recompiles; c->out is done)
    static int log_left = -1;
    if (log_left < 0) log_left = cfg_int("S3D_TEV_LOG_FALLBACK", 0);
    if (log_left > 0) {
        u32 drop = 0;
        for (int s = 0; s < s_cur_n; s++) {
            const tev_stage_t* st = &s_cur[s];
            if (st->texmap < GX_NUM_TEXMAP && stage_uses_texture(st) && map->stage_unit[s] < 0) drop |= 1u << s;
        }
        if (drop || info->fallback) {
            log_left--;
            char buf[1024];
            gx_tev_describe(buf, sizeof(buf));
            LOG_INFO("tevlog", "fallback %x dropped stages %03x: %s", info->fallback, (unsigned)drop, buf);
            for (int u = 0; u < map->unit_count; u++)
                LOG_INFO("tevlog", "  unit%d map%d tc%d", u, map->unit_texmap[u], map->unit_texcoord[u]);
            gx_tev_dump_program(map);
        }
    }
}

// The program for the current stages (s_cur) and unit map, from the cache or
// compiled. The returned combiners stay valid until the next call
static const C3D_TexEnv* get_program(const GxUnitMap* map, u8* nout, u8* buf_mask, u8* fallback) {
    if (s_tev_cache_on < 0) {
        s_tev_cache_on = cfg_bool("S3D_TEV_CACHE", true);
        if (s_tev_cache_on) {
            s_tev_cache = calloc(TEV_CACHE_SIZE * 2, sizeof(TevCacheEntry));
            if (!s_tev_cache) {
                LOG_WARN("tev", "no memory for the TEV program cache (%u KB)",
                         (unsigned)(TEV_CACHE_SIZE * 2 * sizeof(TevCacheEntry) / 1024));
                s_tev_cache_on = 0;
            }
        }
    }

    static C3D_TexEnv s_env[GX_PICA_TEV_STAGES];
    s_last_entry = NULL;
    if (!s_tev_cache_on) {
        TevProgInfo info;
        compile_program(map, s_env, &info);
        *nout = info.nout;
        *fallback = info.fallback;
        buf_mask[0] = info.buf_mask[0];
        buf_mask[1] = info.buf_mask[1];
        return s_env;
    }
    static u32 s_tick;
    u32 h = build_key(map);
    TevCacheEntry* e = &s_tev_cache[h % TEV_CACHE_SIZE];
    TevCacheEntry* e1 = (g_rt_memo & 4) ? &s_tev_cache[TEV_CACHE_SIZE + h % TEV_CACHE_SIZE] : NULL;
    bool hit = e->valid && e->hash == h && memcmp(&e->key, &s_key, s_key_len) == 0;
    if (!hit && e1 && e1->valid && e1->hash == h && memcmp(&e1->key, &s_key, s_key_len) == 0) {
        e = e1;
        hit = true;
    }
    if (hit) {
        PROF_COUNT("tev.cache_hits", 1);
    } else {
        // Two-way: replace the empty or least recently used way
        if (e1 && (!e1->valid || (e->valid && e1->stamp < e->stamp))) e = e1;
        PROF_COUNT("tev.cache_misses", 1);
        if (e->valid) PROF_COUNT("tev.cache_evictions", 1);
        GX_PROF_ZONE("gx.tev.compile");
        compile_program(map, e->env, &e->info);
        memcpy(&e->key, &s_key, s_key_len);
        e->hash = h;
        e->valid = true;
    }
    e->stamp = ++s_tick;
    s_last_entry = e;
    s_last_entry_hash = h;
    if (e->info.dyn_mask) patch_konst(e->env, &e->info);
    *nout = e->info.nout;
    *fallback = e->info.fallback;
    buf_mask[0] = e->info.buf_mask[0];
    buf_mask[1] = e->info.buf_mask[1];
    return e->env;
}

static u8 s_last_buf[2];

static bool s_applied_valid;
static u8 s_applied_nout;
static bool s_apply_kept;  // gx_tev_apply_kept
static TevCacheEntry* s_applied_entry;  // Cache entry of the applied program (NULL: none / tinted)
static u32 s_applied_entry_hash;
static TevKey s_applied;
static bool s_applied_rewritten;

static bool movie_stages_unchanged(void) {
    if (!movie_active()) return true;
    if (s_applied_rewritten) return false;
    for (int s = 0; s < s_cur_n; s++)
        if (stage_uses_texture(&s_stages[s]) && stage_movie_plane(&s_stages[s]) != MOVIE_PLANE_NONE) return false;
    return true;
}

void gx_tev_touched(void) { s_applied_valid = false; }

// Shadow of the combiners last handed to citro3d: C3D_GetTexEnv marks a stage
// dirty and citro3d re-emits every dirty stage, so only differing stages are
// written. Code that writes combiners behind this (citro2d, bench, debug
// paths) invalidates it
static C3D_TexEnv s_hw_env[GX_PICA_TEV_STAGES];
static u8 s_hw_valid;  // bit per stage
static int s_hw_buf = -1;

void gx_tev_hw_invalidate(void) {
    s_hw_valid = 0;
    s_hw_buf = -1;
}

void gx_tev_hw_stage(int i, const C3D_TexEnv* env) {
    if ((s_hw_valid >> i & 1) && memcmp(&s_hw_env[i], env, sizeof(*env)) == 0) return;
    *C3D_GetTexEnv(i) = *env;
    s_hw_env[i] = *env;
    s_hw_valid |= 1u << i;
    PROF_COUNT("tev.hw_stage_writes", 1);
}

void gx_tev_hw_write(const C3D_TexEnv* env, const u8* buf_mask) {
    for (int i = 0; i < GX_PICA_TEV_STAGES; i++) gx_tev_hw_stage(i, &env[i]);
    int buf = buf_mask[0] | buf_mask[1] << 8;
    if (buf != s_hw_buf) {
        C3D_TexEnvBufUpdate(C3D_RGB, buf_mask[0]);
        C3D_TexEnvBufUpdate(C3D_Alpha, buf_mask[1]);
        s_hw_buf = buf;
    }
}

// Debug (S3D_TEV_TINT_FALLBACK): draws the TEV could not reproduce come out in
// a flat colour, by cause in priority order: magenta truncated, yellow
// approximated, cyan dropped texture, green indirect texturing
static bool tint_fallback(C3D_TexEnv* env, u8 fallback, const GxUnitMap* map) {
    u32 color;
    if (fallback & TEV_FB_TRUNCATED) color = 0xFFFF00FF;
    else if (fallback & TEV_FB_APPROX) color = 0xFF00FFFF;
    else if (gx_tev_drops_texture(map) && map->proj_split < 0) color = 0xFFFFFF00;
    else if (s_num_ind_stages) color = 0xFF00FF00;
    else return false;
    // The last stage's colour becomes the constant; its alpha (the program's
    // alpha, or a pass-through) is kept so cutouts and blending keep the shape
    C3D_TexEnv* e = &env[GX_PICA_TEV_STAGES - 1];
    C3D_TexEnvSrc(e, C3D_RGB, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvOpRgb(e, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvScale(e, C3D_RGB, GPU_TEVSCALE_1);
    e->color = (e->color & 0xFF000000u) | (color & 0x00FFFFFFu);
    return true;
}

u8 gx_tev_apply(const GxUnitMap* map) {
    static bool s_tint;
    CFG_LIVE(s_tint, cfg_bool("S3D_TEV_TINT_FALLBACK", false));
    bool tint = s_tint || gx_capture_mode() == GX_CAP_TINT;  // Render capture: the tint pass
    static bool s_was_tint;
    if (tint != s_was_tint) s_applied_valid = false;  // The cached program may be tinted
    s_was_tint = tint;
    // Texture-only changes keep the stages and constants: the program is the
    // one applied last unless the unit map or secondary color changed
    if (!tint && s_applied_valid && s_applied.pad == s_frag_sec &&
        memcmp(s_applied.stage_unit, map->stage_unit, sizeof(s_applied.stage_unit)) == 0 &&
        memcmp(s_applied.stage_bump, map->stage_bump, sizeof(s_applied.stage_bump)) == 0) {
        bool same = true;
        for (int u = 0; u < GX_PICA_TEX_UNITS; u++)
            if (s_applied.unit_swizzle[u] != (u < map->unit_count ? map->unit_swizzle[u] : 0)) same = false;
        if (same && movie_stages_unchanged()) {
            PROF_COUNT("tev.apply_skips", 1);
            s_apply_kept = true;
            return s_applied_nout;
        }
    }
    s_apply_kept = false;
    u8 nout, buf_mask[2], fallback;
    const C3D_TexEnv* env = get_program(map, &nout, buf_mask, &fallback);
    s_applied_entry = tint ? NULL : s_last_entry;
    s_applied_entry_hash = s_last_entry_hash;
    static C3D_TexEnv s_tint_env[GX_PICA_TEV_STAGES];
    if (tint) {
        memcpy(s_tint_env, env, sizeof(s_tint_env));
        if (tint_fallback(s_tint_env, fallback, map)) env = s_tint_env;
    }
    s_applied.pad = s_frag_sec;
    memcpy(s_applied.stage_unit, map->stage_unit, sizeof(s_applied.stage_unit));
    memcpy(s_applied.stage_bump, map->stage_bump, sizeof(s_applied.stage_bump));
    for (int u = 0; u < GX_PICA_TEX_UNITS; u++) s_applied.unit_swizzle[u] = u < map->unit_count ? map->unit_swizzle[u] : 0;
    s_applied_nout = nout;
    s_applied_rewritten = s_cur != s_stages;
    s_applied_valid = true;
    u8 hw_buf[2] = { buf_mask[CH_COLOR], buf_mask[CH_ALPHA] };
    gx_tev_hw_write(env, hw_buf);
    s_last_buf[0] = buf_mask[CH_COLOR];
    s_last_buf[1] = buf_mask[CH_ALPHA];
    PROF_COUNT("tev.pica_stages", nout);
    return nout;
}

bool gx_tev_apply_kept(void) { return s_apply_kept; }

bool gx_tev_repatch(void) {
    TevCacheEntry* e = s_applied_entry;
    if (!s_applied_valid) PROF_COUNT("tev.repatch_not_applied", 1);
    else if (!e) PROF_COUNT("tev.repatch_no_entry", 1);
    else if (!e->valid || e->hash != s_applied_entry_hash) PROF_COUNT("tev.repatch_replaced", 1);
    if (!s_applied_valid || !e || !e->valid || e->hash != s_applied_entry_hash || !movie_stages_unchanged()) return false;
    if (e->info.dyn_mask) {
        patch_konst(e->env, &e->info);
        u8 hw_buf[2] = { s_last_buf[0], s_last_buf[1] };
        gx_tev_hw_write(e->env, hw_buf);
    }
    PROF_COUNT("tev.repatch", 1);
    return true;
}

void gx_tev_last_buf(u8* buf_mask) {
    buf_mask[0] = s_last_buf[0];
    buf_mask[1] = s_last_buf[1];
}

// --- Multipass (GRAPHICS_PLAN.md 4.1) ---------------------------------------------------
// A draw with more textures than units loses one. When the lost texture only
// enters the colour as one last term, a second pass over the same triangles
// draws that term and the blender combines it with the first pass:
//   additive: PREV = PREV + b*c (reflections, glows). Pass 2 outputs b*c, blended ONE, ONE
//   multiply: PREV = PREV*X (light or shadow maps). Pass 2 outputs X, blended DST_COLOR, ZERO
// The first pass already keeps PREV unchanged on the dropped stage (compile).

// An input pass 2 can compute: the texture, the vertex colour, constants, or a
// register nothing before the stage writes (so it still holds its set value)
static bool mp_arg_ok(u8 arg, int s_d) {
    switch (arg) {
        case GX_CC_TEXC: case GX_CC_TEXA: case GX_CC_RASC: case GX_CC_RASA:
        case GX_CC_ONE: case GX_CC_HALF: case GX_CC_KONST: case GX_CC_ZERO:
            return true;
        case GX_CC_C0: case GX_CC_A0: case GX_CC_C1: case GX_CC_A1: case GX_CC_C2: case GX_CC_A2: {
            int reg = GX_TEVREG0 + (arg - GX_CC_C0) / 2;
            for (int s = 0; s < s_d; s++)
                if (s_cur[s].color_reg == reg || s_cur[s].alpha_reg == reg) return false;
            return true;
        }
        default:
            return false;
    }
}

static bool mp_cprev_passthru(const tev_stage_t* st) {
    return st->color_in[0] == GX_CC_ZERO && st->color_in[1] == GX_CC_ZERO && st->color_in[2] == GX_CC_ZERO &&
           st->color_in[3] == GX_CC_CPREV && st->color_op == GX_TEV_ADD && st->color_bias == GX_TB_ZERO &&
           st->color_scale == GX_CS_SCALE_1;
}

bool gx_tev_drops_texture(const GxUnitMap* map) {
    for (int s = 0; s < s_cur_n; s++) {
        const tev_stage_t* st = &s_cur[s];
        if (st->texmap >= GX_NUM_TEXMAP || st->texcoord >= GX_NUM_TEXCOORD || !stage_uses_texture(st)) continue;
        if (map->stage_unit[s] < 0) return true;
    }
    return false;
}

bool gx_tev_multipass(const GxUnitMap* map, u8 unit_ok, int force_unit, GxMultipass* out) {
    // The one dropped texture and the one stage sampling it
    int dropped = -1, s_d = -1;
    for (int s = 0; s < s_cur_n; s++) {
        const tev_stage_t* st = &s_cur[s];
        if (st->texmap >= GX_NUM_TEXMAP || st->texcoord >= GX_NUM_TEXCOORD || !stage_uses_texture(st)) continue;
        if (map->stage_unit[s] >= 0) continue;
        if (dropped >= 0) {
            PROF_COUNT("tev.multipass_skip_two", 1);
            return false;  // Two dropped stages
        }
        dropped = st->texmap;
        s_d = s;
    }
    if (dropped < 0) return false;

    const tev_stage_t* st = &s_cur[s_d];
    const u8* in = st->color_in;
    u8 kind = 0;
    bool ok = unit_ok && st->color_op == GX_TEV_ADD && st->color_bias == GX_TB_ZERO &&
              st->color_scale == GX_CS_SCALE_1 && st->color_reg == GX_TEVPREV && in[0] == GX_CC_ZERO;
    for (int i = 0; i < 4; i++) ok &= st->alpha_in[i] != GX_CA_TEXA;
    if (ok) {
        if (in[3] == GX_CC_CPREV && mp_arg_ok(in[1], s_d) && mp_arg_ok(in[2], s_d)) {
            kind = GX_MULTIPASS_ADD;
        } else if (in[3] == GX_CC_ZERO && in[1] == GX_CC_CPREV && mp_arg_ok(in[2], s_d)) {
            kind = GX_MULTIPASS_MUL;
        } else if (in[3] == GX_CC_ZERO && in[2] == GX_CC_CPREV && mp_arg_ok(in[1], s_d)) {
            kind = GX_MULTIPASS_MUL;
        }
    }
    // Later stages may not change the colour any further
    for (int s = s_d + 1; kind && s < s_cur_n; s++)
        if (s_cur[s].color_reg == GX_TEVPREV && !mp_cprev_passthru(&s_cur[s])) kind = 0;

    // A unit whose vertex coordinates fit: any for position / normal
    // texgens (the shader reads those attributes), else one fed by the same
    // source (the texture matrix is a uniform)
    int coord = st->texcoord, unit = -1;
    if (kind && force_unit >= 0 && force_unit < GX_PICA_TEX_UNITS) {
        unit = force_unit;
    } else if (kind && coord < g_gx.num_texgens) {
        u8 src = g_gx.texgen[coord].src;
        for (int u = 0; u < map->unit_count && unit < 0; u++) {
            if (!(unit_ok & (1u << u))) continue;
            int uc = map->unit_texcoord[u];
            if (src == GX_TG_POS || src == GX_TG_NRM ||
                (uc >= 0 && uc < g_gx.num_texgens && g_gx.texgen[uc].src == src))
                unit = u;
        }
    }
    if (unit < 0) {
        PROF_COUNT(!kind ? "tev.multipass_skip_form" : "tev.multipass_skip_unit", 1);
        return false;
    }

    // Pass 2: one stage computing the lost term from the dropped texture
    tev_stage_t mp = *st;
    if (kind == GX_MULTIPASS_ADD) {
        mp.color_in[3] = GX_CC_ZERO;
    } else {
        mp.color_in[3] = in[1] == GX_CC_CPREV ? in[2] : in[1];
        mp.color_in[1] = mp.color_in[2] = GX_CC_ZERO;
    }
    mp.color_in[0] = GX_CC_ZERO;
    mp.alpha_in[0] = mp.alpha_in[1] = mp.alpha_in[2] = mp.alpha_in[3] = GX_CA_ZERO;
    mp.alpha_op = GX_TEV_ADD;
    mp.alpha_bias = GX_TB_ZERO;
    mp.alpha_scale = GX_CS_SCALE_1;
    mp.alpha_reg = GX_TEVPREV;

    GxUnitMap m2 = *map;
    for (int t = 0; t < GX_NUM_TEXMAP; t++) m2.texmap_unit[t] = -1;
    m2.texmap_unit[dropped] = (s8)unit;
    memset(m2.stage_unit, -1, sizeof(m2.stage_unit));
    memset(m2.stage_bump, -1, sizeof(m2.stage_bump));
    m2.stage_unit[0] = (s8)unit;
    m2.unit_texmap[unit] = (s8)dropped;
    m2.unit_texcoord[unit] = (s8)coord;
    m2.unit_swizzle[unit] = 0;
    m2.unit_warp[unit] = 0.0f;
    if (m2.unit_count <= unit) m2.unit_count = unit + 1;

    const tev_stage_t* save = s_cur;
    int save_n = s_cur_n;
    s_cur = &mp;
    s_cur_n = 1;
    const C3D_TexEnv* env = get_program(&m2, &out->nout, out->buf_mask, &(u8){0});
    memcpy(out->env, env, sizeof(out->env));
    s_cur = save;
    s_cur_n = save_n;

    out->kind = kind;
    out->texmap = (s8)dropped;
    out->texcoord = (s8)coord;
    out->unit = (s8)unit;
    PROF_COUNT(kind == GX_MULTIPASS_ADD ? "tev.multipass_add" : "tev.multipass_mul", 1);
    return true;
}

// Projected water (screen-copy reflection and refraction): the last stage
// mixes the reflection with a register holding refraction + konst, by the
// konst alpha. Only unit 0 divides by q per fragment, so the two copies are
// drawn in two passes, each on unit 0 (gx_render.c water_split_prepare)
static bool ws_plain(const tev_stage_t* st) {
    return st->color_op == GX_TEV_ADD && st->color_bias == GX_TB_ZERO && st->color_scale == GX_CS_SCALE_1 &&
           st->alpha_op == GX_TEV_ADD && st->alpha_bias == GX_TB_ZERO && st->alpha_scale == GX_CS_SCALE_1 &&
           st->tex_swap == 0 && st->ras_swap == 0;
}

bool gx_tev_water_split(const GxUnitMap* map, GxWaterSplit* out) {
    if (s_cur_n < 2) return false;
    int l = s_cur_n - 1;
    const tev_stage_t* st = &s_cur[l];
    const u8* c = st->color_in;
    const u8* a = st->alpha_in;
    if (!ws_plain(st) || st->color_reg != GX_TEVPREV || st->alpha_reg != GX_TEVPREV) return false;
    if (c[0] != GX_CC_TEXC || c[3] != GX_CC_ZERO || c[1] < GX_CC_C0 || c[1] > GX_CC_C2 || (c[1] - GX_CC_C0) & 1 ||
        c[2] != c[1] + 1)
        return false;
    if (a[0] != GX_CA_ZERO || a[1] != GX_CA_ZERO || a[2] != GX_CA_ZERO || a[3] != GX_CA_RASA) return false;
    int reg = GX_TEVREG0 + (c[1] - GX_CC_C0) / 2;
    // The last stage before writing the register: refraction + konst
    int w = -1;
    for (int s = l - 1; s >= 0 && w < 0; s--)
        if (s_cur[s].color_reg == reg || s_cur[s].alpha_reg == reg) w = s;
    if (w < 0) return false;
    const tev_stage_t* ws = &s_cur[w];
    const u8* wc = ws->color_in;
    const u8* wa = ws->alpha_in;
    if (!ws_plain(ws) || ws->color_reg != reg || ws->alpha_reg != reg) return false;
    bool kt = wc[0] == GX_CC_KONST && wc[3] == GX_CC_TEXC;
    bool tk = wc[0] == GX_CC_TEXC && wc[3] == GX_CC_KONST;
    if (!(kt || tk) || wc[1] != GX_CC_ZERO || wc[2] != GX_CC_ZERO) return false;
    if (wa[0] != GX_CA_ZERO || wa[1] != GX_CA_ZERO || wa[2] != GX_CA_ZERO || wa[3] != GX_CA_KONST) return false;

    int ur = map->stage_unit[l], uf = map->stage_unit[w];
    if (ur != 0 || uf <= 0 || uf >= map->unit_count) return false;
    if (map->unit_texmap[ur] != map->unit_texmap[uf] || map->unit_swizzle[ur] || map->unit_swizzle[uf]) return false;
    int tr = map->unit_texcoord[ur], tf = map->unit_texcoord[uf];
    if (tr < 0 || tf < 0 || tr >= g_gx.num_texgens || tf >= g_gx.num_texgens) return false;
    if (g_gx.texgen[tr].func != GX_TG_MTX3x4 || g_gx.texgen[tf].func != GX_TG_MTX3x4) return false;

    Val kc = konst_value(0, ws->kcsel, false);
    Val ka = konst_value(0, ws->kasel, true);
    out->konst = kc.cval[0] | (kc.cval[1] << 8) | (kc.cval[2] << 16) | ((u32)ka.cval[3] << 24);
    out->refr_unit = (s8)uf;
    out->refr_texcoord = (s8)tf;
    return true;
}

// Debug: describe the active TEV stages
void gx_tev_describe(char* buf, int size) {
    int n = snprintf(buf, size, "%u stages", s_num_stages);
    for (int s = 0; s < s_num_stages && s < GX_MAX_TEVSTAGE && n < size - 72; s++) {
        const tev_stage_t* st = &s_stages[s];
        n += snprintf(buf + n, size - n, " | s%d c(%u,%u,%u,%u)op%u>r%u a(%u,%u,%u,%u)op%u>r%u map%u tc%u ch%u k%u/%u",
                      s, st->color_in[0], st->color_in[1], st->color_in[2], st->color_in[3], st->color_op,
                      st->color_reg, st->alpha_in[0], st->alpha_in[1], st->alpha_in[2], st->alpha_in[3],
                      st->alpha_op, st->alpha_reg, st->texmap, st->texcoord, st->channel, st->kcsel, st->kasel);
    }
    for (int i = 0; i < GX_MAX_KCOLOR && n < size - 16; i++) {
        const GXColor* k = &s_kcolors[i];
        n += snprintf(buf + n, size - n, " K%d=%02X%02X%02X%02X", i, k->r, k->g, k->b, k->a);
    }
    for (int i = 0; i < GX_MAX_TEVREG && n < size - 16; i++) {
        const GXColor* r = &s_regs[i];
        n += snprintf(buf + n, size - n, " R%d=%02X%02X%02X%02X", i, r->r, r->g, r->b, r->a);
    }
}

void gx_tev_dump_program(const GxUnitMap* map) {
    Compiler* c = &s_compiler;
    compile_pruned(c, map, true);
    bool two = !c->overflow;
    if (c->overflow) compile_pruned(c, map, false);
    LOG_INFO("dump", "  pica %d stages, %s%s buf %x/%x", c->nout, two ? "full" : "fallback", c->overflow ? " truncated" : "",
             c->buf_mask[CH_COLOR], c->buf_mask[CH_ALPHA]);
    for (int i = 0; i < c->nout; i++) {
        const PicaStage* ps = &c->out[i];
        LOG_INFO("dump", "   p%d rgb f%d %x/%d %x/%d %x/%d x%d | a f%d %x/%d %x/%d %x/%d | k %02x%02x%02x%02x", i,
                 ps->func[0], ps->src[0][0], ps->op[0][0], ps->src[0][1], ps->op[0][1], ps->src[0][2], ps->op[0][2],
                 ps->scale[0], ps->func[1], ps->src[1][0], ps->op[1][0], ps->src[1][1], ps->op[1][1], ps->src[1][2],
                 ps->op[1][2], ps->konst[0], ps->konst[1], ps->konst[2], ps->konst[3]);
    }
    if (map->avg_map >= 0)
        LOG_INFO("dump", "   avg map%d = %02X%02X%02X%02X", map->avg_map, s_drop_avg[0], s_drop_avg[1], s_drop_avg[2],
                 s_drop_avg[3]);
    for (int s = 0; s < c->nstages; s++)
        if (c->passthru[s][0] || c->passthru[s][1] || c->lost_write[s][0] || c->lost_write[s][1])
            LOG_INFO("dump", "   passthru s%d %d%d%s", s, c->passthru[s][0], c->passthru[s][1],
                     c->lost_write[s][0] || c->lost_write[s][1] ? (c->lost_write[s][0] ? " lost rgb" : " lost a") : "");
}

// State census (S3D_STATE_CENSUS, gx_render.c): hash of every TEV input, so
// the render thread can count how often a whole material state repeats
static u32 hash_words(u32 h, const void* p, u32 bytes) {
    const u8* b = p;
    for (u32 i = 0; i < bytes; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

// part: 0 everything, 1 stages and swap/indirect setup only, 2 register and konst colors only
u32 gx_tev_state_hash(u32 h, int part) {
    if (part == 2) {
        h = hash_words(h, s_regs, sizeof(s_regs));
        return hash_words(h, s_kcolors, sizeof(s_kcolors));
    }
    u32 n = s_num_stages <= GX_MAX_TEVSTAGE ? s_num_stages : GX_MAX_TEVSTAGE;
    h = hash_words(h, &n, sizeof(n));
    h = hash_words(h, s_stages, sizeof(tev_stage_t) * n);
    if (part == 0) {
        h = hash_words(h, s_regs, sizeof(s_regs));
        h = hash_words(h, s_kcolors, sizeof(s_kcolors));
    }
    h = hash_words(h, s_swap_tables, sizeof(s_swap_tables));
    h = hash_words(h, &s_num_ind_stages, 1);
    h = hash_words(h, s_stage_warp, sizeof(float) * n);
    h = hash_words(h, s_stage_ind, n);
    h = hash_words(h, s_stage_bump_sel, n);
    h = hash_words(h, s_stage_ripple, n);
    h = hash_words(h, s_ind_coord, sizeof(s_ind_coord));
    h = hash_words(h, s_ind_map, sizeof(s_ind_map));
    return h;
}

// Texture maps the active stages sample (state census)
u32 gx_tev_texmap_mask(void) {
    u32 m = 0;
    u32 n = s_num_stages <= GX_MAX_TEVSTAGE ? s_num_stages : GX_MAX_TEVSTAGE;
    for (u32 i = 0; i < n; i++)
        if (s_stages[i].texmap < GX_NUM_TEXMAP) m |= 1u << s_stages[i].texmap;
    for (int i = 0; i < s_num_ind_stages && i < GX_MAX_INDTEXSTAGE; i++)
        if (s_ind_map[i] < GX_NUM_TEXMAP) m |= 1u << s_ind_map[i];
    return m;
}
