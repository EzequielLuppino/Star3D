#pragma once
#include <citro3d.h>

typedef struct {
    u32 fragOpMode;
    u32 fragOpShadow;
    u32 zScale, zOffset;
    GPU_CULLMODE cullMode;
    bool zBuffer, earlyDepth;
    GPU_EARLYDEPTHFUNC earlyDepthFunc;
    u32 earlyDepthRef;
    u32 alphaTest;
    u32 stencilMode, stencilOp;
    u32 depthTest;
    u32 blendClr;
    u32 alphaBlend;
    GPU_LOGICOP clrLogicOp;
} GxC3DEffect;

typedef struct {
    gxCmdQueue_s gxQueue;
    u32* cmdBuf;
    size_t cmdBufSize;
    float cmdBufUsage;
    u32 flags;
    shaderProgram_s* program;
    C3D_AttrInfo attrInfo;
    C3D_BufInfo bufInfo;
    GxC3DEffect effect;
    C3D_LightEnv* lightEnv;
    u32 texConfig;
    u32 texShadow;
    C3D_Tex* tex[3];
    C3D_TexEnv texEnv[6];
    u32 texEnvBuf, texEnvBufClr;
    u32 fogClr;
    C3D_FogLut* fogLut;
    u16 gasAttn, gasAccMax;
    u32 gasLightXY, gasLightZ, gasLightZColor;
    u32 gasDeltaZ : 24;
    u32 gasFlags : 8;
    C3D_GasLut* gasLut;
    C3D_ProcTex* procTex;
    C3D_ProcTexLut* procTexLut[3];
    C3D_ProcTexColorLut* procTexColorLut;
    C3D_FrameBuf fb;
    u32 viewport[5];
    u32 scissor[3];
    u16 fixedAttribDirty, fixedAttribEverDirty;
    C3D_FVec fixedAttribs[12];
} GxC3DContext;

enum {
    GXC3D_F_EFFECT = BIT(4),
    GXC3D_F_FRAMEBUF = BIT(5),
    GXC3D_F_VIEWPORT = BIT(6),
    GXC3D_F_SCISSOR = BIT(7),
    GXC3D_F_PROGRAM = BIT(8),
};
#define GXC3D_F_TEX(n) BIT(23 + (n))

extern GxC3DContext __C3D_Context;

// gx_c3d_queue.c: citro3d's frame queue, with a pipelined mode
bool C3Dx_SetPipelined(bool on);  // Between frames; returns the mode now in effect
bool C3Dx_Pipelined(void);
bool C3Dx_WaitIdle(s64 timeout);  // Everything queued so far (ns, -1 = no limit)
float C3Dx_GetWaitTime(void);     // ms the last C3D_FrameEnd waited for the previous frame
bool C3Dx_WaitPrevFrame(s64 timeout);  // The last submitted frame only
void C3Dx_SetSplitFlushHook(void (*hook)(void));  // Mid-frame cache flush (pipelined)
void C3Dx_Recycle(void);         // Pipelined: clear the queue if the GPU is done with it (render thread)
u32 C3Dx_Recycles(void);
void C3Dx_SetPacing(int bits);   // S3D_RT_PACING: 1 swap from GSP callbacks, 2 recycle the queue
u32 C3Dx_ForcedClears(void);      // Queue full: waits for the GPU to clear it
