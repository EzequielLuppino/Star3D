# Dialog fur look plan (OPEN_BUGS.md 2)

Goal: dialog close-ups of Fox should look like the GameCube reference
(`Screenshot 2026-10-11 005120.png` in the repo root): soft, fluffy fur over a
blurred background. Gameplay fur stays off (GameCube behaviour).

## What the capture shows (20261011_005010_f1944, taken before the white fix)

- An 8x crop of Fox (build/emu/fox_crop.png) has no DOF staircase: the blocks
  are plain 400x240 pixels. The GameCube draws the same scene at 640x480,
  2.56x the pixels, and the VI scaler softens it further.
- The fur noise is linear-filtered, but it has no mip levels (none on
  GameCube either). At 37.5x tiling, one screen pixel covers several noise
  texels, so it samples them as speckle.
- The shells already alpha-blend (`GX_BM_BLEND`, src alpha, no z write), so
  the hard look comes from the sampling, not from an alpha test.
- The dialog draws 8 of up to 15 shells (port_fur_step budget).
- The background blur (doBlurFilter) samples mip 1/2 of the screen copy. That
  is a lighter blur than the GameCube's at a lower resolution. The copy6 depth
  copy has wrapping bands (OPEN_BUGS.md 11).

## Hardware limits that shape the options

- VRAM is 6 MB. The 240x400 top target uses about 0.75 MB (RGBA8 plus D24S8).
  A 480x800 supersampled target needs about 3 MB.
- Fill rate: every shell is a full Fox model pass, and stereo doubles it. The
  hub runs at about 17 fps; dialogs are mostly static camera shots.
- 3 texture units, no indirect stage. The fur already needs 4 textures, so
  any extra texture means losing another.
- The display transfer can downscale 2x2 for free (box filter), which the
  existing S3D_AA mode already uses.

## Options

### 0. Verify the pending build (step 0, done in code)
White fix (sphere map sampled again, bump mask dropped) and half noise tiling
(S3D_FUR_TILE=50). Needs a rebuild and a Bomb Spore dialog capture.
- Difficulty: none left.
- Pros: may already fix most of the "white" and part of the "aliased" look.
- Cons: fur strands get twice as coarse; the bump mask that limits where fur
  grows is gone.

### A. Supersample dialogs only (auto S3D_AA during dialog cameras)
The 2x2 supersampled mode already exists (gx_init.c `TOP_AA`: drawn at
480x800, halved by the display transfer). Switch to it while a dialog camera
is active (mapEventSlot 0x3f, the same test objRenderFuzz uses), and back to
mono afterwards.
- Difficulty: medium. set_top_mode already switches modes per frame. The work
  is the trigger, keeping the 480x800 target allocated (or allocating it on
  dialog start, which risks VRAM fragmentation), and checking that EFB copies
  and the blur map correctly at 480 wide during the switch.
- Pros: this is the "resolution" fix. Fur, Fox's silhouette, grass and text
  edges all get 4 samples per pixel, closest to the GameCube look. No art
  changes.
