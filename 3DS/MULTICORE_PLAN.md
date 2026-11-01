# Multicore plan (Star 3D)

Status (2026-10-14): build steps 1 and 2 are implemented (see
"Implementation" below). First hardware run done (build 89A1E78D, see
"Hardware results (build 89A1E78D)"): no hang, but the render thread gains
only ~1 ms in the hub, and every mode is 10-17 ms slower than build 5EB6A447.
**Cause found by the A/B runs: the core 1 time limit.** With `S3D_CORE1=0`
(no limit set, nothing on core 1) the hub runs at 57.3 ms (17.4 fps) with the
render thread. At the same spot, 5EB6A447 ran at 65.8 ms with no render
thread. Core 2's GX replay (47.6 ms) is now the limit. Goal:
30 fps in the hub, so the frame must be under 33 ms. GPU overlap
(PIPELINE_PLAN.md) is done, so before this work the hub frame equalled the
CPU time on core 0.

This port is New 3DS only: core 2 is assumed to exist (the old "Old 3DS:
mode 1 or off" step is dropped). If the render thread cannot be created, mode
2 falls back to mode 1 with a log line; nothing else depends on it.

## Implementation (steps 1 and 2, 2026-10-14)

Threads now:

| Thread | Core | Priority | Work |
|---|---|---|---|
| Game thread | 0 | 0x30 | Game logic and traversal; GX calls are only **recorded** (mode 2) |
| Render thread (`gx/gx_rec.c`) | 2 | 0x2E | Replays the recorded GX calls through the unchanged gx/*.c into citro3d |
| Worker (`port/worker.c`) | 1 | 0x2F | DVD async reads, movie prefetch (was core 2) |
| Skinning helper (`port/helper.c`) | 1 | 0x2C | Fork-join jobs: skinning chunks |
| MusyX tick thread (`port/musyx_ndsp.c`) | 1 | 0x18 | MusyX synth ticks, woken by the ndsp frame callback (was inside the ndsp thread on core 0, ~175 us per 5 ms) |
| Audio stream reader (`port/audio_3ds.c`) | 1 | 0x2F | Disc stream chunk reads (was core 0) |
| ndsp thread, watchdog | 0 | 0x18 | unchanged |

`core1_init` (worker.c) asks for 80% of core 1 (`S3D_CORE1_LIMIT`, was 30;
`S3D_CORE1=0` keeps every helper off core 1). **Changed after the hardware
A/B runs: core 1 is off by default (`S3D_CORE1=0`).** The worker runs on
core 2, the MusyX ticks in the ndsp callback and skinning inline, so the
table above only applies with `S3D_CORE1=1`. See "A/B results".

### Step 1

- **Coarse zone** in every build: `game.sceneRender` (port/frame_zones.c, one
  zone per frame). With `S3D_RENDER_THREAD=1` the whole GX translation runs at
  the frame end and is timed by `rt.replay`, so one normal-build log splits the
  frame into game logic (frame - sceneRender - replay), traversal plus
  recording (`game.sceneRender`) and GX (`rt.replay`). This replaces the
  `gx.api_total` idea: a zone per GX call would cost ~20k tick reads per frame.
  Mid-frame drains (`rt.syncs`) happen inside sceneRender in mode 1 and are
  also in `rt.replay`; subtract them.
- **Worker, audio stream reader and MusyX ticks on core 1.** The MusyX ticks
  take the existing recursive lock (hwDisableIrq/hwEnableIrq) and wait for it
  on core 1, instead of try-locking inside the ndsp callback. Voice changes
  now reach the DSP one ndsp frame (~5 ms) later. `S3D_AUDIO_CORE1=0` restores
  the old path. `fhAIPump` stays on the game thread: its DMA callback is game
  code (attract movie audio).
- **Skinning jobs** (port/skin_3ds.c, `--wrap` of ObjModel_BlendVertexStream
  and ObjModel_BlendNormalStream). Each 0x74-byte chunk record is a job. The
  vertex call is queued and runs together with the normal call that always
  follows it, so a model costs one fork-join. The block copies run first, on
  the game thread in the game's order (blocks are rounded up to 32 bytes and
  can overlap); then the blends run on the game thread and the core 1 helper,
  both taking the next chunk (work stealing: a preempted core 1 only delays
  the chunk it already took). Same float operations as the game, with the
  GQR7 scale passed in. Falls back to the game's functions when outputs
  overlap sources (not in place) or a model has more than 256 chunks; models
  under 192 vertices run inline. `S3D_SKIN_JOBS=0` turns it off. Counters
  `skin.models_parallel`, `skin.models_inline`, `skin.models_game`,
  `helper.join_wait_us`; zone `skin.blend`. `modelAnimBuildJointMatrices`
  (parent before child) is not split.

### Step 2: recorder and render thread

`S3D_RENDER_THREAD` (default 2; touch menu Settings > "Render thread": Off /
Test (1 core) / Core 2, applied at the next frame start):
- 0: no recorder; GX runs on the game thread as before.
- 1: GX calls are recorded and replayed on the game thread at the frame end
  (and when the 2 MB ring fills or a call needs an answer). Checks the
  recording and the late reads without a second thread, and is the timing
  tool above.
- 2: the render thread on core 2 replays while the game continues.

How it works (details in gx/gx_rec.c and gx/gxrec.list):
- `gx/gxrec.list` lists every GX entry point and port hook that touches the GX
  state (g_gx) or citro3d: 107 names. `tools/gen_gxrec.py` generates the
  `--wrap` wrappers, the encoders and the replay switch at build time
  (build/*/gxrec_gen.c). The generated file redeclares each function with the
  listed prototype, so a mismatch with a header is a compile error. Anything
  new that touches g_gx or citro3d from the game thread must be added there.
- Arguments go into a ring of 32-bit words. Matrices, GXTexObj and GXLightObj
  are copied at record time (the real GX copies them into its FIFO); display
  lists, vertex arrays and textures are read at replay time, as the GameCube
  GPU reads them late. Immediate-mode vertex data is appended as raw
  big-endian bytes after its GXBegin, like the write-gather pipe.
- **Lag: within the frame.** `gxrec_frame_end` (VIWaitForRetrace) drains the
  ring, and the frame boundary (touch UI, C3D_FrameEnd, frame begin, save
  states, the GX cache reset) runs on the game thread with the render thread
  idle. So the gain is the overlap of traversal and recording with
  translation inside the frame; logic before the first draw does not overlap
  yet (step 4). Game memory only has to stay valid until the frame end, as on
  the GameCube; no render fence is needed yet.
- Calls with a result are answered on the game thread: `GXGetScissor` from a
  recorder-side copy, display-list recording (`GXBegin/EndDisplayList`) done
  on the game thread into game memory, `gx_hud_begin`, `gx_offscreen_begin`
  and `port_stereo_active/reduced` from the frame's fixed state plus the
  recorder's own HUD/offscreen tracking. `port_copytex_mip_alias` (the
  reflection, every frame) drains the first time, and later calls with the
  same arguments answer true on the game thread (`rt.answered.mip_alias`).
  The rest drains first ("sync"): `GXPeekZ` (frame-end breakpoint callback),
  `port_copytex_blur`, `port_copytex_blur_scope` (level > 0), `GXInit`, the
  first use of each offscreen target size, and port code bracketed with
  `gxrec_direct_begin/_end` (movie frame upload, save states).
- Thread safety added: profiler entries keep one accumulator per thread slot
  (game / render / helpers) and register under a lock; the watchdog keeps one
  phase per slot (the render thread's phase is the GX call it is replaying,
  printed in "no frame" warnings); libctru's linear and VRAM allocators have no
  lock (checked in libctru.a), so all 14 entry points are wrapped with one
  (port/alloc_lock.c; also covers citro3d and citro2d). The GPU trace / sync
  debug modes (`S3D_GPU_TRACE`, `S3D_GPU_SYNC`) force mode 0.
- `perf_wrap.c` no longer wraps sceneRender, GXCallDisplayList, GXEnd or the
  blend functions (a symbol can only be wrapped once); their timers moved into
  the new wrappers. Draw-source attribution (PROFILE=1) only works with
  `S3D_RENDER_THREAD=0`.

Log counters: `rt.replay` (replay time, either thread), `rt.cmds`,
`rt.frame_end_wait` (game thread waiting for the render thread at the frame
end; in mode 1 the replay itself), `rt.sync_wait`, `rt.syncs`,
`rt.sync.<name>` (which call drained), `rt.space_wait` (ring full).

### Azahar check (2026-10-14, build of this change)

Title, menus and the ThornTail hub run in mode 2 from boot. In the hub the
touch-menu switches 2 → 1 → 0 → 2 → 1 → 0 (standing and after turning) gave
identical screenshots (build/emu/m*_hub.png, m*_turn.png) and no decode
errors, bad vertices or stalls. Hub numbers in Azahar (its core 2 timing is
not the hardware's): ~20.8k recorded commands per frame, 2 syncs per frame
(both `GXPeekZ` at the frame end), `rt.answered.mip_alias` 1 per frame,
8 skinned models per frame in parallel. One "no frame for 2 s" right after the
hub loads: the first frame builds the texture and display-list caches, now on
the render thread (`render thread: "GXCallDisplayList"`).

Bug found and fixed in that run: after mode 2 had created the render thread,
switching to mode 1 let the sleeping thread wake on publish and replay the
same ring as the game thread (two consumers: corrupt display-list decodes,
then a hang). The thread now only consumes while mode 2 is active
(`s_thread_on`, set at the frame start while the ring is empty).

Also seen: `dvd: callback queue full, completing immediately` during boot (3x
in Azahar, 1x in the old hardware log). Safe fallback; may show up more often
now that the worker shares core 1.

### What to look at in the first hardware log (hub, standing still)

1. fps with the default (mode 2) against "Render thread: Off" in the touch
   menu, same spot. Estimate from Step 0: 66 ms to roughly 40-45 ms, since
   the logic before sceneRender still does not overlap.
2. `rt.sync_wait` and `rt.sync.*`: every mid-frame drain stalls core 0 until
   core 2 catches up. In Azahar only `GXPeekZ` remains (twice, in the
   frame-end callback, so `rt.sync_wait` there is really the frame-end wait
   and `rt.frame_end_wait` stays 0). A frequent sync early in the frame is the
   next call to answer on the game thread.
3. `rt.replay` (core 2) against `game.sceneRender` (core 0): which side is the
   limit.
4. Mode 1 ("Test (1 core)") against mode 0 on screen: a difference is a late
   read of game memory rewritten during the frame (suspects from the survey:
   splash colours, sky stars, the warp list).
5. Audio on core 1 (voices, music timing); `skin.models_parallel` and
   `helper.join_wait_us`.

Remaining: step 3 (touch UI, movie upload, capture, the vi reset and save
states behind the render thread, so the frame boundary needs no drain) and
step 4 (render fence, one frame of lag, late-read audit). If core 2 turns out
to be the limit, DRAWCALL_PLAN.md phase 2 applies to the render thread only.

## Hardware results (build 89A1E78D, New 3DS XL, 2026-10-14)

Log: repo root `Star3D.log`, one session of 637 s, quit with START+SELECT.
Defaults from env.txt (`S3D_CORE1=1`, `S3D_CORE1_LIMIT=80`, `S3D_HELPER=1`,
GPU pipelining on). The user ran mode 2, then Off, then "Test (1 core)". The
log shows these touch-menu switches:
- title and attract scene (map 63, `gamefront`), f455-f4145: many switches
  between modes 2, 0 and 1;
- ThornTail hub (`swaphol`, loaded at f5389): mode 2 until f5834, then 0
  until f6202, then 1 until the end.
In the hub the player stood still at -5593 -780 -3148, angle 113 (807 draws,
85k triangles; the 5EB6A447 spot had 644 draws and 72k triangles).
Per-segment averages come from `tmp_scripts/seg_prof.py` (usage:
`python -I tmp_scripts/seg_prof.py Star3D.log t0:t1 ...`, times in seconds
from the log). The first report window after each switch is skipped.

**Stability: OK.** No hang, crash, svcBreak or decode error in any mode, and
the runtime switches (2 → 1 → 2, 2 → 0 → 1 → 2, ...) went through cleanly.
The fix for two consumers on one ring holds on hardware. The only ERROR is
the known `dvd: callback queue full` at boot (f32). Watchdog lines appear only
during the startup benchmarks. Visuals in mode 1 and audio on core 1: no
report from the user yet (ask).

### Hub, standing still

| hub (ms/frame) | mode 2 (core 2) | mode 0 (off) | mode 1 (1 core) |
|---|---|---|---|
| frame | **86.5 (11.6 fps)** | **87.7 (11.4 fps)** | **94.8 (10.6 fps)** |
| `game.sceneRender` (core 0) | 23.6 | 68.5 | 20.6 |
| `rt.replay` (GX translation) | 68.5 (core 2) | - | 56.7 (core 0, at the frame end) |
| `rt.sync_wait` (core 0 waiting at the frame end) | 45.0 | - | - |
| rest of the frame (logic, audio, frame end) | ~17.4 | 19.2 | 17.5 |
| `gpu.split_flush` (11 calls) | 9.4 | 8.3 | 8.7 |
| `audio.pump` | 4.9 | 5.3 | 4.8 |
| `skin.blend` / `helper.join_wait_us` | 1.4 / 0.19 | 1.3 / 0.12 | 1.2 / 0.13 |
| `rt.cmds` / `rt.syncs` | 24.4k / 2 (`GXPeekZ`) | - | 24.5k / 2 |

`gpu.submit_us` equals the frame and `gpu.wait_us` is 0 in every mode: the GPU
is not the limit.

What the hub numbers mean:
1. **Core 2 is the limit.** The frame is the non-overlapping logic (~18 ms)
   plus the core 2 replay (68.5 ms). The game thread finishes traversal and
   recording in 23.6 ms, then waits 45 ms for core 2 at the frame end
   (`rt.sync_wait`, through the frame-end `GXPeekZ` sync). The render thread
   saves only 1.2 ms over Off.
2. **GX translation costs far more than Step 0 estimated.** Mode 1 times it
   alone on core 0: **56.7 ms** (including ~8.7 ms of split flushes), against
   the 25-35 ms estimate. Traversal plus recording is 20.6 ms and logic
   ~17.5 ms, so game code is ~38 ms, at the top of its estimate.
3. **The same replay is 21% slower on core 2** (68.5 vs 56.7 ms), and the
   game thread's `sceneRender` is 15% slower while core 2 runs (23.6 vs
   20.6 ms). Both cores slow down when they run at the same time. Candidates:
   shared L2 / memory bus contention, the allocator lock (port/alloc_lock.c),
   cache-line traffic on the ring. Not measured yet. The startup benchmark
   has no core 2 number.
4. **Recording overhead:** mode 1 is 7 ms slower than Off (77.3 vs 68.5 ms
   for sceneRender plus replay). That is the cost of encoding and decoding
   ~24k commands per frame (~0.3 us per command).
