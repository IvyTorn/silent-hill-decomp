# N64 performance: native characters + furniture

## The corrected diagnosis (2026-09-06)

Earlier I blamed texture-DMA volume. The `[PROF]` numbers say otherwise:

```
frame=233ms  rdp pipe=223ms  tmem=12ms  cmd=43ms  tri=835  win=492  px=101K
```

- `tmem=12ms` — the texture DMA itself is cheap.
- `pipe=223ms` for `tri=835` = **267us PER TRIANGLE**, 50-250x too slow for
  the 101K pixels actually drawn (px is only 1.3x overdraw).
- The missing ~168ms is **per-load pipeline sync stalls**: each of the 492
  per-primitive TMEM window loads forces the RDP to drain its pipe before the
  load, and drain waits for the previous (textured, filtered) triangle to
  finish. 492 x ~0.34ms = ~168ms. That is the whole frame.

So the enemy is the **NUMBER of TMEM window loads** on the per-primitive PSX
path, not their size. The native world already proves the cure: it pre-tiles
each area into TMEM-sized tiles and loads each **once** (83 tiles for the
police station), then draws every primitive that uses it back-to-back — no
per-primitive reload, no per-primitive sync stall.

Everything still on the per-primitive path feeds those 492 loads:
1. **Characters** (Harry always on screen; enemies) — animated.
2. **Shared "PLM" furniture** (the reception counter, etc.) — static; the
   counter fix (g5) re-enabled them onto the per-prim path on purpose.
3. World geometry the converter couldn't handle (small residual).

## The plan, in dependency order

### Phase A — native PLM furniture (static, tractable, do first)

PLM furniture is STATIC world geometry that merely lives in a different model
pool than the per-cell geometry mkworld already converts. `collect_cell()` in
`mkworld.py:282` already *detects* `info.is_global_plm` and skips it — the only
missing input is the shared model geometry.

- **Where the geometry is:** each map's `mapInfo->plmFileIdx` file (an LM-format
  pool, same shape as the per-cell `ipd.lm` mkworld already parses). Referenced
  by `info.name`, exactly like local models.
- **Converter change:** add `--plm-file <PLM>` (or `--plm-dir`); parse it with
  the existing `Lm` reader into a name->model map; in `collect_cell`, replace
  the `is_global_plm: skip` with a lookup in that pool and emit its geometry
  into the cell's SHW just like a local model. Its transform already comes from
  the IPD instance.
- **Runtime change:** none to the draw path — the geometry becomes ordinary
  SHW buffer data. BUT the g5 per-instance PSX fallback in `Ipd_ChunkDraw` must
  stop routing now-native PLMs to the PSX path: once a map's SHW contains its
  PLMs, `continue` (skip PSX) for global-PLM models too. Gate on an SHW version
  bump so un-reconverted maps still fall back.
- **Win:** removes the PLM furniture's window loads AND reclaims the `chunk=42ms`
  the counter fix costs. Reconverts with one mkworld run per map.

### Phase B — native characters (animated, the big one)

Characters are the dominant share of the 835 tris / 492 loads and are ANIMATED,
so they cannot be pre-baked as static geometry. Two sub-problems:

1. **Textures** — pre-tile each character's TIM pages into SHT-style tiles
   loaded once (like the world). This alone kills most of a character's window
   loads. Straightforward, reuses the SHT baker.
2. **Geometry** — the model animates via bone matrices. Options:
   - (b1) Keep the software-GTE per-vertex transform (unchanged animation), but
     GROUP the character's projected primitives by tile so each tile loads once
     and all its prims draw together. Self-occlusion (no Z buffer) must be
     handled: a character is a closed mesh, so tile-grouping reorders its own
     front/back faces. Mitigation: the AABB-style painter split already used for
     the world, applied per-character, or a small per-character depth pre-sort.
   - (b2) Full t3d skinning: feed bone matrices to the RSP, transform on the
     RSP like the world. Biggest win (offloads the 22ms `chara` CPU transform
     too) and biggest effort; needs the ILM skeleton mapped to t3d matrices.

   Start with (b1) texture tiling + grouping (contained, testable), evaluate
   (b2) after.

## Possible quick experiment (independent of A/B)

