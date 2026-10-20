# Debugging on real hardware (New 3DS)

How to find bugs that only happen on the console, and what the first
hardware bring-up (bug 12, 2026-10-11 to 2026-10-13) found. Azahar is not
a reliable reference for these: it models neither caches nor GPU faults,
and it skips several hardware checks (see "Azahar blind spots").

## Performance (2026-10-14)

- The hub no longer crashes, but runs at 8-14 fps.
- The cause: the CPU (~65 ms) and the GPU (~38 ms) never ran at the same time.
- Build **5EB6A447** (second version; 0AEC7BB3 gave 10.3 → 12.9 fps) overlaps them (GPU pipelining, touch UI toggle
  "GPU overlap"). Design, hazards, counters and the hardware test procedure
  are in [PIPELINE_PLAN.md](PIPELINE_PLAN.md).
- If the console hangs in `C3D_FrameEnd (GPU: previous frame)`, start there.
- The GPU trace turns pipelining off.

## Current state (2026-10-13, 24th log)

- **Bug 12 (boot hang at frame 3) fixed on hardware.** Cause: vertex shader
  output registers written partially (see "Azahar blind spots"). Thirteenth
  log (build 3579D2E2, no self-test): the game boots, loads the sound banks
  (starfoxs.sam 9.7 MB in 885 ms), the memory card and map 63 (attract scene),
  starts the attract movie, then **hangs the GPU at frame 52**:
  `C3D_FrameBegin (GPU: previous frame)`, so frame 51's GPU work never
  finished. Frame 51 is the first 3D frame (the first `TEV program needs more
  than 6 PICA stages` and `indirect texturing not supported` warnings, both
  also in Azahar). This is bug 14.
- In Azahar the same frame is f107 (frame numbers drift with SD speed: the
  map loads at f100 there, f44 on hardware). It is the first frame with
  EFB copies (`copytex`, 32x64 and 32x32), 32x32 offscreen targets in VRAM
  (0x1F464400), fragment lighting (`light 0x1199280`) and mipmapped
  RGB565 / ETC1 textures. Trace saved in 3DS/build/emu/trace107.log.
  Odd: Azahar's trace of f107 stops after step #40 with no frame end; the
  frame counter seems to advance mid-frame there.
- **Bug 13 solved** (build ids match the tool since 121C6E79).
- texcoord0w: t7 (own register) passed; t6 / ta (sharing texcoord0's
  register, needed by the game shaders) not run on hardware yet. Since
  build EBA4E4C4 the water shader (gx/water.v.pica) uses the t7 layout with
  unit 0 in GPU_TEX_PROJECTION mode in game (OPEN_BUGS.md bug 28); verified
  on hardware, so projective texturing works there.
- 14th log (build 6F4808B1): same hang, now at f93 (map loaded at f85,
  first 3D frame f92). No `gputrace` lines and no `init env ...` lines:
  **env.txt was not read** (missing or misnamed on the SD card), so the trace
  never ran. The starred switches in the config summary came from the touch
  menu settings file. Frame numbers also drift too much between runs for a
  narrow trace window.
- **15th log (build 524C9672, trace from frame 0, 3D slider up): the hanging
  operation is `f28 #84 draw 00000600`**: 1536 indices, game lit shader,
  `light 0x1199280` (s_fog_env: the first draw with fragment lighting) and
  tex0 64x64 RGB565 with mip levels 0-3 (the first mipmapped texture). All
  earlier draws passed, including stereo draws, the movie's 512x256 RGB565
  texture and two copytex. With the slider up, offscreen passes are skipped
  (S3D_OFFSCREEN_STEREO=0), so it drew into the 240x800 stereo target; in
  Azahar (mono) the same draw goes to a 32x32 offscreen target.
- Azahar dump of that draw (`S3D_GPU_DUMP_LIT=1`, decoded in
  3DS/build/emu/lit_dump.txt): 2 directional specular lights (env lights 1-2,
  permutation 0x21), spotlight LUT with N.H input, D0 off, clamped highlights,
  fog light off; bool uniforms 0x0507 (fraglit on: first use of the shader's
  fragnq path). Every register looks legal; nothing obviously wrong.
