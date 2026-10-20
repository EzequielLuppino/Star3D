# Indirect fallback counters: GXSetTevIndirect calls are counted per calling
# function (gx_tev.c port_tev_indirect)
1i\
#include "dolphin/gx/GXBump.h"\
void port_tev_indirect(const char* func, GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,\
                       GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,\
                       GXBool add_prev, GXBool ind_lod, GXIndTexAlphaSel alpha_sel);
s|\bGXSetTevIndirect(|port_tev_indirect(__func__, |g
# Env map bump fallback (GRAPHICS_PLAN.md 5.6, gx_tev.c port_env_bump_skip)
1i\
int port_env_bump_skip(void);
/^    t = lbl_803DB5E8 & 1;$/a\
    if (port_env_bump_skip()) t = 0;
# Bug 34 (C12 floor switch): selectTexture(NULL) loads nothing, so the unit
# keeps the previous material's texture. Report it (diag_3ds.c port_null_texture)
1i\
void port_null_texture(int mapId, void* caller);
/^void selectTexture(Texture\* texture, int mapId)$/,/^}/s|^        return;$|        { port_null_texture(mapId, __builtin_return_address(0)); return; }|
# Bug 35 (C12 floor caustics): the two baked caustic-net layers drift against
# each other with the caustic scroll (port/caustic_3ds.c port_warped_ring_scroll)
1i\
void port_warped_ring_scroll(float (*m0)[4], float (*m1)[4]);
s#^\( *\)m1b8\[2\]\[3\] = 1\.0f;$#&\
\1port_warped_ring_scroll(m1e8, m1b8);#
