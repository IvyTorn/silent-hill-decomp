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
