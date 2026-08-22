#!/bin/sh

set -eu

offset=0x180000
partition_size=0x140000
partition_bytes=1310720
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
firmware=${2:-"$repo_dir/.pio/build/cardputer-adv-hermes/firmware.bin"}

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 /dev/cu.usbmodemNNNN [firmware.bin]" >&2
    exit 2
fi

port=$1
case "$port" in
    /dev/cu.usbmodem*|/dev/cu.usbserial*|/dev/tty.usbmodem*|/dev/tty.usbserial*) ;;
    *)
        echo "Refusing unexpected serial path: $port" >&2
        exit 1
        ;;
esac

if [ ! -e "$port" ]; then
    echo "Serial device is not present: $port" >&2
    exit 1
fi
if [ ! -f "$firmware" ]; then
    echo "Firmware image is missing: $firmware" >&2
    exit 1
fi

image_size=$(wc -c < "$firmware" | tr -d '[:space:]')
if [ "$image_size" -gt "$partition_bytes" ]; then
    echo "Refusing oversized image: $image_size > $partition_bytes bytes" >&2
    exit 1
fi

echo "Hermes-only M5Apps update"
echo "  port:      $port"
echo "  image:     $firmware"
echo "  size:      $image_size / $partition_bytes bytes"
echo "  partition: $offset + $partition_size"
shasum -a 256 "$firmware"

if [ "${HERMES_FLASH_CONFIRM:-}" != "YES" ]; then
    printf "Type HERMES to erase and replace only this partition: "
    IFS= read -r confirmation
    if [ "$confirmation" != "HERMES" ]; then
        echo "Cancelled."
        exit 1
    fi
fi

uvx --from esptool esptool --chip esp32s3 --port "$port" --baud 921600 \
    erase-region "$offset" "$partition_size"
uvx --from esptool esptool --chip esp32s3 --port "$port" --baud 921600 \
    write-flash "$offset" "$firmware"
uvx --from esptool esptool --chip esp32s3 --port "$port" --baud 921600 \
    verify-flash "$offset" "$firmware"
uvx --from esptool esptool --chip esp32s3 --port "$port" run

echo "Hermes partition flashed and verified; adjacent M5Apps were untouched."
