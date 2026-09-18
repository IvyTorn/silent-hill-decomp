#!/usr/bin/env bash
# Repackage ONLY the diag ROM (emulator) after changing n64_port/emu/silenthill.cfg
# or the staged filesystem. No compile, no link: seconds instead of minutes.
#   ./n64_port/docker_run.sh bash ./n64_port/repack_diag.sh
set -eu
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
I="${N64_INST:-/n64_inst}"
OUT="$SCRIPT_DIR/bin"
[ -d "$SCRIPT_DIR/build/diagfs" ] || { echo "no build/diagfs -- run build_n64.sh first"; exit 1; }
cp "$SCRIPT_DIR/emu/silenthill.cfg" "$SCRIPT_DIR/build/diagfs/silenthill.cfg"
MSYM=""
[ -f "$OUT/sh.msym" ] && MSYM="$OUT/sh.msym"
"$I/bin/mkdfs" "$SCRIPT_DIR/build/sh_diag.dfs" "$SCRIPT_DIR/build/diagfs" >/dev/null
rm -f "$OUT/sh_diag.z64"
"$I/bin/n64tool" --toc --title "SILENT HILL" --output "$OUT/sh_diag.z64" \
    --align 256 "$OUT/sh.elf.stripped" "$OUT/sh.elf.sym" ${MSYM:+"$MSYM"} \
    --align 4096 "$SCRIPT_DIR/build/sh_diag.dfs"
ls -l "$OUT/sh_diag.z64"
grep -v '^#' "$SCRIPT_DIR/emu/silenthill.cfg" | grep -v '^[[:space:]]*$' | sed 's/^/    cfg: /'