5. Step 4 alone (logic overlapping the previous frame's replay) would give
   at most max(42, 68.5) = 68.5 ms (14.6 fps). Total CPU work in mode 1 is
   ~95 ms per frame, so even a perfect 2-core split is ~48 ms. **The render
   thread cannot reach 33 ms unless GX translation gets much cheaper**
   (DRAWCALL_PLAN.md phase 2/3, split flushes) and the regression below is
   recovered.

### Attract scene (map 63, 333 draws, movie playing)

| attract (ms/frame) | mode 2 | mode 0 | mode 1 |
|---|---|---|---|
| frame | 80.6-82.5 | 76.7-78.3 | 83.2-84.3 |
| `game.sceneRender` | 10.3-10.7 | 34.2 | 9.1 |
| `rt.replay` | 41.8-42.6 (core 2) | - | 33.0-33.3 |
| `rt.sync_wait` | 30.4-31.0 | - | - |
| `worker.busy_us` (core 1) | 42-45 | 39-41 | 39-41 |
| `stub.PADReset` (I/O spin count) | ~11k | ~11k | ~11k |

Here mode 2 is **3-4 ms slower than Off**. The replay is again ~27% slower on
core 2 (42 vs 33 ms). Syncs: 3 per frame (`GXPeekZ` x2 plus `rt.sync.port`,
the movie frame upload). One segment (219-266 s, 9.3 fps) had
`gx.wait_vsync` 32 ms and `split_flush` 0.06 ms: GPU overlap was off then
(probably toggled in the menu; the touch UI does not log that toggle).

### Regression in every mode since 5EB6A447 (suspect: step 1, core 1)

| | 5EB6A447 (PIPELINE_PLAN.md) | 89A1E78D, mode 0 |
|---|---|---|
| attract frame, overlap on | 57.9-62.5 ms | 76.7-78.3 ms |
| `gpu.split_flush` per call | 0.245 ms (10 per frame) | 0.75-0.86 ms (11 per frame) |
| `audio.pump` | 0.7-1.3 ms | 4.8-6.0 ms |
| attract `worker.busy_us` (one ~25 KB async read per frame) | ~30 ms | 39-52 ms |

- The attract scene is the same (333 draws) and ~17 ms slower with the
  render thread off. In the hub the spot differs, so there is no direct
  comparison, but `split_flush` and `audio.pump` alone add ~10 ms there.
- What these three have in common: each waits on a system service. Split
  flushes call GSPGPU_FlushDataCache (GSP), `pump_dma` calls
  DSP_FlushDataCache and `ndspChnWaveBufAdd` (DSP), and the worker's reads
  go to FS. Those services run on core 1. Step 1 raised the app's core 1
  share from 30% to 80% (`S3D_CORE1_LIMIT`) and moved the worker, the stream
  reader, the MusyX tick thread (priority 0x18) and the skinning helper
  there. **Hypothesis:** the higher limit and our threads delay the system
  threads on core 1, so every IPC round trip gets slower. The startup
  benchmark agrees that core 1 is slow: zlib 15.3 MB/s on core 1 against
  19.6 MB/s on core 0.
- `audio.musyx_ticks` and `audio.stream` do not appear in the hub, so the
  5 ms of `audio.pump` is in `pump_dma` (memcpy, DSP_FlushDataCache,
  `ndspChnWaveBufAdd`, the game's DMA callback) or `movie_audio_pump`.
  The MusyX ticks themselves cost ~200 us per 5 ms on core 1 (4%).

**Next: env.txt A/B runs, no rebuild.** These settings are read once at
boot, so restart the app after each change. Attract scene ~15 s, then the
same hub spot ~15 s, render thread on default (mode 2):
1. `S3D_CORE1_LIMIT=30`: the old limit with the new placement.
2. `S3D_CORE1=0`: no helpers on core 1 (the worker falls back to core 2 or
   inline, the MusyX ticks run in the ndsp callback).
3. `S3D_AUDIO_CORE1=0` with the default limit: the MusyX ticks only.
Compare `gpu.split_flush` per call, `audio.pump`, `worker.busy_us` and the
attract frame with this log. If 1 or 2 brings split flushes back to ~0.25 ms
and the attract scene to ~60 ms, lower the default limit or keep
latency-critical work off core 1.

### A/B results (same build 89A1E78D, 2026-10-14)

The log keeps only two files (`Star3D.log`, `Star3D.prev.log`), so run 1
(`S3D_CORE1_LIMIT=30`) was rotated away and run again afterwards (below).
`Star3D.prev.log` is run 2 and `Star3D.log` is run 3. Copying logs over
SFTP from the console served the previous run's file (stale, and cut short).
The right file only showed up with the SD card in the PC. Check the `env`
lines at the top of a log before analysing it. Both hub segments are at the same spot,
-5587 -781 -3293 angle 85, with ~648 draws and 72k triangles. That is about
the 5EB6A447 spot (644 draws, 72k). The render thread was on (mode 2).

| hub (ms/frame) | run 2: `S3D_CORE1=0` | run 3: `S3D_AUDIO_CORE1=0` (limit 80) |
|---|---|---|
| frame | **57.3 (17.4 fps)** | 74.0 (13.5 fps) |
| `rt.replay` (core 2) | 47.6 | 55.5 |
| `game.sceneRender` (core 0) | 24.5 | 22.5 |
| `rt.sync_wait` | 23.3 | 33.2 |
| rest of the frame (logic, audio) | ~9.5 | ~18.3 |
| `gpu.split_flush` (11 calls) | **1.46 (0.13 per call)** | 10.7 (0.97 per call) |
| `audio.pump` | **0.47** | 3.9 |
| skinning | inline, `skin.blend` 1.6 | 8 models in parallel, 1.3 |

| attract (ms/frame) | run 2 | run 3 | first log, mode 2 |
|---|---|---|---|
| frame | **68.5** | 98.5 | 80.6-98.8 |
| `gpu.split_flush` | 1.1 | 7.7 | 6.5-6.9 |
| `audio.pump` | 0.66 | 5.3 | ~5 |
| `worker.busy_us` | 46 ms (core 2) | 52 ms (core 1) | 42-52 ms |
| `stub.PADReset` (spin) | 8.8k | 14.8k | 11-14.6k |

Run 1 again (`S3D_CORE1_LIMIT=30`, everything else default, same hub spot,
644 draws): hub **60.0 ms (16.6 fps)**, attract 77.9 ms.
- `gpu.split_flush` 2.75 ms (0.25 per call, exactly 5EB6A447's value);
  `audio.pump` 0.77 ms; replay on core 2 49.7 ms; `sceneRender` 23.9 ms;
  `rt.sync_wait` 26.0 ms.
- Skinning on core 1 is slow at 30%: `helper.join_wait_us` 0.5 ms (0.14 at
  80%), `skin.blend` 2.0 ms. The boot benchmark gives core 1 zlib 5.7 MB/s
  (80%: 15.4, core 0: 19.6).
- Attract: `worker.busy_us` 39 ms on core 1 (46 ms on core 2 in run 2), but
  the frame is 9 ms slower than run 2.

Hub frame by core 1 limit (same spot, mode 2): **0% 57.3 ms, 30% 60.0 ms,
80% 74.0 ms.** Split flushes per call: 0.13, 0.25, 0.97 ms.

Conclusions:
- **The core 1 time limit causes the regression.** Moving only the MusyX
  ticks back (run 3) changes nothing. With no limit (run 2), split flushes
  cost 0.13 ms per call and `audio.pump` 0.5 ms.
- The flush cost follows the limit: 0% (run 2) 0.13 ms, 30% (5EB6A447)
  0.245 ms, 80% (first log, run 3) 0.85-0.97 ms per call. So the limit
  delays the system services on core 1 (GSP, DSP, FS) even while our core 1
  threads are idle: the kernel seems to reserve the app's share of core 1
  whether it is used or not. 5EB6A447 also paid for this, since it set 30%.
- core1_init returns before APT_SetAppCpuTimeLimit when `S3D_CORE1=0`, so
  run 2 never set a limit.
- **Core 1 is not worth using.** What it held is small: skinning 1.3 ms
  (inline 1.6), MusyX ticks ~4%. The limit costs ~10 ms per frame in GSP/DSP
  latency.
- Hub with `S3D_CORE1=0`: 57.3 ms (17.4 fps), against 65.8 ms (15.2 fps)
  for 5EB6A447 at the same spot. The render thread now gains ~8.5 ms, and
  ~9 ms more from the 30% limit that 5EB6A447 set.
- Core 2 is still the limit. The frame is ~9.5 ms of logic plus the 47.6 ms
  replay (the game thread waits 23.3 ms at the frame end).
- The attract scene improved too (68.5 ms), but it still spins on SD reads:
  the worker now shares core 2 with the render thread, at a lower priority.
- The startup benchmark: zlib 20.2 MB/s on core 2, against 19.6 on core 0
  and 15.4 on core 1. Core 2 is not slower on its own, so the replay slowdown
  in the first log came from the limit (split flushes inside the replay) and
  from contention.

**Done (2026-10-14, build 5EDB78C9, verified on hardware): `S3D_CORE1` defaults to 0**, and no
`APT_SetAppCpuTimeLimit` call at all. Worker on core 2 below the render
thread, MusyX ticks in the ndsp callback, skinning inline (a helper on
core 2 would only lengthen the limiting core). `S3D_CORE1=1` restores the
step 1 placement (with `S3D_CORE1_LIMIT`, default 80).

Hardware check of 5EDB78C9 (clean env.txt, ~15 s per area): the log shows
`core1: not used`, the worker on core 2 and skinning inline. Hub (same spot,
angle 79): **57.9 ms (17.3 fps)**. `rt.replay` 47.4, `sceneRender` 24.4,
`rt.sync_wait` 23.2, `split_flush` 1.36, `audio.pump` 0.47 ms. Attract
68.8 ms. Same as run 2. The only error is the usual boot `dvd: callback queue
full`.

### Hub with `S3D_CORE1=0`: what's next

- Core 2 replay 47.6 ms against core 0's ~34 ms (logic 9.5 + traversal and
  recording 24.5). To reach 33 ms:
  - make GX translation cheaper (DRAWCALL_PLAN.md phase 2: `gx.prep.tev`,
    `gx.draw.update`);
  - move the game-memory reads (dl lookup/validate/gather) back to core 0,
    which has ~13 ms of slack;
  - then step 4 (logic overlapping the previous frame's replay).
- A PROFILE=1 run of the replay on core 2 would show which GX zones to cut
  first. The draw-source attribution needs mode 0.
  - **Build B6C1F1B4** (2026-10-14, `docker run ... star3d-builder all
    PROFILE=1`, NO_CIA) is that build: render thread mode 2, core 1 off.
    Run: clean env.txt, the same hub spot (-5587 -781 -3293), standing still
    ~30 s. Optional: ~15 s in mode 0 for comparison.
  - The zones cost ~3 us each and now land mostly on core 2. Compare the GX
    zones with each other (`gx.prepare`, `gx.prep.*`, `gx.dl.*`, `gx.draw*`,
    `gpu.split_flush`), not with the normal build's frame.
  - out/ holds this profiling build now; rebuild without PROFILE for normal
    runs.

### Profiling run (build B6C1F1B4, PROFILE=1, core 1 off, 2026-10-14)

Log: repo root `Star3D.log`, 164 s, complete. Segments: attract in mode 2
(24-52 s), attract Off (58-71 s), hub Off (100-130 s), 1 s of mode 1, hub
mode 2 (134-160 s). Hub spot -5587 -781 -3293, angle 79 (640 draws, 72k
triangles). The profiling build runs ~7.8k zones per frame, ~1.4 us each on
hardware: the hub replay is 58.3 ms here against 47.4 ms in the normal build.
`perf.*` timers are counters in system ticks (268,112 per ms).

| hub (profiling build, ms/frame) | mode 2 | Off |
|---|---|---|
| frame | 69.3 | 79.9 |
| `game.sceneRender` (core 0) | 24.9 | 68.2 |
| `rt.replay` (core 2) | 58.3 | - |
| `rt.sync_wait` | 33.9 | - |
| `gx.displaylist` (553 calls) | 33.4 | 37.1 |
| - `gx.dl.play` (cached replay; draw 10.3 = update 4.7 + submit 2.9; gather 4.9) | 16.3 | 18.2 |
| - `gx.prepare` (tev 7.8, bindtex 3.9, fog 0.7, lightenv 0.6, proj 0.4, posmtx 0.3) | 11.8 | 13.8 |
| - `gx.dl.lookup` / `gx.dl.validate` (read game memory) | 3.7 / 1.7 | 3.7 / 1.6 |
| `gx.immediate` (464 calls) | 5.1 | 5.3 |
| `gpu.split_flush` | 1.4 | 1.3 |
| **replay outside the GX zones** (decode, ~20.8k setter commands, copies, offscreen, frame end) | **~18** | (inside sceneRender) |

Game thread in mode 2 (`perf.*`, traversal plus recording): `objRender`
12.1, `sceneDrawTransparentPolys` 7.6, `renderMapBlock` 4.3,
`Obj_UpdateAllObjects` 5.0, skinning 1.6 inline plus joint matrices 1.2,
`objShadowRender` 1.2, `drawGlow` 1.1 ms. With the render thread off the
same timers include their GX: 32.1, 20.0, 17.0 ms.

Attract scene (profiling build): mode 2 76.0 ms, Off 71.9 ms. Replay 49.2 ms:
`gx.displaylist` 24.2 + `gx.immediate` 2.0 + split flushes 1.1, so ~22 ms of
it is outside the GX zones. With the render thread off, `drawSkyStars` takes
13.4 ms (2,325 points expanded to quads per frame); in mode 2 its game side is
0.16 ms. The fur (`objRenderFuzz`) costs 8.5 ms on the game thread even in
mode 2.

What this shows:
1. **Core 2 is not slower.** The GX zones run ~10% faster on core 2 than
   inline on core 0 (`gx.displaylist` 33.4 vs 37.1, `gx.prepare` 11.8 vs
   13.8), probably because they no longer share core 0's caches with game
   code. The 21% slowdown in the first log came from the core 1 limit.
2. **The biggest bucket on core 2 has no zone: ~18 ms in the hub, ~22 ms in
   the attract scene.** That is the replay of the ~21k recorded commands
   outside display-list and immediate draws: decoding, the setters (TEV,
   matrices, texture objects, arrays) updating `g_gx`, EFB copies, offscreen
   passes and the frame end. Next instrumentation: in PROFILE builds, time
   each replayed opcode in `replay_until` (gx/gx_rec.c) and report the top
   opcodes per window (ticks and count). Two tick reads per command is fine
   in a profiling build.
3. Inside the display lists, the main costs are `gx.prep.tev` (7.8 ms, ~430
   calls per frame, ~18 us each, with only 10 cache misses), `gx.draw.update`
   (4.7), `gx.gather` (4.9), `gx.prep.bindtex` (3.9) and the game-memory
   hashing (lookup plus validate, 5.4). DRAWCALL_PLAN.md phase 2 targets TEV
   and update. Lookup/validate/gather (~10 ms) could move to core 0 (the
   "front half").
4. **Core 0 is also at the limit.** In the normal build the game thread does
   ~10 ms of logic plus 24.4 ms of traversal and recording (34.5 ms). The
   frame today is logic + max(traversal, replay) ≈ 10 + 47.4. So:
   - without step 4, 30 fps needs both traversal and replay under ~23 ms;
   - with step 4 (logic overlapping the previous frame's replay), it needs the
     replay under 33 ms and core 0 trimmed by ~2 ms. **Step 4 is required for
     30 fps** in this design.
   - So moving the front half to core 0 only pays off together with step 4
     and a cheaper recording path.
5. The attract scene is worse in mode 2 (76 vs 72 ms) because the
   unzoned replay part is large there: sky-star display lists and fur shell
   matrices. Not the target, but the per-opcode timing will show it too.

**Per-opcode replay timing (build 577C261A, PROFILE=1, 2026-10-14).** In
PROFILE builds `replay_until` (gx/gx_rec.c) reads the tick once per command
and charges the time since the previous read to that command's opcode.
Nested zones count inside their opcode, so `GXCallDisplayList` contains
`gx.displaylist`. Every ~3 s at the frame end it logs `rtops` lines: the
total (it should equal `rt.replay`) and the top 20 opcodes with ms/frame,
calls/frame and us per call. Modes 1 and 2 only. The tick read itself costs
something per command, so for the cheap setters compare call counts as
well as time.
- Azahar check (attract scene, mode 2): 89.48 ms/frame total against
  `rt.replay` 89.49, 17.9k commands per frame. Top: `GXCallDisplayList`
  35.6 ms (285 calls); `GXLoadPosMtxImm`, `GXLoadTexMtxImm` and
  `GXLoadNrmMtxImm` each ~2,900 per frame (7-9 ms each in Azahar, probably
  the fur shells); `GXSetProjection` and `GXSetTevColor` ~15 us each; the
  TEV setters ~520 per frame each. Azahar's tick read costs ~1.4 us, so its
  per-call times are inflated.
- Hardware test: hub at the usual spot, ~30 s in mode 2, then ~20 s in
  mode 1; the attract scene ~20 s in mode 2. Clean env.txt.

**Per-opcode results (577C261A on hardware, 2026-10-14).** Log `Star3D.log`
(137 s): attract 20-40 s in mode 2, hub mode 2 74-105 s, hub mode 1
108-134 s. Aggregated with `tmp_scripts/rtops.py Star3D.log t0:t1 ...`.
The per-command tick read adds ~12 ms per frame on core 2 (~0.6 us per
command): hub replay 70.7 ms here against 58.3 ms in B6C1F1B4.

| hub, per frame | mode 2 (core 2) | mode 1 (core 0) |
|---|---|---|
| replay total / commands | 70.7 ms / 20.7k | 62.5 ms / 20.4k |
| `GXCallDisplayList` (556 calls) | 33.6 ms (60 us each) | 36.3 ms (66 us) |
| immediate-mode data (~400 chunks) | 5.6 | 5.6 |
| matrices: PosMtx 1.6k, TexMtx 850, NrmMtx 700 | 6.1 | 3.3 |
| TEV setters (Order, ColorIn/Op, AlphaIn/Op, SwapMode, Direct: ~840 each) | 7.1 | 4.9 |
| `GXSetVtxDesc` 1.6k, `GXLoadTexObj` 700, `GXSetChanCtrl` 670, `GXSetTexCoordGen2` 640, `GXSetArray` 550, `GXSetCullMode` 570 | 7.7 | ~4.5 |
| `GXSetProjection` (190, ~6 us each) / `GXCopyTex` (10, ~130 us) | 1.2 / 1.4 | 1.1 / 1.2 |

- **Every small command costs about 3x more on core 2.** Small setters take
  ~1.2 us each on core 2 against ~0.8 us on core 0, and ~0.6 us of that is
  the tick read. So it is roughly 0.6 against 0.2 us of real work, ~8 ms per
  frame over ~19.5k commands. The display lists are faster on core 2. The
  difference is per command, not per unit of work. That points at memory
  shared between the cores:
  - the ring lines, written on core 0 and read on core 2;
  - the ring's control variables. The map file showed `s_r` (the read index,
    written by the render thread every 8 commands and read by the game thread
    on every command) on one 32-byte line with `s_prod_waiting` (read by the
    render thread on every command). The published index `s_wpub` shared a
    line with `s_w`, which the game thread writes on every command.
- Setters that change state flush the pending immediate-mode batch
  (`gx_state_changed` → `gx_render_flush`). Part of the immediate-mode cost
  is therefore charged to the next setter. `GXSetProjection` (6 us) is
  mostly that.
- Attract: the fur shells load ~2.9k PosMtx, TexMtx and NrmMtx each per frame
  (15.8 ms of the profiled replay), and `GXSetTevColor` costs ~8 us each
  (224 per frame).

**Fix: build 36F640FC** (normal build, 2026-10-14, Azahar OK):
- The read index (`s_read.idx`) and the published index (`s_pub.idx`) each
  sit on their own 32-byte line. The map file confirms this: 0x011a7520 and
  0x011a7540.
- The game thread works from a cached copy of the read index
  (`s_read_seen`). It reads the shared one only when the cached value says
  the ring is full. A stale value only underestimates the free space.
- The render thread checks `s_prod_waiting` only at its release points
  (every 8 commands), not on every command.
- It prefetches the ring 128 bytes ahead (`__builtin_prefetch`).
  `S3D_RT_PREFETCH=0` turns this off.
- Not changed yet: the 2 MB ring (`S3D_RT_RING_KB`) is as large as the
  shared L2 and streams ~400 KB per frame through it. 256 KB would be enough
  (the game thread then waits for space earlier, but it waits at the frame
  end anyway).

Hardware A/B (hub same spot ~20 s, attract ~20 s, restart between runs,
compare with 5EDB78C9: hub 57.9 ms, attract 68.8 ms):
A. clean env.txt;
B. `S3D_RT_RING_KB=256`;
C. `S3D_RT_PREFETCH=0` (only if A or B improved, to see which part helped).

After that (written before the A/B runs):
- Measure the cross-core slowdown (point 3 in the hub list): a core 2 CPU
  benchmark at boot, and a run with `S3D_HELPER=0` to rule out the
  allocator lock.
- Make GX translation cheaper on the render thread: the split flushes
  (9 ms), DRAWCALL_PLAN.md phase 2 (`gx.prep.tev`, `gx.draw.update`).
- Balance the cores: core 0 has ~45 ms of slack in mode 2. Moving the
  game-memory reads (dl lookup/validate/gather, ~11 ms profiled) back to
  core 0 (the "front half" from the survey) would shorten core 2's path.
- Step 4 (logic overlapping the previous frame's replay) only helps once
  core 2 is shorter than core 0's whole frame.

### Hardware A/B results (build 36F640FC, New 3DS XL, 2026-10-14)

Logs (repo root, both complete, build id and env lines checked):
- `Star3D.prev.log` = A, clean env.txt (ring 2048 KB, prefetch on);
- `Star3D.log` = B, `S3D_RT_RING_KB=256`.

Each run: attract scene with the movie (map 63) ~25 s, then the hub
(map 7) ~30 s at the usual spot (650 draws, 72k triangles, same as before).
Run C (`S3D_RT_PREFETCH=0`) was not done. Segments aggregated with
`tmp_scripts/seg_prof.py` (A: 17:41 and 63:96; B: 17:48 and 68:97). No new
errors or warnings.

| ms/frame | 5EDB78C9 (before) | A: 36F640FC clean | B: 36F640FC, ring 256 KB |
|---|---|---|---|
| **hub frame** | 57.9 (17.3 fps) | **53.3 (18.7 fps)** | **52.5 (19.1 fps)** |
| hub `rt.replay` (core 2) | 47.4 | 43.6 | 42.8 |
| hub `game.sceneRender` (core 0) | 24.4 | 25.0 | 24.5 |
| hub `rt.sync_wait` / `rt.space_wait` | 23.2 / - | 18.8 / - | 17.3 / 1.6 |
| **attract frame** | 68.8 | **61.2** | **58.6** |
| attract `rt.replay` | - | 36.3 | 34.0 |
| attract `rt.sync_wait` / `rt.space_wait` | - | 21.4 / - | 6.2 / 13.1 |
| attract `game.sceneRender` | - | 13.9 | 26.6 (holds the space wait) |

`split_flush` (1.4 hub / 1.1 attract), `audio.pump` (0.4 / 0.6) and
`skin.blend` (1.7 hub) are unchanged. Command counts are unchanged (hub
21.4k, attract 17.9k per frame), so the gain is cheaper commands, not fewer.

What this shows:
1. **The ring layout fix works.** The hub replay on core 2 dropped 3.8 ms
   (47.4 → 43.6, -8%), and the hub frame 4.6 ms. The attract scene gained
   7.6 ms. The false-sharing explanation from the per-opcode run was right
   (at least partly; run C would split the index layout from the prefetch).
2. **The smaller ring is faster again**: hub replay -0.8 ms, attract replay
   -2.3 ms. That fits the L2 explanation: a 2 MB ring is the size of the
   shared L2 and evicts everything else; 256 KB stays in L2. (A 2048 KB ring
   streams ~400 KB per frame through fresh lines; with 256 KB the same lines
   are reused every ~half frame.)
3. With 256 KB the game thread waits for ring space (attract 13.1 ms, hub
   1.6 ms) instead of at the frame-end sync. That costs nothing: core 0
   would wait for core 2 anyway, and the total wait went down (attract 21.4
   → 19.3 ms). But the space wait is fine-grained: **~17k `rt.space_wait`
   calls per frame in the attract scene** (~0.77 us each). Each one is a
   `wait_progress` sleep, and core 2 then calls `LightEvent_Signal` at every
   release point (every 8 commands) while `s_prod_waiting` is set. So core 2
   pays for a wakeup syscall roughly every 8 commands during those stretches.
   Fix (not done): hysteresis. When the ring is full, the game thread waits
   until a large block is free (e.g. 1/4 of the ring or 64 KB), and the
   render thread signals only once it has freed that much. That should cut
   the wakeups to a few per frame and may give back part of the attract
   replay time.
4. **Core 2 is still the limit in the hub.** Frame ≈ logic (~9.6) + max
   (traversal 24.5, replay 42.8) = 52.4 ms, which matches the measured
   52.5. Core 0 waits 17-19 ms per frame. 30 fps still needs the replay
   under ~23 ms without step 4, or under ~33 ms with step 4.
5. The fps gain so far from the multicore work, hub at this spot: 65.8 ms
   (5EB6A447, single GX thread) → 52.5 ms (-20%).

**Build 802F1335** (normal build, NO_CIA, 2026-10-14) does the first two
next steps:
- The default ring is 256 KB (`S3D_RT_RING_KB`, gx/gx_rec.c `alloc_ring`).
- Block wakeups for the game thread (`wait_progress(from, words)`):
  - When the ring is full, the game thread now sleeps until the render
    thread has freed a whole block: `S3D_RT_WAKE_KB`, default 64, capped at
    1/4 of the ring. 0 brings back the old behaviour (wake at every release
    point). The render thread signals only once it has replayed that many
    words past the wait start (`s_wake_from` / `s_wake_words`, checked at
    its release points), or when it reaches the published end.
  - Syncs and the frame-end wait (`sync_all`) now wake only when the replay
    reaches the end, not every 8 commands. Before, a 17-19 ms sync wait in
    the hub also cost core 2 a signal every 8 commands.
  - The 2 ms wait timeout is still there as a fallback.
- Azahar (attract scene, 60 s): renders normally, no new errors. 
  `rt.space_wait` is down to ~4 calls per frame; on hardware with 36F640FC
  and the 256 KB ring it was ~17k.

Hardware test for 802F1335 (clean env.txt; the same attract ~20 s and hub
spot ~30 s; compare with run B: hub 52.5 ms, attract 58.6 ms, replay 42.8 /
34.0):
- what to read: frame, `rt.replay`, `rt.space_wait` calls, `rt.sync_wait`;
- optional second run: `S3D_RT_WAKE_KB=0` (wakeup change off, 256 KB ring),
  to split the gain from the wakeups out of the ring size;
- optional: `S3D_RT_PREFETCH=0` (run C) and ring sizes 128 / 512 KB.

Then the bigger items in the list above: cheaper GX translation on core 2
(split flushes, DRAWCALL_PLAN.md phase 2: `gx.prep.tev`, `gx.draw.update`),
moving the game-memory reads (dl lookup/validate/gather, ~10 ms) to core 0,
then step 4.

### Hardware results (build 802F1335, New 3DS XL, 2026-10-15)

Logs (repo root, both complete, build id 802F1335, env lines checked):
- `Star3D.prev.log` = D, clean env.txt (ring 256 KB, wake block 64 KB);
- `Star3D.log` = E, `S3D_RT_WAKE_KB=0` (old wake-per-release-point space
  wait; the sync-wait change is not behind this switch).

Same route as before: attract scene with the movie (map 63) ~25 s, then the
hub (map 7) ~30 s at the usual spot (650 draws, 72k triangles, 21.5k
commands per frame, unchanged). Segments with `tmp_scripts/seg_prof.py`
(D: 17.5:38.5 and 57.8:90.2; E: 17.3:36.6 and 58.7:87.1).

| ms/frame | B: 36F640FC, ring 256 KB | D: 802F1335 clean | E: 802F1335, WAKE_KB=0 |
|---|---|---|---|
| **hub frame** | 52.5 (19.1 fps) | **50.1 (20.0 fps)** | **50.4 (19.8 fps)** |
| hub `rt.replay` (core 2) | 42.8 | 40.4 | 40.2 |
| hub `game.sceneRender` (core 0) | 24.5 | 26.9 | 27.5 (one hitch, see below) |
| hub `rt.sync_wait` / `rt.space_wait` | 17.3 / 1.6 | 13.6 / 3.1 | 13.4 / 3.1 |
| **attract frame** | 58.6 | **56.6** | **56.5** |
| attract `rt.replay` | 34.0 | 32.3 | 32.3 |
| attract `rt.sync_wait` / `rt.space_wait` | 6.2 / 13.1 | 4.2 / 13.4 | 4.2 / 13.4 |

`split_flush` (1.4 hub / 1.35 attract), `audio.pump` (0.4 / 0.6) and
`skin.blend` (1.6 hub) are unchanged. No new errors or warnings (the boot
`dvd: callback queue full` once, as before).

What this shows:
1. **802F1335 is ~2 ms faster than run B in both scenes**: hub 52.5 → 50.1
   ms (first steady 20 fps), attract 58.6 → 56.6 ms. All of it is core 2:
   hub replay -2.4 ms, attract replay -1.7 ms.
2. **`S3D_RT_WAKE_KB` makes no difference** (D and E are within 0.3 ms on
   every zone). So the gain does not come from the block wakeups on a full
   ring. What is left between B and D/E: the 256 KB ring is now the default
   instead of set by env (same size, so no effect expected), and the
   sync/frame-end waits that now wake only at the end of the replay instead
   of every 8 commands. The hub has a 13-17 ms sync wait per frame, so that
   change removed ~1k `LightEvent_Signal` calls per frame from core 2.
   That fits the hub gain, but run-to-run variance between different days
   can't be ruled out from these two runs. Since E still signals every 8
   commands during the attract scene's 13.4 ms space wait and is as fast as
   D, a signal is cheap, so the sync-wait change is not a sure explanation
   either. To settle it: one more run of D on the same day, or a switch for
   the sync-wait wakeups.
3. **The ~17k wakeups per frame seen with 36F640FC were real but cheap.**
   The zone counts differ only because of where it sits: in 36F640FC it was
   entered once per sleep, and since 802F1335 it covers the whole
   wait-for-space loop, so it counts ring-full episodes (~3 per frame in
   both D and E). Run E still does the per-release-point wakeups (not
   counted any more) and is as fast as D. The block-wakeup change is
   harmless but bought nothing measurable. Keep the default of 64 KB (fewer
   wakeups, same speed).
4. **Core 2 is still the limit in the hub.** Frame ≈ logic (~9.7) + replay
   (40.4) = 50.1 ms. Core 0 waits 16.7 ms per frame (sync 13.6 + space 3.1).
   For 30 fps the replay has to drop by another ~17 ms (to ~23 ms) without
   step 4.
5. Attract scene: core 0 spends 13.4 ms per frame waiting for ring space and
   4.2 ms at the frame-end sync. The I/O spin from "Attract scene I/O spin"
   is still there (~8k `padUpdate` per frame, `worker.busy_us` 42 ms per
   frame for ~25 KB of async reads).
6. One-off hitch in run E: a 355 ms frame inside `game.sceneRender` (core 0)
   around f1150 in the hub, while standing still. No ring wait involved
   (`rt.space_wait` max 3.8 ms, `rt.sync_wait` max 14.5 ms) and no log line
   in that window. Not seen in run D. Watch for it in later runs; if it
   repeats, it needs a zone below `game.sceneRender` to locate.

Multicore gain so far, hub at this spot: 65.8 ms (5EB6A447) → 50.1 ms
(-24%, 15.2 → 20.0 fps).

Next: the bigger items above. The ring and wakeup tuning is done; what is
left on core 2 is the cost of the GX translation itself (DRAWCALL_PLAN.md
phase 2, split flushes), and moving the dl lookup/validate/gather (~10 ms)
to core 0, which has ~17 ms of idle time per frame in the hub.

## Step 4: overlapped frame end (build C2546C07, 2026-10-15)

**Why this before the dl front half.** Hub frame today = logic 9.7 +
max(traversal ~24, replay 40.4) = 50.1 ms: core 2 sits idle during the
logic part and core 0 sits idle 13.6 ms at the frame-end sync. Letting the
render thread finish frame N while the game thread runs frame N+1's logic
makes it ≈ max(logic + traversal ≈ 37, replay ≈ 40) ≈ 40 ms (~25 fps),
about 10 ms. Moving lookup/validate (~4.5 ms in a normal build) to core 0
would give ~4.5 ms at most, and adds that time to core 0. With the overlap
in place, core 0 and core 2 are close to balanced, and the next gains come
from making the replay cheaper (or moving part of it to core 0).

**What changed** (S3D_RT_OVERLAP, default 1, mode 2 only; 0 = the 802F1335
behaviour, switchable at runtime):
- The frame end no longer drains the ring. `VIWaitForRetrace` records the
  frame boundary as one command (`GXREC_OP_FRAME`, args: VISetBlack, the
  save-state cache reset flag, the 3D slider value). The render thread runs
  it (`port_frame_boundary`, vi_3ds.c): touch UI draw, `gx_frame_end`
  (C3D_FrameEnd), `gx_frame_begin_slider` (VBlank wait), cache reset. The
  game thread goes straight on to input, the touch UI logic and the next
  frame's logic.
- **Render fence** (`gxrec_fence`, zone `rt.fence_wait`): waits until the
  render thread has run every recorded boundary. Called:
  - at `sceneRender` entry (port/frame_zones.c), before the game rewrites
    in place what the previous frame's draws read (skinned normals, sky
    stars, splash colours, distortion / shadow textures are all written
    during the rendering pass);
  - from `mm_free` (new `--wrap`) when the game's free delay is 0 (attract
    mode, map unloads, `mmSetFreeDelay(0)` sections). With the usual
    2-tick delay a free outlives one frame of lag, so those don't wait;
  - at the start of the next `VIWaitForRetrace`, so the touch UI input never
    runs while the render thread draws the menu, and the lag never exceeds
    one frame.
  - Full drains (direct sections, syncs, mode switches) cover everything as
    before.
- Late reads are GameCube-like now: the render thread reads game memory up
  to one frame late, which is what the GameCube GPU does (it may still draw
  frame N during frame N+1's logic; the game protects GPU data with its
  2-tick free delay).
- Recorder answers that depended on render-side frame state now come from
  the boundary arguments: `gx_hud_begin` (frame open), `gx_offscreen_begin`
  (`gx_offscreen_check(..., stereo)`), `port_stereo_active/_reduced`
  (stereo from the slider value the boundary passes on). `gx_hud_can_begin`
  is gone.
- `GXPeekZ` is no longer a sync call (it drained the ring at every frame
  end from the breakpoint callback, which would have cancelled the
  overlap). It reads the finished depth copies under a lock (gx_copy.c), and
  each copy keeps its own frame's EFB-to-screen mapping.
- The profiler report resets every thread's counters: when a report is due
  (`prof_report_due`, every ~3 s) the frame end drains first
  (`gxrec_report_drain`), so one frame in ~55 doesn't overlap.
- APT: an `aptHook` drains the ring on suspend / sleep / exit, before
  citro3d's hook, since `aptMainLoop` now runs while a frame may still be
  replaying.
- config.c: new switches are added under a lock (both threads look them up,
  and the touch UI now sets them while the render thread runs).
- Not overlapped yet: the attract scene's movie upload (`THPVideoDecode`)
  is a direct section and drains every frame (`rt.sync.port` 1/frame), so
  the hub video gains little. Recording the upload would fix that.

**Azahar (2026-10-15, C2546C07):** attract 70 s and hub ~5 min, no errors,
no crash. Rendering identical with overlap on and off (same spot,
screenshots `build/emu/ov_on.png` / `ov_off.png`), touch menu and bottom
HUD fine. Runtime switches all clean: S3D_RT_OVERLAP 0 → 1, render thread
2 → 1 → 2 → 0 → 2, then a save state save + load (cache reset now runs on
the render thread: "GX caches rebuilt"). `rt.fence_wait` 13.3 ms/frame in
the hub (core 0 waiting for the previous frame at sceneRender, i.e. the
overlap happens). Azahar hub frame: overlap on 52.6 ms, off 57.5 ms (Azahar
timing is not hardware timing). Not testable in Azahar: HOME menu / sleep
during gameplay (the APT hook).

**Hardware test for C2546C07** (compare with 802F1335: hub 50.1 ms, replay
40.4, attract 56.6):
1. Clean env.txt: attract ~20 s, hub usual spot ~30 s. Read: frame,
   `rt.fence_wait`, `rt.replay`, `game.sceneRender`, `rt.space_wait`.
   Expected hub ~40-43 ms if core 2 is the limit.
2. In the hub, press HOME, wait, resume; close the lid (sleep), open it.
   Then quit with START+SELECT. Anything odd (hang, black screen, crash)
   points at the APT hook.
3. Optional: an `S3D_RT_OVERLAP=0` run (env.txt) for an A/B on the same
   day. There is no touch menu entry for it.

### Hardware results (build C2546C07, New 3DS XL, 2026-10-15)

Logs: `Star3D.prev.log` = overlap on (clean env), `Star3D.log` =
`S3D_RT_OVERLAP=0`. Same route (attract ~25 s, hub usual spot ~30 s, 650
draws, 21.5k commands). Segments: on 19:36 / 59:88.5, off 18.9:37.8 / 59.9:85.4.

| ms/frame | overlap off | overlap on |
|---|---|---|
| **hub frame** | 50.8 | **49.6** |
| hub `rt.replay` (core 2, on: including the frame boundary) | 40.5 | **48.5** |
| hub `game.sceneRender` (includes space waits) | 26.9 | 30.3 |
| hub `rt.space_wait` / sync or fence wait | 3.1 / 13.1 | 6.4 / 8.2 |
| hub `gpu.draw_us` | 28.9 | 34.9 |
| **attract frame** | 56.8 | 57.3 |

- **The overlap gained only ~1 ms in the hub** (expected ~10). Core 0 got
  its time back, but core 2's replay grew by 8 ms with the same commands,
  and core 2 is the limit. VBlank / GPU-wait zones are ~0, so it is not a
  wait in the boundary. `game.sceneRender` grew exactly by its extra space
  wait (+3.3), so core 0's traversal itself did not slow down.
- Two explanations, not separable from this log: (a) the boundary work
  (touch UI draw, C3D_FrameEnd with the per-frame cache flush, frame begin)
  now costs core 2 several ms; (b) memory contention: the game logic now
  runs at the same time as the replay (before, only the traversal did).
  Azahar shows the boundary at ~1.7 ms, but Azahar timing says little.
- Attract: no change, as expected (the movie upload drains every frame).
- **Crash at quit** (START+SELECT) with overlap on: citro3d `svcBreak` in
  `C3D_RenderTargetDelete` from `gx_deinit`. The overlapped frame end
  records the boundary after input, so at exit the frame was still open
  (citro3d panics on target deletion inside a frame). The other stack words
  were stale. Fixed in F767D896: `port_shutdown_and_exit` closes the frame
  (`gx_frame_end`, a no-op when no frame is open) before `gx_deinit`.
- HOME / sleep: nothing in the log (APT events were not logged).

**Build F767D896** (2026-10-15): the exit fix, plus measurements:
- zones `frame.boundary` (overlap only), `frame.touchui`, `frame.end`,
  `frame.begin` on whichever thread runs them (comparable on and off);
- counters `game.logic_us` (end of VIWaitForRetrace to sceneRender, without
  the fence) and `game.post_us` (sceneRender to the next VIWaitForRetrace);
- log line `rt: APT suspend/sleep/exit: render thread drained`.
Azahar hub: boundary 1.7 ms (touchui 0.27, end 0.47, begin 0.9), logic
4.1 ms, post 2.0 ms, no errors.

Next hardware test: the same two runs (on, then `S3D_RT_OVERLAP=0`); in the
"on" run also press HOME / resume and close / open the lid in the hub, then
quit with START+SELECT. Decision: if `frame.boundary` is large on core 2,
move the boundary work back to core 0 (record only the GPU part); if it is
small, the replay is losing to memory contention and the next step is
cutting its memory traffic (dl gather / copies) instead.

### Hardware results (build F767D896, New 3DS XL, 2026-10-15)

Logs: `Star3D.prev.log` = overlap on, `Star3D.log` = `S3D_RT_OVERLAP=0`.
Hub segments 45.5:58.6 (on) and 42.7:58.3 (off), same spot.

| hub, ms/frame | overlap off | overlap on |
|---|---|---|
| **frame** | 50.7 | **48.5 (20.6 fps)** |
| `rt.replay` (core 2) | 40.3 | 47.2 |
| of it `frame.boundary` (touchui 0.17, end 1.0, begin 0.5) | - | 1.7 |
| replay without the boundary | 40.3 | **45.6** |
| `game.logic_us` (core 0, before sceneRender) | 6.2 | **8.4** |
| `game.post_us` | 1.1 | 1.1 |
| `game.sceneRender` (incl. space waits 3.0 / 6.0) | 26.8 | 29.9 |
| `gpu.draw_us` | 29.1 | 32.3 |

- **The cost is memory contention, not the moved boundary.** The boundary
  costs core 2 only 1.7 ms. But while the game logic runs at the same time,
  the replay of the same commands takes 5.3 ms longer, and the logic itself
  2.2 ms longer. Before step 4 the replay only overlapped with the
  traversal. The overlap is still a net gain of ~2.2 ms (50.7 → 48.5).
- (`frame.touchui` is 1.1 ms on core 0 but 0.17 ms on core 2.)
- **HOME then lid, overlap on: hang.** The suspend hook drained the ring
  (`APT suspend: render thread drained`, 70.4 s); after resume (74.3 s) the
  render thread stuck forever in `gx_gpu_wait_previous` (touch UI draw,
  previous GPU frame never done) and the game thread in the fence. So it
  was the HOME resume, seen at the lid. Cause: with the overlap the frame is
  still open in citro3d when `aptMainLoop` suspends (the boundary is
  recorded after it); the non-overlapped path always ends the frame first.
  Overlap off: HOME and lid fine. START+SELECT exit fine in both (the
  F767D896 fix).

**Build DAC1E1FE** (2026-10-15): the APT hook also ends the open frame
after draining (suspend, sleep, exit); the recorded boundary then only
begins the next frame. If the hook runs on another thread it now logs
`APT hook N on another thread` (libctru may call the sleep hook from its
event thread; then this fix would not cover the lid). Azahar attract
regression run OK (HOME/lid not testable there).

Next HW test: overlap on only — hub, HOME and resume, play a few seconds,
lid close / open, play, START+SELECT. Check the log for the `rt: APT ...`
lines.

What next for speed: the replay is memory-bound when both cores run.
Candidates that cut its memory traffic: the dl gather / incremental copies
(`dl.inc_copied_vertices` 36k per frame), the per-command ring traffic,
and the 256 KB ring itself (try 128 KB on hardware, `S3D_RT_RING_KB=128`).

### Hardware results (build DAC1E1FE, New 3DS XL, 2026-10-15)

Overlap on: HOME + resume, lid close / open all fine (`APT suspend` at
50.4 s and `APT sleep` at 61.6 s, both on the game thread, so the hook
covers sleep too). **START+SELECT crashed the console**, after the log had
closed normally (`shutting down` → `3DS graphics deinitialized` → `log
closed`, 50 ms): so the crash is in `gfxExit` / `exit()` / libctru's exit
handlers, not in our shutdown code. Overlap-off exits (four runs, two of
them after HOME and lid) never crashed. Not known yet: whether it needs the
HOME/lid sequence first, and the crash address (Luma dump).
Luma dump (possibly older, file dated Sep 27): PC FFF23584 in SVC mode, a
kernel fault; tracked as OPEN_BUGS.md bug 20, deferred.

### Contention profile (build A967EEC9, PROFILE=1, 2026-10-15)

Question: with the overlap, the hub replay on core 2 takes 5.3 ms longer and
the game logic 2.2 ms longer for the same work (F767D896). Which parts of the
replay and of the logic slow down when the two cores run at the same time?
The answer picks between DRAWCALL_PLAN.md phase 2 (per-draw state setup) and
moving the dl lookup/validate to core 0.

Profiling build: GX zones, game function timers (`perf.*`) and per-opcode
replay timing (`rtops`, `tmp_scripts/rtops.py`). The profiling overhead
(~7.8k zones and ~21k tick reads per frame) is the same in both runs, so
compare the runs with each other, not with normal builds. Azahar: rtops
total = `rt.replay` with the overlap on.

Hardware runs (same route as always: attract ~20 s, hub usual spot ~30 s
standing still):
A. clean env.txt (overlap on);
B. `S3D_RT_OVERLAP=0`.
Then copy both logs (the game keeps only two).

What to read: per zone and per opcode, B → A. Zones that grow under overlap
are memory-bound (they compete with core 0); zones that stay the same are
compute-bound. `perf.*` on core 0 shows which game functions slow down.
- Growth mostly in `gx.dl.*` / gather / copies (game-memory reads and
  vertex copies) → cut those or move them to core 0.
- Growth mostly in `gx.prepare` / `gx.draw.update` / setters → phase 2
  (thin state emitter) also helps the contention.

Exit bug 20 (crash after the log closes, overlap on) may still happen when
quitting; the log is complete by then.

### Contention profile results (A967EEC9, New 3DS XL, 2026-10-15)

Logs: `Star3D.prev.log` = A (overlap on), `Star3D.log` = B
(`S3D_RT_OVERLAP=0`), both complete. Hub segments A 64.3:94.3, B
66.9:89.95 (same spot). Compared with `tmp_scripts/cmp_seg.py` and
`tmp_scripts/rtops.py`.

| hub (profiling build), ms/frame | B: off | A: on | A-B |
|---|---|---|---|
| frame | 67.7 | **62.6** | -5.1 |
| `rt.replay` (core 2) | 57.0 | 61.2 | +4.1 |
| of it `frame.boundary` (on core 2 only in A) | - | 2.6 | +2.6 |
| `gx.displaylist` | 32.8 | 33.8 | +1.0 |
| `gx.dl.play` (gather 4.7 → 5.0) | 15.9 | 16.5 | +0.6 |
| `gx.prepare` (tev 7.8, bindtex 3.9) | 11.8 | 12.1 | +0.2 |
| `gx.draw` (update 4.7, submit 2.9) | 10.2 | 10.4 | +0.1 |
| `gx.dl.lookup` + `validate` | 5.2 | 5.5 | +0.2 |
| `gx.immediate` | 5.0 | 5.2 | +0.2 |
| game: `Obj_UpdateAllObjects` / `drawGlow` / transparent polys | 5.1 / 2.9 / 11.7 | 6.1 / 4.1 / 13.3 | +1.1 / +1.2 / +1.6 |

- **The contention has no single hotspot.** Without the boundary, the
  replay grows by ~1.5 ms, spread over every zone by 2-7% (gather the most,
  +7%). On core 0 the logic grows by 1.2 ms and the transparent pass and
  glow by ~1.5 ms each. The profiling build is less memory-bound than the
  normal one (its zones add compute), so the effect is smaller here than the
  5.3 ms in F767D896, but it is just as evenly spread. Cutting any one area's
  memory traffic does not fix it: the way forward is less total work on core
  2.
- The boundary costs 2.6 ms on core 2 in this build (1.7 in the normal build).
- **Per-opcode (B, rtops):** small setters now cost ~0.67 us each including
  the ~0.6 us tick read, so they are nearly free since the 36F640FC ring fix.
  `GXCallDisplayList` dominates: 557 calls x 60 us = 33.4 ms. Then
  immediate-mode data 5.6 ms (481 chunks), `GXLoadPosMtxImm` 1.8 (1.6k),
  `GXCopyTex` 1.35 (10 x 135 us), `GXSetProjection` 0.9 (191 x 4.9 us, mostly
  the immediate-mode flush it triggers).
- Per display-list call (~60 us): prepare ~21 (TEV + texture bind ~14),
  draw ~18 (citro3d state update ~8.5, submit ~5), gather ~9, lookup +
  validate ~10 (hashing game memory).

What this means for the next step (core 2 = the limit, ~45 ms contended in
the normal build):
1. DRAWCALL_PLAN.md phase 2 targets prepare + draw.update, ~17 ms profiled
   (~12-14 normal). Biggest pool.
2. Moving the game-memory hashing (lookup list hash + validate, ~5 ms
   profiled) to core 0 at record time. Core 0 idles 15 ms per frame at the
   fence, so it absorbs that. The cache table lookup and the state hash stay
   on core 2.

### Game-memory hashing on core 0 (build 654B271E, 2026-10-15)

The display list cache's game-memory hashes move from the render thread
(core 2) to the game thread (core 0), which idles ~15 ms per frame at the
fence:
- A recorded `GXCallDisplayList` becomes `gx_dl_call_hashed(list, nbytes,
  mask, hashes)` (gxrec.list, manual). The game thread computes the list hash
  (`gx_dl_list_hash`, same 32-sample hash as before) and, for every array
  its vertex descriptors make indexed (POS..TEX7, `GX_INDEX8/16`), a keyed
  hash `gx_dl_array_key(content, data, size)` with a per-frame memo
  (`rec_array_hash`, like the render side's). Zone `rt.dl_prehash` in
  PROFILE builds.
- The recorder keeps its own copy of the vertex descriptors and arrays:
  `GXSetVtxDesc`, `GXClearVtxDesc` and `GXSetArray` became manual wrappers
  (gx_rec.c), refreshed from `g_gx` whenever the ring is idle
  (`refresh_shadows`).
- Render side (gx_dlcache.c): `GXCallDisplayList` takes the list hash from
  the command, and `arrays_unchanged` compares the stored entry's key with
  the precomputed one. If they differ (a display list changed its own
  descriptors/arrays, so the recorder's view was stale, or the content really
  changed), it hashes on core 2 as before (`dl.prehash_rechecks`). The vertex
  state hash, unit map hash and table lookup stay on core 2 (they depend on
  render-side state).
- Hashing at record time also means the cache sees game memory as it was
  when the game made the call, which is the GameCube's semantics.

Azahar (hub ~3 min): 552/552 display list calls per frame prehashed, all
hits, `dl.prehash_rechecks` 0 for the whole run, misses only at load as
before; rendering identical; switches render thread 2 → 1 → 0 → 2 and
overlap 0/1 fine. Azahar replay 50.3 → 48.5 ms.

Hardware test (compare with F767D896 overlap on: hub 48.5 ms, replay 47.2,
`gx.dl.lookup`/`validate` not zoned in normal builds):
A. clean env.txt: attract ~20 s, hub usual spot ~30 s;
B. `S3D_RT_RING_KB=128` (the ring-size test), same route.
Read: frame, `rt.replay`, `rt.fence_wait`, `game.sceneRender`,
`dl.prehash_rechecks` (should stay ~0).

### Hardware results (build 654B271E, New 3DS XL, 2026-10-15)

Logs (kept in `perf/hwlogs/`): `654B271E_clean.log` = A (clean env),
`654B271E_ring128.log` = B (`S3D_RT_RING_KB=128`). Hub segments A 69.1:95.1,
B 63.8:92.2. All 556 display list calls per frame prehashed, all hits,
`dl.prehash_rechecks` never logged (0).

| hub, ms/frame | F767D896 (on) | A: 654B271E | B: ring 128 KB |
|---|---|---|---|
| **frame** | 48.5 | 50.1 | **49.3** |
| `rt.replay` | 47.2 | 48.7 | 47.9 |
| of it `frame.boundary` | 1.7 | **4.3** | 4.6 |
| replay without the boundary | 45.6 | **44.4** | 43.3 |
| `frame.touchui` / of it `gx.wait_gpu_prev` | 0.17 / ~0 | **2.7 / 1.7** | 2.9 / 1.7 |
| `game.logic_us` | 8.4 | 8.4 | 7.7 |
| `rt.fence_wait` / `rt.space_wait` | 7.7 / 6.0 | 6.4 / 4.2 | 2.5 / 8.1 |
| `gpu.draw_us` | 32.3 | 37.8 | 36.6 |

- **The hashing move worked on core 2** (replay work -1.2 ms, -2.3 with the
  128 KB ring), but the frame did not get faster: the render thread now
  reaches the frame boundary while the GPU is still on the previous frame.
  The touch UI draw waits for it (`gx_ui_begin` → `gx_gpu_wait_previous`,
  1.7 ms per frame); without that wait, `C3D_FrameEnd` would wait instead.
- So **the GPU is now at or near the limit** at this spot, ~48-50 ms per
  frame. `gpu.draw_us` cannot show it: pipelined, it runs from the frame end
  to the queue running dry, which includes waiting for the next frame's
  mid-frame submits. Until the GPU's own time is known, more core 2 work
  cuts may not show up in the frame time.
- The 128 KB ring is ~0.8 ms faster than 256 KB (third size cut that helped).

**Build D9596AC6** (2026-10-15):
- GPU busy time from GSP's own performance log (`GSPGPU_SetPerfLogMode`,
  `GSPGPU_GetPerfLog`, read once per frame in `gx_frame_end`): counters
  `gpu.p3d_us` (command lists: vertex + pixel work), `gpu.ppf_us` (display
  transfers, texture copies), `gpu.psc_us` (memory fills). GSP times each
  PICA operation from start to interrupt; the queue runs them one at a time,
  so frame time minus their sum is GPU idle time. `S3D_GPU_PERFLOG=0` turns
  it off; values over 1 s per frame are dropped (Azahar does not emulate the
  log). Turned off again in `gx_deinit`.
- Default ring 128 KB (`S3D_RT_RING_KB`).

Next hardware test: A. clean env; B. `S3D_RT_OVERLAP=0`. Same route.
Decision: if `gpu.p3d_us + ppf + psc` ≈ the frame time, the GPU is the wall
and the next work is GPU-side (fewer splits / state changes per draw, the
offscreen passes, fill rate); if it is well below, core 2 is still the
limit and phase 2 goes ahead.

### Hardware results (build D9596AC6, New 3DS XL, 2026-10-15)

Logs: `perf/hwlogs/D9596AC6_clean.log` (overlap on) and
`D9596AC6_overlap0_pausehang.log` (`S3D_RT_OVERLAP=0`; ends in a GPU hang,
OPEN_BUGS.md bug 21). Hub segments 67.9:93.6 and 78.6:107.2.

| hub, ms/frame | overlap on | overlap off |
|---|---|---|
| frame | 49.2 | 49.0 |
| **GPU 3D (`gpu.p3d_us`)** | **42.4** | **40.1** |
| GPU transfers / fills (`ppf` / `psc`) | 2.9 / 0.6 | 3.2 / 0.4 |
| **GPU busy total** | **45.9 (93%)** | **43.8 (89%)** |
| `rt.replay` (core 2) | 47.9 (boundary 4.8, GPU wait 1.9) | 38.5 |
| `game.logic_us` | 7.7 | 6.1 |

- **The GPU is the wall.** It is busy 89-93% of the frame, almost all of it
  3D command list execution (vertex + pixel work). The CPU side has slack:
  with the overlap off, core 2 finishes its replay in 38.5 ms and the frame
  still takes 49 ms. That is why the hashing move and the overlap stopped
  showing up in the frame time.
- The overlap makes the GPU itself 2.3 ms slower (42.4 vs 40.1): CPU memory
  traffic on both cores competes with the GPU for FCRAM bandwidth (vertex
  buffers and textures live in linear memory).
- (`gpu.split_flush` "610 calls" is per ~3 s report window, i.e. 10 per frame:
  the copies. Splits are not a cost.)
- Old CPU-side estimates assumed the GPU at ~21 ms (5EB6A447). Since then
  the renderer gained offscreen passes, fragment lighting/fog, grass layers,
  water effects and blur; the GPU now needs ~40 ms.

**What to do next:** GPU-side work. First find out what the 40 ms are made
of. **Build 654FC0A2** adds a GPU cost sweep: `S3D_GPU_SWEEP=7` in env.txt
(7 = the hub map). 600 frames after the hub loads, it runs 11 phases of 180
frames (~9 s each), each with one setting changed from the run's own values
and put back after: baseline, low resolution (fill rate), no grass layers, no
shadows, no fragment lighting, no fog, no water ripples/caustics, no GPU
blur, no heat haze, texture LOD bias +1, baseline again. Each phase starts
with a drain and a forced profiler report, logged as `sweep phase N: name`.
Table: `python -I tmp_scripts/sweep.py Star3D.log` (frame, gpu p3d/ppf/psc,
replay, boundary, sceneRender, draws, triangles per phase). The scene drifts
a little over time (Azahar: 645 → 659 draws between the two baselines), so
compare phases with both baselines. Azahar: all phases run and settings come
back; Azahar does not emulate the GSP log, so its GPU columns are meaningless.

Reading the sweep: what drops `gpu.p3d_us` most is where GPU work goes.
Low resolution isolates fill rate (pixels); if p3d hardly moves there, the
GPU is vertex/command bound (72k triangles, 650 draws, state changes), and
the levers are fewer triangles or draws (culling, merging static geometry,
DRAWCALL_PLAN.md phases 1/4) rather than cheaper pixels.

### GPU sweep results (build 654FC0A2, New 3DS XL, 2026-10-15)

Log: `perf/hwlogs/654FC0A2_sweep.log`. The player stood at -5598 -780 -3134,
angle 89 (not the usual spot: ~790 draws, 76-80k triangles). Table from
`tmp_scripts/sweep.py`:

| phase | frame | GPU 3D (p3d) | GPU total | replay | sceneRender | draws | k tris |
|---|---|---|---|---|---|---|---|
| baseline | 52.6 | 44.6 | 48.0 | 50.2 | 39.8 | 789 | 76.3 |
| low resolution | 50.8 | 47.6 | 50.6 | 48.0 | 38.9 | 796 | 78.5 |
| no grass layers | 51.0 | 43.3 | 47.1 | 49.1 | 39.4 | 748 | 77.8 |
| **no shadows** | **45.6** | **39.4** | **42.7** | **42.8** | **33.5** | 739 | 71.7 |
| no fragment lighting | 54.4 | 46.5 | 50.0 | 52.3 | 41.0 | 791 | 79.1 |
| no fog | 54.4 | 46.5 | 49.8 | 52.3 | 40.6 | 791 | 79.1 |
| no water ripples/caustics | 55.6 | 47.4 | 50.9 | 52.5 | 43.4 | 801 | 80.6 |
| no GPU blur | 55.9 | 47.8 | 51.5 | 52.3 | 44.4 | 806 | 81.2 |
| no heat haze | 55.6 | 46.6 | 50.3 | 53.6 | 43.2 | 802 | 79.9 |
| texture LOD bias +1 | 54.1 | 44.9 | 49.0 | 52.1 | 42.6 | 808 | 80.2 |
| baseline again | 53.0 | 43.5 | 47.5 | 51.0 | 41.8 | 765 | 79.3 |

- **The scene drifted during the run** (standing still): before the sweep
  46.8 ms with 5.2 offscreen passes, 7 copies and 18k moving-object
  triangles; by the end 8 offscreen passes, 10 copies, 24k (an animal or NPC
  walking in, with its shadow). Phases 4-9 sit on a rising curve, so effects
  under ~2 ms are not measurable here.
- **The GPU is not fill-bound.** Low resolution (a smaller render target,
  then upscaled) did not reduce GPU 3D time (47.6 vs 44.6 / 43.5 around it).
  So the GPU's ~44 ms goes to vertices and per-draw work (76-80k triangles,
  ~790 draws, TEV/texture reconfiguration between draws), not pixels.
  Consistent with that: fragment lighting, fog, water effects, blur, heat haze
  and texture LOD made no measurable difference.
- **Shadows are the biggest single cost:** GPU 3D -5 ms, core 2 replay
  -7.4 ms, game thread (sceneRender) -6.3 ms, frame -7 ms (52.6 → 45.6). They
  add ~50 draws and ~5k triangles plus the offscreen caster passes and copies
  (the ground under each shadow is drawn again with the shadow texture).
- Grass layers: ~-1.3 ms GPU, -41 draws. Small.
- (Bug 21: the pause menu was not opened in this run.)

Next steps, by expected gain:
1. **Shadows** (~5 ms GPU, ~7 ms CPU per side): measure their parts first
   (caster passes into offscreen targets, copies, the receiver re-draws),
   then cheapen the biggest: e.g. refresh the shadow textures every other
   frame, fewer/smaller casters, or cull receiver re-draws.
2. **GPU per-vertex vs per-draw cost:** a second sweep that separates them:
   force the unlit vertex shader (vertex cost), and a draw-count test.
   Decides between triangle reduction (culling, LOD) and draw merging
   (DRAWCALL_PLAN.md phases 3/4).

**Correction to the sweep reading (2026-10-15):** the user changed settings
by hand during that run, not in the sweep's order, so the per-phase rows
above are not reliable; only the overall picture (GPU ~44 ms of 3D work) is.
The old `S3D_LOWRES=1` mode did run in its phase (`gx.lowres_present` in the
log), but it is not in the touch menu, so it could not be checked by hand.

**Build FA1085B8** (2026-10-15):
- Every setting change is logged: `config: set NAME old -> new` (cfg_set:
  touch UI, remote `set`, the sweep), so a log shows which setting was
  active when.
- Half resolution: `S3D_LOWRES=2` renders the game at 200x120 (half each
  axis, a quarter of the pixels) into a 120x200 area of the 128x256 low
  resolution texture (viewport set after every bind, `draw_on_game`; scissor
  and copies use the drawn area, `gx_copy_source_size`, while copies keep the
  texture's memory stride), stretched over the top screen. `S3D_LOWRES=1`
  stays the old 256x128 mode (a third of the pixels). Mono only (not stereo,
  wide or AA).
- Touch menu: "Resolution (test)": Full / Half (200x120) / Third (256x128).
  It goes back to Full at every boot.
- The sweep's resolution phase now uses half resolution.
- Azahar: half and third render with the same framing as full (viewport
  and stretch agree), switches logged.

### Manual settings run (build FA1085B8, New 3DS XL, 2026-10-15)

Log: `perf/hwlogs/FA1085B8_run1.log`. The user toggled settings in the touch
menu at one hub spot; every change is logged (`config: set ...`). Table:
`python -I tmp_scripts/cfgwin.py <log> 45 373` (one row per stretch between
changes, only report windows fully inside). The scene drifted again (draws
684 → 797 → 586 over the run), so compare neighbouring rows only.

| change (neighbouring rows) | frame | GPU 3D (p3d) | draws |
|---|---|---|---|
| shadows off (54.7 → 48.7) | -6.0 | -4.1 | -36, offscreen 8 → 6 |
| fog off | ~0 | ~0 | 0 |
| per-pixel lighting off | ~0 | ~0 | 0 |
| fur shading off / wind off | ~0 | ~0 | 0 |
| fur in gameplay (mode 2) | +1 to +3 | +1 to +2 | +13 to +24 |
| grass 16 → 0 layers | -2.8 | -3.5 | -34, passes 44 → 9 |
| grass 0 → 2 → 4 → 8/16 | +0.8 / +1.5 / +0.8 | +0.6 / +2.3 / +0.5 | +10 / +22 / +3 |
| water ripples off, caustics off | ~0 | ~0 | 0 |
| texture LOD bias -2 → 0 → 2 → 4 | -0.7 / -1.7 / -0.3 | -0.6 / -0.3 / -0.8 | 0 |
| **half resolution, first 10 s (same scene, 734 draws, 75k tris)** | +1.0 | **+2.8 (41.3 → 44.1)** | 0 |

- **Not fill-bound, confirmed.** At half resolution (a quarter of the
  pixels) with an unchanged scene, GPU 3D time did not drop. Fog, per-pixel
  lighting and water effects cost nothing measurable either.
- **GPU time follows the draw count.** A fit over the 70 report windows
  (fur off, before the resolution change): p3d ≈ 9.6 ms + **47 us per
  draw** (R² 0.67); per triangle fits worse (R² 0.49), and with both, the
  triangle term comes out negative (the two are collinear). ~750 draws ≈ 35
  ms of the GPU's ~45. Each draw costs the GPU far more than its triangles:
  state setup between draws (texture/TEV/shader/vertex-format changes,
  cache flushes) dominates.
- Shadows (~4 ms GPU) and grass (~3.5 ms GPU for 8-16 layers) are expensive
  because they add draws.
- After ~10 s of half resolution the draw count fell (677, then 586) and the
  user reported the game broke; the cause is not known yet (the resolution
  setting is a test setting, Full at every boot).
- Fur in gameplay (mode 2, not the default) adds shells: +1-2 ms GPU.

Next: find what each draw costs the GPU (which state changes are expensive),
with a boot-time GPU microbenchmark through the GSP perf log: N small draws
with nothing changing between them, then with a texture change, a TEV change,
a shader switch, a vertex format change, a depth/blend change. That says
whether the fix is avoiding specific state changes (and the cache flushes
citro3d emits with them) or cutting draws (sorting, merging static geometry,
DRAWCALL_PLAN.md phases 3/4).

**Build 897794DF** (2026-10-15):
- Touch menu: "Resolution (test)" removed (it freezes the top screen, then
  crashes: OPEN_BUGS.md bug 22, won't fix for now) and both GPU overlap rows
  removed (bug 19 follows from turning overlap off). Saved values for these
  are ignored at boot; env.txt can still set them. The GPU sweep no longer
  has a resolution phase.
- **Per-draw GPU cost benchmark at boot** (os/bench.c `bench_drawcost`,
  part of the existing `S3D_BENCH` boot benchmarks, ~2 s): 400 tiny draws in
  one frame per test, GPU time from the GSP perf log (P3D), best of 3
  rounds. Tests: same state (baseline), same state indexed, same state with
  100 triangles per draw (per-triangle cost), then one change per draw:
  texture, combiner (1 stage), combiner (6 stages), shader program rebind,
  vertex format re-set, vertex buffer change, uniform matrix upload, blend
  mode, depth test, and texture+combiner+buffer+matrix together (a typical
  port draw). Log lines `bench: gpu per draw ...`: total GPU us, us per draw,
  cost against the baseline, and CPU us per draw. Azahar prints "GPU n/a" (no
  GSP log); its CPU column runs 8-17 us per draw.
- Reading it: the hub shows ~47 us of GPU time per draw. If one change costs
  most of that, avoid it (sort draws, skip redundant changes and the cache
  flushes citro3d emits with them). If the baseline alone is large, every
  draw is expensive and the answer is fewer draws (merging, batching).

### Per-draw benchmark results (build 897794DF, New 3DS XL, 2026-10-15)

Log: `perf/hwlogs/897794DF_bench.log`. 400 tiny draws per test, GSP P3D time:

| per draw | GPU us | cost vs same state |
|---|---|---|
| same state | 0.42 | - |
| same state, indexed | 0.91 | +0.49 |
| same state, 100 triangles (300 vertices) | 15.66 | +15.24 |
| texture change | 2.54 | +2.12 |
| combiner change, 1 or 6 stages | 0.94 | +0.52 |
| shader program rebind | 0.78 | +0.36 |
| vertex format / vertex buffer / matrix upload | 0.57-0.77 | +0.15-0.35 |
| blend / depth change | 1.03 | +0.61 |
| texture + combiner + buffer + matrix | 3.17 | +2.75 |

Fill rate (same boot): 0.49-0.70 ms per full 400x240 screen (fragment light
x4: 1.45, x8: 2.9). Vertices with the 12-instruction bench shader: 16.8 Mvert/s
(float), 13.3 (s16).

- **State changes between draws are cheap**: a whole typical change set is
  ~2.75 us, a bare draw 0.4 us. 750 draws x ~3 us ≈ 2-3 ms, nowhere near the
  hub's ~44 ms. So it is not per-draw overhead either.
- Fill rate cannot explain it (the hub's few full-screen passes cost ~0.5 ms
  each), matching the half resolution result.
- What is left is **vertex processing**: the port's lit shader
  (`default.v.pica`) is ~190 instructions with up to 8 per-vertex lights in
  subroutines, against the bench shader's 12, and the hub draws ~219k
  indices per frame (Azahar counters, below). GPU time following the draw
  count fits that: draws carry the geometry.

**Build 039826F3** (2026-10-15):
- Boot benchmark of the game's vertex shaders (`gx_vbench`, gx_init.c, part
  of `S3D_BENCH`, off with `S3D_BENCH_VSH=0`): 12,000 unique vertices
  (DrawArrays, GxOutVertex layout, the same uniforms as the GPU self-test)
  per configuration: unlit; lit with lighting off, 1, 2, 4 and 8 lights; lit
  with 1 light and the fragment-lighting outputs; warp shader with warp on;
  unlit with the packed 36-byte layout; and lit with 1 light, indexed, each
  vertex reused by 3 consecutive triangles. Log lines `bench: vertex shaders`
  with us per vertex and Mvert/s.
- Hub counters (summed per draw, emitted at the frame end): `gx.vidx_lit`,
  `gx.vidx_unlit`, `gx.vidx_warp` (indices drawn with each variant) and
  `gx.vidx_x_lights` (lit indices x lights evaluated). Azahar hub (usual
  spot): 132.7k lit + 86.4k unlit = 219k indices per frame, 26k light-indices
  (lit draws evaluate 0.2 lights on average: the lit shader's main path
  matters more than the lights).
- Combining both: estimated hub vertex shading time = lit indices x lit cost +
  unlit indices x unlit cost (corrected by the reuse result), compared with
  `gpu.p3d_us` (~40-44 ms).

### Vertex shader results (build 039826F3, New 3DS XL, 2026-10-15)

Log: `perf/hwlogs/039826F3_vbench.log`. Boot benchmark, 12,000 vertices per
test, GSP P3D time:

| configuration | us per vertex | Mvert/s |
|---|---|---|
| unlit shader | 0.195 | 5.12 |
| lit shader, lighting off | 0.210 | 4.76 |
| lit, 1 / 2 / 4 / 8 lights | 0.298 / 0.400 / 0.611 / 1.032 | 3.36 / 2.50 / 1.64 / 0.97 |
| lit, 1 light + fragment-light outputs | 0.333 | 3.01 |
| warp shader, warp on | 0.213 | 4.69 |
| unlit, packed layout (36 B) | 0.161 | 6.22 |
| lit, 1 light, indexed, each vertex in 3 triangles | 0.099 per index | 10.11 |
| (bench shader, 12 instructions, 24 B: earlier boot bench) | ~0.06 | 16.8 |

- Each light adds ~0.1 us per vertex. The base cost of the game's shaders
  (0.195-0.21 us) is ~3.3x the minimal bench shader: 7 input attributes /
  52 bytes against 3 / 24 bytes, and more work per vertex. The packed layout
  saves 17%.
- The post-transform cache works: with 3x reuse the cost per index drops to
  exactly a third.

Hub on hardware (spot -5595 -782 -3102, angle 85; segment 58:85.3): frame
51.6 ms, GPU 3D 43.4 ms, 738 draws, 74.4k triangles, indices: lit 130.3k,
unlit 93.8k, warp 0.07k; lit indices x lights 37.2k (0.29 lights per lit
index on average).

**Build 01E7BFF2** adds vertex reuse statistics to the display list cache
(`weld_stats`, computed once per recorded list; counters `dl.vs_*`). Azahar
hub (usual spot; content counts do not depend on timing):

| cached draws, per frame | count |
|---|---|
| indices | 214.1k |
| output vertices | 124.5k (1.72 indices per vertex) |
| after merging identical vertices per draw | 105.7k (-15%) |
| vertex shader runs, current indices, FIFO cache of 4 or 16 | 124.5k (each vertex exactly once) |
| the same after merging, unchanged index order | 123.8k / 121.2k |

- Every output vertex is shaded once: the cache already catches the reuse
  inside each strip. Merging identical vertices could save at most 15% of the
  vertex work, and only with the indices reordered for the cache. Not the
  big lever.
- Estimated vertex shading in the hub: ~125-130k vertex runs x ~0.21-0.24 us
  (lit/unlit mix plus 0.29 lights per lit vertex) ≈ **27-31 ms of the GPU's
  ~43 ms**. State changes ~2-3 ms. The remaining ~10 ms is not explained yet;
  the candidate is per-triangle setup and clipping (74k triangles; back-face
  culling is off, DRAWCALL_PLAN.md phase 1 item 4), since fill was ruled out.

Levers, by size:
1. **Cheaper vertex shaders**: the game's shaders cost 3.3x the minimal one
   per vertex. Lean variants for the common cases (e.g. one texture
   coordinate, no lighting; skip texture matrices that are identity, fewer
   outputs), and the packed layout for more lists (-17%). First part done:
   see "Leaner vertex shaders" below.
2. Per-triangle cost: measure it (a boot test that draws many triangles over
   3 cached vertices), and check back-face culling.
3. Fewer vertices: culling cached segments by bounds before drawing
   (DRAWCALL_PLAN.md phase 5 per-part culling), welding + reordering (<=15%).

### Leaner vertex shaders (build FFBB7670, 2026-10-15)

Lever 1, plus a bench that says where the rest of the per-vertex cost is.
The first hardware build (B3EE1AF9) had a visual bug, now fixed (see
"Hardware result (build B3EE1AF9)" below). Measured on hardware with
A74D5292: -2 to -3 ms of GPU time in the hub, no visual change.

**Constraints found first** (they decide the design):
- **Shader code memory.** `default.shbin` (the three variants share one code
  blob) used 352 of the PICA's 512 instruction words, so the variants can't
  be multiplied. Now 457 words (lit 0-139 plus light procs, unlit 255-347,
  warp 347-457), 56 of 128 operand descriptors.
  `tmp_scripts/shbin_mad.py <shbin>` lists the MAD instructions and their
  operand descriptors (MAD only has 5 bits for the descriptor index).
  `tmp_scripts/shbin_info.py <shbin>` prints these, plus every uniform's
  register.
- **Float uniforms are full.** The lit shader uses c0-c91 for uniforms, and
  picasso puts `.constf` constants at the top (c92-c95), so all 96 are taken.
  Constants of the variants also sit at the top, so they never land on
  another variant's uniforms. citro3d re-uploads a program's constants when
  it is bound, but other uniforms only when they are dirty.
- **Bools are now full too, and b15 is off limits.** `tcaff[3]` is declared
  first in all three files, so it sits at b0-b2 in each. That moved the lit
  shader's bools: lighten b3-b10, lighting b11, warpon b12, fraglit b13,
  nrmout b14. **b15 does not work as a vertex shader bool on hardware**
  (HARDWARE_DEBUG.md, Azahar blind spots). gx_init.c `init_shader` checks
  that the shared registers match, and refuses a bool at b15.
- PICA instructions can't read two different input registers (picasso:
  "source operands must be different input registers").
- Bugs 12 and 14 still apply. Every output is written once, all 4
  components, on every path. Exclusive `if`/`else` branches may each write
  the same output (as the existing lighting branch already did).

**What changed:**
- Texture coordinates were 35-41 of the unlit shader's 57 instructions. Each
  of the 3 slots ran the general texgen every time: input mix of texcoord /
  position / normal, 3x4 matrix, `rcp` divide, warp term.
- Every shader now picks a path per slot:
  - `tcaff[s]` (bool): the **affine path**. input = texsrc.x·(s,t,1,1) +
    texsrc.y·(pos,1) + texsrc.z·(nrm,1) (one `mul` + two `mad`), then two
    `dp4`, with no q row, divide or warp. 8 instructions against 13 (unlit)
    or 15 (lit/warp) for the general path. gx_render.c
    `upload_unit_texmtx` sets it when the slot doesn't warp and its q row is
    exactly (0, 0, 0, 1), so q = 1 for every input. The only difference from
    the general path is the rounding of `rcp(1)` and the multiply by it.
  - Slot 1 or 2 not sampled: the CPU writes texsrc[s].y = 2 (real slots
    have 0 or 1), and the slot outputs zeros. The shader tests it with
    `cmp texsrc[s].yyyy, gt, gt, r9.wwww` (r9.w = 1) plus `ifc`. B3EE1AF9
    used a bool `tc12` for "slots 1 and 2 unused"; it became a marker to
    free a bool and keep b15 empty.
  - The normal input (r9) is computed once, no longer per slot.
- `S3D_VSH_LEAN` (default 3, re-read when settings change, so the remote
  `set` changes it live; no menu row). A bitmask: bit 0 = affine path, bit 1
  = skip unused slots 1 and 2. 0 = every slot computed on the general path.
  That isn't byte-identical to the old shaders: +1 `ifu` per slot and the
  two slot checks, about +7 instructions.
- Multipass and water-split passes call `upload_unit_texmtx` for their unit,
  which updates the bool and the markers. A slot used by any pass counts as
  sampled.

Unlit shader instructions per vertex (position 11, view 3, color 1 and the
normal input 2 are common to all paths):

| case | before | lean |
|---|---|---|
| 1 slot | 56 | 32 |
| 2 slots | 56 | 40 |
| 3 slots | 56 | 48 |
| 3 slots, general path (S3D_VSH_LEAN=0 or projective/warp) | 56 | 63 |

**New counters** (normal builds, per frame, indices × eyes):
- `gx.vidx_skip1`, `gx.vidx_skip2`: indices with slot 1 / slot 2 skipped
  (B3EE1AF9 had `gx.vidx_tc1` for both skipped, and `skip2` for slot 2
  alone).
- `gx.vidx_slots_used`: indices × slots sampled.
- Per computed slot, by path: `gx.vidx_aff_tc`, `gx.vidx_aff_pos`,
  `gx.vidx_aff_nrm` (affine from the texcoord, position or normal), and
  `gx.vidx_gen_proj`, `gx.vidx_gen_warp`, `gx.vidx_gen_off` (general path:
  projective q row, warp, lean off).

**Azahar, attract scene** (content counts; Azahar's fps doesn't model the
GPU):

| per frame | lean on | lean off |
|---|---|---|
| indices (lit + unlit) | 120.4k | 120.4k |
| `gx.vidx_tc1` / `skip2` (B3EE1AF9 counters) | 36.3k / 4.5k | 0 |
| slots sampled | 271k | 271k |
| slots computed, affine (texcoord / normal) | 147.7k / 136.3k | 0 |
| slots computed, general | 0 | 361k |

- The first version (Azahar-only build C819BA77) only took the affine path
  for texcoord inputs, and 47% of slots stayed general. All of
  those were **normal-source texgen** (`GX_TG_NRM`, likely environment and
  light maps on lit models). None were position, projective or warp. Hence
  all sources on the affine path.
- Most draws sample all 3 slots (2.25 per index), so the attract scene gets
  the "3 slots" row: about -16% unlit instructions, more for lit (general
  slot 15 → 9). The hub may differ; its counters will tell.
- Azahar can't show the b15 bug (below): it treats b15 as an ordinary bool.
- Comparing screenshots across separate runs is unreliable. The attract demo
  drifts by a few seconds between runs, so the same timestamp shows different
  moments (a green attack glow in one run looked like a lean bug). Compare
  within one session instead: `tools/drive.ps1` with remote `set
  S3D_VSH_LEAN N` and `@shot` (script `tmp_scripts/lean_ab.txt`: hub save
  state, shots with lean 3, 0, 1, 2, 3). In FFBB7670, Fox and the Arwing look
  the same in all modes.
- Azahar hub with FFBB7670 (the window mixes the toggled modes, so rough):
  slot 1 skipped on ~152k and slot 2 on ~157k indices per frame. Unlike the
  attract scene, the hub has many 1-texture draws.

**Bench additions** (`gx_vbench`, boot, `bench: vertex shaders`, now 24
rows). Each comparison answers a question:

| compare | question |
|---|---|
| `unlit lean, 1/2/3 affine slots` vs `unlit shader` | how much do instructions cost? (29-47 vs 56) |
| `lit lean, lighting off / 1 light, 1 slot` vs the lit rows | the same, lit shader |
| `io: 7 in (52 B), 7 out, ~9 instr` vs `unlit shader` | the ALU share of the unlit cost (same I/O) |
| `io: 7 in, 3 out` vs `io: 7 in, 7 out` | the cost of outputs (a lean variant with fewer outputs would need its own DVLE and code words) |
| `io: 3 in (52 B stride)` vs `io: 7 in` | the cost of input attributes (same bytes per vertex) |
| `io: 3 in (24 B)` vs `io: 3 in (52 B stride)` | the cost of bytes fetched (same attributes) |
| `unlit, VRAM` / `unlit lean, 1 slot, VRAM` / `unlit, packed, VRAM` / `io ... VRAM` vs FCRAM | whether cached lists should move to VRAM |
| `unlit lean, 1 slot, packed` | lean and packed together |

- The I/O shaders are `gx/vbench/v*.v.pica`, in their own `vbench.shbin`
  (Makefile `VBENCH_VSH`), so they cost the game no code words.
- VRAM copies use a GX texture copy (`C3D_SyncTextureCopy`, raw) and are
  checked by reading back 64 bytes from each end. On failure, or with no
  VRAM, the log has a WARN and the VRAM rows say n/a.

**Hardware result (build B3EE1AF9, 2026-10-15)**: the user saw Fox, every
other character and the title screen washed out toward white, and many
flickering, whitish models in the hub (especially the Arwing). The logs
weren't in the repo; the two render captures were (`captures/`, title f490
and hub f2318). The f490 `light` pass (vertex color only, `nrmout` off) showed
Fox in normals colors. So on hardware the lit shader ran the `nrmout` branch
(color = view-space normal / 2 + 0.5) with the bool set to false, and the
only change to `nrmout` was its move to b15. Fixed in FFBB7670 (no bool at
b15, plus a boot check). The flicker is probably the same cause; the re-test
will tell. The performance numbers of B3EE1AF9 are unusable (lit models ran
the short `nrmclr` path instead of lighting).

