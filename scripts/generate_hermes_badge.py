#!/usr/bin/env python3
"""Generate the packed four-level 92x92 Hermes portrait header."""

import subprocess
import sys
from pathlib import Path


WIDTH = 92
HEIGHT = 92
BACKGROUND = (8, 12, 16)


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: generate_hermes_badge.py LOGO OUTPUT_HEADER")
    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    command = [
        "magick", "-size", f"{WIDTH}x{HEIGHT}", "xc:#080c10",
        "(", "-size", "88x88", "xc:none", "-fill", "#f4f1e8",
        "-draw", "roundrectangle 0,0 87,87 6,6", ")",
        "-gravity", "center", "-composite",
        "(", str(source), "-resize", "84x84!", ")",
        "-gravity", "center", "-composite", "-depth", "8", "rgb:-",
    ]
    rgb = subprocess.run(command, check=True, stdout=subprocess.PIPE).stdout
    if len(rgb) != WIDTH * HEIGHT * 3:
        raise SystemExit(f"unexpected RGB data size: {len(rgb)}")

    levels: list[int] = []
    for offset in range(0, len(rgb), 3):
        red, green, blue = rgb[offset:offset + 3]
        if (red, green, blue) == BACKGROUND:
            level = 0
        else:
            luminance = (red * 299 + green * 587 + blue * 114) // 1000
            level = 1 if luminance < 72 else 2 if luminance < 184 else 3
        levels.append(level)

    encoded = bytearray()
    for offset in range(0, len(levels), 4):
        a, b, c, d = levels[offset:offset + 4]
        encoded.append((a << 6) | (b << 4) | (c << 2) | d)

    lines = []
    for offset in range(0, len(encoded), 16):
        values = ", ".join(
            f"0x{value:02X}" for value in encoded[offset:offset + 16]
        )
        lines.append(f"    {values},")
    expected = WIDTH * HEIGHT // 4
    output.write_text(
        "#pragma once\n\n"
        "#include <cstddef>\n#include <cstdint>\n\n"
        "// Four pixels per byte, most-significant pair first.\n"
        f"constexpr int kHermesBadgeWidth = {WIDTH};\n"
        f"constexpr int kHermesBadgeHeight = {HEIGHT};\n\n"
        "const std::uint8_t kHermesBadge2Bpp[] = {\n"
        + "\n".join(lines)
        + "\n};\n\n"
        "static_assert(sizeof(kHermesBadge2Bpp) == "
        "kHermesBadgeWidth * kHermesBadgeHeight / 4,\n"
        "              \"Hermes badge packing mismatch\");\n",
        encoding="utf-8",
    )
    if len(encoded) != expected:
        raise SystemExit(f"unexpected packed size: {len(encoded)}")


if __name__ == "__main__":
    main()
