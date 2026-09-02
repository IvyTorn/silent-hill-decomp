#!/usr/bin/env python3
"""ipd_decimate.py - reverse-tessellate Silent Hill 1 map chunks for the N64 port.

    ipd_decimate.py --analyze  <dir with BG/>  [--area ER]
    ipd_decimate.py <dir with BG/> --out <outdir>  [--area ER] [--uv-max 64] [--size-max 2048]

Every cost the N64 frame has left is per-primitive: the CPU transforms and
emits each world quad, the OT walk parses each one, and the RDP pays a large
fixed cost per triangle command. The PSX data is heavily subdivided -- walls
and floors are grids of small quads, because the PSX's affine texturing and
per-vertex fog needed the density. The N64 texture-maps with perspective
correction, so that density buys nothing here.

This tool merges neighbouring primitives of one mesh that are coplanar, share
an edge, use the same material/CLUT/blend, and whose texture coordinates form
ONE affine map across the pair (so the merged quad shows exactly the texels
the two did). Two quads become one quad, two triangles become a quad, and the
process repeats until nothing more merges within the caps:

  --uv-max    largest texel extent of a merged quad's UV box. The N64 port
              loads texture windows of ~4096 texels per triangle; a quad wider
              than the window gets split again at draw time, which hands the
              saving straight back to the RDP. 64 keeps every merge inside one
              window.
  --size-max  largest edge of a merged quad, in Q8 (256 = one world unit).
              Fog and lighting are per corner and interpolated across the
              quad, so a very large quad shows the fog ramp as a straight
              gradient instead of the PSX's stepped one.
  --drop-below A
              THE LOSSY STEP, off by default. Removes primitives whose world
              area is under A square units (1 unit = 256 Q8; a door is about
              2x3). Surveyed: in the police station 48% of all primitives are
              under 0.05 u^2 and cover 1.7% of its surface -- screws, trim,
              bevels, wires that are a pixel or two at 320x240 but cost the
              same transform, walk and RDP command as a wall. A primitive is
              only dropped when it is not blended (glows, light sprites) and
              shares no edge with a larger primitive, so a sliver that is a
              piece of a wall's tessellation stays and no hole opens. The
              report prints how much surface the pass removed.

Vertices, normals and lighting slots are left untouched: the runtime reads a
mesh's slot bytes either as vertex indices (dynamic relight) or as baked
brightness (flat copy) depending on the draw path, and a merged primitive
keeps each surviving corner's original slot, so it lights exactly as before.
Model/buffer/draw-table topology is unchanged, and the collision block passes
through byte-for-byte: only the primitive lists shrink. Output files are
smaller than the originals, so they are loose-file safe on every port.

Parsing and serialisation are the validated sh1fmt package from the
TrenchBroom SH1 editor (byte-identical round trip on all 493 retail IPDs),
vendored under n64_port/tools/sh1fmt/.
"""
import argparse
import collections
import math
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh1fmt.ipd import Ipd                      # noqa: E402
from sh1fmt.lm import Primitive                 # noqa: E402
from sh1fmt.writer import write_ipd             # noqa: E402

PERIM = (0, 1, 3, 2)          # PSX strip order (0,1,2,3) -> perimeter order
EPS_PLANAR = 0.5              # Q8 distance from the plane
EPS_UV = 1.0                  # texels
EPS_COLLINEAR = 0.5           # Q8, distance of a shared vertex from the outer edge
REJ = collections.Counter()   # --analyze: why candidate pairs did not merge


# ---------------------------------------------------------------- vector helpers
def v_sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def v_dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def v_cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def v_len(a): return math.sqrt(v_dot(a, a))


def solve3(m, rhs):
    """3x3 linear solve by Cramer; None if singular."""
    a, b, c = m
    det = (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
           + a[2] * (b[0] * c[1] - b[1] * c[0]))
    if abs(det) < 1e-9:
        return None
    out = []
    for col in range(3):
        mm = [list(a), list(b), list(c)]
        for r in range(3):
            mm[r][col] = rhs[r]
        d = (mm[0][0] * (mm[1][1] * mm[2][2] - mm[1][2] * mm[2][1])
             - mm[0][1] * (mm[1][0] * mm[2][2] - mm[1][2] * mm[2][0])
             + mm[0][2] * (mm[1][0] * mm[2][1] - mm[1][1] * mm[2][0]))
        out.append(d / det)
    return tuple(out)


