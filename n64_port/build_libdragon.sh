#!/usr/bin/env bash
# Build libdragon and its host tools into the mounted /n64_inst prefix.
# Run once (and again whenever the libdragon checkout moves):
#
#   ./n64_port/docker_run.sh bash ./n64_port/build_libdragon.sh
#
# Not libdragon's own build.sh: that one shells out to sudo and also builds
# every example, neither of which is wanted here. The three make targets below
# are what build.sh actually does for an install.
set -eu

: "${N64_INST:?run this through n64_port/docker_run.sh}"
: "${N64_GCCPREFIX:?run this through n64_port/docker_run.sh}"
LIBDRAGON="${LIBDRAGON:-/libdragon}"

cd "$LIBDRAGON"

# The checkout is a copy of one built in a DIFFERENT container, where it was
# mounted at /workspace/libdragon. gcc's .d files record dependencies as
# absolute paths, so those stale deps name a directory that does not exist here
# and make dies on "No rule to make target /workspace/libdragon/include/ktls.h".
# Nuking the object trees is the only reliable fix; make clean does not remove
# the .d files that carry the bad paths.
rm -rf build tools/build
find . -name '*.d' -delete

# install-mk lays down n64.mk itself; tools-install builds the HOST binaries
# (n64tool, n64sym, mkdfs, mkasset, audioconv64 ...) with the container's own
# gcc, which is why the image needing a host compiler is not incidental.
make -j"$(nproc)" install-mk
make -j"$(nproc)" install
make -j"$(nproc)" tools-install

echo
echo "=== installed ==="
ls "$N64_INST/mips64-elf/lib/"libdragon*.a
ls "$N64_INST/bin/" | grep -vE '^mips64-elf-' | tr '\n' ' '
echo
