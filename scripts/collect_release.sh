#!/bin/sh
# Copy the built firmware variants into one folder under release names, with
# checksums and install notes. Build all three environments first:
#   platformio run -e cardputer-adv-hermes -e cardputer-adv-hermes-web \
#       -e cardputer-adv-hermes-standalone
# Usage: scripts/collect_release.sh VERSION [OUTPUT_DIR]

set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "Usage: $0 VERSION [OUTPUT_DIR]" >&2
    exit 2
fi

version=$1
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${PLATFORMIO_BUILD_DIR:-"$repo_dir/.pio/build"}
out=${2:-"$repo_dir/dist"}
name="hermes-terminal-$version"

mkdir -p "$out"
copy() {
    if [ ! -f "$build_dir/$1" ]; then
        echo "Missing build output: $build_dir/$1" >&2
        exit 1
    fi
    cp "$build_dir/$1" "$out/$2"
}
copy cardputer-adv-hermes/firmware.bin "$name-m5apps.bin"
copy cardputer-adv-hermes-web/firmware.bin "$name-m5apps-web.bin"
copy cardputer-adv-hermes-standalone/hermes-standalone-full.bin \
    "$name-standalone-full.bin"
copy cardputer-adv-hermes-standalone/firmware.bin "$name-standalone-app.bin"

cd "$out"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$name"-*.bin > SHA256SUMS.txt
else
    shasum -a 256 "$name"-*.bin > SHA256SUMS.txt
fi

cat > RELEASE_NOTES.md <<EOF
## Firmware files

| File | What it is | Where it goes |
|---|---|---|
| \`$name-m5apps.bin\` | Terminal, voice, animated sleep states, Wi-Fi setup | M5Apps application slot (\`0x180000\`), keeps the launcher |
| \`$name-m5apps-web.bin\` | Same slot, with the LAN web panel instead of Wi-Fi setup and animated sleep states | M5Apps application slot (\`0x180000\`), keeps the launcher |
| \`$name-standalone-full.bin\` | Every feature, with its own bootloader and partition table | Offset \`0x0\`. **Erases the launcher and all other apps** |
| \`$name-standalone-app.bin\` | Application part of the standalone build | \`app0\` at \`0x10000\` on a device already using the stock 8 MB layout |

Never write an \`m5apps\` or \`standalone-app\` file at \`0x0\`. Read
[Safe flashing](https://github.com/${GITHUB_REPOSITORY:-m0rg0t/cardputer-hermes-terminal}/blob/main/docs/SAFE_FLASHING.md)
before installing, and check the download against \`SHA256SUMS.txt\`.

EOF
echo "Release files in $out:"
ls -l