- Build 586FBEB5 added `S3D_GPU_DUMP_LIT=1` and self-test steps 16-21.
- **16th log (586FBEB5, `S3D_GPU_SELFTEST=1`, FROM=16): step 16 hung.** That
  step is the game lit shader with only the `fraglit` bool on, so the shader
  runs its fragnq proc; no fragment lighting is configured. Step 11 (same
  shader, same vertex lighting, fraglit off) passes, so the hang is in
  fragnq's code. fragnq had the only `cmp` and `jmpc` in any of our shaders
  (the degenerate-normal branch). The other instructions it uses also run in
  the light proc, which passes.
- Build BEE704C3 made fragnq branchless:
  q = normalize(n.x + 1/16384, n.y, 1 + n.z, 0), which gives the same values
  (max difference 0.01, only for normals pointing straight away from the
  camera; such a normal gives (1, 0, 0, 0) as before). New mode-9 self-test
  steps 24 (tb: the old fragnq with the cmp, no jmpc) and 25 (tc: cmp +
  jmpc) tell which of the two instructions hangs.
- **17th log (BEE704C3, SELFTEST=1 FROM=16): step 16 still hung**, so
  cmp/jmpc was NOT the cause. **18th log (SELFTEST=9 FROM=24): tb and tc
  pass** (the old fragnq math with cmp + jmpc, inline, each output written
  once), then the game hung on the same draw (f64 #57, first fragment-lit
  draw). Remaining differences between tc and the game shader: the game
  shader wrote outview / outnq in main and then **again** inside fragnq, it
  reached fragnq through `ifu` + `call`, and step 16 also runs vertex
  lighting.
- Current build: build id **152A5735**. The game lit shader computes the view
  vector and quaternion inline in an `ifu fraglit` / `.else` block into
  r12 / r11 and writes outview and outnq exactly once afterwards (no call, no
  second write). New steps: 22 tf (proc under ifu + call, single writes),
  23 te (double writes, inline), and in mode 9 step 28 td (double writes, the
  second in a called proc = the old structure, expected to hang). t9/t4/tb/tc
  moved to 24-27. Azahar: every step passes, the attract scene renders.
- **19th log (152A5735, SELFTEST=1 FROM=16): bug 14 cause found.** Step 16
  (restructured game lit shader) now passes, and so do 17-21 (mipmaps,
  1 light, spec lights + N.H LUT, fog light, spec + mip): fragment lighting
  and mipmaps were never the problem. 22 tf (proc under `ifu` + `call`,
  single writes) passes; **23 te (outview / outnq written in main, then
  again inline under `ifu`) hangs**. So rewriting an output register under a
  condition hangs the GPU, with or without a call. A straight-line rewrite
  does not: unlit / warp write `outview` then `outview.z` and pass on
  hardware. The run ended at te, so the game itself did not run.
- 20th log (SELFTEST=9, FROM=**25**, not 28): ran t4, the known hang. No new
  information; td is no longer needed (te already answers it).
- Current build: build id **221D9B51**. te only runs in mode 9 now (same
  step number 23), so SELFTEST=1 continues into the game. default.v.pica: the
  normals capture (`nrmout`) used to overwrite `outclr` after the light proc
  (the same conditional double write); it is now `ifu nrmout` / `.else` around
  the lighting block. Azahar: steps 16-22 pass, attract scene renders.
- **21st log (221D9B51, SELFTEST=1 FROM=16, TRACE=1000): bug 14 fixed on
  hardware.** Steps 16-22 pass, the game runs, and the fragment-lit draws
  f64 #57 / #60 / #63 (which hung before) finish. **New hang (bug 15):
  `f64 #70 draw 00000048`**: 72 indices, no lighting, tex0 0x310c5960
  pa 210C5960 128x64 **fmt 12 (ETC1) levels 0-3**. Same texture as Azahar's
  f107 #26. It is the first ETC1 texture on hardware (the movie is shown as
  RGB565). It is zero-copy (the game's own data in MEM1, only 8-byte
  alignment guaranteed): the pa is 32-byte aligned, while every texture that
  passed was 128-byte aligned (...9D00, ...F580, ...B700).
