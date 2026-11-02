# Bug 38 (round 2): black hub water in stereo 3D — root cause and fix

Session 2026-11-02, build `00F86547` (new) vs `1CFF306B` (the run that showed the bug).
Fixes are in `gx/gx_copy.c` only. Nothing else changed.

## Symptom (user report)

- 3D effect ON: the water looks fine at first, then **after a while it turns black**.
- Toggling the 3D effect off/on fixes it (for a while).

## Evidence

Hardware captures (fetched from the console, `sdmc:/3ds/star3d/captures/`):

- `20261102_180227_f1334` — **black water** (left-middle of the frame is ~29% near-black;
  the same region in the good capture is 0%).
- `20261102_180234_f1485` — water fine (taken ~7 s later, after a 3D toggle).

The ID pass (`id.bmp` + `states.txt`) names the water state: **state #581 (bad) / #580 (good)**,
the game's caustic water TEV (4 GX stages, unit0 = tc3 reflection, unit1 = tc0 refraction,
both sampling the 320x240 RGB565 copy `gNewShadowReflectionTexture` @ `0x317cfb80`).

The two dumps of the same state differ in exactly one thing — the texture binding:

| | f1334 (black) | f1485 (fine) |
|---|---|---|
| unit0/unit1 bind | raw game buffer, **no COPY marker** | the copy texture, **COPY** |
| PICA texture | `512x256 fmt 3` (RGB565) @ pa `2342F580` | `256x512 fmt 0` (RGBA8) @ pa `23F31180` |
| texel dump | none (never-written memory) | `r 0..254 g 0..254 b 0..254` (real EFB pixels) |

So the water did not sample the managed EFB copy — it sampled the **raw, never-written
game buffer** (`gNewShadowReflectionTexture`'s own memory, all zeros = black). All other
copy textures in the bad capture still bind fine, so only this entry was lost.

## Root cause

`gx_copy.c` keeps EFB copies in a 64-slot table keyed by the game's destination address.
In stereo, every `GXCopyTex` used to also copy the right eye's half (`tex_r`) plus edge
fills and mips, all from one 24-entry command-queue budget (`QUEUE_BUDGET`, citro3d's
queue holds 32 and panics when full).

The hub frame runs 7 game copies: shadow textures, the distortion slots (heat haze),
the 80x60 small reflection, and last the two 640x480 screen copies (colour + Z8, the
ones water/heat-haze/blur sample). With both eyes copied the queue ran out before the
big pair, so:

1. **Every frame in stereo** the 640x480 colour copy was dropped with
   `GXCopyTex: too many copies in one frame (skipped)` — the hardware log shows
   `gx.copytex_over_budget` 1-2/frame, every frame, plus `copy edge fill skipped` at
   queue 22/24.
2. A skipped copy does not refresh `last_frame`, so after **300 frames**
   (`EXPIRE_FRAMES`) the entry was freed by `gx_copy_frame_begin`.
3. With the entry gone, `gx_copytex_find` returns NULL and `bind_textures` falls back
   to the decoded game texture — the raw buffer the port never writes → **black water**.
4. Toggling 3D off makes the queue fit (mono = one copy per call), the copy succeeds,
   the entry is recreated → fixed. Toggling back on re-enters the skip pattern, and
   ~300 frames later it goes black again. Exactly the reported behaviour.

This is the same `gx.copytex_*` budget path bug 8 already suspected for the ghost
offset; the captures finally prove the full chain for the water.

## The fix (two belts)

1. **Right-eye copies only for screen-spanning copies** (`W >= 640 && H >= 480`, i.e.
   the two reflection copies). They hold view-dependent content (the screen grab the
   water/heat haze/blur sample). The small procedural copies (shadow textures,
   distortion slots, frame textures) are view-independent: both eyes must sample the
   same texture, so their right-eye copies were pure queue cost. This frees ~5 queue
   entries per frame in the hub — the big copies now always fit.
   `S3D_COPY_RIGHT_FULL=0` restores the old every-copy behaviour (hardware A/B).
   New counter: `gx.copytex_right_2d` (right-eye copy skipped because the copy is
   view-independent).
2. **A skipped copy keeps its entry alive** (`last_frame` refreshed in the budget-skip
   path, with a rate-limited `copy skipped (queue ...) kept stale` log). If the queue
   ever runs out again, the draw keeps sampling the last copied contents instead of
   expiring into the raw black buffer.

## Verification so far

- **Azahar smoke test** (this machine, build `00F86547`, `S3D_STEREO_FORCE=1`,
  the recorded pad route): boots, attract/title render, no crash; `copytex_right_2d`
  3/frame, `copytex_right` 2/frame, **0** over-budget skips, 0 "kept stale" lines.
  The old build in the same conditions always skipped 1-2 copies per frame.
  (The save-game load then stalled on the loading screen in the PROFILE build; that is
  a separate, pre-existing-looking Azahar/PROFILE issue, not this fix — the fix cannot
  stall the game thread. Not yet investigated.)
- **Hardware**: pending. Test plan below.

## Hardware test plan (user)

1. Start the new `star3d.3dsx` from the Homebrew Launcher (pushed to `/3ds/star3d.3dsx`).
   Check the log line `build id 00F86547`.
2. Stand at the pond with the 3D slider up for several minutes (well past the old
   ~10-15 s blackout window; give it 5+ minutes).
3. Expected in `Star3D.log`:
   - `gx.copytex_right_2d` counting every frame, `gx.copytex_over_budget` 0,
     no `copy skipped (queue ...)` lines.
   - Water never goes black; toggling 3D is no longer needed.
4. If the water ever DOES go black again: take a `mark` capture and check whether the
   water state still has `COPY` bindings. If it lost them, the capture's `copyN.ppm`
   (enable `S3D_CAP_COPIES=1`) shows what the copies hold.

## Artifacts

- Code: `out/star3d-nodata.3dsx` (build id `00F86547`), `out/star3d.elf` (symbols).
  Built with the Docker image (`star3d-builder`), `PROFILE=1` like the user's testing
  build. Pushed to the console over FTP at `/3ds/star3d.3dsx` (upload verified by
  download-back).
- Change: `3DS/gx/gx_copy.c` only (uncommitted).
