#!/usr/bin/env python3
"""mkchara.py - convert a Silent Hill character .ILM into the native renderer's
SHW/SHT (the same formats mkworld.py emits for the world).

A character .ILM is an LM archive whose "models" are RIGID BODY PARTS, one per
bone (HERO.ILM = 23 parts). There is NO per-vertex skinning -- each part is a
rigid mesh transformed by its bone's matrix at runtime.

THE VERTEX POOL (the thing that makes characters different from the world):
primitive vertex indices are NOT local to their own mesh. func_8005759C copies
each part's vertices into a per-character scratch pool (screenXy_0) at
ModelHeader.vertexOffset; parts replay in LmHeader.modelOrder and their pool
ranges OVERLAP ON PURPOSE, so a part reads vertices an earlier part deposited
wherever they meet -- that is how joint seams weld. The pool holds each vertex
already transformed by its OWNER's bone matrix, so a seam vertex read by part A
but owned by part B must be transformed by B's matrix, not A's. (Resolving it to
A's mesh, or to A's matrix, is what produced "the spikes.")

So each part's buffer loads the verts IT references GROUPED BY OWNER: one
OP_MATRIX(owner)+OP_VERTS per owner, accumulating into one cache window, then
OP_TRIS. Max distinct verts a HERO part references = 51 (fits the 70 cache).

Output: <out>/N64C/<NAME>.SHW / .SHT
    python mkchara.py --ilm HERO.ILM --tim HERO.TIM --out build/chara
"""
import argparse
import os
from collections import defaultdict, OrderedDict

from sh1fmt.lm import Lm
from sh1fmt.tim import Tim
import mkworld
from mkworld import (Piece, fan, prim_corner_order, assign_tiles, bake_tiles,
                     encode_shw, encode_sht, op, OP_TILE, OP_MATRIX, OP_VERTS,
                     OP_TRIS, OP_END, VERT_WINDOW)


class _Inst:
    """Rest-pose placeholder: the runtime replaces every part's matrix each frame
    from the live skeleton (ShT3d_CharaBone), so what is baked here is unused."""
    __slots__ = ("rot", "trans")
    def __init__(self):
        self.rot = [[4096, 0, 0], [0, 4096, 0], [0, 0, 4096]]
        self.trans = [0, 0, 0]


class _IpdShim:
    cell_x = -128
    cell_z = -128


def resolve_pool(lm):
    """Replay the shared vertex pool in modelOrder. Returns, per model index, a
    list of resolved prims: (prim, [(x, y, z, owner) per corner]). A corner whose
    global index is not yet in the pool at this part's turn is dropped (never
    happens for a well-formed character -- the draw order guarantees deposits
    precede reads)."""
    order = list(lm.model_order)
    if sorted(order) != list(range(len(lm.models))):
        order = list(range(len(lm.models)))
    vpool = {}   # global index -> (x, y, z, owner)
    out = {}     # model index -> [(prim, corners)]
    for mi in order:
        model = lm.models[mi]
        for mesh in model.meshes:
            for j, v in enumerate(mesh.verts):
                vpool[model.vertex_offset + j] = (v[0], v[1], v[2], mi)
        for mesh in model.meshes:
            for prim in mesh.prims:
                cor = prim_corner_order(prim)
                if len(cor) < 3:
                    continue
                pts = []
                ok = True
                for c in cor:
                    rec = vpool.get(prim.vi[c])
                    if rec is None:
                        ok = False
                        break
                    pts.append((rec, c))
                if ok:
                    out.setdefault(mi, []).append((prim, pts))
    return out


