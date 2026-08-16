#!/usr/bin/env sh
# gen_map_syms.sh <mapname> <output.syms> <obj>...
#
# Emits a xenon-objcopy --redefine-syms rename list that prefixes every DEFINED
# extern symbol of a map overlay's objects with the map's name:
#   g_MapOverlayHeader_map0_s01 -> map0_s01_g_MapOverlayHeader_map0_s01
#
# Applying the SAME list to all of a map's objects renames definitions and
# intra-map references together, while UNDEFINED (shared game) symbols are left
# alone and still bind to bodyprog/pc_port -- the same binding a PC map DLL gets
# from the exe's import lib. That is what kills the 500+ cross-map duplicate
# symbols: the shared player/particle/chara sources are #included into every map,
# so each map defines its own copy of all of them.
#
# Adapted from the PS3 port's ps3_port/tools/gen_map_syms.sh. Its PPC64 ELFv1
# ".foo descriptor vs foo code" special case does not arise here -- the 360 is
# 32-bit ELF, one symbol per function -- but the dot-form branch is kept because
# it costs nothing and makes the two ports diffable.
set -e

map="$1"
out="$2"
shift 2

xenon-nm --extern-only --defined-only "$@" \
    | awk -v map="$map" '
        NF >= 3 {
            s = $3
            if (substr(s, 1, 1) == ".")
                print s, "." map "_" substr(s, 2)
            else
                print s, map "_" s
        }' \
    | sort -u > "$out"
