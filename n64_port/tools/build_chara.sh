#!/usr/bin/env bash
# Regenerate the native-character assets (N64C/*.SHW + .SHT) into build/chara,
# which build_n64.sh then copies into the ROM/hwfs.
#
# CHARACTERS USE THE ORIGINAL, NON-DECIMATED ILM. ilm_decimate.py is for the big
# world maps; a character is only ~840 tris and its FACE is critical -- decimating
# HERO's head (74 prims -> 46) leaves the survivors stretching the dark eye/nose
# texels across the whole face = the "cross in the face" glitch. The original SHW
# is ~25KB and needs ~27KB of the 40KB CHAR_ARENA, so it fits. Do NOT point this
# at decimated_maps/CHARA.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/../build/chara"

# Original character assets (not redistributable; gitignored). The N64 repo keeps
# only decimated copies, so source them from the sibling decomp checkout.
CHARA="${SH_CHARA_DIR:-/c/Claude/silenthill/silent-hill-decomp/assets/USA/CHARA}"
if [ ! -f "$CHARA/HERO.ILM" ]; then
    echo "ERROR: original CHARA assets not found at $CHARA (set SH_CHARA_DIR)" >&2
    exit 1
fi

# name:ilm:tim  (add enemies here as mkchara gains coverage)
for NAME in HERO; do
    echo "=== mkchara $NAME (original ILM) ==="
    python3 "$HERE/mkchara.py" --ilm "$CHARA/$NAME.ILM" --tim "$CHARA/$NAME.TIM" --out "$OUT"
done
echo "=== chara assets in $OUT/N64C ==="
ls -l "$OUT/N64C"
