# Configuration

Runtime settings are loaded from `/HERMES.CFG` on the microSD card. The real file is ignored by Git; only `sdcard/HERMES.CFG.example` belongs in the repository.

## Core settings

```ini
wifi_ssid=YOUR_WIFI
wifi_password=YOUR_WIFI_PASSWORD
hermes_base_url=https://hermes.example.com
auth_mode=password
hermes_username=YOUR_USERNAME
hermes_password=YOUR_PASSWORD
```

`hermes_base_url` is the Dashboard origin, without `/api/ws` or another API path.

## Authentication

Password mode signs into the Dashboard, keeps the returned session cookie in memory, and requests a one-time WebSocket ticket.

```ini
auth_mode=password
hermes_username=YOUR_USERNAME
hermes_password=YOUR_PASSWORD
```

Cookie mode accepts the complete cookie name and value. A token without the name is not sufficient.

```ini
auth_mode=cookie
hermes_session_cookie=hermes_session_at=REPLACE_ME
```

## TLS and plain HTTP

HTTPS is recommended. To trust a private certificate authority, place its PEM certificate at `/HERMES_CA.PEM` on the SD card.

Plain HTTP can expose credentials and cookies to anyone observing the LAN. It remains blocked unless the explicit insecure-LAN option in `HERMES.CFG.example` is enabled. Never use it over the public Internet.

## Display, sleep, and audio

The device settings screen controls brightness, idle timeout, sleep animation, motion, interface sounds, speaker volume, and TTS. Use `←` / `→` to change a selected value and `↑` / `↓` to move between rows. `AUDIO / ALERTS = OFF` is a master mute for every interface-generated sound, including startup, connection, session-entry, attention, and speaker-test tones. Hermes speech/TTS remains available. Enabling alerts plays one short preview note.

With alerts enabled, startup uses a short two-note cue, a single note confirms a Hermes connection, and a rising two-note cue confirms entry into a session. Voice capture exclusively owns the audio path while active, then releases and mutes it to avoid residual speaker hum.

## Local admin panel

The optional panel is intended for configuration and diagnostics on a trusted local network. It advertises its configured mDNS hostname and also shows the numeric IP in Status. It must never return full Hermes credentials to the browser or logs.

## Credential hygiene

- Keep `HERMES.CFG` and `HERMES_CA.PEM` out of Git.
- Do not paste cookies or passwords into issues or serial logs.
- Rotate a credential immediately if exposed.
- Prefer a dedicated least-privilege Hermes account.
- Remove or protect the SD card before lending the device.
