#!/usr/bin/env python3
"""Build a sparse sector pack (.shpak) from a Silent Hill disc image.

The N64 cannot carry the disc. A flashcart's SD card can hold the whole 616 MB
BIN and cd_n64.c reads that directly -- but an emulator has no SD card, and a
real cartridge has a 64 MB address window. This produces the thing that fits
both: only the sectors the selected directories actually occupy, cooked down to
2048-byte user data, with an index that preserves the original LBAs so the
game's file table still resolves.

    python mkdiscpack.py "Silent Hill (USA).bin" out.shpak
    python mkdiscpack.py disc.bin out.shpak --dirs 1ST,ANIM,TIM,SND,MISC

Sizes for the USA disc, cooked:

    1ST,ANIM                    7.8 MB   fast iteration
    1ST,ANIM,TIM,SND,MISC      34.1 MB   boot + title + audio banks (default)
    everything except XA       77.8 MB   SD only, over the cartridge window

XA is excluded by default and cannot be included: it is Mode-2 Form-2, whose
2324-byte payloads do not survive the 2048-byte cooked form. Streaming audio
needs the raw BIN on SD, or a VADPCM conversion, which is a different job.

Header and index are BIG-ENDIAN so the N64 reads them without byte swapping.
"""
import argparse
import os
import re
import struct
import sys

RAW_SECTOR = 2352
DATA_OFFSET = 24          # 12 sync + 3 addr + 1 mode + 8 subheader
DATA_SIZE = 2048
MAGIC = b"SHPK"
VERSION = 1
HEADER_FMT = ">4sIIIII"   # magic, version, runCount, sectorSize, dataOffset, reserved
HEADER_SIZE = struct.calcsize(HEADER_FMT)
RUN_FMT = ">III"          # startLba, count, sectorIndex

DEFAULT_DIRS = "1ST,ANIM,TIM,SND,MISC"


