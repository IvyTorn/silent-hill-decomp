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

echo
echo "=== link ($(ls "$GATE"/*.o | wc -l) objects) ==="
# g++ drives the link even though almost everything is C: libdragon's n64.mk
# does the same, because ld is inconsistent about global ctors/dtors and the
# --wrap __do_global_ctors below is written against g++'s arrangement.
if ! mips64-elf-g++ -o "$OUT/sh.elf" "$GATE"/*.o -lc -mabi=o64 \
        -Wl,-L"$I/mips64-elf/lib" -Wl,-ldragon -Wl,-lm -Wl,-ldragonsys \
        -Wl,-T"$I/mips64-elf/lib/n64.ld" \
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
cp "$OUT/sh.elf" "$OUT/sh.elf.stripped"
mips64-elf-strip -s "$OUT/sh.elf.stripped"
"$I/bin/n64elfcompress" -o "$OUT" -c 1 "$OUT/sh.elf.stripped"
rm -f "$OUT/sh.z64"
"$I/bin/n64tool" --toc --title "SILENT HILL" --output "$OUT/sh.z64" \
    --align 256 "$OUT/sh.elf.stripped" "$OUT/sh.elf.sym"

ls -l "$OUT/sh.z64"
echo
echo "Run: powershell -File n64_port/run_emu.ps1 -Rom n64_port/bin/sh.z64"
