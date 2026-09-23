#!/usr/bin/env python3
"""pack_setup_tool.py - build the standalone SD card setup tool people get.

    python n64_port/tools/pack_setup_tool.py

Writes n64_port/build/sh_n64_setup/ and a .zip of it: the setup script, the
converters it drives, and the USA file table, with nothing else of the repo.
Nothing in it is game data -- the person runs it against their own disc.
"""
import os
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DECOMP = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "..", "build", "sh_n64_setup")

FILES = [
    "Silent Hill N64 Setup.cmd",
    "sh_n64_setup.py",
    "mkdiscpack.py",
    "mkworld.py",
    "mkchara.py",
]
FILETABLE = os.path.join(DECOMP, "src", "main", "filetable.c.USA.inc")


def main():
    out = os.path.normpath(OUT)
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)

    for n in FILES:
        shutil.copy2(os.path.join(HERE, n), os.path.join(out, n))
    if not os.path.isfile(FILETABLE):
        sys.exit(f"missing {FILETABLE}")
    shutil.copy2(FILETABLE, os.path.join(out, "filetable.c.USA.inc"))
    shutil.copytree(os.path.join(HERE, "sh1fmt"), os.path.join(out, "sh1fmt"),
                    ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))

    zip_path = out + ".zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(out):
            for n in files:
                p = os.path.join(base, n)
                z.write(p, os.path.join("sh_n64_setup",
                                        os.path.relpath(p, out)))
    print(f"{out}")
    print(f"{zip_path} ({os.path.getsize(zip_path) / 1024:.0f} KB)")


if __name__ == "__main__":
    main()
