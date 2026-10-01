#!/usr/bin/env python3
"""sh_n64_setup.py - turn a Silent Hill disc image into the SD card files the
N64 port needs.

    python sh_n64_setup.py "Silent Hill (USA).bin"
    python sh_n64_setup.py "Silent Hill (USA).cue" --out E:\\

The ROM ships with no game data: everything it draws comes from your own copy
of the disc. This reads that disc once and writes, under <out>/silenthill/ :

    disc.shpak                      the sectors the game reads at runtime
    gamedata/load/N64W/*.SHW,.SHT   world geometry, rebuilt for the N64 GPU
    gamedata/load/N64C/*.SHW,.SHT   characters, ditto
    silenthill.cfg                  settings (left alone if it already exists)
    README.txt                      what the files are

Point --out at the root of the flashcart's SD card, or leave it out and the
tool asks. Nothing else on the card is touched.

The disc must be the USA release (SLUS-00707) as a 2352-byte-per-sector image:
a .bin (with or without its .cue) or a .img. A 2048-byte .iso will not work --
the port reads raw CD sectors.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RAW_SECTOR = 2352

# The areas that have native world assets. Each is one mkworld run over the
# area's IPD cells; an area missing here still plays, through the slower
# per-primitive path. Keep in step with n64_port/build_n64.sh's staging.
# Area tags are the MapType_* names the engine uses (include/bodyprog/map/map.h).
# An area missing here still plays, but falls back to the per-primitive path and
# is much slower, so this list should match what the ROM expects.
AREAS = [
    ("ER",  None),              # interiors; this area has no global-PLM file
    ("SPR", "SPR_GLB.PLM"),     # Central Silent Hill
    ("THR", "THR_GLB.PLM"),     # the town exterior (the largest area, 128 cells)
    ("HP",  None),              # hospital
    ("HU",  None),              # hospital, otherworld
]

# Held weapons, baked into HERO's model in this exact order: it is the contract
# with ShT3d_HeldItemNative's slot table in t3d_world.c.
WEAPONS = ["KNIFE", "HAMMER", "AXE", "HANDGUN", "RIFLE", "SHOTGUN"]

# Keys whose DEFAULTS are already right are left out on purpose. An earlier
# version of this wrote "zbuffer=2", which is character-only depth, and that one
# line cost a test session: the world stopped depth-testing, so the police
# station's reception counter and other furniture were erased by the foreground
# world pass, 2D objects showed through Harry, and geometry outdoors vanished
# and came back only at point-blank range. The engine default is 1 (world AND
# character). Write a key here only to CHANGE a default, never to restate one.
DEFAULT_CFG = """# Silent Hill N64 - settings. Delete a line to get its default back.
# A full list of keys is in the port's README.

refresh_rate = 30
control_style = classic
map=map0_s00
"""

README = """Silent Hill (N64 port) - SD card contents
=========================================

silenthill/
  disc.shpak                  Data read straight from your disc image. The
                              port reads this the way the PSX read the CD.
                              It holds every directory except XA, whose
                              streamed voice tracks cannot survive the form
                              this file uses.
  gamedata/load/N64W/         World geometry rebuilt for the N64's GPU.
  gamedata/load/N64C/         Characters, same.
  silenthill.cfg              Settings. Edit with any text editor.
  silenthill.log              Written by the game. Send this along with any
  silenthill.prev.log         bug report; .prev is the previous session.

Put the SD card in the flashcart, load the ROM, and play. If the game boots to
a black screen or complains about missing data, the card is either not at the
root (the folder must be \\silenthill on the card, not inside another folder)
or the files came from a different disc release than the one the port expects.

