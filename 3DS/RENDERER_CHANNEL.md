# Source-level renderer channel

The plan: replace the GX record/replay/translate pipeline with a direct
channel between the game's render code and the port's renderer, one system
at a time, with the GX translation staying as the fallback until each path is
proven. Everything is measured with the probe (S3D_PROBE=3) on Azahar and
then hardware.

## Why

The measured translation tax at a busy hub spot (~58 ms frames):
- game thread: the recorder encodes ~3400 GX calls a frame (~15-21 ms:
  wrapper/encode 16.3, the state-delta bookkeeping, the DL hashing)
- render thread: ~40 ms of replay CPU (per-draw state apply, DL lookup/
  validate/gather, submission) under a ~31-41 ms GPU

A direct channel collapses the setters (game-side state tracked in the
port's shadow only), ships the state ONCE per draw with the draw, and
eventually lets the game emit the port's vertex format directly so the DL
decode goes away too.

## The channel, in stages

### A. Draw-attached state (port-side, no game patch)

Today the delta machinery already accumulates setter changes and flushes
them as DELTA/MBLOCK ops at each draw - the per-call cost is the wrapper +
shadow update (~4.8 us x ~3400). Stage A makes the draw carry everything:

- the game-side setters keep updating the shadow state (no ring writes,
  as today) but the wrappers get out of the way entirely: the game calls
  GXSetTevOp -> __real_ (the shim stores) + a dirty mark, nothing else
- GXCallDisplayList serializes (state-delta blob, matrix block, list,
  size, hashes) into ONE recorded op
- the render thread applies the blob once and runs the DL path

Expected: game-thread recorder ~15.7 ms -> ~3-4 ms; render-thread
state-delta apply and per-op dispatch shrink.

### B. Game-side draw sites (sed patches)

`objprint_dolphin.c` has the per-object setup (objSetupRenderOpGxState,
82 GX calls + helpers, ~300 in the file) and three GXCallDisplayList
sites (1899 / 2114 / 2451) plus a GXBegin path (792). Stage B patches
the draw sites to call `port_model_draw(list, size)` directly, so the
game thread stops passing through the GX draw wrappers and the port
sees exactly one seam per draw. The state snapshot stays as in A.

### C. Material + vertex emission (the fork proper)

- patch objSetupRenderOpGxState / setupToRenderMapBlock (shader.c) to
  build a compact material descriptor instead of the GX stream; the port
  caches material-hash -> baked PICA state (the TEV compiler runs once
  per material, not per draw)
- the GXBegin/GXPosition* emitters write the port's vertex format
  directly into a game-thread buffer; the DL hash/validate/gather
  machinery retires for converted paths
- the record/replay collapses to (material-id, matrix-id, vertex range)

## Order of conversion

1. The batchable models (flag 0x8000: grass clumps, flowers) - one
   segment, one material, drawn in runs; the port side already batches
   them (dlb).
2. The map blocks (mapBlockRender, ~750 DL calls a frame) - the big one.
3. Everything else (water, fur, particles...) stays on the fallback until
   reached.

## Measurement

Each stage: build, run the probe in Azahar (the .3dsx + romfs in the
Azahar sdmc), compare the recorder ops / render-thread phases / frame
time against the baseline, then verify on hardware before the next stage.
The GX fallback stays live throughout: any path not yet converted renders
exactly as it does today.

## Status

- Stage A: implementing (the draw-attached op + the shim changes)
- Stage B: the three objprint_dolphin.c draw sites identified
- Stage C: material descriptor schema TBD (mirrors the port's per-draw
  state inputs: TEV stages, textures, texgen, matrices, lights, fog,
  blend, cull, depth)

## Progress log

- **Stage B done** (551ea22): the three object-renderer draw sites call
  port_model_draw instead of GXCallDisplayList (patches/main/
  objprint_dolphin.c.sed).
- **Stage A/B done** (bc8b4de): port_model_draw records one GXREC_OP_MODELDRAW
  op carrying the pending matrix block and state delta (gxrec_delta_take,
  gx_rec_delta.c); the render side applies them once and runs the DL call.
  Azahar replay: the model draws collapsed (111 model-draw ops/frame x 16 us
  carrying the embedded state vs the separate MBLOCK/DELTA ops; recorder ops
  13.5 -> ~12.4 ms), frames 61 ms, zero crashes through the full route.
- **DL trust done** (6fac3b9): non-dynamic entries skip the content hashes
  for 512 frames (renewed by the periodic hashed hit). Azahar: clean route;
  the hash saving shows on hardware (the Azahar uncached path dominates).
- Next: the vertex emission (the emitters assemble GxOutVertex + indices so
  the recorded-DL decode retires), then the material cache for the
  render-side state bake.
