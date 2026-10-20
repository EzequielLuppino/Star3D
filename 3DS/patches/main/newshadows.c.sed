# blendTextures mixes two loaded sky textures on the CPU in GX tile layout.
# Loaded textures are converted to PICA formats at build time, so the loop
# would read the wrong layout (garbage sky gradient). The port blends from the
# converted data instead (gx/gx_tex.c port_blend_textures) and returns 0 when
# the sources are plain GX data.
/^void blendTextures(Texture\* src1, Texture\* src2, f32 blend, Texture\* dst)$/i\
int port_blend_textures(const void* src1, const void* src2, u32 w, u32 h, u32 fmt, float blend, void* dst);
/^        wA = (int)(255.0f \* blend) & 0xff;$/i\
        if (port_blend_textures((u8*)src1 + sizeof(Texture), (u8*)src2 + sizeof(Texture), w, h, fmt, blend, (u8*)dst + sizeof(Texture))) { DCStoreRange((u8*)dst + sizeof(Texture), dst->dataSize); return; }
# The 16:9 mode fills the 5:3 top screen (main.c setup_screen_style)
s|Camera_SetAspectRatio(1.7777778f);|Camera_SetAspectRatio(1.6666666f);|
# boxBlurTexture blurs the shadow texture's memory on the CPU, which the bound
# EFB copy texture ignores (shadows were sharp). The port blurs the copy on
# the GPU instead (gx/gx_copy.c port_copytex_blur, S3D_GPU_BLUR).
/^static void boxBlurTexture(u8\* texData, int size, int window, u32 fill) {$/i\
bool port_copytex_blur(const void* dest, int window);
/^static void boxBlurTexture(u8\* texData, int size, int window, u32 fill) {$/a\
    if (port_copytex_blur(texData + sizeof(Texture), window)) return;
# Offscreen passes (GRAPHICS_PLAN.md 6.1): the object shadow and the shadow
# casters draw into a small scratch target (gx/gx_init.c gx_offscreen_begin)
# instead of a corner of the EFB: drawn once in stereo, copied at the
# texture's resolution, and the frame is left alone. When begin fails they
# draw into the EFB as before; end is a no-op then.
/^void renderObjectShadowTexture(GameObject\* obj)$/i\
bool gx_offscreen_begin(float x, float y, float w, float h, int res);\
void gx_offscreen_end(void);
/^        objSetRenderingShadowPass(1);$/i\
        gx_offscreen_begin(256.0f, 176.0f, 128.0f, 128.0f, 128);
/^        obj->anim.modelState->shadowScale = 1.0f \/ sc;$/i\
        gx_offscreen_end();
/^            GXSetScissor(2, 2, screenW - 4, screenW - 4);$/i\
            int port_skip_rt = port_shadow_reuse((u8)texIdx, obj, casterPtr->flags);\
            if (!port_skip_rt) gx_offscreen_begin(0.0f, 0.0f, (float)screenW, (float)screenW, (int)w);
# Shadow levers (MULTICORE_PLAN.md "Shadow levers", gx/gx_init.c): at most
# S3D_SHADOW_TEXTURES casters (0-8, default 8) get a render-to-texture pass;
# the others use their static texture, as casters past the 8th always did.
# With S3D_SHADOW_REFRESH=N (default 2) a caster texture is redrawn every Nth frame when
# the same object keeps its slot (port_shadow_reuse), else last frame's copy
# is sampled again: the copy is skipped and the object pass draws nothing
# (gx_draw_discard). The object pass still runs: it is the caster's first
# render in the frame, which computes the joint matrices the main pass reuses
# (bufferFlags 8). Skipping it made Fox's staff explode in cutscenes on every
# other frame.
/^void renderShadows(/i\
int port_shadow_textures(void);\
int port_shadow_reuse(int tex, const void* obj, int kind);\
void gx_draw_discard(_Bool on);
s|if ((u8)texIdx < NEW_SHADOW_MAX_CAST_TEXTURES \&\& (kind = casterPtr->flags) != 0)|if ((u8)texIdx < port_shadow_textures() \&\& (kind = casterPtr->flags) != 0)|
s|^\( *\)objRenderShadowIfVisible(obj, 0, 0, 0, 0, 0);$|\1if (port_skip_rt) gx_draw_discard(1);\
\1objRenderShadowIfVisible(obj, 0, 0, 0, 0, 0);\
\1if (port_skip_rt) gx_draw_discard(0);|
s|^\( *\)GXCopyTex(obj->anim.modelState->shadowTexture + 1, GX_TRUE);$|\1if (!port_skip_rt) GXCopyTex(obj->anim.modelState->shadowTexture + 1, GX_TRUE);|
s|^\( *\)GXCopyTex(\*texture + 1, GX_TRUE);$|\1if (!port_skip_rt) GXCopyTex(*texture + 1, GX_TRUE);|
/^        slotIdx++;$/i\
        gx_offscreen_end();
# drawReflectionTexture draws the 320x240 reflection copy at a quarter size
# into the EFB corner and copies it back as the 80x60 small reflection
# (GRAPHICS_PLAN.md 6.2). The port binds the small texture as a coarser mip
# level of the reflection copy instead (gx/gx_copy.c port_copytex_mip_alias,
# S3D_REFLECT_MIP): no redraw, no copy, nothing drawn over the frame.
/^void drawReflectionTexture(void)$/i\
bool port_copytex_mip_alias(const void* dest, const void* src, int w);
/^    char\* texture = (char\*)gNewShadowReflectionTexture;$/a\
    if (port_copytex_mip_alias((char*)gNewShadowReflectionSmallTexture + sizeof(Texture), texture + sizeof(Texture), 0x50)) return;
# Runtime IA8 textures (OPEN_BUGS.md 2): the game stores each texel as a u16
# (alpha << 8 | intensity), big-endian [A, I] on GameCube. On the
# little-endian 3DS the bytes land as [I, A], so the IA8 decoder (gx/gx_tex.c)
# swapped coverage and colour: the fur noise showed its coverage pattern as
# colour speckle. The stores (reflection gradient, fur noise frames, caustic,
# distortion vectors) are byte-swapped to the GameCube order.
s#^\( *\*(u16\*)(texel + sizeof(Texture)) = \)\(.*\);$#\1__builtin_bswap16((u16)(\2));#
s#(u16)(value | ((int)(255\.0f \* (y / 3\.0f - 0\.5f) + 128\.0f) & 0xff))#__builtin_bswap16(&)#
# Fur noise mip chain (OPEN_BUGS.md 2, gx/gx_tex.c port_tex_want_mips): each
# noise frame is registered after it is filled, so its upload gets box-filtered
# levels and the 37.5x tiled shells stop sampling as speckle. The local decomp
# names the loop variable i, the Docker one frame
1i\
void port_tex_want_mips(const void* data);
s#^\( *\)DCFlushRange(gNewShadowNoiseTexFrames\[\(i\|frame\)\] + 1, gNewShadowNoiseTexFrames\[\2\]->dataSize);$#&\
\1port_tex_want_mips(gNewShadowNoiseTexFrames[\2] + 1);#
# Bug 35 (C12 floor caustics): the warped ring's indirect warp is baked into
# the ring texture once it is filled (port/caustic_3ds.c port_warped_ring_bake)
1i\
void port_warped_ring_bake(unsigned char* data);
s#^\( *\)fillRingTexture();$#&\
\1port_warped_ring_bake((u8*)gNewShadowRingTexture + sizeof(Texture));#
