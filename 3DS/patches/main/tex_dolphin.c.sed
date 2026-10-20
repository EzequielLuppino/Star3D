# Grass pass budget (GRAPHICS_PLAN.md step 11, port/fur_3ds.c
# port_grass_step): walk the indirect grass passes with a step, ending on the
# top pass; noise textures and heights keep their absolute pass index
1i\
int port_grass_step(int passCount);
s|^    for (; i < passCount; i = i + 1)$|    int fhGrassStep = port_grass_step(passCount);\n    for (i = fhGrassStep - 1; i < passCount; i = i + fhGrassStep)|
# Map sort (port/mapsort_3ds.c S3D_MAP_SORT, DRAWCALL_PLAN.md "Map sort"):
# mapSortAdd / mapSortFlush are appended from tex_dolphin_mapsort.c.inc.
# sMsLit tells it that a piece added point-light stages to its material
1i\
int sMsLit;
/^void mapBlockRender_callList(/,/^}/s|^                    Rcp_ApplyTextureStageCounts();$|                    sMsLit \|= count != 0;\
                    sMdLit \|= count != 0;\
&|
$r patches/main/tex_dolphin_mapsort.c.inc
# Map material dedup (port/mapsort_3ds.c S3D_MAP_DEDUP, DRAWCALL_PLAN.md "Map
# material dedup"): setShader skips setters whose state is already in place,
# in the game's own draw order. The helpers are appended from
# tex_dolphin_mapdedup.c.inc; sMdLit (set next to sMsLit above) tells them a
# piece added point-light stages
1i\
struct Shader;\
extern int sMdLit;\
void mapDedupReset(void);\
void mapDedupCheck(void);\
int mapDedupFog(struct Shader* s);\
int mapDedupPe(struct Shader* s);\
int mapDedupChan(struct Shader* s);\
int mapDedupCull(struct Shader* s);\
int mapDedupTev(struct Shader* s);
/^Shader\* mapBlockRender_setShader(/,/^}/{
s|^    if ((SHADER_FLAGS(shader) \& 4) != 0) {$|    if (mapDedupFog(shader)) {\
    } else if ((SHADER_FLAGS(shader) \& 4) != 0) {|
s|^    if ((shader != 0) \&\& ((SHADER_FLAGS(shader) \& 0x80000000) != 0)) {$|    mapDedupCheck();\
&|
s|^    return shader;$|    mapDedupCheck();\
&|
s|^        res = AttractMovie_DrawTextureCallback(0, 0, 0);$|&\
        mapDedupReset();|
s|^    Rcp_ResetTextureStageState();$|    if (!mapDedupTev(shader)) {\
&|
/^        renderHeavyFog(fogRgba);$/{
n
s|^    }$|    }\
    }|
}
s|^    if (((SHADER_FLAGS(shader) \& 0x40000000) != 0) \|\| ((SHADER_FLAGS(shader) \& 0x20000000) != 0)) {$|    if (mapDedupPe(shader)) {\
    } else if (((SHADER_FLAGS(shader) \& 0x40000000) != 0) \|\| ((SHADER_FLAGS(shader) \& 0x20000000) != 0)) {|
s|^    if ((SHADER_FLAGS(shader) \& 1) != 0 \|\| (SHADER_FLAGS(shader) \& 0x40000) != 0 \|\|$|    if (mapDedupChan(shader)) {\
    } else if ((SHADER_FLAGS(shader) \& 1) != 0 \|\| (SHADER_FLAGS(shader) \& 0x40000) != 0 \|\||
s|^    if ((SHADER_FLAGS(shader) \& 0x8) != 0) {$|    if (mapDedupCull(shader)) {\
    } else if ((SHADER_FLAGS(shader) \& 0x8) != 0) {|
}
$r patches/main/tex_dolphin_mapdedup.c.inc
