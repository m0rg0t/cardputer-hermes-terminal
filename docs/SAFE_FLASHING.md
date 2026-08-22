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

## Recovery

If the launcher no longer starts, restore a known-good full-flash backup or the official M5Stack image using its documented offsets. A full restore is different from installing this repository's application image.
