# Desktop UI preview

`native-sim` renders the real device screens on a Mac or Linux desktop. It
compiles `src/app_draw.cpp`, the same drawing code that runs on the Cardputer,
against the M5GFX SDL backend and scripts `App`'s private state for each
screen. Networking, SD, audio, and the keyboard are replaced by the small
stubs in `sim/stubs/` and `src/sim/harness.cpp`, so what you see is the real
layout and copy, not a mock-up.

The harness is not a device target: `pio run` and CI build only the firmware
profiles, and `src/sim/` is excluded from them.

## Requirements

- PlatformIO
- SDL2 (`brew install sdl2` on macOS, `libsdl2-dev` on Debian/Ubuntu)
- Python 3 with Pillow for the documentation export

## Interactive window

```sh
pio run -e native-sim
.pio/build/native-sim/program
```

LEFT/RIGHT (or SPACE) switch scenarios, hold A to auto-cycle, ESC quits. The
window title names the current scenario. `--list` prints scenario names.

## Export screens for docs and the site

```sh
python3 sim/render_docs.py            # docs/images/screens + site/assets/screens
python3 sim/render_docs.py --out /tmp/shots --sheet /tmp/sheet.png --sheet-scale 2
python3 sim/render_docs.py --only chat-long approval
```

Frames are 240 x 135 pixels scaled 3x with nearest-neighbour resampling so the
pixels stay crisp. Re-run the export whenever `src/app_draw.cpp` changes and
commit the PNGs with the change.

## Adding a scenario

Scenarios live in `SimAccess::scenarios()` in `src/sim/harness.cpp`. Each one
calls `reset()`, sets the fields the screen reads, and is rendered with the
real `App::draw()`. Members whose definitions live in hardware-heavy units
(`HermesClient::connected()`, `VoiceCapture::elapsedMs()`, ...) are mirrored
in the harness from a `FakeDevice` struct; keep them in step with the device
implementation when those change. Use realistic data: long titles, error and
empty states, non-Latin text, and full transcripts are what find layout bugs.

## Limits

- Keyboard input, audio, storage, and networking are not simulated; the
  preview does not exercise `serviceInput()`.
- The SDL backend has no Cardputer bezel, so frames are bare.
- `millis()` is frozen in `--shots` mode so exports are deterministic.