def uv_axes(pts, uvs, n):
    """Affine texture map u = ua.p + uo, v = va.p + vo from three corners,
    with the axes constrained to the plane (ua.n = 0)."""
    e1, e2 = v_sub(pts[1], pts[0]), v_sub(pts[2], pts[0])
    m = (e1, e2, n)
    ua = solve3(m, (uvs[1][0] - uvs[0][0], uvs[2][0] - uvs[0][0], 0.0))
    va = solve3(m, (uvs[1][1] - uvs[0][1], uvs[2][1] - uvs[0][1], 0.0))
    if ua is None or va is None:
        return None
    return ua, uvs[0][0] - v_dot(ua, pts[0]), va, uvs[0][1] - v_dot(va, pts[0])


def uv_at(axes, p):
    ua, uo, va, vo = axes
    return v_dot(ua, p) + uo, v_dot(va, p) + vo


# ---------------------------------------------------------------- polygons
class Poly:
    """One primitive as a perimeter polygon: per corner (vertex index,
    lighting-slot index, uv) plus the prim's material key."""
    __slots__ = ("vi", "li", "uv", "pos", "key", "n", "d", "src")

    def __init__(self, prim, verts, src):
        seen, cs = set(), []
        for c in PERIM:
            v = prim.vi[c]
            if v != 0xFF and v not in seen:
                seen.add(v)
                cs.append(c)
        self.vi = [prim.vi[c] for c in cs]
        self.li = [prim.li[c] for c in cs]
        self.uv = [prim.uv[c] for c in cs]
        # A vertex index that does not resolve to this mesh's own pool means the
        # primitive reads a NEIGHBOUR's already-transformed vertex -- that is how
        # character parts weld their seams, and its world position does not exist
        # until the skeleton runs. Such a primitive cannot be evaluated here, so
        # it is left with no geometry and every caller passes it through intact.
        self.pos = []
        for v in self.vi:
            q = verts[v] if 0 <= v < len(verts) else None
            if q is None:
                self.pos = []
                break
            self.pos.append(q)
        self.key = (prim.material_idx, prim.clut, prim.is_transparent, prim.tpage)
        self.src = src
        self.n = None
        self.d = 0.0
        if len(self.pos) >= 3:
            n = v_cross(v_sub(self.pos[1], self.pos[0]), v_sub(self.pos[2], self.pos[0]))
            ln = v_len(n)
            if ln > 1e-6:
                self.n = (n[0] / ln, n[1] / ln, n[2] / ln)
                self.d = v_dot(self.n, self.pos[0])

    def area(self):
        k = len(self.pos)
        if k < 3:
            return 0.0
        a = (0.0, 0.0, 0.0)
        for t in range(1, k - 1):
            c = v_cross(v_sub(self.pos[t], self.pos[0]), v_sub(self.pos[t + 1], self.pos[0]))
            a = (a[0] + c[0], a[1] + c[1], a[2] + c[2])
        return 0.5 * v_len(a)

    def planar(self):
        if self.n is None:
            return False
        return all(abs(v_dot(self.n, p) - self.d) <= EPS_PLANAR for p in self.pos)

    def to_prim(self):
        k = len(self.vi)
        if k == 4:
            vi = (self.vi[0], self.vi[1], self.vi[3], self.vi[2])
            li = (self.li[0], self.li[1], self.li[3], self.li[2])
            uv = (self.uv[0], self.uv[1], self.uv[3], self.uv[2])
        else:
            vi = (self.vi[0], self.vi[1], self.vi[2], self.vi[2])
            li = (self.li[0], self.li[1], self.li[2], self.li[2])
            uv = (self.uv[0], self.uv[1], self.uv[2], self.uv[2])
        mat, clut, transp, tpage = self.key
        return Primitive(uv, clut, tpage, mat, transp, vi, li, 0)


def convex(ring, n):
    k = len(ring)
    sign = 0
    for i in range(k):
        a, b, c = ring[i], ring[(i + 1) % k], ring[(i + 2) % k]
        s = v_dot(v_cross(v_sub(b, a), v_sub(c, b)), n)
        if abs(s) < 1e-9:
            continue
        if sign == 0:
            sign = 1 if s > 0 else -1
        elif (s > 0) != (sign > 0):
            return False
    return sign != 0


def point_on_segment(p, a, b):
    """p lies on segment ab (within EPS_COLLINEAR), strictly between the ends."""
    ab = v_sub(b, a)
    l2 = v_dot(ab, ab)
    if l2 < 1e-9:
        return False
    t = v_dot(v_sub(p, a), ab) / l2
    if t <= 0.02 or t >= 0.98:
        return False
    proj = (a[0] + ab[0] * t, a[1] + ab[1] * t, a[2] + ab[2] * t)
    return v_len(v_sub(p, proj)) <= EPS_COLLINEAR


