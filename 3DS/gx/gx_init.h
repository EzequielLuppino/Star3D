#ifndef GX_INIT_H
#define GX_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <citro3d.h>

// Graphics initialization functions. gfxInitDefault() must be called first.
bool gx_init(void);
void gx_deinit(void);

// Frame management: render_frame() draws one empty frame;
// gx_frame_begin()/gx_frame_end() bracket custom drawing.
void gx_frame_begin(void);
void gx_frame_begin_slider(float slider);  // Same, with the 3D slider value read earlier
float gx_frame_slider(void);               // The 3D slider value gx_frame_begin would use
void gx_frame_end(void);
void render_frame(void);

// Display target management
void setup_display_targets(void);
void clear_top_screen(unsigned int color);
void gx_set_projection(const C3D_Mtx* projection);
void gx_set_clear_black(bool black);  // VISetBlack: clear to black
C3D_RenderTarget* gx_top_target(void);
// Right-eye color/depth buffers of the current stereo frame (false when mono or on the HUD)
bool gx_right_eye_buffers(u8** color, u8** depth);
void gx_selftest(void);  // make SELFTEST=1
void gx_c3d_invalidate(void);
void gx_c3d_tex_invalidate(void);
void gx_c3d_tex_unbind(int unit);  // Disable a texture unit (C3D_TexBind cannot take NULL)
void gx_hwtest(void);  // S3D_GPU_SELFTEST (bug 12)
void gx_vbench(void);  // Boot benchmark of the game's vertex shaders (S3D_BENCH, S3D_BENCH_VSH)
void gx_restore_state(void);  // Re-bind the GX shader/state after other GPU users

// Bottom screen HUD (S3D_BOTTOM_HUD): draws between begin/end go to the
// bottom screen; begin returns false when there is nothing to redirect
bool gx_hud_begin(void);
void gx_hud_set_map(float src_x, float src_y, float scale, float dst_x, float dst_y);
void gx_hud_end(void);
bool gx_hud_enabled(void);
// Touch UI: begin returns the bottom target (NULL when unavailable)
C3D_RenderTarget* gx_ui_begin(void);
void gx_ui_end(void);

// Offscreen pass (GRAPHICS_PLAN.md 6.1): draws aimed at the EFB rectangle
// x,y,w,h go to a res x res scratch target until end, and GXCopyTex reads
// from it. begin returns false (draw into the EFB as usual) when unavailable.
bool gx_offscreen_begin(float x, float y, float w, float h, int res);
void gx_offscreen_end(void);
bool gx_offscreen_active(void);
void gx_draw_discard(bool on);  // Drop draws until off (state still applies)
C3D_RenderTarget* gx_copy_source_target(void);
void gx_copy_source_size(int* w, int* h);  // Its drawn area (half resolution: smaller than the target)
// citro2d setup shared by the touch UI and the reduced resolution upscale
bool gx_c2d_init(void);

#ifdef __cplusplus
}
#endif

#endif // GX_INIT_H
