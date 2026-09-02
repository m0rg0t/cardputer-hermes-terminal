# Hermes Pocket Terminal UI

Every screen can be rendered on a desktop with the [preview harness](../sim/README.md);
the frames in `docs/images/screens/` are produced by the firmware's own drawing
code (`src/app_draw.cpp`). Transcript, composer, and question text use greedy
word wrap at 38 to 39 columns; non-ASCII code points fold to `?` because the
6 x 8 font carries ASCII glyphs only.

The 240x135 interface follows a functional Showa/Heisei electronics language.
It is an instrument panel first and a retro decoration second.

## Visual roles

- Graphite is the uninterrupted working surface.
- Warm ink is the only normal reading color.
- Muted grey labels metadata, rails, and inactive controls.
- Red means selection, recording, progress, or required attention.
- Green appears only in the small `LINK` LED.

Every regular screen uses the same hierarchy: product/section header, red
system rule, one work area, and a physical-key footer. Decorative calibration
marks stay outside text cells.

## Runtime states

The compact state plate uses `READY`, `WORK`, `TOOL`, `WAIT`, or `ERR`. Chat
labels distinguish `YOU /` and `HERMES /`; the session picker uses a dark
burgundy selection plate with warm text. Recording uses a stable buffered frame
and a 12-segment microphone meter. Sleep keeps the four-level official portrait,
drawn inverted (warm ink line art directly on the graphite surface, no white
card), as its primary READY image, switches to a generated WORKING portrait while the
agent is active, and uses a short generated eyes-closed frame as a quiet blink.
The fixed-size link LED is the only green element; optional one-pixel motion has
no random particles or colored halo.

The source artwork is stored in `docs/assets/`: `hermes-ready.png` is the exact
Hermes logo supplied for the project. `hermes-blink.png` is derived from READY
with changes restricted to the visible eye region; `hermes-working.png` is a
generated expressive state. All three are monochrome assets prepared for
reduction to 92x92 pixels, and firmware stores packed 2-bit frames rather than
full PNG images.

## Settings safety

The long-Go panel edits only non-secret presentation values. Changes are held
in memory while the panel is open and saved transactionally to
`/.HERMES-UI.CFG` when it closes. Temporary and backup files recover an
interrupted write.
The interface-alert master switch defaults to off and mutes startup, link,
session-entry, and attention cues without muting Hermes TTS. All cues use the
same explicit codec shutdown as normal playback. A separate Status page exposes read-only network
details, microphone/speaker tests, and a reconnect action. `/HERMES.CFG`, the
runtime cookie, and the CA certificate are never rewritten by the UI.

When interface alerts are enabled, startup uses a quiet rising two-note cue, a
successful Hermes connection uses one short higher note, and session entry uses
a compact rising signature. Cues are capped below the configured TTS volume and
always pass through the explicit codec shutdown; navigation keys remain silent.
