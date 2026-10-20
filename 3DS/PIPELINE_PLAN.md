# CPU/GPU overlap (GPU pipelining)

Status (2026-10-14): build **5EB6A447** waits for the previous frame only.
It is the second version and has not run on hardware yet. The first version,
0AEC7BB3, ran on hardware: 10.3 → 12.9 fps in the hub with no errors (see
"Hardware results"). Azahar can't show the gain: it does not model GPU time.

## Why

Hardware log of build 2091F24C (New 3DS XL, ThornTail hub, `Star3D.log`):

| Section | fps | frame | CPU in frame (`gpu.submit_us`) | waiting for GPU (`gx.wait_vsync`) | GPU (`gpu.draw_us`) |
|---|---|---|---|---|---|
| Attract scene (map 63) | 25-31 | 32-39 ms | 28-33 ms | 4-6 ms | 4-6 ms |
| Hub (maps 7/8) | 8-14 | 70-122 ms | 44-84 ms | 25-38 ms | 25-39 ms |
| Hub, standing still (f1063-1182) | ~10 | ~103 ms | ~65 ms | ~36 ms | ~38 ms |

In every block, frame = CPU + GPU, and the time spent waiting is as long as
the GPU frame. The two never ran at the same time:

- citro3d 1.7.1 keeps its gx queue stopped during a frame. It only starts it
  in `C3D_FrameEnd`, and `C3D_FrameBegin` waits for it to finish.
- `VIWaitForRetrace` (port/vi_3ds.c) calls `gx_frame_end` and then
  `gx_frame_begin` right away, and the game logic of the next frame runs after
  that.
- So the CPU is idle for the GPU's ~38 ms, and the GPU is idle for the CPU's
  ~65 ms.

With overlap, a frame takes max(CPU, GPU) instead of CPU + GPU:
- Hub, standing still: ~103 → ~65 ms (10 → ~15 fps).
- Hub average: ~80 → ~50 ms (12 → ~20 fps).
- Past that, the CPU sets the frame time. 30 fps needs the CPU frame under
  33 ms, which is the CPU work planned for later (DRAWCALL_PLAN.md). Once the
  CPU is fast enough, the GPU (25-39 ms in the hub) becomes the limit, and
  only overlap keeps it busy all the time.

## Design

### Frame queue: gx/gx_c3d_queue.c

A copy of citro3d 1.7.1's `renderqueue.c`:
- It defines every symbol of that file, so the library's object is never
  linked. Check: `star3d.map` has no `libcitro3d.a(renderqueue.o)`.
- If citro3d is updated, re-check this file against the new `renderqueue.c`.
  A symbol added there and not here would cause duplicate-definition errors at
  link time, so the failure is loud.

Pipelined mode (`C3Dx_SetPipelined(true)`):

- **Two command buffers.** The second one (1 MB of linear memory) is
  allocated the first time pipelining turns on. `C3D_FrameBegin` switches to
  the other buffer and **does not wait**.
- **`C3D_FrameEnd` waits for the previous frame only**, just before
  submitting this one. It does not wait for this frame's own mid-frame work.
  - It polls the queue's completion count up to `prevEnd`, the queue index
    just past the previous frame's entries.
  - 0AEC7BB3 waited for the whole queue instead. That included the current
    scene, which is submitted at the post-effect copies late in the frame, so
    the CPU still waited ~10 ms per hub frame. While it waits, the watchdog phase reads `C3D_FrameEnd (GPU: previous
  frame)`. The wait is needed for three reasons:
  - The screen transfers of frame F must land in the right framebuffer, and the
    swap for F-1 must happen first.
  - It frees the command buffer for the frame after.
  - It is the point the GPU fence counts on (below).