- Cons: 4x fill in dialogs, so expect the dialog frame rate to drop by about
  half on hardware (Azahar won't show this). The big target permanently takes
  about 3 MB of VRAM, the same as stereo plus wide mode. Not available with
  the 3D slider up: stereo already uses the big target at 240x800, so stereo
  dialogs keep the plain look. An Old 3DS may not have the VRAM headroom.

### B. Mip levels for the fur noise
The game regenerates the 64x64 IA8 noise frames at runtime (newshadows.c).
After each update, build a mip chain (32, 16, 8, 4 by box filter, about 1.3 K
texels in total on the CPU) and sample with linear mip filtering. Distant or
small shells then average into soft coverage instead of speckle.
- Difficulty: low to medium. gx_tex.c already uploads mipmapped textures;
  this needs a hook after the noise fill and a "dirty" re-upload.
- Pros: cheap at runtime, works in stereo and in mono, and makes
  S3D_FUR_TILE=100 (the GameCube's tiling) usable again.
- Cons: the fur gets softer and greyer the smaller it is on screen, a look
  the GameCube doesn't have. It doesn't fix silhouette aliasing.

### C. Full shell count in dialogs
Raise the dialog budget from 8 to 15 shells (port_fur_step, cnt <= 4 case)
in mono only.
- Difficulty: trivial (one constant). It can be tried today with
  S3D_FUR_BUDGET=0 in env.txt.
- Pros: denser, smoother fur volume with less visible layering.
- Cons: about double the fur cost in dialogs. It does little for the speckle
  by itself.

### D. Stronger, correct background DOF
Fix the copy6 depth bands, then blend a coarser blur (mip 2/3 or the
display-transfer pyramid, GRAPHICS_PLAN.md step 6) by depth, so the
background behind Fox goes as soft as on the GameCube.
- Difficulty: medium to high. It needs depth copy format work in gx_copy.c
  and a two-stage depth-weighted blend.
- Pros: this is the "lack of blur" half. A soft background makes the sharp
  foreground read as intentional, as in the reference shot.
- Cons: it doesn't touch the fur itself. Depth copy precision on the PICA
  (D24 read back as a colour texture) is fiddly, and there is an extra pass
  per dialog frame.

### E. Full-frame soften (cheap fake)
Run the screen through a light 2x2 blur at frame end in dialogs (sample the
copy at a half-texel offset with bilinear filtering).
- Difficulty: low.
- Pros: almost free, and it hides all speckle.
- Cons: everything goes blurry, including text and Fox's face. Rejected
  unless all else fails.

## Recommended order

1. Step 0: rebuild and capture. Judge the white fix and S3D_FUR_TILE.
2. C: try S3D_FUR_BUDGET=0 in the same session; it costs nothing to test.
3. A: dialog supersampling, the biggest visual win for the "resolution"
   problem. Make it a touch-UI option ("Dialog AA", default on for New 3DS).
4. B: noise mip levels if speckle remains, or for stereo dialogs where A
   can't run.
5. D: later, together with the copy6 band bug (OPEN_BUGS.md 11).

## Status

| Step | State |
|------|-------|
| 0 | Verified 2026-10-11 (capture f1806): white fixed (brown, sphere-map lit). High-contrast speckle remains in the dialog. Title fur good and animated, slightly aliased |
| C | Not tried |
| A | Not started |
| B | Built 2026-10-11 01:28, accepted by the user (bug closed). gx/gx_tex.c `port_tex_want_mips` registers a texture's data pointer; its upload (convert_into) then gets a box-filtered chain down to 8x8 (64, 32, 16, 8), bound with linear mip filtering when S3D_TEX_MIPS is on. newshadows.c.sed registers each noise frame after its DCFlushRange. A/B switch: S3D_FUR_MIPS=0. Log counter `gx.tex_mipped` |
| D | Not started. Evidence: the f1806 dialog shows a horizontal seam at about y=118, likely the copy6 band |

Note: the reference screenshot is an upscaled Dolphin render, softer than a
real GameCube at 640x480, so it overstates the target softness a little.

## History (moved from OPEN_BUGS.md row 2, closed 2026-10-11)

Closed by the user after the mip chain build (01:28). Original symptom: fur shells missing in normal gameplay (the "pale head"); captures `20261010_233527_f2362`, `20261010_233300_f9558`.

intensity), which are big-endian [A, I] on GameCube. On the 3DS the bytes were swapped, so the coverage pattern showed as colour speckle and the colour shift became alpha. patches/main/newshadows.c.sed byte-swaps these stores and the other IA8 ones in that file (reflection gradient, caustic, distortion). The indirect bump warp on stage 3 is still missing. Black shells (captures `20261010_*_f1588`, `20261010_235046_f4061` state #836): the shell has 4 textures, so the sphere map (map1, stage 1) is dropped and the cadd rule keeps REG1 = RASC. In gameplay the channel is unlit with a black material (`en 0/0 mat 000000FF`), so REG1 = 0 and PREV*REG1 is black. The title screen has a lit channel, which is why bug 9 looked right there. port_fur_sphere (port/fur_3ds.c, patched into objFuzzRenderCb) reads the sphere map as ONE, so REG1 = RASC + C2 and the draw has 3 textures. S3D_FUR_SPHERE=0 restores TEXC, 2 reads it as HALF. Gameplay captures have no `post 37.5` noise states, and the log has no `fur.shells` or `fur.objects_skipped`, so the port budget never ran. Cause (game math, objRenderFuzz): only riding objects, dialog cameras (mapEventSlot 0x3f) and the death models get the "strong" falloff. Otherwise cnt = 10.5*dist/(hitboxScale*rootMotionScale), and at gameplay camera distance 16 - cnt <= 0. port_fur_gameplay (port/fur_3ds.c) recomputes cnt the strong way for the player (classId 1), keeping at most 3 shells (now opt-in with S3D_FUR_GAMEPLAY=1, see the top of this row). The log line `fur player strong .. dist .. scale .. n a -> b` (once a second) shows the inputs.

## Fast shells (S3D_FUR_FAST, 2026-10-17)

Goal: make each fur shell cheaper on the game thread and the render thread,
without changing the look. This is a native port path compiled into the
game's fur loop (`patches/main/objprint_dolphin.c.sed`, `port/fur_3ds.c`).

### Where the cost was

- `objRenderFuzz` draws every shell as a full `modelDoRenderInstrs` pass over
  Fox's whole model instruction stream. Only render ops with the fur flag
  (`rop->flags & 0x200`) draw. `objFuzzRenderCb` clears `gObjFuzzPassActive`
  for the others, and their display lists are skipped.
- The skipped ops still did all their setup, every shell:
  - their full material setup (Rcp TEV stages, blend, z, alpha compare, cull);
  - their vertex format (`modelRenderFn_setVtxDescr`);
  - their joint matrix loads (`renderOpMatrix`: PosMtx, NrmMtx and TexMtx).
- Each shell also set the projection, lights, fog and the five vertex arrays
  again, with the same values.
- Measured before (77981237 sampler, hub, Fur "Always"): the fur scope cost
  the game thread 3.7 ms per frame, 1.9 ms of it recording GX commands.
  In the attract scene the fur shells load ~2.9k PosMtx, TexMtx and NrmMtx
  each per frame (MULTICORE_PLAN.md, per-opcode results).

### What the fast path does

The fast path only runs inside `objRenderFuzz`'s shell loop (passMask 4).
`modelDoRenderInstrs` becomes a small wrapper that sets `fhCur` for that pass
only, so nested or other passes keep the game's path. `S3D_FUR_FAST` is a bit
mask (default 15, read live):

| bit | what | how it stays exact |
|---|---|---|
| 1 | Matrix loads of skipped ops wait in a table, one entry per matrix slot (`port_fur_mtx`). They are sent before the next fur draw. A later load to the same slot replaces the waiting one. | A loaded slot can only be seen by a draw, so the GPU sees the same matrices. Loads that are sent directly (fur ops, the vertex-anim matrices at the top of each shell) cancel a waiting load for their slot. |
| 2 | A skipped op's material setup is kept as "pending" (its bitstream position) and runs only when a fur op follows it, or at the loop end. A newer skipped op replaces the pending one. | The fur op inherits what the last material before it set, as before. Matrix loads carry a generation, so loads queued before the pending material are sent before it runs, and its own loads after it. |
| 4 | A skipped op's vertex format runs with its GX calls muted (the out flags are still computed). It is replayed before a fur draw if no newer format was set. | `setVtxDescr` clears and sets the whole format, so the last one wins. |
| 8 | Projection, lights, fog and vertex arrays are set only in the first shell of an object. | Nothing in the fur pass changes them between shells. `objFuzzRenderCb` doesn't touch lights or channels, and material setups don't touch channels. |

- At the loop end, the pending material, vertex format and matrices are
  applied. The GX state after the fur therefore matches the game's.
- Bit 1 without bit 2 still works, but every material setup then flushes the
  table, so it saves little.
- `S3D_FUR_FAST=0` is the game's path.
- `objRenderFuzzShells` and `objRenderFuzzShadowShells` (passMask 8 and 2)
  are not changed. In the decomp only `playerRenderFuzz(obj, 1, 1)` is called,
  which goes through `objRenderFuzz`.

**Counters** (per frame, in the profiler report):
- `fur.fast_mtx_deferred` / `fur.fast_mtx_flushed`: matrix loads held back
  and loads actually sent.
- `fur.fast_mat_skipped` / `fur.fast_mat_run`: material setups skipped and
  material setups run late.
- `fur.fast_desc_skipped` / `fur.fast_desc_run`: the same for vertex formats.

### Azahar check (build 6B0EAF66, 2026-10-17)

Hub savestate with Fur "Always" (`tmp_scripts/furfast.txt`, `furfast2.txt`;
shots `build/emu/furfast_*.png`, `ff2_*.png`):

| scene | shells/frame | matrix loads held / sent | materials skipped / run | formats skipped / run |
|---|---|---|---|---|
| hub, Fox (3 shells) | 3 | 852 / 66 | 12 / 4 | 12 / ~0-1 |
| title (Fox close-up) | 10 | 3144 / 192 | 40 / 11 | 40 / ~1 |

- So ~790 fewer matrix commands, 8 fewer material setups and 12 fewer vertex
  formats per frame in the hub. On the title screen it is ~2,950 fewer
  matrix commands per frame.
- Fur looks the same with 15, 0 and 7 (shots taken ~6 frames apart, crops
  in `ff2_cmp.png`). No errors in the log.
- Emulator timing says nothing about the gain. It must be measured on
  hardware.

### Hardware test (build 7C7F1EFF, .3dsx only)

1. env.txt: `S3D_FUR_FAST_AB=300`, `S3D_BENCH=0`, Fur set to "Always"
   (touch menu, or `S3D_FUR_MODE=2`).
2. In the hub, stand at the usual spot and don't touch anything for ~2 min.
   The game cycles 15 / 0 / 7 by itself (tag `furfastab`).
3. Compare frame time and GPU per phase. Also check the fur on Fox's head
   and tail, and in a dialog close-up (the title screen draws 10 shells).

What to expect:
- In the hub, Fur "Always" cost ~0.7 ms of frame (21DE01AE), so the frame
  gain there is at most about that. The game thread can gain up to ~1-2 ms
  (fewer recorded commands).
- The attract and title scenes and dialogs draw 8-10 shells. That is where
  the gain should show most.
- If 7 is as fast as 15, the lights-once bit is not worth its risk and can
  be dropped.

### Possible next steps

- **Fur-op TEV state once per shell.** `objFuzzRenderCb` sets ~60 TEV/texture
  commands per fur op per shell. Only the layer's noise texture, wind matrix,
  shade konst and indirect matrix change between shells. This needs care
  when a model has several fur ops with different textures. Built as
  `S3D_FUR_FAST` bit 16, but Fox has only one fur op per shell, so it never
  fires. See "Shading once per layer" below.
- **Joint scaling.** Each shell concatenates a scale into every joint matrix
  and rebuilds all pos/nrm matrices (`modelBuildPosNrmMtxs`). Only the joints
  that fur ops use would be needed. This is CPU game code, not GX recording.
- **Shells in the vertex shader** (DRAWCALL_PLAN.md "Fur pseudo-instancing"):
  one draw per mesh instead of shells x mesh. Much bigger job.

### Hardware run 1 (7C7F1EFF, 2026-10-17, title screen)

Log `perf/hwlogs/7C7F1EFF_title_fast15.log`. The user stayed on the title
screen and saw nothing wrong with the fur. **The A/B never switched**: it
waits for map 7 (the hub), and the title is map 63. So the whole run is
`S3D_FUR_FAST=15`.
- Title, steady from f265 to the end: 7 shells, 21.3-22.6 fps (frame
  ~46 ms), GPU p3d 18.9 ms, `game.scene_own_us` ~12.4 ms, `rt.replay` ~26 ms.
- Per frame: 2,128 matrix loads held, 138 sent; 28 materials skipped, 8 run;
  28 vertex formats skipped.
- Older title windows (E9567DFF, BE7BDA48) can't serve as a baseline: they
  ran with the sampler on (`S3D_PROBE=2`) and the camera was at 10 shells,
  not 7.
- Re-run: env.txt `S3D_FUR_FAST_AB=300`, `S3D_AB_MAP=63`, `S3D_BENCH=0`,
  and stay on the title ~2 min. No rebuild is needed.

### Hardware A/B (7C7F1EFF, 2026-10-17, title screen)

Log `perf/hwlogs/7C7F1EFF_furfastab_title.log`. Env `S3D_FUR_FAST_AB=300`,
`S3D_AB_MAP=63`, `S3D_BENCH=0`; title at 7 shells. The first phase (f232-532)
is dropped: the title camera was still moving (GPU 16.1 ms there against
~18.9 later). The user saw nothing wrong with the fur.

| S3D_FUR_FAST | frame | GPU | sceneRender (own) | rt.replay |
|---|---|---|---|---|
| 0 | 47.61 / 45.43 | 19.19 / 19.23 | 21.28 / 21.30 (12.55 / 12.66) | ~27.4 |
| 7 | 46.49 / 43.84 | 18.88 / 18.88 | 20.28 / 20.32 (12.73 / 12.77) | ~26.3 |
| 15 | 46.44 / 43.86 | 18.89 / 18.85 | 20.20 / 20.17 (12.54 / 12.48) | ~26.3 |

- **Game thread `sceneRender` -1.1 ms**: GX recording; the own time is the
  same. **Render thread replay -1.1 ms. GPU -0.3 ms.** Both runs of each
  value agree within 0.05 ms.
- The frame time falls ~4 ms over the run in every phase (47.6 → 43.8), so
  any frame gain is below that noise. sceneRender (20 ms), replay (26 ms) and
  GPU (19 ms) are all well under the ~45 ms frame, so the title's frame is
  limited by something else.
- Bit 8 (lights/arrays once per object) is worth ~0.1 ms (15 vs 7).
- Decision (user, 2026-10-17): keep the default `S3D_FUR_FAST=15`, bit 8
  included. No visual issues were seen with it.

## Shading once per layer (S3D_FUR_FAST bit 16, 2026-10-17)

Goal (user, 2026-10-17): set the fur's own shading state once per layer
(shell) instead of once per fur part (fur render op), the first "next step"
of "Fast shells".

### What was built

`patches/main/objprint_dolphin.c.sed` (last section) and `port/fur_3ds.c`:

- `fhFurLite`, called at the top of `objFuzzRenderCb`'s fur branch. Inside
  `objRenderFuzz`'s loop it records a key for each fur op's full setup:
  whether the op has an indirect texture, whether the bump-mask branch is
  taken (`port_fur_bump`), and the model's fog flag (`flags & 0x100`). These
  pick the setup's branches. It also records the three per-op textures: the
  base (layer 0, map 0), the sphere map (`texture0`, map 1) and the indirect
  texture (map 2).
- Everything else the callback sets is the same for all fur ops of one
  shell: the noise frame, shade konst, wind matrix, indirect matrices,
  caustic, projected light, sphere-map intensity, render alpha and fog. With
  bit 16, the next fur op with the same key therefore only binds the textures
  that differ (`selectTexture(NULL)` binds nothing, as in the game) and
  returns 1.
- `fhFurValid` says that the GX state still holds the last full fur setup.
  It is cleared at each `modelDoRenderInstrs` entry and exit (each shell), by
  the callback's non-fur branch (a non-fur op's full material follows), by
  `fhMaterialize` (a deferred material run late), and at the loop end. Only
  matrix loads and vertex formats can run while it is set, and the callback
  doesn't set either of them.
- Counters: `fur.fast_cb_full` / `fur.fast_cb_lite`. They count fur ops in the
  fur loop with any `S3D_FUR_FAST` value other than 0, so `cb_full` is also
  the fur op count.
- `static int fhCur` moved up to this block, because the callback is defined
  before the "Fast shells" block.

### Azahar result (build AF53D8C4, `tmp_scripts/furlite.txt`)

| scene | shells/frame | fur.fast_cb_full | fur.fast_cb_lite |
|---|---|---|---|
| hub, Fur "Always" | 3 | 3 | 0 |
| title (boot, before the savestate) | 10 / 8 | 10 / 8 | 0 |

**Fox has exactly one fur op per shell.** The premise ("once per fur part")
doesn't hold for him, so bit 16 never fires and saves nothing. It can't have
changed the picture either (shots `build/emu/fl_31.png`, `fl_15.png`,
`fl_0.png`). If it had found two fur ops in one shell separated by a non-fur
op, `cb_full` would be 2x the shell count; it is 1x.

Decision: bit 16 stays in the code but is **opt-in** (`S3D_FUR_FAST=31`). The
default stays 15, because the path has never run on a model where it fires.
The `furfastab` A/B is unchanged (15 / 0 / 7). Final build **EA390CA6**
(.3dsx only), the same as AF53D8C4 except for those defaults. There is no
hardware test to do for this: on Fox, 31 and 15 send the same commands.

### Across shells: why it wasn't done

The real repetition is across shells: the same single fur op runs the full
setup 3 (hub) to 10 (title) times per frame. Between two shells the game also
runs one deferred non-fur material (`fur.fast_mat_run` = shells + 1). Call it
M: the last non-fur op before the fur op, the same op each shell. M rewrites
TEV state, so the fur callback must run in full after it.

Skipping both M and the full fur setup on shells 1+ (re-sending only the
noise texture, shade konst, wind `PTTEXMTX0` and the indirect matrices) is
exact for the TEV/pixel state, because M and the callback write the same
values each shell. It is **not** provably exact for matrices: Rcp's material
stages load `PNMTX0` (pos/nrm, rcp_dolphin.c) and `TEXMTX0-2`
(shader_dolphin.c) directly. The fur draw reads `PNMTX0` (projected light
texgen, skinning) and `TEXMTX0` (sphere map). Without M, a deferred load from
another skipped op could be what the fur draw sees instead of M's value.
Alpha compare is also inherited from M (the callback doesn't set it).

Expected gain: about 60 (fur) + ~40 (M) commands per extra shell. That is
~200 commands per frame in the hub (3 shells) and ~600 on the title. For
scale, "Fast shells" removed ~3,000 commands on the title for -1.1 ms
sceneRender and -1.1 ms replay. So this would be ~0.1 ms per thread in the
hub and ~0.2-0.3 ms on the title. To do it exactly, M's matrix loads would
have to be replayed: run M once per loop with its GX writes recorded (or
muted except for matrix loads) and re-send only those loads per shell. That
needs a mute switch in the gx layer for calls made from rcp/shader code.
Not worth it for the gain unless dialogs (8 shells) turn out to be fur-bound.

## Joint texture matrices (bug 27, 2026-10-18)

Until build 730F2888, renderOpMatrix's per-joint `GXLoadTexMtxImm` went to
IDs 0xFF/0xFD (read past `gObjGxPosMtxIdTable`, OPEN_BUGS.md 27) and gx
dropped them. The fur shells' sphere-map lighting (map1, `GX_TG_NRM` x
TEXMTX0) therefore used a stale matrix: fur and face went black at night.
With the fix the shells and Fox's body read real joint matrices.

Effect on "Fast shells": the texture matrix loads now reach port_fur_mtx
with IDs 30-57 (slots 10-19), so they are deferred like the position and
normal loads. `fur.fast_mtx_deferred` / `fur.fast_mtx_flushed` grow by about
half compared with the numbers above (measured while those loads were sent
straight through to be dropped). `S3D_JOINT_TEXMTX=0` restores the old drop.
