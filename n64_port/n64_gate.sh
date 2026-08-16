#!/usr/bin/env bash
# Milestone 1 gate: compile the shared game tree for the N64's VR4300.
#
# Runs INSIDE the libdragon toolchain container:
#   ./n64_port/docker_run.sh ./n64_port/n64_gate.sh
#
#   ./n64_gate.sh            # whole tree
#   ./n64_gate.sh src/main   # restrict to a subtree
#
# Compiles to real objects, never -fsyntax-only: a missing header makes
# -fsyntax-only report a file CLEAN, which is how the iOS port lost a day.
#
# The base is the Xbox 360 port, not the PSP one, even though the PSP port is
# the other MIPS target. o64 and the 360's -m32 PPC share a data model --
# 32-bit pointers, 32-bit long, big-endian -- and endianness is the half that
# fails SILENTLY (it reorders bitfields as well as bytes). MIPS-vs-PPC is
# nothing at the C level by comparison.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECOMP="$(cd "$SCRIPT_DIR/.." && pwd)"
PCPORT="$DECOMP/pc_port"
PSYCROSS="$PCPORT/PsyCross"
LIBDRAGON="${LIBDRAGON:-/libdragon}"
# The INSTALLED headers, not $LIBDRAGON/include: only the install has the
# vendored trees (libcart/, fatfs/) laid out where libdragon.h expects them.
N64_INC="${N64_INST:-/n64_inst}/mips64-elf/include"

CC="${CC:-mips64-elf-gcc}"
CXX="${CXX:-mips64-elf-g++}"
command -v "$CC" >/dev/null 2>&1 || {
    echo "no $CC on PATH -- run this through n64_port/docker_run.sh" >&2; exit 1; }

