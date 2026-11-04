# Stereo 3D: the close-up blow-up and how stereo is calibrated

Session notes for the 3D "object too close to the camera" work (2026-11-03).

## The problem

With the 3D slider up, anything close to the camera (the title-screen menu,
close-ups, objects passing near the camera) looked wrong: extreme crossed
parallax, double vision and eye strain. The user's title-screen capture
(`captures/20261103_204224_f1683`, map 63, stereo left eye) shows the scene:
~90% of the drawn pixels sit at the far background depth (buffer value
169600) while the foreground geometry sits much nearer (4.7M-8.6M), so the
screen mixes a distant background with close geometry - exactly the case that
breaks naive stereo.

## Why it happens (the math)

The port's stereo model (GRAPHICS_PLAN.md step 11) is the standard one used by
citro3d's `Mtx_PerspStereoTilt`:

```
x_clip += eye * sep * (w - conv)
```

`w = -z_view` is the clip w, `sep` the parallax at infinity per eye (in clip-x
units, 0.025 at the full slider by default), `conv` the zero-parallax distance
(80 world units by default, eased toward the camera-to-Fox distance by
`S3D_STEREO_AUTOCONV`, clamped 20-400).

In screen pixels the parallax of a point at distance z is

```
P(z) = 2 * sep * (1 - conv/z) * hw        (hw = half the screen width, 200)
```

- Background, z -> infinity: P -> 2 * sep * hw = **10 px** at the full slider
  (2.5% of the 400-px screen, a deliberately conservative "divergence budget").
- z = conv: 0 (the screen plane).
- z < conv: P goes negative (crossed, pops out) and **grows like 1/z** with
  no bound: at z = 4 with conv = 80 it is `(1 - 80/4) * 10 = -190 px`.

That unbounded foreground is the bug. Even the minimum auto-convergence (20)
leaves 40 px of crossed parallax at z = 4; menus routinely get closer than
that. The near plane does not help: the game's near is a fraction of a unit,
and the dangerous zone spans tens of units.

## How stereo is usually calibrated

The knobs and the industry-standard budget they are set with:

1. **Eye separation / interaxial ("strength").** Sets the parallax at
   infinity: `2 * sep * hw` pixels. On an autostereoscopic 3DS display this
   must stay small (the parallax-barrier sweet spot is tiny): ~2-3% of the
   screen width is comfortable. Default 25e-3 clip units = 10 px = 2.5%.
   The 3D slider scales it globally and stays the player's comfort valve.
2. **Convergence ("screen depth").** The distance whose pixels have zero
   parallax. Convention: track the subject, i.e. what the player looks at.
   The port eases it toward the camera-to-Fox distance (`S3D_STEREO_AUTOCONV`).
3. **Parallax budget / close-up clamp.** The crossed (foreground) parallax
   is limited to a small multiple of the divergence parallax - the piece this
   port was missing. Clamping *screen-space* disparity instead of letting it
   grow with 1/z is exactly what this change adds.
4. **Near-plane push.** Many stereo titles also push the near clip plane out
   in 3D to remove extreme close-up geometry. Rejected here: with a
   comfortable clamp the surviving near geometry is fine, and pushing the
   near plane to the clamp distance (conv/(1+C) = 40 units for C = 1) would
   clip away legitimate close-ups (menus, faces, doors).

## The fix: per-vertex close-up clamp

The offset becomes

```
x_clip += eye * sep * max(w - conv, -C * w)
```

- w >= conv/(1+C): unchanged (`sep * (w - conv)`).
- w < conv/(1+C): `-eye * sep * C * w`, so the *screen-space* parallax is
  pinned at `C` times the parallax at infinity: at C = 1 the foreground can
  pop out exactly as far as the background recedes (10 px at full slider).

`C` comes from `S3D_STEREO_CLAMP`:

| Level | C   | Meaning |
|-------|-----|---------|
| 0     | 0   | Flat: nothing pops out, close objects sit on the screen |
| 1     | 0.5 | Gentle |
| 2     | 1.0 | Safe (default): foreground <= background depth |
| 3     | 1.5 | Extra pop-out |
| 4     | 1e6 | No limit (the old behaviour, kept for A/B) |

`max()` is nonlinear in w, so the offset moved from the projection matrix
into the vertex shader. `upload_projection` (gx_render.c) now uploads the
mono projection for both eyes plus a per-eye `stereo` uniform
`(eye*sep, -conv, -C, 0)`; `draw_eyes` refreshes it per eye per draw; the
raw EFB-clear path zeroes it. Orthographic HUD passes still use the matrix
path (constant w, `S3D_STEREO_HUD_X1000` unchanged).

