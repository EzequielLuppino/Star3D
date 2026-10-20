// Replacement for citro3d 1.7.1's renderqueue.c (frame begin/end, render
// targets, the gx command queue). It defines every symbol of that file, so the
// linker never pulls the library's copy (a missing one would show up as a
// duplicate definition at link time). Keep it in sync if citro3d is updated.
//
// Pipelined mode (C3Dx_SetPipelined, PIPELINE_PLAN.md): stock citro3d keeps the
// gx queue stopped during a frame, runs it in C3D_FrameEnd and makes the next
// C3D_FrameBegin wait for it, so the CPU and the GPU take turns (hub on a New
// 3DS: 65 ms CPU + 38 ms GPU = 103 ms per frame). Pipelined, C3D_FrameBegin
// does not wait: the next frame is built in the second command buffer while
// the GPU still runs the previous one, and C3D_FrameEnd waits for the
// previous frame just before submitting this one. The queue keeps running
// during the frame, so work submitted mid-frame (C3D_FrameSplit) starts at once.
// Everything the CPU writes for the GPU must then survive one extra frame:
// see gx_gpu_* in gx_init.c.
//
// Not pipelined, this behaves like stock citro3d, except that waits run the
// queue first and screen swaps can't be lost (see swap_pending).

#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <string.h>
#include "gx_c3d_ctx.h"
#include "profiler.h"
#include "frametrace_3ds.h"

// From citro3d's base.c (not static there, but not in its public headers)
bool C3Di_SplitFrame(u32** pBuf, u32* pSize);

#define QUEUE_ENTRIES 256  // citro3d: 32. Pipelined, the queue is only cleared when idle
#define QUEUE_HEADROOM 48  // Free entries wanted before a frame or a split adds more

static C3D_RenderTarget *firstTarget, *lastTarget;
static C3D_RenderTarget* linkedTarget[3];

static TickCounter gpuTime, cpuTime;
static u64 waitTicks;  // C3D_FrameEnd's wait for the previous frame

static bool inFrame, inSafeTransfer, measureGpuTime;
static float framerate = 60.0f;
static float framerateCounter[2] = { 60.0f, 60.0f };
static u32 frameCounter[2];
static void (*frameEndCb)(void*);
static void* frameEndCbData;

static bool pipelined;
static bool swapPoll = true;  // C3Dx_SetPacing bit 0: swap from the GSP callbacks
static bool recycle = true;   // C3Dx_SetPacing bit 1: C3Dx_Recycle clears an idle queue
static u32 recycles;
static u32* cmdBufs[2];
static int cmdBufCur;
static u16 prevEnd;  // Queue index just past the last submitted frame's entries
static void (*splitFlushHook)(void);
static u32 forcedClears;

// Screen swaps for the last submitted frame. citro3d swaps in the queue's
// completion callback. libctru's gxCmdQueueWait returns as soon as the GPU is
// idle, before that callback has run (it runs on the GSP event thread), so a
// frame end right after a wait could queue its transfers into the buffer
// still on screen, and a late callback could then swap the new frame's
// buffers before they were written. Whichever side finds the swap pending
// first does it, under the lock; the callback only when the whole queue is
// done. Pipelined, the queue rarely drains, so the swap is also done as soon
// as the GPU got past the frame's transfers (swapAfter, poll_swap)
enum { SWAP_TOP = 1, SWAP_BOTTOM = 2, SWAP_TOP_STEREO = 4 };
static u32 swap_pending;
static u16 swapAfter;  // Queue index just past the transfers swap_pending belongs to
static LightLock swap_lock = 1;

static GxC3DContext* ctx(void) { return &__C3D_Context; }

// From citro3d's internal.h
static inline bool addrIsVRAM(const void* addr) {
    u32 vaddr = (u32)addr;
    return vaddr >= OS_VRAM_VADDR && vaddr < OS_VRAM_VADDR + OS_VRAM_SIZE;
}

static inline vramAllocPos addrGetVRAMBank(const void* addr) {
    u32 vaddr = (u32)addr;
    return vaddr < OS_VRAM_VADDR + OS_VRAM_SIZE / 2 ? VRAM_ALLOC_A : VRAM_ALLOC_B;
}

