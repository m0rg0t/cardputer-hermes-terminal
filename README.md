<div align="center">
  <img src="docs/assets/hermes-ready.png" width="118" alt="Hermes portrait">
  <h1>Cardputer Hermes Terminal</h1>
  <p><strong>A native pocket terminal for Nous Research Hermes Agent.</strong></p>
  <p>Sessions, chat, voice, spoken replies, status, and administration — directly from an M5Stack Cardputer ADV.</p>

  [![Firmware CI](https://github.com/m0rg0t/cardputer-hermes-terminal/actions/workflows/ci.yml/badge.svg)](https://github.com/m0rg0t/cardputer-hermes-terminal/actions/workflows/ci.yml)
  [![GitHub Pages](https://github.com/m0rg0t/cardputer-hermes-terminal/actions/workflows/pages.yml/badge.svg)](https://m0rg0t.github.io/cardputer-hermes-terminal/)
  [![License: MIT](https://img.shields.io/badge/license-MIT-b93a40.svg)](LICENSE)
  ![Target: Cardputer ADV](https://img.shields.io/badge/target-Cardputer%20ADV-23262b.svg)
</div>

<table>
  <tr>
    <td><img src="docs/images/screens/sessions-list.png" alt="Session list with 143 cached sessions" width="240"></td>
    <td><img src="docs/images/screens/chat-long.png" alt="Chat transcript with word-wrapped Hermes reply" width="240"></td>
    <td><img src="docs/images/screens/approval.png" alt="Tool approval request" width="240"></td>
  </tr>
  <tr>
    <td><img src="docs/images/screens/recording.png" alt="Voice recording with level meter" width="240"></td>
    <td><img src="docs/images/screens/help-manual.png" alt="On-device key manual" width="240"></td>
    <td><img src="docs/images/screens/sleep-ready.png" alt="Hermes portrait sleep screen" width="240"></td>
  </tr>
</table>

These frames are rendered by the firmware's own drawing code through the
[desktop preview](sim/README.md); regenerate them with
`python3 sim/render_docs.py`. All 36 scripted screens are in
[docs/images/screens](docs/images/screens).

Cardputer Hermes Terminal is a purpose-built ESP32-S3 client for a self-hosted Hermes Agent Dashboard. It reproduces the Desktop connection flow on-device: authenticate over REST, request a short-lived WebSocket ticket, then connect to Hermes directly — without a relay service or notes recorder.

## What it does

- Browse, create, and resume Hermes sessions.
- Send keyboard messages or record a voice prompt.
- Read the latest Hermes reply and optionally play it with TTS.
- Show explicit connecting, loading, ready, and error states.
- Cancel slow session loads instead of locking the interface.
- Open Help, Settings, and Status with a long press of the Go button.
- Cache thousands of sessions and their history on SD while keeping only a
  small visible window in RAM.
- Configure Wi-Fi and Hermes from the SD card or an optional local admin panel,
  or scan and join a Wi-Fi network from the device settings.
- Display a flicker-free Hermes portrait sleep screen with status.
- Render Cyrillic titles, replies, and Wi-Fi names with a built-in 5x7 glyph
  set (optional at build time).
- Preserve the M5Apps launcher by flashing only the application slot.

The visual language borrows from late Shōwa and early Heisei portable electronics: dark graphite panels, warm-white type, precise red accents, and small status lamps — adapted for a readable 240×135 display.

## Quick start

### 1. Build the firmware

Requirements: PlatformIO 6.1+, Python 3.10+, and a Cardputer ADV.

```bash
git clone git@github.com:m0rg0t/cardputer-hermes-terminal.git
cd cardputer-hermes-terminal
platformio run
```

The application image is produced at `.pio/build/cardputer-adv-hermes/firmware.bin`.

The default profile prioritizes the full terminal, voice, animated Hermes
states, and the SD cache. It does not include the LAN web panel. To build the
optional Web profile, which uses the static READY portrait to fit the exact
M5Apps application slot:

```bash
platformio run -e cardputer-adv-hermes-web
```

Its image is written to `.pio/build/cardputer-adv-hermes-web/firmware.bin`.

### 2. Prepare the SD card

```bash
cp sdcard/HERMES.CFG.example /Volumes/YOUR_SD/HERMES.CFG
```

Minimum configuration:

```ini
wifi_ssid=YOUR_WIFI
wifi_password=YOUR_WIFI_PASSWORD
hermes_base_url=https://hermes.example.com
hermes_login_username=YOUR_USERNAME
hermes_login_password=YOUR_PASSWORD
```

`HERMES.CFG`, certificates, recordings, and flash backups are ignored by Git. Never commit credentials.

### 3. Install safely

> [!IMPORTANT]
> `firmware.bin` is an **application image**, not a full-device image. Never write it at address `0x000000`; doing so overwrites the bootloader and M5Apps launcher.

```bash
./scripts/flash_hermes_m5apps.sh /dev/cu.usbmodemNNNN
```

The installer checks the chip, partition layout, image size, and target offset before writing. See [Safe flashing](docs/SAFE_FLASHING.md) for the recovery-safe workflow.

## Controls

| Context | Key | Action |
|---|---|---|
| Session list | `↑` / `↓` | Select a session |
| Session list | `Enter` | Open selected session |
| Session list | `N` | New text session |
| Session list | `V` | New session from voice |
| Session list | `R` | Refresh from Hermes |
| Terminal | `T` | Type a message |
| Terminal | `V` | Record a voice message |
| Terminal | `R` | Speak the latest Hermes reply |
| Terminal | `/` | Open the composer with a slash command |
| Terminal | `S` / `X` | Steer the running turn / stop it |
| Terminal | `B` / `C` / `U` | Branch, compact, or undo the session |
| Terminal | `↑` / `↓` | Scroll; loads older cached history at the top |
| Settings / Status | `W` | Scan and join a Wi-Fi network |
| Anywhere | `Z` | Sleep screen (list and terminal) |
| Anywhere | `Esc` | Back or cancel active loading |
| Anywhere | Long-press Go | Help, Settings, and Status |

See the full [Keyboard reference](docs/KEYBOARD_REFERENCE.md).

## Documentation

| Guide | Purpose |
|---|---|
| [Getting started](docs/GETTING_STARTED.md) | Build, SD preparation, first connection |
| [Configuration](docs/CONFIGURATION.md) | Wi-Fi, authentication, TLS, audio, admin panel |
| [Safe flashing](docs/SAFE_FLASHING.md) | Install without overwriting M5Apps |
| [Keyboard reference](docs/KEYBOARD_REFERENCE.md) | Every screen and shortcut |
| [Architecture](docs/ARCHITECTURE.md) | Firmware modules and connection flow |
| [Hermes protocol notes](docs/HERMES_PROTOCOL.md) | Dashboard REST and WebSocket behavior |
| [Device test plan](docs/DEVICE_TEST_PLAN.md) | Hardware acceptance checklist |
| [Release checklist](docs/RELEASE_CHECKLIST.md) | Repeatable release process |
| [Desktop preview](sim/README.md) | Render every screen on a desktop without flashing |

## Authentication model

```text
credentials or session cookie
        ↓
authenticated REST session
        ↓
POST /api/auth/ws-ticket
        ↓
single-use 30-second ticket
        ↓
WSS /api/ws?ticket=…
```

HTTPS is mandatory. The firmware rejects a `hermes_base_url` that does not start with `https://`, so credentials and cookies never travel in clear text.

## Project boundaries

This is a focused Hermes terminal. It does not record notes, proxy conversations through a custom cloud service, or replace Hermes Agent itself. The optional local web panel is limited to device configuration and diagnostics.

## Cache and offline roadmap

The SD cache is read-through, not a second source of truth. Session indexes and
message pages are downloaded cooperatively, committed transactionally, checked
with CRC32, and rendered from a 3 KiB window. Live assistant deltas are spooled
to SD in bounded chunks and interrupted writes are recoverable. Hermes remains
authoritative whenever the link returns.

A future offline mode may add an explicit outbox for prompts composed without a
connection. It will require visible queued/failed states, user-controlled retry
and deletion, idempotency keys, and conflict handling after server history has
changed. The current firmware deliberately does **not** send cached text later
without confirmation.

## Contributing

Bug reports, protocol findings, UI refinements, and hardware testing are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. Please report security concerns using [SECURITY.md](SECURITY.md).

## Credits and license

Hermes Agent is developed by [Nous Research](https://github.com/NousResearch). M5Stack and Cardputer are trademarks of their respective owners. This independent client is not an official Nous Research or M5Stack product.

Released under the [MIT License](LICENSE).
