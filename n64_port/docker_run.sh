#!/usr/bin/env bash
# Run a command inside the libdragon toolchain container.
#
#   ./docker_run.sh ./n64_port/n64_gate.sh
#   ./docker_run.sh bash            # interactive poke-around
#
# The image is JUST the compiler: ghcr.io/dragonminded/libdragon ships
# mips64-elf-gcc 14.2.0 + newlib + libstdc++ at $N64_INST=/n64_toolchain, and
# nothing of libdragon itself. libdragon is a separate checkout mounted at
# /libdragon, so the container stays a pinned compiler and the library stays
# a thing we can rebuild and bisect without touching the image.
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"

# Host path of the libdragon checkout. Override to point at another one.
LIBDRAGON_HOST="${LIBDRAGON_HOST:-C:/Claude/N64/N64FlashcartMenu/libdragon}"
IMAGE="${N64_IMAGE:-ghcr.io/dragonminded/libdragon:latest}"

[ -d "$LIBDRAGON_HOST" ] || { echo "no libdragon at $LIBDRAGON_HOST" >&2; exit 1; }

# MSYS2 rewrites anything that looks like a unix path in an argument to a
# Windows path before exec. That mangles the container-side paths (/work
# becomes C:/msys64/work) and every mount silently lands somewhere wrong.
export MSYS2_ARG_CONV_EXCL='*'

exec docker run --rm -i \
    -v "$(cygpath -w "$DECOMP" 2>/dev/null || echo "$DECOMP")":/work \
    -v "$(cygpath -w "$LIBDRAGON_HOST" 2>/dev/null || echo "$LIBDRAGON_HOST")":/libdragon \
    -w /work \
    -e LIBDRAGON=/libdragon \
    "$IMAGE" "$@"
