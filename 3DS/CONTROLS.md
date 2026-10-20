# 3DS controls

How the 3DS-only controls work: the C-stick camera, the touch screen item
menu and gyro aiming. The player-facing summary is in the README
("Controls"). Code: `port/controls_3ds.c`, `port/pad_3ds.c`,
`port/touchui_3ds.c`, `patches/dlls/engine/66/66.c.sed`,
`patches/dlls/objects/195_Player/player.c.sed`.

## Where the C-stick goes

On the GameCube the C-stick opens the item menu (the ring with the items,
staff powers and Tricky pages, `dlls/engine/0/0.c` `GameUI_frameEnd` and
`cMenuRun`). No gameplay camera reads it. On the 3DS the item menu moved to
the touch screen and the C-stick moves the camera.

`PADRead` (`pad_3ds.c`) only gives the game the physical C-stick when
`port_cstick_to_game()` says so:

- there is no player object (title, file select, memory card menus,
  progressive scan prompt), or
- the pause menu is open (`pauseMenuState`, it scrolls the map with the
  C-stick), or
- the camera mode (`gCamcontrolActiveActionId`) is first person (0x44, the
  C-stick zooms), the debug camera (0x46) or the world map (0x4E).

Everywhere else the game sees a centred C-stick, and the item pad's pulses
(below) instead. `S3D_CSTICK_CAMERA=0` gives the game the C-stick everywhere
again, which is the original behaviour. The remote `cstick X Y N` command
counts as the physical C-stick.

## Camera orbit (normal camera, DLL 66 / mode 0x42)

The normal follow camera is a leash: `CameraModeNormal_follow` only moves the
camera when its distance to Fox leaves [minDistance, maxDistance], and the
yaw is derived each frame from where the camera stands
(`yaw = 0x8000 - getAngle(dx, dz)`). So the patch swings the camera's
position around Fox each frame, after `CameraModeNormal_updateWallAvoidance`
and before `follow`. It does this the same way the game's own wall avoidance
rotates it. Nothing pulls the swing back, so there is no stored offset.

- `port_camera_orbit_yaw()` returns this frame's turn: C-stick X with a 15%
  dead zone, `S3D_CAM_SPEED` degrees per second at full tilt (default 150),
  scaled by `timeDelta` (60 Hz frames). Right turns the view right.
- Up/down moves `port_camera_orbit_height()`, which is added to the camera's
  height band (`lowerY`/`upperY` in `CameraModeNormal_updateSlide`). The
  camera eases into the band at its own rate. The range is -25..+45 world
  units at 60 units per second. Up raises the camera.
- L resets the height. L already swings the camera behind Fox (mode 0x43,
  DLL 67).
- `S3D_CAM_INVERT_X` / `S3D_CAM_INVERT_Y` flip the axes.

Other camera modes (combat 0x49, climbing, cutscenes and so on) are left
alone. Wall avoidance can push back near walls.

Emulator check (build F50D560A, ALX save in Krazoa Palace): a full-right
C-stick for 45 frames turned the view about 100 degrees to the right and it
stayed there. Full up for 30 frames raised the camera; the old +70 cap was
almost top-down, so it is now +45.

## The C-menu ring on the bottom screen (Map tab)

The game's C-menu ring (red items, yellow staff powers, blue Tricky) is drawn
on the bottom screen, right of the minimap, every frame, and no longer on the
top screen. The ring's area (x 214-319, below the tabs) is also its touch pad.

**Drawing.** `hudDrawButtons` (DLL 0) draws the ring in one block that only
runs while the menu fades in (`cMenuFadeCounter != 0`).
`patches/dlls/engine/0/0.c.sed` wraps that block in
`port_cmenu_hud_begin()` / `port_cmenu_hud_end()` (`touchui_3ds.c`):

- It opens a bottom screen HUD pass (`gx_hud_begin`, like the minimap).
- It remaps it with `gx_hud_set_map()` so the ring's GameCube-screen region
  (`RING_SRC_*`, about x 535-613, y 105-271) lands right of the minimap at
  `RING_SCALE` 1.1. `gx_hud_set_map` is a recorded command (`gx/gxrec.list`),
  so the render thread replays it inside the same pass. The ring's 3D model
  goes through the same viewport and scissor mapping as everything else.
- It forces `cMenuFadeCounter` to 255 for the draw only. So the ring shows
  while the menu is closed, and B closes the menu but the ring stays. The
  menu's open state, which decides whether A uses an item or reaches Fox, is
  not touched. The ring rotation and the item list are updated every frame
  by `cMenuRun` either way.
- The item counts use text box 0x93, which the top screen HUD shares, so in
  this mode they go to `port_cmenu_count()` instead. The touch UI draws them
  with citro2d at the mapped position. They are double-buffered: written
  during the frame, published in `touchui_frame`.

`S3D_CMENU_BOTTOM=0` puts the ring back on the top screen (also used when
the bottom screen HUD is off).

