# Web converter for the game data (like the Simpsons Hit & Run 3DS builder)

Goal: let players turn their own disc image into `sdmc:/3ds/star3d/gamedata/romfs.bin`
without installing a build toolchain. The Simpsons Hit & Run 3DS port's
["browser CIA builder"](https://github.com/AlexMolina08/The-Simpsons-Hit-and-Run-O3DS/tree/main/browser-builder)
is the model to copy: a static page, all conversion in the browser (Web
Workers + WebAssembly), nothing uploaded, nothing copyrighted hosted.

Status: **planned**. Nothing below is implemented yet.

## Why this is easier for us than for SHAR3DS

Their builder packs a complete CIA in the browser (game data **and** code,
makerom-equivalent in WASM). We only need the **data file**: the code ships
separately as the small `.cia`/`.3dsx` (GitHub Actions already builds it).
The browser tool only has to run the extraction pipeline
(`tools/extract_disc.py` and friends) and zip `3ds/star3d/gamedata/romfs.bin`.
No CIA packaging, no banner, no makerom in the browser.

The pipeline is Python + Pillow + two small C libraries (ctypes):

| Part | Pure Python? | What happens without the native lib |
| --- | --- | --- |
| `extract_disc.py` | yes | — |
| `audio_convert.py` (DSP-ADPCM) | yes | — |
| `tex_convert.py` | lib only (`texconv.c`, ETC1 encoder etc.) | textures stay in GameCube formats, the game converts them at runtime (slower, bigger data) |
| `mesh_reduce.py` | lib only (`meshsimp.c`) | models keep all polygons (heavier GPU load; ~47 vs ~39 ms hub) |
| `video_convert.py` | needs Pillow **and** the lib (ETC1 frames) | the attract movie is skipped entirely |
| `make_gamedata_zip.py` | yes | — |

So a browser build in two quality tiers:

- **Tier 1 (pure Python + Pillow):** valid data file, no movie, GameCube
  textures (runtime-converted), full-poly models. Simpler to ship first.
- **Tier 2 (full quality):** `texconv.c` / `meshsimp.c` compiled with
  Emscripten and loaded from Pyodide as native side modules, giving byte-for-
  byte the same data as the desktop build.

## Option A: in-browser converter (Pyodide)

Pyodide (CPython on WASM) already ships Pillow. Load the pipeline scripts,
feed it the ISO, write the zip in the browser.

- **Input:** a `<input type=file>` with the ISO (1.4 GB). Pyodide's virtual FS
  copies the file into WASM memory; the whole 1.4 GB ISO plus extracted files
  plus output will not fit the ~2 GB wasm32 heap. Needs the extraction loop
  reworked to stream: read FST, then read each file with `File.slice()` and
  pass bytes per file (discard after use), never load the whole ISO. The
  extraction already works file-by-file, so this is a moderate change
  (extract_disc.py reads the ISO with seek/read — a small shim object can
  back those calls with sliced File reads).
- **Output:** write the zip with a streaming writer into OPFS (Origin
  Private File System), then trigger a download from OPFS (Chrome/Edge can
  stream directly; Safari needs a full-file download fallback, same as the
  SHAR3DS builder documents).
- **Workers:** conversion is CPU-heavy (textures, mesh reduce, movie). Run
  Pyodide in several Web Workers, one per core, like the desktop build's
  multiprocessing. OPFS gives disk-backed scratch for the mesh/texture caches.
- **Tier 2 native libs:** build `texconv.c` / `meshsimp.c` with Emscripten as
  side modules; Pyodide can `loadWebAssemblyModule` / dlopen them, but the
  Emscripten version must match Pyodide's, and ctypes uses need small
  wrappers. This is the fiddliest step.
- **Gotchas:** Pyodide version pinning, `.wasm` MIME type on the host,
  Safari download/limits quirks, progress UI + cancellation, several GB of
  free disk (OPFS), mobile browsers will be slow or out of memory (SHAR3DS
  restricts phones).

Effort: Tier 1 ~2-3 weeks of work and testing; Tier 2 on top ~1-2 weeks.
Reference: SHAR3DS `browser-builder/` (workers, OPFS, checkpoints) — worth
studying, but ours is smaller: no CIA packaging.

## Option B: one-click desktop tool (PyInstaller)

Bundle the existing pipeline (Python + Pillow + the two native libs compiled
for the host) into a single executable per OS: "Star3D Data Maker.exe".
The tool shows the same progress as the build, writes
`star3d-gamedata.zip` next to the ISO. No browser limits, full quality,
works offline.

- PyInstaller + the `.so`/`.dll` native libs + Pillow; a GitHub Actions
  workflow can build the Windows/Linux/macOS binaries from the pinned Docker
  image (`pyinstaller` run per OS on `ubuntu`/`windows`/`macos` runners).
- Cost: one new `3DS/tools/data_maker.py` wrapper (reuse `extract_disc.py`
  + `make_gamedata_zip.py`), maybe a tiny progress UI (tkinter is fine).

Effort: a few days, low risk. This is the pragmatic first step while the
browser converter is built.

## Option C: server-side conversion (upload the ISO)

Rejected: hosting a service that processes copyrighted disc images is a legal
and bandwidth burden (1.4 GB uploads), and it stops working when the server
goes away. Both A and B keep the conversion on the player's machine.

## Recommendation

1. Ship **B** first (covers most players quickly, no legal exposure).
2. Then build **A tier 1**, link it from the README/releases (it replaces B
   for anyone with a decent desktop browser).
3. Add **A tier 2** once the Emscripten side modules work.
4. Until then, players without a PC can ask someone else to run the build;
   the gamedata zip is a fixed output per release, but only for their own
   dump.

## Deliverables when this is built

- The web page (static hosting, e.g. GitHub Pages) or the desktop tool
  download links in the README's Installing section.
- Release notes keep the same update contract: "new gamedata (version N)" or
  "gamedata unchanged" (RELEASING.md).
