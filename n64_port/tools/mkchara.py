#!/usr/bin/env python3
"""mkchara.py - convert a Silent Hill character .ILM into the native renderer's
SHW/SHT (the same formats mkworld.py emits for the world).

A character .ILM is an LM archive whose "models" are RIGID BODY PARTS, one per
bone (HERO.ILM = 23 parts: chest, head, neck, shoulders, arms, hands, hips,
legs, feet). There is NO per-vertex skinning -- each part is a rigid mesh
transformed by its bone's matrix at runtime. So a part maps 1:1 onto a world
"instance": the geometry is baked here, the per-frame BONE matrix is supplied
at draw time by the runtime (t3d_world.c's compose path, fed from the game's
skeleton instead of the IPD). The instance transform baked here is therefore
IDENTITY -- a rest pose placeholder the runtime overwrites.

Output: <out>/CHARA/<NAME>.SHW  (24 parts as instances, one buffer)
        <out>/CHARA/<NAME>.SHT  (HERO.TIM tiled + its CLUT palettes)

    python mkchara.py --ilm HERO.ILM --tim HERO.TIM --out build/chara

Reuses mkworld's tile assigner/baker and stream encoder verbatim; only the
front-end (parts instead of cells) is new.
"""
import argparse
import os
import struct
from collections import defaultdict

from sh1fmt.lm import Lm
from sh1fmt.tim import Tim
import mkworld
from mkworld import (Piece, fan, prim_corner_order, assign_tiles, bake_tiles,
                     encode_shw, encode_sht)


class _Inst:
    """Rest-pose instance: identity rotation (Q12), zero translation. The
    runtime replaces this with the live bone matrix each frame."""
    __slots__ = ("rot", "trans")
    def __init__(self):
        self.rot = [[4096, 0, 0], [0, 4096, 0], [0, 0, 4096]]
        self.trans = [0, 0, 0]


class _IpdShim:
    """encode_shw only reads .cell_x/.cell_z off the ipd. A character has no
    cell; -128 marks the SHW as a character (the loader keys on it)."""
    cell_x = -128
    cell_z = -128


def collect_parts(lm, tim_get, pieces_by_key, untex, stats):
    """One Piece list, inst = part index (= bone index by ILM order)."""
    instances = []
    for part_idx, model in enumerate(lm.models):
        instances.append(_Inst())
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
                    pc = Piece(0, 0, part_idx, prim.is_transparent, tri, key)
                    (untex if key is None else pieces_by_key[key]).append(pc)
    return instances


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ilm", required=True)
    ap.add_argument("--tim", required=True, help="the character's .TIM (its one material)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    name = os.path.splitext(os.path.basename(a.ilm))[0].upper()
    lm = Lm.parse(open(a.ilm, "rb").read())
    tim = Tim.parse(open(a.tim, "rb").read())
    mat_name = lm.materials[0].name if lm.materials else name
    tims = {mat_name: tim}

    def tim_get(n):
        return tims.get(n)

    stats = defaultdict(int)
    pieces_by_key = defaultdict(list)
    untex = []
    instances = collect_parts(lm, tim_get, pieces_by_key, untex, stats)

    tiles, pal_table = assign_tiles(pieces_by_key, tims, stats)
    payloads = bake_tiles(tiles, tims, stats)
    sht = encode_sht(payloads, pal_table)

    all_pieces = [pc for plist in pieces_by_key.values() for pc in plist] + untex
    stats["tris"] = len(all_pieces)
    shw = encode_shw(_IpdShim(), 1, all_pieces, instances)

    outdir = os.path.join(a.out, "CHARA")
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, name + ".SHW"), "wb").write(shw)
    open(os.path.join(outdir, name + ".SHT"), "wb").write(sht)

    print(f"[{name}] parts={len(lm.models)} tris={stats['tris']} "
          f"tiles={len(tiles)} uniqueTiles={len(payloads)} pals={len(pal_table)} "
          f"sht={len(sht)//1024}KB shw={len(shw)//1024}KB")
    for k in ("degenerate", "missing_tim", "tile_dedup", "split"):
        if stats[k]:
            print(f"    {k}={stats[k]}")


if __name__ == "__main__":
    main()
