#!/usr/bin/env python3
"""mkplm.py - bake a Silent Hill global-PLM archive (BG/<AREA>_GLB.PLM) into the
native renderer's format. PLM models are the shared placed props (walls, the
reception counter, furniture, pipes) that mkworld SKIPS (info.is_global_plm);
cells reference them BY NAME through the global LM pool (g_Map.globalLm). mkworld
only bakes cell-LOCAL models, so PLMs fall to the PSX per-prim path (2D, no Z,
independent projection) -> they drift and clip through walls.

Output (per area):
    <out>/N64W/<AREA>_GLB.SHP    name-keyed native model streams (one "buffer"
                                 per PLM model, + an 8-char name table)
    <out>/N64W/<AREA>_GLB.SHT    shared tiles (same encoder as mkworld)

A .PLM file is the LM archive at offset 0 (sh1fmt.lm parses it). PLM models are
static props: prims index their own mesh's verts (no shared scratch pool like
characters), so the world encoder applies directly.

    python mkplm.py --plm assets/USA/BG/DR_GLB.PLM --tim-dir <tim> --out build
"""
import argparse
import os
import struct
from collections import defaultdict

from sh1fmt.lm import Lm
from sh1fmt.tim import Tim
from mkworld import (Piece, fan, prim_corner_order, assign_tiles, bake_tiles,
                     encode_sht, encode_buffer_cmds)


def collect_models(lm, tim_get, pieces_by_key, untex, stats):
    """One Piece list, buf == model index (each PLM model is its own buffer).
    Returns the ordered list of 8-char model names for the name table."""
    names = []
    for mi, model in enumerate(lm.models):
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
                    pc = Piece(0, mi, 0, prim.is_transparent, tri, key)
                    (untex if key is None else pieces_by_key[key]).append(pc)
    return names


def encode_shp(names, model_pieces):
    """SHP1 layout (big-endian) -- SHW1 with a name table and no instances:
      0x00 u32 'SHP1'
      0x04 u16 modelCount, u16 tileRefCount
      0x08 u32 tileRefsOff, u32 namesOff
      0x10 modelCount * { u16 vertCount; u16 opaWords; u16 semiWords; u16 pad;
                          u32 vertOff; u32 opaOff; u32 semiOff; }   (20 B)
      tileRefs: u16 final tile indices used (load/refcount, same as SHW1)
      names:    modelCount * char[8]  (model name, '\0'-padded; the cell
                instance's info.name is the lookup key)
      verts/cmds blobs: identical to SHW1.
    """
    per_buf = defaultdict(lambda: ([], []))   # model -> (opa, semi)
    tile_refs = set()
    for pc in model_pieces:
        per_buf[pc.buf][1 if pc.semi else 0].append(pc)
        if pc.tile is not None:
            tile_refs.add(pc.tile.final)

    def sort_key(pc):
        return (0 if pc.tile is None else pc.tile.final + 1, pc.pal, pc.inst)

    buffers = []
    for mi in range(len(names)):
        opa, semi = per_buf.get(mi, ([], []))
        opa.sort(key=sort_key)
        semi.sort(key=sort_key)
        verts = []
        c_opa = encode_buffer_cmds(opa, verts)
        c_semi = encode_buffer_cmds(semi, verts)
        if len(verts) & 1:   # runtime rebases per buffer; keep verts even
            verts.append(verts[-1] if verts else (0, 0, 0, 0, 0, 0, 0, 0, 0))
        buffers.append((c_opa, c_semi, verts))

    refs = sorted(tile_refs)
    base = 0x10 + 20 * len(buffers)
    blobs = bytearray()

    def blob(data):
        nonlocal blobs
        off = base + len(blobs)
        blobs += data
        while len(blobs) & 3:
            blobs += b"\x00"
        return off

    refs_off = blob(struct.pack(">%dH" % len(refs), *refs) if refs else b"")
    name_data = bytearray()
    for nm in names:
        raw = nm.encode("ascii", "replace")[:8]
        name_data += raw + b"\x00" * (8 - len(raw))
    names_off = blob(bytes(name_data))

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

    out = bytearray(struct.pack(">4sHH2I", b"SHP1", len(buffers), len(refs),
                                refs_off, names_off))
    assert len(out) == 0x10, len(out)
    out += table
    out += blobs
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plm", required=True, help="BG/<AREA>_GLB.PLM")
    ap.add_argument("--tim-dir", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    area = os.path.splitext(os.path.basename(a.plm))[0].upper()   # e.g. DR_GLB
    lm = Lm.parse(open(a.plm, "rb").read())

    tims = {}
    missing = set()

    def tim_get(name):
        if name not in tims:
            path = os.path.join(a.tim_dir, name + ".TIM")
            if os.path.exists(path):
                tims[name] = Tim.parse(open(path, "rb").read())
            else:
                tims[name] = None
                missing.add(name)
        return tims[name]

    stats = defaultdict(int)
    pieces_by_key = defaultdict(list)
    untex = []
    names = collect_models(lm, tim_get, pieces_by_key, untex, stats)

    tiles, pal_table = assign_tiles(pieces_by_key, tims, stats)
    payloads = bake_tiles(tiles, tims, stats)
    sht = encode_sht(payloads, pal_table)

    all_pieces = [pc for plist in pieces_by_key.values() for pc in plist] + untex
    stats["tris"] = len(all_pieces)
    shp = encode_shp(names, all_pieces)

    outdir = os.path.join(a.out, "N64W")
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, area + ".SHP"), "wb").write(shp)
    open(os.path.join(outdir, area + ".SHT"), "wb").write(sht)

    print(f"[{area}] models={len(names)} tris={stats['tris']} "
          f"tiles={len(tiles)} unique={len(payloads)} pals={len(pal_table)} "
          f"sht={len(sht)//1024}KB shp={len(shp)//1024}KB")
    for k in ("degenerate", "missing_tim", "bad_clut_row"):
        if stats[k]:
            print(f"    {k}={stats[k]}")
    if missing:
        print(f"    missing TIMs: {sorted(missing)[:10]}"
              f"{' ...' if len(missing) > 10 else ''}")


if __name__ == "__main__":
    main()