def collect_parts(lm, tim_get, pieces_by_key, untex, stats):
    """One Piece list, buf == part index. Each vertex tuple carries its OWNER as
    a 6th field: (x, y, z, u, v, owner)."""
    resolved = resolve_pool(lm)
    instances = []
    for part_idx in range(len(lm.models)):
        instances.append(_Inst())
        for (prim, pts) in resolved.get(part_idx, []):
            poly = []
            for ((x, y, z, owner), c) in pts:
                u, v = prim.uv[c]
                poly.append((float(x), float(y), float(z), float(u), float(v), owner))
            key = None
            if prim.material_idx >= 0:
                mat = lm.materials[prim.material_idx]
                if tim_get(mat.name) is not None:
                    key = (mat.name, prim.clut // 64)
                else:
                    stats["missing_tim"] += 1
            for tri in fan(poly):
                pc = Piece(0, part_idx, part_idx, prim.is_transparent, tri, key)
                (untex if key is None else pieces_by_key[key]).append(pc)
    return instances


def _ckey(p):
    """Vertex identity for dedup: coord + uv + OWNER (two verts at the same place
    owned by different bones are DISTINCT -- different matrix)."""
    return (round(p[0] * 2), round(p[1] * 2), round(p[2] * 2),
            round(p[3] * 32), round(p[4] * 32), p[5])


def _window_slots(groups):
    """Cache slots a window occupies: each owner group is even-padded (t3d loads
    vertex PAIRS), so a group of n verts costs (n+1)&~1 slots."""
    return sum((len(g) + 1) & ~1 for g in groups.values())


def encode_buffer_cmds_chara(pieces, verts_out):
    """Character buffer encoder: within each tile window, load the referenced
    verts GROUPED BY OWNER -- OP_MATRIX(owner) + OP_VERTS per owner, accumulating
    into one cache window (the runtime's vertFill) -- then OP_TRIS. Seam verts
    thus land transformed by their owner's bone matrix.

    Windows are capped at VERT_WINDOW (64) like the world's, NOT the cache's 70:
    the RSP VERT_BUFFER is exactly 70*36 bytes and sits directly before
    CLIP_BUFFER_TMP in DMEM, and a 70-vert load (the original-ILM head face)
    corrupted it -> garbage verts then an RSP crash. A tile group that needs
    more is split into several windows under ONE OP_TILE: the tile stays bound
    (no re-upload) and the runtime resets vertFill after each OP_TRIS."""
    cmds = []
    by_tile = OrderedDict()
    for pc in pieces:
        tkey = (0, 0) if pc.tile is None else (pc.tile.final + 1, pc.pal)
        by_tile.setdefault(tkey, []).append(pc)

    def flush_window(tile, groups, wtris):
        u0 = tile.u0 if tile else 0
        v0 = tile.v0 if tile else 0
        # cache index of every vert; each owner group padded to an EVEN size
        cidx = {}
        ci = 0
        for owner, g in groups.items():
            for k in g:
                cidx[k] = ci
                ci += 1
            if len(g) & 1:
                ci += 1
        if ci > VERT_WINDOW:
            raise ValueError("char window %d slots > VERT_WINDOW %d" % (ci, VERT_WINDOW))
        for owner, g in groups.items():
            if len(verts_out) & 1:
                verts_out.append(verts_out[-1] if verts_out else
                                 (0, 0, 0, 0, 0, 0, 0, 0, 0))
            base = len(verts_out)
            for k, p in g.items():
                s = p[3] - u0
                t = p[4] - v0
                verts_out.append((int(round(p[0])), int(round(p[1])),
                                  int(round(p[2])), 128, 128, 128, 255, s, t))
            cnt = len(g)
            if cnt & 1:                 # pad the group to an even vert count
                verts_out.append(verts_out[-1])
                cnt += 1
            cmds.append(op(OP_MATRIX, owner))
            cmds.append(op(OP_VERTS, cnt))
            cmds.append(base)
        tris = []
        for pc in wtris:
            idx = [cidx[_ckey(p)] for p in pc.verts]
            # REVERSED winding (idx[0], k+2, k+1): t3d's viewport uses an axis-flip
            # camera (up=-Y), which inverts screen-space winding for all geometry.
            # The world draws un-culled so it never noticed; the character is the
            # only CULL_BACK geometry, so its front faces must be emitted reversed
            # or standard back-cull removes the NEAR side (Harry renders inside-out).
            for k in range(len(idx) - 2):
                tris.append((idx[0], idx[k + 2], idx[k + 1]))
        cmds.append(op(OP_TRIS, len(tris)))
        packed = []
        for t in tris:
            packed += list(t)
        if len(packed) & 1:
            packed.append(0)
        for i in range(0, len(packed), 2):
            cmds.append((packed[i] << 8) | packed[i + 1])

    for tkey, tpieces in by_tile.items():
        tile = tpieces[0].tile
        cmds.append(op(OP_TILE, tkey[1]))
        cmds.append(tkey[0])
        groups = OrderedDict()          # owner -> OrderedDict(ckey -> vert)
        wtris = []
        for pc in tpieces:
            trial = OrderedDict((o, OrderedDict(g)) for o, g in groups.items())
            for p in pc.verts:
                trial.setdefault(p[5], OrderedDict()).setdefault(_ckey(p), p)
            if _window_slots(trial) > VERT_WINDOW and wtris:
                flush_window(tile, groups, wtris)
                groups = OrderedDict()
                wtris = []
                for p in pc.verts:
                    groups.setdefault(p[5], OrderedDict()).setdefault(_ckey(p), p)
            else:
                groups = trial
            wtris.append(pc)
        if wtris:
            flush_window(tile, groups, wtris)

    cmds.append(op(OP_END, 0))
    return cmds


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
    shw = encode_shw(_IpdShim(), len(instances), all_pieces, instances,
                     buf_encoder=encode_buffer_cmds_chara)

    outdir = os.path.join(a.out, "N64C")
    os.makedirs(outdir, exist_ok=True)
    open(os.path.join(outdir, name + ".SHW"), "wb").write(shw)
    open(os.path.join(outdir, name + ".SHT"), "wb").write(sht)

    print(f"[{name}] parts={len(lm.models)} tris={stats['tris']} "
          f"tiles={len(tiles)} uniqueTiles={len(payloads)} pals={len(pal_table)} "
          f"sht={len(sht)//1024}KB shw={len(shw)//1024}KB")
    for k in ("degenerate", "missing_tim", "unresolved"):
        if stats[k]:
            print(f"    {k}={stats[k]}")


if __name__ == "__main__":
    main()
