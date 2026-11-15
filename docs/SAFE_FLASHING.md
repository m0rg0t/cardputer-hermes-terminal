# Safe flashing

Cardputer Hermes Terminal is normally installed alongside M5Apps. The generated `firmware.bin` contains only the application; it does not contain a bootloader or partition table.

> Never flash `.pio/build/cardputer-adv-hermes/firmware.bin` at `0x000000`.

Address zero contains boot and partition data. Writing an application there can remove the launcher until a complete factory image is restored.

## Recommended workflow

```bash
./scripts/flash_hermes_m5apps.sh /dev/cu.usbmodemNNNN
```

The script verifies the chip family, expected layout, application-slot offset, image type and size, then reads the written region back for comparison.

## Before the first installation

1. Keep stable USB power connected.
2. Back up the existing full flash.
3. Save and verify the backup outside the SD card.
4. Confirm the selected serial port belongs to the Cardputer.
5. Close serial monitors holding the port.

Flash backups are ignored by Git because they may contain credentials and application data.

## Standalone image (no launcher)

The `cardputer-adv-hermes-standalone` environment builds Hermes as the only
firmware on the device. It carries its own bootloader and the stock 8 MB
partition table, whose 3264 KiB application slot has room for every optional
feature at once: Wi-Fi setup, animated sleep states, the LAN web panel and
mDNS.

```bash
platformio run -e cardputer-adv-hermes-standalone
```

The build writes two files to `.pio/build/cardputer-adv-hermes-standalone/`:

| File | Offset | Use |
|---|---|---|
| `hermes-standalone-full.bin` | `0x0` | Whole device: bootloader, partition table and application |
| `firmware.bin` | `0x10000` | Application only, for a device that already uses the stock 8 MB layout |

> Writing `hermes-standalone-full.bin` erases M5Apps and every app installed
> through it. Back up the full flash first; the only way back is restoring
> that backup or the official M5Stack image.

```bash
uvx --from esptool esptool --chip esp32s3 --port /dev/cu.usbmodemNNNN \
    --baud 921600 write-flash 0x0 \
    .pio/build/cardputer-adv-hermes-standalone/hermes-standalone-full.bin
```

This image is the only one in the repository that may be written at `0x0`.
It is built and size-checked in CI but has not been through the device test
plan.

## Recovery

If the launcher no longer starts, restore a known-good full-flash backup or the official M5Stack image using its documented offsets. A full restore is different from installing this repository's application image.
