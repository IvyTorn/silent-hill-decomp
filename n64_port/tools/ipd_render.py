#!/usr/bin/env python3
"""ipd_render.py - crude software render of one or more IPD cells, to LOOK at
what a rewrite did before anyone burns a ROM.

    ipd_render.py out.png CELL.IPD [CELL2.IPD ...] [--view top|iso|corner] [--size 640x480]
    ipd_render.py cmp.png --compare A/BG B/BG --cell ERFCFC

Flat-filled polygons, one colour per material, painter's order, fog by
depth. Not pretty and not the game's shading; enough to see a missing wall,
a hole, an exploded mesh or a wrong corner order at a glance. --compare
renders the same cell from two directories side by side.
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh1fmt.ipd import Ipd, CELL_Q8   # noqa: E402

from PIL import Image, ImageDraw       # noqa: E402

PERIM = (0, 1, 3, 2)
CULL = 0


def cell_polys(ipd):
    """World-space polygons of every drawn instance: (corners[(x,y,z)], key)."""
    out = []
    cx, cz = ipd.cell_x * CELL_Q8, ipd.cell_z * CELL_Q8
    for buf in ipd.model_buffers:
        for inst in buf.instances:
            info = ipd.model_infos[inst.model_info_idx]
            if info.is_global_plm:
                continue                       # lives in the area's PLM; not in this file
            model = ipd.lm.model_by_name(info.name)
            if model is None:
                continue
            r = inst.rot
            tx, ty, tz = inst.trans[0] + cx, inst.trans[1], inst.trans[2] + cz
            for mesh in model.meshes:
                for prim in mesh.prims:
                    seen, pts = set(), []
                    for c in PERIM:
                        vi = prim.vi[c]
                        if vi == 0xFF or vi in seen or vi >= len(mesh.verts):
                            continue
                        seen.add(vi)
                        x, y, z = mesh.verts[vi]
                        wx = (r[0][0] * x + r[0][1] * y + r[0][2] * z) / 4096.0 + tx
                        wy = (r[1][0] * x + r[1][1] * y + r[1][2] * z) / 4096.0 + ty
                        wz = (r[2][0] * x + r[2][1] * y + r[2][2] * z) / 4096.0 + tz
                        pts.append((wx, wy, wz))
                    if len(pts) >= 3:
                        out.append((pts, prim.material_idx, prim.is_transparent))
    return out


def colour(mat, transp):
    h = (mat * 2654435761) & 0xFFFFFF
    r, g, b = 80 + (h & 0x7F), 80 + ((h >> 8) & 0x7F), 80 + ((h >> 16) & 0x7F)
    if transp:
        r, g, b = 255, 200, 80
    return (r, g, b)


def render(polys, size, view, bounds):
    W, H = size
    img = Image.new("RGB", (W, H), (20, 20, 28))
    dr = ImageDraw.Draw(img)
    (x0, y0, z0), (x1, y1, z1) = bounds
    cxw, czw, cyw = (x0 + x1) / 2, (z0 + z1) / 2, (y0 + y1) / 2
    ext = max(x1 - x0, z1 - z0, y1 - y0, 1.0)

    def proj(p):
        x, y, z = p[0] - cxw, p[1] - cyw, p[2] - czw
        if view == "top":              # +Y is down in SH: look along +Y
            return (W / 2 + x / ext * W * 0.9, H / 2 + z / ext * H * 0.9, y)
        if view == "iso":
            a = math.radians(35)
            sx = (x - z) * math.cos(a)
            sy = (x + z) * math.sin(a) + y
            return (W / 2 + sx / ext * W * 0.6, H / 2 + sy / ext * H * 0.6, x + z - y)
        # corner: perspective from a cell corner, above the floor, looking in
        eye = (x0 - cxw + ext * 0.05, y0 - cyw - ext * 0.15, z0 - czw + ext * 0.05)
        vx, vy, vz = x - eye[0], y - eye[1], z - eye[2]
        # rotate so the diagonal (1,0,1) is the view axis
        f = (vx + vz) / math.sqrt(2)
        s = (vx - vz) / math.sqrt(2)
        d = max(f, 1.0)
        return (W / 2 + s / d * W * 0.55, H / 2 + vy / d * H * 0.55, f)

    items = []
    culled = 0
    for pts, mat, transp in polys:
        pp = [proj(p) for p in pts]
        depth = sum(p[2] for p in pp) / len(pp)
        if view == "corner" and any(p[2] < 1.0 for p in pp):
            continue
        if CULL:
            ar = ((pp[1][0] - pp[0][0]) * (pp[2][1] - pp[0][1])
                  - (pp[2][0] - pp[0][0]) * (pp[1][1] - pp[0][1]))
            if ar * CULL > 0.0:
                culled += 1
                continue
        items.append((depth, pp, colour(mat, transp)))
    if CULL:
        print(f"    cull={CULL}: dropped {culled} of {len(polys)} ({100.0*culled/max(len(polys),1):.0f}%)")
    items.sort(key=lambda it: -it[0])          # far first
    zmax = max((it[0] for it in items), default=1.0) or 1.0
    for depth, pp, col in items:
        f = 1.0 - 0.6 * max(0.0, min(1.0, depth / zmax))
        c = tuple(int(v * f) for v in col)
        dr.polygon([(p[0], p[1]) for p in pp], fill=c, outline=(0, 0, 0))
    return img


def bounds_of(polys):
    xs = [p[0] for pts, _, _ in polys for p in pts]
    ys = [p[1] for pts, _, _ in polys for p in pts]
    zs = [p[2] for pts, _, _ in polys for p in pts]
    return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("ipds", nargs="*")
    ap.add_argument("--compare", nargs=2, metavar=("DIR_A", "DIR_B"))
    ap.add_argument("--cell", help="cell name for --compare, e.g. ERFCFC")
    ap.add_argument("--view", default="iso", choices=("top", "iso", "corner"))
    ap.add_argument("--size", default="640x480")
    ap.add_argument("--cull", type=int, default=0, help="1 or -1: reject one winding")
    a = ap.parse_args()
    global CULL
    CULL = a.cull
    W, H = (int(v) for v in a.size.lower().split("x"))

    if a.compare:
        name = a.cell.upper() + ".IPD"
        pa = os.path.join(a.compare[0], name)
        pb = os.path.join(a.compare[1], name)
        A = cell_polys(Ipd.parse(open(pa, "rb").read()))
        B = cell_polys(Ipd.parse(open(pb, "rb").read()))
        bb = bounds_of(A)
        ia = render(A, (W, H), a.view, bb)
        ib = render(B, (W, H), a.view, bb)
        img = Image.new("RGB", (W * 2 + 8, H), (255, 255, 255))
        img.paste(ia, (0, 0)); img.paste(ib, (W + 8, 0))
        d = ImageDraw.Draw(img)
        d.text((6, 4), f"{name}  A: {len(A)} polys", fill=(255, 255, 0))
        d.text((W + 14, 4), f"B: {len(B)} polys", fill=(255, 255, 0))
        img.save(a.out)
        print(f"wrote {a.out}: {len(A)} vs {len(B)} polygons")
        return

    polys = []
    for p in a.ipds:
        polys += cell_polys(Ipd.parse(open(p, "rb").read()))
    img = render(polys, (W, H), a.view, bounds_of(polys))
    img.save(a.out)
    print(f"wrote {a.out}: {len(polys)} polygons")


if __name__ == "__main__":
    main()
