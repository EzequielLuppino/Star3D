# Draw-call plan (Star 3D)

Baseline (Dinosaur Island, Azahar, normal build): 17.1 fps / 58.5 ms, ~558 draws.
- CPU 57 ms vs emulated GPU 2.4 ms, so the cost is CPU time per draw, not pixel work.
- GX cost per frame (PROFILE build):
  - gx.displaylist 41.5 ms (dl.play 20)
  - gx.prepare 14.5 ms (prep.tev 7.0, bindtex 2.7)
  - gx.draw 19 ms: C3Di_UpdateContext ~14 us/draw, draw packet ~8 us/draw
  - immediate mode 6.5 ms
- 30 fps needs 33 ms. Cutting the number of draws alone won't get there. The cost of *each* draw has to drop too.

What the game already does (from the surveys):
- Immediate-mode draws are already merged when state is unchanged (s_batch_*). Particles and weather benefit.
- Display-list calls never merge. Each one flushes and owns its own vertex buffer.
- Grass = 4/8/16 passes × block DL; fur = shells × mesh DLs. Both change texture, matrix and TEV on every pass.
- Culling already in the game:
  - camera-cell PVS
  - per-block AABB against 5 planes
  - per-DL box test inside map blocks
  - per-object distance, under-10-px and sphere/frustum tests
- No per-model-part culling, no far plane, a fixed 4:3 cull aspect, and back-face culling off by default in the port (`S3D_CULL`, gx_render.c; since build 94783369 it can be turned on, MULTICORE_PLAN.md "Back-face culling").

---

## Verdicts on the proposed techniques

| Technique | Possible? | Worth it? | Notes |
|---|---|---|---|
| Dynamic batching (immediate mode) | Already done | Small extras | Sort expgfx by texture/blend; CPU-transform shadow quads if each one loads its own matrix. Ceiling ≈ part of the 6.5 ms immediate-mode cost. |
| Dynamic batching (display lists) | Partly | Yes, for static map | Only DLs with identical state and matrix. Means sharing a vertex buffer between DLs (Phase 4). |
| Texture atlasing | **No** (except a clamp-only subset) | **No** | Map textures use REPEAT/MIRROR wrap and scrolling texture matrices; UVs are s16 at ±32/±128; animated textures swap frames; the PICA can't wrap a sub-rectangle. As instructed: not doing it. |
| Pseudo-instancing: grass | Maybe | Only if Phase 0 says grass is expensive | Needs one base noise texture plus a per-layer alpha threshold (vertex alpha − noise, alpha test) instead of N noise textures. Feasible only if the noise layers are thresholds of one noise; must be verified. Bake N offset copies of the block into one buffer → 1 draw per block instead of N. |
| Pseudo-instancing: fur | Same as grass | Same | Shells are skinned on the CPU already; build shell copies along the normal with a layer value per vertex. Cost: N× vertex upload. Wind and KColor shading per shell become per-vertex. |
| Pseudo-instancing: sky stars | **Yes** | Yes, on night/space maps | 92 point DLs whose only difference is the TEV colour → bake the colour into the vertices, 1–2 draws. |
| Pseudo-instancing: repeated static props | Possible | Low | Matrix palette in uniforms (96 vec4 limit), but per-object lighting/opacity varies. Skip unless Phase 0 shows many identical props. |
| Frustum culling | Already done | Small additions | Per-model-part culling, check the 4:3 vs 5:3 aspect, sky. |
| Occlusion culling | **No HW queries on PICA** | No | The game's PVS already is the occlusion culling; software occlusion would cost CPU, which is the bottleneck. |
| Back-face culling | Yes (re-enable) | Yes on real HW | Saves fill on real 3DS; Azahar won't show it. Must respect GX cull mode and mirrored matrices. |

---

## Phases

### Phase 0: Measure where the draws come from (½ day)
- PROFILE-only counters for draws and time per source: map blocks, grass passes, objects, fur shells, shadows, expgfx, sky, HUD.
- Tag the source via a global set around the PERF_WRAP sites.
- Output: an island + hub table. Decides whether the grass/fur/star work below is worth doing.
- Verify: counts sum to the total draws.

### Phase 1: Cheap wins (1 day, low risk), expect 1–3 ms
1. Sort expgfx particles by (texture, blend) inside each pool before drawGlow.
2. Shadows: if each shadow loads its own pos matrix, transform its vertices on the CPU so consecutive shadows merge.
3. Sky stars: bake the per-list colour into vertex colour and draw all lists as one buffer.
4. Re-enable back-face culling. **Done as a setting** (`S3D_CULL=1`, default 0, build 94783369; MULTICORE_PLAN.md "Back-face culling"): winding checked in Azahar (title, attract, hub), hardware A/B pending.
   - Screenshots: hub, island, fur, water, HUD.
   - Real-HW gain only.
5. Cull fixes: use 5:3 for the cull frustum if it is narrower than the screen (fixes pop-in at the edges); far plane at the fog end where fog is opaque.

### Phase 2: Cut the fixed cost of every draw (2–3 days, medium risk), expect 6–10 ms
Biggest lever that doesn't depend on content.
1. **Thin PICA state emitter**: write only the registers that changed since the last draw, using our own shadow copy, and skip C3Di_UpdateContext's full dirty walk (~14 us/draw → target ~4).
   - Keep citro3d for frame/buffer management.
   - Mark its state dirty at the boundaries (gx_restore_state, citro2d UI, movie).
2. Stop redundant dirtying:
   - tex 344 dirtying vs far fewer real texture changes
   - effect 229
   - scissor 164
   Compare against the last values before touching citro3d.
3. Uniform upload: send only the changed vec4 ranges (citro3d currently scans the whole uniform dirty set).
- Verify: gx.draw zone per draw, island fps, screenshots identical.

### Phase 3: Pre-built GPU command lists per cached DL (4–6 days, high risk), expect 8–15 ms
The 3DS counterpart of a real GX display list.
- On first play, record the PICA commands for each DL segment into a linear buffer:
  - attribute-buffer offsets
  - index buffer
  - draw packet
  - static state
