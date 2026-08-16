#!/usr/bin/env bash
# Run a command inside the libdragon toolchain container.
#
#   ./docker_run.sh bash ./n64_port/n64_gate.sh
#   ./docker_run.sh bash            # interactive poke-around
#
# Three mounts, and the split between them is the point:
#
#   /work        this repo
#   /libdragon   libdragon SOURCE  (../libdragon, our own copy)
#   /n64_inst    libdragon INSTALL (../n64_inst: headers, libs, host tools)
#
# The image is JUST the compiler: ghcr.io/dragonminded/libdragon ships
# mips64-elf-gcc 14.2.0 + newlib at /n64_toolchain and nothing of libdragon
# itself -- no libdragon.a, no n64tool, no mkasset. Those get built once into
# the mounted /n64_inst by build_libdragon.sh, which is why N64_GCCPREFIX is
# set separately from N64_INST: the compiler lives in the image and stays
# pinned, the library lives on the host and stays rebuildable and bisectable.
#
# libdragon is OUR OWN COPY, not C:/Claude/N64/N64FlashcartMenu/libdragon.
# Building in that one would dirty a working SummerCart64 project.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
ROOT="$(cd "$DECOMP/.." && pwd)"

LIBDRAGON_HOST="${LIBDRAGON_HOST:-$ROOT/libdragon}"
N64_INST_HOST="${N64_INST_HOST:-$ROOT/n64_inst}"
IMAGE="${N64_IMAGE:-ghcr.io/dragonminded/libdragon:latest}"

[ -d "$LIBDRAGON_HOST" ] || { echo "no libdragon source at $LIBDRAGON_HOST" >&2; exit 1; }
mkdir -p "$N64_INST_HOST"

win() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

# MSYS2 rewrites anything that looks like a unix path in an argument to a
# Windows path before exec. That mangles the container-side paths (/work
# becomes C:/msys64/work) and every mount silently lands somewhere wrong.
export MSYS2_ARG_CONV_EXCL='*'

exec docker run --rm -i \
    -v "$(win "$DECOMP")":/work \
    -v "$(win "$LIBDRAGON_HOST")":/libdragon \
    -v "$(win "$N64_INST_HOST")":/n64_inst \
    -w /work \
    -e LIBDRAGON=/libdragon \
    -e N64_INST=/n64_inst \
    -e N64_GCCPREFIX=/n64_toolchain \
    "$IMAGE" "$@"