Regenerating: run the setup tool again. It overwrites the data files and
leaves silenthill.cfg and the logs alone.
"""


def run(cmd, what):
    print(f"\n--- {what}")
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(f"\nFAILED: {what} (exit {r.returncode})")


def resolve_image(path):
    """Accept a .cue/.bin/.img; return the path of the raw 2352-byte image."""
    if not os.path.isfile(path):
        sys.exit(f"no such file: {path}")
    ext = os.path.splitext(path)[1].lower()
    if ext == ".iso":
        sys.exit("that is a 2048-byte .iso. The port reads raw CD sectors, so it\n"
                 "needs a .bin/.cue (2352 bytes per sector) image of the disc.")
    if ext == ".cue":
        text = open(path, encoding="utf-8", errors="replace").read()
        m = re.search(r'FILE\s+"([^"]+)"', text) or re.search(r"FILE\s+(\S+)", text)
        if not m:
            sys.exit(f"no FILE line in {path}")
        path = os.path.join(os.path.dirname(os.path.abspath(path)), m.group(1))
        if not os.path.isfile(path):
            sys.exit(f"{os.path.basename(path)} (named by the .cue) is not next to it")
    size = os.path.getsize(path)
    if size % RAW_SECTOR != 0:
        sys.exit(f"{os.path.basename(path)} is not a raw 2352-byte-sector image\n"
                 f"(its size is not a multiple of {RAW_SECTOR}). A .iso or a\n"
                 "compressed image will not do; use the .bin.")
    return path


def check_usa(path):
    """The file table is the USA release's. A different disc would read the
    right LBAs for the wrong files, which looks like random corruption much
    later, so refuse it here where the message can say why."""
    with open(path, "rb") as f:
        head = f.read(RAW_SECTOR * 400)
    if b"SLUS_007.07" in head:
        return
    for tag in (b"SLES_016.64", b"SLES_", b"SLPM_", b"SLPS_"):
        if tag in head:
            sys.exit("that disc is not the USA release (SLUS-00707).\n"
                     "The port's file table is the USA one; another region's\n"
                     "disc has its files at different places.")
    print("  note: could not confirm this is SLUS-00707; continuing anyway.")


def pick_out():
    print("\nWhere should the files go? Give the SD card's root (e.g. E:\\),")
    print("or a folder to copy there yourself.")
    ans = input("  path: ").strip().strip('"')
    if not ans:
        sys.exit("nothing given")
    return ans


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", nargs="?", help="disc image (.bin, .img or .cue)")
    ap.add_argument("--out", default=None, help="SD card root, or a folder")
    # Every directory but XA. The smaller default mkdiscpack ships (boot, title
    # and audio banks) is for iterating on a cartridge, and a game played from
    # it would find no map data at all: BG holds the rooms and VIN the map
    # overlays. XA cannot be packed -- Mode-2 Form-2 sectors do not survive the
    # cooked 2048-byte form -- so the streamed voice tracks need the raw BIN.
    ap.add_argument("--dirs", default="ALL",
                    help="disc directories for the pack (default: ALL, i.e. everything but XA)")
    ap.add_argument("--keep-pack", action="store_true",
                    help="keep an existing disc.shpak instead of rebuilding it")
    ap.add_argument("--no-world", action="store_true",
                    help="pack only; skip the N64W/N64C rebuild")
    args = ap.parse_args()

    print("Silent Hill - N64 SD card setup")
    print("===============================")

    image = args.image
    if not image:
        print("\nDrag your Silent Hill disc image onto this window and press Enter,")
        print("or type its path.")
        image = input("  image: ").strip().strip('"')
        if not image:
            sys.exit("nothing given")
    image = resolve_image(image)
    check_usa(image)
    print(f"\ndisc  : {image}")

    out = args.out or pick_out()
    # "E:" alone is the current directory ON drive E, not its root, and
    # os.path.join would quietly produce "E:silenthill" somewhere else entirely.
    if re.fullmatch(r"[A-Za-z]:", out):
        out += os.sep
    root = os.path.join(out, "silenthill")
    os.makedirs(root, exist_ok=True)
    print(f"target: {root}")

    py = sys.executable or "python"
    pack = os.path.join(root, "disc.shpak")
    if args.keep_pack and os.path.isfile(pack):
        print(f"\n= keeping the existing {pack}")
    else:
        cmd = [py, os.path.join(HERE, "mkdiscpack.py"), image, pack]
        if args.dirs:
            cmd += ["--dirs", args.dirs]
        run(cmd, "packing the disc sectors the game reads")

    if not args.no_world:
        tmp = tempfile.mkdtemp(prefix="sh_n64_")
        try:
            run([py, os.path.join(HERE, "mkdiscpack.py"), image, "unused",
                 "--extract", tmp, "--dirs", "BG,TIM,CHARA,ITEM"],
                "reading the map, texture and model files off the disc")
            src = os.path.join(tmp, "gamedata", "load")
            bg, tim = os.path.join(src, "BG"), os.path.join(src, "TIM")
            chara, item = os.path.join(src, "CHARA"), os.path.join(src, "ITEM")
            load = os.path.join(root, "gamedata", "load")

            for prefix, glb in AREAS:
                # Three texture directories, searched in this order: the area's
                # own BG art, the shared TIM folder, and ITEM -- the pickup
                # models' textures live there and nowhere else, so leaving it
                # out silently drops one item model from <PREFIX>ITEM.SHW.
                cmd = [py, os.path.join(HERE, "mkworld.py"),
                       "--ipd-dir", bg, "--tim-dir", bg, "--tim-dir", tim,
                       "--tim-dir", item, "--out", load]
                items = os.path.join(bg, "BG_ITEM.PLM")
                if os.path.isfile(items):
                    cmd += ["--items", items]
                glb_path = os.path.join(bg, glb) if glb else None
                if glb_path and os.path.isfile(glb_path):
                    cmd += ["--glb", glb_path]
                cmd.append(prefix)
                run(cmd, f"rebuilding the {prefix} world for the N64 GPU")

            weapons = []
            for w in WEAPONS:
                p = os.path.join(item, w + ".PLM")
                if os.path.isfile(p):
                    weapons += ["--weapon", p]
            run([py, os.path.join(HERE, "mkchara.py"),
                 "--ilm", os.path.join(chara, "HERO.ILM"),
                 "--tim", os.path.join(chara, "HERO.TIM"),
                 "--out", load] + weapons,
                "rebuilding Harry and his weapons")
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    cfg = os.path.join(root, "silenthill.cfg")
    if not os.path.isfile(cfg):
        with open(cfg, "w", encoding="utf-8", newline="\r\n") as f:
            f.write(DEFAULT_CFG)
    with open(os.path.join(root, "README.txt"), "w",
              encoding="utf-8", newline="\r\n") as f:
        f.write(README)

    total = 0
    for base, _, files in os.walk(root):
        for n in files:
            total += os.path.getsize(os.path.join(base, n))
    print("\n===============================")
    print(f"Done. {root} holds {total / 1048576:.0f} MB.")
    print("If that folder is not already on the SD card, copy it to the card's")
    print("root, so the card has \\silenthill\\ at the top level. Then load the")
    print("ROM on the flashcart.")


if __name__ == "__main__":
    main()
