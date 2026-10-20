// Startup self-test (make SELFTEST=1): draws known patterns through the GX
// emulation, reads the framebuffer back and logs where they landed. Used to
// verify how EFB coordinates map to framebuffer memory and texture space.

#ifdef GX_SELFTEST

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>
#include "gx_init.h"
#include "gx_internal.h"
#include "log.h"

static void demorton(u32 i, u32* x, u32* y) {
    *x = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
    *y = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
}

// Bounding box (in framebuffer memory column / row) of pixels matching a test
static void find_bbox(const char* what, const u32* fb, u32 w, u32 h, u32 mask, u32 value) {
    u32 x0 = w, y0 = h, x1 = 0, y1 = 0, n = 0;
    for (u32 i = 0; i < w * h; i++) {
        if ((fb[i] & mask) != value) continue;
        u32 tile = i / 64, tx, ty;
        demorton(i % 64, &tx, &ty);
        u32 x = (tile % (w / 8)) * 8 + tx;
        u32 y = (tile / (w / 8)) * 8 + ty;
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x > x1) x1 = x;
        if (y > y1) y1 = y;
        n++;
    }
    LOG_INFO("selftest", "%s: %lu px, memory cols %lu..%lu rows %lu..%lu", what, (unsigned long)n,
             (unsigned long)x0, (unsigned long)x1, (unsigned long)y0, (unsigned long)y1);
}

static void quad(float x0, float y0, float x1, float y1, bool tex) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x0, y0, -0.5f); if (tex) GXTexCoord2f32(0, 0); else GXColor4u8(255, 0, 0, 255);
    GXPosition3f32(x1, y0, -0.5f); if (tex) GXTexCoord2f32(1, 0); else GXColor4u8(255, 0, 0, 255);
    GXPosition3f32(x1, y1, -0.5f); if (tex) GXTexCoord2f32(1, 1); else GXColor4u8(255, 0, 0, 255);
    GXPosition3f32(x0, y1, -0.5f); if (tex) GXTexCoord2f32(0, 1); else GXColor4u8(255, 0, 0, 255);
    GXEnd();
}

// I8 8x8 texture: top-left quadrant 0xFF, rest 0x00 (two 8x4 tiles)
static u8 s_tex[64] __attribute__((aligned(32)));

void gx_selftest(void) {
    LOG_INFO("selftest", "start");
    GXInit(NULL, 0);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) s_tex[y * 8 + x] = 0xFF;

    float ortho[4][4] = { { 2.0f / 640, 0, 0, -1 }, { 0, -2.0f / 480, 0, 1 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    float ident[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
    GXSetProjection(ortho, GX_ORTHOGRAPHIC);
    GXSetViewport(0, 0, 640, 480, 0, 1);
    GXLoadPosMtxImm(ident, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTevStages(1);

    gx_set_clear_black(true);
    gx_frame_begin();

    // A: solid red at EFB (0,0)-(64,32), the top-left corner of the screen
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumTexGens(0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    quad(0, 0, 64, 32, false);

    // B: textured quad at EFB (320,240)-(640,480), s/t = 0 at its top-left;
    // white should land in its top-left quadrant (320,240)-(480,360)
    GXTexObj obj;
    GXInitTexObj(&obj, s_tex, 8, 8, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
    GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    quad(320, 240, 640, 480, true);

    // C: copy EFB (0,0)-(128,64) (the red rectangle) with clear to green, then
    // draw the copy at EFB (0,240)-(320,480): red should land in its top-left
    // quarter (0,240)-(160,320), green where the copy was taken
    static u8 copybuf[128 * 64 * 4] __attribute__((aligned(32)));
    GXSetCopyClear((GXColor){ 0, 255, 0, 255 }, 0xFFFFFF);
    GXSetTexCopySrc(0, 0, 128, 64);
    GXSetTexCopyDst(128, 64, GX_TF_RGBA8, GX_FALSE);
    GXCopyTex(copybuf, GX_TRUE);
    GXInitTexObj(&obj, copybuf, 128, 64, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(&obj, GX_TEXMAP0);
    quad(0, 240, 320, 480, true);

    gx_frame_end();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // Waits for the frame above
    C3D_FrameEnd(0);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_FrameEnd(0);

    C3D_RenderTarget* t = gx_top_target();
    const u32* fb = (const u32*)t->frameBuf.colorBuf;
    u32 w = t->frameBuf.width, h = t->frameBuf.height;
    LOG_INFO("selftest", "framebuffer %lux%lu at %p", (unsigned long)w, (unsigned long)h, fb);
    find_bbox("red (expected cols 60..119 rows 40..119)", fb, w, h, 0xFFFFFF00, 0xFF000000);
    find_bbox("white (expected cols 60..119 rows 200..279)", fb, w, h, 0xFFFFFF00, 0xFFFFFF00);
    find_bbox("green (expected cols 208..239 rows 40..103)", fb, w, h, 0xFFFFFF00, 0x00FF0000);
    // Colors inside quad C (cols 0..119, rows 40..199), sampled every 8 px
    for (u32 r = 40; r < 200; r += 20) {
        char line[160];
        int n = 0;
        for (u32 c = 4; c < 120; c += 16) {
            u32 i = ((r / 8) * (w / 8) + c / 8) * 64;
            u32 tx, ty, k;
            for (k = 0; k < 64; k++) { demorton(k, &tx, &ty); if (tx == (c & 7) && ty == (r & 7)) break; }
            n += snprintf(line + n, sizeof(line) - n, " %08lX", (unsigned long)fb[i + k]);
        }
        LOG_INFO("selftest", "row %3lu:%s", (unsigned long)r, line);
    }
    const GxCopyTex* ct = gx_copytex_find(copybuf);
    if (ct) LOG_INFO("selftest", "copy tex %ux%u at %p au %.3f %.3f %.3f av %.3f %.3f %.3f", ct->tex.width,
                     ct->tex.height, ct->tex.data, ct->au[0], ct->au[1], ct->au[2], ct->av[0], ct->av[1], ct->av[2]);
    else LOG_INFO("selftest", "copy tex not registered");
    gx_set_clear_black(false);
    LOG_INFO("selftest", "done");
}

#endif