**Hardware test (build FFBB7670)**:
1. Boot with a clean env.txt. The `bench: vertex shaders` lines are the
   result. First check visually: Fox, the title screen and the hub Arwing
   must have their normal colors and must not flicker.
2. Hub, the usual spot (-5595 -782 -3102, angle 85), standing still ~30 s,
   clean env (lean on).
3. The same with `S3D_VSH_LEAN=0` (add `S3D_BENCH=0` to skip the bench).

Compare `gpu.p3d_us`, the frame time and the `gx.vidx_*` counters between
runs 2 and 3. Baseline: hub frame 49-51 ms, GPU 3D 43.4 ms.

If the boot hangs in the bench, use `S3D_BENCH_VSH=0`. If the game hangs,
use `S3D_VSH_LEAN=0`. `S3D_GPU_SELFTEST=1` runs the game shaders with the
lean bools off (general path, as before).

**Hardware results (build FFBB7670, New 3DS XL, 2026-10-15)**. Logs:
`Star3D.log` (hub, 122 s, clean exit) and `Star3D.prev.log` (ended in a GPU
hang, bug 21), both lean on. Captures `f1006` (Bomb Spore dialog) and `f1305`
(hub).
- **The b15 fix works.** The title screen is fine, and the captures' `light`
  passes show no normals colors.
- **Still seen:** some small flowers look white at a distance, and Fox, the
  floor and the grass looked white in a dialog box once or twice ("happens
  much less"). The two captures' color frames look right, so the white frames
  weren't captured. Not yet known whether lean causes it: the next build has a
  menu row to flip it live.
- `Star3D.prev.log` hung at f1987, 5 frames after `DVDOpen
  gametext/Communicator/English.bin`, with the render thread in
  `gx_gpu_wait_previous`. That is bug 21's pattern (seen before the lean
  shaders, build D9596AC6); OPEN_BUGS.md updated.
- Hub (`Star3D.log`, frames 1020-1155): 43.8 ms per frame (22.6 fps), GPU 3D
  38.2 ms. **Not comparable** with the 49-51 ms / 43.4 ms baseline: this view
  draws 185k indices against the baseline's 224k, and there was no lean-off
  run. Lean counters there: slot 1 skipped on 104k and slot 2 on 106k of
  185k indices; computed slots 207k affine texcoord + 132k affine normal +
  3.5k affine position, 4.2k general (warp).

Vertex bench (two boots, within 1%):

| test | us per vertex |
|---|---|
| unlit shader (56 instructions, 7 attributes, 52 B, 7 outputs) | 0.201 |
| unlit lean, 1 / 2 / 3 affine slots | 0.174 / 0.182 / 0.192 |
| lit, lighting off: general / lean 1 slot | 0.218 / 0.180 |
| lit, 1 light: general / lean 1 slot | 0.318 / 0.247 |
| unlit packed (36 B) / lean 1 slot packed | 0.165 / 0.136 |
| unlit in VRAM / lean 1 slot in VRAM / packed in VRAM | 0.122 / 0.111 / 0.122 |
| io: 7 attributes (52 B), 7 outputs, ~9 instructions | 0.161 |
| io: 7 attributes, 3 outputs | 0.157 |
| io: 3 attributes from the 52 B stride, 7 outputs | 0.053 |
| io: 3 attributes, 24 B layout, 7 outputs | 0.053 |
| io: 7 attributes, VRAM | 0.100 |

What it says:
- **Input attributes are the cost**: about 0.027 us per attribute per vertex
  (7 → 3 attributes: 0.161 → 0.053). Bytes don't matter (3 attributes at a
  52 B or 24 B stride cost the same), and skipping attributes with loader
  padding is free.
- **Instructions are ~20%** of the unlit cost (0.201 vs 0.161 with the same
  I/O). The lean paths deliver what that allows: -13% unlit with 1 slot, -17%
  to -22% lit.
- **Outputs cost almost nothing** (7 vs 3: -2%). A lean variant with fewer
  outputs is not worth code words.
- **VRAM is ~40% faster than FCRAM** for vertex data (0.201 → 0.122; io
  0.161 → 0.100). The packed layout helps only in FCRAM. In game the log
  shows `VRAM free 0 KB`, so cached lists in VRAM need VRAM freed first.

Next levers, by expected size:
1. **Per-draw attribute pruning**: load only the attributes a draw reads,
   with padding in the loader permutation (proven by the 52 B stride test).
   Candidates: tc1/tc2 when those slots are unused (half the hub's indices),
   the normal when nothing lit or normal-sourced, the matrix index when one
   matrix is used, and merging tc0+tc1 into one float4. At ~0.027 us per
   attribute and ~125k vertex runs per hub frame, 2-3 fewer attributes is
   roughly 7-10 ms of GPU time. The shader must not read an unloaded input
   (r9's normal read moves under its users).
2. **Cached lists in VRAM** (-40% vertex time), once VRAM has room.

**Hardware test (build A74D5292)**:
- New: a "Lean shaders" settings row (On / Off / Fast math / Skip slots =
  S3D_VSH_LEAN 3 / 0 / 1 / 2) to flip it while looking at the white flowers
  or a dialog.
- New: `S3D_VSH_LEAN_AB=<frames>` alternates lean 3 and 0 every N frames and
  logs `vshab` lines with each phase's average frame and GPU time (skipping
  30 frames after each switch). Don't change touch menu settings during an
  A/B run: the menu saves every row, including the A/B's current mode.
1. env.txt `S3D_VSH_LEAN_AB=300` and `S3D_BENCH=0`. Hub, the usual spot,
   stand still ~60 s (6+ phases).
2. Visual: with the menu row, check the distant white flowers and a dialog
   with lean On and Off.

**Hardware A/B result (build A74D5292, New 3DS XL, 2026-10-15)**. Log:
`perf/hwlogs/A74D5292_leanab.log` (`S3D_VSH_LEAN_AB=300`, 270 measured frames
per phase). Two standing positions, 60 s each: position 1 near the distant
flowers, position 2 in a dialog.

| position | lean 3: GPU | lean 0: GPU | saved |
|---|---|---|---|
| 1 (2 pairs) | 46.6 / 46.5 ms | 48.6 / 48.7 ms | -2.0 ms (-4%) |
| 2, dialog (2 pairs) | 42.0 / 42.5 ms | 45.1 / 45.1 ms | -2.8 ms (-6%) |

- **Visuals:** the user saw the distant white flowers unchanged through all
  phases, and the dialog looked right throughout. **Lean doesn't cause the
  remaining white.** Candidate for the flowers: `S3D_TEX_LODBIAS_X4=-2`
  (sharper mips on distant alpha textures); not checked.
- **Frame time didn't follow the GPU** (`vshab` wall time 59-62 ms in both
  modes at position 1, ~55 ms at position 2). The frame was ~13 ms longer than
  the GPU time, so this run was CPU-bound.