def classify_tiling(A, B, axes):
    """A non-affine pair is 'tiled' when B's UVs equal A's affine extension
    shifted by whole tiles: the same power-of-two period P on every corner
    (per axis), A's own UV box inside one P-wide tile, and the extended UVs
    still a byte. Such a pair CAN merge on the N64 with the texture rect
    loaded once and the RDP masking (wrapping) at P."""
    best = None
    for P in (8, 16, 32, 64, 128):
        okp = True
        ext_u, ext_v = [], []
        for p, uv in zip(B.pos, B.uv):
            u, v = uv_at(axes, p)
            du, dv = u - uv[0], v - uv[1]
            if abs(du - P * round(du / P)) > EPS_UV or abs(dv - P * round(dv / P)) > EPS_UV:
                okp = False
                break
            ext_u.append(u); ext_v.append(v)
        if not okp:
            continue
        au = [x[0] for x in A.uv]; av = [x[1] for x in A.uv]
        # A's box must sit inside one tile in the axis that repeats
        rep_u = any(abs(eu - bu[0]) > EPS_UV for eu, bu in zip(ext_u, B.uv))
        rep_v = any(abs(ev - bv[1]) > EPS_UV for ev, bv in zip(ext_v, B.uv))
        if rep_u and (min(au) // P) != ((max(au) - 0.01) // P):
            continue
        if rep_v and (min(av) // P) != ((max(av) - 0.01) // P):
            continue
        allu = au + ext_u; allv = av + ext_v
        if min(allu) < -EPS_UV or max(allu) > 255 + EPS_UV or min(allv) < -EPS_UV or max(allv) > 255 + EPS_UV:
            return f"tiled, period {P}, but extended UV exceeds a byte"
        best = P
        break
    if best is None:
        for P in (8, 16, 32, 64, 128):
            okm = True
            for p, uv in zip(B.pos, B.uv):
                u, v = uv_at(axes, p)
                # reflect about the nearest multiple of P: u' = 2kP - u
                ru = 2 * P * round((u + uv[0]) / (2 * P)) - u
                rv = 2 * P * round((v + uv[1]) / (2 * P)) - v
                same_u = abs(u - uv[0]) <= EPS_UV or abs(ru - uv[0]) <= EPS_UV
                same_v = abs(v - uv[1]) <= EPS_UV or abs(rv - uv[1]) <= EPS_UV
                if not (same_u and same_v):
                    okm = False
                    break
            if okm:
                return f"mirrored tile, period {P} (N64 mirror-wrap could merge)"
        return "UVs not one affine map (seam / other atlas region)"
    return f"tiled with period {best} (N64 wrap could merge)"


def try_merge(A, B, uv_max, size_max):
    """Merged Poly of A and B, or None."""
    if A.key != B.key or A.n is None or B.n is None:
        return None
    if abs(v_dot(A.n, B.n)) < 0.9995:
        return None
    if any(abs(v_dot(A.n, p) - A.d) > EPS_PLANAR for p in B.pos):
        return None

    ka, kb = len(A.pos), len(B.pos)
    why = None
    # shared edge: consecutive corners of A equal (by position) two corners of B
    for i in range(ka):
        a0, a1 = A.pos[i], A.pos[(i + 1) % ka]
        j0 = next((j for j in range(kb) if B.pos[j] == a0), None)
        j1 = next((j for j in range(kb) if B.pos[j] == a1), None)
        if j0 is None or j1 is None or j0 == j1:
            continue
        if (j0 - j1) % kb != 1 and (j1 - j0) % kb != 1:
            continue           # not an edge of B
        # A's ring starting after the shared edge: a1, ..., a0 (all of A)
        ring_a = [(i + 1 + t) % ka for t in range(ka)]
        # B's corners strictly between a0 and a1, walking away from the edge
        if (j1 - j0) % kb == 1:      # B goes a0 -> a1 along the shared edge
            step = -1
        else:
            step = 1
        between = []
        j = (j0 + step) % kb
        while j != j1:
            between.append(j)
            j = (j + step) % kb
        # union ring (corner refs): A corners, then B's extra corners
        refs = [("A", c) for c in ring_a] + [("B", c) for c in between]
        pts = [A.pos[c] if s == "A" else B.pos[c] for s, c in refs]
        # drop the two shared vertices when they sit on a straight outer edge
        keep = []
        k = len(refs)
        for idx in range(k):
            s, c = refs[idx]
            p = pts[idx]
            is_shared = (s == "A" and c in (i, (i + 1) % ka))
            if is_shared and point_on_segment(p, pts[(idx - 1) % k], pts[(idx + 1) % k]):
                continue
            keep.append(idx)
        if len(keep) not in (3, 4):
            why = "L-shape (shared edge not straight-through)"
            continue
        ring = [pts[idx] for idx in keep]
        if not convex(ring, A.n):
            why = "union not convex"
            continue
        # world-size cap
        if any(v_len(v_sub(ring[t], ring[(t + 1) % len(ring)])) > size_max
               for t in range(len(ring))):
            why = "size-max cap"
            continue
        # one affine UV map must fit every corner of both source prims
        axes = uv_axes(A.pos[:3], A.uv[:3], A.n)
        if axes is None:
            continue
        ok = True
        for P in (A, B):
            for p, uv in zip(P.pos, P.uv):
                u, v = uv_at(axes, p)
                if abs(u - uv[0]) > EPS_UV or abs(v - uv[1]) > EPS_UV:
                    ok = False
                    break
            if not ok:
                break
        if not ok:
            why = classify_tiling(A, B, axes)
            continue
        # merged corner data: each kept corner is an original corner of A or B
        vi, li, uv = [], [], []
        for idx in keep:
            s, c = refs[idx]
            P = A if s == "A" else B
            vi.append(P.vi[c]); li.append(P.li[c]); uv.append(P.uv[c])
        us = [x[0] for x in uv]; vs = [x[1] for x in uv]
        if max(us) - min(us) > uv_max or max(vs) - min(vs) > uv_max:
            why = "uv-max cap"
            continue
        M = Poly.__new__(Poly)
        M.vi, M.li, M.uv, M.pos, M.key = vi, li, uv, ring, A.key
        M.n, M.d = A.n, A.d
        M.src = A.src + B.src
        return M
    if why:
        REJ[why] += 1
    return None


def drop_small(prims, verts, thresh_q8sq, dstats):
    """Remove small, unblended primitives that share no edge with a larger one."""
    polys = [Poly(p, verts, [k]) for k, p in enumerate(prims)]
    areas = [P.area() for P in polys]
    big_edges = set()
    for P, a in zip(polys, areas):
        if a >= thresh_q8sq:
            k = len(P.pos)
            for t in range(k):
                e = frozenset((P.pos[t], P.pos[(t + 1) % k]))
                big_edges.add(e)
    out = []
    for prim, P, a in zip(prims, polys, areas):
        if a < thresh_q8sq and not P.key[2] and len(P.pos) >= 3:
            k = len(P.pos)
            touches = any(frozenset((P.pos[t], P.pos[(t + 1) % k])) in big_edges for t in range(k))
            if not touches:
                dstats["dropped"] += 1
                dstats["area_dropped"] += a
                continue
        dstats["area_kept"] += a
        out.append(prim)
    return out


def merge_mesh(mesh, uv_max, size_max):
    """Returns (new prim list, merges done)."""
    polys, passthrough = [], []
    for k, prim in enumerate(mesh.prims):
        P = Poly(prim, mesh.verts, [k])
        if len(P.pos) in (3, 4) and P.planar():
            polys.append(P)
        else:
            passthrough.append(prim)
    merges = 0
    changed = True
    while changed:
        changed = False
        i = 0
        while i < len(polys):
            j = i + 1
            merged = None
            while j < len(polys):
                merged = try_merge(polys[i], polys[j], uv_max, size_max)
                if merged is not None:
                    break
                j += 1
            if merged is not None:
                polys[i] = merged
                del polys[j]
                merges += 1
                changed = True
            else:
                i += 1
    out = [P.to_prim() for P in polys] + passthrough
    return out, merges


def area_by_key(prims, verts):
    """Summed polygon area per material key, plus the count of degenerate prims."""
    acc = collections.Counter()
    for prim in prims:
        P = Poly(prim, verts, [])
        k = len(P.pos)
        if k < 3:
            continue
        a = (0.0, 0.0, 0.0)
        for t in range(1, k - 1):
            c = v_cross(v_sub(P.pos[t], P.pos[0]), v_sub(P.pos[t + 1], P.pos[0]))
            a = (a[0] + c[0], a[1] + c[1], a[2] + c[2])
        acc[P.key] += 0.5 * v_len(a)
    return acc


def process_file(path, out_dir, args, stats):
    data = open(path, "rb").read()
    ipd = Ipd.parse(data)
    before = after = 0
    merges = 0
    for model in ipd.lm.models:
        for mesh in model.meshes:
            before += len(mesh.prims)
            new_prims, m = merge_mesh(mesh, args.uv_max, args.size_max)
            merges += m
            merged_only = new_prims
            if args.drop_below > 0:
                n0 = len(new_prims)
                new_prims = drop_small(new_prims, mesh.verts, args.drop_below * 65536.0, stats)
                m += n0 - len(new_prims)
            after += len(new_prims)
            if args.verify and merged_only is not mesh.prims and len(merged_only) != len(mesh.prims):
                # the merge step must be shape-exact; the drop step is lossy by design
                a0 = area_by_key(mesh.prims, mesh.verts)
                a1 = area_by_key(merged_only, mesh.verts)
                for key in set(a0) | set(a1):
                    if abs(a0[key] - a1[key]) > 0.002 * max(a0[key], 1.0):
                        stats["verify_fail"] += 1
                        print(f"  VERIFY {os.path.basename(path)} {model.name}: material {key[0]} "
                              f"area {a0[key]:.0f} -> {a1[key]:.0f}")
            if out_dir is not None and m:
                mesh.prims = new_prims
    stats["files"] += 1
    stats["before"] += before
    stats["after"] += after
    name = os.path.basename(path)
    if out_dir is not None:
        if merges:
            body = write_ipd(ipd)
            out = body + bytes((-len(body)) % 256)
            # sanity: the result must parse and hold the primitive count we wrote
            chk = Ipd.parse(out)
            got = sum(len(ms.prims) for mo in chk.lm.models for ms in mo.meshes)
            assert got == after, f"{name}: re-parse prim count {got} != {after}"
            os.makedirs(out_dir, exist_ok=True)
            open(os.path.join(out_dir, name), "wb").write(out)
            stats["written"] += 1
            stats["bytes_before"] += len(data)
            stats["bytes_after"] += len(out)
    if args.verbose or (out_dir is None and merges):
        pct = (100.0 * (before - after) / before) if before else 0.0
        print(f"  {name:14s} prims {before:5d} -> {after:5d}  (-{pct:4.1f}%)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("disc", help="extracted disc dir containing BG/ (or a gamedata/load dir)")
    ap.add_argument("--out", help="write decimated IPDs here (a BG/ dir); omit with --analyze")
    ap.add_argument("--analyze", action="store_true", help="report only, write nothing")
    ap.add_argument("--area", action="append", help="area tag(s) to process, e.g. ER (default all)")
    ap.add_argument("--uv-max", type=int, default=64)
    ap.add_argument("--size-max", type=int, default=2048)
    ap.add_argument("--drop-below", type=float, default=0.0, metavar="AREA",
                    help="drop isolated unblended primitives under AREA square world units (lossy; try 0.05)")
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--verify", action="store_true",
                    help="check that per-material polygon area is unchanged by every merge")
    args = ap.parse_args()

    bg = os.path.join(args.disc, "BG")
    if not os.path.isdir(bg):
        bg = args.disc
    files = sorted(f for f in os.listdir(bg) if f.upper().endswith(".IPD"))
    if args.area:
        # a cell is <TAG> + 4 hex chars + .IPD, so the tag is exact by length
        tags = tuple(a.upper() for a in args.area)
        files = [f for f in files
                 if any(len(f) == len(t) + 8 and f.upper().startswith(t) for t in tags)]
    if not files:
        sys.exit(f"no IPD files matched in {bg}")
    out_dir = None if args.analyze else args.out
    if out_dir is None and not args.analyze:
        sys.exit("give --out <dir> or --analyze")

    stats = dict(files=0, before=0, after=0, written=0, bytes_before=0, bytes_after=0,
                 verify_fail=0, dropped=0, area_dropped=0.0, area_kept=0.0)
    t0 = time.time()
    for f in files:
        process_file(os.path.join(bg, f), out_dir, args, stats)
    dt = time.time() - t0
    b, a = stats["before"], stats["after"]
    pct = (100.0 * (b - a) / b) if b else 0.0
    if args.analyze and REJ:
        print("  shared-edge pairs that did NOT merge, by reason:")
        for why, n in REJ.most_common():
            print(f"    {n:7d}  {why}")
    if args.verify:
        print(f"  verify: {stats['verify_fail']} material/mesh area mismatches")
    if args.drop_below > 0:
        tot = stats["area_dropped"] + stats["area_kept"]
        print(f"  drop-below {args.drop_below}: removed {stats['dropped']} primitives = "
              f"{100.0 * stats['area_dropped'] / tot if tot else 0:.2f}% of the surface")
    print(f"{stats['files']} files: primitives {b} -> {a} (-{pct:.1f}%) in {dt:.1f}s"
          + (f"; wrote {stats['written']} files, {stats['bytes_before']} -> "
             f"{stats['bytes_after']} bytes" if out_dir else ""))


if __name__ == "__main__":
    main()
