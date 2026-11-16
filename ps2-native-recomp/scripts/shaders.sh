#!/usr/bin/env bash
# Regenerate GLSL 430 shaders from WGSL sources using naga-cli (Step 2.2).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GPU="$ROOT/runtime/src/gs/gpu"
OUT="$GPU/glsl"
NAGA="${NAGA:-naga}"
mkdir -p "$OUT"

OVERRIDES="SCALE=1,TILE=16,EXPLICIT_FMA=0,HAS_MIP=0,TEX_SS=0,DEPTH_FRAC=0"
for entry in upload copy_read copy_write load_clut display decode snapshot resolve raster; do
  "$NAGA" "$GPU/gs.wgsl" "$OUT/gs_${entry}.comp" \
    --entry-point "$entry" --profile core430 --compact --override "$OVERRIDES"
done

"$NAGA" "$GPU/gs.wgsl" "$OUT/gs_raster_mip.comp" \
  --entry-point raster --profile core430 --compact \
  --override "SCALE=1,TILE=16,EXPLICIT_FMA=0,HAS_MIP=1,TEX_SS=0,DEPTH_FRAC=0"

"$NAGA" "$GPU/blit.wgsl" "$OUT/blit.vert" --entry-point vs --profile core430 --compact
"$NAGA" "$GPU/blit.wgsl" "$OUT/blit.frag" --entry-point fs --profile core430 --compact
"$NAGA" "$GPU/blit.wgsl" "$OUT/blit_easu.frag" --entry-point fs_easu --profile core430 --compact

python3 - "$OUT" <<'PY'
import glob, os, re, sys
out = sys.argv[1]
pat = re.compile(r'layout\((std430|std140)\)(\s+(?:readonly\s+)?(?:buffer|uniform)\s+\w+\s*\{\s*[^}]*?_group_0_binding_(\d+)_)')
for path in glob.glob(os.path.join(out, "*")):
    txt = open(path, "r", encoding="utf-8").read()
    new = pat.sub(r'layout(\1, binding = \3)\2', txt)
    if new != txt:
        open(path, "w", encoding="utf-8", newline="\n").write(new)
PY

# make naga's GLSL run on nouveau and Ryujinx and keep OpenGL's clip space (scripts/glsl_compat.py says what and why)
python3 "$ROOT/scripts/glsl_compat.py" "$OUT"/*.comp "$OUT"/*.frag "$OUT"/*.vert

echo "Generated GLSL shaders in $OUT"
