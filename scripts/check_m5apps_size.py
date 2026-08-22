Import("env")
import os

MAX_M5APPS_APP_BYTES = 0x140000


def check_size(source, target, env):
    firmware = str(target[0])
    size = os.path.getsize(firmware)
    remaining = MAX_M5APPS_APP_BYTES - size
    print(
        "M5Apps image: {} / {} bytes ({} bytes remaining)".format(
            size, MAX_M5APPS_APP_BYTES, remaining
        )
    )
    if remaining < 0:
        raise RuntimeError(
            "firmware.bin exceeds the Hermes 0x140000-byte app slot by {} bytes".format(
                -remaining
            )
        )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_size)
