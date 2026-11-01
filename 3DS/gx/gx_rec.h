#ifndef GX_REC_H
#define GX_REC_H

// GX recorder and render thread (MULTICORE_PLAN.md step 2, gx_rec.c).
//
// S3D_RENDER_THREAD (touch UI "Render thread", switched at a frame start):
//   0  GX calls run on the game thread as they are made (no recorder)
//   1  GX calls are recorded and replayed on the game thread at the frame end
//      (and whenever the ring fills or a call needs a result): no second
//      thread, for checking the recording and the late reads of game memory
//   2  GX calls are recorded and replayed by the render thread on core 2 while
//      the game continues. With S3D_RT_OVERLAP (default 1) the frame end does
//      not wait: the render thread also runs the frame boundary (touch UI,
//      C3D_FrameEnd, frame begin) and finishes the frame while the game thread
//      runs the next frame's logic, up to its rendering (gxrec_fence).
//      S3D_RT_OVERLAP=0: the frame end waits until everything is replayed
//
// The wrappers (generated from gxrec.list) record when gxrec_recording() is
// true. The render thread, and the game thread while it replays or runs a
// call directly, set t_gxrec_direct, so calls they make run directly.

#include <3ds.h>
#include <stdbool.h>

// Opcodes below the generated ones (gxrec_gen.h)
enum {
    GXREC_OP_WRAP,    // Rest of the ring unused: continue at its start
    GXREC_OP_IMM,     // Immediate-mode vertex bytes: u32 byte count, bytes
    GXREC_OP_FRAME,   // Frame boundary (port_frame_boundary): black, reset, 3D slider bits
    GXREC_OP_DELTA,   // State delta (gx_rec_delta.c): regions bitmap, flags, dirty, vtx_gen, payload
    GXREC_OP_MBLOCK,  // Matrix block (gx_rec_delta.c): count, then (header, matrix) entries
    GXREC_OP_MODELDRAW,  // Model draw (port_model_draw): list, size|scope bit, mblk words, delta words, [mblk][delta]
    GXREC_OP_RESERVED
};
#define GXREC_OP_RESERVED_NAMES [GXREC_OP_WRAP] = "(wrap)", [GXREC_OP_IMM] = "immediate-mode data",     [GXREC_OP_FRAME] = "frame boundary", [GXREC_OP_DELTA] = "(state delta)", [GXREC_OP_MBLOCK] = "(matrix block)", [GXREC_OP_MODELDRAW] = "model draw",

extern bool g_gxrec_on;               // Recording (mode 1 or 2), game thread only
extern __thread bool t_gxrec_direct;  // This thread runs GX calls directly

static inline bool gxrec_recording(void) { return g_gxrec_on && !t_gxrec_direct; }

// Used by the generated code
u32* gxrec_begin(u32 op, u32 words);  // Space for one command's arguments
void gxrec_end(u32* end);             // The command is complete up to end
// Waits until everything recorded has run, then lets this thread call GX
// directly until gxrec_sync_direct_end (counter: rt.sync.<name>)
void gxrec_sync_direct_begin(const char* counter, int* counter_id);
void gxrec_sync_direct_end(void);

// Port side
void gxrec_init(void);
int gxrec_mode(void);         // 0, 1 or 2 (current frame)
void gxrec_frame_begin(void); // After gx_frame_begin: applies S3D_RENDER_THREAD, recording starts
void gxrec_frame_end(void);   // Before the frame end: drains, then the game thread runs GX directly
// Overlapped frame end (mode 2, S3D_RT_OVERLAP): instead of gxrec_frame_end
// and the frame boundary, the game thread records the boundary and the render
// thread runs it (port_frame_boundary, vi_3ds.c)
bool gxrec_overlap_frame_end(void);  // This frame end may overlap
void gxrec_record_frame(bool black, bool reset, float slider);
// Waits until the render thread has run every recorded frame boundary: before
// the game rewrites memory the previous frame's draws read (sceneRender,
// immediate frees) and before the next frame end
void gxrec_fence(void);
// Waits until everything recorded has been replayed, then writes the
// per-opcode report in GX_PROFILE_DETAIL builds (diag_3ds.c's GPU sweep uses
// it to change settings with the render thread idle)
void gxrec_report_drain(void);
// Before a profiler report at an overlapped frame end: drains only in
// GX_PROFILE_DETAIL builds, for the per-opcode report
void gxrec_ops_report_drain(void);
void port_frame_boundary(bool black, bool reset, float slider);  // Render thread, vi_3ds.c
// Port code that touches GX or citro3d state outside the recorded calls
// (movie upload, save states): drains, and runs GX directly until _end.
// Nested calls are fine; free when not recording.
void gxrec_direct_begin(void);
void gxrec_direct_end(void);
void gxrec_shutdown(void);    // Before gx_deinit
void gxrec_forget_answers(void);  // The GX caches were reset (answers remembered from render-side state)

#endif // GX_REC_H