- The user's settings differ from the earlier baselines: Fur "Always"
  (`S3D_FUR_MODE=2`), grass layers 16 in the previous run, LOD bias -2.
  D9596AC6 (the "GPU is the wall" run) used Fur "Cutscene" and 8 grass
  layers. Absolute frame times since FFBB7670 are not comparable with the
  49-51 ms baseline; the A/B is, since both modes share the settings.
- The perf windows' "frame avg" (~55-57 ms) is lower than wall time per
  frame (window length / frames: ~60 ms at position 1). `vshab` measures wall
  time. Compare like with like.
- The lean saving (~2-3 ms of GPU) matches the bench: instructions are ~20%
  of vertex cost. The bigger vertex levers are attribute pruning and VRAM
  (above). They only show in frame time once the frame is GPU-bound again,
  so a CPU profile with the user's settings comes first (what Fur "Always" and
  16 grass layers cost on the CPU).
- `S3D_VSH_LEAN` stays at 3 by default.

**CPU profile with the user's settings (build A81DF396, PROFILE=1,
2026-10-15)**: `out/star3d_profile.3dsx`. Purpose: the A74D5292 A/B showed the
frame CPU-bound (~60 ms wall time, GPU ~47 ms) with Fur "Always", grass layers
16 and LOD bias -2. Run: env.txt `S3D_BENCH=0` only (no A/B), the user's
settings, then stand still ~30 s at position 1 (distant flowers), and ~30 s
with Fur set to "Cutscene" at the same spot (the log records the `config:
set` line, so the windows split there). Read `game.logic_us`, `game.sceneRender`,
the `gx.*` zones, `rt.*` and `gpu.p3d_us` per window. Profile builds run
slower than normal builds (B6C1F1B4: replay 58 vs 47 ms), so compare zones,
not fps.

**Result (A81DF396, New 3DS XL, 2026-10-15).** Log:
`perf/hwlogs/A81DF396_profile.log` (`S3D_BENCH=0`, user's settings). The user
stood in the hub (map 7 from f1136) and stepped through most touch menu rows
(shadows, fog, frag light, fur shade/wind/budget/mode, grass 16/0/2/4/8/16,
water/caustic/screen warp, blur, stereo reduced/autoconv, lean 3/0/1/2/3,
LOD bias). Tables: `tmp_scripts/winrows.py <log>` (one row per report
window, with the settings changed in it) and `tmp_scripts/cfgstretch.py`
(wall ms/frame between two changes; inflated by touch menu use, unreliable).

- **The render thread is the wall, not the GPU.** In every hub window,
  frame avg ≈ `rt.replay` (frame 73-77 ms, replay 70-74 ms), while GPU
  `p3d` is 41-45 ms. The game thread spends 63-66 ms in `game.sceneRender`,
  but 29-32 ms of that is `rt.space_wait` (waiting for ring space because
  the render thread is behind). Game logic is 7.7 ms. So the game thread is
  busy ~42 ms per frame, the GPU ~42 ms, and the render thread ~72 ms.
- Profile overhead: B6C1F1B4 measured replay 58 vs 47 ms (+23%). Scaled,
  replay is ~58 ms in a normal build. That matches the ~60 ms wall time of
  the A74D5292 run with the same settings, so the normal build is
  render-thread bound too.
- **Render thread per frame** (window at 106.9 s, all user settings; zones
  nest):

  | zone / op | ms/frame | per call |
  |---|---|---|
  | `gx_dl_call_hashed` op (726/frame) | 40.6 | 56 us |
  | ↳ `gx.dl.play` (733/frame) | 21.3 | 29 us |
  | ↳ `rt.dl_prehash` | 5.3 | 7 us |
  | ↳ `gx.dl.lookup` + `gx.dl.validate` | 3.7 | 5 us |
  | `gx.prepare` (~800/frame) | 15.1 | 19 us |
  | ↳ `gx.prep.tev` (580/frame, includes bindtex) | 9.9 | 17 us |
  | ↳ `gx.prep.bindtex` | 4.6 | 8 us |
  | ↳ fog + lightenv + proj + posmtx | 2.5 | |
  | `gx.draw` (817 draws) | 13.3 | 16 us |
  | ↳ `gx.draw.update` / `gx.draw.submit` | 5.9 / 3.7 | |
  | `gx.gather` | 6.2 | |
  | immediate-mode data op | 3.4 | 16 us |
  | frame boundary op | 2.9 | |
  | `gpu.split_flush` | 1.9 | |
  | `skin.blend` | 1.8 | |
  | `GXCopyTex` (9.4/frame) | 1.4 | 153 us |
  | the other ~26,000 commands/frame (state ops) | ~22 | ~0.7-1.2 us |

  The per-op times include the profiler's own timing of every op, so the
  ~22 ms of small state ops is probably the most inflated row.
- `gx.prep.tev` costs 17 us per call even though the TEV cache hits almost
  always (261 hits, 7 misses, 313 apply skips per frame). The non-texture
  part (unit map, texmtx upload, `gx_tev_apply`) is ~5 ms and bind_textures
  ~4.6 ms. Both are candidates, together with `gx.dl.play` (21 ms).
- **Settings, effect on the frame** (report windows are ~40 frames / 3 s;
  most toggles lasted less than one window, so effects under ~1-2 ms are
  not visible):
  - Fur Always -> Off/Cutscene: replay -3..4 ms, `game.sceneRender` -8 ms,
    frame ~74 -> ~69 ms. The only setting with a clear effect. In the hub,
    Cutscene and Off measured the same.
  - Grass 16 -> 0/2: replay -1.5..2 ms, draws 803 -> 772, `p3d` -1..2 ms.
  - Lean shaders 0 vs 3: `p3d` 45.2 vs 43.4 ms (matches the A74D5292 A/B),
    no frame change (the frame is render-thread bound).
  - Shadows, fog, frag light, fur shade/wind/budget, water/caustic/screen
    warp, blur, stereo reduced/autoconv, LOD bias: no measurable change in
    frame, replay or `p3d`. Shadows were off for only 16 frames; the
    654FC0A2 sweep had measured ~5 ms GPU for them, which would not show in
    the frame now anyway.
  - The large wall-time values between changes (e.g. 193 ms/frame after fog
    off) come from touch menu use and settings saves, not the settings.
- **Conclusion:** with the user's settings, GPU-side work (shaders,
  shadows, effects) no longer limits fps. The next gains are on the render
  thread: `gx.dl.play`, `gx.prep.tev`/bindtex, `gx.draw.update`, and the
  per-command cost of the ~27k replayed commands per frame. Alternative:
  move part of that work to the game thread, which has ~30 ms of slack
  (all of `rt.space_wait`).
- Side notes: the attract scene (map 63) ran at frame 67-69 ms with GPU
  23-25 ms (the same pattern). `dvd: callback queue full` errors at f43
  (boot audio loads), as before.

### Render thread: redundant work (build 54AEB989, 2026-10-15)

Follows the A81DF396 result: the render thread is the wall. The user chose
"measure first, then cut redundant work" (options 1 and 2 of the proposal).
`S3D_RT_DEDUP` (bitmask, default 7) switches each part, so one build can A/B
them on hardware.

**Measured in Azahar first (PROFILE=1, hub save, user settings).** The counts
come out the same on Azahar and hardware; the times don't.
- New report columns: rtops `N% unchanged` per opcode, and the line
  `unchanged setters X/frame of Y/frame`. A command counts as unchanged when
  its replay leaves `g_gx_change_seq` (bumped by every `gx_state_changed`)
  alone.
- **73% of the state commands change nothing:** 12,977 of 17,861 setters per
  frame. The render-side setters already compare and return early, so the
  waste is the ring traffic plus the dispatch, ~0.7-1.2 us per command on
  hardware (profiled).
  - Worst offenders: TEV ops (900+/frame each, 70-99% unchanged) and
    `GXLoadTexMtxImm` (1,500/frame, 93% unchanged).
  - `GXLoadNrmMtxImm`: 1,350/frame, 14 words each, and nothing ever reads the
    normal matrices (the vertex shader uses the position matrix).
- **State setup:** it runs ~730 times per frame. 217 of the TEV/texture
  rebuilds are texture-only changes, and 72% of the unit-map rebuilds produce
  the same map.

**What changed:**
1. Bit 1, `gx_render_prepare`: a texture-only change keeps the unit map
   (`prep.unitmap_kept` ~220/frame). The map depends only on TEV and texgen
   state, and all of their changes mark TEV. Exception: the attract movie,
   whose stages are recognised by their texture (`gx_tev_movie_dependent`).
2. Bit 2, record-time filter (gx_rec.c, `dd_redundant` in `gxrec_end`):
   - A setter command is dropped when its arguments are bit-identical to the
     last one recorded for the same state. The key is opcode + key argument:
     the stage, texmap, channel or light index, or a constant for global
     state.
   - The remembered commands are forgotten on any unclassified command and at
     `refresh_shadows` (direct sections, syncs, frame start).
   - The matrix group (position/texture matrices, texgens) is forgotten at
     every display list call: lists can load XF matrices and the CP matrix
     index registers, but not BP.
   - The indirect group (`GXSetTevDirect`, `GXSetNumIndStages`) is forgotten
     at `port_tev_indirect`.
   - Overlapping channel IDs (COLOR0 vs COLOR0A0) forget each other.
3. Bit 4: `GXLoadNrmMtxImm` is not recorded at all.
4. `GXSetProjection` now compares like the other setters. It used to always
   mark the projection dirty (~190 calls/frame, half unchanged). Projection
   inputs that change mid-frame (stereo, HUD, offscreen) already mark it
   themselves.
5. PROFILE=1 bit 8, verify: predicted-redundant commands are replayed with a
   mark instead of dropped, and any that change state are logged
   (`dedup verify: ...`, counters `rec.dedup_verify_fail_*`). A second hook
   logs state changes made under neutral commands.

**Bug found by the verify mode (fixed before the build):** global setters
(blend mode, TEV stage count, fog, Z mode, ...) first used their first
argument as the key. That kept one entry per value, so Blend(NONE) →
Blend(ADD) → Blend(NONE) dropped the third call. It showed as different
render counters with the filter on (PICA stages 916 vs 578, priority drops
115 vs 11). Global setters now use one key per opcode (`DD_KEY_NONE`).

**Azahar results (profile build, hub, user settings, one session 7 → 0 → 7):**

| | DEDUP 7 | DEDUP 0 | DEDUP 7 |
|---|---|---|---|
| commands replayed / frame | 12,498 | 23,775 | 12,561 |
| commands dropped / frame | 9,938 | 0 | 9,963 |
| `rt.replay` ms/frame | 112.8 | 134.1 | 111.8 |
| `rt.space_wait` (game thread) | 50.8 | 88.3 | 50.2 |
| `game.sceneRender` | 100.5 | 122.9 | 99.4 |
| `gx.prepare` | 20.0 | 20.5 | 19.9 |

- The render counters match across the three phases, i.e. the same
  rendering: PICA stages 571/577/576, priority drops 11/11/11, pixel changes
  166/167/166, draws 755/749/748. The small drift is the scene animating.
  The screenshots (build/emu/dd7_a, dd0, dd7_b) only differ by the
  day/night tint and a moving creature.
- **Verify mode (`S3D_RT_DEDUP=15` from boot)** covered boot, title,
  attract movie, the map load and the hub: 0 failures. It dropped ~3,700
  commands per frame in the attract scene and ~9,900 in the hub.
- Normal build in Azahar (`S3D_RT_DEDUP_AB=200`, hub): 7 → 78.4 ms, 0 → 83.7
  ms, 7 → 78.2 ms wall time. About -5 ms per frame; emulator timing, not
  hardware.
- Misses left (per frame): 2,450 matrix/texgen commands forgotten at display
  list calls, 3,700 real changes, 1,085 after resets (GXCopyTex, offscreen
  begin/end, `GXSetTevColorS10`), 600 slot collisions. The display list
  misses could be recovered if the game thread knew which lists contain XF/CP
  loads (none in the hub: `dl.replay_ops` = `dl.replay_segments`).

**Hardware test (pending):** out/star3d_dedup.3dsx (build 54AEB989, normal
build).
- Put `S3D_RT_DEDUP_AB=300` and `S3D_BENCH=0` in env.txt. The A/B is read
  only at boot; a live `set` does not start it.
- Use the user's settings and the usual hub spot, and stand still ~60 s. The
  log gets `dedupab` lines: frame and GPU time per phase, DEDUP 7 vs 0.
- Visual check: play normally with the default (7) and look for wrong blending,
  colours or textures. Any difference from DEDUP 0 is a filter bug (repro it
  in Azahar with verify mode: PROFILE=1, `S3D_RT_DEDUP=15`).

**Hardware result (54AEB989, New 3DS XL, 2026-10-15).** Log:
`perf/hwlogs/54AEB989_dedupab.log` (`S3D_RT_DEDUP_AB=300`, user's
settings). The user saw nothing wrong on screen apart from "really unstable
frame time".
- **Hub, standing still (f1799-2999, five phases):** `dedupab` wall time
  0: 59.2, 7: 59.6, 0: 57.8, 7: 58.0, 0: 56.1 ms. **No frame time gain.**
- Zones, same spot (windows 110/146 s on vs 130/165 s off):

  | | on | off |
  |---|---|---|
  | commands | 13.0k | 26.3k |
  | `rt.replay` | 47.6-48.0 ms | 49.0-49.5 ms |
  | `rt.space_wait` | 3.3-3.6 ms | ~10 ms |
  | GPU `p3d` | 45 ms | 45 ms |
  | `ppf` | ~3.5 ms | ~4 ms |
  | perf frame avg | 53.5 ms | 53.5 ms |

- **Correction to the A81DF396 conclusion:** in a normal build the render
  thread is *not* clearly the wall. Replay (~48-49 ms) and GPU (~49-50 ms per
  frame with post-processing) are nearly equal. The profile build inflated
  the render thread by ~50% (72 vs ~48 ms), not the ~23% assumed from
  B6C1F1B4. The filter saves ~1.5 ms of replay and ~7 ms of game-thread ring
  waits, but the frame is held by the GPU.