`[PROF]` shows `cull=0` — backface culling is OFF (`n64CullBackfaces` config).
The PSX drew both faces of everything; ~half of 835 tris are back-faces that
load a window and rasterize only to be overdrawn. Characters are clean-wound,
so `cull_backfaces=1` in `silenthill.cfg` could roughly HALVE the load count for
free. Risk: world geometry winding is not guaranteed, so a wrong guess makes
walls vanish. Worth a single test toggle before the big work — measure the
frame time and eyeball for holes.

## What the user converts manually

The one input I need is the **map PLM file** for each map, extracted from
`Silent Hill (USA).bin`. The existing extractor already pulls the `BG`
directory (`mkdiscpack.py --extract <tmp> --dirs BG`); the PLM pool is in that
same disc filesystem. Concretely for the police station (map2_s04 / tag `ER`):
extract its `plmFileIdx` file alongside the IPDs, then re-run mkworld with
`--plm-file`. I can do the extraction from the BIN myself now that E: is
mounted; the only thing I would need from the user is confirmation of the disc
path and permission to read it. No hand-editing of assets is required — every
step is a converter run.

## Status / REDIRECT (2026-09-06, after extracting the disc BG dir)

Phase A is NEARLY EMPTY for the police station and does not help it:
- Extracted all 11 `*.PLM` pools + every `ER*.IPD` from the BIN. Parsed with
  sh1fmt: ER's cells reference **320 local models (all resolve in the cell LM,
  already native) and exactly ONE** global/missing model, `BOOK1`, which is in
  NO pool file. So there is no furniture to convert here; the world is already
  native. (Other town areas DO have big `_GLB.PLM` pools -- APR/DR/SP/RS/MGR/THR,
  14-41KB each -- so Phase A still matters for THEM, later.)
- Therefore the police-station `tri=835` / `win~492` per-frame load is
  **CHARACTERS** (Harry), not furniture.

Character textures are 256x256 CI4 PSX pages; TMEM holds 64x64 (2KB). A
character's prims sample many windows of its page -> the per-prim window
reload thrash. There is NO "texture fits TMEM" shortcut. The only cure is to
draw TILE-GROUPED (each tile loaded once), which needs occlusion handled
without per-prim depth order == a **Z-BUFFER**.

Z-buffer feasibility CHANGED: 8MB Expansion Pak, image 6.6MB, a 320x240x16 Z
is 150KB -> it FITS now (the old "no Z-buffer" was a PSX-parity choice made
before the char-perf wall, README line 31). A Z-buffer + draw-by-tile is the
"real N64 game" architecture: kills the ~492 loads -> kills the ~168ms of
sync stalls, AND fixes all remaining occlusion.

**DECISION NEEDED (reverses a documented choice): adopt a Z-buffer + tile
-grouped character drawing?** It is the one lever that turns 4fps into a real
framerate. Not starting it unilaterally because the user was previously firm
on "no Z-buffer, painter's order like the PSX." Phase A (furniture) remains
valid for the town areas and can proceed in parallel regardless.

## 2026-09-06: decision = YES (Z-buffer). Stage log + the real numbers.

Approved. Perf truth from an accidental Harry-only void: **Harry alone = 84ms**
(~19ms CPU software-GTE transform + ~17ms CPU submit + ~48ms RDP / 254 window
reloads). World adds ~150ms, SAME window-reload cost. Both TMEM-window-reload
bound. Key: if the 254 reloads went to ~0, Harry is CPU-bound at ~36ms = ~28fps
-- most of the way. So **tile-grouping is THE lever**, RSP transform is the
follow-up to clear 30.

- **Stage 1a DONE**: Z-buffer alloc/attach/clear + world writes depth. Bug found
  and fixed: 150KB via surface_alloc starved the in-game heap -> t3d ready=0 ->
  world void. Now static .bss. zbuffer=1 default. AWAITING confirm it renders.

