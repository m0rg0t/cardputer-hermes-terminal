#!/usr/bin/env python3
"""Generate packed 2-bit Hermes expression frames."""

import subprocess
import sys
import re
from pathlib import Path


WIDTH = 92
HEIGHT = 92
BACKGROUND = (8, 12, 16)


def pack(source: Path) -> bytearray:
    command = [
        "magick", "-size", f"{WIDTH}x{HEIGHT}", "xc:#080c10",
        "(", "-size", "88x88", "xc:none", "-fill", "#f4f1e8",
        "-draw", "roundrectangle 0,0 87,87 6,6", ")",
        "-gravity", "center", "-composite",
        "(", str(source), "-resize", "84x84!", ")",
        "-gravity", "center", "-composite", "-depth", "8", "rgb:-",
    ]
    rgb = subprocess.run(command, check=True, stdout=subprocess.PIPE).stdout
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
    return encoded


def array(name: str, encoded: bytearray) -> str:
    lines = []
    for offset in range(0, len(encoded), 16):
        values = ", ".join(
            f"0x{value:02X}" for value in encoded[offset:offset + 16]
        )
        lines.append(f"    {values},")
    return f"const std::uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def ready_frame(header: Path) -> bytearray:
    text = header.read_text(encoding="utf-8")
    match = re.search(
        r"kHermesBadge2Bpp\[\]\s*=\s*\{([\s\S]*?)\};", text
    )
    if not match:
        raise SystemExit(f"READY frame not found in {header}")
    return bytearray(
        int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match.group(1))
    )


def set_pixel(frame: bytearray, x: int, y: int, value: int) -> None:
    pixel = y * WIDTH + x
    index = pixel // 4
    shift = 6 - (pixel % 4) * 2
    frame[index] = (frame[index] & ~(0x03 << shift)) | (value << shift)


def get_pixel(frame: bytearray, x: int, y: int) -> int:
    pixel = y * WIDTH + x
    return (frame[pixel // 4] >> (6 - (pixel % 4) * 2)) & 0x03


def main() -> None:
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: generate_hermes_states.py WORKING BLINK READY_HEADER OUTPUT_HEADER"
        )
    working = pack(Path(sys.argv[1]))
    generated_blink = pack(Path(sys.argv[2]))
    blink = ready_frame(Path(sys.argv[3]))
    # At the packed 92x92 resolution the visible eye occupies this small box.
    # Start with the exact checked-in READY bytes and copy only that box from
    # the closed-eye artwork. This is a hard guarantee against scale/position
    # drift or color-profile changes elsewhere in the portrait.
    for y in range(37, 49):
        for x in range(28, 44):
            set_pixel(blink, x, y, get_pixel(generated_blink, x, y))
    expected = WIDTH * HEIGHT // 4
    if len(working) != expected or len(blink) != expected:
        raise SystemExit("unexpected packed frame size")

    output = Path(sys.argv[4])
    output.write_text(
        "#pragma once\n\n"
        "#include <cstdint>\n\n"
        "#include \"hermes_terminal/hermes_badge.h\"\n\n"
        "// Optional expressive states. The blink frame is pixel-identical to\n"
        "// the READY portrait outside the visible eye region.\n"
        + array("kHermesWorking2Bpp", working)
        + "\n"
        + array("kHermesBlink2Bpp", blink),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
