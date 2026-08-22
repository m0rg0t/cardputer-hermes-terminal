# Third-party source

## Hermes desktop icon

- Source: `apps/desktop/assets/icon.png` in
  <https://github.com/NousResearch/hermes-agent>
- Upstream SHA-256: `d60d164e24fdcf6532133b8ea43c77a201e4b9e9dbc396187b58d51d8590ef52`
- Owner/source: Nous Research
- Firmware copy: proportionally resized to 92x92 and embedded as a predecoded
  RGB565 bitmap for reliable rendering while the network connection is active

The artwork is retained as Hermes product identification; ownership remains
with its original owner.

## minimp3

`include/third_party/minimp3.h` is vendored from
https://github.com/lieff/minimp3 at commit
`ea99364f61c14656440e8d77e9c233ccf3124633`.

Only the single-header MP3 decoder is included. Its upstream CC0-1.0 license is
preserved in `include/third_party/minimp3.LICENSE`; the project itself remains
MIT licensed.
