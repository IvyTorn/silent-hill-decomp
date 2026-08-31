#!/usr/bin/env bash
# Build the 42 map overlays beyond map0_s00 as libdragon DSOs.
#
#   ./n64_port/docker_run.sh bash ./n64_port/build_maps.sh          # all
#   ./n64_port/docker_run.sh bash ./n64_port/build_maps.sh map0_s01 # one
#
# This is the one place the N64 port diverges structurally from every other
# port, and it is the reason it can exist at all.
#
# PC, Xbox, 360 and PSP link all 42 maps into the executable. They can afford
# to, and they have to, because the shared AI / particle / player sources are
# #included into EVERY map -- a naive link of all 43 hits 500+ duplicate
# symbols, so those ports rename every defined symbol to <map>_<sym> and link
# 42 private copies. That is what makes the PSP image 17.7 MB.
#
# An N64 has 8 MB. So this port goes back to what the PSX actually did: one
# overlay resident at a time, loaded on room transition. libdragon's DSO loader
# is that mechanism, and it dissolves the duplicate-symbol problem on the way
# past -- each DSO is its own link unit, so two maps may both define
# Chara_Update without ever meeting.
#
# WHY THIS BLOCKS RENDERING, not just map transitions (the 360 port's note,
# which applies verbatim here): with only map0_s00 present, MapRegistry_Load
# refuses every transition, so the attract demo -- which wants map2_s00,
# map1_s01, map1_s02 -- never loads a world. There is then no world geometry in
# the ordering table, which reads as "the renderer is broken" when the renderer
# has simply been handed nothing to draw.
#
# map0_s00 stays statically linked and is built by n64_gate.sh with the rest of
# the game, matching map_n64.c's registry.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"
: "${N64_INST:?run this through n64_port/docker_run.sh}"
I="$N64_INST"

OBJ="$SCRIPT_DIR/build/maps"
DSOELF="$SCRIPT_DIR/build/dso_elf"
OUT="$SCRIPT_DIR/filesystem/maps"
LOG="$SCRIPT_DIR/build/maps.log"
mkdir -p "$OBJ" "$DSOELF" "$OUT"; : > "$LOG"

ALL_MAPS="
map0_s01 map0_s02
map1_s00 map1_s01 map1_s02 map1_s03 map1_s04 map1_s05 map1_s06
map2_s00 map2_s01 map2_s02 map2_s03 map2_s04
map3_s00 map3_s01 map3_s02 map3_s03 map3_s04 map3_s05 map3_s06
map4_s00 map4_s01 map4_s02 map4_s03 map4_s04 map4_s05 map4_s06
map5_s00 map5_s01 map5_s02 map5_s03
map6_s00 map6_s01 map6_s02 map6_s03 map6_s04 map6_s05
map7_s00 map7_s01 map7_s02 map7_s03
"
MAPS="${1:-$ALL_MAPS}"

# Identical to n64_gate.sh's, deliberately: a map TU is ordinary game code, and
# any divergence here shows up as a link error rather than a compile one.
DEFS="-DSH_N64_PORT -DSH_XBOX_PORT -DSH_PC_PORT"
DEFS="$DEFS -DVER_USA -DSKIP_ASM -DUSE_PGXP=0 -DN64 -DSH_NO_DIRENT -DRENDERER_OGL"
CDEFS="-Dstatic_assert=_Static_assert"

INCS="-I$SCRIPT_DIR/include -I$PCPORT/include -I$PCPORT/include/psyq_compat -I$PCPORT/src
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PSYCROSS/include/psx -I$PSYCROSS/include
      -I$DECOMP/xbox_port/include"

WARN="-Wno-implicit-function-declaration -Wno-implicit-int
      -Wno-incompatible-pointer-types -Wno-int-conversion
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
      -Wno-sign-compare -Wno-unused-variable -Wno-unused-function
      -Wno-missing-braces -Wno-parentheses -Wno-return-type -Wno-pointer-sign
      -Wno-return-mismatch -Wno-builtin-declaration-mismatch"

# -G0 and -DN64_DSO are libdragon's DSO requirements, not ours: a DSO is
# position-independent-ish code relocated at load time, and anything in
# .sdata/.sbss would be addressed through a $gp the loader does not set up.
CFLAGS="-march=vr4300 -mtune=vr4300 -mabi=o64 -falign-functions=32
        -ffunction-sections -fdata-sections -std=gnu17 -g -G0 -DN64_DSO
        -fgnu89-inline ${SH_OPT:--Os -fno-strict-aliasing -fwrapv}"

built=0; failed=0
for m in $MAPS; do
    [ -d "$DECOMP/src/maps/$m" ] || { echo "no such map: $m" >&2; failed=$((failed+1)); continue; }

    # MAP0_S01 from map0_s01. The per-map define gates the shared chara/particle
    # code the map TUs #include.
    MDEF=$(echo "$m" | tr 'a-z' 'A-Z')
    rm -f "$OBJ/$m"_*.o

    ok=1
    for f in "$DECOMP/src/maps/$m"/*.c \
             "$PCPORT/build_gen/extracted_data/${m}_extracted_data.c"; do
        [ -f "$f" ] || continue
        obj="$OBJ/${m}_$(basename "$f" .c).o"
        if ! err=$(mips64-elf-gcc $CFLAGS $DEFS $CDEFS $INCS $WARN \
                        "-D$MDEF" "-DSH_MAP_NAME=$m" -c "$f" -o "$obj" 2>&1); then
            { echo "########## $f"; echo "$err"; } >> "$LOG"
            ok=0
        fi
    done
    [ "$ok" = 1 ] || { echo "  $m: COMPILE FAILED"; failed=$((failed+1)); continue; }

    # --unresolved-symbols=ignore-all: a map references hundreds of symbols that
    # live in the main executable. They stay undefined here and are bound by
    # dlopen against the .msym table built from the main ELF.
    if ! err=$(mips64-elf-ld --emit-relocs --unresolved-symbols=ignore-all --nmagic \
                    -T"$I/mips64-elf/lib/dso.ld" \
                    -o "$DSOELF/$m.elf" "$OBJ/$m"_*.o 2>&1); then
        { echo "########## LINK $m"; echo "$err"; } >> "$LOG"
        echo "  $m: LINK FAILED"; failed=$((failed+1)); continue
    fi

    "$I/bin/n64dso" -o "$OUT" -c 1 "$DSOELF/$m.elf" >> "$LOG" 2>&1
    built=$((built+1))
done

echo "=========================================="
echo " map overlays (DSO)"
echo "   built  : $built"
echo "   failed : $failed"
if [ "$built" -gt 0 ]; then
    echo "   size   : $(du -sh "$OUT" | cut -f1)  ($(ls "$OUT"/*.dso 2>/dev/null | wc -l) files)"
    echo "   largest: $(ls -S "$OUT"/*.dso 2>/dev/null | head -1 | xargs -r basename) \
$(ls -S "$OUT"/*.dso 2>/dev/null | head -1 | xargs -r stat -c %s) bytes"
fi
echo "=========================================="
[ "$failed" -gt 0 ] && { echo "see $LOG"; exit 1; }
exit 0
