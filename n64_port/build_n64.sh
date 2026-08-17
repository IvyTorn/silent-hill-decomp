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
        -Wl,-L"$I/mips64-elf/lib" -Wl,-ldragon -Wl,-lm -Wl,-ldragonsys \
        -Wl,-T"$I/mips64-elf/lib/n64.ld" \
        ${EXTERNS:+-Wl,-T"$EXTERNS"} \
        -Wl,--gc-sections -Wl,--wrap,__do_global_ctors \
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
"$I/bin/n64elfcompress" -o "$OUT" -c 1 "$OUT/sh.elf.stripped"
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
echo
echo "Run: powershell -File n64_port/run_emu.ps1 -Rom n64_port/bin/sh.z64"