static bool framerateLimit(int id) {
    framerateCounter[id] -= framerate;
    if (framerateCounter[id] <= 0.0f) {
        framerateCounter[id] += 60.0f;
        return true;
    }
    return false;
}

static void poll_swap_by(u32 who);

// Pipelined, the queue rarely drains, so the completion callback rarely swaps:
// the swap used to wait for the next C3D_FrameEnd on the render thread, up to a
// frame late and at an uneven point (frame trace 7AED0EF9: 1-7 VBlanks per
// frame instead of 3-4). Every GPU completion and VBlank checks whether the
// last frame's transfers are done and swaps then, as the GPU finishes
// GSP events seen by the callbacks below, on the GSP event thread: P3D, PPF,
// VBlank0, VBlank1 (S3D_PROBE reports them, port/probe_3ds.c)
volatile u32 g_gsp_event_counts[4];

static void onP3D(void* unused) {
    (void)unused;
    g_gsp_event_counts[0]++;
    ft_ev(FT_GPU_P3D, 0);
    if (pipelined && swapPoll) poll_swap_by(9);
}

static void onPPF(void* unused) {
    (void)unused;
    g_gsp_event_counts[1]++;
    ft_ev(FT_GPU_PPF, 0);
    if (pipelined && swapPoll) poll_swap_by(9);
}

static void onVBlank0(void* unused) {
    (void)unused;
    g_gsp_event_counts[2]++;
    ft_ev(FT_VBLANK0, 0);
    if (pipelined && swapPoll) poll_swap_by(9);
    if (framerateLimit(0)) frameCounter[0]++;
}

static void onVBlank1(void* unused) {
    (void)unused;
    g_gsp_event_counts[3]++;
    if (framerateLimit(1)) frameCounter[1]++;
}