- Afterwards, play it with a GPUREG_CMDBUF_ADDR/SIZE + JUMP pair, and patch the return jump.
- The cost per draw drops to roughly the prepare-state diff plus one jump.
- Needs Phase 2's shadow state (the emitter must know what the list changed).
- Skinned DLs: the list stays valid because only the vertex buffer contents change; incremental skinning is unchanged.
- Risks:
  - citro3d's command buffer splitting
  - Azahar vs real HW behaviour of CMDBUF jumps
  - invalidation when arrays change (reuse arrays_unchanged)
- Verify: dl.play + gx.draw zones, long play sessions (memory), stereo.

### Phase 4: Merge static map geometry (3–4 days, medium risk), expect 3–6 ms
- At first load of a map block, group its DLs by (shader/material, texture, TEV key).
- Rebase their indices into one vertex buffer per block → one draw per group instead of per DL.
- The per-DL box culling inside blocks is lost for merged groups. Measure whether it nets positive; keep culling per block.
- Grass passes reuse the merged block buffer.
- Verify: renderMapBlock zone, draws count, screenshots (scrolling textures, animated textures excluded from merging).

### Phase 5: Content-specific (only if Phase 0 justifies it)
- **Grass pseudo-instancing:** first check whether noiseTextures are thresholds of one noise. If yes:
  - bake layers into one buffer per block
  - alpha from (vertex alpha − noise)
  - result: 1 draw per block instead of 4–16
- **Fur pseudo-instancing:** same idea on CPU-skinned shells; 1 draw per mesh instead of shells × mesh. Keep the port_fur_step budget as the fallback.
- **Per-model-part culling:** compute bounds per DL per joint at load and test against the frustum before GXCallDisplayList in modelDoRenderInstrs. Helps big skinned models that are only partly on screen.

---

## Order and expected result
- Phase 0 → 1 → 2 → 3 → 4, with Phase 5 picked from Phase 0's data.
- Phases 2+3 are the core: ~15–25 ms of the ~45 ms GX cost.
- Hitting 30 fps on island also needs game-side CPU savings (objRender, skinning, movie prefetch on the worker core). This plan alone likely gives ~22–26 fps there; the hub should get closer to 30.

---

## Render-thread per-draw cost (2026-10-17, build BE7BDA48, HW result at the end)

The user picked "cut the render thread's per-draw cost" after the plant A/B
(MULTICORE_PLAN.md "Result (D9E45810...)"): the hub is bound by three stages
at once (game ~35-40 ms, render thread ~45, GPU ~38), and the render thread
pays per draw, not per triangle.

**Where the render thread's time goes** (D9E45810 HW, plants normal,
tmp_scripts/rtcat.py): state setup (`gx.prep.*`, `gx.tev.*`, `gx.bind.*`)
8.8 ms, citro3d update/submit/uniforms 7.8, display-list lookup/play 7.5, GX
setter replay 7.5, gather 3.9, immediate mode 3.8, frame end 1.7, idle 4.8.
About 790 draws per frame, ~45 us each, spread over ~40 small steps.

**Phase 3 as written above doesn't fit:** each display-list call has one
segment (`dl.replay_segments` = `dl.hits` = 729), so a pre-built command list
per list replaces only one draw packet. The cost sits in the state work
around every draw.

**State census** (`S3D_STATE_CENSUS=1`, gx_render.c `state_census`, Azahar
hub; the counts match hardware). At each state setup (690/frame) it hashes
the material state: TEV stages/swaps/indirect setup, texgen and channel
setup, pixel state, and only the textures the stages sample.
- With register/konst colors in the key, 124 new keys appear every frame.
  Those colors are animated per object, so they are per-draw values (like
  matrices), not material.
- Without them: **96% of setups use a material seen before** (this frame or
  the last), ~29 new per frame. 279 setups per frame have the same material
  as the previous setup.
- Of those 279: 169 only changed matrices/projection (already cheap), 103
  had TEV dirty, **100 of them only because a register/konst color changed**.
  Every one of those re-ran the unit map, texture binds, texture matrices,
  TEV lookup, fog and light environment.
- Different material (413): texture dirty 394, TEV 195, texmtx 153, pixel
  158, light 94. About 200 are texture-only changes.
- Counters `sc.<key>_same/new/prevframe/unique` (keys mat, col = + material
  colors, all = + projection/viewport/scissor, lit = + light values) and
  `sc.same_d_*` / `sc.diff_d_*` (dirty bits).

**Steps in this build** (all switched by `S3D_RT_STEPS`, A/B
`S3D_RT_STEPS_AB` = 15 all / 0 none / 7 state setup only / 8 uniform check
only; -1 = the individual settings below):
- **A. Uniform value check** (`S3D_C3D_FAST` bit 4, default now 7;
  third_party/citro3d/source/uniforms.c). The upload keeps a shadow of what
  the GPU registers hold and drops dirty vec4s and bool words equal to it.
  Shader constants and the dirty-all after a GPU restore invalidate the
  shadow, which is kept even with the bit off. Azahar hub: **10,900 → 4,400
  vec4 uploads per frame** (59% were unchanged; ~26k command words less per
  frame), bool writes -466/frame. Counters `c3d.unif_vec4`,
  `c3d.unif_vec4_skipped`, `c3d.unif_bool_skipped`.
- **B1. Konst-only changes** (`S3D_PREP_FAST` bit 1). New dirty bit
  `GX_DIRTY_KCOL`, set by GXSetTevColor/S10/KColor instead of TEV. When it
  comes alone, `gx_tev_repatch` refills the constant colors of the program
  applied last (its cache entry, checked by hash: gx_tev_multipass can
  replace entries) and rewrites only the changed stages. Not taken with a
  second pass or water split, a tinted program or movie stages; then it
  falls back to the full TEV path as before. Azahar: `tev.repatch`
  100/frame, TEV lookups 303 → 202.