- **Unstable frame time = the filter changes frame pacing.**
  - Per-window distribution, hub standing still, same average (~53.5 ms):
    - off: every frame 50-67 ms (max 66-68);
    - on: 7-42 frames per 56-frame window under 50 ms, the rest slower, max
      80-89 ms.
  - So with the filter the frames alternate fast/slow around the same mean.
  - Likely mechanism (not yet proven): the render thread (~48 ms) now finishes
    before the GPU (~49-50 ms), and the hand-off (one frame in flight, no
    vsync pacing: `gx.unpaced_frames` 1/frame) beats between the two
    near-equal periods. Without the filter, the ring-full waits throttled the
    game thread to a steady cadence.
  - Next: a per-frame trace (wall dt, replay end, GPU end, waits) to see the
    pattern, then pace frames (e.g. present at a steady interval, or wait for
    the previous frame's GPU at one fixed point).
- Map 8 (from f3060): the "on" phases look faster (37-43 vs 45-60 ms), but
  the user was moving (draws 400-900, GPU time differs between phases). Not
  comparable.
- The run ended in **bug 21** (GPU hang 5 frames after loading
  `gametext/Communicator/English.bin`, f5091 → f5096; filter on). Same
  signature as FFBB7670; not attributed to the filter.

### Frame pacing: per-frame trace (build 7AED0EF9, 2026-10-15)

The user chose to measure the uneven frame times before anything else.
`port/frametrace_3ds.c`:
- **Setup:** `S3D_FRAME_TRACE=<frames per phase>` in env.txt.
- **Start:** L+R+SELECT (it replaces the "mark" snapshot while the setting
  is on) or the remote `frametrace` command.
- **Phases:** `S3D_RT_DEDUP=7` (30 settle frames + N), then 0, then the
  setting is restored.
- **Recording:** events are timestamped into a 512 KB memory buffer, so
  nothing is written while recording. Afterwards the buffer goes to
  `sdmc:/3ds/star3d/frametrace.csv`.
- **Events:**
  - game thread: frame start, VIWaitForRetrace, fence and ring-full waits,
    boundary recorded;
  - render thread: idle, frame boundary, `C3D_FrameEnd`;
  - citro3d queue: GPU waits, screen swaps (and which thread did them),
    command list submits;
  - GSP event thread: P3D and PPF completions (`gspSetEventCallback` while
    the trace runs), top VBlank.
- **Analysis:** `tmp_scripts/frametrace.py frametrace.csv [rows]`.
  - Per phase: swap interval and game frame interval (mean, sd, p10/50/90,
    max, average frame-to-frame jump), VBlanks per swap, who swapped.
  - Game work / fence / ring waits, render idle, GPU waits, GPU idle (no
    command list queued).
  - A strip of consecutive frames.

**Azahar check (hub, 150 frames per phase):** the trace runs and the game is
unaffected (10 P3D completions per frame).
- Azahar is render-thread bound: the game thread waits for ring space 15.5
  ms (DEDUP 7) or 32.5 ms (0) per frame.
- Swap interval 78.3 ms (7) vs 83.4 ms (0), steady (sd ~3 ms). No
  alternation in the emulator.
- GPU idle 7.7 ms (7) vs 0.5 ms (0) per frame: Azahar's GPU is the faster
  side.
- Screen swaps are done mostly by the GSP thread's queue-finish callback.
  Presentation lands on VBlanks: 4-5 VBlanks per frame here.

**Hardware run (pending):** out/star3d_frametrace.3dsx.
- env.txt: `S3D_FRAME_TRACE=200` and `S3D_BENCH=0`.
- Hub, the usual spot with the user's settings, stand still, press
  L+R+SELECT once. Keep standing ~25 s, until the log says
  `frametrace: wrote N events`.
- Copy `sdmc:/3ds/star3d/frametrace.csv` together with the log.

**Hardware trace (7AED0EF9, New 3DS XL, hub, standing still, 2026-10-15).**
Files: `perf/hwlogs/7AED0EF9_frametrace.csv` / `.log`. Three findings:

1. **Late, uneven screen swaps with the filter on (the "unstable frame
   time").** Game frames came steadily every ~58 ms in both phases. The
   swaps did not:

   | | filter off | filter on |
   |---|---|---|
   | swaps done by | GSP queue-finish callback, all 199 | render thread 151, GSP 47 |
   | when | ~38.5 ms into each frame | at the next frame boundary |
   | VBlanks per swap | 3 (109) / 4 (79) | spread 1-7 |
   | swap interval p10-p90 | 55-66 ms | 38-84 ms |

   - Cause: pipelined, the swap is only done by the queue-finish callback
     (whole queue idle) or by `poll_swap` on the render thread (frame end,
     waits).
   - With the filter off, the GPU ran dry every frame, so the callback
     swapped right when the frame was done.
   - With the filter on, the render thread had queued the next frame's
     work, so the swap waited for the next `C3D_FrameEnd`: up to a frame
     late, at an uneven point.
2. **A ~25 ms GPU drain every ~9 frames with the filter on** (23 `queue_wait`
   in 200 frames, `gpu.queue_full_waits` 4-6 per window, 0 with the filter
   off).
   - The citro3d command queue (256 entries) is only emptied while the GPU is
     idle, and the render thread never saw it idle.
   - So the queue filled up and forced a full drain.
3. **0.3-1.5 s hitches at profiler reports** (f851: report at 47.995 s, next
   frame at 49.556 s; another 288 ms), in every build.
   - The ~110 report lines were written and flushed to the SD card on the
     game thread. The SD card stalls at times.
   - Warnings were flushed at once too.

**Fixes (build 29059F15, out/star3d_pacing.3dsx):**
- **Swaps on time:** gx_c3d_queue.c registers GSP P3D and PPF callbacks
  (and uses the VBlank0 one). Each checks `poll_swap`, so the swap happens as
  soon as the GPU is past the frame's transfers, from the GSP thread. The
  frame trace now gets its GPU events from these callbacks.
- **Queue recycling:** `C3Dx_Recycle()` empties an idle queue. The render
  thread calls it when it wakes from waiting for the game thread, by which
  time the GPU has usually caught up (Azahar: ~1 recycle per frame,
  `gpu.queue_recycles`).
- **Switch:** `S3D_RT_PACING`, default 3: bit 0 swaps from the callbacks,
  bit 1 recycles the queue. 0 gives the old behaviour.
- **Background log writer (os/log.c):**
  - Lines go to a 256 KB memory ring. A writer thread on core 0, one
    priority below the game thread, writes them out: at least once a second,
    at once for warnings, and when the ring is 1/4 full.
  - `log_flush()` still writes everything before it returns (debug paths,
    watchdog, mark). The profiler report uses the new `log_kick()`.
  - The crash handler drains the ring without locks. If the ring is full,
    the logging thread writes it out itself.
  - Azahar: log complete and in order.
- **Trace phases:** the frame trace now runs three: filter/pacing 7/3 (the
  new defaults), 7/0 (filter, old pacing), 0/3.

**Hardware test (pending):** same as the 7AED0EF9 trace, with
out/star3d_pacing.3dsx.
- env.txt: `S3D_FRAME_TRACE=200`, `S3D_BENCH=0`. L+R+SELECT in the hub,
  stand still ~35 s, until `frametrace: wrote N events`.
- Also play normally a while and say whether the frame time feels steadier.
  Hitches at the 3 s profiler report should be gone.

**Hardware result (29059F15, New 3DS XL, hub, standing still, 2026-10-15).**
Files: `perf/hwlogs/29059F15_frametrace.csv` / `.log`. The trace buffer filled
up (65,536 events), so the third phase has 139 frames instead of 200. User:
"definitely seems better", couldn't tell about fps.

| phase (dedup/pacing) | 7/3 (new defaults) | 7/0 (old pacing) | 0/3 |
|---|---|---|---|
| game frame mean | **54.8 ms (18.3 fps)** | 56.1 ms (17.8 fps) | 56.7 ms (17.6 fps) |
| swap interval sd | 4.4 ms | 14.0 ms | 2.7 ms |
| swap interval p10-p90 | 51-60 ms | 38-79 ms | 55-57 ms |
| VBlanks per swap | 3: 143, 4: 52 | 2-6 spread | 3: 84, 4: 54 |
| swaps by | GSP 199 | render 162, GSP 37 | GSP 139 |
| GPU queue waits / frame | 0.7 ms | 2.1 ms | 0 |
| game ring-full wait | 0.7 ms | 1.9 ms | 10.0 ms |

- **Pacing fixed:** the swap jitter is gone, and the swaps happen on the
  GSP thread.
- **FPS:** the filter plus the new pacing is now the fastest combination,
  ~2 ms per frame faster than either other phase (+0.5-0.7 fps, ~3%). Small,
  as expected while the GPU (~45-50 ms per frame) and the render thread
  (~46 ms) are nearly equal.
- **No SD hitches during the trace.** The long frames left in the log are
  at boot, the map loads, and one 387 ms frame in the hub before the trace.
- **Left:**
  - Every ~3 s, a ~73 ms frame: the profiler report waits for the render
    thread (`gxrec_report_drain`, 12-20 ms fence wait) so it can reset the
    render thread's counters. It could snapshot those instead.
  - About one forced queue drain per report window
    (`gpu.queue_full_waits` total 1).
- `S3D_RT_DEDUP=7` and `S3D_RT_PACING=3` stay the defaults.

**Tools:** tmp_scripts/wincmp.py (zones/counters of chosen report windows
side by side), tmp_scripts/dedup_ab.txt, dedup_verify.txt,
dedup_verify_boot.txt (Azahar drive scripts). The A/B runner in
port/diag_3ds.c is now a table (`k_ab_tests`: lean and dedup).

### Profiler report without the stall (2026-10-15)

Goal: remove the last regular stutter, a ~73 ms frame every ~3 s.

**The cause.** With the overlapped frame end, the report
(`os/profiler.c` `prof_report`) used to drain the render thread first
(`gxrec_report_drain`, a 12-20 ms fence wait). It had to: the report clears
every thread's counters, and the render thread was still adding to them.
Formatting the ~100 report lines then took another ~5.4 ms on the game
thread, all in one frame (Azahar: zone `prof.report` max 5.4 ms).

**The fix:**
- Each profiler entry now keeps **two halves** (`acc[2][THREAD_SLOTS]`).
  Threads add to `acc[s_buf]`.
- When a window ends, `prof_frame_end` stores the frame stats and flips
  `s_buf`.
- From the next frame end on, it writes the closed half's report, 12 lines
  per frame (`REPORT_LINES_PER_FRAME`), then clears that half. The next
  window can't close before the report is written.
- **Why no thread still writes the old half by then:**
  - The render thread has finished every command recorded before the flip.
    The frame end's `gxrec_fence` waits for the previous frame boundary, and
    that boundary was recorded after the flip.
  - Commands recorded after the flip were published after `s_buf` changed
    (barriers in `publish`).
  - Helper threads could only race if one slept a whole frame inside
    `prof_add`.
- The render thread's half runs one frame later than the game thread's. That
  only matters for a window's first and last frame.
- `prof_report()` (the GPU sweep in diag_3ds.c) still writes at once. Its
  caller drains first.
- `vi_3ds.c` now calls `gxrec_ops_report_drain`, which drains only in
  `GX_PROFILE_DETAIL` builds (for the per-opcode report).
- New zone `prof.report`: the report's formatting time. It is counted in the
  next window.

**Azahar (build 76B86AEE):** report lines arrive complete and in order.
`prof.report` costs at most 0.73 ms per frame, spread over 9 frames (was
5.4 ms in one frame, plus the drain).

**Hardware check (pending):** the ~73 ms frame every 3 s should be gone. Look
at the frame trace or the per-window `max`, and `rt.fence_wait` max.

### Vertex attribute pruning (S3D_VSH_FIXATTR, 2026-10-15)

Lever 1 of "Next levers" in the vertex shader results above. The bench found
**input attributes are most of the vertex cost** (~0.027 us per attribute
per vertex, whatever their size). Every draw loaded all 7.

**How it works** (gx/gx_render.c, "Vertex attribute pruning"):
- An attribute the draw doesn't read, or whose value is the same in all of
  its vertices, becomes a PICA **fixed attribute**: a constant, set once with
  `C3D_FixedAttribSet`, no load. The buffer skips its bytes with padding
  components (0xC-0xF), so each buffer's components still fill its stride
  (bug 12).
- Fixed, not dropped: the shaders multiply every input they read, even by a
  zero weight. An unloaded input could hold NaN.
- Per attribute:

  | attribute | fixed when | value |
  |---|---|---|
  | texcoord slot s | the slot is skipped (marker), takes a position/normal source, or has no texgen (`s_tc_fix`, set in `upload_unit_texmtx`) | 0 |
  | normal | the unlit or warp shader, and no computed slot uses the normal source (they only read it for texgen) | 0 |
  | matrix index | one value across the cached segment | that index |
  | color | one value across the cached segment | that color |

- gx_dlcache.c `segment_uniform` computes the per-segment values once, when
  the list is stored (`DlSegment.uni`, `GxVtxUniform`). `play_entry` hands
  them over with `gx_render_set_vtx_uniform` before each segment's draw.
  Immediate batches (~2% of hub triangles) only get the state-based ones.
- Draw sites now call `set_vsrc` instead of binding buffers.
  - `apply_vsrc` (at the top of `draw_eyes`) picks the mask, binds the
    `C3D_AttrInfo` for (layout, mask), sets the fixed values and binds the
    padded buffers. The AttrInfo is cached in a 3 x 128 table, built lazily.
  - The mask is picked per pass, so multipass and water passes (which change
    a slot's texgen) get their own.
  - Raw draws (copy clears) use no pruning.
- **Switch:** `S3D_VSH_FIXATTR`, a mask of the attributes allowed. Bits 1
  color, 2 matrix index, 3-5 texcoord slots, 6 normal. Default 0x7E (126),
  0 = off. Re-read when settings change, so the remote `set` changes it live.
- **A/B:** `S3D_VSH_FIXATTR_AB=<frames>` alternates 126 and 0 and logs
  `fixab` lines (port/diag_3ds.c table).
- **Counters** (indices x eyes per frame): `gx.vidx_fix_clr`, `_idx`,
  `_tc0`, `_tc1`, `_tc2`, `_nrm`.
- **Bench rows** (boot, `bench: vertex shaders`). Each answers whether fixed
  attributes are free on hardware:
  - `io: 7 in, 4 of them fixed`, against `io: 7 in` and `io: 3 in`;
  - `unlit lean, 1 slot, 4 fixed`, against `unlit lean, 1 affine slot`;
  - `lit lean, 1 light, 1 slot, 3 fixed`, against `lit lean, 1 light, 1 slot`.

**Azahar (build 0BA404E8, hub save state; the same code is in 21DE01AE).** In-session screenshots with
`S3D_VSH_FIXATTR` 126 / 0 / 126 (tmp_scripts/fix_ab.txt) look the same. No
warnings. Per frame, of 232k indices (133k lit, 100k unlit):

| fixed attribute | indices |
|---|---|
| color | 179k |
| matrix index | 58k |
| tc0 | 123k |
| tc1 | 227k |
| tc2 | 160k |
| normal | 100k |

That is **3.65 of the 7 attributes fixed per index** on average. If fixed
attributes are free on hardware, ~0.1 us per vertex at ~130k vertices would
be ~10+ ms of GPU time. The bench rows say whether they are.

**Risk:** fixed attributes alongside loaders are standard PICA (citro3d
`AttrInfo_AddFixed`), but this port hasn't used them before. If the GPU
hangs or models look wrong, set `S3D_VSH_FIXATTR=0`.

**Hardware test (pending):**
1. Boot with a clean env.txt. Read the `bench: vertex shaders` lines (the
   three new rows).
2. Visual: hub, title screen and Fox should look normal. Check models with
   environment maps or vertex colors especially.
3. env.txt `S3D_VSH_FIXATTR_AB=300` and `S3D_BENCH=0`. Hub, the usual spot,
   stand still ~60 s. Compare the `fixab` lines (frame and GPU).

### Shadows and Fur "Always": what they cost (2026-10-15)

A code survey, plus the earlier measurements. Paths in the patched game are
under `build/us_3ds/patched/main/`.

**Shadows** (`S3D_SHADOWS`, read live; patches/main/lightmap.c.sed gates
`queueObjectShadow` / `renderObjectShadowTexture`):
- Blob shadows (`renderObjectShadowTexture`, newshadows.c:541): a full
  `objRender` of the object into a 128x128 offscreen target, one `GXCopyTex`,
  then a blur (mip sampling, gx_copy.c `port_copytex_blur`).
- Projected shadows (`renderShadows`, newshadows.c:620):
  - Up to 100 casters. The first 8 (`NEW_SHADOW_MAX_CAST_TEXTURES`) get a
    render-to-texture pass each.
  - Sizes by rank: 256 for casters 0-2 (a 512 request is clamped to 256),
    128 for 3-4, 64 for 5-7.
  - Each receiver redraws the ground under it with the shadow texture.
- No dedicated counter. Related ones: `gx.offscreen`, `gx.copytex`,
  `gx.gpu_blur_calls`.
- **Measured:** 654FC0A2 sweep: frame -7 ms, GPU -5 ms, ~50 draws and ~5k
  triangles. FA1085B8: frame -6 ms, GPU -4.1, -36 draws.
- **The cost is draws, not pixels:** the GPU is draw/vertex-bound (~47 us per
  draw) and not fill-bound. Smaller shadow textures would not help.

**Fur** (`S3D_FUR_MODE`: 0 off, 1 Cutscene = default, 2 Always; port/fur_3ds.c):
- Every shell is a full model pass over the same display lists with scaled
  joint matrices. A shell costs the whole fur mesh in vertices.
- Always forces the player's shells in gameplay (up to 3 by distance).
  Budget `port_fur_step` (`S3D_FUR_BUDGET`): 8 near, 4 mid, 2 far, 0 far in
  stereo.
- Not budgeted: `objRenderFuzzShells` (16 layers) and
  `objRenderFuzzShadowShells` (4 layers) called from player.c.
- **Measured:** FA1085B8: Always +1-2 ms GPU, +13-24 draws. A81DF396
  (profile): Always costs ~5 ms frame, 3-4 ms replay, 8 ms `game.sceneRender`
  (game-thread time, not GPU).

**Levers, if the A/B prices them high:**
- A cap on shadow render-to-texture casters (`S3D_SHADOW_CASTERS`, in place
  of the fixed 8).
- Refreshing shadow textures every other frame.
- Lower fur budgets (e.g. 4/2/0).
- Applying the budget to the two unbudgeted shell loops.
- A distance cutoff for Always.
- Every shadow and fur draw also gets cheaper with attribute pruning, so
  they are priced again with pruning on.

**A/B switches** (port/diag_3ds.c table): `S3D_SHADOWS_AB=<frames>` (tag
`shadowab`, on/off) and `S3D_FUR_MODE_AB=<frames>` (tag `furab`, Always vs
Cutscene). Only the first enabled A/B runs, so use one per run.

**Hardware test for build 21DE01AE** (all of this session's changes; no
.cia):
1. Boot with a clean env.txt. Read the bench lines, and check visuals (hub,
   title, Fox).
2. Run A: `S3D_VSH_FIXATTR_AB=300`, `S3D_BENCH=0`. Hub spot, stand still
   ~60 s.
3. Run B: `S3D_SHADOWS_AB=300`, `S3D_BENCH=0`. Same spot, ~60 s.
4. Run C: `S3D_FUR_MODE_AB=300`, `S3D_BENCH=0`. Same spot, ~60 s.
5. The log keeps only 2 files, so copy Star3D.log after each run.
6. Throughout: no ~73 ms frame every 3 s (report stall).

**Hardware result (21DE01AE, New 3DS XL, 2026-10-15).** Logs:
`perf/hwlogs/21DE01AE_fixab.log`, `_shadowab.log`, `_furab.log`. Each run
started its A/B at boot, so the first ~2 phases fell on loading and the
title scene (map 63). The hub (map 7) only came in later phases. No boot
bench run (S3D_BENCH=0 in all three).
- **Attribute pruning is a large regression on hardware.**

  | run A, window | pruning | GPU (p3d) per frame |
  |---|---|---|
  | title scene | off | 22 ms |
  | title scene | on (0x7E) | 102-129 ms |
  | hub | off | 42 ms |
  | hub | on | 105-129 ms, ~190 ms at the end |

  Frames follow the GPU (hub: ~50 ms off, 110-195 ms on). Azahar showed no
  difference, since it doesn't model GPU time.
- Runs B and C ran with pruning on (it was the default), so their numbers
  are inflated and can't price shadows or fur:
  - B, hub: shadows on ~115-130 ms GPU, off ~70 ms;
  - C, hub: no clear fur difference, and GPU time crept up over the run
    (115 -> 180 ms).
  They need a rerun with pruning off.
- In the frames with pruning off, the report stall did not show up as a
  regular long frame.

- **The user saw many vertex explosions in these runs** (all had pruning
  on). So on hardware the fixed-attribute setup corrupts positions; Azahar
  renders it correctly. The ~3x GPU time is most likely the exploded
  triangles' fill, not a real cost of fixed attributes. Pruning is
  **broken on hardware** until debugged. Suspects:
  - the matrix-index constant reaches the shader wrong (explosions only
    with 0x7E);
  - the loader steps over padding differently (explosions with 0x78 too).

**Follow-up build E7529984:**
- `S3D_VSH_FIXATTR` now defaults to **0** (off).
- The fixattr A/B cycles three values: 0, 0x78 (only the zero-valued
  texcoords and normal, whose fixed values are written once) and 0x7E (all).
- A/B runs now start 150 frames after map `S3D_AB_MAP` loads (default 7, the
  hub; -1 = at boot). The table takes 2 or 3 values per test.
- New counters: `gx.fixattr_writes` (fixed values written) and
  `gx.attrinfo_changes` (C3D_SetAttrInfo calls).
- Azahar hub:
  - fixed-value writes are only 3-10 per frame, so per-draw value changes
    are probably not the cost;
  - attrinfo changes are ~100 per frame with pruning off (layout switches
    std/packed/dyn) and 150-230 with it on.
  - Either the extra attribute-format writes stall the hardware, or fixed
    attributes are slow in themselves. The boot bench rows (fixed once, no
    changes) tell which.
- **Hardware test:**
  1. Boot with a clean env.txt (the bench runs). Then take the log; the
     bench lines are early.
  2. `S3D_VSH_FIXATTR_AB=300`, `S3D_BENCH=0`. Go to the hub spot and stand
     still: the A/B starts by itself after the hub loads. ~3 cycles of 3
     phases, ~3 min.
  3. `S3D_SHADOWS_AB=300` and then `S3D_FUR_MODE_AB=300` (each with
     `S3D_BENCH=0`), same spot, ~2 min each.

### Attribute dropping instead of fixing (S3D_VSH_DROPATTR, build 5A5EC2CB, 2026-10-15)

Fixed attributes explode vertices on hardware (21DE01AE). This is a safer
way to the same saving, for the two attributes where it is possible.

**The idea.** When the shader skips texture slot 1 or 2 (the texsrc marker,
`S3D_VSH_LEAN` bit 1), it never reads that slot's texcoord input. All three
shaders read `intc1` / `intc2` only inside the non-skipped branch. So the
attribute need not be loaded at all: no loader, no fixed attribute, nothing
for the hardware to interpret.

**How it works** (gx/gx_render.c):
- `draw_dropped_attrs` drops tc1 / tc2 when `s_tc_skip[s]` (set in
  `upload_unit_texmtx`) and `S3D_VSH_DROPATTR` allows it.
- `attr_info(layout, fixed, drop)` leaves the dropped loaders out. The
  loaders after them move down one index. The AttrInfo permutation still maps
  each to its own shader input, so v4 / v5 are just not written.
- `va_comp` gives each buffer component its loader index, and pads over
  dropped (or fixed) attributes. This is the same padding the dyn layouts
  already use on hardware, and the bench's "3 in (52 B stride)" test.
- Dropping and fixing never mix: with drops on, the fixed path is off for
  that draw.
- **Switch:** `S3D_VSH_DROPATTR`, mask 0x10 (tc1) | 0x20 (tc2). Default 0
  (off) until hardware says yes. Re-read live.
- **A/B:** `S3D_VSH_DROPATTR_AB=<frames>` (tag `dropab`, values 0 and 48).
- **Counters:** `gx.vidx_drop_tc1`, `gx.vidx_drop_tc2`.
- **Bench rows:** `unlit lean, 1 slot, tc1+tc2 dropped` and
  `lit lean, 1 light, 1 slot, 2 dropped`. Compare them with the matching
  lean rows without drops.

**Azahar (hub save state, in-session shots with 0 / 48 / 0,
tmp_scripts/drop_ab.txt):** looks identical. tc1 is dropped on 152k and tc2
on 158k of 232k indices per frame, ~1.3 attributes per vertex on average. At
the bench's ~0.027 us per attribute per vertex, the expected saving is ~4 ms
of GPU time in the hub.

**Hardware test for build 5A5EC2CB** (no .cia; fixed and dropped both off by
default, so the normal game is unchanged):
1. **Boot with a clean env.txt** (the bench runs). Take the log; the
   `bench: vertex shaders` lines are early. They have the fixed rows and the
   new dropped rows.
2. `S3D_VSH_DROPATTR_AB=300`, `S3D_BENCH=0`. Hub spot, stand still ~2 min
   (starts by itself after the hub loads). **Watch for vertex explosions**
   and note whether they come and go with the phases.
3. `S3D_SHADOWS_AB=300`, `S3D_BENCH=0`. Same spot, ~2 min.
4. `S3D_FUR_MODE_AB=300`, `S3D_BENCH=0`. Same spot, ~2 min.
5. Copy Star3D.log after each run.

**Hardware result (5A5EC2CB, New 3DS XL, 2026-10-16).** Logs:
`perf/hwlogs/5A5EC2CB_boot_bench.log`, `_dropab.log`, `_shadowab.log`,
`_furab.log`.

Boot bench (us per vertex):

| test | plain | variant |
|---|---|---|
| io: 7 in vs 7 in with 4 fixed | 0.160 | 0.160 |
| unlit lean 1 slot vs 4 fixed | 0.176 | 0.173 |
| unlit lean 1 slot vs tc1+tc2 dropped | 0.176 | 0.170 |
| lit lean 1 light 1 slot vs 3 fixed / 2 dropped | 0.247 | 0.247 / 0.247 |
| io: 7 in vs 3 in (separate 3-input shader) | 0.160 | 0.053 |

- **Attribute pruning is a dead end.** Fixed attributes cost the same as
  loaded ones. Dropping loaders the shader doesn't read saves ~3% unlit and
  nothing lit.
- In-game A/B agrees: `dropattr` 0 / 48 / 0 gave GPU 48.9 / 49.5 / 49.5 ms,
  no gain. The user saw no explosions with dropping.
- So the "~0.027 us per attribute" model was wrong. The io 7 -> 3 gap comes
  from the shader reading fewer inputs (a different program), not from the
  loader. Attributes that are loaded but not read are nearly free. That is
  why the lean rows (slots 1-2 skipped, their inputs unread) already got
  most of it.
- Fixed attributes stay broken in game as well (explosions). Both switches
  stay off by default.

**Shadows A/B** (hub, `S3D_SHADOWS` 1 / 0 / 1):

| shadows | frame | GPU |
|---|---|---|
| on | 66.3 ms | 44.4 ms |
| off | 59.3 ms | 37.1 ms |
| on | 62.9 ms | 46.2 ms |

Shadows cost **~7-8 ms GPU and ~5 ms frame**: the biggest single setting
left.

**Fur A/B** (hub, `S3D_FUR_MODE` 2 / 1, one pair):

| fur mode | frame | GPU |
|---|---|---|
| Always | 67.5 ms | 52.4 ms |
| Cutscene | 66.8 ms | 50.8 ms |

Fur "Always" costs only **~1.6 ms GPU and ~0.7 ms frame** in a normal
build. The earlier ~5 ms was profile-build game-thread time.

**Other facts from these logs:**
- Lights are rare: `gx.vidx_x_lights` 48k per 154k lit indices, so ~0.3
  lights per lit vertex. Not a lever.
- VRAM free in the hub is ~610 KB. With AA off, the big target is 240x800,
  not 480x800. The hub's per-frame vertex data is ~5-6 MB, so VRAM could hold
  only ~10% of it (~1 ms at the bench's -40%). Not worth it now.

**Next:**
- Shadow levers, with an A/B each: cap the render-to-texture casters (now
  8), and refresh caster textures every other frame.
- Remove the pruning code (fixed and drop paths, and their bench rows), or
  keep it off.

### Shadow levers and pruning cleanup (build ECE5E4A8, 2026-10-16)

The user chose "shadow levers + cleanup".

**Cleanup:** the attribute pruning code is gone.
- Removed: the fixed and drop paths, `GxVtxUniform` / segment values, the
  `S3D_VSH_FIXATTR` / `S3D_VSH_DROPATTR` switches and A/Bs, and their bench
  rows.
- Kept: the vertex-source restructure (`set_vsrc`, then `apply_vsrc` in
  `draw_eyes`, one cached AttrInfo per layout).
- The A/B runner keeps its improvements: 2-3 values, start after the hub
  loads.

**Shadow levers** (gx/gx_init.c, hooks in patches/main/newshadows.c.sed
`renderShadows`):
- `S3D_SHADOW_TEXTURES` (0-8, default 8): how many casters get a
  render-to-texture pass (a full object draw plus a copy each). The others
  use their static texture, as casters past the 8th always did.
- `S3D_SHADOW_REFRESH` (default 2 since the ECE5E4A8 HW test; 1 = every
  frame): with N >= 2, a caster texture is redrawn
  only every Nth frame, staggered by slot, and only while the same object
  keeps the slot (`port_shadow_reuse`). Otherwise last frame's copy is
  sampled again (copies live 300 frames). The object pass and the copy are
  skipped, but the matrices still follow the object.
- **Counters:** `shadow.tex_drawn` and `shadow.tex_reused` (per frame).
- **A/Bs:**
  - `S3D_SHADOW_TEXTURES_AB=<frames>` (tag `shtexab`, values 8 / 3 / 0);
  - `S3D_SHADOW_REFRESH_AB=<frames>` (tag `shrefab`, values 1 / 2).

**Azahar** (hub save state, tmp_scripts/shadow_ab.txt):
- 8 offscreen passes per frame, of which 3 are caster passes
  (`shadow.tex_drawn` 3).
- Textures 0: offscreen 8 -> 5. Fox's detailed shadow becomes the game's
  large round blob, a visible quality loss.
- Refresh 2: drawn 1.5 + reused 1.5 per frame, offscreen 6. Looks the same
  as before.
- **Not covered:** the other 5 offscreen passes. They don't come from
  `renderShadows`. `renderObjectShadowTexture` draws into a per-frame texture
  ring, so the reuse trick doesn't fit there. Finding out what the 5 are is a
  possible next step.

**Hardware test (build ECE5E4A8, no .cia; defaults unchanged):**
1. `S3D_SHADOW_REFRESH_AB=300`, `S3D_BENCH=0`. Hub spot, stand still ~2 min.
   Then walk around a bit with `S3D_SHADOW_REFRESH=2` (or set it from the
   remote) and look for shadow lag or flicker.
2. `S3D_SHADOW_TEXTURES_AB=300`, `S3D_BENCH=0`. Same spot, ~3 min (8 / 3 /
   0).
3. Compare the frame and GPU times with the 5A5EC2CB shadows A/B (on 66.3 /
   44.4 ms, off 59.3 / 37.1 ms).

**Hardware results (ECE5E4A8, hub standing still, 270-frame phases;
logs perf/hwlogs/ECE5E4A8_shrefab.log and ECE5E4A8_shtexab.log):**
- The hub has exactly 3 casters (`shadow.tex_drawn` 3), so textures 3 is the
  same as 8.
- Textures 8 / 3 / 0: frame 50.6 / 50.9 / 46.3 ms, GPU 42.8 / 42.9 / 35.2 ms.
  Textures 0 saves 7.5 ms GPU but only ~4 ms of frame time (the GPU is no
  longer the only wall).
- Refresh 1 / 2 (4 pairs in the hub): frame 52.6 / 49.0 ms, GPU 42.5 /
  39.9 ms. Refresh 2 draws 1 and reuses 1 per frame.
- Walking around with refresh 2: no shadow lag or flicker (user check).
- **Decision:** `S3D_SHADOW_REFRESH` default is now 2. `S3D_SHADOW_TEXTURES`
  stays 8; 0 is only a low-quality option (blob shadows), and its frame gain
  is about the same as refresh 2.
- Open: the 5 other offscreen passes per frame (not `renderShadows`, see
  "Not covered" above) are the next shadow-side target. Done below, in
  "Other offscreen passes".

### Back-face culling (build 94783369, 2026-10-16)

The port had culling compiled out (`GX_ENABLE_CULLING 0`, "until the winding
is verified"), although the game sets a GX cull mode on its draws.
- **Setting:** `S3D_CULL` (gx/gx_render.c `apply_pixel_state`, read live):
  0 = off (default), 1 = the game's cull mode, 2 = the opposite winding
  (wrong: it drops the front faces, only for checks). Touch menu row
  "Back-face culling (test)" toggles 0 / 1. A/B `S3D_CULL_AB=<frames>`
  (tag `cullab`, 0 / 1).
- **Winding:** GX front faces come out counter-clockwise on the PICA, so GX
  back maps to `GPU_CULL_FRONT_CCW` and GX front to `GPU_CULL_BACK_CCW`. The
  straight mapping (now value 2) ate the ground, walls and Fox in the hub.
  The strip/fan/quad conversion (gx_vtx.c `emit_indices`) keeps the winding;
  mirrored matrices need nothing extra (the GameCube culls on screen winding
  too).
- **Azahar check (mode 1):** title screen, attract scenes (the offscreen TV
  picture included) and hub look the same as with culling off
  (build/emu/cs_*.png, cr_*.png; scripts tmp_scripts/cull_scenes.txt,
  cull_rcp.txt).
- **Census, hub:** cull none ~900, back ~775, front 5 states, so about half
  the geometry can lose its back faces.
- **What it can save:** the PICA culls after the vertex shader, so vertex
  shading (~27-31 ms of the hub's GPU time) stays. The target is the ~10 ms
  of GPU time that vertex shading, state changes and fill don't explain
  (per-triangle setup and clipping, 74k triangles; see "Per-draw benchmark
  results"). Azahar can't show the gain.
- GX_CULL_ALL still draws (as before); only 0 states use it in the hub.

### Other offscreen passes (build 94783369, 2026-10-16)

The 5 non-shadow offscreen passes per hub frame are all 32x32 passes of
`Rcp_UpdateDistortionTextures` (foxhollow main/rcp_dolphin.c, called from
lightmap.c each frame). New counters `gx.offscreen_32/64/128/256` (per
target size) show it: hub 5 x 32 + 1 caster pass (refresh 2), none at 128
(`renderObjectShadowTexture`, shadow type 4, is not used in the hub).
- **What they are:** lighting textures, not distortion. Each slot draws a
  16-strip hemisphere mesh (~540 vertices, positions + normals, one display
  list) lit by the sky, then copies it to a 32x32 texture that material
  shaders sample (`shaderInit`: an object's light slot and the reg2 slot).
  - mode 1 slots (up to 3): sun and moon specular maps;
  - mode 0 slots (up to 3): an object's light colour (`gxLoadObjectLights`).
  - Each pass: a render target switch and clear, ~540 lit vertices, a copy.
- **Lever:** `S3D_RCP_REFRESH=N` (default 1; gx/gx_init.c `port_rcp_reuse`,
  hooks in patches/main/rcp_dolphin.c.sed). With N >= 2 a slot is redrawn
  every Nth frame, staggered by slot, while it keeps its model, mode and
  group. Otherwise last frame's copy is sampled again (copies live 300
  frames). The light setup still runs; the pass and the copy are skipped.
  Counters `rcp.tex_drawn` / `rcp.tex_reused`. A/B `S3D_RCP_REFRESH_AB`
  (tag `rcpab`, 1 / 2).
- **Azahar (hub):** refresh 2 draws ~2.5 and reuses ~2.5 per frame; the
  picture looks the same. Expected gain is small (5 x ~540 vertices ≈ 0.6 ms
  of vertex work plus 5 target switches), so the hardware run decides.

**Hardware test (build 94783369, no .cia; defaults unchanged: culling off,
Rcp refresh 1, shadow refresh 2):**
1. `S3D_CULL_AB=300`, `S3D_BENCH=0`. Hub spot, stand still ~2 min.
2. `S3D_RCP_REFRESH_AB=300`, `S3D_BENCH=0`. Same spot, ~2 min.
3. Then play a bit with "Back-face culling (test)" on in the touch menu and
   look for missing walls, holes in models, missing effects or HUD parts
   (Azahar only covered the title, the attract scenes and the hub).
4. Baseline for this build: the off phases of runs 1-2 (shadow refresh 2 is
   now the default, so they should land near ECE5E4A8's refresh 2 phases,
   ~49 ms / ~40 ms GPU).

**Hardware result (94783369, 2026-10-16, logs
`perf/hwlogs/94783369_cullab.log` / `94783369_rcpab.log`):** both levers
are **no frame gain**. Each A/B had 4 phases per value, 270 frames each, in
the same hub spot. The numbers barely moved between phases (±0.2 ms), so
these differences are real but small.

| A/B | value | frame | GPU (`p3d`) | notes |
|---|---|---|---|---|
| cull | 0 (off) | 47.83 ms | 38.79 ms | |
| cull | 1 (on) | 47.83 ms | 38.68 ms | same triangles/draws/commands |
| rcp refresh | 1 | 47.87 ms | 38.96 ms | `rcp.tex_drawn` ~4.4-5 |
| rcp refresh | 2 | 47.78 ms | 38.47 ms | drawn 2.2 + reused 1.8 |

- **Culling:** 0 ms. That fits "the GPU is vertex-bound": culling happens
  after vertex shading, and back faces were already mostly depth-rejected.
  The log can't prove that the cull state actually reached the GPU (there's
  no counter for it). The user toggled it in game and saw nothing wrong.
  Keep it off by default (nothing to gain). It stays available as a test
  row.
- **Rcp refresh 2:** GPU -0.5 ms, frame -0.1 ms. That matches the ~0.6 ms
  estimate. The user saw no visual difference. It's too small to matter, so
  the default stays 1 (it could be flipped to 2 at no cost later).
- **New baseline: the frame is no longer GPU-bound.** Hub 47.7 ms /
  21 fps, with shadow refresh 2 now the default. The profiler per frame:

  | thread | time per frame |
  |---|---|
  | game thread: `game.sceneRender` | 38.1 ms |
  | game thread: `game.logic_us` | 7.2 ms |
  | game thread: post / boundary / end | ~1.4 / 3.6 / 2.3 ms |
  | render thread: `rt.replay` | 40.6 ms |
  | GPU: `p3d` | 38.8 ms (+3.4 ms `ppf`) |

  `rt.space_wait` almost never shows up, so the game thread is not waiting
  on the ring. The game thread is now the longest of the three, about
  ~9 ms above the GPU. The render thread comes next, then the GPU. Any
  further GPU-only lever (like these two) won't move the frame time. The
  next work has to cut game-thread CPU: scene traversal and GX translation
  inside `game.sceneRender`, plus logic. Render-thread replay has to come
  down too, right after.

### Where the game thread's time goes (build A8BA7DC0, HW done 2026-10-16)

Across the last 14 hardware logs, `game.sceneRender` has moved one-for-one
with GPU time (7AED0EF9 45.4 / 48.2 ms, 29059F15 41.7 / 45.2, ECE5E4A8
39.6 / 40.4, 94783369 38.2 / 38.8). Before step 4 the same work measured
24-27 ms. Real game work would not shrink with GPU-only changes, so part of
those 38 ms is probably not game work. Three candidates:
1. **Waits:** waits on the render thread or GPU that no timer covers.
2. **Preemption:** core 0 running something else. The libctru GSP event
   thread probably runs on core 0 at priority 0x1A (from memory of the
   libctru source, not checked), above the game's 0x30, and it
   wakes for every P3D/PPF/PSC completion to start the next queue entry and
   run our swap callbacks.
3. **Memory contention:** core 2's replay and the GPU slowing core 0's
   memory access.

Build A8BA7DC0 measures all three. It is a normal build with everything off
unless `S3D_PROBE=1` is set:
- **`game.scene_own_us`** (every build): `sceneRender` minus the time the
  game thread spent replaying inline (mode 1) or waiting on the render thread
  (`rt.space_wait`, `rt.sync_wait`). The waits are added up in
  `g_rt_game_wait_ticks` (gx_rec.c). The A/B lines print it as "own".
  Azahar smoke test: mode 1 `sceneRender` is 85 ms because a mid-frame ring
  drain replays inline, not at the frame end as frame_zones.c says. Own time
  is ~36 ms in both modes, so own time is the number that compares.
- **Probe slices** (port/probe_3ds.c, `S3D_PROBE=1`, `S3D_PROBE_US`
  default 300). Each slice runs at three points per frame: at the frame
  start, just before `sceneRender` and just after it. All three are outside
  logic and `sceneRender`, but they add ~1.8 ms to the frame.
  - The ALU slice spins on the system tick. Any gap > 5 us is time taken
    from the game thread: `probe.stolen_us` / `probe.alu_us`,
    `probe.gaps`, plus `probe.stolen_us.<where>` per point.
  - The memory slice reads one word per cache line from a 4 MB buffer,
    bigger than the 2 MB L2: `probe.mem_us` / `probe.mem_lines` count clean
    chunks only, and `probe.mem_bad_chunks` counts the rest.
  - Boot baseline: taken before the GPU and render thread start, logged as
    "probe on: ... boot baseline". Azahar: 0.3% stolen, 38 ns per line
    (emulated timing, so only hardware means anything).
- **GSP event counts** `gsp.p3d`, `gsp.ppf`, `gsp.vblank0/1` per frame,
  counted in our GSP callbacks (gx_c3d_queue.c). PSC and DMA events are not
  hooked.
- **Coarse game timers** (perf/perf_lite.c, every non-PERF_WRAP build, live
  only with `S3D_PROBE=1`): `perf.objRender`, `renderMapBlock`,
  `sceneDrawTransparentPolys`, the fur and shadow functions, `drawGlow`,
  `expgfx_renderSourcePools`, sky, reflections, `Rcp_UpdateDistortionTextures`,
  `Obj_UpdateAllObjects`, `updateLights` and the joint matrices. Values are
  CPU ticks per frame (268,112 per ms), and nested timers overlap. There
  are no per-draw, matrix or malloc wrappers (those made PROFILE=1 ~50%
  slower).
- **A/B `S3D_RENDER_THREAD_AB`** (tag `rtab`, mode 2 / 1): if own time drops
  a lot in mode 1 (core 2 idle), it's memory contention. A/B lines now also
  print `sceneRender` and own time.

Reading the result:
- If own time is well below `sceneRender`, the waits are uncovered: the
  wall is render-thread replay or the GPU, not game work.
- If `stolen_us / alu_us` is high (e.g. 15-25%), the GSP thread or other core
  0 threads are taking that share of `sceneRender`. That share
  multiplied by 38 ms is the cost. Fixes would be fewer queue entries (544
  split flushes per frame) or moving the GSP thread off core 0.
- If memory ns/line in game is well above the boot baseline, and own time
  drops in mode 1, it's contention.

**Hardware test (build A8BA7DC0, .3dsx only):** env.txt
`S3D_PROBE=1`, `S3D_RENDER_THREAD_AB=300`, `S3D_BENCH=0`. Hub spot, stand
still ~2.5 min. Analysis: `tmp_scripts/phase_cmp.py <log> x
game.sceneRender game.scene_own_us rt.replay probe.alu_us probe.stolen_us
probe.mem_us probe.mem_lines gsp.p3d gsp.ppf perf.objRender ...` (it splits
by `set S3D_` lines).

**Hardware result (2026-10-16, A8BA7DC0, New 3DS XL, hub spot, standing
still 2.5 min, log `perf/hwlogs/A8BA7DC0_rtab_probe.log`):** 4 phases of
mode 2 and 3 of mode 1, 270 frames each. The phases agree within ±0.5 ms.

| rtab line | frame | GPU | sceneRender | own |
|---|---|---|---|---|
| mode 2 (render thread) | 50.9-51.3 ms | 40.7-41.8 ms | 39.7-40.0 ms | 39.7-40.0 ms |
| mode 1 (inline replay) | 83.6-84.8 ms | 36.2-36.8 ms | 68.5-69.9 ms | 33.6-33.9 ms |

Probe and GSP numbers per frame (`phase_cmp.py` averages of the profiler
reports, so slightly different from the rtab lines):

| | boot baseline | mode 1 | mode 2 |
|---|---|---|---|
| ALU stolen (`stolen_us / alu_us`) | 0.1% | 4.4% | 4.8% |
| memory ns per line (`mem_us / mem_lines`) | 66 | 108 | 128 |
| `gsp.p3d` / `gsp.ppf` events | | 9.0 / 14.0 | 8.8 / 13.8 |
| `game.logic_us` | | 5.7 ms | 6.7 ms |

What this says about the three candidates:
1. **Waits: ruled out.** In mode 2, own time equals `sceneRender` (no
   `rt.space_wait` / `rt.sync_wait`). The 40 ms really is game-thread work.
2. **Preemption: small.** ~4.5% of core 0 is taken (vs 0.1% at boot), the
   same in both modes, so ~1.7 ms of the 40. Only ~23 GSP events per frame,
   so the GSP thread is not the problem (split flushes are ~2 per frame now,
   not 544). Not worth chasing.
3. **Memory contention: real but not the main cost.** Memory reads are
   1.6x slower than boot with the GPU running (mode 1) and 1.9x with the
   render thread replaying on core 2 (mode 2). Running the render thread in
   parallel makes the game thread's own work **+6 ms (+18%)** slower (33.8 ->
   39.8 ms), and logic +1 ms. Mode 2 still wins by far (51 vs 84 ms frame),
   so this is the price of the overlap, not a reason to drop it.

**Conclusion:** even with no contention at all, the game thread's own scene
work is ~34 ms. That alone is above the 33 ms budget for 30 fps. The 1:1
tracking with GPU time is most likely because the game thread's cost scales
with what gets drawn (GX calls recorded per draw and per immediate-mode
vertex), the same as the GPU's. It is not a hidden wait.

Coarse timers in mode 2 (CPU ticks / 268,112, ms per frame; mode 1 numbers
are inflated by inline replay inside them, so only mode 2 is used here):

| timer | ms/frame | notes |
|---|---|---|
| `objRender` | 18.8 | all objects, including fur and shadows below |
| `sceneDrawTransparentPolys` | 14.8 | overlaps with `objRender` (transparent objects) |
| `renderMapBlock` | 8.2 | static map geometry |
| `Obj_UpdateAllObjects` (logic) | 5.7 | |
| `playerRenderFuzz` | 4.5 | contains `objRenderFuzz` (4.2) |
| `drawGlow` | 2.5 | |
| `objShadowRender` | 1.8 | shadow refresh 2 |
| `joint_matrices` | 1.1 | |
| `objDrawGroundShadow` | 0.5 | |
| sky, water, particles, Rcp, reflections, expgfx | < 0.4 total | |

`objRender` + `renderMapBlock` + `drawGlow` already cover ~29.5 of the
~40 ms. That leaves at most ~10 ms for the rest of the traversal (map
culling, transparent sorting, the parts of transparent polys outside
`objRender`). The player's fur alone is 4.5 ms of game-thread CPU (shells
built per frame), well above the ~0.7 ms frame cost measured for
"Fur Always" (that A/B only compared fur modes, not fur vs no fur).

**Next (proposed):**
- **Split `objRender` and `renderMapBlock`** with a second level of timers
  (model setup / matrices, skinning, display-list submit, immediate-mode
  vertex submit) and counters of GX vertices and display-list bytes per
  timer. That says whether the cost is in the game code or in our GX
  recording wrappers (fixable in the port).
- **Fur:** see how `objRenderFuzz` builds its shells (immediate mode per
  vertex, per shell?) and look at caching or a cheaper submit path. 4.5 ms
  of the game thread plus its replay time on core 2.
  Done 2026-10-17 as fast shells (FUR_PLAN.md "Fast shells", build
  7C7F1EFF, `S3D_FUR_FAST_AB`). Title A/B: sceneRender and replay -1.1 ms
  each, GPU -0.3 ms.
- **Map blocks:** static geometry, 8.2 ms on core 0 plus its replay. A
  "record once, replay many" cache of the translated commands would cut
  both threads. Bigger job; wait for the split timers first.
- The render thread (`rt.replay` 41.4 ms) and the GPU (`p3d` 40.7 ms) are
  right behind the game thread, so cuts on the game thread alone stop
  paying after ~10 ms. Work that reduces what is drawn or recorded helps all
  three.

### Inside the game thread (build 77981237, HW done 2026-10-16)

This is the "split objRender / renderMapBlock" step from the list above. Most
of the model drawing code (`modelDoRenderInstrs`, `objSetupRenderOpGxState`,
...) is `static` in objprint_dolphin.c, so `--wrap` timers can't reach it.
Timing every GX call with `svcGetSystemTick` would cost more than it
measures (~13k recorded commands per frame). So this build uses a
**sampler** instead:
- **Tags** (`port/ptag.h`): each thread slot has a *scope* byte and a *leaf*
  byte.
  - The scope is set by the perf_lite wrappers (logic, lights, objRender,
    objRender inside the transparent pass, mapBlock, transpPolys, fur,
    ground shadow, shadow, glow, expgfx, water, sky, particles, reflections,
    Rcp) and by `sceneRender` (scope "scene" for its own code).
  - The leaf is what the code is doing at that moment: game code; GX state
    command (every generated cmd/sync wrapper plus the hand-written ones);
    immediate-mode vertex call; GXBegin/GXEnd/GXCallDisplayList; display
    list hashing (`rt.dl_prehash`); skinning; joint matrices; render-thread
    waits / inline replay; probe slices.
  - The leaf is set in every build (a load and two stores per call), the
    scope only with the probe on.
- **Sampler thread:** core 2, priority game - 8, so above the render thread
  and the worker. It wakes every `S3D_PROBE_SAMPLE_US` (default 500) and adds
  the elapsed time to the game thread's scope x leaf cell. It also adds it to
  the render thread's watchdog phase (the opcode being replayed, or
  "rt: idle"), and, when the game thread is outside every scope, to the game
  thread's watchdog phase. Gaps over 20 ms (HOME menu) are dropped.
- **Per-scope draw counts** (game thread): immediate-mode vertex calls,
  GXBegin vertices, display-list calls and display-list KB.
- **`S3D_PROBE` is now a bit mask:** 1 = slices (as in A8BA7DC0), 2 =
  sampler. Either bit turns on the coarse timers and the GSP counts.
  `probe_shutdown()` joins the sampler at exit.
- **Report:** every 300 frames, `sample` lines with ms per frame per cell.
  Columns: total | game gx.cmd gx.vtx gx.draw dl.hash skin joints wait
  probe | imm calls, GXBegin verts, DL calls, DL KB per frame. Then the
  render thread's top phases and the game thread's top phases outside
  every scope.

Azahar smoke test (title screen, `S3D_PROBE=2`): ~110 samples per frame
(507 us apart) and no crash. On the title screen, the fur scope showed
~14.6 of its 27 ms as `wait` (render-thread waits inside the fur code), plus
64 display-list calls / 155 KB per frame and no immediate-mode vertices.
Emulator timing, so only the shape counts: check on hardware whether fur
waits on the render thread in the hub too.

**Hardware test (build 77981237, .3dsx only):** env.txt `S3D_PROBE=2`,
`S3D_BENCH=0` (no rtab, no slices). Stand still at the hub spot ~2 min.
What to read:
- **Per scope:** game code vs `gx.cmd` / `gx.vtx` / `gx.draw` / `dl.hash`
  (our port's recording cost) vs `wait` (time blocked on the render thread
  that the A8BA7DC0 own time did not catch: own time only subtracted
  `rt.space_wait` / `rt.sync_wait` zones).
- **Render thread:** which opcodes the 41 ms of replay goes to.
- If the GX columns are large, the fix is in the port (cheaper recording,
  hashing less often). If game code dominates, look at the game's
  traversal (map block culling, object setup).

**Hardware result (2026-10-16, 77981237, New 3DS XL, hub spot, standing
still, `S3D_PROBE=2`, log `perf/hwlogs/77981237_sampler.log`):** the last
seven 300-frame windows agree within ~0.2 ms per cell. About 88 samples per
frame, 508 us apart, none dropped.

Frame (`gpu.submit_us`) **44.3 ms / 22.6 fps**. GPU `p3d` 39.5 ms.
`rt.replay` 37.2 ms (the render thread is idle ~7-9 ms per frame).
`sceneRender` 35.2 ms, logic 7.1 ms. The game thread never idles: the
samples add up to the whole frame. This is ~3-5 ms faster than A8BA7DC0
mode 2 (~49 ms without its slices). The slices read a 4 MB buffer just
before `sceneRender`, which probably flushed the 2 MB L2 each frame, so
A8BA7DC0's absolute times were inflated (its A/B differences still hold).

Game thread by activity (ms per frame, all scopes):

| leaf | ms | |
|---|---|---|
| game code | 24.6 | logic 6.2, objRender 5.0 + 1.6 transparent, scene 3.2, outside 2.5, map 2.2, fur 1.6, shadow 0.8, glow 0.6 |
| `gx.cmd` (GX state wrappers, recording) | **12.4** | ~11k commands per frame, ~1.1 us each |
| `dl.hash` (display list / array hashing) | **4.0** | 512 calls, ~7.8 us each |
| `gx.draw` / `gx.vtx` | 0.7 / 0.4 | |
| skin / joint matrices | 1.25 / 1.23 | |
| wait (render thread) | 0.02 | no waits on hardware (the Azahar fur wait was emulator-only) |

By scope (total, of which `gx.cmd` / `dl.hash`):

| scope | total | gx.cmd | dl.hash | DL calls | DL KB |
|---|---|---|---|---|---|
| objRender (opaque) | 13.3 | 4.0 | 1.9 | 229 | 364 |
| mapBlock | 6.8 | 3.5 | 1.0 | 165 | 81 |
| logic | 6.2 | 0 | 0 | | |
| scene (own code) | 4.2 | 0.5 | 0.3 | 18 | 58 |
| fur | 3.7 | 1.9 | 0.2 | 24 | 45 |
| outside | 3.2 | 0.6 | 0 | | |
| objRender (transparent) | 3.0 | 0.7 | 0.4 | 19 | 89 |
| transpPolys | 1.6 | 0.8 | 0.3 | 36 | 7 |
| shadow / glow / ground shadow / rcp / rest | ~2.6 | | | | |

Render thread: `gx_dl_call_hashed` 22-23 ms, idle 7-9, immediate-mode data
3.7, `C3D_FrameEnd` 1.5-1.6, `GXLoadPosMtxImm` 1.4, touch UI draw 1.3,
`GXCopyTex` 1.0, then many small ones.

**What it means:**
- **Our port's recording costs the game thread ~17.5 ms of its 44** (`gx.cmd`
  12.4 + `dl.hash` 4.0 + draw/vertex 1.1). That is the most fixable part, and
  it is in our code.
  - In mapBlock, recording state costs more than the game's own code (3.5
    vs 2.2 ms).
  - ~1.1 us (~900 cycles) per recorded state command is far more than the
    encode should cost. Candidates: the record-time filter
    (`dd_redundant`: its 1024 x 100-byte table is bigger than L1), cache
    misses on the ring, `publish()` signals (~112 render-thread wakes per
    frame), TLS reads. Unknown until measured per opcode.
  - `dl.hash` reads ~32 + 128 x arrays sampled words per call, each probably
    a cache miss (strided). The array memo only lasts one frame and has 64
    slots.
- The game's own code is ~27 ms (with skin/joints), including 6.2 ms of
  logic. Even with zero recording cost the game thread would be ~27 ms. So
  30 fps is reachable on core 0 only if most of the recording cost goes.
- The GPU (39.5 ms) is right behind. Cutting ~5 ms from the game thread
  gives ~25 fps, and then the GPU is the wall again. 30 fps needs both
  about -11 ms on the game thread and about -7 ms on the GPU.
- The render thread has 7-9 ms of slack, so work can move from the game
  thread to it (e.g. the record-time filter, if it turns out to be
  expensive).

**Next (proposed):** sample the game thread per opcode (a byte store in
each wrapper) to find which wrappers make up the 12.4 ms. In the same run,
use the existing `S3D_RT_DEDUP_AB` (7 vs 0) to measure the record-time
filter's game-side cost now that the game thread is the wall. Then fix
the expensive wrappers and look at keeping array hashes across frames.

### Recorder cost per opcode (build E9CF8B0F, HW done 2026-10-16)

This build splits the game thread's 12.4 ms of `gx.cmd` (plus `gx.draw` and
`dl.hash`) by opcode and by recorder step. Still the sampler, so the game
thread only pays stores:
- **`g_ptag_op`:** set in `gxrec_begin` (every recorded command, manual ones
  included) and at the start of `GXCallDisplayList` (so its hashing counts
  under `gx_dl_call_hashed`). `g_ptag_op_calls[op]` counts the commands
  recorded, including the ones the filter then drops.
