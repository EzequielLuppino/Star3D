// Draw-source attribution (make PROFILE=1 / PERF_WRAP=1). The --wrap timers in
// perf_wrap.c push a source tag around the game's render entry points; every
// GPU draw is charged to the innermost tag and to the game code that issued
// it (return address of GXCallDisplayList / GXEnd). Report: sdmc:/3ds/star3d/Star3D.log.

#ifndef DRAW_SRC_H
#define DRAW_SRC_H

#include <3ds.h>

typedef enum {
    DSRC_OTHER,    // Outside every tagged function
    DSRC_SCENE,    // sceneRender, not inside a narrower tag
    DSRC_TRANSP,   // sceneDrawTransparentPolys queue, not inside a narrower tag
    DSRC_MAP,      // renderMapBlock
    DSRC_MAPPASS,  // mapBlockRender_drawLightmapIndirectPasses (lightmap/grass passes)
    DSRC_OBJ,      // objRender
    DSRC_FUR,      // objRenderFuzz, playerRenderFuzz
    DSRC_SHADOW,   // objShadowRender, objDrawGroundShadow
    DSRC_EXPGFX,   // expgfx_renderSourcePools, drawGlow, renderParticles
    DSRC_SKY,      // drawSkyStars, renderClouds
    DSRC_WATER,    // waterFxDraw
    DSRC_REFL,     // updateReflectionTextures, drawReflectionTexture
    DSRC_POSTFX,   // screen image, motion blur, heat, blur, distortion, lightning
    DSRC_HUD,      // curUiDllDraw, gameTextRun, mainLoopDoGameText
    DSRC_COUNT
} DrawSrcTag;

#ifdef PERF_WRAP
void draw_src_push(DrawSrcTag tag);
void draw_src_pop(void);
void draw_src_set_caller(void* pc);  // Game code issuing the next draws (NULL = none)
void draw_src_batch_begin(void);     // An immediate-mode batch starts: latch tag and caller
void draw_src_draw(u32 tris);        // A display-list draw now
void draw_src_draw_batch(u32 tris);  // The latched immediate-mode batch is drawn
void draw_src_frame_end(void);
#define DRAW_SRC_BATCH_BEGIN()   draw_src_batch_begin()
#define DRAW_SRC_DRAW(tris)      draw_src_draw(tris)
#define DRAW_SRC_DRAW_BATCH(tris) draw_src_draw_batch(tris)
#define DRAW_SRC_FRAME_END()     draw_src_frame_end()
#else
#define DRAW_SRC_BATCH_BEGIN()   ((void)0)
#define DRAW_SRC_DRAW(tris)      ((void)0)
#define DRAW_SRC_DRAW_BATCH(tris) ((void)0)
#define DRAW_SRC_FRAME_END()     ((void)0)
#endif

#endif
