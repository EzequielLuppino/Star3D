# Indirect fallback counters: GXSetTevIndirect calls are counted per calling
# function (gx_tev.c port_tev_indirect)
1i\
#include "dolphin/gx/GXBump.h"\
void port_tev_indirect(const char* func, GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,\
                       GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,\
                       GXBool add_prev, GXBool ind_lod, GXIndTexAlphaSel alpha_sel);\
_Bool port_copytex_blur_scope(const void* dest, int level);
s|\bGXSetTevIndirect(|port_tev_indirect(__func__, |g
# DOF (doBlurFilter): the 5-tap cross samples the screen copy at mip level 1
# (2 for the bigger blur) so it reads as a smooth blur (gx_copy.c
# port_copytex_blur_scope); other users of the copy still sample it sharp
/^void doBlurFilter(/,/^}/{
/^    selectReflectionTexture(0);$/a\
    port_copytex_blur_scope((char*)getReflectionTexture1() + sizeof(Texture), param5 ? 2 : 1);
/^    Camera_RebuildProjectionMatrix();$/i\
    port_copytex_blur_scope(0, 0);
}
# Heat shimmer and distortion shockwave (GRAPHICS_PLAN.md 5.2): the indirect
# warp of the screen copy becomes a 40x24 grid with per-vertex offsets
# (port/screenwarp_3ds.c); S3D_SCREEN_WARP=0 keeps the plain quad
1i\
_Bool port_screen_warp_enabled(void);\
void port_heat_grid(float fA, float fB, float rot_c, float rot_s, unsigned char a_top, unsigned char a_bot);\
void port_distortion_grid(float proj5, float proj4, float ind_s, float angle);
/^void doDistortionFilter(/,/^}/{
/^    GXBegin(GX_QUADS, GX_VTXFMT0, 4);$/i\
    if (port_screen_warp_enabled()) {\
        f32 grid_s = gDistortionIndMtxRadius / radius;\
        port_distortion_grid(proj5, proj4, grid_s > 0.5f ? 0.5f : grid_s, angle);\
    } else {
/^    Camera_RebuildProjectionMatrix();$/i\
    }
}
/^void doHeatEffect(/,/^}/{
/^    GXBegin(GX_QUADS, GX_VTXFMT0, 4);$/i\
    if (port_screen_warp_enabled()) {\
        port_heat_grid(fA, fB, mulY, mulX, a2, a1);\
    } else {
/^    Camera_RebuildProjectionMatrix();$/i\
    }
}
