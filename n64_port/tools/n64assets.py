#!/usr/bin/env python3
"""n64assets.py - one step from a Silent Hill disc image to the SD card folder.

    python n64assets.py "Silent Hill (USA).bin" --out D:\\          (an SD card root)
    python n64assets.py "Silent Hill (USA).bin" --out .\\sdcard  [--drop-below 0.05]

Writes:

    <out>/silenthill/disc.shpak                 the sector pack the ROM reads
    <out>/silenthill/gamedata/load/BG/*.IPD     map chunks rebuilt for the N64

and leaves anything already there (silenthill.cfg, saves, the log) alone.
Copy or point <out> at the SD card root; the ROM finds sd:/silenthill/.

The pack is the disc's own data, unchanged (mkdiscpack.py). The map chunks
are the decimated ones (ipd_decimate.py): every merge is shape-exact, and the
lossy --drop-below step is off unless asked for. The ROM loads a chunk from
gamedata/load/ in preference to the pack, so deleting that folder restores
the original geometry with no other change.

Options after -- are passed straight to ipd_decimate.py (e.g. --uv-max 64
--size-max 2048 --drop-below 0.05 --area ER).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    print("+", " ".join(f'"{c}"' if " " in c else c for c in cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(f"step failed ({r.returncode})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bin", help="disc image (.bin, 2352-byte raw sectors)")
    ap.add_argument("--out", required=True, help="SD card root (or a folder to copy there)")
    ap.add_argument("--dirs", default=None,
                    help="disc directories to put in the pack (mkdiscpack default if omitted)")
    ap.add_argument("--repack", action="store_true", help="rebuild disc.shpak even if present")
    ap.add_argument("--no-decimate", action="store_true", help="pack only, leave the maps alone")
    ap.add_argument("--drop-below", type=float, default=0.0, metavar="AREA",
                    help="lossy: drop isolated tiny primitives under AREA square units (try 0.05)")
    ap.add_argument("rest", nargs="*", help="extra ipd_decimate.py options after --")
    args = ap.parse_args()

    if not os.path.isfile(args.bin):
        sys.exit(f"no such disc image: {args.bin}")
    root = os.path.join(args.out, "silenthill")
    os.makedirs(root, exist_ok=True)
    py = sys.executable

    pack = os.path.join(root, "disc.shpak")
    if args.repack or not os.path.isfile(pack):
        cmd = [py, os.path.join(HERE, "mkdiscpack.py"), args.bin, pack]
        if args.dirs:
            cmd += ["--dirs", args.dirs]
        run(cmd)
    else:
        print(f"= {pack} exists; keeping it (--repack to rebuild)")

    if args.no_decimate:
        print("done (pack only)")
        return

    tmp = tempfile.mkdtemp(prefix="n64assets_")
    try:
        run([py, os.path.join(HERE, "mkdiscpack.py"), args.bin, "unused",
             "--extract", tmp, "--dirs", "BG"])
        bg_in = os.path.join(tmp, "gamedata", "load", "BG")
        bg_out = os.path.join(root, "gamedata", "load", "BG")
        cmd = [py, os.path.join(HERE, "ipd_decimate.py"), bg_in, "--out", bg_out, "--verify"]
        if args.drop_below > 0:
            cmd += ["--drop-below", str(args.drop_below)]
        cmd += args.rest
        run(cmd)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    print(f"done. SD layout under {root}:")
    print(f"  disc.shpak            (from your disc, unchanged)")
    print(f"  gamedata/load/BG/     (decimated map chunks; delete this folder to restore the originals)")


if __name__ == "__main__":
    main()
