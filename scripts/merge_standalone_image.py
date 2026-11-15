Import("env")

# Standalone profile only: join bootloader, partition table, OTA selector and
# the application into one image that is written at offset 0x0. The M5Apps
# profiles never produce this file; their firmware.bin stays application-only.

MERGED_NAME = "hermes-standalone-full.bin"


def merge_image(source, target, env):
    board = env.BoardConfig()
    images = []
    for address, path in env.get("FLASH_EXTRA_IMAGES", []):
        images += [str(address), '"%s"' % env.subst(str(path))]
    images += [env.subst("$ESP32_APP_OFFSET"), '"%s"' % str(target[0])]
    flash_mode = board.get("build.flash_mode", "dio")
    # The second-stage loader switches to quad mode itself; the header mode
    # must stay one the ROM loader can read.
    if flash_mode in ("qio", "qout"):
        flash_mode = "dio"
    command = " ".join([
        '"$PYTHONEXE"', '"$OBJCOPY"',
        "--chip", board.get("build.mcu", "esp32s3"),
        "merge_bin",
        "-o", '"$BUILD_DIR/%s"' % MERGED_NAME,
        "--flash_mode", flash_mode,
        "--flash_freq", "${__get_board_f_flash(__env__)}",
        "--flash_size", board.get("upload.flash_size", "8MB"),
    ] + images)
    if env.Execute(command):
        raise RuntimeError("could not merge the standalone image")
    print("Standalone full-device image: %s (write at 0x0)"
          % env.subst("$BUILD_DIR/%s" % MERGED_NAME))


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_image)