**Gestures.** They become short virtual C-stick and A pulses for the game's
own menu code. The pulses are merged in `PADRead` through
`touchui_pad_state()`; each lasts `PULSE_FRAMES` (2) game frames, followed
by one centred frame. The game's thresholds:

| Gesture | Pulse | Game code |
| --- | --- | --- |
| Tap | C-stick X = 8 | Any tilt over 5 opens the closed menu; the page comes from the context as before (Tricky nearby, targets, last page) |
| Flick left/right (18 px) | C-stick X = ±0x30 | A rising edge past 15 turns to the next colour (`GameUI_frameEnd`) |
| Flick up/down | C-stick Y = ±0x30 | A rising edge past 10 scrolls one item; under 60, so no auto-repeat (`cMenuRun`). Down = next |
| Two taps within 400 ms, 40 px apart | A | Uses the item (`cMenuRun`, `b2 & 0x900`), only when the menu is fully open (`cMenuOpen` and `gCMenuOpenAnim == gCMenuOpenAnimMax`), so A never reaches Fox |

Because the ring is always visible, a flick or a double tap while the menu
is closed opens it first. The gesture is queued (`s_pending`) and runs once
the menu is fully open, or is dropped after 60 frames. Gestures while the
menu is closing are dropped. Flicks repeat while the finger keeps moving.
The gesture hints are two lines under the minimap.

Touch edges come from the held state (`s_touch_prev`), not `hidKeysDown`,
because `PADRead` also calls `hidScanInput`. A touch that starts between the
two scans would otherwise be missed.

Remote commands for tests: `tap X Y` (taps inside the ring area count as
pad taps; put `wait 3` between two taps for a double tap, because the
remote keeps one pending tap per frame) and `flick DX DY`.

Emulator checks (ALX save, Krazoa Palace):
- F50D560A/3EFEAC65 (old pad panel): tap, flicks and double tap into
  Fire Blaster aim worked.
- Ring build: the ring is drawn closed next to the map and gone from the top
  screen; a tap opened it; a right flick turned red to yellow; after B it
  stayed visible; a left flick while closed opened yellow and turned it to
  red; a double tap from closed opened and used the item. At scale 1.15 the
  ring touched the bottom edge, hence 1.1 (final build 8BA78CB5, not
  re-shot).

## Aiming: no spring-back, gyro

The staff blasters aim in `playerStateAimStaff` (player state 0x2c, camera
mode 0x52 "force behind", DLL 82). The game eased `aimInputX/Z` toward the
main stick's absolute position:

```c
inner->aimInputX += interpolate(stick - inner->aimInputX, 0.1f, timeDelta);
```

So releasing the stick brought the aim back to the centre. The crosshair
(`aimScreenX/Y`) and the camera (DLL 82 aims at "behind Fox + half the aim
angle" via `playerGetAimAngles`) followed it. That is the rubber banding.

With `S3D_AIM_HOLD` (default on), `port_aim_axis()` changes this:

- The stick is a speed: full tilt crosses from the centre to the edge in
  about 0.5 s. The value stays where it is left.
- Gyro turns are added (`S3D_GYRO_AIM`). The default sensitivity
  (`S3D_GYRO_SENS=100`) maps 25 degrees of console turn from the centre to
  the edge.
- The value is clamped to ±1. Sideways overflow turns Fox
  (`port_aim_take_turn()`, 4096 yaw units per aim unit, about 22 degrees),
  so turning the console keeps turning Fox past the edge.
- The game's own edge spin (aimInputX past ±0.75 keeps turning Fox every
  frame) is off in this mode, because with a held aim it would never stop.

`aimInputZ` is the stick's Y: stick up is positive and aims down (the
crosshair moves down the screen), so gyro "tilt up" is subtracted.
`S3D_AIM_HOLD=0` restores the game's code exactly. The aim is still reset
to the centre when aiming starts (player.c `playerStateStartAimStaff`).

### Gyro

`port_controls_frame()` runs once per game frame (`VIWaitForRetrace`, after
`hidScanInput`). It enables the gyroscope and the accelerometer on the first
call. The raw-to-degrees coefficient comes from
`HIDUSER_GetGyroscopeRawToDpsCoefficient` (14.375 in Azahar). Then:

- **yaw** = the angular rate projected on the accelerometer's (normalised)
  up vector. This is the turn around the world's vertical, however far the
  console is tilted back. A positive turn around up is to the left, so it is
  negated (right = +).
- **pitch** = the rate around the console's x axis (left-right), which stays
  horizontal while the console is held.

Both are multiplied by the frame time (capped at 0.1 s after loads) and used
by the aim on that frame only. The signs come from the axis conventions and
were not checked on hardware; `S3D_GYRO_INVERT_X` / `S3D_GYRO_INVERT_Y`
flip them. The remote `gyro YAW PITCH N` command injects a turn in degrees
per second (right/up positive) for N frames.

## Settings

The Settings tab has "Gyro aiming" (`S3D_GYRO_AIM`) and "C-stick moves
camera" (`S3D_CSTICK_CAMERA`).
