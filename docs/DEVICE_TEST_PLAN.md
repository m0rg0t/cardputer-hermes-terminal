# Device acceptance plan

This checklist verifies the layers that a host build cannot exercise: the
Cardputer ADV peripherals, M5Apps boot path, Wi-Fi/TLS environment, and a live
authenticated Hermes server.

## Prepare

1. Run a current `hermes serve` instance behind HTTPS. Record the exact public
   base URL and export its issuing CA certificate in PEM format.
2. Copy `sdcard/HERMES.CFG.example` to `/HERMES.CFG` on a FAT32 microSD card.
   Set Wi-Fi, the HTTPS base URL, and exactly one Hermes authentication mode.
   For a `basic` gateway, use `hermes_login_username` and
   `hermes_login_password`; Cookie and token modes remain alternatives.
3. Copy the PEM certificate to `/HERMES_CA.PEM` (or change
   `hermes_ca_path`). Never put provider API keys on the device.
4. Build with `uvx platformio run`, then copy
   `.pio/build/cardputer-adv-hermes/firmware.bin` to the card.
5. Install that app-only binary with **M5Apps > Installer > SD**. Do not flash
   it at offset `0x0000`.

If the connected board uses the standard dual-OTA table rather than M5Apps,
inspect and back up its entire flash before installing. A compatible table has
`app0` at `0x10000` and `app1` at `0x340000`, each 3264 KiB. Install only into
the inactive slot and switch OTA selection after verifying the written image;
never overwrite the active slot merely because a serial port is visible.

## Core terminal

1. Boot and confirm the display reaches `HERMES CONNECTED` and lists the same
   durable sessions as Hermes Desktop/TUI.
2. Create a session, send a short typed prompt, and verify streamed assistant
   text plus completion. Reboot, resume the session, and verify its history.
3. Run a harmless tool call and confirm start/progress/completion events are
   visible. Trigger one approval and verify `once`, `session` (when offered),
   and `deny` each produce the expected server result.
4. Trigger single- and multi-question clarification, including a multi-select
   response. Trigger sudo/secret only with test credentials, then confirm the
   entered value never appears in the timeline or web page.
5. Exercise interrupt, steer, branch, compress, and undo. Run `/model` (or
   another installed command) to verify `slash.exec`; test an alias if one is
   configured.
6. Restart Hermes or interrupt Wi-Fi, then confirm the terminal reconnects and
   resumes the selected durable session without duplicating the last prompt.

## SD cache and large histories

1. Sync an account with at least 2,000 sessions. Confirm the list scrolls in
   fixed-size windows and free heap does not fall as the total count grows.
2. Open a session with thousands of messages and one message larger than 64
   KiB. Confirm the screen becomes usable from the latest cached window before
   the full oldest-first sync completes.
3. Press `Esc` during both session-index and history downloads. Confirm the UI
   responds immediately and the previous committed cache remains readable.
4. Reset the device during history commit and during a streamed assistant
   response. Confirm pair recovery prevents mixed history/view generations and
   the partial assistant spool is marked interrupted on the next response.
5. Corrupt one byte in a cached `.view` file on another computer. Confirm CRC
   verification quarantines the pair and online sync replaces it.
6. Set each quota value, exceed it, and confirm least-recently-opened sessions
   are removed while the open session remains intact. Verify `C CLEAR` requires
   the second confirmation press.

## Audio

1. Record a short push-to-talk prompt and confirm the transcript becomes a
   prompt (or editable text if reconnect is still underway).
   Force one upload/server failure: confirm the screen offers `V RETRY`, that
   retry reuses the SD recording, and that `Del` discards it.
   Also force `session.resume` to fail once: the WAV or completed transcript
   must remain attached to the durable session, and `V`/`Enter` must retry the
   resume instead of dropping the user into an unusable compose screen.
2. Speak the last response and test both a WAV-capable and an MP3-capable TTS
   provider if available. Verify the configured volume and clean playback.
   Press `Esc` once while authentication is refreshing, once during synthesis,
   and once during playback; all must return to chat with an explicit
   cancelled status, and playback cancellation must leave the codec muted.
3. Set `AUDIO / ALERTS` to `OFF`, reboot, reconnect, enter a saved session,
   and trigger an approval or clarification request. Confirm that automatic
   interface cues stay silent while `READ`/TTS still plays normally. Re-enable
   alerts and confirm the preview, connection, session-entry, attention, and
   speaker-test cues.
4. Power-cycle once during recording and once during synthesis. On the next
   boot, verify `/.HERMES-VOICE.wav` and `/.HERMES-TTS.bin` are absent.

## Authentication and optional administration

1. In gated mode, let the access token age past its normal refresh point, then
   reconnect and use STT/TTS. Verify the session stays authenticated and
   `/.HERMES-COOKIE` is present with the configured base URL on its first line.
2. In password mode, remove `/.HERMES-COOKIE`, reboot, and confirm the device
   logs in through `/auth/password-login`, persists both returned cookies, and
   connects. Invalidate the stored session and confirm one automatic re-login;
   verify wrong credentials are retried no more than once per minute.
3. Change `hermes_base_url` deliberately and confirm the old runtime cookie is
   not reused for the new endpoint. Restore the correct URL afterward.
4. With `web_admin=false`, confirm no device HTTP page is available. With it
   enabled on a trusted LAN, confirm the configured Basic Auth username and
   password are required, and test
   status, one prompt, and interrupt. Paste a complete access+refresh Cookie
   through the write-only sign-in form, verify the device reconnects, and
   confirm no cookie, token, password, secret, CA, or audio data is returned by
   the status response. Disable `web_admin` after the test and rotate the
   session if the LAN may have been observed.
5. Build `cardputer-adv-hermes-web`, set `web_admin=true`, and confirm the panel
   is reachable only through the numeric IP shown on Status. Confirm the HTTP
   service disappears after Wi-Fi is disconnected and returns after reconnect.
6. Revoke/rotate the Hermes session and web-admin token after any test that
   exposes or loses the SD card.

Record the firmware SHA-256, Hermes commit/version, authentication mode, TTS
codec, and any failing step before changing configuration; those five facts
make failures reproducible.