- **B2. Texture bind memo** (bit 2). A unit that gets the same texture object
  again in the same frame keeps its binding: no `gx_tex_get`, copy/movie
  lookups or sampler setup. Valid only while the frame,
  `g_gx_copy_epoch` (bumped when a copy destination moves, gx_copy.c),
  the bound C3D_Tex and its sampler words are unchanged. Azahar:
  `gx.bind_kept` 108/frame.
- **B3. Fog/light environment kept** (bit 4). A texture-only change where
  `gx_tev_apply` kept the program (`gx_tev_apply_kept`) skips `apply_fog`
  and `apply_light_env`. The combiners, including the fog stage after them,
  weren't rewritten. Multipass and water split setup still run. Azahar:
  `prep.env_kept` 140/frame.
- Azahar screenshots identical in every phase (hub; title movie and attract
  with all steps on). Azahar frame time doesn't move (no GPU command cost
  model, CPU-side emulation), so the hardware A/B decides.
- Expected on HW: render thread -2..4 ms. The frame gains less if the game
  thread or the GPU becomes the wall.

**Found on the way: the movie module stays "active" after the boot movie.**
`movie_active()` is true for the rest of the run, and the slots keep the
game's freed Y/U/V buffer addresses. A texture later allocated at one of
those addresses would be drawn as a movie frame (Y) or white (U/V); no case
has been seen. Two fixes failed in Azahar: expiring slots by time since the
last decode broke the title screen, which keeps drawing a paused frame; a
marker written into the game's planes got wiped. The behavior is unchanged
for now.

**HW test:** env.txt `S3D_PROBE=2`, `S3D_RT_STEPS_AB=300`, `S3D_BENCH=0`,
usual hub spot, stand still ~4.5 min (4 phases x 300 frames, 3+ cycles; skip
the first window). Compare frame / GPU / own / render-thread idle and
`rtcat.py` categories per phase.

**Next candidates** (by the same breakdown):
- C. Display-list call bookkeeping: lookup 2.4 ms (vertex state hash + probe
  of a 2048 x ~300 B table), play 1.8, validate 0.5.
- A full memo of the translated material state (96% hit rate measured; needs
  a snapshot/restore of every state setup output and a verify mode).
- Then plant batching (user's order).

### Result (BE7BDA48 HW 2026-10-17): frame -0.8 ms, smaller than hoped

Log perf/hwlogs/BE7BDA48_stepsab.log (342 s, 5 cycles; first window
skipped). The user saw nothing odd. Per cycle, all steps on vs off: frame
-0.6..-1.0 ms (mean -0.8; same drift +2 ms over the run in every phase).

| S3D_RT_STEPS | frame ms (mean of 4-5) | GPU p3d ms | own ms | uniform vec4/frame | prepare (RT) ms | c3d.uniforms (RT) ms |
|---|---|---|---|---|---|---|
| 0 none | 49.66 | 37.0 | 35.1 | 11,750 | 8.99 | 2.98 |
| 15 all | 49.12 | 36.4 | 34.7 | 5,116 | 8.41 | 3.17 |
| 7 state setup | 49.28 | 37.2 | 35.3 | 11,400 | 8.38 | 2.94 |
| 8 uniform check | 49.65 | 36.2 | 34.8 | 5,116 | 8.92 | 3.27 |

- **State setup steps (7): render-thread setup -0.6 ms, frame -0.4.** Fewer
  hits than in Azahar's view: `tev.repatch` ~30/frame (Azahar 100),
  `gx.bind_kept` 93, `prep.env_kept` 154. Fog -0.2, copyfind/texget -0.2,
  TEV apply -0.15, unit map -0.1.
- **Uniform check (8): GPU -0.7..-0.9 ms** (56% fewer vec4 uploads), but
  `c3d.uniforms` on the render thread goes **up** 0.2-0.3 ms: comparing
  against the shadow costs more than copying. Frame about even. Kept
  (default 7), since the GPU is close to being the wall too.
- The render thread still has ~4.8 ms idle and the game waits ~4-5 ms on
  ring space (`rt.space_wait`) in all phases: the stages stay balanced, so
  single-stage savings show up only partly.
- A sampler phase "(table full)" took 0.78 ms: the probe's phase table is
  full, so some phases are lumped together. Raise PHASES next time.
- Defaults stay on (S3D_PREP_FAST 7, S3D_C3D_FAST 7).

---

## Plant batching (S3D_DL_BATCH, build E9567DFF, 2026-10-17, HW pending)

The user's "item 3" after the render-thread step A/B: draw plants of one kind
together. It applies to **every map**: the code keys on what the game draws,
not on the hub or on plant DLL ids.

**What the game already does** (code survey, foxhollow, read only):
- Models whose file has flag 0x8000 take `modelDoAltRenderInstrs`
  (objprint_dolphin.c:1782), the game's own "runtime batchable" path: one
  shader, one vertex descriptor, one matrix load, one display list; no
  lighting, colour fade or per-object lights. `getVisibleObjects`
  (lightmap.c:383) sorts opaque batchable objects first, grouped by
  `romDefNo` (object kind), then by depth. Each object after the first of a
  kind skips the joint matrix update (`gObjCachedModel`) and only loads its
  matrices (`modelLoadMtxsToGx`), plus a konst colour if the sun colour or
  alpha changed, then calls the same display list.
- Rigid models without vertex animation share the file's vertex array
  (model.c:862), so all instances of a kind already hit **one** cached
  display-list entry.
- Objects in the 100-unit fade band (`renderAlpha < 0xff`) go to the
  transparent queue (back to front, mixed with everything else) and can't
  be grouped.
- Census (`port_model_path` in port/lod_3ds.c, patched into
  `objRenderModel`; counters `plant.alt_path`, `plant.full_path`,
  `obj.alt_path`, `obj.full_path`), Azahar hub: **62 plants per frame take
  the batchable path, 30 the full path** (`modelDoRenderInstrs`, which
  re-emits all state per object), plus 15-16 other batchable objects.

**What the port does now** (gx/gx_dlcache.c "Display-list batching",
gx/gx_rec.c, gx/gx_state.c):
- During a replay (`g_gxrec_replaying`), a cache hit on an eligible entry is
  held instead of drawn (`dlb_try`). Eligible means static vertices (no
  gather), one segment, no list commands, and every vertex on a position
  matrix slot 0-9. The matrices of the slots the entry uses are copied per
  instance.
- The next call of the same entry adds an instance (up to 64). Position
  matrix loads keep the instances held.
- Anything else draws the held instances first:
  - any real state change (`gx_state_changed` with more than
    `GX_DIRTY_POSMTX`). Every GX setter compares before marking dirty: the
    same rule the immediate-mode batch already relies on;
  - any replayed command that isn't a setter (`GXSet*`, `GXLoad*`,
    `GXClearVtxDesc`, `port_tev_indirect`, display-list calls), e.g. GXBegin,
    copies, the frame boundary, HUD/offscreen passes;
  - a different display list;
  - the end of each `replay_until`, so nothing outside a replay sees held
    instances.