OUT="$SCRIPT_DIR/build/gate"
LOG="$SCRIPT_DIR/build/gate.log"
WARNLOG="$SCRIPT_DIR/build/gate_warnings.log"
mkdir -p "$OUT"; : > "$LOG"; : > "$WARNLOG"
# The gate is not incremental -- every source is rebuilt every time -- so
# clearing the objects costs nothing and closes a real trap: an object whose
# SOURCE has since been deleted or renamed lingers here and still gets linked.
rm -f "$OUT"/*.o

# SH_XBOX360_PORT rides along because that port is this one's base: it carries
# the 32-bit big-endian corrections, and the whole surface is 8 files. There is
# deliberately no SH_PSP_PORT here -- the PSP port's gates mean "32-bit little
# endian", which is the opposite of what this target needs.
DEFS="-DSH_N64_PORT -DSH_XBOX360_PORT -DSH_XBOX_PORT -DSH_PC_PORT"
DEFS="$DEFS -DVER_USA -DSKIP_ASM -DUSE_PGXP=0 -DN64"
# newlib's bare-metal <dirent.h> is a single #error. Turns off the loose-file
# case-insensitive resolver in fsqueue_3.c, which a console does not want anyway.
DEFS="$DEFS -DSH_NO_DIRENT"
# PsyX_render.h #errors unless a renderer is picked, purely to typedef
# TextureID/ShaderID for game code that mentions them. RENDERER_OGL without
# USE_GLAD pulls in no GL header, which is why it is the one to pick rather
# than RENDERER_OGLES -- same choice the PSP gate made, same reason.
DEFS="$DEFS -DRENDERER_OGL"
# PsyCross asserts every PSX primitive's size in longs. Wiring these up is the
# point, not a formality: they are the first thing that would catch VR4300
# packing drift in the structs that overlay disc data. C ONLY -- in C++
# static_assert is a keyword, and defining it over libstdc++ breaks <type_traits>
# before a single line of our own code is read.
CDEFS="-Dstatic_assert=_Static_assert"

# psyq's include dir carries a kernel.h and a sys/ioctl.h that collide by name
# with libdragon's. Game TUs get this set, which has no libdragon on it at all,
# so the collision cannot arise. HAL TUs under n64_port/src get HAL_INCS below,
# where libdragon comes FIRST and psyq is absent.
INCS="-I$SCRIPT_DIR/include -I$PCPORT/include -I$PCPORT/include/psyq_compat -I$PCPORT/src
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PSYCROSS/include/psx -I$PSYCROSS/include
      -I$DECOMP/xbox_port/include"
# xbox_port/include carries gpu_nv2a.h (the ShVertex/GpuNv2a_* interface every
# console backend implements) and the declaration-only SDL_* shims. Neither
# pulls psyq, so they are safe on the libdragon side of the firewall.
HAL_INCS="-I$N64_INC/newlib_overrides -I$N64_INC -include ktls.h
      -I$SCRIPT_DIR/include -I$PCPORT/include -I$PCPORT/include/psyq_compat
      -I$DECOMP/xbox_port/include
      -I$DECOMP/include -I$DECOMP/include/decomp
      -I$PSYCROSS/include/psx -I$PSYCROSS/include"

# Mirrors the Xbox/360 suppression set: the decomp leans on gcc-permissive
# diagnostics.
WARN="-Wno-implicit-function-declaration -Wno-implicit-int
      -Wno-incompatible-pointer-types -Wno-int-conversion
      -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
      -Wno-sign-compare -Wno-unused-variable -Wno-unused-function
      -Wno-missing-braces -Wno-parentheses -Wno-return-type -Wno-pointer-sign
      -Wno-return-mismatch -Wno-builtin-declaration-mismatch"
# -Wno-return-mismatch: gcc 14 split "bare `return;` in a non-void function" out
# of -Wno-return-type AND made it an error by default. The decomp reproduces
# several PSX functions that fall off the end leaving whatever was in $v0
# (func_8005AA08 is one); that is the behaviour to keep, not a bug to fix.

# libdragon's own baseline, verbatim from n64.mk, minus its -Werror set (the
# decomp would never clear that) and minus -O2 (see SH_OPT below).
#   -mabi=o64      : 32-bit pointers and long, 64-bit registers.
#   -falign-functions=32 : required by libdragon's backtrace().
TARGETFLAGS="-march=vr4300 -mtune=vr4300 -mabi=o64 -falign-functions=32"
TARGETFLAGS="$TARGETFLAGS -ffunction-sections -fdata-sections -std=gnu17"
# -g is what turns libdragon's on-screen exception backtrace from a list of
# function names into file:line. It costs nothing in the ROM: n64sym extracts
# the symbol table to a side file and the ELF is stripped before packaging.
# libdragon's own n64.mk passes it for the same reason.
TARGETFLAGS="$TARGETFLAGS -g"
# -G0: put nothing in .sdata/.sbss. Those are addressed GP-relative through a
# SIGNED 16-BIT offset, so the whole small-data area has to fit in a 64 KB
# window around $gp -- and this game's static footprint is megabytes. Without
# this the link dies on "relocation truncated to fit", which reads like a
# linker-script problem and is actually just the game being too big for the
# default -G8 heuristic. libdragon sets -G0 for its DSO builds for the same
# reason; a static image this size needs it too.
TARGETFLAGS="$TARGETFLAGS -G0"

# Optimisation is an OVERRIDE with no default, deliberately: a warning-harvest
# run must not be able to change what ships. With it empty the compiler runs at
# -O0, and that is exactly why -Wmaybe-uninitialized never fires by default --
# gcc only runs that analysis with optimisation enabled. Harvest with
# SH_OPT='-O2 -fno-strict-aliasing -fwrapv'; the decomp type-puns constantly and
# relies on wrapping signed overflow, so -O2 without those two is not safe here.
TARGETFLAGS="$TARGETFLAGS ${SH_OPT:-}"
CXXFLAGS="$(echo "$TARGETFLAGS" | sed 's/-std=gnu17/-std=gnu++17/; s/-fgnu89-inline//')"

collect_srcs() {
    local root="${1:-}"
    if [ -n "$root" ]; then
        find "$DECOMP/$root" -name '*.c'
        return
    fi
    # Exclusions mirror xbox_port/Makefile.nxdk: main.c and memcpy.c carry MIPS
    # register-asm bodies written against the PSX's R3000 and its compiler, not
    # something a VR4300 build can reuse as-is.
    find "$DECOMP/src/main" -maxdepth 1 -name '*.c' ! -name 'main.c' ! -name 'memcpy.c'
    find "$DECOMP/src/bodyprog" -name '*.c' ! -name 'bodyprog_80032D1C.c' ! -name 'text_draw_jp.c'
    find "$DECOMP/src/screens" -name '*.c' ! -name 'hp_safe1.c' ! -name 's__safe2.c'
    # PCPORT_HAL_EXCLUDE: the pc_port sources a console replaces wholesale.
    # Compiling them here would only ever report the HAL we have not written yet.
    local excl="main_pc pc_combat pc_console_cmd pc_crash pc_quicksave dbg_overlay
                warning_screen hires_override fs_pc dll_loader map_overlay_loader
                map_registry xa_player control_style pc_mouse_cursor combat_target
                miniz tex_pack map7_s03_boss_motion"
    local pat=""
    for f in $excl; do pat="$pat ! -name $f.c"; done
    find "$PCPORT/src" -maxdepth 1 -name '*.c' $pat
    find "$PCPORT/src/stubs" -name '*.c' ! -name 'map_overlay_stub.c'
    # Only map0_s00, matching the 360 and PSP early milestones. The other 400
    # map TUs come once the overlay mechanism is proven -- and on N64 that
    # mechanism is libdragon's DSO loader, not a static link of all 42.
    find "$DECOMP/src/maps/map0_s00" -name '*.c'
    ls "$PCPORT/build_gen/extracted_data/map0_s00_extracted_data.c" 2>/dev/null
    find "$SCRIPT_DIR/src" -name '*.c' 2>/dev/null
    # Xbox HAL files that pull in NO nxdk headers, compiled straight out of
    # xbox_port/src rather than copied so the console ports cannot drift.
    for f in "$DECOMP"/xbox_port/src/*.c; do
        case "$(basename "$f")" in
            crash_xbox.c|dbg_overlay_xbox.c|dsound_bridge.c|dsound_xbox.c|\
            gpu_nv2a.c|net_xbox.c|pad_xbox.c|ra_badge_xbox.c|sdl_compat_xbox.c|\
            sh_log_xbox.c|xa_xbox.c|cd_xbox.c|fs_xbox.c|main_xbox.c|\
            ra_xbox.c|msvc_compat.c|fmv_xbox.c|\
            map_xbox.c) ;;
            # map_xbox.c: a static MAP_XBOX_HEADERS table naming all 42 map
            # overlay headers, which is what a console with the RAM to link
            # every map at once wants. N64 does not have it and goes back to
            # the PSX's one-resident-at-a-time model via libdragon DSO, so this
            # table would only ever demand 42 symbols that will never be
            # statically present.
            *) echo "$f" ;;
        esac
    done
    # The software GTE: portable C/C++, and the one piece of PsyCross a
    # bare-metal console keeps.
    echo "$PSYCROSS/src/psx/inline_c.c"
    echo "$PSYCROSS/src/psx/libgte.c"
    echo "$PSYCROSS/src/psx/abs.c"
    echo "$PSYCROSS/src/gte/PsyX_GTE.cpp"
}

pass=0; fail=0
while IFS= read -r f; do
    [ -z "$f" ] && continue
    obj="$OUT/$(echo "${f#$DECOMP/}" | tr '/' '_').o"
    case "$f" in
        */maps/map0_s00/*|*map0_s00_extracted_data.c) EXTRA="-DMAP0_S00 -DSH_MAP_NAME=map0_s00" ;;
        *) EXTRA="" ;;
    esac
    # -fgnu89-inline is a GAME-TU flag only. PsyCross declares helpers like
    # fst_min/fst_max as plain `inline`, which under C99 emits NO out-of-line
    # definition and links as undefined; gnu89 semantics emit one. But
    # libdragon's fmath.h declares its whole vector/quaternion library the same
    # way, and gnu89 semantics there emit a copy of every one of them in EVERY
    # HAL TU that includes libdragon.h -- "multiple definition of fm_lerp"
    # across gpu_rdp.o, cd_n64.o and main_n64.o.
    # n64_port/src/game/ is the port's own code that sits on the GAME side of
    # the include firewall: it needs decomp/psyq types (s_MapOverlayHdr and the
    # like) and must NOT see libdragon, whose kernel.h collides with psyq's.
    # Everything else under n64_port/src is HAL and gets the opposite set.
    case "$f" in
        "$SCRIPT_DIR"/src/game/*) USE_INCS="$INCS";     INLINEFLAG="-fgnu89-inline" ;;
        "$SCRIPT_DIR"/src/*)      USE_INCS="$HAL_INCS"; INLINEFLAG="" ;;
        *)                        USE_INCS="$INCS";     INLINEFLAG="-fgnu89-inline" ;;
    esac
    case "$f" in
        *.cpp) TOOL="$CXX"; FLAGS="$CXXFLAGS"; LANGDEFS="" ;;
        *)     TOOL="$CC";  FLAGS="$TARGETFLAGS"; LANGDEFS="$CDEFS" ;;
    esac
    if err=$("$TOOL" $FLAGS $INLINEFLAG $DEFS $LANGDEFS $USE_INCS $WARN $EXTRA -c "$f" -o "$obj" 2>&1); then
        pass=$((pass+1))
        # Warnings from a SUCCESSFUL compile are kept: a green gate that threw
        # them away is how an uninitialised blob reached Xbox hardware twice.
        [ -n "$err" ] && { echo "########## $f"; echo "$err"; } >> "$WARNLOG"
    else
        fail=$((fail+1))
        { echo "########## $f"; echo "$err"; } >> "$LOG"
    fi
done < <(collect_srcs "${1:-}")

echo "=========================================="
echo " VR4300 big-endian compile gate  [$CC]"
echo "   passed : $pass"
echo "   failed : $fail"
echo "   total  : $((pass+fail))"
echo "=========================================="
if [ "$fail" -gt 0 ]; then
    echo
    echo "Top error kinds:"
    grep -h "error:" "$LOG" | sed 's/.*error: //' \
        | sed "s/'[^']*'/'X'/g; s/\"[^\"]*\"/\"X\"/g" | sort | uniq -c | sort -rn | head -20
    echo
    echo -n "Files with errors: "; grep -c "^##########" "$LOG"
    echo "Full log: $LOG"
fi