The water variant runs the identical position code so pass 2 stays at depth
EQUAL with pass 1.

## Shader resource cost (why the extra work)

The four GX vertex shader variants (lit / unlit / warp / water) share the
512-word shader code memory, which was at 510/512 words, and the lit variant
uses all 96 float uniform registers (92 uniforms + 4 constants). To add the
5-word clamp block (4 dp4 + add/mul/max/mad + mov) to every variant:

- Freed **17 words** in the lit variant: the light2-light7 helper procedures
  were flattened into the 2-instruction `mova a0.y + call onelight` pattern
  that lights 0-1 already used inline.
- Freed **1 word** in warp: a redundant `mov r10, zeros` (r10.xy is
  overwritten unconditionally by the wave block).
- Freed **1 word** each in unlit / warp / water: the two-instruction
  `mov outview, zeros; mov outview.z, r1.z` became one
  `mul outview, zunit, r1.zzzz` with a new `zunit(0,0,1,0)` constant.
- The new `stereo` uniform goes at register 48 in **all four** variants (the
  lit variant's light arrays shrink from 8 to 7 lights to make room).
  Expected total: ~503/512 words, 9 spare.

**Cost of the 7-light change:** if the game ever enables an 8th light, it is
dropped, counted (`gx.light8_dropped`) and logged once. If that log line ever
shows up, revisit (the alternative is packing the stereo values into the
unused `litsel.zw` for the lit variant instead, keeping 8 lights).

**Runtime cost:** +4 always-executed ALU ops per vertex (add/mul/max/mad) in
every draw, mono included. The GPU is vertex-bound, so measure with the usual
A/B runs; if it shows, the obvious follow-up is a conditional (cmp + ifc) so
mono frames skip the block - it costs 3 more words per variant and needs the
word budget rechecked.

## The settings (bottom screen, Settings tab)

| Row | Switch | Values | Default |
|-----|--------|--------|---------|
| 3D: strength | `S3D_STEREO_SEP_X1000` | 15 Soft, 20 Low, 25 Normal, 30 Strong, 40 Max (clip units x1000) | 25 |
| 3D: screen depth | `S3D_STEREO_CONV` | 40 Near, 60 Close, 80 Normal, 120 Far, 160 Distant (world units) | 80 |
| 3D: close-up limit | `S3D_STEREO_CLAMP` | Safe, Extra, Gentle, Flat, No limit | Safe (2) |

All three are live (apply within a frame, saved to settings.txt, overridable
from env.txt). Note: with "3D: depth follows Fox" on, the screen depth value
is only the starting point - the convergence eases back toward Fox in ~0.2 s.

Calibrating on the console: stand at a spot with close and far geometry (the
title screen, the hub), tap through the rows, and check:
- the background keeps a small, stable uncrossed parallax (strength),
- the focus subject sits at the screen (screen depth),
- objects approaching the camera never exceed the pop-out of the background
  (close-up limit; "Flat" proves the clamp is on: close objects sit on the
  screen).
For A/B numbers the existing runner takes env.txt switches of the form
`S3D_*_AB`; a stereo-clamp entry was deliberately not added (all levels cost
the same GPU time).

## Files touched

- `3DS/gx/default.v.pica`, `unlit.v.pica`, `warp.v.pica`, `water.v.pica`:
  position block + `stereo` uniform; lit: 7 light arrays, flattened light
  procedures; unlit: unused `texwarp` (register layout) + `zunit`; warp:
  removed redundant mov; water: `zunit`.
- `3DS/gx/gx_render.c`: `s_loc_stereo`, clamp state + live reload
  (`S3D_STEREO_CONV`, `S3D_STEREO_CLAMP`), per-eye `stereo` upload
  (`upload_projection`, `draw_eyes`), zeroing in `gx_render_clear_efb_rect`,
  7-light upload with the 8th-light detector in `upload_lighting`.
- `3DS/gx/gx_init.c`: `stereo` in the shared-register check; live
  `S3D_STEREO_SEP_X1000` via `CFG_LIVE`.
- `3DS/port/touchui_3ds.c`: the three Settings rows.
- `README.md` (Settings menu), `GRAPHICS_PLAN.md` (step 11).

## Open items

- Hardware verification of the new shader (position block in 4 variants,
  flattened light calls, 7 lights). Boot with `S3D_GPU_TRACE` on if anything
  hangs; watch the log for `gx.light8_dropped`.
- GPU/frame time against the previous build (the +4 vertex ALU ops).
- If the 8th light ever fires: move the stereo values into `litsel.zw` for
  the lit variant and restore `lightpos[8]`.
