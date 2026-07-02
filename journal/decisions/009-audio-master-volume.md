# 009: Audio master volume by digital sample scaling

**Date:** 2026-06-24
**Status:** Decided

## Context

The audio panel could play preset tones and WAV files, but always at a fixed
loudness. A volume control was wanted so the nudge audio can be set to a
comfortable level. The natural first thought is that volume is set "over I²S",
but I²S is only the digital transport (BCLK, LRCLK, data). The MAX98357A (U7) is
a **fixed-gain** Class-D amp: its gain is set by the resistor on the SD_MODE /
GAIN pin (the 634 kΩ part on this board), and it has no volume register to write
over any bus. So the loudness has to be changed before the samples reach the amp.

## Options Considered

1. **Scale the PCM samples digitally in firmware** — multiply every sample by a
   0..1 master-volume factor in the two render paths (`render_chunk` for presets,
   `stream_file_chunk` for files) before `i2s_write`. No hardware change, reuses
   the headroom scaling those paths already do (`0.5f` for presets, `GAIN 0.8f`
   for files). Resolution is limited at the very bottom of the range (fewer bits
   left), which is fine for a nudge cue.
2. **Switch the SD_MODE/GAIN resistor at runtime** — the MAX98357A exposes a few
   discrete gain steps via that pin, but they are set by a fixed resistor on the
   PCB, not switchable without rework, and would only give coarse steps. Not
   possible on PCB1.
3. **Add a digital potentiometer / different amp with an I²C volume register** —
   a board change, out of scope for this revision.

## Decision

Option 1. Added `audio::set_volume(uint8_t pct)` (0..100, clamped) which stores a
`volatile float g_volume` (0.0..1.0); both render paths multiply by it. It is a
single aligned float store, so it is written from the BLE host task and read on
the loop task without locking. Exposed over a new BLE characteristic
`AUDIO_VOL` (discriminator `0011`, WRITE|WRITE_NR, one byte 0..100), applied
immediately in the GATT callback since it touches no file handle or SD access.
The webapp adds a volume slider to the Audio panel that writes the percentage.

The volume is persisted to NVS (namespace `audio`, key `vol`, one byte) and
restored on boot, defaulting to 100 % when never set. To avoid hammering flash
while the slider is dragged, `set_volume` only updates RAM and marks the value
dirty; the actual NVS write is debounced in `audio::tick()` (written once the
value has been stable ~1.5 s, and skipped entirely if it already matches the
stored byte). The webapp also coalesces slider writes so only one BLE write is
in flight at a time, matching the servo-slider path.

## Sources

- MAX98357A datasheet (gain set by SD_MODE pin resistor; no volume register) —
  `datasheets/`.
- `firmware/src/drivers/audio.cpp` (render paths and `set_volume`).
- `journal/decisions/005-sd-wav-audio-playback.md` (the playback path this builds on).
