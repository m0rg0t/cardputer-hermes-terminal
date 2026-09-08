#!/usr/bin/env python3
"""Report actual M5Apps slot usage and the largest linked symbols.

Build first, then run: python3 scripts/firmware_size.py
Use --json to save machine-readable measurements, or --nm for a custom toolchain.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SLOT_BYTES = 0x140000
PROFILES = ("cardputer-adv-hermes", "cardputer-adv-hermes-web")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--environment", choices=PROFILES, action="append")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--json", action="store_true")
    core = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
    parser.add_argument("--nm", default=str(core / "packages" /
                        "toolchain-xtensa-esp32s3" / "bin" /
                        ("xtensa-esp32s3-elf-nm.exe" if os.name == "nt"
                         else "xtensa-esp32s3-elf-nm")))
    args = parser.parse_args()
    if args.top < 1:
        parser.error("--top must be positive")
    reports = []
    try:
        for profile in args.environment or PROFILES:
            build = ROOT / ".pio" / "build" / profile
            size = (build / "firmware.bin").stat().st_size
            output = subprocess.check_output([
                args.nm, "--print-size", "--size-sort", "--radix=d", "--demangle",
                str(build / "firmware.elf")], text=True)
            symbols = []
            for line in output.splitlines():
                fields = line.split(maxsplit=3)
                if len(fields) != 4 or not fields[1].isdigit():
                    continue
                # BSS has no payload in firmware.bin. Initialized data and
                # read-only/code symbols do; report symbols, not additive totals.
                if fields[2] in "TtRrDd":
                    symbols.append({"bytes": int(fields[1]), "name": fields[3]})
            reports.append({"environment": profile, "image_bytes": size,
                            "slot_bytes": SLOT_BYTES,
                            "remaining_bytes": SLOT_BYTES - size,
                            "largest_symbols": sorted(symbols, key=lambda s: s["bytes"],
                                                      reverse=True)[:args.top]})
    except (OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Size report failed: {error}\nBuild the selected profiles first.\n")
    if args.json:
        print(json.dumps(reports, indent=2))
    else:
        for report in reports:
            print(f'{report["environment"]}: {report["image_bytes"]:,} / '
                  f'{SLOT_BYTES:,} bytes; {report["remaining_bytes"]:,} free')
            for symbol in report["largest_symbols"]:
                print(f'  {symbol["bytes"]:6,}  {symbol["name"]}')
    return int(any(report["remaining_bytes"] < 0 for report in reports))


if __name__ == "__main__":
    raise SystemExit(main())