// who: frame trace only (thread slot, 9 = the GSP thread's completion callback)
static void do_swaps(u32 who) {
    u32 s = swap_pending;
    if (s) ft_ev(FT_Q_SWAP, who);
    swap_pending = 0;
    if (s & SWAP_TOP) gfxScreenSwapBuffers(GFX_TOP, (s & SWAP_TOP_STEREO) != 0);
    if (s & SWAP_BOTTOM) gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

static void onQueueFinish(gxCmdQueue_s* queue) {
    if (measureGpuTime) {
        osTickCounterUpdate(&gpuTime);
        measureGpuTime = false;
    }
    if (inSafeTransfer) {
        inSafeTransfer = false;
        if (inFrame) {
            gxCmdQueueStop(queue);
            gxCmdQueueClear(queue);
        }
        return;
    }
    LightLock_Lock(&swap_lock);
    // A late callback must not swap a frame whose transfers were queued after
    // the wait that this callback belongs to
    if (swap_pending && queue->numEntries && queue->lastEntry == queue->numEntries) do_swaps(9);
    LightLock_Unlock(&swap_lock);
}

// Wait for everything in the queue. libctru's wait returns at once on a
// stopped queue even with entries pending, so run it first. Then do a swap
// the completion callback has not done yet
static bool queue_wait(s64 timeout) {
    gxCmdQueue_s* q = &ctx()->gxQueue;
    gxCmdQueueRun(q);
    ft_ev(FT_Q_WAIT_B, 0);
    bool ok = gxCmdQueueWait(q, timeout);
    ft_ev(FT_Q_WAIT_E, 0);
    if (!ok) return false;
    LightLock_Lock(&swap_lock);
    if (swap_pending) do_swaps((u32)t_thread_slot);
    LightLock_Unlock(&swap_lock);
    return true;
}

// Only on an idle queue: libctru's stop on a busy queue drops the completion
// count of commands still in flight
static void queue_clear_idle(void) {
    gxCmdQueue_s* q = &ctx()->gxQueue;
    gxCmdQueueStop(q);
    gxCmdQueueClear(q);
    prevEnd = 0;
    swapAfter = 0;
}

static bool C3Di_WaitAndClearQueue(s64 timeout) {
    if (!queue_wait(timeout)) return false;
    queue_clear_idle();
    return true;
}

// Swap the last frame's screens once the GPU got past its transfers. who:
// frame trace only (thread slot, 9 = GSP thread)
static void poll_swap_by(u32 who) {
    gxCmdQueue_s* q = &ctx()->gxQueue;
    LightLock_Lock(&swap_lock);
    if (swap_pending && q->lastEntry >= swapAfter) do_swaps(who);
    LightLock_Unlock(&swap_lock);
}

static void poll_swap(void) {
    poll_swap_by((u32)t_thread_slot);
}

// Pipelined: wait until the GPU has completed the queue up to index target.
// Polls, because libctru only signals when the whole queue is done, and
// pipelined it rarely is (this frame's mid-frame work follows the previous
// frame's)
static bool wait_entries(u16 target, s64 timeout) {
    gxCmdQueue_s* q = &ctx()->gxQueue;
    if (target > q->numEntries) target = q->numEntries;
    if (q->lastEntry >= target) return true;
    gxCmdQueueRun(q);
    u64 start = svcGetSystemTick();
    ft_ev(FT_Q_WAIT_B, 1);
    while (*(volatile u16*)&q->lastEntry < target) {
        // Not running with work left would never finish (only possible if the
        // queue was stopped while busy)
        if (gxCmdQueueWait(q, 0) && *(volatile u16*)&q->lastEntry < target) return false;
        if (timeout >= 0 && (s64)(svcGetSystemTick() - start) * 1000 / (SYSCLOCK_ARM11 / 1000000) > timeout)
            return false;
        svcSleepThread(50000);
    }
    ft_ev(FT_Q_WAIT_E, 1);
    return true;
}

// Pipelined: entries are only removed by clearing an idle queue. Clear when
// the GPU happens to be idle; when the queue is nearly full, wait for that
static void queue_make_room(u32 needed) {
    gxCmdQueue_s* q = &ctx()->gxQueue;
    if (q->numEntries && q->lastEntry == q->numEntries && gxCmdQueueWait(q, 0)) {
        poll_swap();
        queue_clear_idle();
    }
    if (q->numEntries + needed > q->maxEntries) {
        queue_wait(-1);
        queue_clear_idle();
        forcedClears++;
    }
    gxCmdQueueRun(q);  // Active (e.g. after a clear elsewhere): entries added later start at once
}

void C3D_FrameSync(void) {
    u32 cur[2];
    u32 start[2] = { frameCounter[0], frameCounter[1] };
    do {
        gspWaitForAnyEvent();
        cur[0] = frameCounter[0];
        cur[1] = frameCounter[1];
    } while (cur[0] == start[0] || cur[1] == start[1]);
}

u32 C3D_FrameCounter(int id) {
    return frameCounter[id];
}

void C3Di_RenderQueueEnableVBlank(void) {
    gspSetEventCallback(GSPGPU_EVENT_VBlank0, onVBlank0, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_VBlank1, onVBlank1, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_P3D, onP3D, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_PPF, onPPF, NULL, false);
}

void C3Di_RenderQueueDisableVBlank(void) {
    gspSetEventCallback(GSPGPU_EVENT_VBlank0, NULL, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_VBlank1, NULL, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_P3D, NULL, NULL, false);
    gspSetEventCallback(GSPGPU_EVENT_PPF, NULL, NULL, false);
}

void C3Di_RenderQueueInit(void) {
    GxC3DContext* c = ctx();
    // The queue is empty and not yet bound: give it more entries
    gxCmdEntry_s* entries = realloc(c->gxQueue.entries, QUEUE_ENTRIES * sizeof(gxCmdEntry_s));
    if (entries) {
        c->gxQueue.entries = entries;
        c->gxQueue.maxEntries = QUEUE_ENTRIES;
    }
    cmdBufs[0] = c->cmdBuf;
    cmdBufCur = 0;

    C3Di_RenderQueueEnableVBlank();

    GX_BindQueue(&c->gxQueue);
    gxCmdQueueSetCallback(&c->gxQueue, onQueueFinish, NULL);
    gxCmdQueueRun(&c->gxQueue);
}

static void C3Di_RenderTargetDestroy(C3D_RenderTarget* target);

void C3Di_RenderQueueExit(void) {
    C3D_RenderTarget *a, *next;

    C3Di_WaitAndClearQueue(-1);
    gxCmdQueueSetCallback(&ctx()->gxQueue, NULL, NULL);
    GX_BindQueue(NULL);

    C3Di_RenderQueueDisableVBlank();

    for (int i = 0; i < 3; i++) linkedTarget[i] = NULL;

    for (a = firstTarget; a; a = next) {
        next = a->next;
        C3Di_RenderTargetDestroy(a);
    }

    // citro3d's C3D_Fini frees ctx->cmdBuf: hand it the first buffer back
    if (cmdBufs[1]) {
        ctx()->cmdBuf = cmdBufs[0];
        linearFree(cmdBufs[1]);
        cmdBufs[1] = NULL;
    }
    pipelined = false;
}

void C3Di_RenderQueueWaitDone(void) {
    C3Di_WaitAndClearQueue(-1);
}

float C3D_FrameRate(float fps) {
    float old = framerate;
    if (fps > 0.0f && fps <= 60.0f) {
        framerate = fps;
        framerateCounter[0] = fps;
        framerateCounter[1] = fps;
    }
    return old;
}

bool C3D_FrameBegin(u8 flags) {
    GxC3DContext* c = ctx();
    if (inFrame) return false;

    if (flags & C3D_FRAME_SYNCDRAW) C3D_FrameSync();
    if (!pipelined) {
        if (!C3Di_WaitAndClearQueue((flags & C3D_FRAME_NONBLOCK) ? 0 : -1)) return false;
    } else {
        // The other buffer's frame finished: the last C3D_FrameEnd waited for it
        cmdBufCur ^= 1;
        c->cmdBuf = cmdBufs[cmdBufCur];
        poll_swap();
        queue_make_room(QUEUE_HEADROOM);
    }

    inFrame = true;
    osTickCounterStart(&cpuTime);
    GPUCMD_SetBuffer(c->cmdBuf, c->cmdBufSize, 0);
    return true;
}

bool C3D_FrameDrawOn(C3D_RenderTarget* target) {
    if (!inFrame) return false;

    target->used = true;
    C3D_SetFrameBuf(&target->frameBuf);
    C3D_SetViewport(0, 0, target->frameBuf.width, target->frameBuf.height);
    return true;
}

static void flush_linear_heap(void) {
    extern u32 __ctru_linear_heap, __ctru_linear_heap_size;
    GSPGPU_FlushDataCache((void*)__ctru_linear_heap, __ctru_linear_heap_size);
}

// Pipelined, the queue may run what is submitted here at once, so the CPU
// cache is written back first (stock citro3d does that only at frame end,
// before it starts the queue). Mid-frame, the hook (C3Dx_SetSplitFlushHook)
// flushes what was written since the last split; without one, the whole
// linear heap (~0.4 ms each on a New 3DS). GX_CMDLIST_FLUSH makes
// GX_ProcessCommandList flush the command list itself
enum { FLUSH_NONE, FLUSH_HOOK, FLUSH_ALL };
static void split(u8 flags, int flush) {
    u32 *cmdBuf, cmdBufSize;
    if (!inFrame) return;
    if (C3Di_SplitFrame(&cmdBuf, &cmdBufSize)) {
        if (flush == FLUSH_HOOK && splitFlushHook) {
            PROF_ZONE("gpu.split_flush");
            splitFlushHook();
            flags |= GX_CMDLIST_FLUSH;
        } else if (flush != FLUSH_NONE) {
            PROF_ZONE("gpu.split_flush");
            flush_linear_heap();
        }
        GX_ProcessCommandList(cmdBuf, cmdBufSize * 4, flags);
        ft_ev(FT_Q_SUBMIT, flush == FLUSH_ALL || flush == FLUSH_NONE ? 1 : 0);
    }
}

void C3D_FrameSplit(u8 flags) {
    if (pipelined) queue_make_room(QUEUE_HEADROOM);
    split(flags, pipelined ? FLUSH_HOOK : FLUSH_NONE);
}

void C3D_FrameEnd(u8 flags) {
    GxC3DContext* c = ctx();
    if (!inFrame) return;

    if (frameEndCb) frameEndCb(frameEndCbData);

    osTickCounterUpdate(&cpuTime);
    if (pipelined) {
        // Wait for the previous frame only, not for this frame's mid-frame
        // work (it keeps running): the previous frame's screen transfers must
        // land and swap before this frame's are queued, and its command
        // buffer is the next frame's. The queue keeps running, so the whole
        // linear heap is flushed before anything is added (citro3d flushes
        // after, with the queue stopped)
        u64 t0 = svcGetSystemTick();
        if (!wait_entries(prevEnd, -1)) queue_wait(-1);  // Inconsistent queue state: wait for all
        poll_swap();
        queue_make_room(QUEUE_HEADROOM);
        waitTicks = svcGetSystemTick() - t0;
        split(flags, (flags & GX_CMDLIST_FLUSH) ? FLUSH_NONE : FLUSH_ALL);
        GPUCMD_SetBuffer(NULL, 0, 0);
        inFrame = false;
    } else {
        waitTicks = 0;
        split(flags, FLUSH_NONE);
        GPUCMD_SetBuffer(NULL, 0, 0);
        inFrame = false;

        // Flush the entire linear memory if the user did not explicitly mandate to flush the command list
        if (!(flags & GX_CMDLIST_FLUSH)) flush_linear_heap();
    }

    u32 swaps = 0;
    for (int i = 2; i >= 0; i--) {
        C3D_RenderTarget* target = linkedTarget[i];
        if (!target || !target->used) continue;
        target->used = false;
        C3D_FrameBufTransfer(&target->frameBuf, target->screen, target->side, target->transferFlags);
        if (target->screen == GFX_TOP) {
            swaps |= SWAP_TOP;
            if (target->side == GFX_RIGHT) swaps |= SWAP_TOP_STEREO;
        } else if (target->screen == GFX_BOTTOM) {
            swaps |= SWAP_BOTTOM;
        }
    }
    // After the transfers are in the queue (see onQueueFinish). Pipelined
    // they may run before this: poll_swap catches that later
    LightLock_Lock(&swap_lock);
    swap_pending = swaps;
    swapAfter = c->gxQueue.numEntries;
    LightLock_Unlock(&swap_lock);
    prevEnd = c->gxQueue.numEntries;

    measureGpuTime = true;
    osTickCounterStart(&gpuTime);
    gxCmdQueueRun(&c->gxQueue);
}

void C3D_FrameEndHook(void (*hook)(void*), void* param) {
    frameEndCb = hook;
    frameEndCbData = param;
}

float C3D_GetDrawingTime(void) {
    return osTickCounterRead(&gpuTime);
}

float C3D_GetProcessingTime(void) {
    return osTickCounterRead(&cpuTime);
}

// --- Additions ---------------------------------------------------------------

// Between frames only. Turning it on allocates the second command buffer
bool C3Dx_SetPipelined(bool on) {
    if (inFrame) return pipelined;
    if (on && !cmdBufs[1]) {
        GxC3DContext* c = ctx();
        cmdBufs[1] = (u32*)linearAlloc(c->cmdBufSize * 4);
        if (!cmdBufs[1]) return pipelined = false;
    }
    pipelined = on;
    return pipelined;
}

bool C3Dx_Pipelined(void) {
    return pipelined;
}

// Wait until the GPU has run everything queued so far (mid-frame work of the
// current frame included). Leaves the queue running
bool C3Dx_WaitIdle(s64 timeout) {
    return queue_wait(timeout);
}

// Wait for the last submitted frame only (not the current frame's mid-frame work)
bool C3Dx_WaitPrevFrame(s64 timeout) {
    bool ok = pipelined ? wait_entries(prevEnd, timeout) || queue_wait(timeout) : queue_wait(timeout);
    poll_swap();
    return ok;
}

// Pipelined, called before a mid-frame submit instead of flushing the whole
// linear heap: it must write back everything the CPU wrote for the GPU
void C3Dx_SetSplitFlushHook(void (*hook)(void)) {
    splitFlushHook = hook;
}

// Times the queue was full and the CPU had to wait for the GPU to clear it
u32 C3Dx_ForcedClears(void) {
    return forcedClears;
}

// Pipelined: clear the queue if the GPU has run everything in it. Entries are
// only removed by clearing an idle queue, and with the render thread ahead of
// the GPU it is rarely idle when a split or a frame end looks (queue_make_room),
// so the queue filled up every ~9 frames and forced a ~25 ms wait for the GPU
// to drain (frame trace 7AED0EF9, S3D_RT_DEDUP=7). The render thread calls this
// when it wakes from waiting for the game thread: the GPU had time to catch up
void C3Dx_Recycle(void) {
    if (!pipelined || !recycle) return;
    gxCmdQueue_s* q = &ctx()->gxQueue;
    if (q->numEntries && q->lastEntry == q->numEntries && gxCmdQueueWait(q, 0)) {
        poll_swap();
        queue_clear_idle();
        recycles++;
    }
}

u32 C3Dx_Recycles(void) {
    return recycles;
}

// S3D_RT_PACING (gx_init.c): bit 0 swap from the GSP callbacks, bit 1 recycle
void C3Dx_SetPacing(int bits) {
    swapPoll = (bits & 1) != 0;
    recycle = (bits & 2) != 0;
}

// Milliseconds the last C3D_FrameEnd waited for the previous frame
float C3Dx_GetWaitTime(void) {
    return (float)waitTicks / (SYSCLOCK_ARM11 / 1000.0f);
}

// --- Render targets (unchanged from citro3d) -------------------------------

static C3D_RenderTarget* C3Di_RenderTargetNew(void) {
    C3D_RenderTarget* target = (C3D_RenderTarget*)malloc(sizeof(C3D_RenderTarget));
    if (!target) return NULL;
    memset(target, 0, sizeof(C3D_RenderTarget));
    return target;
}

static void C3Di_RenderTargetFinishInit(C3D_RenderTarget* target) {
    target->prev = lastTarget;
    target->next = NULL;
    if (lastTarget) lastTarget->next = target;
    if (!firstTarget) firstTarget = target;
    lastTarget = target;
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, C3D_DEPTHTYPE depthFmt) {
    GPU_DEPTHBUF depthFmtReal = GPU_RB_DEPTH16;
    void* depthBuf = NULL;
    void* colorBuf = vramAlloc(C3D_CalcColorBufSize(width, height, colorFmt));
    if (!colorBuf) goto _fail0;
    if (C3D_DEPTHTYPE_OK(depthFmt)) {
        depthFmtReal = C3D_DEPTHTYPE_VAL(depthFmt);
        size_t depthSize = C3D_CalcDepthBufSize(width, height, depthFmtReal);
        vramAllocPos vramBank = addrGetVRAMBank(colorBuf);
        depthBuf = vramAllocAt(depthSize, vramBank ^ VRAM_ALLOC_ANY);  // Attempt opposite bank first...
        if (!depthBuf) depthBuf = vramAllocAt(depthSize, vramBank);   // ... if that fails, attempt same bank
        if (!depthBuf) goto _fail1;
    }

    C3D_RenderTarget* target = C3Di_RenderTargetNew();
    if (!target) goto _fail2;

    C3D_FrameBuf* fb = &target->frameBuf;
    C3D_FrameBufAttrib(fb, width, height, false);
    C3D_FrameBufColor(fb, colorBuf, colorFmt);
    target->ownsColor = true;
    if (depthBuf) {
        C3D_FrameBufDepth(fb, depthBuf, depthFmtReal);
        target->ownsDepth = true;
    }
    C3Di_RenderTargetFinishInit(target);
    return target;

_fail2:
    if (depthBuf) vramFree(depthBuf);
_fail1:
    vramFree(colorBuf);
_fail0:
    return NULL;
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, C3D_DEPTHTYPE depthFmt) {
    if (!addrIsVRAM(tex->data)) return NULL;  // Render targets must be in VRAM
    C3D_RenderTarget* target = C3Di_RenderTargetNew();
    if (!target) return NULL;

    C3D_FrameBuf* fb = &target->frameBuf;
    C3D_FrameBufTex(fb, tex, face, level);

    if (C3D_DEPTHTYPE_OK(depthFmt)) {
        GPU_DEPTHBUF depthFmtReal = C3D_DEPTHTYPE_VAL(depthFmt);
        size_t depthSize = C3D_CalcDepthBufSize(fb->width, fb->height, depthFmtReal);
        vramAllocPos vramBank = addrGetVRAMBank(tex->data);
        void* depthBuf = vramAllocAt(depthSize, vramBank ^ VRAM_ALLOC_ANY);  // Attempt opposite bank first...
        if (!depthBuf) depthBuf = vramAllocAt(depthSize, vramBank);         // ... if that fails, attempt same bank
        if (!depthBuf) {
            free(target);
            return NULL;
        }

        C3D_FrameBufDepth(fb, depthBuf, depthFmtReal);
        target->ownsDepth = true;
    }

    C3Di_RenderTargetFinishInit(target);
    return target;
}

static void C3Di_RenderTargetDestroy(C3D_RenderTarget* target) {
    if (target->ownsColor) vramFree(target->frameBuf.colorBuf);
    if (target->ownsDepth) vramFree(target->frameBuf.depthBuf);

    C3D_RenderTarget** prevNext = target->prev ? &target->prev->next : &firstTarget;
    C3D_RenderTarget** nextPrev = target->next ? &target->next->prev : &lastTarget;
    *prevNext = target->next;
    *nextPrev = target->prev;
    free(target);
}

void C3D_RenderTargetDelete(C3D_RenderTarget* target) {
    if (inFrame) svcBreak(USERBREAK_PANIC);  // Shouldn't happen.
    if (target->linked)
        C3D_RenderTargetDetachOutput(target);
    else
        C3Di_WaitAndClearQueue(-1);
    C3Di_RenderTargetDestroy(target);
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags) {
    int id = 0;
    if (screen == GFX_BOTTOM) id = 2;
    else if (side == GFX_RIGHT) id = 1;
    if (linkedTarget[id]) {
        linkedTarget[id]->linked = false;
        if (!inFrame) C3Di_WaitAndClearQueue(-1);
    }
    linkedTarget[id] = target;
    if (target) {
        target->linked = true;
        target->transferFlags = transferFlags;
        target->screen = screen;
        target->side = side;
    }
}

static void C3Di_SafeDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags) {
    C3Di_WaitAndClearQueue(-1);
    inSafeTransfer = true;
    GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
    gxCmdQueueRun(&ctx()->gxQueue);
}

static void C3Di_SafeTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags) {
    C3Di_WaitAndClearQueue(-1);
    inSafeTransfer = true;
    GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
    gxCmdQueueRun(&ctx()->gxQueue);
}

static void C3Di_SafeMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e,
                                u16 control1) {
    C3Di_WaitAndClearQueue(-1);
    inSafeTransfer = true;
    GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
    gxCmdQueueRun(&ctx()->gxQueue);
}

void C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags) {
    if (inFrame) {
        C3D_FrameSplit(0);
        GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
    } else {
        C3Di_SafeDisplayTransfer(inadr, indim, outadr, outdim, flags);
        gspWaitForPPF();
    }
}

void C3D_SyncTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags) {
    if (inFrame) {
        C3D_FrameSplit(0);
        GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
    } else {
        C3Di_SafeTextureCopy(inadr, indim, outadr, outdim, size, flags);
        gspWaitForPPF();
    }
}

void C3D_SyncMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e,
                        u16 control1) {
    if (inFrame) {
        C3D_FrameSplit(0);
        GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
    } else {
        C3Di_SafeMemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
        gspWaitForPSC0();
    }
}
