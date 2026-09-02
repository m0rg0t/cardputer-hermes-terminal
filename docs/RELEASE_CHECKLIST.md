# Hermes Pocket Terminal release checklist

This checklist is intentionally split into build evidence and physical-device
evidence. A successful build alone does not certify LCD, microphone, codec, or
gateway behavior.

## Proven before flashing

- [x] Native WebSocket frame, streamed-text, and cache-rule tests pass.
- [x] Release firmware compiles for M5Stack StampS3 / Cardputer ADV.
- [x] The post-build guard enforces the exact `0x140000` Hermes M5Apps slot.
- [x] Default cache-first image is checked against the M5Apps slot after every
      build; record the exact release size and SHA-256 below before flashing.
- [x] Release image size: `1,281,856 bytes` (`28,864 bytes` remain in the
      `0x140000` M5Apps slot).
- [x] Release SHA-256:
      `61e0d22f60d70e9714c260a5a9cd4712f9ca76665ccedb2771374deecc7c85de`.
- [x] The guarded updater accepts only USB serial paths and always uses
      offset `0x180000`, length `0x140000`, followed by `verify-flash`.
- [x] The oversized ESPmDNS variant is not exposed as a release target; the
      optional Web build uses the numeric IP shown on Status.
- [x] READY uses the supplied official Hermes portrait; BLINK changes only the
      visible eye region at the same fixed origin. Green is limited to LINK LEDs.
- [x] UI settings use a temporary file and backup recovery; secret-bearing
      `/HERMES.CFG` is not modified.

## Flash verification

- [x] Detect the Cardputer as `/dev/cu.usbmodem*` or `/dev/cu.usbserial*`.
- [x] Run `scripts/flash_hermes_m5apps.sh PORT`.
- [x] Record successful `verify-flash` output for the Hermes image.
- [x] Read back Agent Console and BrokenSignal; both SHA-256 digests still
      match their recorded binaries.
- [ ] Boot M5Apps and confirm Hermes, Agent Console, and BrokenSignal remain
      listed; launch Hermes without rewriting either adjacent app.

## Physical UI and audio verification

- [ ] Session selection uses warm white text on dark burgundy and remains
      readable at normal viewing distance.
- [ ] Long Go opens Help; the NAV/CREATE/CHAT/CONTROL key cards are distinct
      and fully readable; Settings and Status remain navigable on 240x135.
- [ ] In Settings, the physical left/right keys decrement/increment the selected
      value, matching the visible `< value >` affordance.
- [ ] Sleep shows the official portrait when READY, generated WORKING state
      during activity, and a short IDLE blink without green fringe or flashing.
- [ ] Sleep footer shows `ANY KEY TO WAKE` on one line and the empty-session
      context reads `SESSION LIST` without clipping or ambiguous abbreviation.
- [ ] Recording redraws as one buffered frame and the 12-segment VU follows
      microphone level without LCD blinking.
- [ ] Finishing/cancelling recording leaves the codec silent with no hum.
- [ ] Failed STT keeps one retryable SD recording; `V` retries it, `Del`
      discards it, and successful transcription removes it.
- [ ] A failed post-audio `session.resume` preserves the WAV/transcript and
      `V`/`Enter` retries recovery against the same durable session.
- [ ] `Esc` cancels the audio authentication refresh, STT upload/wait, and TTS
      synthesis/playback without waiting for the long HTTP timeout.
- [ ] `V` in the picker creates a new session and immediately starts recording.
- [ ] `P` in chat speaks the latest complete Hermes response, then reconnects.
- [ ] Microphone, speaker, and reconnect actions work from Status.

## Remote Hermes verification

- [ ] Password login obtains an authenticated cookie without displaying it.
- [ ] `POST /api/auth/ws-ticket` succeeds and the one-time ticket opens `/api/ws`.
- [ ] Session list, resume, new text session, and voice-first session work.
- [ ] Before Hermes connects, the picker shows `CONNECTING TO HERMES`; after
      connection it shows `LOADING SESSIONS`; `N NEW` appears only after a
      confirmed empty list. Connection/auth/list failures show diagnostics and
      `R RETRY`.
- [ ] During a slow session resume, `ESC` immediately returns to the session
      list; a late resume response does not reopen the cancelled session.
- [ ] Voice transcription completes without `HTTPClient -1`; a successful
      transcript is preserved for sending if WebSocket reconnection is delayed.
- [ ] Approval/question UI wakes the screen; alert sound remains off by default.
- [ ] Optional Web build fits the same M5Apps slot and enforces Basic auth.
- [ ] Cache cancellation, CRC quarantine, reset recovery, quota eviction, and a
      larger-than-64-KiB message pass the SD cache device plan.