- Drawing (`gx_dlbatch_flush`): the entry gets `bt_k = 10 / slots` copies of
  its vertices (built once, in linear memory, counted in the cache budget;
  `dlb.built_kb`). Copy c reads palette slots `c * slots ..`. One draw covers
  up to `bt_k` instances: their matrices go into those palette rows, then the
  rows are put back to what the stream loaded last. A single instance is
  drawn the normal way (only rows that differ are rewritten).
- An entry whose holds end with one instance 8 times in a row is held only
  every 16th frame (`dlb.not_held`). In the hub that cut the useless holds
  from ~350 to ~25 per frame.
- While instances are held, `GXCallDisplayList` skips the state upload when
  only matrices changed (the flush uploads them).
- Setting `S3D_DL_BATCH` (default 1, 0 = off, live). A/B
  `S3D_DL_BATCH_AB` (tag `dlbab`, 1 / 0).
- Counters `dlb.batches`, `dlb.instances`, `dlb.draws` (draws for 2+
  instances), `dlb.single`, `dlb.not_held`, `dlb.built_kb`; zone
  `gx.dl.batch`. Every 600 frames the log line `dlbatch: batch ends per
  frame` lists the commands that ended holds. `(caught up)` means the render
  thread had replayed everything the game thread had published.

**Azahar hub (usual save):**

| | draws/frame |
|---|---|
| S3D_DL_BATCH=0 | ~706-712 |
| S3D_DL_BATCH=1 | ~651-659 |

- 81-82 instances per frame in 12 batches, drawn with 28-30 draws: **-53
  draws per frame (-7.5%)**. Screenshots with batching on, off and on show
  the same plants in the same places (the light changes with the day cycle).
- Holds end mostly at `GXLoadTexObj` (the next object has another texture).
  "(caught up)" never shows in Azahar. On HW the render thread is the wall,
  so it should stay rare.
- Expected on HW: render thread about -1.5..2.5 ms (53 draws x ~30-40 us,
  minus the matrix copies); GPU slightly less (fewer draw packets, same
  triangles). The frame moves only if the render thread stays the wall. The
  D9E45810 plant A/B caps anything plant-related at ~-5 ms.

**Not batched yet:** the 30 plants per frame on the full path, and other
full-path objects. Their command streams repeat the whole state setup per
object, so a setter-level hold can't see that they're identical. That needs
game-side batching: capture each such object's recorded commands into a
side buffer, group objects whose streams match except for the matrix
payloads, and emit one stream with batched display-list calls (the recorder
already has every piece: `gxrec_begin` / `gxrec_end`). Worth it only if this
A/B shows a real per-draw gain.

**Found on the way: the profiler's entry table was full.** `PROF_MAX_ENTRIES`
was 256 and the hub registers more, so later counters were silently dropped
(one `too many profiler entries` warning). Seen here as `sc.diff_d_light`
and the new `mc.*` counters missing. Now 512. Earlier HW runs may have lost
late-registered counters too (the sampler's phase table has the same kind
of limit; see "Result (BE7BDA48...)").

**HW test (build E9567DFF):** env.txt `S3D_PROBE=2`, `S3D_DL_BATCH_AB=300`,
`S3D_BENCH=0`, the usual hub spot, stand still ~3.5 min (2 phases x 300
frames, 4+ cycles; skip the first window). Compare frame / GPU / own / RT
idle per phase (tmp_scripts/abwin.py `<log> S3D_DL_BATCH gx.draw_calls
dlb.draws dlb.instances`). Look for anything odd on plants (wrong position,
flicker, missing clumps), also on other maps if you pass through one.

### Result (E9567DFF HW 2026-10-17): batching works, the frame doesn't move

Log perf/hwlogs/E9567DFF_dlbab.log (267 s, 13 windows; first skipped).

| S3D_DL_BATCH | frame ms (mean) | GPU ms | own ms | draws/frame | RT c3d+draw ms | RT prepare ms | RT idle ms |
|---|---|---|---|---|---|---|---|
| 0 off | 55.35 | 38.65 | 38.43 | 884 | 9.41 | 9.92 | 4.61 |
| 1 on | 55.50 | 38.55 | 38.61 | 821 | 9.13 | 9.97 | 4.61 |

- Batching runs on HW: 92 instances in ~31 draws per frame, **-63 draws**,
  same triangles. Holds that end alone: 33/frame (`dlb.not_held` 505).
- **Frame: no change.** Each "on" window against the mean of its two "off"
  neighbours: +0.12, -0.23, -0.02, 0.00, +0.03, +0.21 ms. The run drifts
  +1.5 ms from start to end in both phases.
- Render thread: `c3d+draw` -0.28 ms, everything else equal. **63 fewer
  draws save only ~4.4 us each.** The instances that got merged were already
  the cheapest draws (only matrices changed: no state setup), so what's left
  of their cost was the draw packet. GPU -0.1 ms.
