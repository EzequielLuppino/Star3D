# Star 3D

An unofficial port of a 2002 GameCube adventure game to the New Nintendo 3DS.
It is built on [foxhollow](https://github.com/JackPriceBurns/foxhollow), a
native PC port of the game, which is in turn built on a full decompilation
([SFA-Decomp](https://github.com/zcanann/SFA-Decomp)). The game code is
compiled as-is for the 3DS. A 3DS platform layer, the **Stellaris engine**,
replaces the PC graphics, audio, input, disc and save code (foxhollow's
own backend is Aurora). Star 3D is a foxhollow port made by Ezequiel.

**No game data is included.** You need your own legally dumped disc image of
the game: the PAL release, game ID **GSAP01, revision 1**. The image is only
read at build time. Its files are extracted and converted, then embedded in
the output.

## Features

- Targets the New 3DS / New 2DS XL, with the 804 MHz clock and the L2 cache
  enabled. The original 3DS lacks the memory and speed.
- GameCube GX graphics rebuilt on the PICA200 GPU (citro3d): TEV combiners,
  fog, lighting, EFB copies, depth of field, fur and grass, water and heat
  warps. Display lists are cached and repacked.
- Stereo 3D that follows the 3D slider, with auto-convergence.
- 16:9 widescreen and a minimap HUD on the bottom screen.
- MusyX music and sound effects on the 3DS DSP. Samples are converted to
  DSP-ADPCM at build time.
- The attract movie plays with its audio.
- Saves go to the SD card, with GameCube memory-card semantics.
- Game assets (textures, audio, movie) are converted at build time into
  3DS-friendly formats.
- 3DS controls: the C-stick moves the camera, gyro aiming for the staff
  blasters, and the item menu on the touch screen (see [Controls](#controls)).
- Adaptive frame pacing, a crash handler, a watchdog and a detailed log.
- Runtime switches in `env.txt` turn features off without a rebuild.

## Getting the source

The game code and the decompilation are git submodules, so clone recursively:

```sh
git clone --recursive <this repository's URL>
```

If you already cloned without `--recursive`:

```sh
git submodule update --init --recursive
```

## Building

Copy your disc image (`.iso`, GSAP01 rev 1) into the repository root, next
to the `Dockerfile`. Any file name works, but keep only one `.iso` there.
Compressed images (RVZ, GCZ, WBFS, NKit...) must first be converted to a
plain ISO, for example with Dolphin (right-click the game > Convert File > ISO).

### Windows

- **Docker (recommended):** install
  [Docker Desktop](https://www.docker.com/products/docker-desktop/), then
  run `build.bat` (double-click it, or run it from a terminal). Nothing else
  is needed.
- **Native devkitPro:** install [devkitPro](https://devkitpro.org/wiki/Getting_Started)
  to `C:\devkitPro` with the 3DS packages (`3ds-dev`, `3ds-zlib`). Also install
  Python 3 with Pillow (`pip install pillow`). Then, from the devkitPro
  MSYS2 shell (or Git Bash):

  ```sh
  bash 3DS/run_test.sh
  ```

  To build the `.cia` natively, put `makerom` from
  [Project_CTR](https://github.com/3DSGuy/Project_CTR) on your `PATH`.

### Linux (and macOS)

- **Docker (recommended):** install Docker Engine (or Docker Desktop), then
  run `./build.sh`.
- **Native devkitPro:** install devkitPro pacman, then:

  ```sh
  sudo dkp-pacman -S 3ds-dev 3ds-zlib
  export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
  make -C 3DS -j$(nproc) PYTHON=python3
  ```

  This needs Python 3 with Pillow (`python3-pil` or `pip install pillow`).
  For the `.cia`, put `makerom` on your `PATH`.

The first build takes a few minutes, most of it extracting and converting the
game data. Later builds only recompile what changed. `make code` (or
`build.bat code` / `./build.sh code`) builds the code without game data,
so no ISO is needed. The Docker image is pinned by digest, so builds are
reproducible.

For a profiling build, add `PROFILE=1` (for example `./build.sh PROFILE=1`).
It writes a per-zone timing breakdown of the graphics code and selected game
functions to `sdmc:/3ds/star3d/Star3D.log`. The timers slow the game down, so use a normal
build for real frame rates. Switching `PROFILE` rebuilds the affected files
automatically.

**Polygon reduction (on by default).** While the game data is extracted, the
build simplifies every model and map block (`3DS/tools/mesh_reduce.py` with
the C simplifier `3DS/tools/native/meshsimp.c`, both run inside the Docker
image). Each display list is rewritten in place with fewer triangles, and the
results are cached like the converted textures. Options, passed like any other
make variable (`build.bat POLY_REDUCE=0`, `./build.sh POLY_REDUCE=30`, or
`docker run -e POLY_REDUCE=0 ...`):
- `POLY_REDUCE`: percent of triangles to remove (default 50; 0 = off, the
  game's full polygon models). In the hub on hardware the GPU needs about
  39 ms per frame at 50 against about 47 ms with full polygons.
- `POLY_MAX_ERROR`: largest allowed change of shape, as a fraction of the
  model's size (default 0.05). A list stops reducing there, so the real
  reduction is lower than the target: about 38% over the whole disc at the
  defaults. Edges that touch the next map block, texture seams and
  bone boundaries are protected to avoid cracks.

Changing either option re-extracts the game data on the next build. The log
shows the options of the game data in use (`game data: ...` near the top).
`build.bat mesh-check` / `./build.sh mesh-check` checks the reduced files
against the disc (only display lists changed, every asset still present).

### Output

- Docker: `out/star3d.3dsx`, `out/star3d.cia` and `out/star3d.elf` (symbols,
  for the crash addresses in the log).
- Native: `3DS/build/us_3ds/star3d.3dsx` (and `.cia` / `.elf`).

The `.3dsx` and `.cia` files are large (several hundred MB) because the game
data is embedded.

For quick test builds, skip the `.cia` (packing it takes a while):
`NO_CIA=1` on the make command line natively, or
`docker run --rm -e NO_CIA=1 -v "<repo>:/workspace" star3d-builder` with Docker.
The Docker build then removes any older `out/star3d.cia`, so only the fresh
`.3dsx` is left to copy.

The `.cia` has a 3D HOME Menu banner: the game's textured Arwing model in
front of a starfield and the title, built from your disc at build time
(`3DS/tools/make_banner.py`, `3DS/tools/banner_cgfx.py`). If it misbehaves,
`BANNER_3D=0` (Docker: `docker run -e BANNER_3D=0 ...`) gives a flat picture
banner instead.

## Installing

- **.3dsx:** copy `star3d.3dsx` to `/3ds/` on the SD card and start it from
  the Homebrew Launcher.
- **.cia:** install `star3d.cia` with [FBI](https://github.com/Steveice10/FBI)
  (or another title manager). It then appears on the HOME Menu.
- **Azahar emulator:** File > Load File, pick `star3d.3dsx`, and select the
  New 3DS model in the settings.

Sound needs the DSP firmware at `sdmc:/3ds/dspfirm.cdc`. Dump it from your
console with DSP1. Without it the game runs silently.

## Controls

| 3DS | Game |
| --- | --- |
| Circle Pad | Move |
| C-stick | Turn the camera around Fox (up/down raises or lowers it) |
| A, B, X, Y, L, R, START | The same GameCube buttons |
| ZR | Z (first person) |
| L | Also puts the camera back behind Fox |
| Gyro (turn/tilt the console) | Aim while the staff blaster is drawn |
| Touch screen, Map tab | The item ring (the old C-stick menu) |

The C-stick menu's ring (red items, yellow staff powers, blue Tricky
commands) is shown on the bottom screen next to the map all the time. Its
touch controls are:

- **Tap** the ring to open the menu.
- **Flick left/right** to turn to the next colour.
- **Flick up/down** to pick the previous or next item.
- **Tap twice** to use the item (A still works too).

A flick or double tap while the menu is closed opens it first. B closes the
menu, but the ring stays on screen.

When aiming the Fire Blaster (or another staff blaster), the aim stays where
you leave it instead of springing back to the centre. Move it with the Circle
Pad or by turning the console; pushing past the edge of the screen turns Fox.
In menus, first person (C-stick zooms), the world map and the title screen,
the C-stick works like the GameCube one.

Switches for `env.txt`: `S3D_CSTICK_CAMERA=0` (C-stick back to the original
menu), `S3D_GYRO_AIM=0`, `S3D_GYRO_SENS=100` (percent), `S3D_GYRO_INVERT_X=1`,
`S3D_GYRO_INVERT_Y=1`, `S3D_CAM_SPEED=150` (degrees per second),
`S3D_CAM_INVERT_X=1`, `S3D_CAM_INVERT_Y=1`, `S3D_AIM_HOLD=0` (the game's
spring-back aim), `S3D_CMENU_BOTTOM=0` (the ring back on the top screen).
Details are in `3DS/CONTROLS.md`.

## Settings menu

The Settings tab on the touch screen lists the everyday options. The
defaults are: 30 FPS cap on, adaptive frame pacing on, shadows on, fog on,
fur in cutscenes, 8 grass layers, depth of field on, gyro aiming on, C-stick
camera on and logging off. The 3D options are "3D: faster (fewer effects)"
(half the fur and grass layers and no blur while the 3D slider is up) and
"3D: depth follows Fox" (the screen depth tracks Fox).

The renderer and debug options are hidden and keep their defaults: render
thread on core 2, far animation LOD strong, back-face culling on, per-pixel
lighting on, fur shading/wind/layer budget on, water ripples and caustics
on, heat haze on, texture LOD 0, lean shaders "fast math" and the debug mip
level off. `S3D_MENU_ADVANCED=1` in `env.txt` lists them again.

Changes are kept in `settings.txt`. A `settings.txt` from a build before
these defaults is ignored once. For bug reports, turn Logging on (or put
`S3D_LOG_VERBOSE=1` in `env.txt`): with it off, only warnings and errors are
logged after the first frame.

## Languages

The game text can be English, French, German, Italian or Spanish, as on the
PAL GameCube release. On the title screen, open the fourth entry (the one
with Subtitles): the five languages are listed under Subtitles. Pick one with
A and the menus change at once.

- A new save starts in the 3DS system language (any other system language
  gives English). Saves from older builds of the port start in English.
- The language is kept in the save's options, like Subtitles, so it is
  written to the card with the next save (or when the title screen creates
  the save file).
- Saves imported from a PAL GameCube card keep their language.
- `FOXHOLLOW_LANGUAGE=en|fr|de|it|es` in `env.txt` forces a language at boot
  and the save's language is then ignored (the menu still changes it until
  the next start).
- Voices stay English (the PAL disc has only English speech).
- The dino language cheat has no On/Off row in this menu, as in the PAL game.

How it works (the build compiles the US code against the PAL disc, so the
US code had no language switch): `3DS/patches/dlls/engine/55/55.c.sed` puts
the PAL panel and its selection handler in the options screen,
`3DS/patches/main/textrender_run.c.sed` adds `gameTextSetLanguage` (the PAL
one without the disc-error messages) and the save language helpers,
`3DS/patches/dlls/engine/21/21.c.sed` applies the saved language when the
options load, `3DS/patches/dlls/engine/23/23.c.sed` sets a new save's
default, `3DS/patches/include/main/dll/savedata_struct.h.sed` names the
options byte (`languageIndex`, the US layout's `pad01`), and `OSGetLanguage`
(`3DS/port/os_3ds.c`) reads the 3DS system language.

## Files on the SD card

| Path | Contents |
| --- | --- |
| `sdmc:/3ds/star3d/card/` | Saves (`Star 3D.bin`, `Star 3D.stat`) |
| `sdmc:/3ds/star3d/env.txt` | Optional runtime switches, one `NAME=value` per line |
| `sdmc:/3ds/star3d/settings.txt` | Touch screen settings (`env.txt` wins over it) |
| `sdmc:/3ds/star3d/Star3D.log` | Log of the last run: errors, crash reports, system info, timings |
| `sdmc:/3ds/star3d/Star3D.prev.log` | Log of the run before that |

The switches use the `S3D_` prefix, for example `S3D_STEREO=0` (no stereo 3D),
`S3D_WIDESCREEN=0` (4:3) or `S3D_BOTTOM_HUD=0`. The log lists every switch
the game queried. See `3DS/port/config.h` and `3DS/GRAPHICS_PLAN.md` for the
full list. Press START+SELECT to quit.

Data from older builds (`sdmc:/3ds/sfa_port/`) is moved to the new folder
automatically on the first start.

## Repository layout

- `3DS/`: the port, made up of the Makefile, the Stellaris engine (the 3DS
  platform layer: `port/`, `gx/`, `os/`), build-time patches to the game code (`patches/`)
  and tools (`tools/`).
- `foxhollow/`: submodule with the game code. It is never modified; fixes
  are applied at build time from `3DS/patches/`.
- `SFA-Decomp/`: submodule with the decompilation, used for reference and by
  `3DS/tools/probe_layout.sh`.

## Credits

- [foxhollow](https://github.com/JackPriceBurns/foxhollow) by JackPriceBurns:
  the native port this project builds on (CC0).
- [aurora](https://github.com/encounter/aurora) by encounter (Luke Street),
  as vendored in foxhollow by JackPriceBurns (MIT). The THP decoder and the
  matrix library are used.
- [borealis](https://github.com/encounter/borealis) by encounter, part of
  foxhollow's tree (MIT).
- [SFA-Decomp](https://github.com/zcanann/SFA-Decomp) by zcanann and its
  contributors: the decompilation (CC0).
- MusyX runtime as included in foxhollow (MIT, Axiomatic Data Laboratories).
- [devkitPro](https://devkitpro.org/): devkitARM,
  [libctru](https://github.com/devkitPro/libctru) and
  [citro3d](https://github.com/devkitPro/citro3d).
- [makerom / Project_CTR](https://github.com/3DSGuy/Project_CTR): CIA packaging.
- [zlib](https://zlib.net/) and [Pillow](https://python-pillow.org/).
- [Azahar](https://azahar-emu.org/): the 3DS emulator used for testing.

This is a fan project. It is not affiliated with or endorsed by Nintendo or
any other rights holder. All trademarks belong to their owners.
