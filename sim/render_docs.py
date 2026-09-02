#!/usr/bin/env python3
"""Render every device screen through the SDL preview and export PNGs.

Usage:
  python3 sim/render_docs.py                 # docs/images/screens + site/assets/screens
  python3 sim/render_docs.py --out DIR       # a single output directory
  python3 sim/render_docs.py --sheet sheet.png --only chat-ready approval

Builds the native-sim target, dumps every scenario as PPM, and converts the
240x135 frames to nearest-neighbour scaled PNGs so pixels stay crisp.
"""
from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUTS = [ROOT / "docs" / "images" / "screens",
                   ROOT / "site" / "assets" / "screens"]


def contact_sheet(frames: list[str], path: str, scale: int = 1) -> None:
    cols = 4
    w, h = 240 * scale, 135 * scale
    pad, label = 8 * scale, 14 * scale
    rows = (len(frames) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (w + pad) + pad, rows * (h + label + pad) + pad),
                      (24, 26, 32))
    draw = ImageDraw.Draw(sheet)
    for index, frame in enumerate(frames):
        x = pad + (index % cols) * (w + pad)
        y = pad + (index // cols) * (h + label + pad)
        image = Image.open(frame).convert("RGB")
        if scale != 1:
            image = image.resize((w, h), Image.NEAREST)
        sheet.paste(image, (x, y))
        draw.text((x, y + h + 2 * scale), Path(frame).stem, fill=(200, 200, 200))
    sheet.save(path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scale", type=int, default=3)
    parser.add_argument("--out", help="single output directory (default: docs + site)")
    parser.add_argument("--only", nargs="*", help="scenario names to export")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--sheet", help="also write a labelled contact sheet PNG")
    parser.add_argument("--sheet-scale", type=int, default=1)
    args = parser.parse_args()

    pio = os.environ.get("PIO", "pio")
    if not args.no_build:
        subprocess.run([pio, "run", "-e", "native-sim"], cwd=ROOT, check=True)
    program = ROOT / ".pio" / "build" / "native-sim" / "program"

    outputs = [Path(args.out)] if args.out else DEFAULT_OUTPUTS
    for out in outputs:
        out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="hermes-shots-") as temp:
        subprocess.run([str(program), "--shots", temp], check=True,
                       stdout=subprocess.DEVNULL)
        frames = sorted(glob.glob(f"{temp}/*.ppm"))
        written = 0
        for frame in frames:
            name = Path(frame).stem
            if args.only and name not in args.only:
                continue
            image = Image.open(frame).convert("RGB")
            size = (image.width * args.scale, image.height * args.scale)
            scaled = image.resize(size, Image.NEAREST)
            for out in outputs:
                scaled.save(out / f"{name}.png", optimize=True)
            written += 1
        if args.sheet:
            contact_sheet(frames, args.sheet, args.sheet_scale)
    print(f"wrote {written} screens to {', '.join(str(o) for o in outputs)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
