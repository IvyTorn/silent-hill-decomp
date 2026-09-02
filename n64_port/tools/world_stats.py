#!/usr/bin/env python3
"""world_stats.py - measure what the native world converter has to handle.

    world_stats.py DIR PREFIX [PREFIX...]

For every <PREFIX>*.IPD in DIR: per-material UV windows, prim mix, vertex
counts per model buffer, semitrans fraction, and whether each material's used
UV span fits a TMEM tile (CI4 4096 texels / CI8 2048). This is the sizing
study for the SHW chunk format, not a shipping tool.
"""
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh1fmt.ipd import Ipd  # noqa: E402

PERIM = (0, 1, 3, 2)


def prim_corners(prim):
    seen, out = set(), []
    for c in PERIM:
        vi = prim.vi[c]
        if vi == 0xFF or vi in seen:
            continue
        seen.add(vi)
        out.append(c)
    return out


def main():
    d = sys.argv[1]
    prefixes = tuple(p.upper() for p in sys.argv[2:]) or ("",)
    files = sorted(f for f in os.listdir(d)
                   if f.upper().endswith(".IPD") and f.upper().startswith(prefixes))

    tot = defaultdict(int)
    # (timName, clutRow) -> [minU, minV, maxU, maxV, primCount]
    mat_windows = {}
    buf_sizes = []      # (verts, prims) per model buffer actually referenced
    model_share = defaultdict(int)

    for fn in files:
        ipd = Ipd.parse(open(os.path.join(d, fn), "rb").read())
        tot["files"] += 1
        lm = ipd.lm
        for buf in ipd.model_buffers:
            bverts = bprims = 0
            for inst in buf.instances:
                info = ipd.model_infos[inst.model_info_idx]
                if info.is_global_plm:
                    tot["global_plm_inst"] += 1
                    continue
                model = lm.model_by_name(info.name)
                if model is None:
                    tot["missing_model"] += 1
                    continue
                model_share[(fn, info.name)] += 1
                for mesh in model.meshes:
                    bverts += mesh.vertex_count
                    bprims += mesh.prim_count
                    for prim in mesh.prims:
                        tot["prims"] += 1
                        cs = prim_corners(prim)
                        tot["quads" if len(cs) == 4 else "tris"] += 1
                        if prim.is_transparent:
                            tot["semitrans"] += 1
                        if prim.material_idx < 0:
                            tot["untextured"] += 1
                            continue
                        m = lm.materials[prim.material_idx]
                        key = (m.name, prim.clut)
                        us = [prim.uv[c][0] for c in cs]
                        vs = [prim.uv[c][1] for c in cs]
                        w = mat_windows.setdefault(key, [255, 255, 0, 0, 0])
                        w[0] = min(w[0], min(us)); w[1] = min(w[1], min(vs))
                        w[2] = max(w[2], max(us)); w[3] = max(w[3], max(vs))
                        w[4] += 1
            if bprims:
                buf_sizes.append((bverts, bprims))
            tot["billboards"] += buf.billboard_count

    print(f"files={tot['files']} prims={tot['prims']} "
          f"(tris={tot['tris']} quads={tot['quads']}) "
          f"semitrans={tot['semitrans']} untex={tot['untextured']} "
          f"billboards={tot['billboards']} plmInst={tot['global_plm_inst']} "
          f"missingModel={tot['missing_model']}")

    if buf_sizes:
        bs = sorted(buf_sizes, key=lambda t: -t[1])
        tv = sum(v for v, p in buf_sizes); tp = sum(p for v, p in buf_sizes)
        print(f"model buffers referenced={len(buf_sizes)} "
              f"verts total={tv} prims total={tp} "
              f"largest buf (v,p)={bs[0]} median={bs[len(bs)//2]}")

    # TMEM feasibility: window in texels; assume CI4 (4bpp) since map TIMs are.
    # CI4 tile budget: 4096 texels, width padded to 16.
    fit = big = 0
    biggest = []
    for (name, clut), (u0, v0, u1, v1, n) in sorted(mat_windows.items()):
        w = (u1 - u0 + 1 + 15) & ~15
        h = v1 - v0 + 1
        if w * h <= 4096:
            fit += 1
        else:
            big += 1
            biggest.append((w * h, name, clut, w, h, n))
    print(f"(material,clut) pairs={len(mat_windows)} tmemFit={fit} tooBig={big}")
    for sz, name, clut, w, h, n in sorted(biggest, reverse=True)[:15]:
        print(f"  BIG {name:>8s} clut={clut:<4d} window={w}x{h} prims={n}")


if __name__ == "__main__":
    main()
