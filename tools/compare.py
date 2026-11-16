#!/usr/bin/env python3
"""Compare reference and test screenshots (PNG/PPM) via Pillow."""
import argparse
import sys
from pathlib import Path
from PIL import Image


def compare_pair(ref_path: Path, got_path: Path, chan_tol: int, max_diff_pct: float) -> tuple[bool, str]:
    ref = Image.open(ref_path).convert("RGB")
    got = Image.open(got_path).convert("RGB")
    if ref.size != got.size:
        return False, f"SIZE MISMATCH {ref.size} vs {got.size}"

    rp = ref.tobytes()
    gp = got.tobytes()
    total = ref.width * ref.height
    diff_pixels = 0
    max_chan = 0
    for i in range(0, len(rp), 3):
        dr = abs(rp[i] - gp[i])
        dg = abs(rp[i + 1] - gp[i + 1])
        db = abs(rp[i + 2] - gp[i + 2])
        m = max(dr, dg, db)
        if m > max_chan:
            max_chan = m
        if m > chan_tol:
            diff_pixels += 1

    pct = (diff_pixels * 100.0) / total if total else 0.0
    ok = pct <= max_diff_pct
    status = "OK" if ok else "FAIL"
    return ok, f"{status} diff={diff_pixels}/{total} ({pct:.3f}%) max_chan={max_chan}"


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare screenshots or directories.")
    ap.add_argument("ref", type=Path, help="Reference image or directory")
    ap.add_argument("got", type=Path, help="Test image or directory")
    ap.add_argument("--tol", type=int, default=4, help="Per-channel tolerance (0-255, default 4)")
    ap.add_argument("--max-pct", type=float, default=0.5, help="Max differing pixel %% (default 0.5)")
    args = ap.parse_args()

    if args.ref.is_file() and args.got.is_file():
        ok, msg = compare_pair(args.ref, args.got, args.tol, args.max_pct)
        print(f"{args.ref.name}: {msg}")
        return 0 if ok else 1

    if args.ref.is_dir() and args.got.is_dir():
        refs = sorted(p for p in args.ref.iterdir() if p.suffix.lower() in {".png", ".ppm"})
        if not refs:
            print(f"no images in {args.ref}", file=sys.stderr)
            return 2
        failures = 0
        for r in refs:
            g = args.got / r.name
            if not g.exists():
                alt = g.with_suffix(".ppm" if r.suffix.lower() == ".png" else ".png")
                g = alt if alt.exists() else g
            if not g.exists():
                print(f"{r.name}: MISSING in {args.got}")
                failures += 1
                continue
            ok, msg = compare_pair(r, g, args.tol, args.max_pct)
            print(f"{r.name}: {msg}")
            if not ok:
                failures += 1
        return 1 if failures else 0

    print("ref and got must both be files or both be directories", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
