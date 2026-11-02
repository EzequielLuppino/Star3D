# Bug 40 - the "log snapshot (mark)" capture crashes the console

Session notes, 2026-11-02. Fix: see `gx_capture.c` (the capture writer thread) and
this file's "Fix" section. OPEN_BUGS.md row 40 holds the short version.

## The feature

The mark (L+R+SELECT, Settings > "Log snapshot (mark)", remote `mark`) runs
`diag_mark()` (port/diag_3ds.c): it logs a stats snapshot, requests a one-frame
render census, and requests a render capture (`gx_capture_request()`).

The capture (gx/gx_capture.c) runs over 5 frames at frame begin, after
`C3D_FrameBegin` waited for the GPU:

1. frame N+1: `color.bmp`, `alpha.bmp`, `depth.bmp`, `depth.bin`,
   `copyN.ppm` (+ mip levels), `info.txt`; opens `states.txt` as the log tee
2. frame N+2 (ID pass): renders every state in its index color, logs each
   state's TEV/texture dump into `states.txt` (the tee), writes
   `tex_sN_uM.bin` for states with >= 7 GX stages (S3D_CAP_TEXDUMP); then
   `id.bmp` at the next frame begin and closes the tee
3. frame N+3: `tint.bmp`; frame N+4: `light.bmp`; frame N+5: `normals.bmp`

All of this file I/O ran **on the render thread**, inline, while the game
thread waited for the render thread and the log writer / game DVD reads kept
using the SD card.

## What the logs and dumps show

### The crash (9 identical Luma dumps: crash_dump_05..14)

All dumps are the same kernel fault:

```
prefetch abort, core 1 (one dump: core 0), SVC mode
pc    FFF1C848   lr    FFF23C38   sp    FF40FCA0   cpsr A0000013
r0    80000000   r1    00000000  r2    00000019  r3    00001003
r4    00000005  r5    00000400/00000C00  r6/r7 = VRAM addresses (1F1687F6, 1FF82480 ...)
ifsr  00000002 (FSR "debug event")   process: fs (the FS module was current)
```

- Same fault on core 0 and core 1, across 3 days and 9 boots, always tied to a
  mark/capture. "Deterministic" in the sense that once the capture runs, some
  core faults at FFF1C848.
- The faulting instruction (from the Luma code dump, which covers
  pc-92..pc+4) is inside a small kernel11 list/`ldrex` helper; the bytes in
  memory are valid, so the kernel code is not (permanently) corrupted.
- FSR 0b00010 = "debug event" on instruction fetch = on ARM1176 this is the
  BKPT-in-privileged-mode signature (a kernel panic / assert), or a poisoned
  icache line.
- A GBAtemp report (2023) shows the *same* fault address and IFSR
  ("prefetch abort (kernel panic), Fault status: Debug event", pc fff1c848,
  lr fff23ce0) in an unrelated process (socket) - corroborates that this
  address is a kernel path that faults when its state/cache is broken, i.e.
  the trigger is in the environment our capture creates, not in the kernel
  function itself.
- The fault is taken while the **fs module is the current process** and the
  registers carry VRAM addresses - the crash lands while the fs module
  services SD requests.

### Where the capture died (today's run, capture `20261102_164756_f618`)

- `Star3D.log` (Nov 2): mark at f617, then the capture's first phase took
  **85708 ms** ("frame begin took 85708 ms: ... capture 85708"), with the
  watchdog reporting "stuck at frame 618" the whole time and each log flush
  taking 1.5-2.2 s ("SD write of 239 bytes took 1452 ms").
- The capture folder contains color/alpha/depth + copies + info.txt, a
  **352 KB states.txt (vs 1.85 MB when complete), one `tex_s284_u0.bin`
  truncated at 1024 bytes** (should be 4106) and **no id/tint/light/normals
  bmps**. The states.txt ends mid-line (`[ 148.248 f618 IN`).
- So the kernel died **during the ID pass, at state #284 (~19% of the frame),
  in the middle of the SD writes of the state dump / texture dump**.

### Why it takes so long (the freeze)

The console's SD card commits are pathologically slow: every `fclose`/`fflush`
commit stalls for ~1.5-2.2 s (the log is full of those lines). The capture
writes ~4-7 MB in many small files (per-line `fputs` into states.txt, per-state
`fopen`/`fclose` tex dumps, per-pixel `fwrite` in the copy PPMs) from the
render thread, while the game streams from `sdmc:/3ds/star3d/romfs.bin` and the
log writer flushes - so the frame stalls for minutes and the fs module sees a
write storm from three directions.

