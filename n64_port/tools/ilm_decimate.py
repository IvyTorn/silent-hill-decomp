#!/usr/bin/env python3
"""ilm_decimate.py - reverse-tessellate Silent Hill 1 character models (.ILM).

    ilm_decimate.py --analyze <CHARA dir or one .ILM> [--drop-below 0.02]
    ilm_decimate.py <CHARA dir or one .ILM> --out <outdir> [--drop-below 0.02]

An .ILM is the same LM archive the map chunks embed (magic 0x30, version 6) --
its "models" are rigid body parts rather than pieces of a room, one per bone.
So the primitive merge and small-primitive drop from ipd_decimate.py apply
unchanged, and this tool is those two passes plus a different way of writing
the file back.

THE WRITE IS IN PLACE AND SIZE-INVARIANT. Only the count byte in each mesh
header and the primitive array itself change; every offset in the file keeps
its value, the vertex/normal/lighting pools are untouched, and the tail of the
old primitive array is left where it lies as dead bytes nothing reads. A
decimated ILM is therefore byte-for-byte the original except inside those two
regions, which is the safest possible edit to a format whose rig fields are
copied through rather than understood.

The part list IS the rig: every `o` object of the OBJ exporter, every model
here, is bound to one bone. Parts are never added, removed or renamed -- only
the polygons inside them get fewer. A part is never emptied either, so no bone
loses its geometry.

Character cost on the N64 is per primitive like everything else: the profile
puts one character at ~24 ms of a ~233 ms frame, all of it transform and
emit. HERO.ILM is 464 primitives across 23 parts.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh1fmt.lm import Lm                                    # noqa: E402
from ipd_decimate import merge_mesh, drop_small, Poly       # noqa: E402

MIN_PRIMS_PER_PART = 1     # never empty a bone's part


def mesh_header_offsets(data, base=0):
    """[(model_index, mesh_index, absolute offset of the 24-byte mesh header)]."""
    model_count = data[base + 8]
    model_hdrs_off = struct.unpack_from("<I", data, base + 0x0C)[0]
    out = []
    for i in range(model_count):
        off = base + model_hdrs_off + i * 16
        mesh_count = data[off + 8]
        mesh_hdrs_off = struct.unpack_from("<I", data, off + 12)[0]
        for j in range(mesh_count):
            out.append((i, j, base + mesh_hdrs_off + j * 24))
    return out


def pack_prim(p):
    """The 20-byte s_Primitive, exactly as sh1fmt's IPD writer lays it out."""
    field6 = (p.tpage & 0xFF) | ((p.material_idx & 0x7F) << 8) \
        | ((p.is_transparent & 1) << 15)
    return struct.pack("<BBHBBHBBBB",
                       p.uv[0][0], p.uv[0][1], p.clut,
                       p.uv[1][0], p.uv[1][1], field6,
                       p.uv[2][0], p.uv[2][1],
                       p.uv[3][0], p.uv[3][1]) + bytes(p.vi) + bytes(p.li)


def vertex_view(model, mesh):
    """A list the mesh's primitive indices can address directly.

    ILM primitive indices are GLOBAL positions in the per-model transform
    scratch pool: this mesh owns the window [vertexOffset, vertexOffset+n), and
    an index outside it names a vertex belonging to an ADJACENT BODY PART --
    the shared slots that weld the seam at a joint. Those hold the neighbour's
    already-transformed vertex, which does not exist until the skeleton runs,
    so they are represented as None and every primitive touching one is passed
    through unchanged. (Map models index their own pool from zero, and this
    returns it verbatim.)"""
    n = len(mesh.verts)
    bias = model.vertex_offset
    hi = 0
    for prim in mesh.prims:
        for vi in prim.vi:
            if vi != 0xFF and vi > hi:
                hi = vi
    if not bias and hi < n:
        return list(mesh.verts)
    view = [None] * max(hi + 1, bias + n)
    for k in range(n):
        view[bias + k] = mesh.verts[k]
    return view


