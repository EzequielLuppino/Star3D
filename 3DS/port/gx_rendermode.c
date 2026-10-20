// Standard Dolphin SDK render mode used by the game (NTSC 640x480 interlaced,
// double-strike XFB). The game reads fbWidth/efbHeight/xfbHeight from it to size
// its framebuffers; values match the SDK's GXNtsc480IntDf.

#include <dolphin/gx.h>

GXRenderModeObj GXNtsc480IntDf = {
    VI_TVMODE_NTSC_INT,
    640,              // fbWidth
    480,              // efbHeight
    480,              // xfbHeight
    (720 - 640) / 2,  // viXOrigin
    (480 - 480) / 2,  // viYOrigin
    640,              // viWidth
    480,              // viHeight
    VI_XFBMODE_DF,
    GX_FALSE,         // field_rendering
    GX_FALSE,         // aa
    {
        { 6, 6 }, { 6, 6 }, { 6, 6 },
        { 6, 6 }, { 6, 6 }, { 6, 6 },
        { 6, 6 }, { 6, 6 }, { 6, 6 },
        { 6, 6 }, { 6, 6 }, { 6, 6 },
    },
    { 8, 8, 10, 12, 10, 8, 8 },
};
