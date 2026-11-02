# "3D: reduced" (GRAPHICS_PLAN.md step 11, gx_render.c port_stereo_reduced):
# stereo frames skip motion blur and the DOF/blur filter
1i\
_Bool port_stereo_reduced(void);
s|^    if (CameraShake_IsActive() != 0 \|\| (int)bEnableMotionBlur != 0) {$|    if ((CameraShake_IsActive() != 0 \|\| (int)bEnableMotionBlur != 0) \&\& !port_stereo_reduced()) {|
s|^    if (bEnableBlurFilter != 0) {$|    if (bEnableBlurFilter != 0 \&\& !port_stereo_reduced()) {|
# Shadows setting (touch UI, S3D_SHADOWS, gx_init.c port_shadows_enabled):
# off queues no shadow casters and renders no object shadow textures
1i\
_Bool port_shadows_enabled(void);
s|^                    (o->anim.modelState->flags \& OBJ_MODEL_STATE_SHADOW_VISIBLE) != 0) {$|                    (o->anim.modelState->flags \& OBJ_MODEL_STATE_SHADOW_VISIBLE) != 0 \&\& port_shadows_enabled()) {|
s|^    ff = 1.333333f;$|    ff = 2.0f;|
# Map sort (port/mapsort_3ds.c S3D_MAP_SORT, DRAWCALL_PLAN.md "Map sort"):
# pass 0 collects the visible blocks and draws them grouped by material
1i\
int port_map_sort(void);\
void mapSortAdd(struct MapBlockData* block, float rowF, float colF);\
void mapSortFlush(void);
/^void renderSceneGeometry(u8 renderType, s8\* order) {$/,/^}/{
s|^    layer = 4;$|    int fhSort = renderType == 0 \&\& port_map_sort() > 0;\
    mapDedupBegin(renderType);\
&|
s|^                    renderMapBlock(block, renderType);$|                    if (fhSort) mapSortAdd(block, rowF, colF);\
                    else renderMapBlock(block, renderType);|
s|^    } while (layer >= 0);$|&\
    if (fhSort) mapSortFlush();\
    mapDedupEnd();|
}
# Map material dedup (port/mapsort_3ds.c S3D_MAP_DEDUP, DRAWCALL_PLAN.md "Map
# material dedup", tex_dolphin_mapdedup.c.inc): armed for pass 0 only, where
# nothing but map blocks touches GX state
1i\
void mapDedupBegin(int renderType);\
void mapDedupEnd(void);
# Shadow decals (gx_init.c port_shadow_decals): queue one flat ground-shadow
# quad per shadowed object instead of the caster RTT passes (renderShadows
# then sees no casters: no object redraws, no copies, no receiver redraws).
# Only objects that passed the distance cuts and the grass quality reach this
# loop, so the decals follow the LOD settings.
1i\
_Bool port_shadow_decals(void);
s|^                    if (t == 2 \|\| t == 1) {$|                    if (port_shadow_decals()) {\
                        if (t == 2 \|\| t == 1 \|\| t == 4) {\
                            renderShadowType3(o, 7, 0);\
                            gLightmapDrawQueue[gLightmapDrawQueueCount].d = 2;\
                            gLightmapDrawQueueCount++;\
                        }\
                    } else if (t == 2 \|\| t == 1) {|
