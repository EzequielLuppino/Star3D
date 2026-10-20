# Indirect fallback counters: GXSetTevIndirect calls are counted per calling
# function (gx_tev.c port_tev_indirect)
1i\
#include "dolphin/gx/GXBump.h"\
void port_tev_indirect(const char* func, GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,\
                       GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,\
                       GXBool add_prev, GXBool ind_lod, GXIndTexAlphaSel alpha_sel);
s|\bGXSetTevIndirect(|port_tev_indirect(__func__, |g
# The C-menu ring (items / staff powers / Tricky) is drawn on the bottom
# screen next to the minimap, always (port/touchui_3ds.c, CONTROLS.md): the
# block runs inside a HUD pass mapped to the ring's region, with the fade
# forced to full so the ring also shows while the menu is closed (the menu's
# open state, which decides who gets A, is untouched). The item counts go to
# the touch UI, their text box (0x93) is shared with the top screen's HUD.
s|^void hudDrawButtons(int cMenuArg0, int cMenuArg1, int cMenuArg2);$|&\
int port_cmenu_hud_begin(void);\
void port_cmenu_hud_end(void);\
void port_cmenu_count(const char* text, int x, int y, int alpha);|
s|^    if ((cMenuFadeCounter != 0) \&\& (cMenuEnabled != 0))$|    int fhRing = port_cmenu_hud_begin();\
&|
s|^\(                gameTextShowStr(label.text, 0x93, 0x247, 0x2B + (gCMenuScrollTimer + k));\)$|                if (!fhRing)\
    \1|
s|^\(                gameTextShowStr(label.text, 0x93, 0x246, 0x2A + (gCMenuScrollTimer + k));\)$|                if (fhRing)\
                    port_cmenu_count(label.text, 0x246, 0x2A + (gCMenuScrollTimer + k), fadedAlpha);\
                else\
    \1|
s|^    if (hudYButtonItemIconTexture != NULL \&\& gHudYButtonItemTextureCache != yButtonItemTextureId)$|    if (fhRing)\
        port_cmenu_hud_end();\
&|
