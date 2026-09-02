Import("env")

import gzip
from pathlib import Path


project_dir = Path(env.subst("$PROJECT_DIR"))
generated_dir = Path(env.subst("$BUILD_DIR")) / "generated"
generated_dir.mkdir(parents=True, exist_ok=True)

source = (project_dir / "web" / "admin.html").read_bytes()
payload = gzip.compress(source, compresslevel=9, mtime=0)
rows = []
for offset in range(0, len(payload), 12):
    rows.append("    " + ", ".join(f"0x{value:02x}" for value in payload[offset:offset + 12]))

header = """#pragma once
#include <Arduino.h>

static const uint8_t kAdminPageGzip[] PROGMEM = {
%s
};
static constexpr size_t kAdminPageGzipSize = sizeof(kAdminPageGzip);
""" % ",\n".join(rows)
(generated_dir / "admin_page_gz.h").write_text(header)
env.Append(CPPPATH=[str(generated_dir)])

# The compact web profile needs LTO for the application sources to retain a
# safe OTA margin. Keep third-party libraries out of LTO: several M5GFX
# translation units are built with intentionally different configuration
# macros and whole-program LTO reports (and may act on) false ODR conflicts.
if env.subst("$PIOENV") == "cardputer-adv-hermes-web":
    env.Append(LINKFLAGS=["-flto"])
