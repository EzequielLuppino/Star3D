# Offscreen passes (GRAPHICS_PLAN.md 6.1): each distortion slot draws its warp
# mesh into a 32x32 scratch target (gx/gx_init.c gx_offscreen_begin) instead
# of the EFB corner, so it is drawn once in stereo and leaves the frame alone.
# With S3D_RCP_REFRESH=N a slot is redrawn every Nth frame while it keeps its
# model, mode and group (port_rcp_reuse, MULTICORE_PLAN.md "Other offscreen
# passes"), else last frame's copy is sampled again: the mesh pass and the
# copy are skipped, the light setup still runs.
/^void Rcp_UpdateDistortionTextures(void) {$/i\
bool gx_offscreen_begin(float x, float y, float w, float h, int res);\
void gx_offscreen_end(void);\
int port_rcp_reuse(int slot, const void* model, int group);
/^            Rcp_DrawWarpDistortionMesh((f32)(i \* 0x20), 0.0f);$/i\
            int port_skip = port_rcp_reuse(i, ((RcpDistortSlot*)slots[0])[i].model, ((RcpDistortSlot*)slots[0])[i].mode * 256 + gRcpDistortGroup);\
            if (!port_skip) gx_offscreen_begin(0.0f, 0.0f, 32.0f, 32.0f, 32);
/^            tex = (Texture\*)((RcpDistortSlot\*)slots\[0\])\[i\].texture;$/i\
            gx_offscreen_end();
s|^\( *\)Rcp_DrawWarpDistortionMesh((f32)(i \* 0x20), 0.0f);$|\1if (!port_skip) Rcp_DrawWarpDistortionMesh((f32)(i * 0x20), 0.0f);|
s|^\( *\)GXCopyTex(((RcpDistortSlot\*)slots\[0\])\[i\]|\1if (!port_skip) GXCopyTex(((RcpDistortSlot*)slots[0])[i]|
