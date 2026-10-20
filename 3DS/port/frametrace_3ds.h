#pragma once
// Per-frame timing trace (S3D_FRAME_TRACE, port/frametrace_3ds.c)
#include <3ds/types.h>
#include <stdbool.h>

enum {
    // Game thread
    FT_G_FRAME = 1,   // VIWaitForRetrace returns: the frame's game code starts (arg: frame number)
    FT_G_VIW,         // VIWaitForRetrace entered: the frame's game code is done
    FT_G_FENCE_B,     // gxrec_fence: waiting for the render thread's previous frame boundary
    FT_G_FENCE_E,
    FT_G_SPACE_B,     // Ring full, waiting for the render thread
    FT_G_SPACE_E,
    FT_G_REC,         // Frame boundary recorded
    // Render thread
    FT_R_IDLE_B = 16, // Nothing to replay: sleeping
    FT_R_IDLE_E,
    FT_R_BND_B,       // port_frame_boundary (touch UI, frame end, frame begin)
    FT_R_BND_E,
    FT_R_FE_B,        // C3D_FrameEnd
    FT_R_FE_E,
    // citro3d queue (gx_c3d_queue.c), either thread
    FT_Q_WAIT_B = 32, // Waiting for the GPU (wait_entries / queue_wait)
    FT_Q_WAIT_E,
    FT_Q_SWAP,        // Screen buffers swapped (arg: thread slot, 9 = GSP thread)
    FT_Q_SUBMIT,      // Command list handed to the GPU (frame end or mid-frame split; arg 1 = frame end)
    // GSP events (GSP thread)
    FT_GPU_P3D = 48,  // A command list finished
    FT_GPU_PPF,       // A display transfer / texture copy finished
    FT_VBLANK0,       // Top screen VBlank
    // Trace control
    FT_PHASE = 60,    // Phase starts (arg: S3D_RT_DEDUP value)
};

extern volatile bool g_ft_on;
void ft_record(u32 ev, u32 arg);
static inline void ft_ev(u32 ev, u32 arg) {
    if (g_ft_on) ft_record(ev, arg);
}

void ft_frame(void);    // Once per frame from VIWaitForRetrace (game thread): runs the trace phases
bool ft_enabled(void);  // S3D_FRAME_TRACE > 0
void ft_request(void);  // L+R+SELECT or the remote "frametrace": start a trace