- **`g_ptag_sub`:** the recorder step: wrapper/encode (default), ring
  `reserve`, the record-time filter (`dd_redundant`), `publish` (including
  waking the render thread), and opening/closing immediate-mode chunks.
- **New report lines** after each window: `sample recorder steps`
  (ms/frame per step) and `sample recorder ops` (top opcodes: ms/frame,
  calls/frame, us per call).
- **With an A/B run on, the sampler windows follow the A/B phases.** They
  start after the 30 settle frames and are reported at the phase end as
  `sample <tag> <VAR>=<value>`.
- **`S3D_RT_DEDUP_AB` now cycles 7 / 5** (was 7 / 0). Only bit 2, the
  record-time filter, switches; the unit-map and normal-matrix bits stay
  on.

Azahar smoke test (title screen, emulator timing, so the shape only): with
the filter on, the filter step was 5-7 ms per frame. That's more than the
encoding itself (5-7 ms). Publish was 3-4 ms with the filter vs 1.2 ms
without. `GXLoadPosMtxImm` / `GXLoadTexMtxImm` (~3k calls/frame each) cost
~1.4-1.5 us per call with the filter and ~0.4-0.5 without. If hardware
agrees, the filter costs the game thread more than it saves the render
thread, now that the game thread is the wall.

**Hardware test (build E9CF8B0F, .3dsx only):** env.txt `S3D_PROBE=2`,
`S3D_RT_DEDUP_AB=300`, `S3D_BENCH=0`. Hub spot, stand still ~2.5 min. Read
the `dedupab` lines (frame / GPU / sceneRender per value), and per phase
the `sample recorder steps` / `ops` lines and the render thread's
`gx_dl_call_hashed` / idle time.

**Hardware result (2026-10-16, E9CF8B0F, New 3DS XL, hub, standing still,
log `perf/hwlogs/E9CF8B0F_dedupab_ops.log`):** 6 phases of each value, 270
frames each. This view was heavier than the 77981237 run (560 vs 512
display-list calls per frame, 51 vs 44 ms frame), so compare values within
this log only.

| dedup | frame | GPU | sceneRender | own | render thread idle |
|---|---|---|---|---|---|
| 7 (filter on) | 50.96-51.20 ms | 40.4-41.1 ms | 40.0-40.3 ms | 40.0-40.3 ms | ~11 ms |
| 5 (filter off) | 48.76-50.09 ms | 41.7-42.3 ms | 37.5-39.1 ms | 32.8-33.6 ms | ~4.6 ms |

Recorder steps and ops (first pair of phases, ms per frame):

| | filter on | filter off |
|---|---|---|
| wrapper/encode | 14.3 | 11.9 |
| dedup filter | 6.3 | 0 |
| publish / reserve | 0.5 / 0.15 | 0.3 / 0.1 |
| `gx_dl_call_hashed` (incl. hashing) | 4.7 (560 calls, 8.4 us) | 4.8 (8.6 us) |
| `GXLoadPosMtxImm` | 2.0 (2072 calls, 0.97 us) | 0.77 (0.38 us) |
| `GXLoadTexMtxImm` | 1.6 (1416 calls, 1.15 us) | 0.67 (0.47 us) |
| TEV setters (~900 calls each) | 0.6-0.7 each (~0.65 us) | 0.2-0.25 each (~0.25 us) |