def load_filetable(path):
    """(startLba, sectors, topdir) for every entry in a filetable .inc."""
    text = open(path, encoding="utf-8", errors="replace").read()
    rows = re.findall(r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(\d+)\s*,.*?//\s*(\S+)", text)
    if not rows:
        sys.exit(f"no entries parsed from {path}")
    return [(int(a, 16), int(b), p.split("/")[0], p) for a, b, p in rows]


def merge_runs(entries, wanted, files_re=None):
    """Union the LBA ranges of the wanted directories into sorted runs.

    The file table has entries that overlap and repeat -- the raw sum of file
    lengths is 323437 sectors against a disc extent of 249145 -- so unioning is
    not an optimisation, it is what stops the pack storing the same sector many
    times over.
    """
    spans = sorted((a, a + n) for a, n, d, path in entries
                   if n > 0 and (d in wanted or (files_re and files_re.search(path))))
    if not spans:
        sys.exit("no files matched the requested directories")

    runs = []
    for lo, hi in spans:
        if runs and lo <= runs[-1][1]:
            runs[-1][1] = max(runs[-1][1], hi)
        else:
            runs.append([lo, hi])
    return runs


def extract_loose(bin_path, out_dir, entries, wanted, files_re):
    """Write each selected file as loose bytes, exactly the blockCount*256 the
    engine reads, decoded from the raw sectors. The tree mirrors what the
    engine's loose-file loader probes: gamedata/load/<TOPDIR>/<NAME>."""
    seen = set()
    n = 0
    total = 0
    with open(bin_path, "rb") as src:
        for lba, blocks, top, path in entries:
            if blocks <= 0 or path in seen:
                continue
            if not (top in wanted or (files_re and files_re.search(path))):
                continue
            seen.add(path)
            size = blocks * 256                       # blockCount is 256-byte blocks
            out = os.path.join(out_dir, "gamedata", "load", *path.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            data = bytearray()
            for s in range((size + DATA_SIZE - 1) // DATA_SIZE):
                src.seek((lba + s) * RAW_SECTOR + DATA_OFFSET)
                data += src.read(DATA_SIZE)
            with open(out, "wb") as f:
                f.write(bytes(data[:size]))
            n += 1
            total += size
    print(f"extracted : {n} files, {total / 1048576:.1f} MB -> {out_dir}/gamedata/load/")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin", help="disc image (2352-byte raw sectors)")
    ap.add_argument("out", help="output .shpak")
    ap.add_argument("--filetable", default=None,
                    help="path to filetable.c.USA.inc (default: alongside this script's repo)")
    ap.add_argument("--files", default=None,
                    help="regex over full disc paths; matching files are "
                         "included IN ADDITION to --dirs (e.g. "
                         "'SND/(FIRST|MAP00|MEP0|.*KDT)')")
    ap.add_argument("--dirs", default=DEFAULT_DIRS,
                    help=f"comma-separated disc directories to include (default: {DEFAULT_DIRS})")
    ap.add_argument("--extract", default=None, metavar="DIR",
                    help="instead of packing, write every selected file as a "
                         "loose file under DIR/gamedata/load/<TOPDIR>/<NAME> -- "
                         "the tree the engine's loose-file loader probes "
                         "(copy it to sd:/silenthill/). Use --dirs ALL for "
                         "every non-XA directory.")
    args = ap.parse_args()

    ft = args.filetable
    if ft is None:
        here = os.path.dirname(os.path.abspath(__file__))
        ft = os.path.join(here, "..", "..", "src", "main", "filetable.c.USA.inc")
    ft = os.path.normpath(ft)

    wanted = {d.strip().upper() for d in args.dirs.split(",") if d.strip()}
    if "XA" in wanted:
        sys.exit("XA cannot be packed: Mode-2 Form-2 payloads do not fit the cooked form")

    entries = load_filetable(ft)
    files_re = re.compile(args.files) if args.files else None

    if args.extract:
        if "ALL" in wanted:
            wanted = {d for _, _, d, _ in entries if d != "XA"}
        extract_loose(args.bin, args.extract, entries, wanted, files_re)
        return
    runs = merge_runs(entries, wanted, files_re)
    total = sum(hi - lo for lo, hi in runs)

    print(f"dirs      : {','.join(sorted(wanted))}")
    print(f"runs      : {len(runs)}")
    print(f"sectors   : {total}")
    print(f"pack size : {(HEADER_SIZE + len(runs) * 12 + total * DATA_SIZE) / 1048576:.1f} MB")

    disc_sectors = os.path.getsize(args.bin) // RAW_SECTOR

    with open(args.bin, "rb") as src, open(args.out, "wb") as dst:
        index = []
        sector_index = 0
        for lo, hi in runs:
            index.append((lo, hi - lo, sector_index))
            sector_index += hi - lo

        data_offset = HEADER_SIZE + len(index) * 12
        dst.write(struct.pack(HEADER_FMT, MAGIC, VERSION, len(index),
                              DATA_SIZE, data_offset, 0))
        for lba, count, si in index:
            dst.write(struct.pack(RUN_FMT, lba, count, si))

        short = 0
        for lo, hi in runs:
            src.seek(lo * RAW_SECTOR)
            for lba in range(lo, hi):
                raw = src.read(RAW_SECTOR)
                if len(raw) < RAW_SECTOR:
                    # Past the end of the image. Write zeros rather than a short
                    # file: the index already promised this sector exists, and a
                    # truncated pack would make every later run read misaligned.
                    dst.write(b"\0" * DATA_SIZE)
                    short += 1
                    continue
                dst.write(raw[DATA_OFFSET:DATA_OFFSET + DATA_SIZE])

    if short:
        print(f"WARNING   : {short} sectors past end of image "
              f"({disc_sectors} sectors) written as zeros")
    print(f"wrote     : {args.out} ({os.path.getsize(args.out) / 1048576:.1f} MB)")


if __name__ == "__main__":
    main()
