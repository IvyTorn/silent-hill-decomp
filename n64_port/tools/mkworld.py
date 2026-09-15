#!/usr/bin/env python3
"""mkworld.py - IPD map cells -> native N64 world chunks (SHW1) + tile store (SHT1).

    mkworld.py --ipd-dir DIR --tim-dir DIR --out DIR PREFIX [PREFIX...]

The offline half of the Tiny3D world renderer. For each map-area PREFIX
(e.g. ER for the police station):

  <out>/N64W/<PREFIX>.SHT       tile store: CI4/CI8 texture tiles + RGBA16
                                palettes, cut from the area's TIM files along
                                the UV clusters the prims actually use.
  <out>/N64W/<CELL>.SHW         per-cell geometry: model-buffer draw streams
                                (vert windows + tri lists + tile refs),
                                instance matrices, opaque/semitrans split,
                                and the list of SHT tiles the cell needs.

Runtime counterpart: n64_port/src/t3d_n64.c records one rspq block per model
buffer from the SHW stream at chunk-load time; Ipd_ChunkDraw keeps its subcell
PVS and runs the block instead of the per-prim GTE path.

Key facts this encoding relies on (verified against the runtime):
  - Prim UVs on disc are TIM-LOCAL texel coords; the runtime adds the TIM's
    VRAM base at load (Model_MaterialFlagsApply). Offline we resolve straight
    into the TIM file and never model VRAM.
  - Prim clut on disc is row*64 into the TIM's OWN clut block.
  - PSX 4bpp/8bpp indexed pixels -> N64 CI4/CI8 losslessly (indices survive;
    CI4 swaps nibble order: N64 EVEN s = HIGH nibble).
  - Palette: PSX 1555 -> N64 5551; PSX colour 0x0000 -> alpha 0 (RDP alpha
    compare discards), matching psx_vram.c's TLUT semantics.
  - Verts stay s16 Q8 model-local; the 1/8 world scale that keeps exterior
    translations inside s16.16 lives in the instance matrix, not the data.

Tiling: per (tim, clut) the prims' UV boxes are greedily packed into
TMEM-sized tiles at 16/8-snapped free origins; only prims whose own box fits
no legal tile are clipped (at the UV midpoint of the worse axis - linear
interpolation is exact for PSX affine texturing, and shared edges clip to
identical points). Identical tiles (same pixels + palette) dedup by content
hash: SH repeats wall/floor art constantly.
"""
import argparse
import hashlib
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh1fmt.ipd import Ipd  # noqa: E402
from sh1fmt.lm import Lm    # noqa: E402  (global-PLM resolution, --glb)
from sh1fmt.tim import Tim  # noqa: E402

PERIM = (0, 1, 3, 2)          # vi order -> quad perimeter
VERT_WINDOW = 64              # t3d_vert_load window (RSP buffer is 70)

# Tile shapes to try per seed box, in preference order. (w, h) in texels for
# 4bpp sources; 8bpp halves the height (TMEM budget 4096 vs 2048 texels).
SHAPES_4 = ((64, 64), (128, 32), (32, 128), (256, 16), (16, 256))
SHAPES_8 = ((64, 32), (128, 16), (32, 64), (256, 8), (16, 128))

# ---------------------------------------------------------------- draw ops
OP_TILE   = 0x1  # next word = tile index + 1 (0 = untextured)
OP_MATRIX = 0x2  # arg = instance index (within the cell's instance table)
OP_VERTS  = 0x3  # arg = count, next word = first vert index
OP_TRIS   = 0x4  # arg = tri count, then packed u8 idx triples (padded to u16)
OP_END    = 0xF


def op(word_op, arg):
    assert 0 <= arg <= 0xFFF, arg
    return (word_op << 12) | arg


class Piece:
    """One triangle (post-split) with float (x,y,z,u,v) corners.
    key = (timName, clutRow) or None; tile/pal filled by assign_tiles."""
    __slots__ = ("cell", "buf", "inst", "semi", "verts", "key", "tile", "pal")

    def __init__(self, cell, buf, inst, semi, verts, key):
        self.cell, self.buf, self.inst = cell, buf, inst
        self.semi, self.verts, self.key = semi, verts, key
        self.tile = None
        self.pal = 0

    def box(self):
        us = [p[3] for p in self.verts]
        vs = [p[4] for p in self.verts]
        return min(us), min(vs), max(us), max(vs)