- **Lesson: the render thread's per-draw cost is the state work between
  draws** (prepare ~10 ms, setter replay ~10 ms, display-list lookup ~8 ms),
  not the draw packet. Cutting draws only pays where it also cuts state
  changes.
- The setting stays on (default 1): neutral on the frame, a little less GPU
  work. `S3D_DL_BATCH=0` switches it off if something looks wrong.

## Map blocks: merge by material? (2026-10-17, analysis only)

**Updated by the E9567DFF result:** the "~30-40 us per draw" estimate below
was wrong for draws that don't change state. Merging map pieces by material
is still worth looking at only because it would cut **material switches**
(~136 per frame in Azahar), and each switch costs state setup on the render
thread (prepare ~10 ms over ~690 setups ≈ 14 us, plus setter replay). For
that the game thread has to emit the pieces sorted by material (a patch in
`renderSceneGeometry` / `renderMapBlock`), so the setters between same-material
pieces become no-ops and the render thread skips their state setup. Merging
the vertex buffers on top would only save the small draw-packet cost.

The user asked to consider merging map-block draws "the same way" next.

**Census** (`S3D_STATE_CENSUS=1` with `S3D_PROBE=2`, Azahar hub). The
recorder tags `renderMapBlock` calls with bit 31 of the size (the scope is
only set with S3D_PROBE on); counters `mc.*`, gx_dlcache.c `map_census`:

| per frame | |
|---|---|
| map-block display-list calls (`mc.calls`) | 205 |
| distinct materials among them (`mc.materials`) | 69 |
| calls with the same material as the call before (`mc.same_as_prev`) | ~1 |
| triangles (`mc.triangles`) | ~5,600 |

(The HW views in earlier runs had more: ~274-318 map calls.)

- **"The same way" doesn't apply.** Plant batching works because instances
  share one display list and only matrices differ. Each map piece is its own
  geometry, and `renderMapBlock` draws one piece per material per block, so
  consecutive calls almost never share a material (~1 per frame). Holding
  calls finds nothing to merge unless the draws are reordered.
- **Merging by material would** take 205 calls to ~69 draws: about -136
  draws per frame, ~2.5x the plant batching cut (~4-5 ms of render thread at
  HW's ~30-40 us per draw). The game thread wouldn't change (it still
  records every call), and neither would the GPU's triangles.
- **What it takes** (Phase 4 above, MULTICORE_PLAN.md option B):
  1. Reorder: draw opaque map pieces grouped by material instead of block by
     block. Safe for opaque, depth-tested pieces. Water, transparent layers,
     grass passes and anything blended stay in order.
  2. Merge: pieces of one material from different blocks need one vertex
     buffer. Per-block matrices can use the palette trick from plant
     batching (one slot per block, 10 blocks per draw). The concatenated
     buffers have to be built per (material, set of visible pieces) and
     cached, since culling changes the set from frame to frame. Or merge at
     map load, per material per group of blocks, and give up the per-piece
     box culling inside a group.
  3. Scrolling textures (texture matrices per piece) and animated textures
     have to stay out, or carry their texture matrix per piece.
- **Cost:** several days, medium-high risk (draw order, culling, memory
  churn). Decide after the S3D_DL_BATCH A/B. If 53 fewer draws give
  ~1.5-2 ms on HW, 136 should give ~4-5 ms. If they give nothing, the draw
  count isn't the render thread's lever, and map merging should wait.

## Map sort (S3D_MAP_SORT, build C8DC5F71, 2026-10-17, HW: net loss, off)

The user's pick after the fur work: group the map's draws by material so the
render thread sets up state less often. This only reorders the draws; nothing
is merged.

### Why reordering alone can pay

The port's GX setters already skip unchanged values (`GXSetNumTevStages`,
`GXSetTevOrder`, `GXLoadTexObj` compare first, gx_tev.c / gx_tex.c). So when
two map pieces with the same material are drawn back to back, the second
piece's full material setup changes nothing and the render thread doesn't
run its state prepare again. The prepare is ~14 us per setup on HW (see the
E9567DFF lesson above), and the HW hub had ~274-318 map calls.

### How the main map pass draws (code reading)

- `renderSceneGeometry(0, front to back)` walks 5 layers x 16x16 cells and
  calls `renderMapBlock(block, 0)` for each visible block. Passes 1 and 2
  (transparent and water) only queue entries for `sceneDrawTransparentPolys`
  and are not touched.
- `renderMapBlock` runs the block's main instruction stream:
  `setupToRenderMapBlock` once (PNMTX0 pos/nrm, TEXMTX2, 4 vertex arrays),
  then ops: 1 = `mapBlockRender_setShader` (fog, Rcp TEV stages, blend/z/alpha
  compare, channel, cull), 3 = vertex format, 2 = `mapBlockRender_callList`
  (piece frustum test, point-light stages for lights near the piece, the
  display list, shadow-row queue entries), 4 = skip, 5 = end.
- Shaders live per block (`block->shaders[]`), so "same material" has to be
  judged by content: flags, layerCount and the layer records (texture,
  type, material id for animated texture overrides, scroll matrix).
- Inherited state: a shader with flag 0x80000000 (or 0x20000 when the attract
  movie callback says so) returns from setShader before its TEV setup, so its
  pieces draw with the previous material. `callList` adds point-light stages
  on top of the current material (they pile up if two pieces follow one
  shader op).
- Queue entries (`lightmapQueueShadowRow`) are sorted by depth key later,
  so the order they are added in only matters on exact ties.

### What was built

`patches/main/tex_dolphin_mapsort.c.inc` (appended to tex_dolphin.c by
`tex_dolphin.c.sed` with sed's `r`; the Makefile has a dependency line for
it), hooks in `lightmap.c.sed` and `tex_dolphin.c.sed`, settings and
counters in `port/mapsort_3ds.c`, A/B in `port/diag_3ds.c`.

