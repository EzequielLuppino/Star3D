# Bug 6: hub degrades over a long session (plan)

Source: `3DS/build/emu/soak1.log`, a hub session of about 17800 frames, no log rotation, 4 marks.

## What the data says

| Finding | Evidence | Verdict |
|---|---|---|
| dlcache slots run out | The table is 1024/1024 from f1799. About 990 entries stay valid and the pool is pinned at 8169-8191 KB. `full` (draws that run uncached because no slot was free): 1427 (f1799) → 35403 (f3000) → flat to f7200 → 43521 (f7799) → 261302 (f14999) → 283417 (f15599). That is about 80 uncached draws per frame late in the session | **Main cause of the FPS loss** |
| FPS follows `full` | 43-54 fps for f3232-7328 while `full` is flat. 26-40 fps from f7675 while `full` climbs. gpu.draw_us stays at about 1300-1500 per frame, so the extra cost is CPU decoding | Confirms the row above |
| Frame vertex buffer overflow | `frame vertex/index buffer full, dropping draws` x2195 around f1942-f2399. gx.vertices peaked at 19584 per frame. It did not happen again | Dropped draws: a likely cause of shadows or objects disappearing |
| Render targets | Every diag line reads offscreen ok 0xd, failed 0, shadows 1 | Not the cause |
| VRAM | 610-1186 KB free, no allocation failures | Tight, worth monitoring |
| Vertex explosions | badvtx 0 in all 4 marks (f3515, f4402, f9636, f12548). poolreset happened once (f4799) | Not caught. Either the glitch is below the 1e6 threshold or it was not on screen at a mark |
| texcache | 103 → 375 of 512 entries, 0 tombstones | Fine |
| Heap / linear | Heap 16390 → about 11350 KB, then stable. About 15.9 MB linear free | No leak. There is room for a bigger dlcache |
| LOG_ONCE noise | Indirect texturing x133k, TEV >6 stages 13 → 103 | Known, unrelated |

Root cause of the slowdown (`gx/gx_dlcache.c`):
- `find_slot` returns NULL when the probe chain has no free slot or tombstone. `GXCallDisplayList` then does `s_table_full++` and calls `run_uncached`, every frame, for that list.
- `make_room` evicts only to stay under the **byte** budget, never to free a **slot**.
- `maintain_table` keeps every valid entry (only stale dynamic markers are dropped), and returns early when `used == live`. A table full of live but cold entries (lists from areas you walked through earlier) is never cleaned up.

## P1: dlcache slot eviction (fixes the FPS loss)

Status: implemented (steps 1-4); step 5 (soak) still to do.


1. **LRU eviction in `find_slot`**: while probing, remember the valid entry with the oldest `last_used` that is not used this frame. If there is no free slot or tombstone, `free_entry` that entry and return its slot. Count these as `s_slot_evicts`.
2. **Cold aging in `maintain_table`**: also drop valid entries unused for more than `DL_COLD_FRAMES` (1800). Remove the `used == live` early-out when cold entries exist, so the rehash actually runs. Count these as `s_cold_drops`.
3. **Grow the cache**:
   - `DLCACHE_SLOTS` 1024 → 2048. The cost is BSS of `sizeof(DlEntry)` × 1024 more; check it in the map file before raising. Keep the table no more than about 50% full.
   - `DLCACHE_BUDGET` 8 → 12 MB. About 15.9 MB of linear memory is free; leave at least 3 MB for the other maps.
4. **Diag**: add `evict %lu cold %lu` to the `dl` line in `port/diag_3ds.c` and to `GxDlStats`.
5. **Verify**:
   - Do a 15+ minute hub soak with the same route.
   - `full` should stay near 0 after the warm-up.
   - fps should stay flat (no step down after about f7000).
   - The `evict`/`cold` counts should grow slowly, not every frame. Thrashing means the budget or slot count is still too low.

## P2: frame vertex buffer (missing geometry and shadows)

Status: step 3 done (`draw` line: drops, drop frames, last drop frame, buffer peaks). First reading in the hub: peak 11032/32768 vertices, no drops. Steps 1-2 wait for a soak that shows drops again.


1. In `gx/gx_render.c`, raise `MAX_FRAME_VERTICES` 0x8000 → 0xC000 and `MAX_FRAME_INDICES` to match. That adds 52 B × 16K, about 830 KB of linear memory per buffer set; check how many sets are double-buffered.
2. Better fix: when the buffer is full, flush the batch and continue in a fresh chunk (or a spill buffer) instead of dropping the draw.
3. Replace the plain LOG_ONCE with a per-frame drop counter in the diag line (`drops %lu`), and record the frame plus player position on the first drop of each burst.
4. Verify by re-running the f1942-f2399 area (same save, same route): no drops, and shadows stay.

## P3: vertex explosions (not caught yet)

Status after soak 2: two fixes in.
- Hot-set misses (step 2): non-full frames also scan a rotating 1/`HOT_SLICE` (4) window of cold refs and add the ones that changed to the hot set (`dl.late_hot`). A vertex that starts animating late is now stale for at most 4 frames.
- Particle matrix runaway (caller 0x10cc68): `GXLoadPosMtxImm` zeroes matrices with a view-space translation over `GX_POSMTX_CULL` (1e6) and counts them (`posmtx culls` on the `draw` line).

1. **Dyn pool reuse race**: the pool resets at the start of the next frame, but the GPU may still be reading the previous frame from it. Fix: split the pool into 2 halves (ping-pong per frame) or defer the reset until after the previous frame's GPU fence. A one-frame burst right after `poolreset` would point to this.
2. **Hot-set misses**: a vertex that starts animating after `HOT_LEARN_SCANS` is only refreshed every `HOT_RESCAN_EVERY` (64) frames, which leaves it stale or stretched. Fixes:
   - When a full scan finds a new hot source, run full scans on the next few frames.
   - Lower `HOT_RESCAN_EVERY` to 16 and measure the cost.
3. **Better detection**:
   - Add a per-entry bounding-box check: log when a gathered vertex falls far outside the box recorded for that entry (for example 4× the box size). This catches "stretched" vertices that stay below 1e6.
   - Log `poolreset` with the frame number so it can be matched against glitches.
4. **User side**: press L+R+SELECT **the moment** a glitch is on screen, so the mark lands on it.

## P4: shadows

Status: copy counters (done/skipped) and offscreen misses are in the `draw` line; the mark census shows the offscreen (shadow) states and their draws for one frame.


Targets were always fine, so check the draw side:
- Add per-frame counters to the diag line: shadow casters drawn, shadow passes skipped by the copy budget, and shadow draws dropped by P2.
- After P1 and P2, check whether shadows still vanish. If they do, a mark while they are gone will show which counter is 0.

## P5: VRAM headroom

Status: done (`vram ... (min N)` on the `tex` line, sampled every 60 frames). First reading: 606 KB minimum.


- Track the VRAM free low-water mark (`vram min %lu KB`) in the diag line.
- If it gets near 0 after the P1/P2 changes, move the low-priority targets back to linear memory.

## Order

- P1 first: it is the measured cause and a small change.
- Then P2.
- Then P4/P5 counters, which are cheap and can go in the same build.
- P3 last, because it needs a reproduction. Its diagnostics (bbox check, poolreset log) can go in with P1 so the next soak gathers that data too.