def clip_tri_line(poly, axis, line, keep_less):
    """Sutherland-Hodgman against u/v == line; kept side includes the line."""
    out = []
    n = len(poly)
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        av, bv = a[3 + axis], b[3 + axis]
        a_in = (av <= line) if keep_less else (av >= line)
        b_in = (bv <= line) if keep_less else (bv >= line)
        if a_in:
            out.append(a)
        if a_in != b_in:
            t = (line - av) / (bv - av)
            # interpolate x,y,z,u,v; carry any extra fields (mkchara's per-vertex
            # owner) unchanged from a -- owner is discrete, not interpolable.
            out.append(tuple(a[k] + (b[k] - a[k]) * t for k in range(5)) + tuple(a[5:]))
    return out


def fan(poly):
    return [[poly[0], poly[i + 1], poly[i + 2]] for i in range(len(poly) - 2)]


class Tile:
    """A pixel tile: one region of one TIM. Palette-independent — the same
    tile serves every clut row; palette is separate draw state (a TLUT)."""
    __slots__ = ("tim", "u0", "v0", "w", "h", "index", "final")

    def __init__(self, tim, u0, v0, w, h):
        self.tim = tim                     # timName
        self.u0, self.v0, self.w, self.h = u0, v0, w, h
        self.index = -1                    # provisional, before content dedup
        self.final = -1


def shapes_for(bpp):
    return SHAPES_4 if bpp == 4 else SHAPES_8


def fits_any(bw, bh, shapes):
    return any(bw <= w and bh <= h for w, h in shapes)


def candidate_origin(u0, u1, w):
    """Aligned-grid origin for span [u0,u1] under tile width w, or None.
    Aligned-only quantisation is what keeps resident tile bytes at the
    touched-cell floor and makes identical wall regions dedup by content:
    every prim of the same texture cell lands in the same tile."""
    base = int(u0) // w * w
    # u1 == base + w is INSIDE: UV coordinate base+w is the right edge of
    # texel base+w-1. Excluding it made a tri whose max sits exactly on a
    # gridline clip into itself forever.
    if u1 <= base + w:
        return base
    return None


