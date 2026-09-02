# Configuration

Runtime settings are loaded from `/HERMES.CFG` on the microSD card. The real file is ignored by Git; only `sdcard/HERMES.CFG.example` belongs in the repository.

## Core settings

```ini
wifi_ssid=YOUR_WIFI
wifi_password=YOUR_WIFI_PASSWORD
hermes_base_url=https://hermes.example.com
hermes_login_username=YOUR_USERNAME
hermes_login_password=YOUR_PASSWORD
```

`hermes_base_url` is the Dashboard origin, without `/api/ws` or another API path.

## Authentication

The authentication mode is inferred from which keys are present: login
credentials select password mode, `hermes_session_cookie` selects cookie mode,
and `hermes_session_token` selects token mode. There is no separate
`auth_mode` key.

Password mode signs into the Dashboard, keeps the returned session cookie in memory, and requests a one-time WebSocket ticket.

```ini
hermes_login_username=YOUR_USERNAME
hermes_login_password=YOUR_PASSWORD
```

Cookie mode accepts the complete cookie name and value. A token without the name is not sufficient.

```ini
hermes_session_cookie=hermes_session_at=REPLACE_ME
```

## Build-time options

| Flag | Default | Effect |
|---|---|---|
| `HERMES_WEB_ADMIN` | 0 (web profile: 1) | LAN admin panel |
| `HERMES_WIFI_SETUP` | 1 (web profile: 0) | Wi-Fi scan/join screen |
| `HERMES_CYRILLIC_FONT` | 1 | 5x7 Cyrillic glyphs for titles, transcript, and Wi-Fi names; set 0 to show `?` instead |
| `HERMES_SLEEP_STATES` | 1 (web profile: 0) | Animated WORK/blink sleep portraits |

Set them in `platformio.ini` `build_flags`, or override for one build with
`PLATFORMIO_BUILD_FLAGS="-DHERMES_CYRILLIC_FONT=0" platformio run`.

## Wi-Fi from the device

The default profile can join a network without editing the card. Open
Settings or Status (long-press Go) and press `W`: the terminal scans nearby
2.4 GHz access points, lists them by signal with a lock marker, and joins the
selected one after you type its key (`Enter` on an open network joins at
once). Every successful join is remembered in `/.HERMES-WIFI.CFG` on the SD
card (most recent first, up to eight), so the device knows several networks:

- `HERMES.CFG` can also list extra networks as `wifi_ssid_2` /
  `wifi_password_2` up to `_9`, next to the primary `wifi_ssid`.
- At boot the most recently joined network is tried first. While the device
  is not connected it scans every 30 seconds and joins the strongest known
  network in range, so moving between home, office, and a phone hotspot
  needs no configuration change.
- In the Wi-Fi list known networks are marked `SAVED`; `Enter` on one joins
  with its stored key, and `Del` forgets a learned network (entries from
  `HERMES.CFG` cannot be removed from the device).

A failed or cancelled join falls back to the previous network. The compact
Web profile omits this screen and the multi-network logic to stay inside the
application slot (`HERMES_WIFI_SETUP=0`); it uses `wifi_ssid` only.

## TLS

HTTPS is mandatory: `hermes_base_url` must start with `https://`, and the
firmware rejects any other scheme at startup. To trust a private certificate
authority, place its PEM certificate at `/HERMES_CA.PEM` on the SD card.

## Display, sleep, and audio

The device settings screen controls brightness, idle timeout, sleep animation, motion, interface sounds, speaker volume, and TTS. Use `←` / `→` to change a selected value and `↑` / `↓` to move between rows. `AUDIO / ALERTS = OFF` is a master mute for every interface-generated sound, including startup, connection, session-entry, attention, and speaker-test tones. Hermes speech/TTS remains available. Enabling alerts plays one short preview note.

With alerts enabled, startup uses a short two-note cue, a single note confirms a Hermes connection, and a rising two-note cue confirms entry into a session. Voice capture exclusively owns the audio path while active, then releases and mutes it to avoid residual speaker hum.

## Local admin panel

The optional panel is intended for configuration and diagnostics on a trusted
local network. It must never return full Hermes credentials to the browser or
logs. The standard cache-first firmware omits the HTTP server to preserve space
for terminal, voice, and animated sleep states.

Build the Web profile and open the numeric IP shown on Status:

```bash
platformio run -e cardputer-adv-hermes-web
```

The Web profile uses a static READY portrait on the sleep screen to remain
inside the M5Apps slot. Switching back requires no source change: build the
standard `cardputer-adv-hermes` environment again.

The source retains the `HERMES_MDNS` compile-time switch for future work, but
no release profile enables it. ESPmDNS makes the current Web image exceed the
fixed M5Apps application slot, so restoring discovery requires either a new
size budget or a smaller responder implementation.

## SD cache

Cache settings are changed on-device under Settings:

- `CACHE`: enable or disable read-through caching without deleting data.
- `CACHE MB`: 8, 32, or 128 MiB quota.
- `C CLEAR`: two-step destructive confirmation.

The cache lives under `/.HERMES-CACHE/` in a namespace derived from the exact
Dashboard URL and Hermes profile. It contains no Wi-Fi or Hermes credentials.
Do not edit cache files while the terminal is running. Corrupt history pairs
are quarantined and replaced by the next online synchronization.

## Credential hygiene

- Keep `HERMES.CFG` and `HERMES_CA.PEM` out of Git.
- Do not paste cookies or passwords into issues or serial logs.
- Rotate a credential immediately if exposed.
- Prefer a dedicated least-privilege Hermes account.
- Remove or protect the SD card before lending the device.