- **Stage 1b (designed, implement on confirmed 1a) -- character depth source.**
  The problem: the OT walk (DrawOTag, gpu_xbox.c:1374) follows a FLAT P_TAG
  linked list; the per-prim OT bucket (=otz=real GTE depth) is NOT recoverable
  from the chain. Two sources:
  * (A) DRAW-ORDER RANK: a monotonic counter over the walk = the painter's
    order. Cheap, no new plumbing. But it is a RANK, not view depth, so it does
    NOT share a Z-space with the world's real t3d depth -- only valid for
    characters occluding among THEMSELVES.
  * (B) REAL GTE SZ: capture C2_SZ like the item path's g_PsyX_RtpSz, but from
    the CHARACTER mesh draw (func_80057090 path) and tag each prim. Invasive but
    composes with the world.
  PLAN: use (A) first for the Harry-ISOLATED milestone (world off -> no
  composition needed), because it is enough to enable tile-grouping and prove
  the fps win. Move to (B) when re-integrating the world so characters occlude
  against furniture. Z-space conflict handling for the interim full scene: clear
  Z once between the world pass and the character pass so characters get a fresh
  rank-Z on top of the world (keeps current compositing: world behind, chars on
  top, chars self-occlude).

- **Stage 2 (the fps win) -- tile-group the character draw.** In the batch/OT
  path: instead of emitting prims in OT order, bucket them by (page,pal) tile,
  load each tile ONCE, draw all its prims, next tile. Z (1b) preserves the
  original ordering so reordering is safe. Target: 254 reloads -> ~tile count.
  Measure against the 84ms baseline.

- **Stage 3 -- RSP transform** (t3d skinning of the character) to cut the ~19ms
  CPU. Only if still short of 30 after Stage 2.

Implementation is gated on Stage 1a rendering confirmed (don't stack unverified
Z changes). Everything else (numbers, sources, order) is settled above.

## 2026-09-06 (later): Z-BUFFER SHELVED, going native-RSP characters instead.

The Z-buffer does NOT fit: the malloc heap is only ~1150KB (g_PsxRam is a
2.125MB static array + the 6.6MB image leave that little of 8MB), and it runs
1149/1150 used at gameplay -- so a 150KB Z-buffer starves t3d (void) and fails
room-load mallocs (the door NULL-deref crash). "Fits in 8MB" was wrong; the
HEAP is the limit. zbuffer defaulted OFF; it is only viable world-off.

The heap is small BY DESIGN (PSX emulation), not a small-map bug. Reclaimable
if ever needed: texture cache PAGE_N 6->4 (~128KB, I bumped it for the counter
fix), the 2x96KB packet arenas. Not a bug to chase now.

USER CALL: do it RIGHT -- NATIVE CHARACTERS ON THE RSP (not the no-Z tile
grouping band-aid). Rationale: Harry's ~19ms is software-GTE per-vertex
transform (Gfx_MeshDraw / gte_rtpt in bodyprog_80055028.c) running on the CPU
because the port emulates the PSX GTE; the RSP is idle and is the right HW.
Native chars cut the CPU transform AND batch the geometry (collapsing the 254
window reloads) -- both levers, no Z-buffer, no heap fight.

### Native character plan (the real work)
- Harry is a SKINNED model: an ILM (skeleton + per-bone mesh headers,
  Bone_ModelAssign, s_Skeleton) animated by bone matrices. Draw path today:
  func_80057090 (bodyprog_80055028.c:1790) -> Gfx_MeshDraw (:2343) -> per-vertex
  gte_ldv3c/gte_rtpt (software GTE, the ~19ms).
