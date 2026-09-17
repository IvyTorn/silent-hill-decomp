#!/usr/bin/env bash
# Build the Silent Hill N64 port -> n64_port/bin/sh.z64
#
#   ./n64_port/docker_run.sh bash ./n64_port/build_n64.sh
#
# Compilation is delegated to n64_gate.sh rather than duplicated here. That is
# the point: the gate owns the one source list and the one flag set, so the ELF
# is by construction exactly what the gate verified. Two lists would drift, and
# the failure mode of that drift is "gate green, link red".
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
: "${N64_INST:?run this through n64_port/docker_run.sh}"
I="$N64_INST"
OUT="$SCRIPT_DIR/bin"
GATE="$SCRIPT_DIR/build/gate"
mkdir -p "$OUT"

# The overlays are separate link units compiled against the same headers as
# the main binary. A struct changed in include/ after the last overlay build
# leaves the two disagreeing on layout, and nothing reports it: the camera read
# every road as garbage for a week of header fixes this way. Rebuild them when
# anything they compile from is newer than the oldest overlay.
OLDEST_DSO=$(ls -t "$SCRIPT_DIR"/filesystem/maps/*.dso 2>/dev/null | tail -1)
STALE=0
if [ -z "$OLDEST_DSO" ] || [ "$SCRIPT_DIR/build_maps.sh" -nt "$OLDEST_DSO" ]; then
    STALE=1
else
    for d in "$SCRIPT_DIR/../include" "$SCRIPT_DIR/../src/maps" \
             "$SCRIPT_DIR/../pc_port/include" "$SCRIPT_DIR/../xbox_port/include" \
             "$SCRIPT_DIR/include" "$SCRIPT_DIR/../pc_port/build_gen/extracted_data"; do
        [ -d "$d" ] || continue
        if [ -n "$(find "$d" \( -name '*.h' -o -name '*.c' \) -newer "$OLDEST_DSO" -print -quit)" ]; then
            STALE=1
            break
        fi
    done
fi
if [ "$STALE" = 1 ]; then
    echo "=== map overlays stale: rebuilding ==="
    bash "$SCRIPT_DIR/build_maps.sh" || { echo "MAP OVERLAY BUILD FAILED"; exit 1; }
fi

echo "=== compile ==="
# -Os, not the gate's default -O0: text is the budget here. See README.
SH_OPT="${SH_OPT:--Os -fno-strict-aliasing -fwrapv}" bash "$SCRIPT_DIR/n64_gate.sh"

ls "$GATE"/*.o >/dev/null 2>&1 || { echo "no objects to link"; exit 1; }

# The DSO externs list. n64dso-extern reads the overlays and emits a linker
# script of EXTERN() directives naming every symbol they import, which forces
# the main link to KEEP those symbols -- without it --gc-sections strips
# anything the main executable does not itself call, and the map that needed it
# fails to bind at dlopen time with no hint as to why.
DSOS=$(ls "$SCRIPT_DIR"/filesystem/maps/*.dso 2>/dev/null | tr '\n' ' ')
EXTERNS=""
if [ -n "$DSOS" ]; then
    echo "=== dso externs ($(echo $DSOS | wc -w) overlays) ==="
    "$I/bin/n64dso-extern" -o "$SCRIPT_DIR/build/main.externs" $DSOS
    EXTERNS="$SCRIPT_DIR/build/main.externs"
fi

echo
echo "=== link ($(ls "$GATE"/*.o | wc -l) objects) ==="
# g++ drives the link even though almost everything is C: libdragon's n64.mk
# does the same, because ld is inconsistent about global ctors/dtors and the
# --wrap __do_global_ctors below is written against g++'s arrangement.
if ! mips64-elf-g++ -o "$OUT/sh.elf" "$GATE"/*.o -lc -mabi=o64 \
        -Wl,-L"$I/mips64-elf/lib" -Wl,-lt3d -Wl,-ldragon -Wl,-lm -Wl,-ldragonsys \
        -Wl,-T"$I/mips64-elf/lib/n64.ld" \
        ${EXTERNS:+-Wl,-T"$EXTERNS"} \
        -Wl,--gc-sections -Wl,--wrap,__do_global_ctors -Wl,--wrap,malloc -Wl,--wrap,calloc \
        -Wl,-Map="$SCRIPT_DIR/build/sh.map",--cref 2> "$SCRIPT_DIR/build/link.log"; then
    echo "LINK FAILED"
    echo
    echo "undefined symbols by count (this IS the HAL surface still to write):"
    grep -oE "undefined reference to \`[^']+'" "$SCRIPT_DIR/build/link.log" \
        | sed "s/undefined reference to \`//; s/'$//" | sort | uniq -c | sort -rn | head -60
    echo
    echo "distinct undefined symbols: $(grep -oE "undefined reference to \`[^']+'" \
        "$SCRIPT_DIR/build/link.log" | sort -u | wc -l)"
    echo
    echo "other link errors:"
    grep -v "undefined reference to" "$SCRIPT_DIR/build/link.log" \
        | grep -vE "^$|In function|first defined here" | head -20
    exit 1
fi

mips64-elf-size -G "$OUT/sh.elf"

echo
echo "=== rom ==="
"$I/bin/n64sym" "$OUT/sh.elf" "$OUT/sh.elf.sym"
# The main symbol table the DSO loader binds overlay imports against. Without it
# every dlopen resolves nothing and the overlay's first call to shared game code
# goes to address 0.
MSYM=""
if [ -n "$DSOS" ]; then
    "$I/bin/n64dso-msym" "$OUT/sh.elf" "$OUT/sh.msym"
    MSYM="$OUT/sh.msym"
fi
cp "$OUT/sh.elf" "$OUT/sh.elf.stripped"
mips64-elf-strip -s "$OUT/sh.elf.stripped"
# -c 0: NOT compressed. The level-1 stream is unpacked in place by IPL3 at
# boot, and once the ELF grew past ~1.1MB the unpack started overwriting the
# tail of its own input: a hardware boot then crashed in Fs_InitializeMem on a
# garbage instruction (0x90dfb200 where the ELF holds jr ra) that the card's
# ROM did not contain. The emulator tolerated it. An extra ~1MB of cartridge
# is nothing; a corrupt .text tail is everything.
"$I/bin/n64elfcompress" -o "$OUT" -c 0 "$OUT/sh.elf.stripped"
# A DragonFS image, when n64_port/filesystem has anything in it. That directory
# is gitignored and holds disc data, which is not ours to redistribute -- the
# ROM builds fine without it and simply finds no disc, which is what a build
# machine should do. Its reason to exist is that an emulator has no flashcart
# SD card, so rom:/ is the ONLY disc source ares can see. On hardware the SD
# card wins anyway (see the probe order in cd_n64.c).
DFS=""
if [ -d "$SCRIPT_DIR/filesystem" ] && [ -n "$(ls -A "$SCRIPT_DIR/filesystem" 2>/dev/null)" ]; then
    echo "    [DFS] $(du -sh "$SCRIPT_DIR/filesystem" | cut -f1)"
    "$I/bin/mkdfs" "$SCRIPT_DIR/build/sh.dfs" "$SCRIPT_DIR/filesystem" >/dev/null
    DFS="$SCRIPT_DIR/build/sh.dfs"
fi

rm -f "$OUT/sh.z64"
"$I/bin/n64tool" --toc --title "SILENT HILL" --output "$OUT/sh.z64" \
    --align 256 "$OUT/sh.elf.stripped" "$OUT/sh.elf.sym" \
    ${MSYM:+"$MSYM"} \
    ${DFS:+--align 4096 "$DFS"}

ls -l "$OUT/sh.z64"

# The hardware ROM: same executable, DFS holds ONLY the map overlays (2.4MB).
# The overlays MUST travel inside the ROM because they bind against this exact
# build's msym table -- a stale maps/ folder on the SD card would dlopen and
# jump into nothing. Disc data stays on SD (sd:/silenthill/, it is user data
# and 78MB), which cd_n64.c's probe order finds by itself.
if [ -d "$SCRIPT_DIR/filesystem/maps" ]; then
    HWDFS="$SCRIPT_DIR/build/sh_hw.dfs"
    rm -rf "$SCRIPT_DIR/build/hwfs"
    mkdir -p "$SCRIPT_DIR/build/hwfs"
    cp -r "$SCRIPT_DIR/filesystem/maps" "$SCRIPT_DIR/build/hwfs/maps"
    # Native world assets ride in the cart so a console needs nothing new on
    # its SD card; sd:/silenthill/gamedata/load/N64W still overrides for mods.
    if [ -d "$SCRIPT_DIR/build/n64w" ]; then
        cp -r "$SCRIPT_DIR/build/n64w" "$SCRIPT_DIR/build/hwfs/N64W"
    fi
    # Native character assets (mkchara.py output: N64C/HERO.SHW + .SHT).
    if [ -d "$SCRIPT_DIR/build/chara/N64C" ]; then
        cp -r "$SCRIPT_DIR/build/chara/N64C" "$SCRIPT_DIR/build/hwfs/N64C"
    fi
    "$I/bin/mkdfs" "$HWDFS" "$SCRIPT_DIR/build/hwfs" >/dev/null
    rm -f "$OUT/sh_hardware.z64"
    "$I/bin/n64tool" --toc --title "SILENT HILL" --output "$OUT/sh_hardware.z64"         --align 256 "$OUT/sh.elf.stripped" "$OUT/sh.elf.sym"         ${MSYM:+"$MSYM"}         --align 4096 "$HWDFS"
    ls -l "$OUT/sh_hardware.z64"
fi

# Diagnostic ROM: SND-less disc pack + maps, small enough (<64MB) that ares
# keeps ISViewer alive and prints the game's ENTIRE debug log to stdout --
# run it with run_emu_log.ps1. Build build/diag_disc.shpak once with:
#   python n64_port/tools/mkdiscpack.py <disc.bin> n64_port/build/diag_disc.shpak --dirs 1ST,ANIM,TIM,MISC
if [ -f "$SCRIPT_DIR/build/diag_disc.shpak" ] && [ -d "$SCRIPT_DIR/filesystem/maps" ]; then
    rm -rf "$SCRIPT_DIR/build/diagfs"
    mkdir -p "$SCRIPT_DIR/build/diagfs"
    cp "$SCRIPT_DIR/build/diag_disc.shpak" "$SCRIPT_DIR/build/diagfs/disc.shpak"
    # Emulator config (start map etc.): read as rom:/silenthill.cfg when no
    # SD card is mounted. Consoles never look at it.
    if [ -f "$SCRIPT_DIR/emu/silenthill.cfg" ]; then
        cp "$SCRIPT_DIR/emu/silenthill.cfg" "$SCRIPT_DIR/build/diagfs/silenthill.cfg"
    fi
    cp -r "$SCRIPT_DIR/filesystem/maps" "$SCRIPT_DIR/build/diagfs/maps"
    # Native world assets (mkworld.py output), when staged: the emulator has
    # no SD card, so rom:/N64W is the only way t3d_world.c finds them there.
    # Native Harry too, or the emulator draws him on the PSX path.
    if [ -d "$SCRIPT_DIR/build/chara/N64C" ]; then
        cp -r "$SCRIPT_DIR/build/chara/N64C" "$SCRIPT_DIR/build/diagfs/N64C"
    fi
    if [ -d "$SCRIPT_DIR/build/n64w" ]; then
        cp -r "$SCRIPT_DIR/build/n64w" "$SCRIPT_DIR/build/diagfs/N64W"
        # THR (the town, ~20MB) would push this ROM past ares's 64MB ISViewer
        # limit; the diag ROM keeps the smaller areas only.
        rm -f "$SCRIPT_DIR/build/diagfs/N64W/"THR*
        echo "    [DIAG] N64W: $(du -sh "$SCRIPT_DIR/build/diagfs/N64W" | cut -f1)"
    fi
    "$I/bin/mkdfs" "$SCRIPT_DIR/build/sh_diag.dfs" "$SCRIPT_DIR/build/diagfs" >/dev/null
    rm -f "$OUT/sh_diag.z64"
    "$I/bin/n64tool" --toc --title "SILENT HILL" --output "$OUT/sh_diag.z64"         --align 256 "$OUT/sh.elf.stripped" "$OUT/sh.elf.sym"         ${MSYM:+"$MSYM"}         --align 4096 "$SCRIPT_DIR/build/sh_diag.dfs"
    ls -l "$OUT/sh_diag.z64"
fi
echo
echo "Run: powershell -File n64_port/run_emu.ps1 -Rom n64_port/bin/sh.z64"