- Current build: build id **D30A1698**. Self-test steps 29-32 appended (step
  numbers 1-28 unchanged): 29 ETC1 64x64, 30 ETC1 128x64 levels 0-3,
  31 RGB565 mip at pa % 128 = 0x60, 32 ETC1 mip at pa % 128 = 0x60.
  Azahar: all pass, attract scene renders.
- **22nd log (D30A1698, SELFTEST=1 FROM=29): bug 15 cause found.** 29 (ETC1)
  and 30 (ETC1 levels 0-3) pass; **31 (RGB565 mips at pa % 128 = 0x60)
  hangs**. So texture data that isn't 128-byte aligned hangs the real GPU,
  whatever the format.
- **23rd log (D30A1698, `S3D_TEX_ZEROCOPY=0`, TRACE=1000):** the three ETC1
  draws (now copied to 128-aligned buffers) pass. The log then stops after
  `f63 #80 draw`, with no ERROR or watchdog line. Not a GPU hang: Azahar does
  the same with the trace on. `svcBreak(PANIC)` in libctru's gxCmdQueueAdd
  (queue full), called from C3D_FrameSplit in gx_gpu_trace. Every traced
  operation adds queue entries, and citro3d's callback doesn't clear the
  queue mid-frame, so a busy frame overflows it. On hardware the panic froze
  the console with no `[CRASH]` block. (This also explains the "odd" stop
  of Azahar's f107 trace at #40.)
- Current build: build id **E66E6F2F**. gx_tex.c convert_pica only uses a
  texture in place when its pa is 128-byte aligned, otherwise it copies it
  (counter `gx.tex_pica_unaligned`). gpu_trace_wait stops and clears the gx
  queue after each wait. Azahar with TRACE=1000: no panic, 70k traced
  operations up to f279.
- **24th log (E66E6F2F, TRACE=1000): bug 15 fixed on hardware.** All of
  f64 runs (360 traced steps: fragment-lit draws, every ETC1 draw, copytex,
  HUD, peekz, frame end + screen transfers); the first complete 3D frame on
  the console. `gx.tex_pica_unaligned` 48 of 65 game textures (copied).
  Then the log stops with no ERROR, no watchdog line and no `[CRASH]`,
  before f65's first traced step (`frame begin`). The watchdog thread died
  too, so the whole process stopped (or the console froze): the same
  signature as the svcBreak queue panic. Azahar runs the same frames fine.
- Current build: build id **39851A7C**. `-Wl,--wrap=svcBreak` (os/crash.c):
  libctru / citro3d panics now log `[CRASH] *** svcBreak(reason) ... called
  from lr XXXXXXXX ***` and flush before stopping. The watchdog and audio
  stream threads now install the crash handler too (only main and the worker
  had it, so a fault there left no `[CRASH]` block).

### Next steps

1. The user runs 39851A7C with `S3D_GPU_TRACE=1000` again and reports what
   the screens showed when it stopped (frozen picture, Luma3DS exception
   screen, back to the Homebrew Launcher) and whether HOME / START+SELECT
   worked. Also check `sdmc:/luma/dumps/arm11/` for a new `.dmp` (Luma
   writes one for crashes no handler catches). A `[CRASH]` line now names
   any svcBreak caller or a fault on the watchdog / audio threads.
2. A run with `S3D_GPU_SELFTEST=1` for t6 (step 14) / ta (step 15). If they
   pass, restore projective unit 0: write the game shaders' slot 0 in that
   layout (no per-vertex divide, `texcoord0w` = q, every component written)
   and put `GPU_TEX_PROJECTION` back on unit 0 (gx_render.c bind_unit; the old
   code used a per-eye `s_unit0_view[2]` copy of the texture with the mode
   bits changed).
3. Remove the trace switches from the console's env.txt once the game runs.

## How the user shares logs

- The log is `sdmc:/3ds/star3d/Star3D.log` (previous run: `Star3D.prev.log`).
  The user copies it to the repo root as `Star3D.log`.
- Older runs came as a saved Google Drive page (`.mht`): split on the MIME
  boundary, take the text/html part, strip the tags (log lines start with `[`).
- **Check the build first.** The header's `build DATE TIME` comes from
  log.c's `__DATE__`/`__TIME__` (Docker clock is UTC, local + 3 h) and only
  changes when log.c recompiles, so it is useless for this. Use the
  `build id XXXXXXXX` line instead: `python 3DS/tools/build_id.py out/star3d.elf`
  prints the id of a build (it skips the crt0 block the launcher fills in, bug 13;
  builds before 121C6E79 never matched on hardware).
- Test builds for the console: skip the .cia (user request 2026-10-13):
  `docker run --rm -e NO_CIA=1 -v "D:\opencode:/workspace" star3d-builder` (from Git Bash prefix `MSYS_NO_PATHCONV=1`)
  (about 1.5 min). It deletes the old `out/star3d.cia`, so only the fresh
  .3dsx is left to copy.

## Tools

All switches go in `sdmc:/3ds/star3d/env.txt` (one `NAME=value` per line).

### Watchdog (os/watchdog.c)

- Logs `no frame for N s (stuck at frame F) in "<phase>"` every 2 s while no
  frame completes. Read the phase first. Phases come from `WD_PHASE()` markers
  (vi_3ds.c, gx_frame_begin, dvd sync reads, worker_wait, gputrace).
  `C3D_FrameBegin (GPU: previous frame)` = the GPU did not finish the last
  frame (GPU hang).
- After 6 s stuck, holding START+SELECT exits the process (svcExitProcess).
- **Never read GPU registers while the GPU hangs.** It used to call
  `GSPGPU_ReadHWRegs` once per stall; on hardware that call never returned
  and the log stopped right there (twice, also from a separate thread),
  so it was removed.

### GPU trace: `S3D_GPU_TRACE=N` (gx_init.c gx_gpu_trace)

Before E66E6F2F, a busy traced frame overflowed the gx queue around step 80 (svcBreak in gxCmdQueueAdd; on hardware a silent freeze, the log just stops).

For frames `S3D_GPU_TRACE_FROM` (default 0) to N-1, each draw, frame begin (clears), HUD begin, offscreen begin,
EFB copy (copytex), mip DisplayTransfer, peekz copy and frame end is logged
(tag `gputrace`), submitted, and waited for with a 2 s timeout (see the list below).

`S3D_GPU_TRACE_MAP=id` (build 0F43047B): no frame is traced until map `id`
loads (the game's `[foxhollow] map-loaded id=N` line, caught with
`--wrap=fhNoteMapLoaded`), then N frames from that frame on. Use it when the
frame number depends on how long the user stays in the menus. The log shows
`map 7 loaded: tracing frames F-G`. Map ids: 63 = attract scene, 7 = ThornTail
hub (loaded from a save).

`S3D_GPU_SYNC=mask` (build 94D5CE74, bug 17): inside the trace window, log
nothing per operation and copy only parts of what the trace does, at close
to normal speed. 1 = flush the frame's vertex/index data before each
operation, 2 = submit each operation and wait for the GPU (a timeout logs
`GPU still busy 2 s after "fN #k <op> ..."`, naming the operation), 4 = wait
for the GPU at frame end so the next frame's CPU work never overlaps it.
8 (build B4DBF439) = only write the framebuffer flush / invalidate / early
depth clear citro3d adds at a split, after each operation (no split, no
wait). `S3D_GPU_SYNC_OPS` limits bit 2 to draws (1) or non-draw operations (2).
Bit 2 needs bit 1: work submitted mid-frame runs before citro3d's frame-end
flush of the whole linear heap.
16 (build 351B64C9) = split and wait only right before draws whose pending
citro3d state (`__C3D_Context.flags` after prune_state) matches
`S3D_GPU_SYNC_FLAGS` (0x prefix for hex), so the GPU is idle when that
state is written. Bit 0 = every draw; 2 attr info, 3 buffer info, 4 effect,
5 framebuffer, 6 viewport, 7 scissor, 8 program, 9 texenv buffer, 10 light
env (also set when the light env itself is dirty: LUT uploads), 14 tex
config, 15-22 proctex/fog/gas, 23-25 texture units, 26-30 texenv 0-4 (bit 31,
texenv 5, can't be set: cfg_int clamps). The first traced frame with 50+ draws
logs `draws per dirty bit`. Logs nothing per draw; a hang logs `GPU still
busy 2 s after "fN commands before draw #k (dirty XXXXXXXX)"`. Azahar,
attract scene: mask 1 runs at 6 fps, 0x100 at 16 fps, no errors.
Since build 2091F24C every mid-frame submit (trace, SYNC bits 1/16) flushes
the whole linear heap first, as C3D_FrameEnd(0) does.

`S3D_GPU_CHECK_REUSE=1` (build 2091F24C, bug 17): each draw records a hash of
its indices, the vertex range they reach in every buffer and the first 2 KB of
its textures; before C3D_FrameEnd the hashes are compared, and a draw whose
data changed in between is logged (`reuse` tag, first 16 per frame, with old
and new first indices and, for frame buffer draws, the gx_render_flush
caller). That data reaches the GPU changed, since the frame runs only at
C3D_FrameEnd. Slow (Azahar about 5 fps); use it in Azahar.

The trace works like this:

- `C3D_FrameSplit(GX_CMDLIST_FLUSH)` + `gxCmdQueueRun`. citro3d only starts
  its gx queue in `C3D_FrameEnd`; its completion callback stops and clears the
  queue again, so mid-frame waits don't overflow it.
- Before each draw: `  tex0 <va> pa <pa> WxH fmt lv | texcfg ... fb color/depth
  WxH idx <pa> vbase ... light fog proctex`. An address the GPU can't use is
  logged as `ERROR bad GPU address`.
- A hang shows as `ERROR gputrace GPU still busy 2 s after "<op>"`.
- `S3D_GPU_SKIP=k` drops the draw that would be trace step #k (A/B test).
- `S3D_GPU_DUMP=k` logs the raw PICA command list of step #k (tag `gpucmd`).
  `S3D_GPU_DUMP_LIT=1` dumps the first draw with fragment lighting instead
  (no step number needed).
  Decode: `grep gpucmd Star3D.log | python 3DS/tools/pica_decode.py`
  (register names from libctru's registers.h). The list is the same in Azahar
  except for addresses, so capture it there.

### GPU self-test: `S3D_GPU_SELFTEST=1` (gx_init.c gx_hwtest)

Before the game, draws one quad per step on the top screen, each in its own
frame with a 2 s timeout; the log names the first step that does not finish
(`ERROR hwtest step N (...) hung the GPU`). Steps go from known-good to the
full game shaders:

| Step | Shader | Notes |
|------|--------|-------|
| 1 | bench.v.pica + bench layout (24 B) | baseline, as os/bench.c |
| 2 | t0 (bench copy) + GxOutVertex layout (52 B) | tests the vertex layout alone |
| 3 | t1: position + color | |
| 4 | t2: + `mova` relative posmtx + projection | |
| 5 | tz: exact copy of t2 at the end of the .shbin | position test, passed |
| 6 | t8: t4 with every output component written | passed |
| 7-12 | game lit from entry 0, unlit (arrays, elements), lit (lighting off/on), warp | passed (12th log) |
| 13 | t7: texcoord0w in its own output register | passed |
| 14 | t6: texcoord0w in .w of texcoord0's register | untested |
| 15 | ta: texcoord0w in .z of texcoord0's register (old game layout) | untested |
| 16 | game lit, `fraglit` on (fragnq path), no light env | hung (16th, 17th logs); passed with the single-write shader (19th log) |
| 17 | game lit + mipmapped 64x64 RGB565 texture (levels 0-3) | passed (19th log) |
| 18 | + fragment lighting: 1 directional light, no LUTs | passed (19th log) |
| 19 | + 2 specular lights, N.H spotlight LUT (the game's setup) | passed (19th log) |
| 20 | fog light (eye light with distance attenuation LUT) | passed (19th log) |
| 21 | spec lights + mipmapped texture together | passed (19th log) |
| 22 | tf: fraglit proc under `ifu` + `call`, each output written once | passed (19th log) |
| 23 (mode 9) | te: outview / outnq written in main, then again inline under `ifu` | **hangs (19th log): bug 14 cause**; mode 9 only since 221D9B51 |
| 24 (mode 9) | t9: t1 + `mov outtc0.xy, v3` | hangs: minimal partial write |
| 25 (mode 9) | t4: texcoord0/1/2 divided per vertex, `.xy` writes | hangs |
| 26 (mode 9) | tb: t8 + the old fragnq code with `cmp`, no `jmpc` | passed (18th log) |
| 27 (mode 9) | tc: t8 + the old fragnq code (`cmp` + `jmpc`) | passed (18th log): cmp/jmpc are fine |
| 28 (mode 9) | td: double writes, the second in a called proc (old game structure) | not run; not needed after te |
| 29 | ETC1 64x64, no mipmaps | passed (22nd log) |
| 30 | ETC1 128x64, levels 0-3 | passed (22nd log) |
| 31 | RGB565 64x64 mips at pa % 128 = 0x60 | **hangs (22nd log): bug 15 cause** |
| 32 | ETC1 128x64 mips at pa % 128 = 0x60 (the hanging texture) | not reached (after 31) |

t3 (texcoord0w in .z, partial write) hung on hardware; it is kept in
gx/hwtest/ but no longer run. A hang ends the run: put steps expected to hang
last and use `S3D_GPU_SELFTEST_FROM=k` to reach them. Each step logs
its code words (`code words A-B`). The self-test also logs the shbin hashes
and a hex dump of hwtest.shbin as the console sees it. Test shaders live in
`gx/hwtest/tN.v.pica`; the Makefile builds them into `hwtest.shbin` (one DVLE
each, in file name order). To test a new suspect feature, add a tN shader
and a step.

### Crash reports

`[CRASH]` blocks give pc/lr/far and stack words. Resolve with
`arm-none-eabi-addr2line -f -i -e out/star3d.elf <pc> <lr> <stack words>`
(the out/ ELF must be the build that ran).

## Azahar blind spots (all hit during bug 12)

- **Partial output register writes** (bug 12 cause, confirmed by t8/t9): a vertex shader that leaves components of an output
  register unwritten (`mov outtc0.xy, ...` and nothing to .zw) hangs the real
  GPU. Write every component of every output (`mov outtc0.zw, zeros`). An
  earlier conclusion blamed `texcoord0w` (t3); t3 also wrote outtc0 partially.
- **Conditional output rewrites** (bug 14 cause, confirmed by te/tf): writing
  an output register and then writing it again inside an `ifu` block hangs
  the real GPU (te), whether inline or through a call. A call alone is fine
  (tf), and so is a straight-line rewrite (`mov outview, zeros` then
  `mov outview.z, r1.z` in unlit / warp). Rule: compute into a temp register
  under the condition and write each output once, after the branch.
- **Vertex shader bool b15** (2026-10-15, build B3EE1AF9): moving `nrmout` to
  b15 made every lit model on the console draw its view-space normals as
  vertex color: washed-out white Fox and Arwing. The capture's `light` pass
  showed the normals colors, so the branch ran though the CPU set the bool to
  false. Azahar treats b15 as an ordinary bool. citro3d also clears b15 for
  geometry shaders. Rule: vertex shader bools stop at b14. `init_shader`
  (gx_init.c) now refuses a variant with a bool at b15.
- **Transfer engine minimum size** (bug 21 cause, fix verified on hardware 2026-10-18): a tiled ->
  tiled display transfer (flag BIT(5), what `gen_mips` in gx_copy.c uses with
  2x2 downscale to build a copy's mip chain) needs an input of at least 64
  wide and 32 high (3dbrew, GPU/External Registers; tiled -> linear needs
  64x16). The pause menu's 128x128 slot shadow copy asked for 3 mip levels,
  and level 3 read the 32x32 level 2: the console's GPU never finished the
  frame (render thread stuck in `gx_gpu_wait_previous`). Azahar runs any
  size. citro3d's `C3D_TexGenerateMipmap` gives up on the transfer for the
  same reason ("size restriction") and filters on the CPU. Rule:
  `mip_levels_for` only allows level l while level l-1 is >= 64x32.
- **Texture alignment** (bug 15 cause, self-test step 31): texture data
  must start on a 128-byte boundary (the step at pa % 128 = 0x60 hangs; at
  0x00 it passes). Azahar accepts any 8-byte aligned address. citro3d's
  allocations are 0x80 aligned; game data used in place (zero-copy) is not.
- **Vertex loader stride**: the hardware loader needs each buffer's
  components, including padding components (types 0xC-0xF = 4/8/12/16 bytes
  in the permutation), to add up to the stride. Azahar just steps by the
  stride. The std layout loaded the normal as s16x3 (50 of 52 bytes) and the
  two-buffer paths left 20-byte gaps; all fixed (s16x4 normal, perm
  `0xCF54321`). This alone did not fix the hang (the self-test later showed
  the 52 B layout works), but it is a real rule.
- **NULL reads**: Azahar returns data for near-NULL addresses; hardware data
  aborts. Example: citro3d 1.7.1's `C3D_TexBind(unit, NULL)` dereferences the
  texture. Use `gx_c3d_tex_unbind(unit)` (clears `__C3D_Context.tex[unit]` and
  sets the dirty flag; C3Di_UpdateContext skips NULL units).
- **Texture units are always fetched**: an enabled unit is read even if no
  combiner uses it. Unused units are now unbound in bind_textures, and all
  three after the touch UI (citro2d left a system-font glyph sheet bound).
- **Caches**: Azahar doesn't model them, so missing `GSPGPU_FlushDataCache`
  calls only show on hardware.
- **LLE audio with the placeholder firmware** freezes Azahar at frame 2 (fake
  repro of bug 12). Azahar's qt-config.ini is left on `audio_emulation=1`
  (LLE) and `%APPDATA%/Azahar/sdmc/3ds/dspfirm.cdc` is a 4 KB zero file;
  switch to `audio_emulation=0` for normal runs (run_emu.ps1 doesn't), or copy
  the console's real dspfirm.cdc.

## Ruled out for bug 12 (don't re-investigate)

- Slow SD card: simulated in Azahar (S3D_IO_SIM_*), never stalled; a later
  hardware run with faster SD reads hung identically.
- Audio / DSP: `ndsp ready` on hardware; sample buffers are linear and flushed.
- The stale citro2d font texture on unit 0: draw #6 still hung with all
  units disabled (`texcfg 00001000`).
- Shader size: 342 instructions / 50 operand descriptors (limits 512 / 128);
  commands with at most 256 parameters; uniforms sane for draw #6
  (orthographic projection, identity matrices, q = 1).
- Game state in general: the self-test hung with a bare quad and no game state.
- texcoord0w as the cause (t4 without it hung too), reading inputs v3+ (t0
  reads v3 and passes), shader code position (tz at words 134+ passes),
  corrupt shader data (11th log: shbin and rodata hashes match the files),
  libctru's code upload (disassembled, correct).

## Hardware numbers (2026-10-11 to 13 logs)

- SD: 4 KB sequential reads 10-67 ms each (varies by run), 1 MB reads ~10 MB/s.
- GPU fill ~195 Mpixel/s flat, ~135 with 3 textures / blending.
- memcpy ~400 MB/s; zlib 20 MB/s, lz4 53 MB/s per core.
