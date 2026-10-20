# Star 3D (N3DS): plan for the missing graphics, video and 3D

Status: steps 1-12 of section 10 are implemented in part (see the table
below). Everything compiles; none of it has been run on hardware yet. The
toggles go in `sdmc:/3ds/star3d/env.txt` (`NAME=0` or `NAME=1`), and every
feature logs to `sdmc:/3ds/star3d/Star3D.log`.

Written 2026-10-08 from a survey of `3DS/gx`, `3DS/port`, `3DS/tools`, the
game code in `foxhollow/game/src` and the extracted RomFS.

The guiding rule (see the project directive on build-time assets): **anything
that can be computed before the game runs is computed at build time**, and
written in the exact byte layout the PICA200, the DSP or the CPU wants to
consume. The console should only stream data and submit it.

## Implementation status (2026-10-09, compiled and checked in Azahar, not run on hardware)

| Step | State | What exists | Toggles (default) |
|---|---|---|---|
| 1 Benchmarks, worker, CIA | Done | `os/bench.c` startup CPU/GPU benchmarks in the log; `port/worker.c` job thread used by DVD reads, movie prefetch and the benchmark, on core 2 below the render thread (core 1 is off by default since the MULTICORE_PLAN.md A/B runs; `S3D_CORE1=1` moves it to core 1); CIA with the 804 MHz/L2 request | `S3D_BENCH` (1), `S3D_BENCH_GPU` (1), `S3D_WORKER` (1), `S3D_WORKER_CORE` (1), `S3D_USE_CORE2` (1), `S3D_CORE1` (0), `S3D_CORE1_LIMIT` (80), `S3D_ASYNC_DVD` (1) |
| 2 Build-time textures | Done | `tools/tex_convert.py` + `tools/native/texconv.c` turn every texture bank texture into PICA data (ETC1/ETC1A4/RGB565/RGBA4/LA/L/A formats chosen per texture, mip chains, NPOT images resampled or padded), re-compressed as ZLB and cached by hash. Runtime: `gx/gx_tex.c` uses the data as-is | `S3D_TEX_PICA` (1), `S3D_TEX_ZEROCOPY` (1), `S3D_TEX_MIPS` (1), `S3D_TEX_LODBIAS_X4` (0) |
| 3 Basics | Done; TEV compare GT ops and `DIVIDE_2` are exact, EQ compares are approximated | Perspective fog per pixel through fragment lighting: a light at the eye whose distance-attenuation LUT holds the fog curve, plus one combiner stage for the fog color (`gx_render.c apply_fog`). The PICA fog LUT is indexed by z/w and put the whole scene in its last entries. The fog LUT is still used for orthographic fog and 6-stage TEV programs (4.3), depth-bias viewports (4.6, exact `C3D_DepthMap` from the viewport near/far), lines and points expanded to quads on the CPU with width, size and texcoord offsets (`gx_vtx.c expand_lines_points`), `GXPeekZ` reading a GPU copy of the previous frame's depth (`gx_copy.c`), TEV program cache | `S3D_FOG` (1), `S3D_FOG_ZFLIP` (0, flips the LUT direction if fog looks inverted), `S3D_FOG_FRAG` (1, fragment-lighting fog), `S3D_DEPTH_BIAS` (1), `S3D_PEEKZ` (1), `S3D_TEV_CACHE` (1) |
| 4 Video | Done | `tools/video_convert.py` pre-decodes the attract movie to an ETC1 frame stream (`.v3d`); `port/movie_3ds.c` streams it. `tools/movie_audio_convert.py` re-encodes the audio as DSP-ADPCM with one coefficient set per channel (`.a3d`, about 35 dB SNR); `port/movie_audio_3ds.c` plays it on the stream channels 22/23, following the game's audio clock (7.2). Checked in Azahar: one sync at the start and no drift resyncs over the 60 s movie; not checked by ear | `S3D_MOVIE` (1) |
| 5 Unified memory, on-demand loading | 5.1 done, 2.4 tried and reverted | MEM1 is allocated from linear memory (`port/os_3ds.c`, linear heap raised in `main.c`), so converted textures are sampled in place (zero-copy). Texture banks are still loaded whole. On-demand TEX0/TEX1 entry reads were tried and reverted: in the Azahar A/B run they saved 4 MB of bank RAM and bank reads, but added 1.9 s of synchronous main-thread SD reads (975 reads, about 2 ms each), and the run fell 18 frames behind by 132 s. Revisit only with a worker-thread read or a per-entry cache, and only if RAM gets tight | — |
| 6 RTT, post effects, GPU blur | Done except hardware shadow maps | EFB copies are GPU texture copies (`gx_copy.c`). GPU blur: the DOF/blur filter (`doBlurFilter`, `patches/track/intersect_render.c.sed`) samples the screen copy at mip level 1 (level 2 for the bigger blur) through `port_copytex_blur_scope`, so the PICA's filtered mip fetch does the downscale and the 5-tap cross reads as a smooth blur; other users of the copy still sample it sharp (counter `gx.gpu_blur_scopes`). Offscreen passes (6.1): `gx_offscreen_begin/end` (`gx_init.c`) draw the object shadow, the shadow casters (`patches/main/newshadows.c.sed`) and the distortion slots (`patches/main/rcp_dolphin.c.sed`) into a square 32-256 px scratch target instead of an EFB corner: once in stereo, and without drawing over the frame. `GXCopyTex` reads the scratch target with the usual mapping (counter `gx.offscreen`, 4-8 per frame in the Azahar replay). Small reflection (6.2): `drawReflectionTexture` no longer redraws the 320x240 copy into the EFB and copies it back. The 80x60 texture is bound as a coarser mip level of the big copy instead (`port_copytex_mip_alias`, counter `gx.copytex_mip_alias`, 1 per frame). Projected shadows (6.3, first item) draw through the offscreen pass with the existing logic OR. Not done: PICA hardware shadow mapping (the Z8 self-shadow map), see 6.3 | `S3D_GPU_BLUR` (1), `S3D_OFFSCREEN` (1), `S3D_REFLECT_MIP` (1) |
| 7 Shader variants, fragment lighting, proctex | Done except proctex | Three vertex shaders, switched per batch (`gx_init.c`, counter `gx.vsh_switches`): `default.v.pica` (vertex lighting, texgen, water warp), `unlit.v.pica` (no lighting or warp, for unlit batches) and `warp.v.pica` (water warp). Each picks a lean or general texture coordinate path per slot (bool `tcaff`, unused-slot markers in `texsrc`, `S3D_VSH_LEAN`; MULTICORE_PLAN.md "Leaner vertex shaders"). Channel 1 specular goes through PICA fragment lighting (`gx_render.c`, decided per TEV program in `gx_tev_set_frag_secondary`): GX_AF_SPEC lights become a spotlight LUT on N.H, GX_AF_SPOT lights get the spot and distance LUTs, and the result is the secondary fragment color that the TEV program reads as channel 1 (LUTs cached, counter `gx.spec_luts_built`). Not covered: channel 1 ambient and alpha, vertex material colors on channel 1, lights past 7, and the radial fog while specular is on (both use the fragment unit). Proctex (4.4) is not used: it reads one of the three units' coordinates, and the noise frames need their own tiled coordinates. Instead fur shells drop the projected light texture so the noise mask keeps its unit (`port_fur_proj_light`); before this, the noise was the fourth texture and every lit shell drew solid. Not reached in the 2026-10-09 Azahar run (no `fur.proj_light_skipped`). The `tev.dropped_tex_stages` left there (6-9 per frame) are not fur: an 11-stage program with 7 texmaps (0-6) loses 4 stages and a 6-stage one with 4 texmaps loses map 3. 4.1 drop priority (done): when a draw has more textures than the 3 units, `gx_tev_build_unit_map` keeps them by `texmap_priority` instead of first-come: the base texture, then alpha-only masks, then modulations (light and shadow maps), then additive terms, then fades, with ties going to the earlier stage (counter `tev.priority_drops`, about 10 per frame in the Azahar replay; the Thorntail shot looked unchanged). 4.1 multipass (done, see the 4.1 status): a draw that loses exactly one texture, whose stage is a last `prev + b*c` or `prev * x` term, gets a second pass with that texture. The pass is blended additively or multiplicatively over the first at equal depth. It is not triggered in the Azahar replay. There the draws either lose two textures (`tev.multipass_skip_two`) or the lost stage is a lerp `tex*(1-Aprev) + prev*Aprev` (`tev.multipass_skip_form`, 6 per frame), which pass 2 can't rebuild | `S3D_FRAG_LIGHT` (1), `S3D_FUR_PROJ_LIGHT` (0), `S3D_TEV_DROP_PRIORITY` (1), `S3D_TEV_MULTIPASS` (1) |
| 8 Indirect replacements | Done as approximations (no flipbooks), except hardware shadow maps | 5.8 fallback counters: GXSetTevIndirect calls go through `port_tev_indirect` (`gx_tev.c`, sed patches on shader_dolphin.c, objprint_dolphin.c, intersect_render.c, dlls/engine/0/0.c) and are counted as `ind.<function>` in the profiler report. Azahar replay top users: objFuzzRenderCb (fur), setupWaterCausticTev, setupCausticBaseTevStages, addEnvMapBumpStages. Indirect stages are still dropped. Fur (5.4), all in `port/fur_3ds.c` with patches in `patches/main/objprint_dolphin.c.sed`: wind (`port_fur_wind` shifts the noise post-matrix per shell with the caustic scroll, approximating the per-pixel warp); shell budget (`port_fur_step`: at most 8 shells close up, 4 mid, 2 far, none far in stereo, spread over the full height by scaling `gObjFuzzStep`; counters `fur.shells`, `fur.objects_skipped`); per-layer shade (`port_fur_shade`, inner shells 60% dark, replacing bump-alpha); fast shells (`port_fur_fast`, `S3D_FUR_FAST`: render ops that don't draw in a shell defer their material, vertex format and matrix loads, see FUR_PLAN.md "Fast shells"); noise kept over the projected light (row 7). Grass (5.4): `port_grass_step` walks the 4/8/16 grass passes with a step ending on the top one (`patches/main/tex_dolphin.c.sed`, counter `grass.passes`). Water ripple (5.1), since bug 35 (OPEN_BUGS.md): the water split's offsets are the GameCube's own caustic reads at TEX0, computed per tessellated vertex on the CPU (gx_render.c `water_tessellate`) and passed in the vertex colour; the warped ring (5.3) is baked into the ring texture (`port/caustic_3ds.c`). Before that: Water ripple (5.1): `port_tev_indirect` maps known functions to a warp amplitude (`s_warp_sites`, only setupWaterCausticTev); the stage's texture slot gets `GxUnitMap.unit_warp`, and the vertex shader adds two analytic waves over object x/z (`texwarp`, `warpon`, sine as 4u(1-abs(u))) to its coordinates after the divide; GXSetTevDirect / GXSetNumIndStages(0) clear it. Per-vertex only, so coarse water meshes ripple coarsely; checked running in Azahar (gx.tex_warp count), but the water was not on screen in the test shots. Heat shimmer and distortion shockwave (5.2): `doHeatEffect` / `doDistortionFilter` draw the screen copy through a 40x24 grid. Each vertex carries the offset the indirect read would add, computed on the CPU (`port/screenwarp_3ds.c`, `patches/track/intersect_render.c.sed`, counter `gx.screen_warp_verts`). Not reached in the replay. Caustics, warped noise and warped ring (5.3): the same per-vertex swirl as the water (`s_warp_sites` group 1, counter `gx.tex_warp`, about 28 per frame with the water). Flipbooks were rejected because the game fills those textures at runtime from its RNG and projects them from world position. Heavy fog (5.5): `renderHeavyFog` gets a softer swirl on the fog texture coordinate (group 0). Bump env (5.6): the game's own bump-off path runs, a plain sphere map with 2 TEV stages and one texmap fewer (counter `env.bump_fallback`, 1 per frame). Hardware shadow maps (6.3) are still open | `S3D_FUR_WIND` (1), `S3D_FUR_BUDGET` (1), `S3D_FUR_SHADE` (1), `S3D_GRASS_BUDGET` (8 passes), `S3D_WATER_WARP` (1, also heavy fog), `S3D_CAUSTIC_WARP` (1), `S3D_SCREEN_WARP` (1), `S3D_ENV_BUMP_SKIP` (1) |
| 9 Compact display lists | Done (runtime packing, not a build-time bake) | Display lists are decoded at runtime and cached (`gx_dlcache.c`). Static entries are now stored in a lossless 36-byte packed layout (`GxPackedVertex`: s16 position and texcoords with a per-entry scale found by `exact_frac`/`common_frac`, u8 color, s16 normal) instead of the 52-byte float layout, drawn by `gx_render_draw_cached_packed` (the shaders scale texcoords by `posscale.z`). Lists whose arrays change (gather lists) stay unpacked. Checked in Azahar: about 86% of cached triangles draw packed. A build-time bake was not done: the lists reference runtime vertex arrays and matrices, so a bake would only save the first decode | `S3D_DL_PACK` (1) |
| 10 Render thread | Partial: adaptive frame pacing done, no GX thread | `gx_frame_begin` (`gx_init.c`) waits for the next VBlank only when the previous frame took less than one VBlank period. A slower frame skips that wait and only waits for the GPU, so a 56 ms frame is no longer rounded up to 67 ms (the game times itself with a stopwatch, `waitNextFrame`). The display still swaps on a VBlank. Checked in Azahar (Thorntail Hollow, 130 s replay): 14.8 → 17.1 fps, `gx.wait_vsync` 11 → 2.1 ms per frame (counter `gx.unpaced_frames`). The 2.1 ms left is the wait for the GPU to finish the previous frame. A real GX render thread (3.5) was not done: the 92 GX entry points plus `gx_render`, `gx_dlcache` and texture uploads all write GPU memory that is reused each frame on the assumption that the previous frame has finished. A lazy frame begin (opening the frame at the first GPU write) would hide only those 2 ms and needs a guard on every one of those paths, so it was left out. The worker (`port/worker.c`) is idle during gameplay; moving the dlcache vertex updates (`update_incremental`) to it would save 1-2 ms but races with the game's vertex arrays | `S3D_FRAME_PACE_ADAPTIVE` (1; 0 = always wait for the VBlank) |
| 11 Stereo 3D | Done | Both eyes are drawn into one 240x800 target: every batch is submitted twice with that eye's viewport, scissor and projection (eye offset on perspective projections only). Per-eye EFB copies: each `GXCopyTex` also copies the right eye's half into a second texture, which the right eye binds (`gx_copy.c`, counters `gx.copytex_right`, `gx.copytex_right_over_budget`; over the copy-queue budget the right eye reuses the left copy). `GXPeekZ` still reads the left eye. Convergence follows the camera-to-Fox distance (`patches/main/camera.c.sed` → `port_stereo_focus`), clamped to 20-400 and eased over about 0.2 s. HUD parallax: orthographic 2D batches can be pushed into the screen by a fraction of the separation (`S3D_STEREO_HUD_X1000`, off by default). "3D: reduced" (`port_stereo_reduced`, stereo frames only): half the fur shells and grass passes, no motion blur and no DOF/blur filter (`patches/main/lightmap.c.sed`). Slider at 0: one eye, no extra cost | `S3D_STEREO` (1), `S3D_STEREO_SEP_X1000` (25 = parallax at infinity per eye at full slider, in clip units x1000), `S3D_STEREO_CONV` (80 = zero-parallax distance in world units, the start value with autoconvergence), `S3D_STEREO_AUTOCONV` (1), `S3D_STEREO_HUD_X1000` (0), `S3D_STEREO_REDUCED` (0), `S3D_STEREO_SWAP` (0, swaps the eyes if depth looks inverted) |
| 12 Widescreen, wide mode, AA, bottom HUD | Done | The game runs in its 16:9 mode (`FOXHOLLOW_SCREEN_STYLE=wide`, set by `main.c setup_screen_style`) with the projection aspect at 5:3 (`widescreenAspect`, `patches/main/newshadows.c.sed`), and the renderer stretches the 640-pixel EFB over the full 400-pixel width instead of pillarboxing (`g_efb_x0`/`g_efb_xs`, `gx_init.c update_screen_mapping`; projection, scissor, clears, EFB copies and `GXPeekZ` follow). The 2D HUD is 1.25x wider than on a 4:3 TV (the GameCube's anamorphic 16:9 stretched it 1.33x). Bottom HUD: the minimap (`gMinimapInterface->vtable->update`, `patches/main/gameloop_main.c.sed`) and its text box 0x83 (`patches/main/textrender_run.c.sed` → `port/hud_3ds.c`) are drawn into a 320x240 bottom-screen target (`gx_init.c gx_hud_begin/gx_hud_end`, mono, EFB x 16-256 y 292-472 scaled 4/3); the debug console is then not shown. EFB copies or `GXPeekZ` issued inside the redirect would use the bottom mapping (none seen). Both checked in Azahar. Wide mode and AA share one big VRAM target with stereo (`gx_init.c setup_big_target/set_top_mode`): wide mode draws 240x800 and switches the top screen to 800x240 (`gfxSetWide`) while the 3D slider is down; it is refused on the Old 2DS. AA draws 480x800 and the display transfer halves it in both directions. With the slider up, stereo wins over both. Scissor, EFB copies and `GXPeekZ` follow the framebuffer size (`g_fb_w`/`g_fb_h`). Checked in Azahar: AA gives a correct, smoother picture; wide mode switches, but Azahar shows only the left 400 of the 800 columns at 1:1, so the picture there is cut and stretched 2x (expected on an emulator without wide-mode scaling; check on hardware) | `S3D_WIDESCREEN` (1; 0 = pillarboxed 4:3; `FOXHOLLOW_SCREEN_STYLE` in env.txt overrides it), `S3D_BOTTOM_HUD` (1; 0 = minimap on the top screen, debug console on the bottom), `S3D_WIDE_MODE` (0), `S3D_AA` (0; 4x the fill cost, wide mode wins if both are set) |

Profiler counters added by these steps (in the log's per-zone/counter
reports): `gx.fog_luts_built`, `gx.peekz_copies`, `gx.peekz_reads`,
`gx.lines_points_expanded`, `gx.lines_points_skipped`, `gx.stereo_frames`,
`gx.copytex_right`, `gx.copytex_right_over_budget`, `gx.vsh_switches`,
`gx.spec_luts_built`, `fur.shells`, `fur.objects_skipped`,
`fur.proj_light_skipped`, `grass.passes`, `gx.gpu_blur_scopes`,
`gx.packed_triangles`, `dl.packed_entries`, `dl.packed_saved_kb`,
`dl.unpacked_for_gather`, `gx.unpaced_frames`, `tev.priority_drops`.

Things to check first on hardware:
- Fog direction (`S3D_FOG_ZFLIP`) and fog strength against the GameCube version.
- Decals and effects that relied on the viewport depth bias (`S3D_DEPTH_BIAS=0` to compare).
- Lens flare and sun glow occlusion (`S3D_PEEKZ`).
- Stereo: eye order (`S3D_STEREO_SWAP`), comfort (`S3D_STEREO_SEP_X1000`, `S3D_STEREO_CONV`), and the frame rate with the slider up (`gx.stereo_frames` and the GPU timings in the log).

Fixes from testing in Azahar (2026-10-09, Thorntail Hollow with the replayed input):
- Vertex lighting: COLOR0 and ALPHA0 have their own light masks, and each light uses its channel's diffuse and attenuation functions (`upload_lighting`, `default.v.pica onelight`). Specular lights (`GX_AF_SPEC`) use N.H and `GXInitSpecularDir` matches the SDK. This fixed the white slab and the yellow Fox/ThornTail: the Rcp distortion copies' specular highlights flooded the whole sphere. COLOR1A1 is still read as zero (the moon highlight in those copies is missing).
- TEV `SUB` with `GX_TB_ADDHALF` is `ADD_SIGNED(d, 1 - x)` (exact). It fixed the dark square of the caustic ground overlay (`setupCausticBaseTevStages`). That overlay's indirect warp is still dropped.
- Sky gradient: `blendTextures` (newshadows.c) blends two loaded sky textures on the CPU in GX layout, but loaded textures are PICA data after the build-time conversion, so the sky was striped garbage. `patches/main/newshadows.c.sed` routes it to `port_blend_textures` (`gx_tex.c`), which reads the converted sources (any PICA format, including ETC1) and writes the GX RGBA8/RGB565 destination. Other game code that reads loaded texture pixels on the CPU would hit the same problem; none is known yet.
- "No Map Data" on the minimap is correct: the map is bought in the shop.
- Debug: `S3D_DBG_HOLD=1` freezes the dumped frame (`-DumpFrame N`) and writes its EFB copies to `sdmc:/3ds/star3d/copyN.ppm`. `S3D_DBG_IDCOLOR=1` paints each state in a color that encodes its dump index. `S3D_DBG_TEXDUMP=<state>` writes that state's textures to `texN.ppm`. The dump also lists each light. `tools/ppm2png.ps1` converts the dumps.

---

## 0. Hardware: what the N3DS does better, what it lacks

### 0.1 GameCube vs New 3DS

| | GameCube | New 3DS | Consequence for the port |
|---|---|---|---|
| Storage | 1.5 GB optical disc, ~2-3 MB/s, ~100 ms seeks | SD card. No seeks, random access costs about as much as sequential. Storage size is not a concern | Stop loading whole banks. Read single assets on demand. Store data uncompressed and pre-converted |
| Main RAM | 24 MB 1T-SRAM + 16 MB ARAM (slow, DMA only) | 256 MB FCRAM, unified. Apps get 124 MB, or 178 MB in N3DS extended mode | One copy of each asset, already in GPU format. Bigger caches. No ARAM staging |
| GPU-visible memory | Everything | Linear heap (physically contiguous) + 6 MB VRAM | Put game data the GPU reads (textures, vertex arrays) in linear memory, so no copy is needed |
| CPU | 1x Gekko 486 MHz | 4x ARM11 MPCore 804 MHz, 2 MB L2. The app owns core 0 and core 2 fully, and up to 80 % of core 1 (syscore) | Game logic stays single-threaded on core 0. Port work moves to core 2: decode, gather, I/O, video, render submission |
| GPU pipeline | 16 TEV stages, 8 textures, indirect texturing, 4x4 TEV regs | 6 combiners + 1 combiner buffer, 3 texture units + **procedural texture unit**, **per-pixel fragment lighting with LUTs** (spot, distance attenuation, specular, Fresnel, **bump/normal mapping**), **hardware shadow maps**, **fog LUT** (and gas), **cube maps**, **ETC1/ETC1A4**, geometry shader, logic ops, stencil | Don't try to emulate TEV/indirect generically for complex effects. Re-express each effect with the PICA features made for it |
| Framebuffer | 640x480 embedded EFB, EFB->texture copies are free | 400x240 per eye in VRAM. Display-transfer engine can **downscale 2x with filtering in hardware**. Render-to-texture targets | Fewer pixels, so effects are cheaper per pixel. Hardware downscale gives free blur pyramids and supersampling AA. Render offscreen passes straight into textures |
| Video decode | Software MJPEG on Gekko | **N3DS hardware H.264 decoder (MVD)**. Plenty of storage to pre-decode | Convert the movie at build time. No software MJPEG on the console |
| Display | One TV, 4:3 (or 16:9 anamorphic) | **Autostereoscopic** 400x240 top screen (5:3), 800x240 wide mode when 3D is off, plus a 320x240 touch screen | Stereo 3D. Use the game's widescreen mode. Move HUD/minimap to the bottom screen |
| Input | C-stick, analog triggers | C-stick (N3DS), ZL/ZR, touch, gyro, accelerometer | Outside graphics, but the bottom-screen HUD makes the touch screen useful (section 9) |

### 0.2 What the hardware lacks, and the general answer

| Missing on PICA200 | Used by the game for | General answer |
|---|---|---|
| Indirect texturing (dependent reads) | Fur, water, heat haze, distortion, heavy fog, bump env map, ice, caustics | Warp per vertex on a dense grid; bake periodic warps into flipbooks at build time; use fragment-light bump mapping |
| More than 6 combiners / 3 textures | Big object materials (up to 8 stages, 6 texmaps) | Fragment lighting replaces texture-based lighting stages. Proctex replaces noise/ramp textures. Fall back to multipass |
| Second vertex colour output | GX COLOR1 (specular) | Fragment lighting secondary colour (`GPU_FRAGMENT_SECONDARY_COLOR`) |
| Points and lines | Stars, water drops, lightning | Expand to quads (at build time if the data is static, else on CPU) |
| TEV compare ops | Shadow-map compare, viewfinder | Hardware shadow test; alpha test |
| Fast CPU read of the framebuffer | `boxBlurTexture`, `GXPeekZ` | Do blurs on the GPU. Read depth one frame late from a GPU copy |

### 0.3 Facts from the survey that shape the plan

- **Rendering is CPU-bound today.** About 21.5 ms per frame on the title screen in Azahar. The game itself takes about 2.5 ms; GX decode/submit about 8 ms; scene traversal the rest. Cores 1, 2 and 3 are unused (`osSetSpeedupEnable(true)` in `main.c:50`, all threads on the default core).
- **Every texture exists twice.** The game keeps the GC copy in its heap. `gx_tex.c` decodes only mip 0 to RGBA8 at runtime and resamples non-power-of-two sizes with nearest filtering. CMPR (51 % of textures) grows 8x, and there are no mipmaps at all. Textures on disc: CMPR 4974, RGB5A3 3232, IA8 867, RGBA8 470, I8 81, I4 48, IA4 28. 1521 are non-power-of-two, 1155 are animated, about half have mips.
- **Display lists are decoded on the CPU** (`gx_vtx.c`) into 52-byte float vertices and cached (`gx_dlcache.c`, 8 MB). Model vertex arrays and all display lists stay big-endian.
- **Whole per-map banks are kept in RAM** (TEX0/TEX1/MODELS/ANIM/…, 4-6 MB compressed per map, two map slots). Each asset is then inflated with zlib on demand. All of this exists to work around disc latency (`pi_dolphin.c:mapLoadDataFile`, `loadAndDecompressDataFile`).
- **The vertex shader is full:** 95 of 96 float uniforms (`default.v.pica`).
- **One render target** of 400x240, 4:3 pillarboxed to 320x240. No stereo. The bottom screen is a debug console.
- **Movie:** a single `starfox.thp`, 512x384 MJPEG at 29.97 fps, 1797 frames, 32 kHz stereo THP-ADPCM. Video decode is patched out (black frames, audio plays).
- **Already missing or wrong in the backend:**
  - fog (`gx_state.c:512`)
  - indirect texturing (`gx_tev.c:219`, `gx_state.c:519`)
  - bump/SRTG texgen
  - lines and points (skipped, `gx_vtx.c:403`)
  - point size and line width
  - `GXPeekZ` (always reports "far", so lens flares never get occluded)
  - viewport near/far ignored, so the game's depth-bias viewports do nothing (`camera.c:787-816`)
  - COLOR1 reads as zero
  - TEV compare ops treated as add; `DIVIDE_2` scale ignored; swap tables that mix channels
  - per-vertex texture matrix index (normal texgens done, bug 27)
  - mip LOD and LOD bias
  - CPU readback of copy textures (`boxBlurTexture`)

---

## 1. Groundwork (do first; everything else builds on it)

### 1.1 Measure the target hardware
Azahar ignores the N3DS clock boost and doesn't model GPU timing, so design numbers must come from real hardware. Add `tools/bench/` and an on-device benchmark mode (`env.txt` flag) that logs:
- RomFS `fread` throughput and latency for 4 KB, 64 KB and 1 MB reads, both sequential and random. This decides whether assets are stored uncompressed or LZ4.
- zlib inflate and LZ4 decode MB/s on core 0 and core 2.
- PICA fill rate for: an opaque textured quad, ETC1 vs RGBA8 sampling, 1-6 combiner stages, blending on and off, fragment lighting with 1/2/4/8 lights, proctex.
- Vertex throughput for the float 52-byte layout vs compact 20-byte layouts.

Every later "is it worth it" decision (number of fur shells, stereo frame rate, compression) uses these numbers.

### 1.2 Memory mode and packaging
- The port currently uses a 48 MB linear heap plus a 28 MB MEM1 arena, so it already relies on a large memory mode from the Homebrew Launcher.
- Add an optional **CIA build** (`makerom` + RSF) that requests the N3DS extended memory mode (178 MB, `SystemModeExt`) and exheader affinity for core 2. Keep the `.3dsx` as the default.
- Log the memory mode at boot. If the large modes are unavailable, fall back to smaller caches.

### 1.3 Unified memory: the GC arena in linear memory
Allocate the 28 MB MEM1 arena (`os_3ds.c:313`) with `linearMemAlign` instead of `memalign`. Grow the linear heap to match, for example 48 MB → about 96 MB.
- The GPU can then read textures and vertex arrays **in place**, straight from the game's heap. The second copy of every texture disappears (up to 20 MB), and so does every texture upload.
- Rule: game code writes with the CPU cache on. The port flushes (`GSPGPU_FlushDataCache`) only the ranges the GPU will read, once per asset load (textures, baked lists) or once per frame (dynamic buffers).
- This is the base for sections 2 and 3: with build-time PICA-format assets in linear memory, "load texture" means "read the file into the heap, flush, wrap a `C3D_Tex` around the pointer".

### 1.4 Core 2 worker, and later a render thread
1. **Worker pool on core 2** (`svcCreateThread(..., processor id 2)`), with a lock-free job queue in `port/`. First jobs:
   - asset file reads (section 2.4)
   - the per-frame gather for CPU-animated meshes (`gx_vtx_gather*`), started as soon as the game finishes skinning and joined before submit
   - video frame reads (section 7)
   - the audio stream reader (`audio_3ds.c:273`, currently on core 0)
2. **Core 1:** call `APT_SetAppCpuTimeLimit(30..80)` and put the low-latency, low-CPU work there (ndsp callback work, watchdog, file prefetch).
3. **Render thread (later, section 3.5):** the GX backend becomes a producer/consumer. Core 0 records GX calls into a frame command stream; core 2 does decode, state compile and citro3d submission one frame behind. This is the largest CPU gain available, since GX work (≥8 ms) then overlaps with game logic and scene traversal.

### 1.5 Shader variants instead of one uber-shader
Split `default.v.pica` into specialised programs, selected by the state compiler and batched by program:
- `unlit` (map blocks with `GX_LIGHT_NULL`; the most common case)
- `vlit` (vertex lighting, COLOR0)
- `flit` (fragment lighting: outputs a normal quaternion and the view vector)
- `warp` (water and screen-grid distortion, section 5)
- `skin` (optional GPU matrix palette)
- `hud` (2D: position, colour, one texcoord)

Each variant only declares the uniforms it uses, so most have plenty of free uniform space. That room is needed for stereo (section 8) and for a bigger matrix palette.

### 1.6 Build-time tooling foundation
- Add host tools to the pinned Docker image: Python with numpy and Pillow, plus small C/C++ tools built from source in the repo (an ETC1 encoder such as rg_etc1 or etcpak, and an LZ4 encoder). Pin the exact versions in the Dockerfile; nothing is downloaded at build time.
- **Compile the port's own `gx_vtx.c`/`gx_tev.c` logic for the host** as `tools/native/gxbake`. The build-time baker (section 3) then uses exactly the same decoder as the runtime fallback, so baked and runtime output can't drift apart.
- Use a content-addressed conversion cache (`build/<v>/cache/<sha1>`) and a process pool. ETC1 encoding of about 10k textures plus mips is slow the first time, so incremental builds must skip unchanged inputs. The conversion scripts are part of the extract stamp hash, as they are today.
- `extract_disc.py:convert_file` stays the hook point. Bank conversion works on bin+tab pairs, like `convert_banks` already does for MusyX.

---

## 2. Textures (build time)

### 2.1 Format policy (per texture, decided by analysing its pixels)

| GC source | Content | PICA target | bpp vs source |
|---|---|---|---|
| CMPR | no alpha | **ETC1** | 4 → 4 (same size, no runtime decode) |
| CMPR | 1-bit alpha | ETC1A4 (or RGBA5551 when cutout edges suffer) | 4 → 8 |
| RGB5A3 | fully opaque | RGB565, or ETC1 if PSNR ≥ threshold | 16 → 16 / 4 |
| RGB5A3 | alpha only 0/255 | RGBA5551 | 16 → 16 |
| RGB5A3 | real alpha | RGBA4, or ETC1A4 if it passes the quality check | 16 → 16 / 8 |
| RGBA8 | | ETC1A4 / RGBA4 / RGBA8 by quality check | 32 → 8..32 |
| I8 | | L8 + combiner swizzle "alpha = R" (GX I formats replicate intensity into alpha) | 8 → 8 |
| I4 | | L4 + swizzle | 4 → 4 |
| IA8 / IA4 | | LA8 / LA4 | same |

- The swizzle mechanism already exists (`unit_swizzle` / `channel_op` in `gx_tev.c`). Extend it to normal textures through a per-texture swizzle byte in the converted header.
- The quality check is a configurable PSNR/SSIM threshold per format. Add a build switch `TEX_QUALITY=high|balanced|small`.

### 2.2 Geometry of the texture
- **Mipmaps:** generate the full chain at build time with gamma-correct box/Kaiser filtering, down to 8x8 (the PICA minimum). Store every level in PICA tile order. Honour the source's min/max LOD and map `GXInitTexObjLOD` bias to the PICA LOD bias (`C3D_TexSetLodBias`). Mips fix the shimmer of distant textures and **reduce** texture bandwidth.
- **Non-power-of-two sizes** (1521 textures): for wrap = repeat, resample with Lanczos at build time. For wrap = clamp, pad to a power of two and store a UV scale in the header; the backend folds that scale into the texture matrix. Either way the runtime nearest-filter resample goes away.
- **Resolution budget:** the top screen has about 1/3.2 the pixels of 640x480. Add an optional per-texture policy that drops mip 0 for textures ≥256 px whose on-screen size never needs it (UI/font/HUD excluded, with an override list). Measure first; the default is to keep full resolution.
- **ETC1 layout:** 4x4 blocks in the PICA's 8x8 tile order, 64-bit words byte-reversed, as citro3d/tex3ds expect.

### 2.3 Container and the game's view of it
- Keep the game's 0x60-byte texture header, which game logic reads (size, animation frames, wrap, filters). Replace the pixel payload with the PICA payload, 128-byte aligned.
- Mark the texture with a reserved format value (`0x40 | pica_fmt`, never a valid GX format). Put a small port header (levels, swizzle, UV scale, data offset) in unused header bytes, or right after the header.
- `GXInitTexObj` receives that format. `gx_tex_get` then sees "already PICA" and calls `C3D_TexInitWithParams` around the pointer, with no decode and no cache copy. This needs section 1.3 (data in linear memory). Before 1.3 lands, it is a plain `memcpy` into linear memory, which is still roughly 100x cheaper than decoding.
- **Store entries as uncompressed `DIR` entries instead of `ZLB`.** The game already supports raw `DIR` entries (it returns a pointer into the bank buffer, `pi_dolphin.c` ~3504-3543), so this removes the zlib inflate from every texture load. Whether to add LZ4 on top is decided by the benchmark in 1.1 (only if SD reads are slower than LZ4 decoding).
- Rewrite `TEX0/TEX1/TEXPRE.tab` at build time, since offsets change (`(word & 0xFFFFFF) * 2` = offset, frame count in bits 29..24). The per-frame offset tables of animated textures (`tex0GetFrame`/`tex1GetFrame`) are regenerated too.
- Textures the game generates at runtime (noise frames, heavy-fog texture, `blendTextures`, box blur) stay on the runtime GX path, or are replaced by sections 4.4 and 5. They currently force re-conversion every frame they change.

### 2.4 Loading: stop holding whole banks
With random-access storage, holding every map's compressed TEX/MODELS/ANIM in RAM is wasted memory.
- Patch `mapLoadDataFile` so TEX banks load **only their tab**. `loadAndDecompressDataFile` already has a fallback that opens the file and reads one entry by offset (~lines 3556-3590); make it the normal path.
- Issue the entry reads as worker jobs on core 2 when the game requests them through `DVDReadAsyncPrio`. The game's async state machine (callbacks, `gAssetLoadInFlightFlags`) then really overlaps I/O with gameplay instead of completing instantly on the main thread.
- Add an LRU of recently used textures in linear memory, sized by the memory mode (for example 24 MB in 124 MB mode). Leaving a map no longer frees everything that the next map reuses.
- **VRAM:** the 6 MB of VRAM holds render targets (2 eyes × color + depth ≈ 1.5 MB), copy and render-to-texture targets, and a small set of hot textures (font, HUD, fur noise, water normals). Texture fetch from VRAM is faster than from FCRAM.

### 2.5 Verification
- `tools/tex_check.py` decodes each converted texture back to RGBA, compares it with the GC decode, and writes a report with the worst PSNR per format.
- A runtime debug flag forces the old runtime path so the two can be compared on screen.
- Counters: `gx.tex_converted` should drop to roughly the number of runtime-generated textures.

---

## 3. Models, map blocks and display lists (build time)

### 3.1 Bake display lists into PICA-ready buffers
Today `GXCallDisplayList` decodes on first use and caches by hash. Do it at build time instead:
- `tools/native/gxbake` loads each `MODELS.bin` entry and each `mod*.zlb.bin` block the same way the game does, using the unpack logic from `model.c:modelUnpackFileData` and `tex_dolphin.c:mapBlockUnpackFile`, run on the host. It sets up the vertex state the game would set (VCD/VAT from the data, plus the CP register loads inside the lists) and decodes every list with the shared decoder.
- Output per list: a **compact PICA vertex buffer** and a u16 index buffer (triangle lists; quads and strips converted). Also emit the matrix-load replay table and the vertex-state hash used to validate it.
- Compact layouts, chosen per list from the source precision and read natively by the PICA:
  - position s16x3 with a per-list scale uniform (same scheme as `posscale`)
  - colour u8x4
  - texcoords s16x2 scaled
  - normal s8x3
  - matrix index u8

  That is 20-24 bytes per vertex instead of 52, so vertex fetch bandwidth and cache memory are halved.
- Storage: a sidecar file per bank (`MODELS.pica`, `modN.pica`), loaded with the bank into linear memory. Lookup by (bank, list offset) through a table built at load time, so the game's data layout doesn't change.
- **At runtime:** `GXCallDisplayList(ptr)` → look up the baked entry → check the vertex-state hash → bind the buffers and draw. No decode at all. A hash mismatch (unexpected state) falls back to the existing runtime decoder and `gx_dlcache`, and logs a counter so the baker can be fixed.

### 3.2 Animated (CPU-skinned) models
Game skinning is cheap (<0.5 ms). The cost is the gather and upload of the skinned output.
- Bake the static attributes (colour, texcoords, indices) plus the source→output position/normal maps that the gather mode currently has to learn at runtime.
- Run the per-frame gather on core 2 (1.4), writing into the `GxDyn16Vertex` buffers.
- **Optional GPU skinning:** the `skin` shader variant with a 20-matrix palette, with per-vertex bone indices and weights baked at build time from the joint blend data. This removes CPU blending and the gather. Worth doing only if 1.1 shows the gather still costs >1 ms after moving it to core 2.

### 3.3 Map block data
- Water geometry: **subdivide water polygons at build time** to a regular grid (about 1 world unit; tune per map) so that per-vertex refraction (section 5.1) has enough vertices. Mark the baked water lists with a flag.
- Static points and lines (sky star display list) are expanded to quads at build time (section 6.4).
- `mapBlockUnpackFile` byte-swaps vertices on load. That is a one-time load cost, so leave it unless profiling shows map loads stutter.

### 3.4 Verification
`gxbake` dumps a per-map report: lists baked, fallbacks, the largest vertex-count lists, and any CP registers it didn't understand. At runtime, `gxdump` already logs per-batch state; add a "baked/fallback" field. The existing dltrace on display-list errors still covers the fallback path.

### 3.5 Render thread (after 3.1; this is the big CPU win)
Once lists are baked, a call is a lookup plus a draw, so the GX stream is cheap to record.
- Core 0 records GX calls into a per-frame command stream. Matrices and immediate data are copied into the stream; pointers to baked buffers and textures are stored as-is.
- Core 2 replays the stream: state compile (with the TEV program cache from 4.1), citro3d calls, `C3D_FrameEnd`.
- Hazards:
  - the game rewrites vertex arrays it has handed to GX. It already double-buffers `vtxBuf`, so check every dynamic-array source.
  - `GXPeekZ` and copy readbacks (both become frame-late reads in section 6)
  - texture memory freed or reused while the render thread still uses it (defer frees by 2 frames, like `gx_copy.c` does)
- Latency rises by one frame; that is acceptable at 30 fps.

---

## 4. TEV, lighting and the fixed-function basics

### 4.1 Faster and more complete TEV compilation
- **Cache compiled programs.** Hash the TEV state (stages, swaps, konst selectors, unit map) into a 256-entry cache of compiled `C3D_TexEnv` arrays and buffer masks. Today `compile()` runs on every TEV state change. Konst and register *values* are patched into the cached program, not hashed.
- Missing operations:
  - `GX_CS_DIVIDE_2`: run the stage at scale 1 into the buffer and halve it in a following stage, or fold 0.5 into a constant operand where possible.
  - **Compare ops** (`GX_TEV_COMP_R8_GT`, `RGB8_GT`, `A8_GT`; used by shadows and the viewfinder): when the result feeds only alpha, use `GPU_SUBTRACT` scaled by 4 into alpha plus the alpha test. Shadow-map compares are replaced by hardware shadows (6.3).
  - Swap tables that mix channels (spirit vision luminance): recognise the luminance pattern and emit `GPU_DOT3_RGB` with a weights constant.
  - Use `GPU_ADD_MULTIPLY` and `GPU_MULTIPLY_ADD` for more `d + a*b` and `(a + b) * c` shapes, so fewer programs need two PICA stages.
- **Per-material overrides** (section 5) take over the effects the generic compiler can't express. The generic compiler handles the long tail of simple 1-4 stage programs, which are most draws.
- When a program still needs more than 6 PICA stages or 3 textures:
  1. drop the stages that only have a small visual effect, ordered by a per-building-block priority table: fades first, then the small reflection, then projected lights 2 and 3
  2. else use multipass with additive or multiplicative blending, for programs whose tail is `prev + x` or `prev * x`
- **Status (2026-10-09):** the cache, the drop priority and multipass are done.
  - Multipass code: `gx_tev_multipass` in `gx_tev.c`, `multipass_prepare` / `draw_second_pass` in `gx_render.c`, toggle `S3D_TEV_MULTIPASS`.
  - When it applies: exactly one texture found no unit, and its stage is `prev + b*c` or `prev * x` with nothing changing the colour after it. Pass 2 binds that texture on a unit whose texgen source fits and draws the triangles again. Only opaque draws that write depth qualify.
  - Pass 2: one stage, depth `EQUAL` with an RGB-only write, blend `ONE, ONE` (add) or `DST_COLOR, ZERO` (multiply). Fog fades the term to black (add) or white (multiply). For multiply, that only approximates fogging the product.
  - Counters: `tev.multipass_add`, `tev.multipass_mul`, `gx.multipass_draws`, and the skip reasons `tev.multipass_skip_two` (two textures lost), `tev.multipass_skip_form` and `tev.multipass_skip_unit`.
  - Azahar replay: it never triggers. The 6 skips per frame are a 6-stage program whose stage 4 is `lerp(tex, prev, Aprev)`; that needs the previous alpha, which only exists inside pass 1. The other dropping draws lose two textures.

  Log both with counters (`tev.truncated` exists today).

### 4.2 Lighting with the PICA fragment lighting unit
The game uses COLOR0/1 and ALPHA0/1 separately: diffuse with `GX_DF_CLAMP`, spot and distance attenuation, and specular with `GX_AF_SPEC` (`modellight.c`). It also does per-pixel map point lights and lit-hemisphere textures in TEV.
- **COLOR1 (specular):** route it through fragment lighting. GX spec lights map to PICA lights with the `D0` LUT set to the GX specular function, and the result appears as `GPU_FRAGMENT_SECONDARY_COLOR` in the combiners. That fixes "second channel reads as zero" and looks better (per-pixel).
- **Spot and distance attenuation:** PICA has per-light spotlight and distance-attenuation LUTs. Generate a LUT from each GX `(a0, a1, a2)` / `(k0, k1, k2)` set when first used and cache it by parameters. The sets are few and change rarely.
- **Map per-pixel point lights** (`addFirst/Accum/DirectPointLightStages`, falloff textures projected from POS): replace them with up to 2 fragment point lights with distance LUTs. This frees one or two texture units and two or three combiner stages per map draw.
- **Lit-hemisphere textures** (`Rcp_UpdateDistortionTextures`, 6 per frame at 32x32, used as reg1/reg2 sphere-map lighting): with fragment lighting enabled, skip both the RTT and its TEV stages, and feed the same lights to the fragment unit instead. This removes 6 offscreen renders and copies per frame.
- **Diffuse COLOR0 stays per-vertex** in the `vlit` variant (cheap; most map geometry is unlit or ambient-only anyway). Objects with specular or spot lights use `flit`.
- Combiner mapping of GX `mat * clamp(amb + Σdiffuse)` when ambient comes from the vertex: fragment ambient 0, then `GPU_ADD_MULTIPLY (frag_primary + vtx_amb) * mat`.

### 4.3 Fog: hardware fog LUT
- The game only uses `GX_FOG_PERSP_EXP` (5 sites) and `GX_FOG_NONE`, with no range adjust.
- Build a 128-entry PICA fog LUT from `(startz, endz, nearz, farz)` (citro3d `FogLut_Exp`-style), cached by parameters. Set `C3D_FogColor`, and toggle `C3D_FogGasMode(GPU_FOG / GPU_NO_FOG)` per batch from the per-material fog flag.
- The PICA fog index uses depth: compute the LUT over the PICA depth mapping actually used. Consider **w-buffering** (`C3D_DepthMap` in w mode) for large outdoor maps; it gives better depth precision and linear fog input.
- "Heavy fog" (the TEV/indirect height fog) is handled in 5.5.

### 4.4 Procedural texture unit
`newShadowsInitProceduralTextures`, `allocLotsOfTextures` and per-frame noise frames generate noise, ramps, disks and rings on the CPU.
- **Ramps and disks:** bake at build time as tiny L8/A8 textures, or replace with the proctex unit's colour LUT plus radial/U/V mapping. Proctex is a fourth texture source (`GPU_TEXTURE3`) that doesn't use one of the 3 texture units.
- **Noise frames** (fur, grass, caustics): proctex noise with per-frame phase/offset uniforms, or a baked noise flipbook (section 5). Either way the per-frame CPU noise generation and re-upload goes away.
- **Status (2026-10-09): not used.** Proctex takes its coordinates from texcoord 0, 1 or 2, so it shares them with a real unit. The fur noise needs its own coordinates (37.5x tiling plus the wind offset), and in the only place where a fourth texture was needed (fur lit by a projected light) all three coordinate sets were taken. The fur now drops the light texture instead (`port_fur_proj_light`). The noise frames are generated once at startup, not per frame, so there is no per-frame cost to remove.

### 4.5 Texgen and matrices
- Dual-transform texgen (`GX_PTTEXMTXn`): when normalize is off, multiply post × pre on the CPU into the one matrix the shader uses. When normalize is on (a few sites), the `vlit`/`flit` variants do the normalize in the shader.
- `GX_TG_NRM` sphere maps already work through `texsrc.z`. Bump texgen (`BINRM/TANGENT`) is only used by the bump env map (5.6).
- Per-vertex `TEXnMTXIDX`: done for normal texgens (bug 27, OPEN_BUGS.md). Skinned models' sphere maps use the joint's position matrix 3x3 times a uniform scale, so the shader turns the normal by `posmtx[a0.x]` (posscale.w = 2, gx_render.c `nrm_pervertex`, `S3D_TEXMTX_PV`). The joint texture matrices were never even loaded before: renderOpMatrix read their IDs past `gObjGxPosMtxIdTable` (.data order differs from the GameCube; patches/main/objprint_dolphin.c.sed, `S3D_JOINT_TEXMTX`). Still unsupported: position texgens with TEX6/7MTXIDX (projected lights on skinned models, `gx.texmtxidx_other`).
- Use a real normal matrix (`xf_nrm`) instead of the position matrix in `light` when the game loads non-uniform scale matrices (fur shells scale joints).

### 4.6 Depth-bias viewports
`Camera_ApplyEffectDepthViewport`/`Decal`/`Transparent` use negative near Z (-0.075 / -0.05 / -0.01) to bias depth. Map viewport near/far (`vp[4]`, `vp[5]`, ignored today, `gx_render.c:657`) to `C3D_DepthMap(zScale, zOffset)`. This removes z-fighting on decals and effects.

### 4.7 Blend and pixel modes
Logic-op OR (shadow bitmasks) and multiply-invert are supported (`C3D_ColorLogicOp`). Confirm that the colour mask (`GXSetColorUpdate` off for depth-only draws) maps to `C3D_DepthTest` write masks; it is used by cloud/sun-flare occlusion and teleport drawing.

---

## 5. Effects that rely on indirect texturing

Strategy per effect: **bake what is periodic, warp per vertex what depends on the screen, and use fragment lighting for bumps**. Each override is selected by the game's own material building blocks. Add a hook in `shader_dolphin.c`'s `add*Stage` functions (a sed patch that sets a port "semantic" flag) so the backend knows what the TEV program *means* without reverse-engineering it from the register state.

### 5.1 Water surfaces (refraction and reflection)
- **Status (2026-10-18, bug 28):** the game copies the whole screen (newshadows_captureReflectionTextures) and samples it twice with position texgens: refraction (light perspective scaled 0.4) and a Y-flipped "reflection" (scale 0.5/-0.5), lerped by K0.a. The port draws it as a water split (two passes on unit 0) with `gx/water.v.pica` and unit 0 in GPU_TEX_PROJECTION mode, so the divide by q is per fragment (S3D_WATER_PROJ); copies get their padding filled with edge pixels (S3D_COPY_EDGE). Build-time water subdivision (3.3) is not needed for this any more. Verified on hardware (EBA4E4C4). Details in OPEN_BUGS.md bug 28.
- In the game: the screen texture projected by screen-space texmtx and warped by a two-stage indirect caustic, tinted with KColor (`mapBlockRenderWater`, `setupWaterCausticTev`, ripples in `waterfx_render`).
- On 3DS: the `warp` shader on the build-time-subdivided water mesh (3.3). It computes the projected screen UV per vertex plus an offset from a sum of 2-3 analytic waves of world position and time (uniforms). Combiners then do screen texture × tint, plus a flipbook caustic overlay (5.3).
- Ripples and wakes: same shader, with the ripple centres as uniforms (up to 4), or as a radial offset on the ripple quads.
- Grid density makes the warp look smooth at 400x240. Cost: one vertex shader pass, with no dependent reads.

### 5.2 Heat haze, distortion shockwave, screen warps
- In the game: the full-screen copy warped by a rotating indirect matrix, weighted by the Z8 depth copy (`doHeatEffect`, `doDistortionFilter`, map heat shimmer).
- On 3DS: draw the screen texture through a **screen-space grid** (40x24 quads, about 2k vertices) in the `warp` shader. Per-vertex UV offset = analytic function (heat: scrolling sines; shockwave: radial ring from centre/radius uniforms). Depth weighting is done per vertex by sampling the depth copy on the CPU one frame late (section 6.2). It can also be dropped, using constant strength below the horizon.
- Map heat shimmer (block flag 0x80): per-vertex offset on that block's geometry, in the `warp` variant.
- **Status (2026-10-09):** heat and shockwave are done (`port/screenwarp_3ds.c`, `S3D_SCREEN_WARP`): a 40x24 grid with the indirect offset computed per vertex on the CPU. Heat uses the scrolling caustic as sines; the shockwave uses the rotated radial push. There is no depth weighting; the game's own top/bottom alpha ramp is kept. Not reached in the Azahar replay, so not checked on screen.

### 5.3 Caustics, warped noise, warped ring, decal noise: build-time flipbooks
- These are static textures warped by scrolling textures. The result is periodic (or close enough).
- At build time, simulate the GX indirect warp exactly (offline, in Python/numpy, with the same matrices and scroll speeds taken from game code and data) and bake **N frames (32-64) of the warped result** as a small L8/ETC1 flipbook. At runtime, pick the frame from the time and cross-fade between two frames using the combiner (interpolate with a konst alpha).
- Memory is tiny (for example 64x64 L8 × 64 = 256 KB).
- **Status (2026-10-09): done as a per-vertex warp, without flipbooks.** The game fills the warped textures at runtime from its RNG and projects them from world position, so a build-time bake can't match them. Instead the caustic base, wavy caustic, warped noise and warped ring stages get the analytic vertex-shader swirl used by the water (`s_warp_sites` in `gx_tev.c`, `S3D_CAUSTIC_WARP`, counter `gx.tex_warp`).

### 5.4 Fur and grass shells
- In the game:
  - 3/7/15 shell redraws (16 for Fox/Krystal close up) with joint matrices scaled about the pivots
  - a per-layer noise threshold (`TEV_SUB + ADDHALF`)
  - an indirect wind warp, bump-alpha shading
  - a shadow-table texture on layers ≥12
- On 3DS:
  - **Geometry:** keep the shells. They are re-calls of cached/baked lists with scaled matrices, which is cheap with GPU transform.
  - **Density mask:** a baked fur noise texture (L8, mipmapped, in VRAM), or proctex noise. The combiners compute `noise - layer_threshold`, and the alpha test discards. Threshold = konst per layer.
  - **Wind:** the indirect warp becomes a per-layer texture-matrix shear (offset ∝ layer² × wind vector). Outer shells bend more, and no dependent read is needed.
  - **Bump-alpha shading:** replace it with a per-layer constant darkening (inner shells darker, cheap self-occlusion), or fragment-light bump with a baked fur normal map if 1.1 shows spare fill rate.
  - **Shell count from fill-rate budgets:** for example 8 for Fox close up, 4 mid, 2 far, 0 in stereo far. Tunable at build time.
  - Layers ≥12 shadow table: becomes fragment lighting with one directional light, if those layers are kept.
- Grass (`mapBlockRender_drawLightmapIndirectPasses`, 4/8/16 passes offset 0.4 in Y) gets the same treatment, capped at 4-6 passes.
- **Status (2026-10-09):** wind, shell budget, per-layer shade and the grass pass budget (8 by default) are in (table, row 8). The bump-alpha read and the shadow table on layers 12 and up are dropped.

### 5.5 Heavy fog (scrolling world-space fog layers)
Hardware fog LUT (4.3) for depth fog, plus one combiner stage that multiplies by a baked scrolling fog-noise flipbook (5.3), projected from world position in the vertex shader. The second indirect stage is dropped. Optional later: PICA gas mode for volumetric fog in specific maps.

**Status (2026-10-09): done as an approximation.** `renderHeavyFog`'s indirect stage becomes a soft per-vertex swirl on the fog texture coordinate (`s_warp_sites`, under `S3D_WATER_WARP`), so the fog bank's edge rolls instead of standing still. The depth fog (4.3) is drawn as before. There is no flipbook (see 5.3) and no gas mode. Not reached in the Azahar replay.

### 5.6 Bump environment mapping (`addEnvMapBumpStages`)
- In the game: tangent/binormal texgen plus `ITB_ST` with dynamic indirect matrices, i.e. the classic GameCube bump-env.
- On 3DS: **PICA fragment-light bump mapping**. At build time, convert the bump height maps into tangent-space normal maps (RGB565/ETC1). The `flit` shader outputs the tangent-frame quaternion; fragment lighting uses the normal map, with the env-map reflection approximated by specular LUTs and the existing sphere-map stage.
- Cheaper alternative if fill rate is tight: plain sphere/cube env map without bump.
- **Status (2026-10-09): the cheaper alternative is done.** Without indirect stages, the bump stages only passed the colour through. `port_env_bump_skip` (`gx_tev.c`, `patches/main/shader_dolphin.c.sed`, `S3D_ENV_BUMP_SKIP`) now takes the game's own bump-off path: the plain sphere map, with 2 TEV stages, 3 texgens and one texmap fewer (counter `env.bump_fallback`, 1 per frame). Fragment-light bump with baked normal maps is not done.

### 5.7 Ice, caustic reflection, projected-indirect objects, pause hologram, particle refraction
- Ice (`objFrozenRenderCb`): NRM sphere-map env (already works) plus fragment specular; the indirect is dropped.
- Others: per-vertex UV jitter from an analytic function in the `warp` shader, or dropped. Low priority, one-off effects.

### 5.8 Fallback
Any indirect stage without an override keeps today's behaviour (the warp is ignored, the base texture is sampled directly), with a counter per building block so the remaining gaps are visible.

---

## 6. EFB copies, post effects, shadows, readbacks

### 6.1 Offscreen passes become render-to-texture
- In the game, scratch renders go into the EFB and are then copied and cleared: shadow casters, the shadow map, the blob shadow, lit-hemisphere spheres, the small reflection.
- On 3DS:
  - patch the begin/end of each scratch pass (`newshadows.c:renderShadows`, `renderObjectShadowTexture`, `drawReflectionTexture`, `rcp_dolphin.c:Rcp_UpdateDistortionTextures`) to call `gx_offscreen_begin(dest, w, h, fmt)` / `gx_offscreen_end()`
  - the backend renders into a `C3D_RenderTarget` created from the destination texture (VRAM), at the requested size
  - the later `GXCopyTex` to that destination becomes a no-op
- Benefits:
  - no framebuffer clobbering
  - smaller targets (64-256 px) render faster
  - no copy-queue budget pressure (`QUEUE_BUDGET` is 24)
  - required for stereo: offscreen passes are rendered once, not per eye (section 8)
- **Status (2026-10-09): done** for the object shadow, the shadow casters and the distortion slots (`gx_offscreen_begin/end`, `S3D_OFFSCREEN`, counter `gx.offscreen`). The pass draws into a square 32/64/128/256 px scratch target with the screen's rotated layout. `GXCopyTex` then reads from it, so the copy is kept rather than made a no-op. If VRAM runs out, the pass draws into the EFB as before. The small reflection uses a mip alias instead (6.2).

### 6.2 Screen copies and post effects
- `updateReflectionTextures` (full screen colour + Z8 depth), motion blur, DOF blur, heat, distortion, colour and spirit-vision filters, glows.
- Screen copies stay as GPU `GX_TextureCopy` (already done), into textures per eye.
- **Blur and DOF** (`doBlurFilter`, 7 stages today): build a blur pyramid with the **display-transfer engine's hardware 2x downscale** (400x240 → 200x120 → 100x60; `GX_TRANSFER_SCALE_XY` averages 2x2). Draw back with bilinear filtering, and blend by depth (Z copy) in two combiner stages. The 7-stage multi-tap TEV blur isn't needed.
- **Small reflection 80x60:** take it from the same pyramid (the 100x60 level) instead of a redraw plus a separate copy.
- **Motion blur:** previous frame's copy blended with KAlpha. Already expressible; check that the copy is per eye and ping-pongs.
- **Glows:** additive quads. They need `GXPeekZ` (6.4).
- **Status (2026-10-09):** the blur samples coarser mip levels of the screen copy (row 6). The 80x60 small reflection is a coarser mip level of the 320x240 copy (`port_copytex_mip_alias`, `S3D_REFLECT_MIP`), with no redraw or second copy. Bug 32 (2026-10-19): the floors that add it (shader flag 0x100) now sample it per fragment in a second pass on projective unit 0 with the water shader (`S3D_REFLECT_SPLIT`); before, the per-vertex divide smeared it into lines. Verified on hardware (D043305B). There is no display-transfer pyramid and no depth-weighted DOF blend.

### 6.3 Shadows
- **Projected shadow textures (R4 copy, logic OR bitmask casters):** render into an RTT (6.1) with logic op OR (supported). Then project with the existing texgen. Works once 6.1 is in.
- **Real shadow map (Z8 copy + `TEV_COMP_R8_GT` against a ramp, self-shadowing):** use **PICA hardware shadow mapping**. Render light-space depth into a shadow texture (PICA shadow render mode), then sample with texture unit 0 in shadow mode; the hardware does the compare and gives soft penumbras. This replaces the copy, the ramp texture and the compare stage.
- **Blob shadow plus CPU `boxBlurTexture`:** CPU readback of a GPU texture must go (it would stall, or read stale data with the async copies). Render the blob into a 128x128 RTT, then blur on the GPU with two 2x hardware downscales, or by binding the same texture to 3 units with offset texture matrices and averaging in the combiners. Patch `boxBlurTexture` to call the port hook.
- The pause-menu character portrait (`pauseMenuRenderSlotShadow`) uses the same path.
- **Status (2026-10-09):** projected shadow textures and the shadow casters now render through the offscreen pass (6.1), and the blob shadow blur runs on the GPU (`port_copytex_blur`, `S3D_GPU_BLUR`). PICA hardware shadow mapping for the real shadow map is not done: the game's Z8 copy, ramp texture and compare stage still go through the generic path.

### 6.4 `GXPeekZ` (lens flare, sun and glow occlusion)
- The game queues up to 20 points and reads them one frame late (`depthReadRequestPoll` → `videoBreakPointCallback`), which fits the GPU nicely.
- At frame end, issue a small `GX_TextureCopy` of the depth tiles that contain the queued points into a linear buffer. On the next frame, read the D24 values on the CPU: tiled and rotated addressing as in `gx_copy.c`, inverted (PICA depth is `1 - GX depth`), rescaled to GX's 24-bit range.
- Flares, sun glare and glow lights then get occluded correctly instead of always drawing.

---

## 7. Video playback (attract movie) — build time

### 7.1 Choice
- **Primary: pre-decoded ETC1 frame stream.**
  - At build time, decode all 1797 MJPEG frames on the PC: THP frames are JPEG, decodable with Pillow after fixing the THP/JPEG markers.
  - Scale each to the displayed size (320x240 for the 4:3 pillarbox) and encode it as an **ETC1 texture of 512x256** in PICA tile order: 64 KB per frame, about 115 MB total.
  - At runtime, playback is one 64 KB SD read per frame (1.9 MB/s) straight into a linear texture buffer, on the core-2 worker, double/triple buffered. There is **no decode on the console**.
- **Optional: N3DS hardware H.264 (MVD service).** Encode with x264 baseline at build time. That is much smaller on disk, but MVD needs N3DS-only service handling and Azahar may not emulate it. Since storage is unlimited, the ETC1 stream is simpler and costs less at runtime. Keep MVD as a documented alternative only.

### 7.2 Audio
Done. At build time `tools/movie_audio_convert.py` decodes the THP-ADPCM track (39 coefficient sets per channel in starfox.thp) and re-encodes it as DSP-ADPCM with the one set per channel that predicts the track best, because an ndsp channel holds one set. The `.a3d` file also holds a frame table (hash of each frame's audio header and its first sample) and a seek table (decoder history every 16 blocks). At runtime `THPAudioDecode` returns silence of the right length and records which track sample each game audio buffer starts at. The game's player, mixer and video timing are unchanged. `movie_audio_pump` plays the track on channels 22/23 from the sample the game is mixing, less the AI output latency, at the AI DMA rate. It restarts the track at a seek point if the two drift more than 0.2 s apart (a skipped frame or a loop). Without the file, with `S3D_MOVIE=0` or while a disc stream holds the channels, the game decodes the audio in software as before.

### 7.3 Integration
- `tools/video_convert.py`, hooked from `convert_file` for `starfox.thp`. It writes `starfox.v3d` (header: frame count, fps, size, offsets) and `starfox.dsp`. Keep the `.thp` file present, unchanged or truncated, so the game's open/size checks still pass.
- Patch `thp/dll_3e.c:DecodeNextMovieFramePC` and the THP audio decode to call the port player instead of `THPVideoDecode`/`THPAudioDecode`. Remove the black-frame sed patch (`patches/aurora/thp_internal.sed`).
- Drawing: the game draws the movie as three I8 Y/U/V textures with a YUV→RGB TEV program (`addYUVVideoTevStages`, flag 0x20000). This happens both on the title screen (`AttractMovie_DrawTextureCallback`) and **on a model texture** (`704.c:926`, pilots sequence). Register the game's Y-plane buffer address as a **video alias**, like `gx_copytex_find` maps copy destinations. When that texture is bound, the backend binds the current ETC1 frame, and the YUV stages compile to a single REPLACE. Both uses work without game changes beyond the decode hook.

---

## 8. Stereoscopic 3D

### 8.1 Design: record once, replay per eye
- Game logic, scene traversal, list lookup and state compilation run **once**. Only GPU work is doubled.
- The backend records the frame as segments:
  - **Shared segments:** offscreen passes (6.1: shadows, RTTs, lighting spheres), rendered once.
  - **Per-eye segments:** each consists of state and draws, and ends either at frame end or at a screen copy.
- For each per-eye segment, citro3d's GPU command buffer range is captured once. **Patch points** are recorded: the projection uniform writes, the framebuffer address registers, and the texture address registers of per-eye copy textures. The range is then submitted twice with the patched values. The CPU cost of the second eye is a `memcpy` plus a few dozen register patches. If the render thread (3.5) exists, all of this runs on core 2.
- Screen copies (`updateReflectionTextures`, Z8 copy, motion blur source) exist per eye (`GxCopyTex` keyed by `(dest, eye)`), so post effects stay consistent in each eye.
- `GXPeekZ` reads the left eye only.

### 8.2 Eye projection
- Inject the eye offset in `upload_projection` (`gx_render.c:409-453`) for `GX_PERSPECTIVE` projections only: `x_clip += eye × iod × (w - convergence)` (the same model as citro3d's `Mtx_PerspStereoTilt`).
- Orthographic passes (HUD, text, `screenImageDraw`, 2D filters) get no offset, so they sit at screen depth. Optionally put a small constant parallax on the HUD so it floats just in front of the screen. Note the shadow ortho pass is offscreen and shared (8.1).
- **Convergence:** from the camera's target distance (`camera.c:Camera_UpdateViewMatrices`), so that Fox sits on the screen plane. Smooth it over a few frames.
- `iod` comes from `osGet3DSliderState()`. With the slider at 0: `gfxSet3D(false)`, a single eye, and **no cost at all**.
- Full-screen post effects drawn in screen space (heat grid, distortion, colour filter) use each eye's own copy, with no offset of their own.

### 8.3 Performance policy
- Stereo doubles GPU fill and vertex work. Add a user option: "3D: 30 fps" (default) and "3D: reduced", which halves the fur shells, skips DOF and motion blur, and uses lower shell/grass counts in stereo.
- VRAM: second colour and depth target, plus per-eye copy textures. About 1 MB more; fits in VRAM with the plan in 2.4.

### 8.4 Verification
- Azahar renders stereo, both side-by-side and anaglyph, so check eye separation and HUD depth there.
- Check comfort on hardware: convergence on Fox, no HUD window violations, no effect that draws in one eye only.

---

## 9. Screens and presentation

- **Fill the 5:3 screen:** the game has a 16:9 (anamorphic) option. Map it to 400x240 instead of pillarboxing to 320x240: adjust `upload_projection`'s viewport fold, plus the scissor and copy mapping in `gx_copy.c`. Make it the default.
- **Wide mode when 3D is off:** `gfxSetWide(true)` gives 800x240 on the top screen. Render at 800x240 for twice the horizontal resolution. This costs 2x fill, so it is a user option, and stereo turns it off automatically.
- **Hardware AA:** render at 2x vertical (400x480) or 2x2 and let the display transfer downscale (`GX_TRANSFER_SCALE_Y`/`XY`). Use it on the title and menu screens, or when 1.1 shows spare fill rate.
- **Bottom screen HUD:** draw the minimap (`dlls/engine/49`), item/C-menu ring and status on the bottom screen. Add a second render target and redirect specific UI draw calls with small patches to `curUiDllDraw` and the minimap DLL. The top screen then gets a clean view, the touch screen can be used for the item ring, and top-screen work drops. The debug console moves to a debug-only toggle.

---

## 10. Order of work

Each step leaves the game working, so each can be built and tested in Azahar.

| # | Step | Depends on | Main gain |
|---|---|---|---|
| 1 | Benchmarks (1.1), worker on core 2 (1.4.1-2), CIA/memory mode (1.2) | — | Real numbers, extra cores |
| 2 | Build-time textures + mips + NPOT (2.1-2.3, runtime memcpy path) | 1.6 | Texture memory /4-8, mips, no runtime decode |
| 3 | Basics: fog LUT (4.3), depth-bias viewports (4.6), points/lines (6.4/3.3), `GXPeekZ` (6.4), TEV cache + ops (4.1) | — | Visible correctness everywhere |
| 4 | Video: ETC1 stream + DSP audio (7) | 1.4, 1.6 | Attract movie and pilots sequence |
| 5 | Unified memory (1.3) + on-demand asset loading (2.4) | 2 | RAM, zero-copy textures, no zlib |
| 6 | Offscreen RTT + hardware-downscale post effects + GPU blur (6.1-6.3, except shadow maps) | 3 | Post effects, shadows, readback removed |
| 7 | Shader variants (1.5) + fragment lighting (4.2) + proctex (4.4) | 3 | Specular, spot lights, frees stages/units/uniforms |
| 8 | Indirect replacements: flipbooks (5.3), fur (5.4), water/heat grid warps (5.1-5.2), heavy fog (5.5), hardware shadow maps (6.3) | 6, 7 | The game's signature effects |
| 9 | Baked display lists + compact vertices (3.1-3.4) | 1.6, 5 | Most of the remaining CPU time |
| 10 | Render thread (3.5) | 9 | GX work runs alongside game logic |
| 11 | Stereo 3D (8) | 6, 10 (works without 10, but slower) | 3D display |
| 12 | Widescreen, wide mode, AA, bottom-screen HUD (9); bump env (5.6) | 11 | Presentation |

---

## 11. Risks and open questions

- **Azahar vs hardware:** fragment lighting, proctex, shadow textures and gas/fog are emulated by Azahar, but must be validated on a real N3DS; edge cases of the shadow and LUT units differ. MVD (only used by the optional path) may not be emulated. All performance tuning must be done on hardware.
- **Exact GX semantics in build-time bakes:** the caustic/noise flipbooks and the baked display lists depend on reproducing game parameters offline. Mitigate by sharing code with the runtime (host build of `gx_vtx.c`), by hash validation with runtime fallback, and by A/B debug toggles.
- **Game assumptions about texture bytes:** some code may read texture pixels (procedural blends, `boxBlurTexture`, CPU-generated textures). Those textures must stay on the GX path; the converter must skip them (list them during step 2 through a debug hook that logs CPU reads of PICA-format textures).
- **Tab rewrite correctness:** offsets, frame tables and the A/B map-slot bits must all be right. Add a build-time self-check that re-parses every rewritten bank and compares the entry count, sizes and frame counts with the original.
- **Memory mode availability:** if a user's launcher gives a smaller memory mode, the caches in 2.4 must shrink, and unified memory must still fit (arena + audio 15 MB + baked lists + textures).
- **Fill rate:** fur shells, stereo, wide mode, AA and post effects all compete for PICA fill rate. Every one of them gets a measured budget (1.1) and a user-visible or build-time knob.
- **foxhollow stays untouched:** all game-side hooks are sed patches in `3DS/patches/` (build fails if one stops matching), following the existing convention.