def assign_tiles(pieces_by_key, tims, stats):
    """Assign every textured piece a pixel Tile (grid-quantised origins) and a
    palette id; split pieces no candidate covers. Returns ([Tile], palettes)
    where palettes is the (tim, clutRow) -> palIdx map's table of raw entries.
    """
    tiles = []
    by_key = {}          # (tim, w, h, u0, v0) -> Tile

    pal_table = []       # [bytes] RGBA16 payloads
    pal_idx = {}         # (timName, clutRow) -> index into pal_table
    pal_by_content = {}

    def palette_id(tim_name, clut_row):
        k = (tim_name, clut_row)
        if k in pal_idx:
            return pal_idx[k]
        tim = tims[tim_name]
        pal = tim.palette(clut_row)
        if pal is None:
            stats["bad_clut_row"] += 1
            pal = [0] * (16 if tim.bpp == 4 else 256)
        pdat = b"".join(struct.pack(">H", psx1555_to_n64_5551(c)) for c in pal)
        idx = pal_by_content.get(pdat)
        if idx is None:
            idx = len(pal_table)
            pal_table.append(pdat)
            pal_by_content[pdat] = idx
        pal_idx[k] = idx
        return idx

    def tile_for(tim_name, shapes, u0, v0, u1, v1):
        for w, h in shapes:
            ou = candidate_origin(u0, u1, w)
            ov = candidate_origin(v0, v1, h)
            if ou is None or ov is None:
                continue
            key = (tim_name, w, h, ou, ov)
            t = by_key.get(key)
            if t is None:
                t = Tile(tim_name, ou, ov, w, h)
                t.index = len(tiles)
                tiles.append(t)
                by_key[key] = t
            return t
        return None

    for key, plist in sorted(pieces_by_key.items()):
        tim_name, clut_row = key
        tim = tims[tim_name]
        shapes = shapes_for(tim.bpp)
        pal = palette_id(tim_name, clut_row)

        work = list(plist)
        ready = []
        while work:
            pc = work.pop()
            u0, v0, u1, v1 = pc.box()
            tile = tile_for(tim_name, shapes, u0, v0, u1, v1)
            if tile is not None:
                pc.tile = tile
                pc.pal = pal
                ready.append(pc)
                continue
            stats["split"] += 1
            # Clip at the base grid line inside the span of the axis that
            # overflows more cells; pieces terminate against cell edges, so
            # the recursion converges and pieces share cell tiles exactly.
            # Crossings are counted HALF-OPEN: a span ending exactly on a
            # gridline does not cross it (the edge coordinate belongs to the
            # lower cell), else the clip line fails to cut and loops forever.
            gw = 64
            gh = 64 if tim.bpp == 4 else 32
            cross_u = int(max(u1 - 1e-4, u0)) // gw - int(u0) // gw
            cross_v = int(max(v1 - 1e-4, v0)) // gh - int(v0) // gh
            assert cross_u > 0 or cross_v > 0, (u0, v0, u1, v1)
            if cross_u >= cross_v and cross_u > 0:
                axis, line = 0, (int(u0) // gw + 1) * gw
            else:
                axis, line = 1, (int(v0) // gh + 1) * gh
            lo = clip_tri_line(pc.verts, axis, line, True)
            hi = clip_tri_line(pc.verts, axis, line, False)
            for part in (lo, hi):
                if len(part) >= 3:
                    for tri in fan(part):
                        work.append(Piece(pc.cell, pc.buf, pc.inst, pc.semi,
                                          tri, pc.key))
        plist[:] = ready
    return tiles, pal_table


PSX_TO_N64_NIBBLE = bytes(((b & 0x0F) << 4) | (b >> 4) for b in range(256))


def psx1555_to_n64_5551(c):
    """PSX 15-bit BGR + STP -> N64 RGBA5551. Colour 0 = fully transparent,
    matching the runtime TLUT rule in psx_vram.c."""
    if c == 0:
        return 0
    r = c & 0x1F
    g = (c >> 5) & 0x1F
    b = (c >> 10) & 0x1F
    return (r << 11) | (g << 6) | (b << 1) | 1


def bake_tiles(tiles, tims, stats):
    """Cut pixel payloads, dedup identical content (palette-independent).
    Returns payloads where payloads[i] = (fmt, w, h, pixBytes)."""
    payloads = []
    by_hash = {}
    for tile in tiles:
        tim = tims[tile.tim]
        fmt = 0 if tim.bpp == 4 else 1
        pix = tim.pixels_rect(tile.u0, tile.v0, tile.w, tile.h)
        if fmt == 0:
            pix = pix.translate(PSX_TO_N64_NIBBLE)
        digest = hashlib.blake2b(
            struct.pack(">3H", fmt, tile.w, tile.h) + pix,
            digest_size=16).digest()
        final = by_hash.get(digest)
        if final is None:
            final = len(payloads)
            payloads.append((fmt, tile.w, tile.h, pix))
            by_hash[digest] = final
        else:
            stats["tile_dedup"] += 1
        tile.final = final
    return payloads


# ------------------------------------------------------------- phase 1

def collect_cell(ipd, cellno, tim_get, pieces_by_key, untex, stats, glb_lm=None,
                 plm_names=None):
    """Parse one cell into Pieces. Returns instances. A global-PLM instance
    (shared wall/counter/prop) is resolved against the area's GLB.PLM (glb_lm)
    and baked like a local model -- its NAME is recorded in plm_names (a set)
    so the runtime knows which PLM instances are now native and skips them on
    the PSX path (else they double-draw). An UNRESOLVED PLM stays on the PSX
    path (not baked, not recorded), so it must NOT be skipped at runtime."""
    lm = ipd.lm
    instances = []
    for bi, buf in enumerate(ipd.model_buffers):
        for inst in buf.instances:
            info = ipd.model_infos[inst.model_info_idx]
            if info.is_global_plm:
                model = glb_lm.model_by_name(info.name) if glb_lm is not None else None
                if model is None:
                    stats["skip_plm"] += 1
                    continue
                src_lm = glb_lm            # PLM materials live in the GLB, not the cell
                if plm_names is not None:
                    plm_names.add(info.name)
                stats["plm_baked"] += 1
            else:
                model = lm.model_by_name(info.name)
                if model is None:
                    stats["skip_missing_model"] += 1
                    continue
                src_lm = lm
            inst_idx = len(instances)
            instances.append(inst)
            for mesh in model.meshes:
                for prim in mesh.prims:
                    corners = prim_corner_order(prim)
                    if len(corners) < 3:
                        stats["degenerate"] += 1
                        continue
                    poly = []
                    for c in corners:
                        vi = prim.vi[c]
                        x, y, z = mesh.verts[vi] if vi < len(mesh.verts) else (0, 0, 0)
                        u, v = prim.uv[c]
                        poly.append((float(x), float(y), float(z), float(u), float(v)))
                    key = None
                    if prim.material_idx >= 0:
                        mat = src_lm.materials[prim.material_idx]
                        if tim_get(mat.name) is not None:
                            key = (mat.name, prim.clut // 64)
                        else:
                            stats["missing_tim"] += 1
                    # PSX quad = two triangles (0,1,2)+(0,2,3 in perimeter order)
                    for tri in fan(poly):
                        pc = Piece(cellno, bi, inst_idx, prim.is_transparent,
                                   tri, key)
                        if key is None:
                            untex.append(pc)
                        else:
                            pieces_by_key[key].append(pc)
    return instances


class _ObjInst:
    """Placeholder instance for an un-instanced LM model baked as a world OBJECT:
    the runtime overwrites its matrix every frame with the object's own view
    matrix (ShT3d_WorldObjectDraw), so what is baked here is never used."""
    __slots__ = ("rot", "trans")
    def __init__(self):
        self.rot = [[4096, 0, 0], [0, 4096, 0], [0, 0, 4096]]
        self.trans = [0, 0, 0]


def collect_objects(ipd, cellno, base_buf, instances, tim_get, pieces_by_key,
                    untex, stats):
    """Bake the cell's UN-instanced local LM models -- the *_HID / *_HIDE item
    pickups (SHOTGUN_, ITEM_HID, KEY_HIDE, MAP_HIDE...) that game logic places
    at runtime as world objects, which collect_cell never sees because no IPD
    instance references them -- as extra buffers base_buf+k, each with a
    placeholder instance. Drawn natively they share the world's Z-buffer, so
    an ammo box sits ON the counter instead of being painted over everything
    by the depth-less PSX OT. Returns the 8-char names in buffer order; main()
    writes them into the OBJ1 trailer the runtime finds them by. Measured on
    the reception cells: at most one or two such models, <= 68 tris each."""
    lm = ipd.lm
    inst_names = set()
    for buf in ipd.model_buffers:
        for inst in buf.instances:
            info = ipd.model_infos[inst.model_info_idx]
            if not info.is_global_plm:
                inst_names.add(info.name)
    names = []
    for model in lm.models:
        if model.name in inst_names:
            continue
        buf_idx  = base_buf + len(names)
        inst_idx = len(instances)
        instances.append(_ObjInst())
        names.append(model.name)
        for mesh in model.meshes:
            for prim in mesh.prims:
                corners = prim_corner_order(prim)
                if len(corners) < 3:
                    stats["degenerate"] += 1
                    continue
                poly = []
                for c in corners:
                    vi = prim.vi[c]
                    x, y, z = mesh.verts[vi] if vi < len(mesh.verts) else (0, 0, 0)
                    u, v = prim.uv[c]
                    poly.append((float(x), float(y), float(z), float(u), float(v)))
                key = None
                if prim.material_idx >= 0:
                    mat = lm.materials[prim.material_idx]
                    if tim_get(mat.name) is not None:
                        key = (mat.name, prim.clut // 64)
                    else:
                        stats["missing_tim"] += 1
                for tri in fan(poly):
                    pc = Piece(cellno, buf_idx, inst_idx, prim.is_transparent, tri, key)
                    (untex if key is None else pieces_by_key[key]).append(pc)
        stats["objects_baked"] += 1
    return names


class _ItemShim:
    """Header identity of the objects-only item pseudo-cell: cellX = cellZ = -127
    (a real cell is never -127; the runtime keys lmIdx-2 objects to it)."""
    cell_x = -127
    cell_z = -127


def collect_item_models(lm, cellno, tim_get, pieces_by_key, untex, instances, stats):
    """Bake EVERY model of the shared item LM (BG/BG_ITEM.PLM: AIDKIT_N, AMPULE_N,
    BULLET_N, DRINK_NE, PAD_NEAR, SHELL_NE, SHOT_NEA -- 69 tris) as its own buffer
    + placeholder instance, so the global-pool (lmIdx 2) world objects -- the
    ammo box on the counter -- draw natively with Z like the cell-local pickups.
    Their tiles join the AREA's SHT (same tile pass), so one bake per area."""
    names = []
    for k, model in enumerate(lm.models):
        instances.append(_ObjInst())
        names.append(model.name)
        for mesh in model.meshes:
            for prim in mesh.prims:
                corners = prim_corner_order(prim)
                if len(corners) < 3:
                    stats["degenerate"] += 1
                    continue
                poly = []
                for c in corners:
                    vi = prim.vi[c]
                    x, y, z = mesh.verts[vi] if vi < len(mesh.verts) else (0, 0, 0)
                    u, v = prim.uv[c]
                    poly.append((float(x), float(y), float(z), float(u), float(v)))
                key = None
                if prim.material_idx >= 0:
                    mat = lm.materials[prim.material_idx]
                    if tim_get(mat.name) is not None:
                        key = (mat.name, prim.clut // 64)
                    else:
                        stats["missing_tim"] += 1
                for tri in fan(poly):
                    pc = Piece(cellno, k, k, prim.is_transparent, tri, key)
                    (untex if key is None else pieces_by_key[key]).append(pc)
        stats["items_baked"] += 1
    return names


def prim_corner_order(prim):
    seen, out = set(), []
    for c in PERIM:
        vi = prim.vi[c]
        if vi == 0xFF or vi in seen:
            continue
        seen.add(vi)
        out.append(c)
    return out


# ------------------------------------------------------------- encoding

def encode_buffer_cmds(pieces, verts_out):
    """pieces: this buffer's pieces of ONE pass (opaque or semi), pre-sorted
    by (tileFinal, inst). Emits the u16 cmd stream; appends verts to verts_out.
    """
    cmds = []
    cur_tile = "unset"      # (tileIdx, palIdx)
    cur_inst = "unset"
    window = {}
    win_base = [len(verts_out)]
    tris = []

    def flush():
        if tris:
            # T3DVertPacked interleaves vertex PAIRS: the runtime hands
            # t3d_vert_load a pointer at win_base/2 packed structs and an even
            # count, so both must be even. Pad with a dup of the last vert.
            if len(window) & 1:
                verts_out.append(verts_out[-1])
            cmds.append(op(OP_VERTS, (len(window) + 1) & ~1))
            cmds.append(win_base[0])
            cmds.append(op(OP_TRIS, len(tris)))
            packed = []
            for t in tris:
                packed += list(t)
            if len(packed) & 1:
                packed.append(0)
            for i in range(0, len(packed), 2):
                cmds.append((packed[i] << 8) | packed[i + 1])
        window.clear()
        if len(verts_out) & 1:
            verts_out.append(verts_out[-1] if verts_out else
                             (0, 0, 0, 0, 0, 0, 0, 0, 0))
        win_base[0] = len(verts_out)
        del tris[:]

    if len(verts_out) & 1:
        verts_out.append(verts_out[-1] if verts_out else
                         (0, 0, 0, 0, 0, 0, 0, 0, 0))
    win_base[0] = len(verts_out)

    def vkey(p):
        return (round(p[0] * 2), round(p[1] * 2), round(p[2] * 2),
                round(p[3] * 32), round(p[4] * 32))

    for pc in pieces:
        tile = pc.tile
        tkey = (0, 0) if tile is None else (tile.final + 1, pc.pal)
        if tkey != cur_tile:
            flush()
            cmds.append(op(OP_TILE, tkey[1]))
            cmds.append(tkey[0])
            cur_tile = tkey
            cur_inst = "unset"
        if pc.inst != cur_inst:
            flush()
            cmds.append(op(OP_MATRIX, pc.inst))
            cur_inst = pc.inst

        fresh = [p for p in pc.verts if vkey(p) not in window]
        if len(window) + len(fresh) > VERT_WINDOW:
            flush()
            fresh = pc.verts
        for p in fresh:
            k = vkey(p)
            if k in window:
                continue
            window[k] = len(window)
            s = p[3] - (tile.u0 if tile else 0)
            t = p[4] - (tile.v0 if tile else 0)
            verts_out.append((int(round(p[0])), int(round(p[1])),
                              int(round(p[2])), 128, 128, 128, 255, s, t))
        idx = [window[vkey(p)] for p in pc.verts]
        for k in range(len(idx) - 2):
            cmds_tri = (idx[0], idx[k + 1], idx[k + 2])
            tris.append(cmds_tri)
    flush()
    cmds.append(op(OP_END, 0))
    return cmds


def encode_shw(ipd, buffer_count, cell_pieces, instances, buf_encoder=None):
    """SHW1 layout (all big-endian, the console's own order):
      0x00 u32 magic 'SHW1'
      0x04 s8 cellX, s8 cellZ, u16 bufferCount
      0x08 u16 instanceCount, u16 tileRefCount
      0x0C u32 instancesOff, u32 tileRefsOff
      0x14 bufferCount * { u16 vertCount; u16 opaWords; u16 semiWords; u16 pad;
                           u32 vertOff; u32 opaOff; u32 semiOff; }   (20 B)
      tileRefs:  u16 final tile indices this cell uses (for load/refcount)
      instances: { s16 rot[9]; s16 pad; s32 t[3]; }  (32 B: 18+2+12, IPD
                  verbatim, t[0]/t[2] cell-relative Q8, t[1] absolute Q8)
      verts:     { s16 x,y,z; u8 r,g,b,a; s16 s,t; u16 pad; } (16 B)
                 s,t are 10.5 fixed-point tile-local texels, T3D's own unit.
      cmds:      u16 words
    """
    # group pieces per buffer/pass, sort by (tile, pal, inst) for minimal state
    per_buf = defaultdict(lambda: ([], []))   # buf -> (opa, semi)
    tile_refs = set()
    for pc in cell_pieces:
        per_buf[pc.buf][1 if pc.semi else 0].append(pc)
        if pc.tile is not None:
            tile_refs.add(pc.tile.final)

    def sort_key(pc):
        return (0 if pc.tile is None else pc.tile.final + 1, pc.pal, pc.inst)

    buffers = []
    for bi in range(buffer_count):
        opa, semi = per_buf.get(bi, ([], []))
        opa.sort(key=sort_key)
        semi.sort(key=sort_key)
        verts = []
        enc = buf_encoder or encode_buffer_cmds
        c_opa = enc(opa, verts)
        c_semi = enc(semi, verts)
        # The runtime concatenates buffers into one T3DVertPacked array and
        # rebases per buffer, so every buffer must hold an EVEN vert count.
        if len(verts) & 1:
            verts.append(verts[-1] if verts else (0, 0, 0, 0, 0, 0, 0, 0, 0))
        buffers.append((c_opa, c_semi, verts))

    refs = sorted(tile_refs)
    base = 0x14 + 20 * len(buffers)   # entries are ">4H3I" = 20 bytes
    blobs = bytearray()

    def blob(data):
        nonlocal blobs
        off = base + len(blobs)
        blobs += data
        while len(blobs) & 3:
            blobs += b"\x00"
        return off

    refs_off = blob(struct.pack(">%dH" % len(refs), *refs) if refs else b"")
    inst_data = bytearray()
    for inst in instances:
        r = inst.rot
        inst_data += struct.pack(">9h", *(list(r[0]) + list(r[1]) + list(r[2])))
        inst_data += struct.pack(">h", 0)
        inst_data += struct.pack(">3i", *inst.trans)
    instances_off = blob(bytes(inst_data))

    table = bytearray()
    for c_opa, c_semi, verts in buffers:
        vdata = bytearray()
        for (x, y, z, r, g, b, a, s, t) in verts:
            vdata += struct.pack(">3h4B2hH", x, y, z, r, g, b, a,
                                 int(round(s * 32)), int(round(t * 32)), 0)
        vo = blob(bytes(vdata))
        oo = blob(struct.pack(">%dH" % len(c_opa), *c_opa))
        so = blob(struct.pack(">%dH" % len(c_semi), *c_semi))
        assert len(verts) <= 0xFFFF and len(c_opa) <= 0xFFFF and len(c_semi) <= 0xFFFF
        table += struct.pack(">4H3I", len(verts), len(c_opa), len(c_semi), 0,
                             vo, oo, so)

    out = bytearray(struct.pack(">4sbbHHH2I", b"SHW1", ipd.cell_x, ipd.cell_z,
                                len(buffers), len(instances), len(refs),
                                instances_off, refs_off))
    assert len(out) == 0x14, len(out)
    out += table
    out += blobs
    return bytes(out)


def encode_sht(payloads, pal_table):
    """SHT1 layout (big-endian):
      0x00 u32 'SHT1'; u16 tileCount; u16 palCount;
      0x08 tiles: { u8 fmt(0=CI4,1=CI8); u8 pad; u16 w, h; u16 pad2;
                    u32 pixOff; u32 pixLen; }  (16 B each)
      then pals: { u16 words; u16 pad; u32 off; }  (8 B each)
      data: pixels row-major packed; palettes RGBA16.
    Pixel tiles are palette-free; a draw pairs (tile, palette) at will.
    """
    meta = bytearray()
    pmeta = bytearray()
    data = bytearray()
    base = 8 + 16 * len(payloads) + 8 * len(pal_table)

    def add(dat):
        nonlocal data
        off = base + len(data)
        data += dat
        while len(data) & 7:
            data += b"\x00"
        return off

    for fmt, w, h, pix in payloads:
        pix_off = add(pix)
        meta += struct.pack(">2B3H2I", fmt, 0, w, h, 0, pix_off, len(pix))
    for pdat in pal_table:
        off = add(pdat)
        pmeta += struct.pack(">2HI", len(pdat) // 2, 0, off)

    out = struct.pack(">4sHH", b"SHT1", len(payloads), len(pal_table))
    return out + bytes(meta) + bytes(pmeta) + bytes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ipd-dir", required=True)
    ap.add_argument("--tim-dir", required=True, action="append",
                    help="texture dir(s), searched in order (repeatable: the shared "
                         "item models texture from TIM/BG_ETC.TIM, not the area dir)")
    ap.add_argument("--items", default=None,
                    help="BG/BG_ITEM.PLM: bake the shared item-pickup models (ammo, health, "
                         "shells...) as an objects-only <PREFIX>ITEM.SHW pseudo-cell so "
                         "global-pool (lmIdx 2) world objects draw natively with Z")
    ap.add_argument("--out", required=True)
    ap.add_argument("--glb", default=None,
                    help="area *_GLB.PLM: resolve+bake global-PLM instances "
                         "(shared walls/counter/props) natively into the cells")
    ap.add_argument("prefixes", nargs="+")
    a = ap.parse_args()

    outdir = os.path.join(a.out, "N64W")
    os.makedirs(outdir, exist_ok=True)

    glb_lm = Lm.parse(open(a.glb, "rb").read()) if a.glb else None

    for prefix in a.prefixes:
        prefix = prefix.upper()
        stats = defaultdict(int)
        plm_names = set()
        tims = {}
        missing = set()

        def tim_get(name):
            if name not in tims:
                tims[name] = None
                for d in a.tim_dir:
                    path = os.path.join(d, name + ".TIM")
                    if os.path.exists(path):
                        tims[name] = Tim.parse(open(path, "rb").read())
                        break
                if tims[name] is None:
                    missing.add(name)
            return tims[name]

        cells = sorted(f for f in os.listdir(a.ipd_dir)
                       if f.upper().startswith(prefix) and f.upper().endswith(".IPD"))
        parsed = []
        pieces_by_key = defaultdict(list)
        untex = []
        for ci, fn in enumerate(cells):
            ipd = Ipd.parse(open(os.path.join(a.ipd_dir, fn), "rb").read())
            instances = collect_cell(ipd, ci, tim_get, pieces_by_key, untex, stats,
                                     glb_lm=glb_lm, plm_names=plm_names)
            obj_names = collect_objects(ipd, ci, len(ipd.model_buffers), instances,
                                        tim_get, pieces_by_key, untex, stats)
            parsed.append((fn, ipd, len(ipd.model_buffers) + len(obj_names),
                           instances, obj_names))

        # Shared item-pickup models as an objects-only pseudo-cell (its own
        # SHW, tiles in this area's SHT). Written as <PREFIX>ITEM.SHW.
        if a.items:
            item_lm  = Lm.parse(open(a.items, "rb").read())
            item_ins = []
            item_nms = collect_item_models(item_lm, len(cells), tim_get, pieces_by_key,
                                           untex, item_ins, stats)
            parsed.append((prefix + "ITEM", _ItemShim(), len(item_nms), item_ins, item_nms))

        tiles, pal_table = assign_tiles(pieces_by_key, tims, stats)
        payloads = bake_tiles(tiles, tims, stats)
        sht = encode_sht(payloads, pal_table)
        open(os.path.join(outdir, prefix + ".SHT"), "wb").write(sht)

        # regroup pieces per cell
        per_cell = defaultdict(list)
        for plist in pieces_by_key.values():
            for pc in plist:
                per_cell[pc.cell].append(pc)
                stats["tris"] += 1
        for pc in untex:
            per_cell[pc.cell].append(pc)
            stats["tris"] += 1

        shw_total = 0
        for ci, (fn, ipd, buffer_count, instances, obj_names) in enumerate(parsed):
            shw = encode_shw(ipd, buffer_count, per_cell.get(ci, []), instances)
            if obj_names:
                # OBJ1 trailer: the LAST len(obj_names) buffers/instances are
                # world objects; their 8-char names follow the SHW body and the
                # 12-byte trailer (magic, count, pad, namesOff) closes the file.
                # A trailer instead of a header field keeps SHW1 byte-identical
                # for the loader; an old file simply has no trailer.
                names_off = len(shw)
                blob = bytearray()
                for nm in obj_names:
                    raw = nm.encode("ascii", "replace")[:8]
                    blob += raw + b"\x00" * (8 - len(raw))
                shw = shw + bytes(blob) + struct.pack(">4sHHI", b"OBJ1", len(obj_names), 0, names_off)
            name = os.path.splitext(fn)[0].upper() + ".SHW"
            open(os.path.join(outdir, name), "wb").write(shw)
            shw_total += len(shw)

        print(f"[{prefix}] cells={len(parsed)} tris={stats['tris']} "
              f"splitClipped={stats['split']} tiles={len(tiles)} "
              f"unique={len(payloads)} (dedup {stats['tile_dedup']}) "
              f"pals={len(pal_table)} "
              f"sht={len(sht)//1024}KB shwTotal={shw_total//1024}KB")
        if glb_lm is not None:
            print(f"    PLM: baked={stats['plm_baked']} unresolved={stats['skip_plm']} "
                  f"(baked names: {sorted(plm_names)[:12]}"
                  f"{' ...' if len(plm_names) > 12 else ''})")
        for k in ("skip_plm", "skip_missing_model", "degenerate", "missing_tim",
                  "bad_clut_row"):
            if stats[k]:
                print(f"    {k}={stats[k]}")
        if missing:
            print(f"    missing TIMs: {sorted(missing)[:10]}"
                  f"{' ...' if len(missing) > 10 else ''}")


if __name__ == "__main__":
    main()
