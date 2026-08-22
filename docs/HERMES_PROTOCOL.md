# Hermes protocol contract

This firmware is a native host for the Hermes TUI Gateway protocol exposed by
`hermes serve`. It does not use the OpenAI-compatible chat endpoint and does not
require a Cardputer-specific gateway.

## Transport and authentication

- WebSocket: `wss://HOST[/BASE]/api/ws`
- Wire format: newline-delimited JSON-RPC 2.0, one or more JSON lines per text
  frame
- Gated remote mode: authenticated cookie `POST /api/auth/ws-ticket`, followed
  by a fresh single-use `?ticket=` WebSocket connection. The client probes the
  public `/api/status` endpoint first and rejects an obvious token/cookie mode
  mismatch before opening the socket.
- Loopback/development mode: `?token=` WebSocket connection
- TLS: the endpoint CA is loaded from `/HERMES_CA.PEM`; plaintext Hermes
  connections are rejected

Ticket minting uses a cookie jar so any `Set-Cookie` rotations returned by
Hermes are applied before the next request. The resulting complete Cookie header is
stored transactionally in `/.HERMES-COOKIE`, paired with the exact configured
base URL, and takes precedence over the bootstrap cookie on later boots. The
client refreshes gated authentication immediately before temporarily releasing
the WebSocket for a long audio request. The runtime file is plaintext secret
material. It is never returned by `/api/status`; the optional administration
page can only replace it through the write-only onboarding endpoint described
below.

For a gateway advertising the `basic` provider, `hermes_login_username` and
`hermes_login_password` select password mode. The terminal sends HTTPS JSON
`{provider, username, password, next}` to `POST /auth/password-login`, captures
the returned access and refresh cookies, persists them in the same
endpoint-bound runtime file, and then follows the normal ticket flow. A stored
session is tried first; password login is repeated only after a `401`/`403`,
with failed login attempts throttled locally to protect the server rate limit.
Password mode, bootstrap Cookie mode, and token mode are mutually exclusive in
`HERMES.CFG`.

When `web_admin=true`, the Basic-authenticated LAN page also offers a
write-only Cookie onboarding endpoint at `POST /api/auth/cookie`. It accepts a
complete Cookie header and stores it in the same endpoint-bound runtime file,
then reconnects. The page is plaintext HTTP, so this option is only suitable
for a trusted LAN and should be disabled after provisioning.
The local-panel Basic Auth username comes from `web_admin_username` and the
password from `web_admin_token`; these are separate from the remote Hermes
password-login fields.

The same mode starts mDNS using the configured `hostname` and advertises
`_http._tcp` on port 80. The default URL is `http://hermes-terminal.local/`.

## Implemented gateway calls

`session.list`, `session.create`, `session.resume`, `session.history`,
`session.close`,
`prompt.submit`, `session.interrupt`, `session.steer`, `session.branch`,
`session.compress`, `session.undo`, `slash.exec`, `command.dispatch`, `approval.respond`,
`clarify.respond`, `sudo.respond`, and `secret.respond`.

The client treats the `session_id` returned by `session.resume` as the live
runtime identifier. The durable identifier selected from `session.list` is not
reused for live prompt calls after resume.

## Implemented events

`gateway.ready`, `message.start`, `message.delta`, `message.interim`,
`message.complete`, `reasoning.available`, `reasoning.delta`, `thinking.delta`,
`tool.start`, `tool.generating`, `tool.progress`, `tool.complete`,
`approval.request`, `clarify.request`, `sudo.request`, `secret.request`,
`subagent.start`, `subagent.spawn_requested`, `subagent.thinking`,
`subagent.progress`, `subagent.tool`, `subagent.complete`, `status.update`,
`session.info`, `session.usage`, `session.resume_progress`, `sessions.changed`,
and `error`.

Interim assistant segments are sealed at tool boundaries. The terminal
reconciles the authoritative `message.complete.text` with already streamed or
interim text so a dropped trailing delta is recovered without duplicating an
identical tool-call preamble. Reasoning streams are visibly labelled and share
the same bounded timeline as normal output.

## Audio

Push-to-talk uses `POST /api/audio/transcribe` with Hermes Desktop's JSON
contract: a WAV `data_url` and `mime_type: audio/wav`. Base64 is generated as a
stream from a transient SD file, so a 30-second clip is never duplicated in
RAM.

Audio calls carry the selected `profile` query parameter. Token mode uses the
required `X-Hermes-Session-Token` REST header; gated mode uses the configured
Hermes session cookies.

On-demand speech uses `POST /api/audio/speak` with `{ "text": "..." }` and
streams the returned `data_url` to SD while decoding base64. PCM 16-bit WAV is
played directly; MP3 is decoded frame-by-frame. Audio files are deleted after
use, on cancellation, and at the next boot if a reset interrupted cleanup.

## Upstream references

The implementation was checked against upstream commit
`fcbd1076a93841fa88855acce810e342a5b78101` on 2026-08-21.

- https://github.com/NousResearch/hermes-agent/blob/main/website/docs/developer-guide/programmatic-integration.md
- https://github.com/NousResearch/hermes-agent/blob/main/tui_gateway/ws.py
- https://github.com/NousResearch/hermes-agent/blob/main/tui_gateway/methods_session.py
- https://github.com/NousResearch/hermes-agent/blob/main/web/src/lib/gatewayClient.ts
- https://github.com/NousResearch/hermes-agent/blob/main/apps/desktop/src/hermes.ts
- https://github.com/NousResearch/hermes-agent/blob/main/apps/desktop/src/types/hermes.ts