## Fix

Move **all** capture file I/O off the render thread:

- A low-priority **capture writer thread** owns the SD: the render thread only
  copies pixels and hands over whole-file buffers (bounded queue, ~2 MB
  backpressure), so a mark never stalls the frame.
- Capture files are staged through **in-memory files** (`capmem:` devoptab):
  the states.txt log tee, the per-state tex dumps, `info.txt` and the copy
  PPMs all buffer in the heap and are handed to the writer on close. The
  render thread does zero SD syscalls during a capture.
- The capture folder still holds the same files; they just appear over the
  next ~30-60 s (background), with the game running normally.
- The framebuffer readback now invalidates the data cache for VRAM too (the
  old code skipped VRAM): fixes the 4x4-block stale-content artifact in
  tint/light/normals.bmp (64-byte cache lines from the previous pass).
- The writer drains before a new capture starts, so files of two captures can
  never mix.

Open questions if the crash ever returns: whether the kernel fault is the fs
module's response to the write storm (fixed by serializing the writes), or a
GPU/DMA corruption of kernel state during the ID pass (the remaining suspect;
the framebuffer read is now cache-coherent, which also changes the GPU/CPU
handoff). The crash used to take 6-11 s of passes then fault; with the fix the
passes are instant, so any remaining fault should show a different shape.

## Files

- `3DS/gx/gx_capture.c` - the writer thread, `capmem:` files, buffered BMP/PPM writers
- `3DS/gx/gx_copy.c` - `gx_copy_dump_capture()` (memfile copies for the capture)
- `3DS/gx/gx_render.c` - `dump_tex_raw` writes through `capmem:`
- `3DS/OPEN_BUGS.md` - row 40

## Round 2 (HW, 2026-11-02): the writer wedged

The first writer build no longer crashed the console, but the mark still
froze the game ("takes ages") and the captures only ever got a partial
`color.bmp` (262144 = 256 KB exactly, both runs). The log showed the game
stuck at the capture frame for minutes while the log writer kept working.

- The writer wedged mid-file (8 fs writes in, then nothing) and the render
  thread blocked forever on the queue backpressure - so a slow/hung write
  still froze the game.
- Suspects, both fixed: (1) the writer's 32 KB stdio-buffered writes (the old
  inline capture always wrote ~1 KB chunks); (2) the writer's fopen racing
  the freshly-created capture folder (the old code did mkdir + fopen + write
  + close sequentially on one thread).
- Also fixed: logging from the writer / under the memfile lock could route
  through the states.txt tee back into capmem and self-deadlock (no LOG calls
  on that path anymore - drops are counted and logged from the frame side).

Round 2 changes:
- writer: default stdio buffering (~1 KB fs writes), the capture folder is
  created on the writer right before the first fopen,
- non-blocking memfile chunking: the states.txt tee never waits for the SD;
  a full queue keeps the bytes in memory (2 MB cap, then the file loses its
  tail),
- drop-timeout: a file that cannot be staged in ~10 s is dropped (counted,
  logged once at the end of the capture) instead of stalling the game,
- volume cut: EFB copy PPMs now need S3D_CAP_COPIES=1, per-state tex dumps
  need S3D_CAP_TEXDUMP (default 0, was 7).

## Verified on hardware (2026-11-02, build 8814a5f)

Two snapshots taken by the user, both complete (color/alpha/depth/depth.bin/
info.txt/states.txt ~1 MB/id/tint/light/normals, no truncation). The log shows:
mark at 37.6 s -> capture phase 2.7 s -> "capture passes done, files land in
... (background)" at 46.9 s -> the folder completed in the background. No
crash, no freeze, no dropped files.

## Repro / test

1. `make -C 3DS code` (or `./build.sh code`), push `star3d-nodata.3dsx` as
   `sdmc:/3ds/star3d.3dsx`.
2. On the console, take a mark (L+R+SELECT) in the hub.
3. Expected: the mark logs instantly, the game keeps running, and the capture
   folder fills in over the next ~minute (the SD is slow). No freeze, no
   `capture` stall line, no `[CRASH]`, no new dump in `/luma/dumps/arm11`.