- Phase C1: offline, convert Harry's ILM meshes to a t3d model (per-bone parts)
  + pre-tile his TIM pages into SHT-style tiles (reuse mkworld's tile baker).
- Phase C2: runtime, at the character-draw hook, feed t3d the per-bone matrices
  the game already computes (the skeleton's world/view mats) and let the RSP
  transform + the tile-grouped stream draw -- exactly the world's model, per
  character. Skip the PSX Gfx_MeshDraw for converted characters (fallback for
  unconverted).
- Phase C3: composite. Characters draw in the OT today; native chars need a
  draw point + depth handling vs the world (painter's split like the world, or
  the small per-char sort). Measure vs the 84ms baseline; target <33ms then
  <25ms.
- START with Harry only (the user's isolation); other charas reuse the path.

This is multi-session; each phase is buildable+testable on its own.

## 2026-09-06: C1 DONE, C2 IMPLEMENTED (first hardware build pending)

**C1 -- mkchara.py** (n64_port/tools): HERO.ILM parses with sh1fmt.Lm (it is
the same LM archive as world models). Harry = **23 RIGID parts, one per bone**
(01CHEST_, 02HEAD1, 02NECK, 03LSHOUL ... 17RFOOT, hands), 838 tris, ONE
material (HERO.TIM: 256x192 CI4, 15 CLUTs). No vertex skinning, so a part is
a world "instance" whose matrix is the bone's. Output N64C/HERO.SHW (22KB,
23 instances, 1 buffer, 1204 verts) + N64C/HERO.SHT (42KB, **21 tiles**, 7
pals). Staged by build_n64.sh from build/chara/N64C into rom:/N64C.

**C2 -- runtime** (t3d_world.c + hooks):
- `ShwLoadBody(c, arena, arenaBytes, f, ..., charMode)`: the SHW parser split
  out of ShT3d_WorldChunkLoaded and SHARED. charMode=1: no cell fold, no
  area-pool TileAcquire. World path byte-identical (charMode=0).
- Character store: `s_cTiles/s_cPool` (22 slots, eager-resident, never
  evicted, own palettes s_cPals) + `s_charChunk` in a 40KB `s_charArena`.
  ~84KB static .bss total. Separate from the area SHT so map changes never
  touch it. `BindCharTile` = BindWorldTile over that store (slot == tref-1).
- `RunPass(..., bind)`: takes the bind function; world passes BindWorldTile.
- API (sh_t3d.h): `ShT3d_CharaDrawBegin(isHarry)` brackets the bone loop
  (lazy CharaLoad("HERO") once; config native_chara=1; flips a 2-phase
  matrix buffer, clears the part mask) / `ShT3d_CharaBone(partIdx, m9, t3)`
  converts the bone's GAME view matrix (Q12 rot, Q8 t -> /8 like the world)
  into the part's T3DMat4FP and sets the mask bit / `ShT3d_CharaDrawEnd` /
  `ShT3d_CharaFlush` draws all fed parts (unwritten parts get a
  behind-the-eye collapse matrix), tile-grouped, groupPos=NULL (no fg/bg
  split -- the whole character is one unit).
- Hooks: bodyprog_bone_80044F14.c per-bone draw (both the SH_PC_PORT branch
  and the #else) -> `if (!ShT3d_CharaBone(modelInfo.modelIdx, &viewMat.m,
  viewMat.t)) func_80057090(...)`; world_draw.c ShxCharaDrawImpl brackets
  func_80045534 with DrawBegin(charaId==Chara_Harry)/DrawEnd; game_main.c
  calls ShT3d_CharaFlush after the background world flush and BEFORE
  GsDrawOt(OT0) (items/effects composite on top, foreground world occludes).
- partIdx == `modelInfo.modelIdx` == the ILM model-header index ==
  mkchara's part order (Bone_ModelAssign indexes the same header array).

**What this build removes**: BOTH levers at once for Harry -- the software-
GTE per-part draw is skipped (the ~19ms `chara` CPU) AND his ~254 per-prim
TMEM window reloads become ~21 tile loads. The held weapon (world_draw.c:
1112) and other charas stay on the PSX path. **Known limitation**: no Z, so
his tile-grouped parts can mis-order among themselves (arm through torso at
some angles). Z-buffer is next once the log's [MEMN64] confirms headroom
(PAGE_N can now drop: HERO pages left the PSX texture cache).

**Test read-out**: log `[T3DC] HERO.SHT: 21 tiles`, `[T3DC] HERO resident:
parts=23`, `[T3DC] native chara: parts=23/23` per census; `[PROF]` frame vs
the 84ms Harry-only baseline (win= should drop by ~250); `[MEMN64]` heap free.
Escape hatch: native_chara=0 in silenthill.cfg.

### C2 render-debugging chain (hardware, 2026-09-06)
1. Harry loaded+submitted (23/23) but INVISIBLE. Ruled out by inspection:
   scale (/8 consistent), culling, matrix math, on-screen projection (chest
   projects to ~(211,138)), palettes (pc.pal is the deduped index), winding
   (PERIM=0,1,3,2 handles strips). [T3DCB2] probe => **838 tris reach the RDP,
   0 misses** => drawn but composited under. FIX: CharaFlush moved AFTER
   GsDrawOt(OT0) (was before => buried under every OT0 prim).
2. Visible but FRAGMENTED (parts punch through). Depth-sort (mkchara one
   buffer per part + CharaFlush sorts by viewRow[.z] farthest-first) did NOT
   help => not inter-part order.
3. => BACKFACES: cullMask=2 (no cull) means each closed part draws front AND
   back faces; no Z => far faces paint over near. World is flat/single-sided
   so needs none. FIX: T3D_FLAG_CULL_BACK for the char draw, config
   chara_cull=1 back / 2 front / 0 none (flip if he vanishes). [T3DCB] now
   logs all 23 part T-positions to confirm placement is coherent in parallel.
Perf: native Harry is ~50ms/frame (233->~175ms). Native-char win CONFIRMED;
world (~150ms) is the remaining, memory-bound bottleneck.

## PERF DIAGNOSIS 2026-09-07 (RDP-bound, concrete)

Police-station reception, [PROF] with native world + native Harry both live:
- frame ~203ms (~5fps); **rdp pipe busy ~182ms of 203ms = RDP-bound** (CPU
  chunk=48ms, chara=3.4ms overlap under it).
- Native world [T3DW]: **blocks=12 tileRam=166K tileUp=215 dedup=0**; the
  resident chunk (ERFF00) has **tiles=83/83** unique. So **83 unique tiles are
  re-uploaded 215x/frame (~2.6x)** — painter's depth order interleaves tiles so
  the consecutive-bind dedup never hits (dedup=0). Each upload forces a pipe sync
  (BindWorldTile: sync_pipe+sync_load+sync_tile — t3d RSP tris race rdpq uploads,
  see its comment), and the pipe drains the previous batch's triangles each time.
  That load->draw->sync serialization x215 IS the 182ms (texture DMA itself is
  only tmem=12ms; fill is ~2ms for px=128K; per old analysis "texture <5%").

THE FIX (draw each unique tile ONCE -> uploads 215->83, syncs with them, ~2x+ fps):
needs a Z-buffer so OPAQUE geometry can be globally sorted BY TILE instead of by
depth. Plan:
1. Z-buffer 320x240x16 = 150KB. Heap is FULL (1152/1208, 56KB free) and a static
   .bss buffer just shrinks the heap (same RAM), so **150KB must be RECLAIMED
   first**. Candidates: s_swzScratch 64KB (swizzle identity on N64 -> likely
   dead), trim s_vram(1MB)/s_spuRam(512KB) to actual use, or lower internal res
   (256x240x16=120KB Z). g_PsxRam(2.125MB)'s +128KB is a REAL overrun guard — do
   not cut.
2. Opaque pass: rdpq_mode_zbuf(true,true); collect all opaque tile-groups across
   the frame's blocks, SORT BY TILE, draw each tile's groups together.
3. Semitrans pass AFTER: rdpq_mode_zbuf(true,false) (Z-test, no write), in
   submission order — preserves PSX blending (the reason a naive Z was rejected).
GATE behind zbuffer=1 so the working painter's path stays default (nothing to
revert; toggle to compare). Confirm the split first with rdp_probe=1 (config-only,
no rebuild): mode 1 NOFILL keeps commands/loads/syncs — if pipe stays high it's
the syncs (expected), mode 2 NOTEX drops loads.

## IMPLEMENTED 2026-09-08 (gated: zbuffer=1; default 0 = untouched painter's path)
- **Memory**: `pc_chara_pool.c` s_poolBoneCoords `[Chara_Count][57]` (205KB .bss)
  is dead on N64 (globalCharaPool=0 -> every pool entry point early-returns);
  now `[POOL_BONE_ROWS=1][57]` under SH_N64_PORT with an index guard in the
  loader. Heap gains ~205KB -> the 150KB `surface_alloc` Z-buffer fits (~110KB
  margin). `s_swzScratch` (64KB, identity swizzle on N64) is a further reclaim
  if ever needed.
- **Z clear**: gpu_rdp.c frame begin, after `rdpq_attach(fb, zbuf)`: retarget
  colour image at the Z surface, fill 0xFFFC (t3d's clear value), restore.
- **RunPass(..., int preamble)**: the top-of-pass `rdpq_sync_pipe` + blender set
  is skipped for the 2nd..Nth record of a same-pass run — a pipe drain per record
  would have cost what the sort saves. All 7 call sites updated.
- **WorldFlushPass Z path** (`s_zActive`): background call = ALL opaque streams
  split at OP_TILE into `ZRec`s (`s_zrec[1024]`, overflow counted), `qsort` by
  (tref,pal), replayed back to back -> the consecutive-bind dedup makes each
  unique tile ONE upload. Foreground call = semitransparent streams in
  submission order under `rdpq_mode_zbuf(true,false)` (test, no write).
  groupPos=NULL / wantFg=0 in the Z path (no depth classification needed).
- **Char**: CharaFlush drawflags add T3D_FLAG_DEPTH when s_zActive (its own
  set_drawflags had been dropping WorldFrameStart's DEPTH).
- Order per frame: opaque world (Z-write) -> PSX OT0 items (no Z, as before)
  -> native Harry (Z-write) -> semitrans world (Z-test) -> OT2 2D (no Z).
VERIFY on hardware with zbuffer=1: `[GPU] z-buffer ON`, `[T3DW] tileUp` should
fall from ~215 toward the ~83 unique tiles, `[PROF] rdp pipe` should drop.

## ROOT CAUSE FOUND 2026-09-08: THE WORLD WAS DRAWN TWICE (user's "old view underneath")
Ipd_ChunkDraw's per-instance native skip (the g5 counter fix) tested "local
model" as `modelHdr in [IPD_BUFFER, IPD_BUFFER+0x2C000)` -- the PSX's FIXED IPD
window (PSX_ADDR(0x00175600) inside g_PsxRam, ~0x803F97B0). N64 chunks live in
heap owned-slot callocs (s_pcSlotOwnedBuf, 0x8072xxxx), so the test NEVER
matched and NO instance was ever skipped. Consequences, all measured:
- the PSX mesh path re-transformed + re-culled the whole world every frame
  (chunk=40ms CPU, pure waste -- native draws it);
- the ~460 cull survivors were drawn AGAIN through the OT with per-primitive
  texture windows: that IS the "509 PSX tris / 256 uploads" half of the RDP
  cost (not items/effects, not PLM -- ER has plmFileIdx=NO_VALUE);
- the RDP overload made a command buffer take >2s and rspq.c:973's watchdog
  reported it as an "RSP crash" at whatever pc the RSP held (5f8, then 018) --
  the walking-around crash. ([OTS] prims is a 60-frame sum: 28214 ~= 470/frame.)
FIX: test local membership in THIS chunk's LM table, `modelHdr in
[ipdHdr->lmHdr->modelHdrs, +modelCount)` (what LmHeader_ModelHeaderSearch uses),
so native buffers' local instances skip the PSX path and global-PLM instances
still fall through. Expect: chunk CPU -> ~0, PSX uploads 256 -> ~0, RDP pipe
drops by the double-draw share, watchdog crash gone. Then zbuffer=1 tile-batching
attacks the remaining native 215 uploads.

## THIRD CAUSE 2026-09-08: THE NATIVE WORLD DREW BOTH SIDES OF EVERY WALL
t3d_world.c never set a cull flag for world blocks (`cull_backfaces` only drives
the PSX-path software cull in gpu_rdp.c). The PSX game is NOT double-sided:
every mesh emitter in bodyprog_80055028.c rejects a quad when
`nclip(v0,v1,v2) <= 0` (the second nclip on v3 only rescues twisted quads), so
the RSP drawing both faces filled every wall, floor and ceiling twice over what
the PSX rasterised -- and the counters said fill, not uploads: `rdp pipe=147ms`
with `tmem=10ms` (loads are cheap; the pipe was rasterising).
FIX: `world_cull` (pc_config, default 1) adds T3D_FLAG_CULL_FRONT to the world
draw flags in WorldFrameStart. Direction: mkworld keeps PSX winding (fan over
the perimeter order) and Harry proved that PSX winding + CULL_BACK culls the
VISIBLE side under the axis-flip camera (mkchara reverses its winding for that
reason), so the PSX's own rejection is CULL_FRONT for the world. `world_cull=2`
flips it, `0` disables. Re-applied every flush pass because CharaFlush sets
Harry's CULL_BACK between the background and foreground passes.
Also: the chunk-resident log line now carries `tris=` (static count over all
passes) so a `[PROF]` pipe time can be read against the chunk's fill bound.
