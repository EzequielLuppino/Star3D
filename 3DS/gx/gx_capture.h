#pragma once
// Render capture (debug): a mark (L+R+SELECT, Settings > "Log snapshot (mark)"
// or the remote "mark" command) writes the frame's buffers and some diagnostic
// passes to sdmc:/3ds/star3d/captures/YYYYMMDD_HHMMSS_f<frame>/:
// - color.bmp, alpha.bmp, depth.bmp (normalized), depth.bin (raw), copyN.ppm
// - id.bmp + states.txt: each state painted in a color encoding its index,
//   with the per-state dump (TEV, textures, lights) of that frame
// - tint.bmp: TEV fallbacks tinted (magenta truncated, yellow approximated,
//   cyan dropped texture, green indirect)
// - light.bmp: vertex color/lighting only; normals.bmp: view-space normals
// The passes take one frame each, so the scene moves slightly between them.
// The files themselves are written in the background by a low-priority writer
// thread (see gx_capture.c), so a mark never stalls the frame on the SD card.
#include <3ds/types.h>
#include <sys/iosupport.h>
#include <citro3d.h>
#include <stdbool.h>

enum { GX_CAP_NONE, GX_CAP_ID, GX_CAP_TINT, GX_CAP_LIGHT, GX_CAP_NORMALS };

extern int g_gx_cap_mode;
// The pass the current frame is drawn with
static inline int gx_capture_mode(void) { return g_gx_cap_mode; }
// The pass replaces the color output (ID, light, normals): no 2-pass draws
static inline bool gx_capture_replaces(void) { return g_gx_cap_mode != GX_CAP_NONE && g_gx_cap_mode != GX_CAP_TINT; }

void gx_capture_request(void);
// Folder of the capture being written ("" before the first)
const char* gx_capture_dir(void);
// Frame begin, after the GPU finished the previous frame: writes what that
// frame drew into fb, then picks the next pass
void gx_capture_frame(const C3D_FrameBuf* fb, int top_mode, bool lowres);
// Stop the background writer (shutdown path)
void gx_capture_shutdown(void);

// "capmem:/<name>" in-memory file device: capture files buffer in the heap and
// are handed to the writer thread on close (gx_render.c's tex dumps and
// gx_copy.c's copy dumps use it, so the render thread never touches the SD)
extern devoptab_t gx_capmem_devoptab;
