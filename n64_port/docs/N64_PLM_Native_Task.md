# N64: Convert global-PLM world content to the native renderer

## Why
The drifting/clipping items on walls and counters, and the MISSING reception
counter, are all **global PLM models**. mkworld (`collect_cell`) bakes only a
chunk's LOCAL models and explicitly skips `info.is_global_plm`
(mkworld.py:284), so PLMs fall through to the PSX per-prim path (OT + GTE) at
`bodyprog_80040B74.c:3518`'s membership test. The PSX path is 2D (pos[2]=0, no
Z) and re-projects independently of the native world, which is exactly why PLMs
(a) clip through walls (no Z vs the world Z-buffer) and (b) drift as the camera
moves. Converting them to native puts them in the same Tiny3D + Z + projection
path as the room, killing the whole PSX-vs-native mismatch class.

## Architecture (verified 2026-09-12)
- Global PLM geometry loads from `BG/*_GLB.PLM` (FILE_BG_*_GLB_PLM in
  fileenum.h; e.g. DR_GLB.PLM for the police station) into
  `g_Map.globalLm` (s_GlobalLm; Lm_Init / LmHeader_LoadStateGet,
  bodyprog_80040B74.c:512-558). A `.PLM` file is the LM format at offset 0
  (sh1fmt/lm.py already parses it; lm.py:3).
- A cell's model-buffer instances whose `modelInfo.isGlobalPlm` is true
  reference models in `g_Map.globalLm.lmHdr->modelHdrs`, NOT the cell's LM.
- Native/PSX split: an instance draws native iff its modelHdr is inside the
  chunk's own `lmHdr->modelHdrs[0..modelCount)` (3518); else PSX fallthrough.

## APPROACH PIVOT (2026-09-12): bake PLMs INTO the cell SHW via mkworld
mkplm.py (standalone baker) works (P1 done: DR_GLB.PLM -> 30 models), but a
SEPARATE PLM runtime store fights the ~151KB in-game free heap (PLM geometry
~69KB + a tile pool would OOM; tiles would have to share the world pool -- lots
of new, risky runtime code). MUCH simpler + safer: teach mkworld to RESOLVE
is_global_plm instances against the area's GLB.PLM and bake their geometry into
the cell SHW like a local model. Then the EXISTING native world renderer draws
them -- tiles, deferred draw, Z, memory all reused. Only runtime change: the
membership test at bodyprog_80040B74.c:3518 must ALSO skip isGlobalPlm instances
(they are native now) so the PSX path does not double-draw them.
- area->GLB map: Ipd_MapFileInfoSet(mapTag, plmIdx, ...) sets g_Map.globalLm's
  file per map; the per-map callers in src/maps/* pass the FILE_BG_*_GLB_PLM.
  mkworld needs prefix->GLB (extract from those callers, or pass --glb).
- NOTE: ER (reception) has only 1 PLM, so this mainly helps the PLM-heavy rooms
  (DR=153, RSR=534, SPR=259). The reception's ammo/poster drift is Track B
  (pickups/items), a separate path.

## Plan (phases)
**P1 - bake GLB.PLM to native (offline tool).** New `mkplm.py` (or a mode of
mkworld): parse `BG/<AREA>_GLB.PLM` with sh1fmt.lm, bake every model with the
EXISTING mkworld tile/stream encoder (`assign_tiles`/`bake_tiles`/
`encode_buffer_cmds`), emitting one native asset keyed by model NAME (the
instance's `info.name` is the link). Output e.g. `N64W/<AREA>_GLB.SHW` + `.SHT`.
Verts stay s16 Q8, world /8 scale as mkworld. Textures: PLM materials resolve
to the same area TIMs mkworld already uses.
  - INPUT SOURCE TBD: locate the extracted `BG/*_GLB.PLM` (same place mkworld's
    IPD inputs come from -- check the mkworld invocation / gamedata tree).

**P2 - runtime native PLM store.** Load the area's `*_GLB.SHW/.SHT` into a
resident native store keyed by model name (parallel to the char store). When a
cell instance is a global PLM (isGlobalPlm), look up the baked model by name and
hand its stream + the instance's composed view matrix to the native flush
(reuse ShT3d_ComposeInstanceView for the matrix, RunPass for the stream). Remove
the PSX fallthrough for resolved PLMs (keep it for any unbaked ones).

**P3 - verify.** Reception counter renders; PLMs Z-test against walls (no
clip); no drift (native shares the world view+proj). Watch heap (now ~150-180KB
free after the kanji reclaim) and the tile pool (128 slots; PLMs add tiles --
may need budget).

## Notes
- This does NOT fix the CHARACTER issues (gun-through-body, folding) -- those are
  native already; separate task ([[project_n64_native_chara_vertex_pool]]).
- Billboards (field_10 Gfx_BillboardDraw) are a separate PSX-path class (camera
  sprites); handle after PLMs if they still drift.
