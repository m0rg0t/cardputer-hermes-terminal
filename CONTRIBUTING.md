# Contributing

Small, hardware-tested changes are especially valuable on a memory-constrained device.

```bash
platformio run -e cardputer-adv-hermes -e cardputer-adv-hermes-web
./scripts/test_native.sh
python3 scripts/firmware_size.py
```

Before submitting a change:

1. Keep credentials and device backups out of the commit.
2. Build the Cardputer ADV environment and run native tests.
3. Check the firmware size against the application-slot limit.
4. Exercise the affected flow on hardware when possible.
5. Update the relevant documentation.

For UI work, design for 240×135, maintain high contrast, never use black text on red selection, keep animation geometry fixed, expose loading and errors, make lengthy operations cancellable with `Esc`, and keep the command rail accurate.

Pull requests should explain the user-visible result, test evidence, firmware-size impact, and configuration changes.

`firmware_size.py --json` reports each actual application image's slot usage and
largest linked code/data symbols. BSS is excluded from the symbol ranking;
the ranking is diagnostic, not an additive size total. Keep the 0x140000-byte
M5Apps slot fixed. The web profile uses application-only LTO with single-use
function inlining disabled to reduce code duplication; third-party libraries
retain their existing compilation settings.

The native suite includes shared interaction logic: request lifecycle,
late/duplicate replies, keyboard edits, separate drafts, and Wi-Fi key recovery.
It does not simulate the physical keyboard or a live Hermes connection; use
the device test plan for those checks. CI builds both firmware profiles.


The portable runner uses a unique temporary directory per invocation and removes
its binaries on success, compiler/test failure, or interruption. `CXX` selects a
compiler executable (default `c++`); `TMPDIR` selects the temporary parent.
Run `python3 scripts/test_native_runner.py` to check isolation and cleanup using
synthetic compiler fixtures. These runner tests supplement the real C++ suite.

### Dependency compatibility (2026-10-03)

The maintenance build uses PlatformIO 6.2.0, espressif32 7.1.3, ArduinoJson
7.4.3 (numeric-string buffer-overrun fix), and the official M5Cardputer 1.2.0
Git tag. Upstream still labels that tag's package metadata as 1.1.1; the
PlatformIO registry has no 1.2.0 package. IRremote stays explicitly pinned at
4.7.1. M5Unified 0.2.17 and M5GFX 0.2.22 are retained deliberately: testing
0.2.25/0.2.32 produced 1,337,520-byte normal and 1,341,824-byte web images,
both over the unchanged 1,310,720-byte application slot.

With the retained graphics pins, the normal image is 1,305,728 bytes (4,992
free), and the web image is 1,310,304 bytes (416 free). The web profile has
very little remaining space; keep the post-link limit and check both profiles
after every change. These are Linux build results, not device validation.
Physical keyboard, SD/audio, sleep/wake and live Hermes acceptance remain
separate checks in the device test plan.
