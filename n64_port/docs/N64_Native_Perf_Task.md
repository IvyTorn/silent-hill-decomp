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

## Status

- Diagnosis corrected and recorded ([[project_n64_port]] g8).
- Phase A converter change: STARTED (see mkworld.py `--plm-file`).
- Awaiting: extract the ER PLM pool from the BIN, run, verify the counter draws
  natively and `win=`/`pipe=` drop in the next `[PROF]`.
