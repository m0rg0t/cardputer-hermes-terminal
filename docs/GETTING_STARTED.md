# Getting started

This guide takes a Cardputer ADV from source checkout to its first Hermes session while preserving the existing M5Apps launcher.

## Requirements

- M5Stack Cardputer ADV (ESP32-S3)
- FAT32-formatted microSD card and USB data cable
- Python 3.10+ and PlatformIO Core 6.1+
- A reachable Hermes Dashboard instance

## Build and test

```bash
git clone git@github.com:m0rg0t/cardputer-hermes-terminal.git
cd cardputer-hermes-terminal
platformio run
./scripts/test_native.sh
```

The application image is `.pio/build/cardputer-adv-hermes/firmware.bin`.

## Configure the SD card

Copy `sdcard/HERMES.CFG.example` to the card root as `HERMES.CFG`, then replace the placeholders:

```ini
wifi_ssid=YOUR_WIFI
wifi_password=YOUR_WIFI_PASSWORD
hermes_base_url=https://hermes.example.com
auth_mode=password
hermes_username=YOUR_USERNAME
hermes_password=YOUR_PASSWORD
```

For cookie auth, TLS certificates, the local admin panel, and audio controls, see [Configuration](CONFIGURATION.md).

## Install

```bash
./scripts/flash_hermes_m5apps.sh /dev/cu.usbmodemNNNN
```

The script refuses unsafe offsets, oversized images, unexpected chips, and unrecognized partition layouts. Read [Safe flashing](SAFE_FLASHING.md) before using another method.

## First boot

The expected sequence is hardware initialization, Wi-Fi connection, Hermes authentication, WebSocket ticket request, and an explicit `ONLINE` or actionable error state. The session list is unavailable while the link is still being established. Press `Esc` to cancel a slow operation; long-press Go to inspect Status.

## First session

- Press `N` to create a text-first session.
- Press `V` to record the first prompt and create a voice-first session.
- Select an existing session with `↑` / `↓`, then press `Enter`.
- Inside a session, press `T` to type, `V` to speak, and `R` to read the latest reply.

## Troubleshooting

| Symptom | Check |
|---|---|
| Wi-Fi error | SSID, password, 2.4 GHz availability, signal strength |
| Authentication error | `auth_mode`, credentials, or current session cookie |
| Ticket HTTP error | Dashboard URL, TLS trust, `/api/auth/ws-ticket` |
| WebSocket error | Reverse-proxy WebSocket support and certificate chain |
| Voice upload error | Online state, ticket refresh, upload endpoint, free heap |
| No sound | Settings → Audio, volume, TTS mode, speaker connection |
