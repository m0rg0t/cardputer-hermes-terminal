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
