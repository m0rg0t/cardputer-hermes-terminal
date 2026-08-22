# Contributing

Small, hardware-tested changes are especially valuable on a memory-constrained device.

```bash
platformio run
./scripts/test_native.sh
```

Before submitting a change:

1. Keep credentials and device backups out of the commit.
2. Build the Cardputer ADV environment and run native tests.
3. Check the firmware size against the application-slot limit.
4. Exercise the affected flow on hardware when possible.
5. Update the relevant documentation.

For UI work, design for 240×135, maintain high contrast, never use black text on red selection, keep animation geometry fixed, expose loading and errors, make lengthy operations cancellable with `Esc`, and keep the command rail accurate.

Pull requests should explain the user-visible result, test evidence, firmware-size impact, and configuration changes.