What it means:
- **The record-time filter costs the game thread ~6.6 ms**: the filter step
  itself, plus slower encoding around it (its 100 KB hashed table likely
  evicts the ring and the game's data from L1). With it on, a state command
  costs ~0.65-1.1 us; without it, ~0.25-0.45 us.
- **But it still saves the render thread ~6.4 ms** (idle 11 -> 4.6 ms with
  it off). With the filter off, the game thread starts waiting on the
  render thread (sceneRender - own = ~4-6 ms), so the frame only improves
  1-2.3 ms and the GPU rises ~1 ms. Turning it off is a small win;
  **making it cheap is the real one.**
- A filter costing ~1-1.5 ms instead of 6.6 would give ~-5 ms on the game
  thread with the render thread's savings kept. That's ~46 ms here, where
  the GPU (~41) and the render thread (~40) become the limits.
- `gx_dl_call_hashed` on the game thread (4.7 ms, mostly hashing) is the
  next recorder item.

**Next (proposed): a cheaper filter.**
- Small fixed-size entries that hold a 64-bit hash of the arguments
  instead of up to 20 words. 32 bytes, one cache line per entry, in a table
  that fits L1.
- Compare op, key, generations and the hash.
- A/B: current filter / compact filter / off (`S3D_RT_DEDUP` gets a value
  for the compact variant).
- A false "same" needs a 64-bit hash collision for the same op and key,
  which is negligible.

### Compact filter (build A1C2F0C5, HW done 2026-10-16: no gain)

This adds `S3D_RT_DEDUP` bit 16, which takes effect together with bit 2: the
record-time filter uses a compact table.
- **Entries** (`DdSmall`, gx_rec.c): op, key, generations, word count and a
  64-bit hash of the arguments (two 32-bit lanes: FNV-1a, and rotate-multiply
  with a final mix). 32 bytes, one cache line each.
- **Table:** 512 entries = 16 KB, 32-byte aligned. The full table is 1024 x
  100 bytes.
- **Rules:** same as the full filter. Same op kinds and keys, the same
  forget/reset generations, and the same channel-overlap invalidation.
- **Switching:** changing bit 2 or bit 16 at a frame start bumps the global
  generation, so neither table answers from before the switch.
- **Risk:** a command is wrongly dropped only if a changed argument list
  hashes to the same 64 bits for the same op and key.

**`S3D_RT_DEDUP_AB` now cycles 7 / 23 / 5:** full filter / compact filter /
no filter.

Azahar (title screen, `S3D_RT_DEDUP=23`): renders correctly. Its filter cost
(~6-9 ms) is the same as the full table's. Azahar doesn't model CPU caches,
and the compact version runs more instructions (the hash), so only hardware
can show the cache gain.

**Hardware test (build A1C2F0C5, .3dsx only):** env.txt `S3D_PROBE=2`,
`S3D_RT_DEDUP_AB=300`, `S3D_BENCH=0`. Hub spot, stand still ~3.5 min (3
values x 300 frames, at least 3 cycles). Compare per value: `dedupab`
frame / GPU / sceneRender / own; `sample recorder steps` (the filter step);
render-thread idle. Also look for wrong-looking state (textures, blending,
colours) in the compact phases.

#### Result (A1C2F0C5, HW 2026-10-16): the compact filter doesn't help

Log: perf/hwlogs/A1C2F0C5_dedupab_compact.log. Table:
`python -I tmp_scripts/compact_hw.py <log>`. Env: `S3D_PROBE=2`,`S3D_RT_DEDUP_AB=300`, `S3D_BENCH=0`, hub, 252 s. Clean run: 5 cycles of 7
and 23 and 4 of 5, every phase 270 frames, no gaps.

Averages per value (ms per frame):

| | 7 full | 23 compact | 5 off |
|---|---|---|---|
| frame | 49.71 | 50.14 | **48.35** |
| GPU p3d | 39.70 | 39.34 | 40.58 |
| sceneRender (own) | 39.85 (39.83) | 40.24 (40.24) | 38.08 (**33.08**) |
| recorder: wrapper/encode | 13.70 | 13.74 | 11.48 |
| recorder: dedup filter | 6.31 | **6.95** | 0 |
| recorder: publish | 0.46 | 0.48 | 0.27 |
| render thread idle | 10.11 | 10.20 | 4.62 |
| render thread `gx_dl_call_hashed` | 22.6 | 22.8 | 23.5 |
| render thread immediate-mode data | 6.07 | 6.09 | 6.55 |
| game `gx_dl_call_hashed` | 9.0 us/call | 8.7 | 8.6 (~516 calls) |
| `GXLoadPosMtxImm` per call | 0.98 us | 1.17 | 0.37 |
| TEV setter per call | ~0.65 us | ~0.65 | ~0.25 |

Every cycle of value 5 was faster than every cycle of 7 or 23: 47.6-48.9 vs
48.3-50.7 ms.

What it means:
- **The compact table is no cheaper** (filter 6.95 vs 6.31 ms). Encoding is
  inflated by the same amount. Cache misses on the table aren't the cost.
  That fits Azahar, which has no cache model but showed the same 6-9 ms.
  - The disassembly is short: `gxrec_end` with the filter inlined is ~60
    instructions on the hit path, plus `memcpy` on a miss.
  - So ~0.55 us per command (~450 cycles) is still unexplained. It's
    probably not the table: shared ring lines, the volatile tag stores, or
    sampler attribution. A boot micro-bench would settle it.
- **No filter is the fastest now: -1.4 ms per frame.** The game thread
  saves 6.7 ms of own time. The render thread loses ~5.5 ms of idle (it
  replays ~6 ms more), so the game thread waits ~5 ms inside sceneRender.
  GPU +0.9 ms.
  - This reverses 29059F15, where the filter was fastest. Since then
    shadow refresh and the report fix took load off the GPU and render
    thread, so the game thread became the wall.
- **With the filter off, the three are balanced.** The frame is 48.4 ms:
  - game thread busy ~43 (logic 7 + scene own 33 + outside 3; it waits ~5);
  - render thread busy ~43.7 (idle 4.6);
  - GPU ~40.6.
  
  Any lever on one of them alone can win at most ~3-5 ms.
- On the render thread, `gx_dl_call_hashed` replay (~23 ms, ~45 us per
  call) is the biggest single item in the frame. Immediate-mode data is
  6.5 ms. `touchui_draw` costs ~1.2 ms every frame (bottom screen, mostly
  static) and `C3D_FrameEnd` ~1.4 ms.
- On the game thread, DL hashing is ~4.5 ms (8.6 us per call). The memo only
  lasts one frame, so the same static model lists are hashed again every
  frame.

**Proposed next:**
1. Default `S3D_RT_DEDUP` 7 -> 5 (filter off). Measured -1.4 ms with no
   pacing risk: the pacing problems were with the filter on. Remove the
   compact table (bit 16): it's dead.
2. Render-thread breakdown of `gx_dl_call_hashed`: per-step tags in the
   render-thread sampler (cache lookup/translate, state flush, TEV/texture
   bind, uniform upload, draw submit). It's now the biggest block on the
   critical path.
3. Cheap render-thread wins: draw the touch UI only when it changes
   (~1.2 ms).
4. Game thread: keep DL and array hashes across frames, keyed by pointer and
   size. Invalidate on `mm_free` / display-list rebuilds, and re-verify a
   few entries per frame. Worth up to ~4 ms of game thread.
5. Optional: a boot micro-bench of the recorder (synthetic commands with the
   filter 0/full) to explain the ~450 cycles per command. That decides
   whether a cheap filter that runs before encoding is possible. It would
   give back the render thread's ~6 ms.

### State deltas (S3D_RT_DELTA, gx/gx_rec_delta.c)

The A1C2F0C5 result left the render thread's `gx_dl_call_hashed` replay
(~45 us per call) as the biggest block in the frame: ~40-50 recorded setter
commands surround every draw, and the replay dispatches each one through the
real setter even when nothing changes. The record-time filter could not drop
them (it cost the game thread more than it saved), and the dedup census says
96% of setups repeat a material seen before.

Instead of recording one command per setter, the game thread now keeps a
shadow copy of the GX state (`s_shadow`, gx_rec_delta.c) and applies each
setter to it: compare, write the field, mark the changed region and the dirty
bits. Before the next draw (or any recorded command) the pending changes go
into the ring as one `GXREC_OP_DELTA` packet: a 64-bit region bitmap, the
invalidate-layouts flag, the dirty-bit union, the `vtx_gen` bump count and
the changed regions' bytes. The replay copies the regions into `g_gx`,
invalidates the vertex layouts if asked, flushes the display-list holds and
the pending immediate-mode batch (exactly where `gx_state_changed` would
have), then marks the dirty bits; `gx_render_prepare` runs unchanged at the
next draw. Matrix loads are batched the same way into one
`GXREC_OP_MBLOCK` packet whose replay calls the real load functions, so the
GameCube's FIFO semantics (compare and skip unchanged values) stay intact.

- **Order:** a state setter flushes a pending matrix block first and a
  matrix load flushes a pending delta first, so packets keep the stream
  order. The order matters for the display-list batch holds, which draw
  with the state in place at the flush point.
- **Shadow hazards:** display lists may write CP/XF registers
  (gx_vtx.c `handle_cp` / `xf_write`) behind the recorder's back. Fields a
  list could change are handled so the shadow never guesses: vertex
  descriptors, formats and arrays are always included (no value compare,
  and `vtx_gen` bumps are summed), while `GXSetCurrentMtx` and
  `GXSetTexCoordGen2` stay recorded commands. TEV, pixel, texture, light
  and projection state cannot be written by lists (BP writes are ignored),
  so they are safe to compare against the shadow. Matrices always go
  through the block, which replays the real setters.
- **Shadow refresh:** at frame starts and after direct sections the shadow
  is copied from `g_gx` (gxrec_delta_refresh, refresh_shadows). A
  `S3D_RT_DELTA` change at a frame start drains first, so the pending delta
  flushes with the old mode.
- **Regions:** the shadow covers `GxState` plus the TEV state, which moved
  from gx_tev.c statics into `g_gx.tev` (gx_internal.h), and the texture
  copy setup from gx_copy.c statics (`texcopy_src` / `texcopy_dst_fmt`).
  The region table uses `offsetof`, so a field move fails the build, and a
  one-time check at boot verifies every region has real bytes (a missing
  entry once shipped as a zero-sized copy that silently dropped texture and
  light loads; the check logs an error and disables the delta path instead).
- **Verify mode** (`S3D_RT_DELTA=2`, PROFILE=1): every delta setter also
  records the real command; the replay compares each delta packet's payload
  against the state the recorded commands produced and logs mismatches
  (`rec.delta_verify_fail`, "delta verify: region %d differs").
- `S3D_RT_DELTA` default 1 (2 = verify), 0 = the old per-setter recording.
  `S3D_RT_DELTA_AB` cycles 1 / 0 (tag `deltaab`); with `S3D_AB_SECONDS=N`
  the phases last N wall-clock seconds instead of frames, so a short run at
  one spot covers both values. Counters:
  `rec.delta_flushed`, `rec.delta_applied`, `rec.delta_bytes`,
  `rec.mblk_applied`, `rec.mblk_matrices`, `rec.mblk_pos`. In delta mode
  the `S3D_RT_DEDUP` filter only sees the few remaining recorded commands,
  and each delta packet resets its memory (gxrec_delta.c, by design).

Expected: the render thread stops paying ~40-100 setter dispatches per
draw; the game thread records far fewer commands (less ring traffic and
fewer space waits). The cost moves to the shadow apply on the game thread
(compare + field write, the same work the setters did in mode 0) and the
delta apply on the render thread (one memcpy per draw).

### Render-thread steps (build 5C202467, HW pending)

The user chose items 1-3.

- **Filter off by default:** `S3D_RT_DEDUP` default 7 -> 5 (gx_rec.c,
  gx_render.c, frametrace_3ds.c). The compact table (bit 16) is removed.
  `S3D_RT_DEDUP_AB` now cycles 5 / 7.
- **Zones become sampler phases.** In normal builds `GX_PROF_ZONE`
  (gx_internal.h) now sets the thread's watchdog phase (`g_main_phase`) for
  the rest of the block and restores it at scope exit. That's two stores and
  no timer. The render-thread sampler line therefore splits replay into
  exclusive steps:
  - `gx.prepare` / `gx.prep.proj` / `posmtx` / `texmtx` / `tev` /
    `bindtex` / `pixel` / `fog` / `light` / `lightenv`;
  - `gx.draw`, then `gx.draw.update` (citro3d `C3Di_UpdateContext`, now
    called explicitly before `C3D_DrawElements`) and `gx.draw.submit`;
  - `gx.flush` (pending immediate-mode batch);
  - `gx.dl.lookup` / `validate` / `play`, `gx.gather`, and new: `gx.dl.listcmd`
    (CP/XF commands inside a cached list), `gx.dl.record` (a miss
    recording the list), `gx.dl.uncached` (dynamic / uncacheable lists
    decoded every time);
  - `gx.immediate` (immediate-mode decode).

  Whatever is left under an opcode name (e.g. `gx_dl_call_hashed`) is the
  time outside every zone.
- **Probe report:** `PHASES` 64 -> 96. The phase lines print 10 entries
  each, with up to 4 `(cont.)` lines, down to 0.05 ms. The old single line
  was cut by the log after ~20 entries.
- **Touch menu** (touchui_3ds.c):
  - phases `touchui: ui begin (flush, GPU wait)` / `C2D prepare` / `rows` /
    `tabs` / `C2D flush` / `ui end (state restore)`;
  - a log line `touchui: tab N` whenever the tab changes;
  - `S3D_TOUCHUI_DRAW=0` skips the draw (the bottom screen shows only the
    minimap);
  - A/B `S3D_TOUCHUI_AB` (tag `touchab`, 1 / 0).
- **Why measure before caching the menu:** the bottom target is cleared and
  the minimap redrawn every frame. "Draw only on change" therefore needs a
  retained image: a texture plus one quad, or skipping the bottom clear and
  transfer. `gx_ui_begin`'s `gx_gpu_wait_previous` never showed up as a
  render-thread phase in A1C2F0C5, so the 1.2 ms is citro2d CPU work or the
  state restore. The phases tell which.

Azahar (title, `S3D_AB_MAP=-1`): renders correctly. The touchab phases
switch and log, and the new phase names appear. Azahar runs the .3dsx in
96 MB mode with reduced caches, so it showed `gx.dl.uncached` ~14 ms; on
hardware (124 MB) that should be far smaller. On the map tab the menu is
small (`touchui: tabs` ~0.2 ms).

**Hardware test (5C202467, .3dsx only):** env.txt `S3D_PROBE=2`,
`S3D_TOUCHUI_AB=300`, `S3D_BENCH=0`. Hub spot, stand still ~2.5 min, on the
touch tab you normally use. Read:
- the `touchab` lines (frame / GPU / own with and without the menu);
- the `sample render thread` lines: the steps of the ~23 ms of
  display-list replay, and the menu phases;
- the `touchui: tab` line.

The dedup default 5 should give ~48 ms in the hub (A1C2F0C5's value 5).

#### Result (5C202467, HW 2026-10-16): the render thread is the wall

Log: perf/hwlogs/5C202467_touchab.log, 197 s, 4 cycles of 1 / 0, clean.
Table: `python -I tmp_scripts/rtsteps.py <log> 60` (the script drops the
`rt: ` / `touchui: ` prefixes).

**This view is heavier than A1C2F0C5's:** 723 DL calls per frame (was 516),
mapBlock 302 (was 178). So the frame was ~56-58 ms, not 48. Compare only
within this log.

The touch menu was on the Settings tab for the whole A/B:

| | menu drawn | not drawn |
|---|---|---|
| frame | 57.4 | 56.7 (-0.4 per pair) |
| GPU | 47.6 | 46.1 (-1.5) |
| game own | 37.7 | 37.6 |
| render thread idle | 4.2 | 5.2 |

Menu phases: `rows` 1.04 ms, `tabs` 0.10, the rest < 0.05. So it's the
citro2d text of the Settings rows; the Map tab costs ~0.1 ms. The GPU also
pays ~1.5 ms for the menu. Cheap fix if wanted: parse each row's text once
and reparse only when its value changes, or use the Map tab when measuring.

**Render thread** (busy ~51 of 56 ms, idle 4-5; the game thread waits ~8 ms:
fur 4.1, objRender 2.1, glow 0.6, transpPolys 0.9). Steps in ms per frame:

| step | ms | what |
|---|---|---|
| `gx.gather` | 5.9 | dynamic-list vertex gather (incremental + per-frame copy) |
| `gx.draw.update` | 5.9 | citro3d `C3Di_UpdateContext` per draw |
| `gx.prep.bindtex` | 4.7 | texture lookup + bind per prepare |
| `gx.prep.tev` | 4.7 | unit map + combiner build (outside bindtex) |
| `gx.draw.submit` | 3.6 | `C3D_DrawElements` packet |
| `gx.dl.lookup` | 2.7 | vertex-state + unit-map hash, slot find (723 calls, ~3.7 us) |
| `gx.draw` | 2.3 | apply_vsrc, prune, census around the draw |
| `gx.immediate` | 2.1 | immediate-mode decode |
| `gx.dl.play` | 1.8 | segment loop |
| frame end / copytex / posmtx | 1.6 / 1.2 / 1.6 | |
| ~40 small GX setters | ~8 | 0.1-0.4 each |

What it means:
- With the filter off, the render thread is the critical path. Its savings
  now go straight to the frame.
- **bindtex (4.7 ms) is suspicious.** `gx_tex_get` (gx_tex.c) probes an
  open-addressed table whose chains end only at never-used slots. After
  evictions, chains can grow long. The fingerprint is checked only once
  per generation, so a per-unit memo is safe: skip the lookup when the
  object is the same as last time and the entry is still used and verified.
  That's a candidate for ~4 ms.
- **tev (4.7 ms):** combiners are rebuilt on every TEV-dirty prepare.
  Candidate: memo by a hash of the TEV/texgen state.
- **gather (5.9 ms):** `frame_copy16` copies each incremental buffer into
  frame memory every frame. Candidate: double-buffered persistent buffers
  instead of the copy.
- draw.update + submit (~9.5 ms) is per-draw citro3d cost: fewer state
  changes / draws, harder.

### Render-thread memos (build D17AAC27, HW 2026-10-16: ~-0.4 ms render thread, frame within noise)

The user chose: the texture-binding shortcut, reusing combiner setups, and
the touch menu text cache.

**Code reading and the 5C202467 counters changed the plan:**
- **There already is a TEV program cache** (`get_program`, gx_tev.c, 256
  direct-mapped entries). Per frame: 276 hits, 12 misses with 12 evictions,
  302 `apply_skips`. So `gx.prep.tev` runs only ~590 times per frame,
  ~8 us each. Every frame, 12 programs evict each other in shared slots, and
  each eviction is a recompile.
- **`bind_textures` also costs ~8 us per call.** Per bound unit it calls
  `gx_copytex_find`, which scans all 64 `GxCopyTex` entries (~150 bytes
  each), so up to 64 cache misses. That's the likely bulk of
  `gx.prep.bindtex`.
- The texture cache was half full (257 of 512 slots, no tombstones), so its
  probe chains are probably short. It gets a memo anyway, plus counters to
  check.

**`S3D_RT_MEMO` bitmask** (gx_internal.h, read at the frame start in
gx_render.c, default 15):
- **1, compact copy index** (gx_copy.c): `s_copy_dest[64]` mirrors every
  `dest` write (`sync_dest` after the three memsets and the assignment).
  `gx_copytex_find` scans those 256 bytes instead.
- **2, texture lookup memo** (gx_tex.c): 128 slots map a data address to the
  entry last found for it.
  - A hit goes through the same once-per-generation fingerprint check
    (`entry_hit`).
  - Entries never move and an evicted one fails `entry_matches`, so a stale
    memo only misses.
  - New counters: `gx.tex_lookups`, `gx.tex_memo_hits`,
    `gx.tex_probe_steps`, `gx.tex_verifies`.
- **4, two-way TEV cache** (gx_tev.c): 2 x 256 entries. Way 0 is the old
  table, so with the bit off it behaves exactly as before. A miss replaces
  the empty or least recently used way (`stamp`). Keys are compared in full
  either way, so switching the bit is safe.
- **8, touch menu text cache** (touchui_3ds.c): up to 64 parsed and optimized
  `C2D_Text`, looked up by content, in their own 1024-glyph buffer. When it
  fills up, everything is cleared and parsed again; texts drawn earlier in
  the frame have already emitted their vertices.

**New zones** (sampler phases):
- `gx.bind.copyfind`, `gx.bind.texget`;
- `gx.tev.unitmap`, `gx.tev.texmtx`, `gx.tev.apply`, `gx.tev.compile`.

**A/B `S3D_RT_MEMO_AB`** (tag `memoab`) cycles 15 / 0 / 3 / 7. The A/B table
now takes up to 4 values.
- 0 vs 3: the copy index plus texture memo. The zones split the two.
- 3 vs 7: the two-way TEV cache.
- 7 vs 15: the menu text cache.

Azahar (title, Settings tab, `S3D_AB_MAP=-1`): every phase renders
correctly, the menu included. 107 of 145 texture lookups per frame hit the
memo; the other 38 took 45 probe steps, so the chains are short. No TEV
misses at the title. Azahar can't show the timing.

**Hardware test (D17AAC27, .3dsx only):** env.txt `S3D_PROBE=2`,
`S3D_RT_MEMO_AB=300`, `S3D_BENCH=0`. Usual hub spot, Settings tab, stand
still ~3.5 min (4 values x ~17 s, 3 cycles). Compare:
- `memoab` frame / GPU / own;
- render-thread `gx.prep.bindtex`, `gx.bind.*`, `gx.prep.tev`, `gx.tev.*`,
  `touchui: rows`, idle;
- counters `tev.cache_evictions` and `gx.tex_*`.

Analyse with `tmp_scripts/rtsteps.py`.

#### Result (D17AAC27, HW 2026-10-16): the memos work, but they were small

Log: perf/hwlogs/D17AAC27_memoab.log. Hub, Settings tab, 12 windows of 270
frames (15/0/3/7, 3 cycles). The view is like 5C202467: ~26.7k render
commands per frame, 55-58 ms frames, so compare within this log only.

| `S3D_RT_MEMO` | frame ms | GPU ms | game own ms | RT work (sampler total) | RT idle |
|---|---|---|---|---|---|
| 15 (all) | 55.47 / 55.12 / 57.45 | 44.7 | 36.4 | 54.15 | 3.91 |
| 0 (none) | 56.04 / 55.73 / 57.96 | 44.8 | 36.4 | 54.59 | 3.79 |
| 3 | 55.15 / 56.94 / 57.89 | 44.8 | 36.3 | 54.75 | 3.83 |
| 7 | 55.15 / 57.49 / 57.69 | 44.9 | 36.4 | 54.89 | 3.83 |

- **Frame: about -0.3 ms with all memos, within noise.** 15 beats 0 by
  0.5-0.6 ms in every cycle, but 15 always runs right before 0 and the frame
  drifts up ~+1 ms per cycle (55.1 → 57.9 over the run). Correcting for the
  drift leaves ~0.3 ms.
- **The drift is not the memos.** Commands (26.4-26.8k), texture verifies
  (130/frame), game own time (36.3) and GPU (44.8) stay flat. Every render
  thread step grows a little (sampler total 53.4 → 55.7 ms). Cause unknown
  (scene animation? memory contention?); not chased.
- **Per memo:**
  - **1, copy index:** `gx.bind.copyfind` 1.10 → 0.67 ms. Works.
  - **2, texture memo:** works by its counters: 600 of 755 lookups hit,
    probe steps 898 → 245 per frame. **But `gx.bind.texget` stays 3.5 ms.**
    Before the memo a lookup took only 1.2 probe steps on average, so probing
    was never the cost. See "Where texget's 3.5 ms goes" below.
  - **4, two-way TEV cache:** evictions 4.7 → 0 per frame. No time effect
    (`gx.tev.compile` is too small to show; `gx.tev.apply` 1.86,
    `gx.tev.texmtx` 1.47, `gx.tev.unitmap` 0.64 unchanged).
  - **8, touch text cache:** touch `rows` 1.06 → 0.92 ms.
- **Defaults stay `S3D_RT_MEMO=15`.** They're cheap, correct (no visual
  issues reported) and save ~0.4 ms of render-thread work.
- **The render thread is still the wall**: idle only ~3.8 ms of 55, GPU ~10 ms
  of slack, game thread own 36 ms. Biggest render-thread steps (ms/frame):
  `gx.draw.update` 5.8, `gx.gather` 5.4, idle 3.8, `gx.draw.submit` 3.6,
  `gx.bind.texget` 3.5, `gx.dl.lookup` 2.7, `gx.draw` 2.4, `gx.tev.apply` 1.9,
  `gx.dl.play` 1.8, `gx.immediate` 1.7, `GXLoadPosMtxImm` 1.6,
  `gx.tev.texmtx` 1.5, `C3D_FrameEnd` 1.4.

**Where texget's 3.5 ms goes (hypothesis, not measured yet).** ~4.6 us per
lookup is far too much for a memo hit. The only heavy part left in
`gx_tex_get` is `entry_hit`'s **fingerprint re-check**: the game calls
`GXInvalidateTexAll` every frame, which bumps `s_generation`, so every
texture used in a frame is fingerprinted again on its first bind (130
verifies/frame). `fingerprint` reads 64 words spread across the whole
texture: ~64 cache misses each, ~8.3k misses/frame. At the probe's measured
~110-130 ns per line under contention that is ~1 ms, plus the L1/L2 lines it
evicts from the replay. Unknown: how the other ~2 ms split (zone overhead,
`entry_result`, wrap/filter setup is outside the zone).

Tools: tmp_scripts/rtsteps.py (per value), tmp_scripts/rtdrift.py (per
window in time order, for drift), tmp_scripts/phase_cmp.py (counters).

**Would fewer polygons help? (user question, 2026-10-16)** Only partly. Most
render-thread work is paid per draw or per state change, not per vertex:
`draw.update`, `draw.submit`, `prep.*`, `tev.*`, `bind.*`, `dl.lookup` and
the setters add up to ~35 of the ~51 busy ms. Per vertex:
- `gx.gather` (5.4 ms) copies skinned or dynamic vertices
  (`dl.inc_copied_vertices` ~31.8k/frame), so it scales with the polygons of
  animated models (characters, fur), not of the map;
- the GPU (44.8 ms; vertex shading was ~27-31 of 43 ms in earlier builds),
  but it has ~10 ms of slack right now;
- `gx.immediate` (1.7 ms, ~3.2k vertices).

So lighter animated models would trim gather. Fewer **draws** (fewer objects
via draw distance or object LOD, merged map blocks) would cut every
per-draw step on the render thread and the game thread's traversal. Lower
polygon count only becomes the main lever once the GPU is the wall again.

### Texture re-checks (build FA1AFBED, HW 2026-10-16: -2.5 ms render thread, ~-2 ms frame; default now 2)

#### Result (FA1AFBED HW, perf/hwlogs/FA1AFBED_verifyab.log)

| `S3D_TEX_VERIFY` | frame ms (3 cycles) | `gx.tex.verify` | `gx.bind.texget` | RT work total |
|---|---|---|---|---|
| 1 (old) | 56.33 / 56.97 / 58.92 | **2.62** | 0.94 | 56.01 |
| 2 (header) | 54.86 / 54.55 / 56.39 | 0.11 | 0.97 | 53.42 |
| 3 (8 words) | 54.88 / 56.11 / 56.74 | 0.44 | 0.94 | 54.02 |
| 0 (none) | 54.84 / 56.04 / 56.42 | 0 | 0.73 | 54.09 |

- **The hypothesis held:** the fingerprint was 2.6 of texget's 3.5 ms. Mode
  1 was the slowest phase in every cycle (~2 ms frame). Mode 2 performs like
  mode 0.
- ~128 verifies/frame, 120 of them PICA textures (all header-only in mode
  2). `gx.tex_changed` stayed 0 in every mode.
- The same frame drift as D17AAC27 (+1-1.5 ms per cycle). GPU ~48 ms, game
  own ~38.3 ms.
- **Default is now `S3D_TEX_VERIFY=2`** (build FFC8BE81).

**Hub draw sources (same log, game-thread sampler, per frame):**

| Scope | DL calls | DL KB | game-thread ms (of which DL hash / skin / joints / wait) |
|---|---|---|---|
| objRender | 302 | 506 | 17.4 (2.15 / 1.64 / 1.40 / 2.50) |
| objRender in transparent pass | 26 | 105 | 2.2 |
| mapBlock | 318 | 141 | 8.6 (1.91 / - / - / -) |
| transpPolys | 59 | 19 | 3.2 |
| fur | 24 | 45 | 6.3 (3.66 wait) |
| scene / sky / rcp | 18 / 16 / 5 | | |
| glow (immediate) | 2158 imm calls, 719 verts | | 2.6 |
| shadow (immediate) | 519 imm calls | | 0.8 |

Objects and map blocks are ~43% and ~41% of the 768 DL calls. On the render
thread a DL call costs ~40 us all-in.

### Hub: fewer objects, merged scenery, cheaper animation (2026-10-16)

The user's ideas: merge the static scenery (rocks, plants) of the hub into one
thing, turn distant animated characters into static ones, and drop objects by
distance. A code survey (foxhollow, read only) found the hooks:

- **Object draw distance:** `objUpdateOpacity` (main/shader.c:3141) is the
  only distance test. The range is per placement
  (`cullDistance2 = placement byte << 3`, object.c:1678), with a 100-unit
  fade band before it; objects in the band leave the opaque path for the
  transparent queue. A 10/15 px screen-size cull follows, then a 5-plane
  sphere test (no far plane). There's no global scale and no model LOD.
- **Map blocks:** `renderSceneGeometry` (lightmap.c:510) walks the PVS cells,
  tests each block's box, then `renderMapBlock` (tex_dolphin.c:1069) does
  one `GXCallDisplayList` per material piece, each with its own box test
  (tex_dolphin.c:498). No distance cull. Grass is queued only within ~250
  units of view depth (`lbl_803DEC28` = -250.0f, tex_dolphin.c:80/89).
- **Animation:** advanced in each DLL's update (no "skip when far" logic).
  Skinning runs at **render** time in `modelDoRenderInstrs`
  (objprint_dolphin.c:2206-2263), once per frame per model (`bufferFlags`
  bit 8). Off-screen objects are only skinned for shadows. Skipping the
  blend + `ObjModel_ToggleVertexBuffer` on some frames reuses last frame's
  model-space vertices; the joint matrices (world transform) must still
  update.
- **Fur:** non-"strong" objects get up to 3 shells; `port_fur_step` rarely
  cuts them in mono.

**Ideas, with what the data says:**

| Idea | Targets | Estimate | Cost / risk |
|---|---|---|---|
| A. Object draw distance scale (`S3D_OBJ_DIST`) | 328 object DL calls, objRender 19.6 ms game | unknown: depends on how many hub objects are far; the A/B says | done (build 00D2184C) |
| B. Merged hub scenery: at hub load, group map-block DLs by material into world-space buffers, one draw per material | 318 map DL calls (~12 ms RT at 40 us), renderMapBlock 8.6 ms game | RT -6..-10 ms, game -4..-6 ms; GPU + (loses per-piece culling, ~8 ms slack) | large (days); scrolling/animated textures, grass passes and water stay separate |
| C. Animation LOD: far characters re-skin every 2nd/4th frame, plus a gather that skips unchanged models | skin 1.6 + joints 1.4 game, gather 5.9 RT | ~2-4 ms | medium; needs a census of the hub's skinned models first |
| D. Map piece far cull (at the fog end) and grass distance (-250 → less) | map DL calls, grass passes | small-medium | small |
| E. Fur: 1 shell / none for far non-player objects | fur 24 calls, 6.3 ms game (mostly waits) | ~1 ms | small |
| F. Pre-built GPU command lists per cached DL (DRAWCALL_PLAN.md phase 3): the general form of B for every map | every DL call (~40 → ~10 us) | the largest, ~15+ ms RT | high |

"Merging rocks and plants" in B covers the props that are map geometry.
Props that are objects (separate models) would need A or C, or batching of
identical static objects (rated low in DRAWCALL_PLAN.md: per-object lighting
and opacity vary).

#### Result (00D2184C HW 2026-10-16): objects are the hub's biggest lever

Log perf/hwlogs/00D2184C_objdistab.log, 4 cycles. With the texture fix the
100% frame is ~53.4 ms (was ~56).

| `S3D_OBJ_DIST` | frame ms | GPU ms | game own ms | objects drawn (`obj.dist_kept`) | objRender DL calls | RT work |
|---|---|---|---|---|---|---|
| 100 | 53.4-55.0 | 44.4-45.3 | 37.4 | 275 | 280 | 52.3 |
| 75 | 48.4-49.7 | 40.6-41.2 | 33.1 | 239 | 203 | 47.3 |
| 50 | 42.5-44.5 | 34.1-34.4 | 27.4 | 150 | 109 | 41.7 |
| 30 | 35.5-37.4 (~28 fps) | 30.6-30.8 | 21.3 | 71 | 60 | 35.0 |

- ~356 objects reach the distance test per frame; 275 are within range at
  100%.
- Cutting 205 objects (30%) takes **-18 ms frame**, about 0.09 ms per object.
  The cut is spread over all three stages: game own -16 ms (objRender 14.4 →
  3.8 ms, shadows, glow), render thread -17 ms (gather 5.3 → 3.1,
  draw.update 5.6 → 3.8, submit, lookup), GPU -14 ms. Map blocks (274 DL calls,
  7.5 ms) and fur (24, 6 ms) are unchanged, as expected.
- So the per-draw cost is not the only thing that matters: the hub is
  **full of objects**, and every one costs on all three stages.
- What disappears at each step is the open question (the user's visual
  report and the census below). A blanket distance cut also removes big far
  landmarks. A size cut (small **and** far) should keep those.

#### Object census + minimum size A/B (build 6DEA8147, HW pending)

- `S3D_OBJ_MINPX` (default 10 = the game's own cut, `objUpdateOpacity`'s
  projected size `sz`, in the game's units): objects with 10 <= sz < MINPX are
  dropped (`obj.size_culled`). A/B `S3D_OBJ_MINPX_AB` (tag `minpxab`) cycles
  10 / 20 / 30 / 45.
- `S3D_OBJ_CENSUS=<frames after the A/B start>`: one frame's census of every
  object at the distance test, grouped by def id + class id: count, how many
  are in range, distance min/avg/max, range, size min/avg, and how many in
  range objects a 75/50/30% distance cut or a 20 size cut would drop (tag
  `objcensus`). The ObjDef name field is empty at run time;
  **tmp_scripts/objcensus.py** adds the names from OBJECTS.tab/.bin (def id =
  index, name at +0x91).
- Azahar attract: census OK (29 objects, 10 kinds, e.g. FrontFox,
  FrontPilots, LGTPointLig x15 point lights, CmbSrc). No objects in the
  10-45 size band there, so the size cut is first seen on HW.
- HW test: env.txt `S3D_PROBE=2`, `S3D_OBJ_MINPX_AB=300`, `S3D_OBJ_CENSUS=30`,
  `S3D_BENCH=0`, the usual hub spot, ~3.5 min.

#### Result (6DEA8147 HW 2026-10-16): size cut and census — the hub is a garden

Log perf/hwlogs/6DEA8147_minpxab.log.
- **Pacing stalls in this run.** Random windows lost 10-20 ms per frame in
  render-thread `gx_frame_begin` (waiting for a free frame buffer), with the
  game thread waiting on its fence. That's waiting, not work, and it hit
  phases at random. So the "net" frame below subtracts the frame_begin
  sample. Cause not chased (pacing; it wasn't in 00D2184C).

| `S3D_OBJ_MINPX` | net frame ms (per cycle) | GPU | game own | `obj.size_culled` |
|---|---|---|---|---|
| 10 (game) | 55.2 / 55.3 / 57.3 | 47.5 | 37.9 | 0 |
| 20 | 52.5 / 53.2 / 53.8 | 45.7 | 34.7 | 31 |
| 30 | 51.1 / 51.1 / 52.7 | 43.8 | 33.3 | 69 |
| 45 | 44.4 / 46.1 | 38.8 | 27.7 | 115 |

- The user: the size cut was **less noticeable** than the distance cut
  (30% distance was noticeable).
- **Census** (one frame, tmp_scripts/objcensus.py): 353 objects at the
  distance test, 53 kinds, 301 in range. **Plants are more than half:**
  LongGrassCl 94 (82 in range), BlueFlowerP 20, SH_FernTree 19, BullRushClu
  17, BlueWhiteFl 11, RedYellowFl 9, YellowWhite 5, BullRush 4, plus single
  SH_* flowers: ~180. Then fuelCell 15, AppleOnTree 12, LargeCrate 10,
  MagicPlant 10, BoulderOne 10, SmallBasket 8, CmbSrc 8, thorntails 7,
  killer mushrooms 7, ...
- **The plants are skinned, animated models.** DLL 685 (SoftBody: grass
  clumps, flowers, bulrushes) and DLL 687 (trees: ferns) render through
  `objRenderModelAndHitVolumes`. SoftBody_update sets move 0 at **one shared
  phase** (slow or fast) for every plant: all instances of a kind are in the
  same pose each frame. Yet each is skinned separately (joint matrices +
  vertex/normal blend in `modelDoRenderInstrs`), and its vertices are then
  gathered again on the render thread.

#### What MKW-3DS does (github.com/MiniBandit/MKW-3DS, read 2026-10-16)

A native 3DS renderer (assets converted at build time, citro3d, its own
render thread), not GX translation; 60 fps on New 3DS. Relevant techniques
from its renderer.cpp:
- **GPU skinning:** the bone palette goes to vertex-shader uniforms; vertex
  buffers stay static. Nothing is re-uploaded per frame for animated models.
- **Quality setting `minScreenRatio`:** course-object parts below a screen
  size are skipped. That's our `S3D_OBJ_MINPX`.
- Per-part culling of rigid multi-part models (64-bit visible mask from
  posed bounds), and precomputed static culling per model.
- Skips rebinding material state when consecutive submeshes match. Merges
  sprites/particles into one draw per texture and blend run. Rewrites
  dynamic quads only when they change.
- Right eye reuses the left eye's buffers.
- No instancing; rigid parts drawn one at a time with a uniform update.

For us: GPU skinning is the natural "keep the detail, drop the cost" form of
the plant fix (and for characters). The cheaper first step is **shared
pose**: skin each plant kind once per frame and let every instance use that
buffer, so the render thread gathers it once too.

#### Plant A/B (build 7ED0F495, HW pending)

- `S3D_PLANTS` (port/lod_3ds.c; plants = DLL 685 or 687): 1 normal, 0 hidden
  (via `port_obj_range`: the most any plant trick can save), 2 **frozen**:
  each plant model is skinned once, then `ObjModel_ToggleVertexBuffer` and
  the vertex/normal blends are skipped (`port_skin_skip`, patched into
  `modelDoRenderInstrs` by objprint_dolphin.c.sed). The joint matrices, hit
  spheres and callbacks still update. A model unseen for 60 frames is
  skinned again (address reuse). Mode 2 measures what re-skinning costs (game
  skin + joints, render-thread gather), and the user can judge how much the
  sway matters.
- Counters `plant.skinned`, `plant.skin_skipped`, `plant.hidden`.
- A/B `S3D_PLANTS_AB` (tag `plantab`) cycles 1 / 2 / 0. Azahar attract: runs
  (no plants there).
- HW test: env.txt `S3D_PROBE=2`, `S3D_PLANTS_AB=300`, `S3D_BENCH=0`, the
  usual hub spot, ~3 min.
- Reading it: if 2 ≈ 0, re-skinning is the plants' cost → implement shared
  pose (keeps the sway). If 2 ≈ 1 but 0 is much faster, the cost is per draw
  → batch plants of a kind into one draw (pre-transformed, or instanced via
  a uniform matrix palette).

#### Result (D9E45810 HW 2026-10-17): hiding all plants = frame -5 ms only

Log perf/hwlogs/D9E45810_plantab.log (237 s; first window skipped). Phase
lines are averaged over 4-5 windows. Counters come from reports at least 80
frames inside a phase (tmp_scripts/abwin.py `<log> S3D_PLANTS <keys>`; the
report of a window's last frames is printed after the switch, so the older
per-phase scripts mix phases).

| S3D_PLANTS | frame ms | GPU p3d ms | own ms | RT idle ms | draws | triangles | unlit vidx |
|---|---|---|---|---|---|---|---|
| 1 normal | 49.7 | 37.8 | 35.1 | 4.8 | 792 | 59.8k | 91k |
| 2 frozen | 49.8 | 37.9 | 35.2 | 4.8 | 796 | 60.0k | 91k |
| 0 hidden | 44.6 | 28.0 | 30.1 | 4.0 | 651 | 36.6k | 21k |

- Hidden removes 184 of 302 objects in range, but only **141 of 792 draws
  (18%)**. Many plants are culled or offscreen and only cost objRender
  logic. It also removes 39% of triangles and 77% of unlit vertices.
- **GPU -10 ms (-26%), but the frame only -5.2 ms (-10%).** The frame
  follows the slowest stage. Without plants the render thread is still busy
  ~40 of 44.6 ms (idle 4.0), and its cost is per draw (~35-40 us), not per
  triangle. The game thread also saves ~5 ms (objRender -4.6 ms, joints
  -0.2), but it was already waiting on the render thread.
- So plants are expensive on the GPU (big, many triangles, unlit) and cheap
  on the CPU side. Since the GPU isn't the wall, removing them gains little.
  Frozen = normal, as expected (rigid models, no skinning to skip).
- Three stages are now close: game ~35-40, render thread ~45, GPU ~38. Any
  single-stage lever gains at most the gap to the next stage. A real speedup
  has to cut the render thread **and** one of the others.
- Lever ranking after this:
  - Plant draw distance (hide only far plants): scales all three stages at
    once, like S3D_OBJ_DIST did (~0.09 ms per object).
  - Render-thread per-draw cost (thin emitter / pre-built DL commands): the
    remaining 650 draws are mostly map blocks (~307 DL calls) and other
    objects.
  - Plant batching only cuts ~141 draws (≈5 ms RT), and leaves the GPU at
    ~38 ms, so it is capped at about the same −5 ms.
  - Done for the game's batchable path (2026-10-17, build E9567DFF,
    `S3D_DL_BATCH`): DRAWCALL_PLAN.md "Plant batching". Map-block merging is
    analysed there too ("Map blocks: merge by material?").

#### Polygon reduction on HW (7AC43A91, 2026-10-17; log perf/hwlogs/7AC43A91_polyreduce.log)

Build-time reduction (README "Polygon reduction"; ~38% fewer triangles on the
disc). User saw nothing major. Not an A/B (it's baked into the data), so it
is compared with the 8683536D run, whose camera angle differed (87 vs 0):
this view had +17% DL calls (744 vs 634) and twice the grass passes. Even
so, **GPU p3d 43.5 → 39.5 ms** (gpu.draw 31.4 → 25.9; gx.triangles 76k →
65k), while the frame went 50.8 → 53.9 ms because the heavier view raised
game own time (+3.1) and render-thread replay (+2.6). **The hub is now CPU
bound** (render thread ~49, game ~43, GPU ~39.5). Linear memory free in the
hub only went 14.6 → 15.0 MB, so the reduction freed no memory worth spending.

**2026-10-17: full polygons were the default for one build (36BAF2FE), then
back to 50.** HW hub with full polygons: GPU p3d ~46.8 ms, frame ~54.1 ms,
83k triangles/frame, and the GPU became the wall (the map dedup's -1,450
commands and -1 ms game thread changed nothing). With 50 (C8DC5F71, same
spot): GPU ~38.8, frame ~52.3, RT ~48.2 the wall. The user chose low poly as
the default again (build 3AF2B4BF) so CPU / render-thread work can pay off;
`POLY_REDUCE=0` builds the full polygon version.

#### Animation LOD (builds 47D8E44C → 0D82830B, 2026-10-17, HW result below)

Answer to "did we do animation off at a distance?": no, only the plant test
mode `S3D_PLANTS=2` existed. HW counters showed why it would barely matter:
only **~7 models per frame blend vertices** (skin.models_inline); the ~44
plants are rigid jointed models whose cost is `ObjModel_UpdateAnimMatrices`.
`S3D_ANIM_LOD` (port/lod_3ds.c, default 1, now 2; touch menu "Far animation LOD":
Off / On / Strong; A/B `S3D_ANIM_LOD_AB`, tag `animab`, cycles 1 / 0 / 2):
- Tiers by camera distance (stored from the distance test):
  - under `S3D_ANIM_LOD_NEAR` (400): every frame;
  - up to `S3D_ANIM_LOD_FAR` (900): every 2nd frame;
  - beyond: every 4th frame.
  Mode 2 doubles the intervals.
- **Vertex-blended models** (`port_skin_skip`): only if the model has no
  morph targets. Last frame's vertices are kept (no buffer toggle, no blend).
  This is safe while the object moves, because the blend is in model space
  and the world transform is a GPU matrix.
- **Rigid models** (`port_anim_skip`): last frame's joint matrices are kept.
  This only happens while the world matrix is bit-identical, since the world
  transform is inside those matrices. Left out: objects with
  `afterBonesCallback`, root motion (`moveControlFlags & 4`, which writes
  `gModelRootRot*`) or fur. Fur passes scale the joint matrices in place, so
  they need a rebuild every frame; fur is recorded by `port_fur_mark` from
  objRenderFuzz.
- Counters: `anim.skinned`, `anim.skin_skipped`, `anim.pose_updated`,
  `anim.pose_skipped`. In the Azahar hub: mode 1 skips 23 of 37 poses per
  frame, mode 2 skips 32 of 42. No visual issues.
- Expected: the game thread saves at most the joint-matrix work (~1.4 ms in
  total, the share of far plants), plus re-skinning of far blended models.
  The frame only moves if the game thread is the wall. Right now the render
  thread is, so the A/B decides.
- **Found on the way: the settings table (`MAX_SWITCHES` 96) was full.** Every
  setting registered after the 96th shared one overflow slot. It kept its
  default/env value but could not be changed live (touch menu rows such as
  Shadows, Fur, Grass layers, water/heat-haze effects; `set` commands).
  Raised to 192, with a warning on overflow. The variables of past HW A/Bs
  were all registered, so those results stand.
- HW test: env.txt `S3D_PROBE=2`, `S3D_ANIM_LOD_AB=300`, `S3D_BENCH=0`, the
  usual hub spot, ~3 min (skip the first window).

#### Result (0D82830B HW 2026-10-17): game thread -0.6 ms, frame unchanged; Strong is now the default

Log perf/hwlogs/0D82830B_animab.log (230 s, 15 windows; first window
skipped). Scripts: tmp_scripts/animab.py (counters per phase) and
tmp_scripts/animab_samp.py (sampler per phase).

| S3D_ANIM_LOD | frame ms | GPU p3d ms | own ms | joints (sampler) ms | joint ticks | rt.space_wait ms | RT idle ms |
|---|---|---|---|---|---|---|---|
| 0 off | 51.65 | 39.83 | 37.2 | 1.53 | 359k (1.34 ms) | 3.68 | 5.08 |
| 1 on | 51.64 | 39.98 | 36.8 | 0.98 | 233k (0.87 ms) | 4.01 | 5.00 |
| 2 strong | 51.70 | 39.95 | 36.6 | 0.82 | 219k (0.82 ms) | 4.11 | 4.91 |

- The joint-matrix work drops by 36-40%, about 0.5-0.7 ms of game thread.
  Vertex skinning is unchanged (~1.78 ms): only a few blended models, all
  near the camera.
- **The frame doesn't move.** The saved game time turns into more
  `rt.space_wait`, where the game thread waits for room in the
  render-thread ring. The render thread is the wall in this view (idle
  ~5 ms of 51.6). Draw calls, triangles and GPU are the same in every phase.
- Visuals: the user saw nothing odd on distant animations ("the screen is
  too small"). **Default `S3D_ANIM_LOD=2` (Strong)**, in lod_3ds.c and the
  touch row (build D9E45810, .3dsx only). The A/B now cycles 2 / 0 / 1.
  It only pays off once the game
  thread becomes the wall again.
- Frame drift again: the same phase goes 50.7 → 52.6 ms over the run.
  Compare phases within a cycle only.
- Note for the plant A/B: hub plants are rigid jointed models, so
  `S3D_PLANTS=2` (frozen = skip vertex skinning) should equal mode 1 now.
  The meaningful phase is 0 (hidden): the most that batching or merging
  plants could save.

### Thin emitter (user chose "option 1", 2026-10-16; build BFDF5C33, HW pending)

**The translation layer.** The port runs the decompiled game unchanged. Its
GX calls are translated at run time by gx/ (~14.5k lines) into citro3d/PICA
commands, ~40 us of render-thread time per draw. MKW-3DS avoids this with a
native renderer (build-time converted models). The options were: (1) keep
translating but move work to build time / pre-built command lists, (2) a
hybrid native renderer for the hot models, (3) a full native renderer. The
user chose **(1)**. Plan: DRAWCALL_PLAN.md phase 2 (thin state emitter),
then phase 3 (pre-built command chunks per cached DL).

**Step 1: citro3d from source.** third_party/citro3d = citro3d 1.7.1 (the
version in the build image, zlib license), built by the Makefile instead of
`-lcitro3d`; `renderqueue.c` is left out (gx/gx_c3d_queue.c replaces it).
Its headers come first in `COMMON_INCLUDES`. citro2d stays a prebuilt
library and links against our citro3d (same ABI). Every change is marked
`S3D:`; source/s3d_hooks.h has the hooks. Unchanged build DB05C280: Azahar
identical (frame, image).

**What citro3d did per draw (code reading):**
- `C3D_DrawElements` = 17 separate `GPUCMD_Add*` library calls for a packet
  whose words never change except the index offset and count → the 3.6 ms
  `gx.draw.submit`.
- `C3Di_UpdateContext` ran **twice** per draw (our explicit call for the
  sampler split, then again inside `C3D_DrawElements`). Each run calls
  `C3D_UpdateUniforms` for the vertex and the geometry shader, and each of
  those walks all 96 float-uniform dirty flags one byte at a time.

**Step 2: fast paths, `S3D_C3D_FAST` bitmask (default 3)**, read at the
frame start (gx_render.c, `g_c3d_fast`):
- **1:** `C3D_DrawElementsFast` (drawElements.c): one context update, then a
  copy of a **template draw packet** recorded once with libctru's own encoder
  (into a scratch buffer), with the index-buffer word and the count patched.
  The GPU gets byte-identical commands. Falls back to the library path at
  the buffer end or if the template looks wrong. Used for mono and both
  stereo eyes.
- **2:** `C3D_UpdateUniforms` skips four clean dirty flags at a time
  (word-aligned flag array). The flags stay the source of truth, so
  citro2d's inline setters keep working.
- New sampler phases inside `C3Di_UpdateContext`: `c3d.attr_buf_effect`,
  `c3d.tex`, `c3d.texenv_fog`, `c3d.light`, `c3d.uniforms` (before them,
  framebuffer/viewport/scissor/program stay `gx.draw.update`).
- A/B `S3D_C3D_FAST_AB` (tag `c3dab`) cycles 3 / 0 / 1.
- **Azahar (attract scene, CPU-emulated timing): frame 57.0 → 53.1 ms**
  (3 vs 0; 1 alone 53.6-53.9), screenshots correct in every phase.
- HW test: env.txt `S3D_PROBE=2`, `S3D_C3D_FAST_AB=300`, `S3D_BENCH=0`, the
  usual hub spot, ~3 min. Compare frame / GPU / own and the render thread's
  `gx.draw.update` / `gx.draw.submit` / `c3d.*` / idle (rtsteps.py).
- Then, by the new `c3d.*` split: our own state shadow for the biggest
  `c3d.*` sections (thin emitter proper), then pre-built chunks per DL
  segment.

#### Result (BFDF5C33 HW 2026-10-16): render thread -3.3 ms, but now it waits for the GPU

Log perf/hwlogs/BFDF5C33_c3dab.log.
- **The fast paths work on HW** (render-thread sampler, mode 3 vs 0):
  `gx.draw.submit` 1.17 → 0.33 ms, `c3d.uniforms` 4.5 → 2.3 ms (mode 1
  alone 2.95: one context update instead of two), `c3d.light` 0.65 → 0.43.
  That's ~3.3 ms less render-thread work. Other `c3d.*` sections: tex 0.6,
  texenv_fog 0.5, attr_buf_effect 0.5, the rest of draw.update 0.35.
- **The frame didn't move** (49.1 vs 48.8 ms in adjacent windows). The time
  saved reappears as **`GXCopyTex` +3.3 ms**: its `C3D_FrameSplit` waits
  in `queue_make_room` for room in the GPU queue. So in this view the render
  thread now runs ahead and waits for the GPU. **The GPU side (~41 ms p3d plus
  copies and transfers) is the wall again**, and further CPU-side savings
  won't show until GPU work drops (objects/plants, size cut, shadows).
- The first window (38.5 ms, GPU 32.6) had a lighter view (the user was
  still settling).

**3-second freezes (new problem).** From frame ~2494 on, a 2.9-3.3 s frame
every ~210 frames (~13 s). One per 270-frame window = +11 ms/frame, which
is the "+11 ms in gx_frame_begin" of some windows here and the random slow
windows in 6DEA8147.
- 6DEA8147 had them too, at the attract scene, matching `slow async read ...
  took 3606 ms` warnings (SD card reads). 00D2184C and FA1AFBED had none
  after boot.
- During a freeze: render thread in phase `rt: gx_frame_begin` (inside
  `gx_frame_begin_slider`, **not** in C3D_FrameBegin's VBlank wait, which
  has its own phase name). Game thread waits on the fence. Nothing is logged;
  worker idle; no texture conversions; no cache reset.
- Suspect: SD card stalls (the slow reads), but in the hub only the log
  writer touches the SD card, and the frame path doesn't wait for it
  (`log_flush` only in debug paths).
- **Build 8683536D adds a stall finder:** `gx_frame_begin_slider` times its
  steps (pipeline update, C3D_FrameBegin, capture, clears, render begin) and
  logs `stall: frame begin took N ms: ...` with the GPU queue state when over
  250 ms. The log writer logs `SD write of N bytes took M ms` when a write
  takes over 250 ms.

**Result (8683536D HW, 2026-10-17; log perf/hwlogs/8683536D_memoab_stall.log).**
The user ran env `S3D_PROBE=2`, `S3D_RT_MEMO_AB=300`, `S3D_BENCH=0` (the
D17AAC27 memo test again, not the plant A/B). They stood ~3.5 min at the hub
spot on the Settings tab and noticed no stutter.
- **No freezes:** 5484 frames, ~4 min in the hub. After the boot/loading
  windows, no perf window has max > 80 ms, and windows from f2900 on stay
  ≤ 60 ms. The only `stall` line is at boot (f29, render begin 503 ms while
  the sound banks load). In BFDF5C33 the freezes started at f2494 (~2 min
  into the hub) and came every ~13 s, so this run got well past that point.
  The freeze is intermittent. The stall finder stays in every build; the next
  time it happens, the `stall:` line names the step.
- Loading hitch: f603 (map load) max 1587 ms, the same as in BFDF5C33
  (1589 ms). A `SD write of 3354 bytes took 1070 ms` happened at the same
  moment.
- **SD log writes are slow:** every ~3 s, a 14-20 KB write takes 250-360 ms
  (~50 KB/s). That's on the background writer, so frames don't wait for it.
  Still, this SD card is slow. Suspect for the freezes: SD/FS contention (a
  GSP/FS service stall?), unproven.
- **Memo A/B: no difference.** Every phase after the first is 50.8-51.7 ms
  frame, GPU 43.2-43.7, own ~36.5 for 15/0/3/7 alike. That matches D17AAC27.
  Defaults stay at 15.
- **The first A/B window is a lighter scene, not faster code:** 46.0 vs
  ~51 ms. Here (and in BFDF5C33, 38.5 vs 49) the view in the first ~300
  frames after A/B start has ~11% fewer triangles/vertices/rt.cmds;
  objShadowRender, drawGlow, transparentPolys and drawSkyStars grow after it
  (tmp_scripts/winstats.py compares counters between two frame ranges).
  **Skip the first A/B window** in comparisons, or raise the settle delay.
- Slow drift again: the frame creeps +1 ms over the run (50.0 -> 51.6) with
  flat counters. That matches D17AAC27; cause unknown (heat/throttling? heap?).

#### Object draw distance A/B (build 00D2184C, HW 2026-10-16, result above)

- `S3D_OBJ_DIST` (percent, default 100), port/lod_3ds.c `port_obj_range`,
  patched into `objUpdateOpacity` by the new patches/main/shader.c.sed.
  Scaled ranges are floored at 140 (or the original range if smaller), so
  nearby objects stay.
- Counters `obj.dist_kept` / `obj.dist_culled` (objects reaching the
  distance test).
- A/B `S3D_OBJ_DIST_AB` (tag `objdistab`) cycles 100 / 75 / 50 / 30.
- Azahar attract scene: the hook runs (29 objects/frame) but nothing is far
  enough to cull, so culling itself is first checked on HW.
- HW test: env.txt `S3D_PROBE=2`, `S3D_OBJ_DIST_AB=300`, `S3D_BENCH=0`, the
  usual hub spot, stand still ~3.5 min. Compare frame / GPU / own,
  `obj.dist_*`, and the sampler's per-scope DL calls (objRender) per phase.
  The user should note which objects disappear at 75/50/30.

`GXInvalidateTexAll` runs every frame, so each texture's first bind in a
frame re-checks the cached copy against its data (`entry_hit`). Mode 1 reads
64 words spread across the texture: ~64 cache misses per texture, ~130
textures per frame. Azahar (attract scene) shows **86% of the checked
textures are build-time converted ones** (`gx.tex_verifies_pica` 69 of 80).

**`S3D_TEX_VERIFY`** (gx_tex.c, read on the next `GXInvalidateTexAll`,
default 1):
- **1:** the old 64-word fingerprint;
- **2:** build-time converted (PICA header) textures only compare an 8-word
  hash of their 32-byte header (one cache line). Their pixels come from
  files and are not rewritten in place. Zero-copy ones are read by the GPU
  in place anyway. Memory that was freed and reused for something else fails
  the header check. Runtime textures (distortion, shadow noise, caustics)
  keep the full check;
- **3:** 8-word fingerprint for every texture;
- **0:** no re-check. For measuring only: it misses textures rewritten in
  place.

All modes also compare the header hash for PICA textures, which is a
stricter check than before. New zone `gx.tex.verify` (the fingerprint read
itself, inside `gx.bind.texget`). New counters: `gx.tex_verifies_pica`,
`gx.tex_verify_header`.

**A/B `S3D_TEX_VERIFY_AB`** (tag `verifyab`) cycles 1 / 2 / 3 / 0. Azahar,
title + attract (`S3D_AB_MAP=-1`): every phase renders and `gx.tex_changed`
stays 0. Azahar can't show the timing.

**Hardware test (FA1AFBED, .3dsx only):** env.txt `S3D_PROBE=2`,
`S3D_TEX_VERIFY_AB=300`, `S3D_BENCH=0`. Usual hub spot, Settings tab, stand
still ~3.5 min. Compare `verifyab` frame / GPU / own, render-thread
`gx.bind.texget` and `gx.tex.verify`, and idle (tmp_scripts/rtsteps.py).
- If mode 0 doesn't shrink `texget`, the fingerprint isn't the cost. Then
  look at the rest of `gx_tex_get` and at the zone placement.
- If 2 ≈ 0, make 2 the default.
- Watch for wrong or flickering textures in phases 2/3/0 (water, shadows,
  the distortion effects).

## Where we are (build 5EB6A447, New 3DS XL, hub standing still)

| | value |
|---|---|
| frame, overlap on | 65.8 ms (15.2 fps) |
| CPU on core 0 (`gpu.submit_us`) | 65.7 ms (59.5 ms without overlap, +6.2 ms pipelining overhead) |
| GPU (`gpu.draw_us`) | 21 ms, fully hidden |
| core 2 worker (`worker.busy_us`) | 0 ms in the hub |
| draws / triangles | 644 / 72k |

To reach 30 fps, the core 0 work has to drop from ~66 ms to under 33 ms.
After that, the 21 ms GPU still fits.

## What ran where before steps 1 and 2

The app can use core 0 fully, core 2 fully (New 3DS only), and up to 80% of
core 1 (syscore, via `APT_SetAppCpuTimeLimit`; we ask for 30%). Core 3 belongs
to the system.

| Thread | Core | Work | Load in the hub |
|---|---|---|---|
| Game thread (main) | 0 | All game logic, scene traversal, **all GX translation** (gx/*.c: TEV compile, dlcache, prepare, draw), citro3d command building, touch UI, movie upload, audio pump (MusyX synth, `audio.pump` ~0.8 ms) | ~66 ms per frame: the whole frame |
| Worker (`port/worker.c`) | 2 (fallback core 1, else inline) | DVD async reads, movie frame prefetch, benchmark | 0 in the hub; ~30 ms of blocking I/O per frame in the attract scene |
| Audio stream thread (`port/audio_3ds.c`) | -2 = the process default core = 0 | streamed audio | small |
| Watchdog (`os/watchdog.c`) | -2 = 0 | stall detection | negligible |
| DSP | - | ndsp mixing | not on the ARM |

So yes: everything that costs time runs on one core. Core 2 is idle in
gameplay and core 1 is unused. The game's own threads are not started
(`OSCreateThread` is a stub: "thread not started; single-threaded port").

## What we don't know yet: how the 66 ms divides

The plan depends on how much of the core 0 frame is **GX translation** (port
code, movable) versus **game code** (logic plus traversal, which has to stay
on one thread). The only breakdown we have is from Azahar (DRAWCALL_PLAN.md,
Dinosaur Island): CPU 57 ms, of which `gx.displaylist` 41.5 ms,
`gx.prepare` 14.5 ms, `gx.draw` 19 ms and immediate mode 6.5 ms. Those zones
nest, so they don't sum, but GX is clearly most of the frame. Azahar's CPU
timing is not the hardware's.

**Step 0 (needed before any code): a PROFILE=1 hardware run in the hub.**
Build **2A1228FF** (2026-10-14, `docker run ... star3d-builder all PROFILE=1`,
`.3dsx` only) is that build. It does not have the `gx.api_total` zone yet.
Waiting for the hardware log: hub, standing still, overlap on, no toggling,
5+ min (this also checks bug 19).
- `make PROFILE=1` turns on the GX detail zones and the PERF_WRAP game
  timers (`perf.sceneRender`, `perf.Obj_UpdateAllObjects`, `perf.objRender`,
  `perf.renderMapBlock`, blend/joint timers, and others).
- Zones cost ~3 us each, so compare zones with each other, not with the
  normal build's frame.
- Output we want: hub table "game logic | traversal (game side of draws) |
  GX translation | citro3d submit | flush/copies".
- Also add one top-level zone around everything the GX entry points do
  (e.g. `gx.api_total`, counted on entry/exit of the outermost GX call). Then
  "GX share of the frame" is one number.

Rule of thumb for what a GX render thread could give:
`frame ≈ max(game, GX + recording cost)`. If GX is 45 of the 66 ms, the frame
drops to ~45 ms (22 fps). Reaching 33 ms then needs DRAWCALL_PLAN.md phase 2/3
on the render thread as well. If GX is 30 ms, the render thread alone reaches
~36 ms.

## Step 0 results (build 2A1228FF, PROFILE=1, hub, 2026-10-14)

Run: ~5 min in the hub (map 7) standing still, with overlap on and no
toggling. **No hang** (bug 19 did not come back without toggling). The spot
has 757-778 draws and 81k triangles, more than the 644 of the earlier run.
The profiling build's CPU frame is 86.3 ms (11.3-11.8 fps). Averages over 93
report windows:

| Part (profiling build) | ms/frame | Source |
|---|---|---|
| Everything outside `sceneRender` (object logic, text, audio, loads) | 12.8 | 86.3 - 73.5 |
| `sceneRender` total | 73.5 | PERF_WRAP |
| of which GX display lists (`GXCallDisplayList`) | 44.8 | perf wrapper = `gx.displaylist` 44.1 |
| of which GX immediate mode | 3.6 | `gx.immediate` |
| of which game traversal + untimed GX setters + matrix math | ~25 | the rest |

Inside `gx.displaylist`:

| Zone | ms | Reads game memory? |
|---|---|---|
| `gx.prepare` (TEV 10.4, bindtex 4.8, fog 1.1, lightenv 1.0, ...) | 16.2 | no (shadow state) |
| `gx.dl.play` (cached replay: `gx.draw` 13.8 with update 6.5 / submit 3.8, gather 4.4, frame copies) | 19.9 | gather + copies: yes |
| `gx.dl.lookup` (state hash + list hash + table) | 4.5 | yes (list bytes) |
| `gx.dl.validate` (`arrays_unchanged`: hashes array contents) | 2.3 | yes |
| `gpu.split_flush` | 2.3 | no |

Game-side timers:
- `Obj_UpdateAllObjects` 5.3 ms
- `objRender` 27.3 ms and `renderMapBlock` 21.4 ms (both include their GX
  calls)
- `sceneDrawTransparentPolys` 14.3 ms
- `drawGlow` 2.9 ms, `drawSkyStars` 1.8 ms, `Rcp_UpdateDistortionTextures`
  1.4 ms
- skinning: joint matrices 1.2, vertex blend 0.8, normal blend 0.75 ms
- `audio.pump` 1.1 ms

**The profiling cost is large and lands almost entirely on GX.** The build
runs ~9,300 zones per frame. At the Makefile's ~3 us per zone, that is up to
~28 ms, all inside the GX numbers. So in a normal build:
- GX translation ≈ 50 ms minus ~15-28 ms of zone cost ≈ **25-35 ms**;
- game code (logic + traversal + untimed setters) ≈ **30-38 ms**.
- Next normal-build run: add 3 coarse zones that cost nothing per draw
  (outside `sceneRender`, `sceneRender`, GX total) to pin these down.

What this means:
1. **The work splits roughly 50/50 between game code and GX translation.**
   A render thread can at best halve core 0's frame. Expected: ~66 ms →
   ~35-40 ms.
2. Game code alone is ~30-38 ms, right at the 33 ms target. So core 0 must
   also shed skinning, audio and everything else that can move, and its GX
   recording cost must stay small. The game-memory reads (lookup, validate,
   gather, ~11 ms profiled) must **not** stay on core 0.
3. **The game already delays frees by 2 frames.** `gMmFreeDelay = 2`
   (foxhollow main/mm.c:1015, mm.c:546): `mmFree` goes on a deferred stack
   that `mmFreeTick` (end of `gameLoop`) releases 2 ticks later. That is
   GameCube GPU-lag protection, and it also covers a render thread one frame
   behind.
4. `gameLoop` (main/gameloop_main.c:744) order: `waitNextFrame` → logic
   (`gameUpdate`, loads, audio) → rendering → `GXFlush_` (frame end) →
   `Obj_FlushDeferredFreeList` → `mmFreeTick` → `doQueuedLoads`.
   - The in-place rewrites from the survey all happen in the rendering part
     of the next frame: normals and splash colours rewritten while drawing,
     sky stars, distortion textures (`Rcp_UpdateDistortionTextures` from
     lightmap.c:622).
   - The logic part mostly writes object state, not GX data.

## Recommended 3-core design (2026-10-14)

| Core | Thread | Work |
|---|---|---|
| 0 | game | Game logic and traversal. GX calls only **record**: setters, matrices and immediate-mode bytes copied into the frame FIFO; for display lists the pointer, size and array pointers (no hashing). |
| 2 | render (priority above everything else on core 2) | Replays the FIFO: dlcache lookup/validate/gather/frame copies (reading game memory), prepare (TEV, textures, fog, lights), citro3d draw, split flushes, frame end. Owns citro3d: touch UI drawing, movie upload, capture, the GX reset. |
| 1 | helpers (syscore, raise `S3D_CORE1_LIMIT` toward 80) | The current worker (DVD reads, movie prefetch) moved off core 2. The audio stream thread and the MusyX pump. Skinning jobs (joint matrices, vertex/normal blend, ~3 ms profiled) as work-stealing jobs: core 0 runs any job core 1 hasn't started by the join, so a system preemption on core 1 never stalls the frame. Log writes. |

**Lag and memory safety** (render thread up to one frame behind):
- The render thread reads game memory late, so the rule is "frame N's GX
  data stays valid until the render thread finishes frame N":
  - Frees: covered by the game's own 2-tick free delay (point 3 above).
    Check that map-block and model unloads go through `mmFree` (not
    immediate) and that `mmSetFreeDelay(0)` sections (attract mode, 53.c,
    FirePipe) don't free GX data.
  - In-place rewrites (normals, splash colours, sky stars, CPU textures)
    happen during the next frame's rendering. A **render fence** at the first
    GX call of frame N+1 (lazily, in the recorder) waits until the render
    thread has finished frame N. Logic then always overlaps with the previous
    frame's translation. Rendering overlaps with streaming translation of the
    same frame.
- Calls with return values are answered on core 0 from a recorder-side
  shadow: `GXGetScissor`, `GXEndDisplayList` (the recorder keeps
  display-list recording on core 0: it only writes game memory), and the port
  hooks `gx_offscreen_begin`, `gx_hud_begin`, `port_copytex_*`,
  `port_stereo_*`. Their answers depend on settings and the budget, not on
  GPU results, so the recorder can decide them. Check each one.
- `GXPeekZ` is already one frame late through the breakpoint callback. With
  the render thread it becomes two frames late. Acceptable for the lens
  flare and other visibility checks; verify.
- Expected frame (normal build): core 0 ≈ game ~30-38 - core 1 jobs ~3-4 +
  recording ~3 ≈ **29-37 ms**; core 2 ≈ GX **25-35 ms** plus the replay
  overhead. That gives roughly 27-33 fps in the hub. If core 2 is the limit,
  DRAWCALL_PLAN.md phase 2 (thin state emitter: `gx.draw.update` 6.5 ms,
  `gx.prep.tev` 10.4 ms) applies on core 2 only.

**Why not other splits:**
- Game logic in parallel: not thread-safe.
- Splitting GX across cores 1 and 2: one command buffer, strictly serial, and
  core 1 is preemptible.
- A 3-stage pipeline (core 0 record → core 1 lookup/gather → core 2
  translate): only if core 2 ends up over budget, and with work-stealing for
  the same reason.

**Build steps** (each one testable in Azahar first; 1 and 2 are done, see
"Implementation" at the top):
1. Coarse zones in the normal build + core 1 helpers: worker to core 1,
   audio to core 1, skinning jobs with work stealing. Small, independent, and
   it frees core 2.
2. Recorder/replayer with `S3D_RENDER_THREAD=1` (record, then replay inline
   on core 0 at the frame end; checks the serialization and the late reads
   without threads) and `=2` (replay on core 2). Generated from a GX
   signature list.
3. Move the remaining citro3d users (touch UI, movie, capture, vi reset,
   savestate) behind the render thread. Thread-safe profiler counters and a
   render-thread watchdog phase.
4. The render fence and the late-read audit (free delay, in-place rewrites,
   with `S3D_GPU_CHECK_REUSE`-style hashing of recorded arrays at record time
   vs replay time in debug builds).

## Options, ranked

### A. GX render thread on core 2 (the big one)

The GameCube already works this way. The CPU writes GX commands into a FIFO
(the write-gather pipe) and the GPU consumes them later, so the game is
written for a consumer that lags behind. We do the same: the game thread
records GX calls into a ring buffer, and a render thread on core 2 replays
them through today's gx/*.c code into citro3d. This is Dolphin's "dual core"
mode.

- **Gain:** GX translation (most of the frame) runs in parallel with game
  logic and traversal. Expected frame = max(game, GX). See Step 0.
- **Cost:** large. Every GX entry point gets a record/replay pair. A
  generator (like tools/gen_stubs.sh) emits these from a signature list.
  Every port path that touches citro3d directly must move to the render
  thread too: touch UI (citro2d), movie upload, capture, `vi_3ds.c` frame
  begin/end, savestate snapshots.
- **Pointers** (first idea; the survey below replaces kind 1 with a front/back split):
  arguments split into three kinds:
  1. Data the GPU reads later (`GXSetArray` arrays, `GXCallDisplayList`
     lists, texture/TLUT memory): store the pointer. This is GameCube
     semantics, since the real GPU also reads them late. It only holds while
     the render thread lags no more than the real GPU would.
  2. Small values passed by pointer that the game may reuse right after the
     call (`GXLoadPosMtxImm`, `GXLoadTexObj`, `GXInitTexObj*`, colors,
     light objects): copy the contents into the FIFO. The real GX also
     copies these into the FIFO immediately.
  3. Getters / readbacks: served from a game-side shadow, or by a sync. See
     the survey below.
- **Immediate mode** (`GXBegin`/`GXPosition*`/`GXColor*`/`GXEnd`): raw
  vertex bytes go straight into the FIFO, like the GameCube's write-gather
  pipe. The per-call cost must stay at a few instructions.
- **Lag:** two possible depths.
  - *Same frame:* the render thread drains the FIFO while the game is still
    producing the frame. The game thread waits for the drain at the frame end
    (`VIWaitForRetrace` → `gx_frame_end`). Data lifetime is the same as
    today. Gain: traversal and logic interleaved with draws overlap with
    translation. The logic-only part at the start of the frame (object
    updates before rendering) does **not** overlap.
  - *One frame behind:* the game runs frame N+1 logic while the render thread
    translates frame N. This gives the full gain. It needs every type-1
    buffer to stay unchanged for one more frame: the game must
    double-buffer it, or we copy it at record time. GRAPHICS_PLAN.md 3.5
    notes `vtxBuf` is already double-buffered. Every other dynamic source
    needs checking (survey below).
  - Build the same-frame version first, then extend the lag.
- **Debug path:** `S3D_RENDER_THREAD=1` records and replays **inline on core
  0** (no threads). This checks the serialization alone, in Azahar, against
  screenshots and dlcache counters. `=2` runs the replay on core 2. The
  non-threaded mode stays as a debug mode (the port is New 3DS only).
- **Other blockers:**
  - The profiler is not thread-safe (dvd_3ds.c comment). It needs
    per-thread counters, or render-thread zones that the render thread
    reports itself.
  - The watchdog phase (`g_main_phase`) is per game thread. Add a
    render-thread phase.
  - The GPU fence and deferred frees (gx_init.c) move with the render thread.
  - Worker jobs (DVD reads) share core 2. The render thread must run at a
    higher priority than the worker so I/O completions don't delay
    translation; I/O jobs are mostly blocked in FS IPC anyway.

#### Survey of the game and port for option A (2026-10-14, code reading only)

What the game assumes:
- The game never calls `GXDrawDone`. The only CPU/GPU throttle is
  `waitNextFrame` (pi_videoinit.c:378-385), which lets the GameCube GPU lag by
  at most ~1 frame. After it, the game calls `GXInvalidateVtxCache` and
  `GXInvalidateTexAll` once per frame.
- `GXFlush_` (pi_videoinit.c:230-266) ends a frame with `GXSetDrawSync`
  followed by `GXCopyDisp`. On 3DS, `GXSetDrawSync`
  (foxhollow/port/src/gx_shim.c:14-22) calls `VIWaitForRetrace`, which is the
  real frame boundary (vi_3ds.c:42-50). That function runs the PeekZ
  breakpoint callback and the draw-sync token, draws the touch UI and calls
  `gx_frame_end`.

Which game buffers are double-buffered:
- **Only skinned positions are double-buffered**:
  `vtxBuf[(bufferFlags>>1)&1]`, toggled once per model per frame
  (model.c:2249, objprint_dolphin.c:1948/1979/2054). The joint matrices are
  double-buffered too.
- **Single-buffered, rewritten every frame:**
  - skinned normals: one `normalBuf` (model.c:878, objprint_dolphin.c:2397);
  - water splash colours `s->vtxColors` (dlls/engine/19/19.c:165-210), whose
    particles also die or get reused;
  - sky-star display lists rebuilt in place (skystars.c:225-343);
  - the warp display list (rcp_dolphin.c:109-147);
  - CPU-written textures: shadow noise, distortion, caustics
    (newshadows.c:1261/1434/1468).
- Map blocks and models are static while loaded, but they are freed on
  unload.
- Fur adds no CPU arrays. Its shells only change matrices
  (`GXLoadPosMtxImm`), which travel in the stream.

How the port's GX layer works today:
- Setters only update the shadow state `g_gx` plus dirty bits.
- The work happens at draw calls, and they **read game memory at call
  time**:
  - `GXCallDisplayList` hashes the list and sampled array contents, then
    replays cached geometry or decodes again (gx_dlcache.c:913+);
  - immediate mode is decoded on the CPU (gx_vtx.c);
  - textures are converted and fingerprinted at prepare time.
- This is why the single-buffered data above is safe today.

Calls whose return value the game needs right away:
- `GXGetScissor`: save/restore of `g_gx.scissor`;
- `GXEndDisplayList`: returns the recorded size, writes game memory;
- `GXPeekZ`: already one frame late, through the breakpoint callback;
- `GXReadDrawSync`;
- port hooks patched into the game: `gx_offscreen_begin`, `gx_hud_begin`,
  `port_copytex_blur`, `port_copytex_mip_alias`, `port_stereo_*`,
  `port_tev_indirect`.

Size and other citro3d users:
- About 150 `GX*` entry points (a regex count, approximate).
- citro3d users outside the GX path: touchui (citro2d, ~25 call sites, drawn
  from `VIWaitForRetrace`), movie_3ds.c, the GX reset in vi_3ds.c:79-87,
  gx_capture.c, gx_selftest.c, main.c.

**Consequence for the design: split GX at the point where game memory is
read.**
- A **front half stays on the game thread**:
  - everything that reads game memory: display-list hash and lookup, decode of
    dynamic lists and immediate mode into port-owned buffers, the texture
    fingerprint plus a copy of CPU-written texture data;
  - shadow state for getters (`g_gx` scissor);
  - display-list recording;
  - the patched hooks that return values.
- The **back half moves to core 2**, and it reads only port-owned or copied
  data:
  - state translation: prepare, TEV compile and cache, matrices, light setup;
  - texture conversion of data already captured;
  - citro3d calls, flushes, frame end.
- The FIFO carries "draw cached entry X with state snapshot S" records, not
  raw pointers into game memory. That removes hazard 1 (late reads of normals,
  splash colours, star lists, freed blocks) and hazard 2 (hashing memory that
  is changing), and it works with one frame of lag.
- The cost: the front half keeps some work on core 0 (hashing, dynamic decode,
  `dl.inc_copied_vertices`). Step 0 must time the front half and the back
  half separately (`gx.dl.lookup` / `gx.dl.validate` / `gx.gather` vs
  `gx.prepare` / `gx.prep.*` / `gx.draw`). DRAWCALL_PLAN.md's Azahar numbers
  suggest prepare and draw (state translation plus citro3d) are the larger
  part.
- Frame pacing: today the GPU already runs one frame behind (pipelining). A
  render thread one frame behind on top of that means 2 frames of latency,
  with deeper buffering of per-frame port buffers (dyn pool, command buffers).
  The same-frame variant avoids that. Decide after Step 0.

### B. Fork-join offload of hot kernels (lower risk, smaller)

Keep one GX thread, but hand self-contained loops to core 2 and join before
their results are used:
- dlcache incremental vertex updates and per-play frame copies
  (`dl.inc_copied_vertices` 36k per frame, `frame_copy16`)
- texture conversion and mip generation on load
- the per-frame vertex gather (GRAPHICS_PLAN.md 1.4)
- the game's CPU skinning (`ObjModel_BlendVertexStream` /
  `ObjModel_BlendNormalStream`), if PERF_WRAP shows it is large. Each model
  is independent, so it is a natural split. It is game code, so the change is
  a wrapper that submits the job and joins before the model's draw.

Each item is probably 1-5 ms. Do these only if Step 0 shows one is large, or
as a stepping stone while A is being built.

### C. Audio to core 1 (small, easy)

The MusyX synth pump (`audio.pump` 0.7-1.3 ms) and the audio stream thread
run on core 0. Moving the stream thread to core 1 is a one-line change to
`threadCreate`'s core argument. The pump needs a lock against the game's
sound calls. ~1 ms.

### D. Parallel game logic: no

The game is not thread-safe: object updates share globals, the heap and
`mmAlloc`. Out of scope.

## Attract scene I/O spin (found in this log, not multicore)

In the attract/title scene (map 63), the game thread spins in a wait loop
that calls `padUpdate()` about 9,000 times per frame (`stub.PADClamp` and
`stub.PADReset` counts). Meanwhile the worker spends ~30 ms per frame on one
~20-40 KB async read (`dvd.async_reads` 1/frame, `worker.busy_us` ~30 ms).
The spin count follows the I/O time across the run (9194 ↔ 32.0 ms, 7599 ↔
28.2 ms, 10137 ↔ 33.9 ms, ~3.5 us per spin). So roughly half of that
scene's ~60 ms CPU frame is waiting for the SD card, not computing. This also
explains the 4.6 ms gap between the two "overlap on" attract segments.

- Candidates for the loop: `DVDRead` (foxhollow/game/src/main/fileio.c:134,
  a synchronous read built on `DVDReadAsyncPrio` plus a `padUpdate` spin),
  the asset wait in pi_dolphin.c (~line 1235), `skyLoadLights` (dlls/engine/5/5.c:1634).
- The hub has none of this (`dvd.bytes` 0, PAD 1/frame), so it is not the hub
  bottleneck.
- Next: log the file name, offset and size of async reads in steady state
  (rate-limited, e.g. the top file per report window). Then either keep that
  file in RAM or make the loop yield (`svcSleepThread`) instead of spinning.

## Order of work

Superseded on 2026-10-14 by the build steps in "Recommended 3-core design".
Step 0 is done (results above).

## 60 fps chase: cuts baseline and the render-thread wall (2026-10-30)

Console loop (in-game HTTP server + hb:ldr relaunch, README "In-game network
server") measured the hub spawn with the A/B harness.

**Cuts config** (env.txt: S3D_OBJ_DIST=30, S3D_OBJ_MINPX=45, S3D_PLANTS=0,
S3D_SHADOWS=0, S3D_FUR_MODE=0, S3D_GRASS_BUDGET=0, S3D_GPU_BLUR=0):

| config | frame | GPU | sceneRender |
|---|---|---|---|
| defaults | 56.9 ms | 28.1 ms | 29.3 ms |
| obj dist 75% | 53.2 ms | 24.8 ms | 25.0 ms |
| obj dist 50% | 49.0 ms | 20.7 ms | 20.5 ms |
| full cuts | 45.8 ms | 15.0 ms | 17.4 ms |

The GPU is below the 16.7 ms target at the cuts config; the render thread is
not. Sampler (S3D_PROBE=2, cuts, delta 1 phase, 46.5 ms/frame):

- Game thread waits **20.4 ms/frame** on the render thread fence; its own work
  is ~26 ms (logic 4.2, mapBlock game 4.0, objRender game 1.3, scene 2.7,
  outside game 2.7, glow 0.7, rcp/lights 0.3, plus recording).
- Render thread ~46.5 ms: **gx.dl.play 24.2** (cached-DL segment replay +
  gather), then dl.batch 1.6, state delta 1.5, c3d.uniforms 1.5, dl.lookup
  1.3, gx.draw 1.3, tev.apply 1.0, immediate 0.9, tev.texmtx 0.9, ~7 ms of
  small items incl. 2.6 idle.
- Recorder per-op: state delta 679/frame @ 5.3 us, gx_dl_call_hashed
  303/frame @ 8.2 us, GXSetTexCoordGen2 551/frame @ 3.1 us (excluded from
  deltas on purpose: DLs may write CP/XF state behind the recorder's back),
  matrix block 224/frame @ 7.0 us, GXBegin 193/frame @ 1.3 us.
- Delta 1 vs 0 at the cuts config: 46.5 vs 48.9 ms (the delta keeps paying).

**Next targets, ranked:**

1. `gx.dl.play`'s per-op cost (~5.5 us x ~4400 ops = the 24 ms). The replay
   of a cached DL re-runs the translation of its commands; per-op dispatch and
   the state-apply path are the cost. Cut per-op cost before cutting op count.
2. GXSetTexCoordGen2 (1.7 ms game + ~3 ms RT replay): either refresh the
   delta shadow after every DL play so the setter can join the delta path, or
   make the wrapper cheaper.
3. Game-side glow: 1388 immediate-mode calls per frame in the glow passes.
4. If 1-3 land, the game thread's own ~26 ms (logic/traversal ~15 ms +
   recording ~10 ms) becomes the next wall, and 60 fps would still need the
   game code itself under 16.7 ms — beyond incremental cuts.

Realistic after 1-2: ~30-35 ms (~30 fps) at the cuts config. The cuts are
env-only; normal play keeps the settings.txt quality.

## 60 fps chase: the 30 fps cap and the DL-call wall (2026-10-30, night)

At the cuts config the PROFILE rtops (per-opcode replay time) showed the
frame boundary opcode at 22.6 ms/frame — that is the `S3D_FPS_CAP30=1`
pacing: `C3D_FrameRate(30)` makes the frame-end swap wait for the 30-fps
cadence even when `C3D_FrameBegin` skips its sync (slow frames). With
`S3D_FPS_CAP30=0` the boundary drops to ~1.0 ms/frame in the steady state.
The steady-state frame itself did not move yet (55.6 vs 55.3 ms): the render
thread's own work is the wall either way, but the cap would have bound any
gains below 33 ms, so it must stay off for this chase.

With the pacing out of the way the rtops at cuts read:

- gx_dl_call_hashed 43.6 ms/frame @ ~320 calls (**~136 us each**, profile)
- (state delta) ~4.8, immediate-mode data ~3.6, (matrix block) ~0.5

New instrumentation for the DL-call cost (S3D_DLCALL_PROFILE=1, both builds):

- gx_dlcache.c sums ticks per DL call across its parts (prepare / lookup /
  dlb / validate / gather / draws / uncached) and reports "dlcall" lines
  every 120 render-thread frames (gx_dlcache_prof_frame, driven from the
  render thread's frame boundary — the game thread also runs
  gx_dlcache_frame_end, so the window counter must not live there).
- gx_render.c splits each cached draw into flush / prepare / layout+vsrc /
  draw ("drawc" lines, same window).

First corrected dlcall windows (normal build, cuts, cap off, per frame):
prepare ~4.8, lookup ~1.5, dlb ~0.4, validate ~0.5, gather ~0.8, draws ~24.8
(~94 us per call), rest ~0.4. The draw submission inside the cached-DL play
loop is the single remaining item; the drawc split is queued for the next
console run.

## 60 fps chase: stereo at full separation, the water 25 ms, 3dslink loop (2026-10-30)

Two changes to how the chase is measured, then a bisect result.

**Forced stereo.** S3D_STEREO_FORCE=1 renders full eye separation regardless
of the slider. This made the real 3D cost visible: the GPU time doubled
(15.0 -> 29.9 ms/frame) because at slider-0 the port had been rendering one
eye only. Hub frame at cuts + forced stereo: 49.4 ms (delta 1) / 52.1 ms
(delta 0), sceneRender ~17 ms. All further numbers are forced-stereo.

**Console loop now runs over 3dslink.** With the console sitting in the
Homebrew Launcher's netloader, a build is launched with:
`docker run --rm --network host -v $PWD:/workspace star3d-builder -c
'/opt/devkitpro/tools/bin/3dslink -a <ip> /workspace/out/star3d-nodata.3dsx'`
(bypass the image entrypoint, which mangles the args). env.txt and the romfs
data come from the SD as before; the in-game HTTP server is up ~10 s after
launch. This replaces both the ftpd dance and the restart-based reload for
iteration: one 3dslink per build, no restart needed.

**drawc bisect result.** The draw-cached cost (31 ms/frame, ~304 calls) is:

- flush 0.11, prepare 0.14, layout 0.14 (14 switches/frame), water 24.98,
  vsrc 0.11, draw 5.7

So layout switches are cheap and the whole cost sits in the water step:
water_tessellate is entered 380x/frame but only 6 calls are active water
draws. Hoisting the six per-call CFG_LIVE config reads into one cfg_gen
check changed nothing (the early-return path is ~2 us in the disassembly).
A warp-entry tick now separates entry+warp-check time from the tessellation
body; if the body owns the 25 ms, the 6 active draws are tessellating huge
meshes and the fix is capping/merging the water geometry instead of micro-
optimizing the call path.

## 60 fps chase: 30 fps with the 3D effect reached (2026-10-30)

The drawc phase split showed the water cost is the tessellation body (vert
phase 18.98 ms of 19.6; ~9.5k output verts from 211 input tris at tess
n ~7-8; ~535 cycles/vertex of lerp + caustic offsets). S3D_WATER_TESS=0 in
the cuts env removes it (the env change alone, no build): delta 1 = 27.4 ms,
GPU 20.9 ms (~36.5 fps, CPU-bound again). Steady-state drawc 6.6 ms (water
0.38, draw 5.7), dlcall 17.4 ms (prepare 5.0, draws 7.0, lookup 1.6, gather
0.9, uncached 1.6). The mark captures pollute one drawc window (prepare
31.2 / dlcall 86 in the f6604-6687 window) — ignore windows overlapping a
mark.

Next: water.v.pica computes bug 33's waves itself (PICA FLR, warp.v.pica's
math) so tess=0 keeps the wave animation at coarse resolution, and the
CPU-side per-vertex offsets disappear for the non-ripple path. Then the
DL-call cost (prepare 5.0 + draws 7.0) and the game thread are the walls.

## 60 fps chase: water waves moved into the vertex shader (2026-10-30, evening)

water.v.pica now has a wave mode (texwarp.z > 0 = wave_params, z <= 0 =
colour-carried decode), computing bug 33's sawtooth waves from the object
position with PICA FLR (warp.v.pica's math, mad-folded to fit the shared
512-instruction blob: 511 total). With S3D_WATER_RIPPLE=0 the CPU skips the
per-vertex offsets and uploads the wave params, so S3D_WATER_TESS=0 keeps
the animation at the mesh's own vertices; the caustic ripple path is
unchanged. The water_tess_restore default-wave reset applies to wave mode
too (s_wt_waves = true after the wave upload).

Console validation is pending (the console's env.txt was accidentally
overwritten with a one-line file, so the HTTP loop is down until ftpd can
restore it). Pending measurements: cuts + S3D_WATER_TESS=0 +
S3D_WATER_RIPPLE=0 with the wave build (expect frame ~27.4 ms, GPU ~20.9,
with animated water), and a visual check of the wave mode.

Next profile targets after that (cuts, tess=0, from the 27.4 ms run):
dlcall prepare 5.0 ms (state setup; ~50 KCOL dirties/frame, ~27 with
TEV/TEX on top - the GX_PROFILE_DETAIL prep.d_* counters need a PROFILE=1
build to break it down), dlcall draws 7.0 ms (play_entry submission),
drawc draw 5.7 ms, lookup 1.6, uncached 1.6.

## Input record/playback: S3D_PAD_RECORD / S3D_PAD_PLAY (2026-10-30/31)

Records the effective input (buttons, sticks, triggers - hardware, remote
commands and the touch menu) and replays it hands off, so a played route can
be re-run under different configs/builds for A/B tests and captures.

Iterations (each broken in its own way):
1. Per-PADRead capture: the game polls PADRead thousands of times per second
   during loading, so the buffer OOM'd in seconds.
2. 15 ms wall-clock gate: the replay diverged (the game's frame cadence
   drifts between runs, so it sampled different points of the stream).
3. Per-game-frame (vi loop) with the capture inside PADRead: the stream
   contained a constant idle stick and no buttons - PADRead is not a
   reliable sample point.
4. Timestamped wall-clock playback: froze in a menu (the game's load timing
   differs, so the inputs land at wrong menu states).
5. **Working**: padrec_frame (vi loop) captures the composed input once per
   frame; playback advances one recorded frame per game frame AND paces the
   frame to the recorded timestamp (svcSleepThread slices). Frame-aligned
   inputs make the frame-based menus navigate exactly like the recording;
   the paced frame rate keeps the delta-time physics on the recorded path.

Usage: record with S3D_PAD_RECORD=1 (play the route, quit cleanly - the
buffer flushes to sdmc:/3ds/star3d/padrec.bin at shutdown), replay with
S3D_PAD_PLAY=1 (the recorded route drives the game from the title; when the
stream ends the real inputs return). SELECT logs a "pmark" line (position,
no capture) for marking spots. Verification: the replayed map-7 load
matched the recording to the frame (f587 vs f588), and the asset-load
sequence matched for 137 loads with only the last ~15 s drifting (the
airwing takeoff prompt vs the shopkeeper talk - accumulated physics
micro-drift). Open issues while roaming: the DL-cache entry free can crash
(a corrupted heap block - S3D_DL_CACHE=0 avoids it, at the cost of bug 36's
missing geometry), and the render capture (L+R+SELECT mark) can crash the
kernel (deterministic prefetch abort in the kernel, dumps 5/6) - see the
capture bug entry.

## Full-quality baseline: 15 fps at the hub (2026-11-01)

With the cuts env removed (settings.txt re-enables shadows, fur, GPU blur,
grass budget 8, FPS cap 30; the env only has the net server), the hub runs
at 15 fps (~66 ms/frame). Prof zones (Star3D.log, the user's run):
- rt.replay ~60 ms/frame - the render thread, GPU-bound (vs ~21 ms GPU at the
  cuts config, a ~3x)
- game.sceneRender ~35 ms/frame (parallel with the render thread, so not the
  long pole alone)
- rt.fence_wait ~19-21 ms - the render thread waiting on the GPU fence
- gx.water_tess_tris 12254/frame over 5 draws (S3D_WATER_TESS=16, ripple on)
- frame.touchui ~1.2 ms after the every-frame redraw fix (was 0.06 with the
  skip; the blink fix's price, acceptable)
The GPU is the long pole. Attribution plan: the setting A/B runs (diag_3ds.c,
one test per boot, standing still at the hub) over the suspects - shadows,
cull (bug 41's tax), object distance, plants, fur - then the daily play
config is assembled from the numbers instead of blanket cuts. Free/cheap
wins in the queue: bug 41's cull fix (correct back-face culling = GPU
saving), S3D_STEREO_REDUCED (half fur/grass layers + no blur in the right
eye only), a lower S3D_WATER_TESS.

## Render-thread probe: where the translation CPU goes (2026-11-01)

S3D_PROBE=3 at the hub (pond in view, full quality + S3D_VSH_LEAN=3 +
S3D_TEX_RES=1): 73 ms/frame sampled. The sampler's render-thread phase
histogram and the game-thread recorder detail:
- Render thread: gx.dl.segment 22.5 ms, c3d.uniforms 5.3, gx.displaylist
  3.6, gx.draw 3.5, gx.gather16 3.3, (state delta) 2.7, gx.dl.lookup 2.4,
  gx.immediate 2.4, gx.tev.apply 1.9 ... (the "segment" zone wraps the
  cached-draw submission itself, not decode)
- drawc: 33.7 ms/frame across 653 cached draws - flush 0.24, prepare 0.36,
  layout 0.34, **water 20.4**, vsrc 0.22, draw 12.8. wtphase: scan 0.32,
  alloc 0.01, **vert 19.2** (the per-frame grid generation), idx 0.06.
- Game thread recorder: wrapper/encode 14.7 ms + dedup filter 4.4 ms a
  frame (~3400 recorded ops: 1299 state deltas, 800 GXSetTexCoordGen2,
  657 DL calls, 637 matrix blocks).
- Game scopes: objRender 15.6 + mapBlock 10.3 + objRender.transp 4.9 +
  logic 6.0 + scene 3.6 ... (the game thread's own code dominates at this
  spot; the recorder tax is ~19 ms of it).

Conclusions that shaped the work:
1. The water tessellation regenerated its grids every frame although the
   grid is a pure function of the draw's static buffers, the level and
   the settings (waves animate in the vsh, the caustic table is static).
   Now cached (gx_render.c WT_CACHE_*, see the commit) - ~20 ms/frame
   reclaimed on the render thread at the pond.
2. The game-thread recorder (~19 ms) and the per-draw submission (~13 ms)
   are the next two translation-layer costs; the recorder's state-delta
   and matrix-block ops (~11 ms) are the same targets the DL batch and
   RT dedup were built for.