- Pass 0 collects the visible blocks (`mapSortAdd`, with the block's view
  matrix and cell position) instead of drawing them. At the end of the pass,
  `mapSortFlush` draws them.
- Each block's stream is split into units at its shader ops. A unit is
  replayed exactly as `renderMapBlock` would run it. Shader ops that inherit
  state (0x80000000, 0x20000) are glued to the unit before them.
- Order rules: across blocks the order is free (opaque and depth tested).
  Inside a block, "ordered" units keep their place against all others: no
  key (blended 0x40000000/0x20000000, heat shimmer 0x80, inheriting, >2
  layers), alpha-tested (0x400), or with a glued part. Plain opaque units may
  swap among themselves only with bit 4.
- Greedy merge: keyless units are drawn as soon as the rules allow; then the
  material with the most units that may be drawn now is drawn, all of them,
  in block order (front blocks first). Repeat.
- A unit from a different block than the last one gets that block's setup
  again. A unit that draws before its own vertex-format op replays the
  block's previous one (never needed in the hub: `map_sort.vtx_replayed` 0).
- `S3D_MAP_SORT` bits (default 0 = the game's order, read live):
  1 sort, keeping each block's order; 2 skip a unit's setShader when the unit
  just drawn had a shader with the same content and changed nothing on top
  (no point lights, no glued part, no 0x800/0x1000/0x2000); 4 the relaxed
  in-block rule above. Any non-zero value sorts.
- Blocks or units that don't fit the tables (96 blocks, 1024 units) are drawn
  the old way (`map_sort.overflow`). Units the merge couldn't place would
  show in `map_sort.stuck`.

### Azahar results (hub savestate, builds 48FB7E57 / C8DC5F71)

| per frame | game's order | S3D_MAP_SORT=1/3 (block order kept) | 5/7 (relaxed) |
|---|---|---|---|
| blocks / units | 16 / 252 | 16 / 252 | 16 / 252 |
| distinct shader contents | 83 | 83 | 83 |
| material groups drawn | ~252 (no repeats) | 210 | 139 |
| block setups | 16 | 83 | 166 |
| setShader calls skipped (bit 2) | - | 41 | ~110 |
| ordered / keyless units | | | 30 / 1 |

- Keeping every block's own order holds the merge back: 210 groups against a
  best case of 83. The relaxed rule gets 139. What's left comes from the 30
  ordered units (mostly alpha-tested) pinning their blocks.
- The cost: block setups go from 16 to 166 per frame. Each one is 3 matrix
  loads (PNMTX0 pos/nrm, TEXMTX2) and 4 `GXSetArray` calls. Whether
  ~110 fewer material changes beat ~150 more matrix/array changes on the
  render thread is what the HW A/B must show.
- Look: shots of 0, 1, 5 and 7 taken a few frames apart differ only in
  animated things (plants, Fox) and the cannon's cast shadow. Two shots of
  mode 0 differ the same way (control run `tmp_scripts/mapsort4.txt`, diff
  images `build/emu/diff_c_*.png`), so that is time passing, not the sort.
  No overflow, no stuck units, no errors.
- Azahar's render-thread replay was ~57.5 ms with 7 and ~58.5 ms with 0, but
  its timing says little about HW.

### Hardware test (build C8DC5F71, .3dsx only)

1. env.txt: `S3D_MAP_SORT_AB=300`, `S3D_BENCH=0`, your usual settings.
2. In the hub, stand at the usual spot and don't touch anything for ~3 min.
   The game cycles `S3D_MAP_SORT` 7 / 0 / 5 by itself (tag `mapsortab`),
   starting 150 frames after the hub loads.
3. Also look at the ground, paths and rock faces for missing or flickering
   pieces in the 7 and 5 phases.

Read: frame, gpu and sceneRender from the `mapsortab` lines, and `rt.replay`
per window (tmp_scripts/phase_cmp.py). 7 vs 5 is the price of bit 2 (skipping
same-content setShader saves game-thread recording and render-thread setter
replay). If neither beats 0, the extra block setups cost as much as the
material changes save. The next try would then be a block-aware merge that
prefers staying in the current block when a material ties.

### Hardware A/B result (C8DC5F71, 2026-10-17, hub)

Log `perf/hwlogs/C8DC5F71_mapsortab_hub.log`. Env `S3D_MAP_SORT_AB=300`,
`S3D_BENCH=0`, the user's settings, standing still. Four rounds of 7 / 0 / 5.
The user saw nothing wrong with the map.

| S3D_MAP_SORT | frame | GPU | sceneRender (game thread) | game.scene_own | rt.replay |
|---|---|---|---|---|---|
| 0 (game's order) | 51.6-53.3 (avg 52.3) | 38.5-39.2 | 39.1-41.1 | 37.3 | 48.2 |
| 7 (all bits) | 53.2-54.0 (avg 53.4) | 37.9-39.3 | 42.4-43.1 | 42.9 | 46.8 |
| 5 (no setShader skip) | 54.8-55.3 (avg 54.6) | 38.5-39.0 | 44.1-44.4 | 43.4 | 48.2 |

On HW the hub had ~19 blocks and ~314 units per frame, against 152 groups
and 228 block setups with 7.

- **Frame: 7 is +1.1 ms and 5 is +2.3 ms against 0.** The sort is a net loss
  as built.
- **Game thread: +5.5 to +6 ms** (`game.scene_own_us` 37.3 → 42.9 / 43.4).
  That is the merge's own CPU work (per round it scans every unit for
  readiness and counts keys, ~150 rounds x ~314 units, plus the stream parse
  each frame), plus recording ~200 extra block setups. With the game thread
  at ~43 + logic, it becomes the wall, so the frame rises.
- **Render thread: grouping alone saves nothing** (5: 48.2 = 0). The fewer
  material changes are paid back by the extra block setups (matrix and
  array changes). **All the render-thread gain (-1.4 ms) comes from bit 2**:
  ~160 skipped same-content setShader calls per frame, whose setter replay
  the render thread no longer does (~9 us each).
- GPU unchanged (~38.8 ms).

**Conclusions**
- The "material switch = prepare" model was wrong for the map: by itself
  the order changes nothing on the render thread. What costs is replaying
  setter commands, even when they don't change state.
- Best case if the sort cost no CPU: the render thread -1.4 ms, so the
  frame would gain at most ~1 ms (GPU ~39 ms is close behind). Getting
  there needs the merge order cached (the visible block set rarely changes
  when standing still; units parsed once per block at load), and the extra
  block-setup recording stays.
- The default stays 0 (off). The code stays for a cached-order retry.

## Map material dedup (S3D_MAP_DEDUP, 2026-10-17, HW: frame -0.45 ms with low polygons, default 3 since A5D1FBE0)

The user's pick after the map sort result: keep the game's own draw order and
only drop the material setup work whose state is already in place. The map
sort A/B showed that all of its render-thread gain (-1.4 ms) came from bit 2
(not replaying a setShader whose values were already set), and that the
sort itself cost the game thread ~6 ms. This keeps the useful part without
the sort.

### Why not just "skip when the previous piece had the same material"

In the game's order consecutive map pieces almost never share a material
(Azahar hub: 252 units, ~1 repeat), because every block draws its pieces
material by material. So the whole-setup skip alone would hardly fire. But
two different materials still share most of their fixed-function state:
almost every opaque map piece sets the same fog, blend / z / alpha compare,
channel and cull values. `mapBlockRender_setShader` records all of them on
every shader op (~8 setter commands), and the render thread replays them
even though nothing changes.

### What was built

`patches/main/tex_dolphin_mapdedup.c.inc` (appended to tex_dolphin.c by
`tex_dolphin.c.sed` with sed's `r`, after the map sort code; Makefile
dependency line), hooks in `tex_dolphin.c.sed` and `lightmap.c.sed`, the
setting and counters in `port/mapsort_3ds.c`, the A/B in `port/diag_3ds.c`.

- `S3D_MAP_DEDUP` bits (default 3, read live):
  - 1: in `setShader`, each group (fog; blend + z mode + z comp loc + alpha
    compare; channel ambient + ctrl; cull) is only recorded when its value
    differs from what the last map setShader of the pass set. The channel
    group compares the actual ambient colour (shader ambient, or the sun
    colour).
  - 2: the TEV / texture part (`Rcp_ResetTextureStageState`, the layer
    stages, cloud / warp / heavy fog stages) is skipped when the shader has
    the same content as the last one set up (`msSameShader`: flags, layer
    count, layer records) and no piece added point-light stages since
    (`sMdLit`, set next to the map sort's `sMsLit` in `callList`). Never for
    heat shimmer (0x80), inheriting (0x80000000), attract movie (0x20000) or
    shaders whose pieces add stages by flag (0x800, 0x1000, 0x2000).
- Armed only inside `renderSceneGeometry(0)` (`mapDedupBegin` /
  `mapDedupEnd`); every value starts as "unknown" at the pass start, so
  state set by other code before the pass is never assumed. Pass 1 / 2
  (transparent, water) don't set up materials and are not touched. Other
  `setShader` callers (lightmap_draw.c) run unchanged.
- Why it is safe inside the pass (code reading): between two map shader ops
  only `callList` (TEV point-light / shadow stages, `GXSetNumTevStages`),
  vertex formats and `setupToRenderMapBlock` (PNMTX0, TEXMTX2, arrays) run.
  None of the TEV helpers sets fog, blend, z, alpha compare, channel or cull
  (checked every helper `setShader` and `callList` call). TEXMTX2 is loaded
  per block, not per material (`addSmallReflectionTevStage` only references
  it), and the Rcp texture matrices are post-transform slots (64+) that the
  block setup doesn't touch. The 0x2000 blend branch in `callList` only runs
  with `visArg != 0`, which the main pass never passes. The attract movie
  callback can draw a THP frame, so it resets all remembered values.
- Works with `S3D_MAP_SORT` too (the sort calls the same `setShader`).
- Counters per frame: `map_dd.calls` (TEV setups reached), `map_dd.tev`
  (skipped), `map_dd.fog` / `map_dd.pe` / `map_dd.chan` / `map_dd.cull`
  (setter groups skipped).

### Azahar result (build 36BAF2FE, hub savestate, full polygons)

Script `tmp_scripts/mapdd.txt` (modes 0 / 3 / 1 / 0, shots `build/emu/dd_*.png`).

| per frame | S3D_MAP_DEDUP=0 | 3 (default) |
|---|---|---|
| map setShader calls reaching the TEV setup | 252 | 252 |
| fog skipped | - | 251 |
| blend / z / alpha compare skipped | - | 221 |
| channel skipped | - | 213 |
| cull skipped | - | 229 |
| TEV setups skipped (bit 2) | - | 0 |
| rt.cmds (all commands replayed) | ~20,100 | ~18,700 |

- About 1,400 fewer recorded / replayed commands per frame (~7% of all).
- Bit 2 never fires in the game's order (no consecutive same-content map
  shaders in this view), as expected; it is cheap and stays on for scenes
  where it may. Modes 1 and 3 behave the same here.
- Look: mode 0 vs 3 shots differ only in animated things (plants, Fox, the
  cannon's moving shadow edge), the same as two shots of mode 0 taken 8
  frames apart. No errors in the log.
- Azahar's rt.replay (58.5 vs 59.2 ms) says little about HW.

### Hardware test (build 36BAF2FE, .3dsx only)

Note: this build also has full polygon models again (the new default), so
its frame times can't be compared with older runs, only within the A/B.

1. env.txt: `S3D_MAP_DEDUP_AB=300`, `S3D_BENCH=0`, your usual settings.
2. In the hub, stand at the usual spot for ~3 min. The game cycles
   `S3D_MAP_DEDUP` 3 / 0 / 1 by itself (tag `mapddab`).
3. Look at the ground, paths and rock faces for wrong blending, fog or
   lighting in the 3 and 1 phases.

Read frame, gpu, sceneRender and `rt.replay` per phase
(tmp_scripts/phase_cmp.py). Expected from the map sort numbers (~9 us per
fully skipped setShader, of which these groups are maybe a third): the
render thread -0.5..-1 ms and the game thread a little less recording.
If 3 is not faster than 0, set the default back to 0.

### Hardware A/B result (36BAF2FE, 2026-10-17, hub)

Log `perf/hwlogs/36BAF2FE_mapddab_hub.log` (full polygons, user env incl.
`S3D_CULL=1`). Only one round of 3 / 0 / 1 plus a 3 (the run was short).

| S3D_MAP_DEDUP | frame | GPU p3d | rt.replay | rt.cmds | game.scene_own |
|---|---|---|---|---|---|
| 0 | 54.1 | 46.8 | 50.3 | 25,890 | 39.0 |
| 1 | 54.4 | 47.1 | 50.9 | 24,620 | 38.4 |
| 3 | 54.4 | 46.9 | 51.0 | 24,450 | 38.0 |

- It does what it should: ~1,450 fewer commands per frame and ~1 ms less
  game-thread time. `map_dd.tev` stayed 0 on HW too, so only bit 1 acted.
- **No frame gain.** With full polygons the GPU is at ~47 ms and the render
  thread waits on it, so fewer commands don't shorten the frame.
- **The user saw a rock's front face disappear** during this run (which
  phase / where not known yet). Code reading found no path that changes
  fog, blend / z / alpha compare, channel or cull between two map shader
  ops of pass 0: all the port's setters are idempotent, display-list BP
  writes are ignored, the TEV helpers don't touch these groups, the shared
  draw queue is reset just before the pass (its 1000-entry flush can't
  happen there), and the attract-movie callback resets. Not reproduced in
  Azahar with `S3D_CULL=1` at the usual hub view. No draw drops in the log
  (peak vtx 10,976 / 32,768). It may also be unrelated (full polygons are
  new in this build; objects aren't touched by the dedup at all).
- **Default set to 0** (build 9367968D). The code stays. To clear it, look
  at the same rock with `S3D_MAP_DEDUP=0` and `=1`.

### Verify mode and the vanished rock (build 3AF2B4BF, low polygons again)

The user said the face "vanished and came back" while standing still, which
fits the A/B switching. To check the dedup without guessing, bit 4 was added:
`S3D_MAP_DEDUP=5` with `S3D_RENDER_THREAD=0` skips nothing, but each group
bit 1 would have skipped is run and checked against `g_gx_change_seq` right
after its setters (`mapDedupCheck`, hooked after the fog block and before
setShader's final return). A group that changed GX state anyway is logged
(`mapdd` WARN "mismatch: ...", first 40) and counted (`map_dd.mismatch`).

- Azahar, hub savestate view and warped to the user's A/B spot
  (`warp -5390 -789 -3089`, the HW log's `where` line; the warp keeps the
  facing, 73 instead of 110), `S3D_CULL=1`: **0 mismatches**, 252 map
  setShader calls per frame checked. Shots of 0 / 1 at that spot (the camera
  sits close to a rock wall) show the wall in both; the diff is only texture
  shimmer from the camera sway. Scripts `tmp_scripts/mapdd_verify.txt`,
  `tmp_scripts/mapdd_spot.txt`. (The remote `stick` / `cstick` commands did
  not move the player after a savestate load.)
- So far no evidence the dedup changes any GX state. Next: the same check on
  HW (where the RT and stereo paths differ from Azahar), and a longer A/B.

### Hardware verify run (3AF2B4BF, 2026-10-18, hub, user's spot)

Log `perf/hwlogs/3AF2B4BF_mapdd_verify_hub.log`, `S3D_RENDER_THREAD=0`,
`S3D_MAP_DEDUP=5`, standing still ~90 s (11.8 fps, render thread off).
- Checked per frame: fog 337, blend / z / alpha 298, channel 289, cull 307
  groups the dedup would have skipped. **0 mismatches** on HW: none of them
  ever changed GX state. With these setters idempotent, skipping them can't
  change what is drawn.
- **The user saw the floor flicker for a second at ~1:45 in this run.** In
  verify mode nothing is skipped and no setting changes during the run, and
  the log shows nothing at that time (player still, steady frame times). So
  that flicker is a separate, pre-existing issue, and the rock face that
  "vanished and came back" in the 36BAF2FE A/B is most likely the same one
  rather than the dedup.

### Hardware A/B with low polygons (3AF2B4BF, 2026-10-18, hub) → default 3

Log `perf/hwlogs/3AF2B4BF_mapddab_hub.log`, `S3D_MAP_DEDUP_AB=300`, user env
(`S3D_CULL=1`, render thread mode 2), standing still ~3.5 min: four full
rounds of 3 / 0 / 1 plus a fifth 3. **No visual glitches seen.**

| S3D_MAP_DEDUP | frame | rt.replay | rt.cmds | game.scene_own | GPU p3d |
|---|---|---|---|---|---|
| 0 | 49.13 | 45.23 | 25,090 | 35.61 | 37.07 |
| 1 | 48.87 | 45.05 | 23,940 | 35.24 | 37.29 |
| 3 | 48.68 | 44.82 | 23,710 | 35.18 | 36.97 |

- 0 was the slowest phase in every one of the four rounds (by 0.3-1.4 ms
  against the 3 of the same round). Average: **frame -0.45 ms**, render
  thread -0.4 ms, game thread -0.4 ms, ~1,380 fewer commands per frame.
- 3 vs 1 (the TEV skip, which never fires) is within noise.
- With low polygons the render thread (~45 ms) is the wall again, which is
  why this shows up now and didn't with full polygons (GPU wall ~47 ms).
- **Default set to 3** (build A5D1FBE0). With the HW verify run clean, the
  vanishing rock / floor flicker is tracked as OPEN_BUGS.md #25.
