#!/usr/bin/env bash
# Deterministic screenshot harness for MC2 (Phase 0.6, reused in Phase 2/3).
# Usage: bash tools/harness.sh [out_dir]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RECOMP="$ROOT/MC2Recomp/ps2-native-recomp"
PS2RUN="${PS2_RUN:-$RECOMP/build/native-mc2/ps2run.exe}"
OUT="${1:-$ROOT/baseline/pc-wgpu}"
TMP_PPM="$RECOMP/games/mc2/harness_ppm"
MISSES="$RECOMP/games/mc2/harness_misses.txt"

rm -rf "$TMP_PPM" "$MISSES"
mkdir -p "$TMP_PPM" "$OUT"

cd "$RECOMP"
PS2_LIVE=0 "$PS2RUN" games/mc2/patched/SLUS_202.09 \
  --disc games/mc2/disc.iso \
  --game-dir games/mc2 \
  --patches games/mc2/patches.txt \
  --misses "$MISSES" \
  --frames 4500 \
  --frames-dir "$TMP_PPM" \
  --frame-at 300 \
  --frame-at 600 \
  --frame-at 1800 \
  --frame-at 3600 \
  --frame-at 4500 \
  --press 250:start:10 \
  --press 450:start:10 \
  --press 700:start:10 \
  --press 1000:cross:10 \
  --press 1200:cross:10 \
  --press 1500:cross:10 \
  --press 1800:down:10 \
  --press 1900:cross:10 \
  --press 2100:cross:10 \
  --press 2300:cross:10 \
  --press 2500:cross:10 \
  --press 2800:cross:10 > "$OUT/harness.log" 2>&1

if [ -s "$MISSES" ]; then
  echo "FAIL: misses recorded in $MISSES:" >&2
  cat "$MISSES" >&2
  exit 1
fi

PY="$HOME/AppData/Local/Programs/Python/Python313/python.exe"
[ -x "$PY" ] || PY="python3"
"$PY" - "$TMP_PPM" "$OUT" << 'EOF'
import sys
from pathlib import Path
from PIL import Image

src = Path(sys.argv[1])
dst = Path(sys.argv[2])
names = {
    "frame_00300.ppm": "01_logo_rockstar.png",
    "frame_00600.ppm": "02_logo_sandiego.png",
    "frame_01800.ppm": "03_menu.png",
    "frame_03600.ppm": "04_race_start.png",
    "frame_04500.ppm": "05_race_ingame.png",
}
for ppm in sorted(src.glob("*.ppm")):
    out_name = names.get(ppm.name, ppm.stem + ".png")
    Image.open(ppm).save(dst / out_name)
    print(f"saved {dst / out_name}")
EOF