def process(path, out_dir, args, stats):
    data = bytearray(open(path, "rb").read())
    if data[0] != 0x30 or data[1] != 6:
        stats["skipped"] += 1
        return
    lm = Lm.parse(bytes(data), 0)
    hdrs = mesh_header_offsets(bytes(data), 0)
    name = os.path.basename(path)
    before = after = 0
    changed = False

    for (mi, mj, mh_off) in hdrs:
        model = lm.models[mi]
        mesh = model.meshes[mj]
        before += len(mesh.prims)
        verts = vertex_view(model, mesh)
        new_prims, _m = merge_mesh_with(mesh, verts, args)
        if args.drop_below > 0 and len(new_prims) > MIN_PRIMS_PER_PART:
            dropped = drop_small(new_prims, verts, args.drop_below * 65536.0, stats)
            if len(dropped) >= MIN_PRIMS_PER_PART:
                new_prims = dropped
        after += len(new_prims)

        if len(new_prims) != len(mesh.prims):
            if len(new_prims) > 255:
                stats["unresolved"] += 1
                continue
            blob = b"".join(pack_prim(p) for p in new_prims)
            start = mesh.prims_off                 # absolute; unchanged
            data[start:start + len(blob)] = blob   # tail left as dead bytes
            data[mh_off] = len(new_prims)          # primitiveCount
            changed = True

    stats["files"] += 1
    stats["before"] += before
    stats["after"] += after
    if out_dir is not None and changed:
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, name)
        open(out, "wb").write(bytes(data))
        assert os.path.getsize(out) == len(data), "in-place write changed the size"
        # Re-parse the result: a bad count byte would show up here, not on hardware.
        chk = Lm.parse(bytes(data), 0)
        got = sum(len(ms.prims) for m in chk.models for ms in m.meshes)
        assert got == after, f"{name}: re-parse counted {got}, wrote {after}"
        stats["written"] += 1
    if args.verbose or out_dir is None:
        pct = (100.0 * (before - after) / before) if before else 0.0
        print(f"  {name:14s} {len(lm.models):3d} parts  prims {before:5d} -> {after:5d}  (-{pct:4.1f}%)")


def merge_mesh_with(mesh, verts, args):
    """merge_mesh, but against a vertex list this model's indices can address."""
    saved = mesh.verts
    try:
        mesh.verts = verts
        return merge_mesh(mesh, args.uv_max, args.size_max)
    finally:
        mesh.verts = saved


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help="a CHARA directory or a single .ILM")
    ap.add_argument("--out", help="write decimated ILMs here")
    ap.add_argument("--analyze", action="store_true", help="report only")
    ap.add_argument("--uv-max", type=int, default=64)
    ap.add_argument("--size-max", type=int, default=2048)
    ap.add_argument("--drop-below", type=float, default=0.0, metavar="AREA",
                    help="drop isolated unblended primitives under AREA square world units "
                         "(characters are small: try 0.02)")
    ap.add_argument("--only", help="comma-separated model basenames, e.g. HERO,SIBYL")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    if os.path.isdir(args.path):
        files = [os.path.join(args.path, f) for f in sorted(os.listdir(args.path))
                 if f.upper().endswith(".ILM")]
    else:
        files = [args.path]
    if args.only:
        want = {n.strip().upper() for n in args.only.split(",")}
        files = [f for f in files
                 if os.path.splitext(os.path.basename(f))[0].upper() in want]
    if not files:
        sys.exit("no .ILM files matched")
    out_dir = None if args.analyze else args.out
    if out_dir is None and not args.analyze:
        sys.exit("give --out <dir> or --analyze")

    stats = dict(files=0, before=0, after=0, written=0, skipped=0, unresolved=0,
                 dropped=0, area_dropped=0.0, area_kept=0.0)
    for f in files:
        process(f, out_dir, args, stats)
    b, a = stats["before"], stats["after"]
    pct = (100.0 * (b - a) / b) if b else 0.0
    if args.drop_below > 0:
        tot = stats["area_dropped"] + stats["area_kept"]
        print(f"  drop-below {args.drop_below}: removed {stats['dropped']} primitives = "
              f"{100.0 * stats['area_dropped'] / tot if tot else 0:.2f}% of the surface")
    if stats["unresolved"]:
        print(f"  {stats['unresolved']} mesh(es) left alone (indices did not resolve)")
    print(f"{stats['files']} models: primitives {b} -> {a} (-{pct:.1f}%)"
          + (f"; wrote {stats['written']}" if out_dir else ""))


if __name__ == "__main__":
    main()
