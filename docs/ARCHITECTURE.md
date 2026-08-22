# Architecture

The firmware is a direct Hermes Dashboard client optimized for a small ESP32-S3 device. It does not introduce a companion relay or store a second copy of the conversation.

```mermaid
flowchart LR
    K[Keyboard and microphone] --> UI[UI state machine]
    UI --> HC[Hermes client]
    HC -->|REST login| D[Hermes Dashboard]
    HC -->|WS ticket| D
    HC <-->|WSS events| D
    UI --> A[Audio and TTS]
    UI --> S[SD configuration]
    S --> HC
    S --> WA[Local admin panel]
```

## Connection lifecycle

1. Load configuration and initialize hardware.
2. Join Wi-Fi.
3. Reuse a configured cookie or authenticate with credentials.
4. Request a short-lived, single-use ticket from `/api/auth/ws-ticket`.
5. Upgrade to `/api/ws?ticket=…`.
6. Fetch sessions and subscribe to live events.
7. Refresh authentication and reconnect with bounded backoff when needed.

Every long operation is represented in the UI state machine. Session loading and connection attempts remain cancellable, and failures retain their stage and HTTP/TLS/WebSocket detail for Status.

## Rendering and audio

Regular screens use a stable framebuffer and dirty-region updates. Sleep blinking changes only eye pixels at a fixed origin. The recorder temporarily owns the audio peripherals, finalizes the upload, then mutes and releases the speaker path; TTS uses the same serialized ownership model.

## Trust boundaries

- SD card: Wi-Fi, Hermes credentials, optional private CA, preferences.
- RAM: active cookie, one-time ticket, current transcript window.
- Hermes Dashboard: authoritative session and message history.
- Local admin: configuration and diagnostics; secrets are never returned in full.

See [Hermes protocol notes](HERMES_PROTOCOL.md) for endpoint behavior.