- **The queue keeps running and is never stopped while busy.** Work submitted
  mid-frame (EFB copies, PeekZ, the frame's final list) runs as soon as the
  GPU gets to it.
  - Entries are removed by clearing the queue whenever it happens to be idle,
    checked at frame begin, frame end and each split (`queue_make_room`).
  - If fewer than 48 of the 256 entries are left, it waits for the GPU and
    clears. Counter: `gpu.queue_full_waits`.
- **Screen swaps.** The completion callback only fires when the whole queue
  drains, which rarely happens when pipelined. So the swap is also done once
  the queue's completion count passes the frame's transfers (`poll_swap`, at
  frame begin and frame end).
- **Cache flush before a mid-frame submit.** Data must be written back first,
  because the GPU may run it at once.
  - The hook `gx_render_flush_new_writes` flushes only what was written since
    the last split: frame vertex, index and dyn data, and the dyn pool when a
    draw reads it directly. The command list is flushed through
    `GX_CMDLIST_FLUSH`.
  - Everything else the GPU reads is flushed where it is written.
  - `C3D_FrameEnd` still flushes the whole heap once per frame.
  - 0AEC7BB3 flushed the whole linear heap at each split: about 0.4 ms each
    on hardware, 2.4 ms per hub frame.
  - `S3D_GPU_SPLIT_FULLFLUSH=1` (touch UI "GPU overlap: full flushes") brings
    back the full flush, in case something written mid-frame is missed and
    shows up as garbage.

Fixes that apply in both modes (libctru behavior, checked by disassembling
`libctru.a(gxqueue.o)`):

- **`gxCmdQueueWait` returns as soon as the GPU is idle**, before citro3d's
  completion callback (the one that swaps screen buffers) has run on the GSP
  thread. It also returns at once on a stopped queue that still has entries.
  So:
  - Waits now run the queue first.
  - The swap is done under a lock by whichever side finds it pending first:
    the waiting thread, or the callback. The callback only swaps when the
    whole queue is done, so a late callback can't swap a newer frame before
    its transfers are written.
- **`gxCmdQueueStop` on a busy queue loses completions.** The commands still
  in flight are never counted, and the next wait hangs. The queue is only
  stopped once it is idle.

Not pipelined, it behaves like stock citro3d.

### GPU fence (gx_init.c)

Frames are counted by `gx_render_frame_number()`:
- `s_gpu_submitted` is the last frame handed to the GPU.
- `s_gpu_done` is the last frame the GPU has finished. It becomes
  `s_gpu_submitted` at every frame end (before the update) and after every
  `gx_gpu_wait_idle()`.
- This holds in both modes. Not pipelined, `C3D_FrameBegin` waited.
  Pipelined, `C3D_FrameEnd` waited.

API (`gx_internal.h`):
- `gx_gpu_free_linear(p, last_used)` and `gx_gpu_free_tex(tex, last_used)`:
  free at once if frame `last_used` is done, otherwise queue the free until it
  is. Deferred frees are released at frame end (`gpu_retire`).
- `gx_gpu_wait_idle()`: waits for everything submitted, then releases deferred
  frees.
- `gx_gpu_reclaim()`: used when an allocation fails. It waits for the GPU,
  frees the deferred memory and returns true if any was freed.

### Everything the CPU writes that the GPU reads, and what was done

| Resource | Hazard with overlap | Fix |
|---|---|---|
| Frame vertex/index/dyn buffers (gx_render.c `s_vbuf`, `s_ibuf`, `s_dynbuf`) | Frame F+1 overwrites what the GPU is drawing for F | Two sets, used by frame parity. +2.9 MB linear. If the second set can't be allocated, pipelining stays off (`gx_render_can_pipeline`) |
| Command buffer | Same | Two buffers (gx_c3d_queue.c) |
| Display list cache entries (gx_dlcache.c `free_entry`) | Bug 17: memory freed and reused while an earlier draw still reads it. That hung the GPU on hardware | `gx_gpu_free_linear(mem, last_used)` replaces the "free at next frame start" list |
| Animated meshes, incremental buffers in the dyn pool (`inc_dyn`) | Rewritten in place every time the entry plays | Each play draws from a copy in frame memory (`frame_copy16`). Also fixes **bug 18** (an entry drawn twice in one frame showed only the last pose). Cost: memcpy of 12 bytes per vertex, counter `dl.inc_copied_vertices` (Azahar attract scene: 28k vertices = 340 KB per frame, ~0.85 ms at 400 MB/s). Out of frame memory, the pool buffer is drawn directly (old behavior, counter `dl.inc_drawn_in_place`) |
| Dyn pool reset | Pool buffers drawn directly in the previous frame | Reset waits for the GPU if that frame isn't done |
| Texture cache evictions (gx_tex.c `evict`) | `C3D_TexDelete` of a texture the previous frame samples | `gx_gpu_free_tex(&e->tex, e->gpu_frame)`. Failed allocations call `gx_gpu_reclaim()` and retry |
| EFB copy textures (gx_copy.c `free_later`) | Its own two-frame delay list freed the oldest entry immediately when full | `gx_gpu_free_tex(tex, current frame)` |
| GXPeekZ depth readback | One buffer, read on the CPU one frame later. Pipelined, that copy may not have run yet, and the GPU may write it while the CPU reads | Two buffers. Reads use the newest copy the GPU has finished (two frames old when pipelined) |
| citro2d (touch UI, low resolution upscale) | citro2d refills its vertex buffer from the start every frame | `gx_ui_begin` and `lowres_present` wait for the previous frame (`gx_gpu_wait_previous`, zone `gx.wait_gpu_prev`). 0AEC7BB3 waited for the whole queue (0.5-0.9 ms per hub frame) |
| Render capture, dump hold, copy dump (debug) | Read the framebuffer / copies on the CPU | `gx_gpu_wait_idle()` first |
| Save state load (vi_3ds.c `sGxReset`) | Frees the texture and display list caches | `gx_gpu_wait_idle()` first |
| `gx_deinit` | Frees everything | `gx_gpu_wait_idle()` first |
| Render targets | All GPU work and transfers run in one queue, in order | Nothing needed. `C3D_RenderTargetDelete` waits (outside frames only, as before) |
| Light, fog and proctex LUTs, shader code, uniforms | citro3d copies them into the command buffer | Nothing needed |
| Zero-copy textures (game memory, MEM1) | The game may overwrite or free them while the previous frame samples them | Not fixed: one frame of wrong texels at worst. Texture reads can't hang the GPU |
| Movie frames (port/movie_3ds.c) | Slots are rewritten on the CPU | 4 slots, each tied to one of the game's THP decode buffers. The slot written is the one being decoded into, not the one on screen. Not changed |

Known side effects:
- One extra frame of latency (input to screen), like any pipelined renderer.
- PeekZ (lens flare, sun occlusion) answers from two frames back instead of one.

## Switches and counters

- `S3D_GPU_PIPELINE`: default 1. Also in the touch UI as Settings >
  "GPU overlap (pipelining)", a live toggle for A/B tests. It switches at the
  next frame start. It is forced off while the GPU trace window
  (`S3D_GPU_TRACE`) is active, and off if the second set of frame buffers
  could not be allocated. The log prints `GPU pipelining on/off` when it
  changes.
- `gpu.wait_us`: per frame, how long `C3D_FrameEnd` waited for the previous
  frame.
  - About 0 means the CPU is the bottleneck.
  - Large means the GPU is the bottleneck.
- `gpu.submit_us`: CPU time from frame begin to frame end, now without the wait.
- `gpu.draw_us`: when pipelined, this is **not** the GPU frame time. It
  measures from frame end until the queue first drains. Most of the hub frame
  is submitted mid-frame, so it reads about 1.5 ms. When not pipelined, it is
  the GPU frame time as before.
- `gpu.pipelined_frames`: frames that ran pipelined.
- `gx.wait_vsync`: pipelined, this is only the VBlank wait (fast frames).
- `gx.wait_gpu_idle`: explicit waits (touch UI, captures, reclaim).
- `gpu.split_flush`: cost of the linear heap flush before each mid-frame submit.
- `gpu.deferred_frees`, `gpu.reclaims`.
- `dl.inc_copied_vertices`, `dl.inc_drawn_in_place`.

Expected frame time when pipelined:

```
frame ≈ wait_vsync + submit_us + wait_us
```

`wait_us` replaces most of the old `wait_vsync`.

## Hardware test

1. Copy `out/star3d.3dsx` (no .cia; `docker run --rm -e NO_CIA=1 -v
   "D:\opencode:/workspace" star3d-builder`). Check the `build id` line
   matches.
2. Remove the trace switches from env.txt (`S3D_GPU_TRACE`,
   `S3D_GPU_TRACE_FROM`). `S3D_GPU_TRACE=6 FROM=45` is harmless (empty
   window), but any active trace window turns pipelining off.
3. Go to the same spot in the ThornTail hub as the 2026-10-14 log (player
   -6386 -756 -3137, angle 53). Stand still about 15 s.
4. Settings > "GPU overlap (pipelining)": on for about 15 s, then off for
   about 15 s. Optionally, with it on, also turn on "GPU overlap: full
   flushes" for about 15 s.
5. Send the log. Compare the perf blocks:
   - fps
   - `gpu.submit_us`, `gpu.draw_us`, `gpu.wait_us`
   - `gpu.split_flush`
   - `dl.inc_copied_vertices`

What to look for:
- **A hang** (watchdog `stuck ... in "C3D_FrameEnd (GPU: previous frame)"`):
  a GPU hang, like bugs 14/15/17.
  - Note the frame, then retest with the toggle off to confirm it's the
    pipelining.
  - Suspect first any CPU-written GPU memory not covered by the table above.
    The trace (`S3D_GPU_TRACE`) turns pipelining off, so it can't be used to
    bisect this. `S3D_GPU_CHECK_REUSE=1` still works.
- **Flicker or wrong geometry** only when it's on: a resource the CPU rewrites
  that the table missed.
- **Screen tearing or old frames shown**: the swap logic in gx_c3d_queue.c.
- **The touch UI flickering**: citro2d's vertex buffer (gx_ui_begin wait).

## Hardware results

0AEC7BB3, hub at -5587 -781 -3291 standing still, 2026-10-14 log:

| | fps | frame | CPU (`submit_us`) | frame end wait (`wait_us`) | frame begin wait (`wait_vsync`) | `split_flush` | `wait_gpu_idle` |
|---|---|---|---|---|---|---|---|
| overlap off | 10.3 | 96.8 ms | 61.5 ms | - | 35.1 ms | - | - |
| overlap on | 12.9 | 77.0 ms | 67.0 ms | 9.7 ms | 0 | 2.4 ms | 0.6 ms |

- No hang, error or visual problem was reported.
- The gain fell short of max(CPU, GPU) for two reasons, both fixed in
  5EB6A447:
  - The frame end waited for the frame's own scene, which is submitted
    mid-frame.
  - The CPU grew by 5.5 ms: whole-heap flushes 2.4 ms, the touch UI's full
    wait 0.6 ms, mesh copies ~1 ms (`dl.inc_copied_vertices` 36k per frame).
- Expected with 5EB6A447: frame ≈ CPU (~63-64 ms) → ~15.5 fps there, if the
  GPU (~37 ms) stays fully hidden. `gpu.wait_us` should be near 0.

5EB6A447, 2026-10-14 log (repo root `Star3D.log`). The run covers the title
and attract scene (map 63, several on/off toggles) and then the hub (map 7,
f1928-2437) with the order: overlap on, + full flushes, both off. The touch UI
does not log the full-flush toggle. Its position comes from the low-fps report
windows while the menu was open (f2063-2088 and f2266-2277). Hub, standing
still (644 draws, 72k triangles):

| hub | fps | frame | CPU (`submit_us`) | `wait_us` | `wait_vsync` | `split_flush` | GPU (`draw_us`) |
|---|---|---|---|---|---|---|---|
| overlap on (targeted flushes) | 15.2 | 65.8 ms | 65.7 ms | 0 | 0 | 2.45 ms (10 calls) | 21 ms |
| overlap on + full flushes | 14.7 | 67.9 ms | 67.9 ms | 0 | 0 | 2.56 ms (10 calls) | 21 ms |
| both off | 10.6 | 94.1 ms | 59.5 ms | - | 34.5 ms | - | 36 ms |

- **Overlap works as designed.** The frame equals the CPU time: the GPU's
  wait is gone (`wait_us` 0, `wait_vsync` 0). The hub went 10.6 → 15.2 fps
  (-28 ms per frame).
- **Full flushes are slower, but only by ~2 ms (3%).** `split_flush` itself
  grows by only 0.1 ms. The rest is the CPU running on a flushed cache
  afterwards. Keep targeted flushes as the default. Full flushes remain the
  fallback if a stale-data glitch shows up.
- **The pipelined CPU overhead is still +6.2 ms** (65.7 vs 59.5 ms CPU). That
  is 2.45 ms of split flushes plus ~3.7 ms elsewhere: mesh copies
  (`dl.inc_copied_vertices` 36k/frame), the touch UI wait, and queue
  handling. Getting it back is worth ~10% fps. Next step: time
  `frame_copy16`, and count `gx.wait_gpu_idle` calls per frame.
- The GPU runs for only 21 ms of a 66 ms frame, so **the hub is now
  CPU-bound**. 30 fps needs the CPU under 33 ms. See MULTICORE_PLAN.md.
- Attract scene (map 63, 333 draws): off 82-84 ms (12 fps), on 57.9-62.5 ms
  (16-17.4 fps). The two "on" segments differ by 4.6 ms. That difference
  tracks a busy-wait on SD reads (see MULTICORE_PLAN.md, "Attract scene I/O
  spin"), not the flush mode.
- **New GPU hang at the end of the run, with overlap OFF.** Hub, f2439,
  about 20 s after overlap was turned off: `stuck in "C3D_FrameBegin (GPU:
  previous frame)"`, worker idle. Logged in OPEN_BUGS.md (bug 17 follow-up).

## Builds

- 5EB6A447: second version.
  - The frame end waits for the previous frame only.
  - The cache flush at splits is targeted, and there is a full-flush toggle.
  - citro2d waits for the previous frame only.
  - Queue: 256 entries, cleared when idle.
  - Azahar: renders, no errors.
- 0AEC7BB3: first pipelined build (2026-10-14).
  - Azahar: the attract scene, movie and touch UI render in both modes, with
    no errors.
  - Azahar numbers: `gpu.split_flush` 6 per frame, 0.06 ms each; `gx.wait_gpu_idle` is the
    touch UI wait (once per frame).
  - Not run on hardware yet.
