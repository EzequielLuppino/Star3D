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

## Stage C attempt 1 reverted: the game-side state misses the DL-loaded formats

The first emission attempt decoded the immediate streams on the game
thread - it broke with vertex explosions (NaN positions). The cause: the
game's own GX state does not include the vertex formats that DISPLAY
LISTS load (the CP-format commands inside the DLs apply only on the
render side during the replay), so the game thread decoded with stale
formats. Reverted (3668ae2 / b968495).

The redesign path: the DL-record capture (gx_rec.c s_dl) sees the DL's
CP commands while the game records them - executing those format loads
against the game-side state at record time would make the game-side
state complete, and the emission then decodes with the right formats.
That is the next attempt; the seam + collapse + trust stack stays.

## Stage C emission: closed as net-negative (2026-11-01)

Attempt 2 (with the DL-CP mirror on both DL-call paths) still produced
vertex explosions from the effect engine's immediate streams (the game-
side state remains incomplete in ways the mirrors did not cover), and
each mirror adds game-thread walk cost that eats the decode savings.
Verdict: the immediate-stream emission nets ~2 ms at best for this
complexity - not worth it. The record-side channel for the immediate
streams stays as it is (the raw-byte IMM chunks decoded on the render
thread). The channel's real wins remain the seam, the one-op collapse
and the DL trust. Reverted to the verified-good state (c387cd8).

## Stage C part 2: the object material cache (scoped)

The function timings refined the target: the map blocks build no material
(setupToRenderMapBlock = matrices + array binds only; their 6.6 ms is the
instruction-stream interpreter), and the transparent pass routes through
the object renderer. The one material-builder is objSetupRenderOpGxState
(objprint_dolphin.c, 82 GX calls + the stage helpers) - rebuilt per object
per frame inside objRender (16.1 ms) and the transparent pass (11.8 ms).

The cache: per (model render-op, material key) the game runs the setup
once and the port replays the stored state. The key is the setup's own
input set - the render-op pointer (the static model data), the object's
material-relevant fields (sphereMapIntensity, the aux textures), the
light count, the fog state and the player flags - computed by the patched
call site from the same expressions the setup reads. The store snapshots
the delta shadow's regions (the material state; the matrices stay on the
per-frame path), the apply re-feeds them through the shadow so the render
side sees the state without the game rebuilding it. The fallback stays:
any key mismatch runs the full setup, so the conversion is per-object
safe from the first frame.

Order: 1) the call sites in objprint_dolphin.c (the render-op loop),
2) the port module (matcache_3ds.c: cached/store/apply + the shadow
integration), 3) Azahar correctness, 4) the PERF_WRAP measurement.

## Object material cache: blocked by the setup's input web (2026-11-01)

The setup's inputs are not enumerable safely: objSetupRenderOpGxState
consumes the bit stream (the per-object render instructions, advancing
it - a cached path would have to replay the exact advance), the object's
animation state, the game's own state caches (gObjGxKColorCache,
gObjCachedModel, gObjOverrideColor...), the light count, the fog and
the shadow state. A game-side key built from those expressions would
break on every upstream change and miss an input on the first miss.

The realizable version is the fork proper: split the setup into the
material (a pure function of the static model data - the TEV stage
builders addSphereMapTexStage / addEnvMapBumpStages /
addLightTexReg2Stage) and the per-object state (the matrices, the
animation, the lights). The material then builds once per render-op and
the port caches it. That is the next fork slice - a multi-session
rewrite of objSetupRenderOpGxState with the GX fallback intact.
