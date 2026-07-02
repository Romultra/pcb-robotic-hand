# 005: Play WAV files from the SD card, confined to a dedicated /audio folder

**Date:** 2026-06-22
**Status:** Decided

## Context

The audio path could only play a few synthesised preset tones (sine, chirp,
jingle, fail buzz) generated on the fly and pushed to the MAX98357A over I²S.
Validation wanted to confirm the full chain works with real recorded audio off
the microSD card (the nudge system's "spoken prompt / melodic alert" role), and
to let the webapp see which clips are on the card. Two constraints shaped the
design:

1. Playback and the file list had to be limited to one **dedicated folder** so
   the feature can never read arbitrary card contents (patient data lives on the
   same card).
2. It had to fit the existing one-task-per-peripheral structure without a heavy
   audio-decoder dependency.

## Options Considered

1. **WAV (16-bit PCM) only, streamed SD → I²S** — no new library; the existing
   audio driver already feeds I²S in `tick()`, so file playback is "read a
   chunk, downmix to mono, write it" with a small WAV-header parser. Files are
   large but trivial on a GB-scale card. Covers the nudge use case fully.
2. **MP3 / AAC via a decoder library (ESP8266Audio / libhelix)** — smaller
   files, but a heavier dependency that wants to own the I²S port, which would
   force a rewrite of the working preset-tone path. More moving parts to debug
   on a validation board.

## Decision

**WAV PCM only.** `audio::play_file()` opens the file through the SD driver,
parses the RIFF/`fmt `/`data` header (requires `audioFormat == 1`, 16-bit, 1–2
channels), switches the I²S sample rate to the file's, and streams it from
`tick()`. Stereo is downmixed to mono (the amp is mono); a fixed 0.8 gain leaves
clip headroom. Returning to a preset restores the 16 kHz rate.

**Sample-rate switch by reinstalling the driver, not `i2s_set_clk`.** First
attempt used `i2s_set_clk(..., I2S_CHANNEL_MONO)` to retune at runtime; on
arduino-esp32 2.0.16 the channel argument disagrees with the install-time
`I2S_CHANNEL_FMT_ONLY_LEFT` framing and clocked the wrong rate, so non-16 kHz
files played back slowed down (the presets were fine because they never call
`set_clk`). The fix uninstalls and reinstalls the driver at the new rate, reusing
the exact config the presets run under. To stop the streamed audio glitching, the
DMA buffer was deepened (`dma_buf_count` 4 → 8, ~46 ms at 44.1 kHz), `tick()`
feeds several chunks per visit, and `loop()` skips its `delay(5)` while audio
plays (the blocking `i2s_write` already paces playback). Bandwidth was never the
limit: even 44.1 kHz stereo is ~176 KB/s versus the card's ~270–500 KB/s.

**Folder confinement in one place.** All card access for audio goes through two
new `sdcard` functions and nothing else: `list_audio()` enumerates only the
`/audio` folder (files only, base names, never recurses), and `open_audio(name)`
rejects any name containing `/`, `\`, or `..` before prefixing `/audio/`. So
neither the list nor playback can ever name a path outside the folder.

**Listing over BLE.** A new `audio_file` characteristic (discriminator `000e`,
WRITE|NOTIFY) carries `LIST` / `PLAY name` / `STOP`. The list is chunked into
MTU-sized notifications tagged with a leading type byte (`0x01` chunk, `0x00`
end, `0x02` play status), exactly like the Wi-Fi scan. BLE is the always-present
control channel, so no Wi-Fi/HTTP listing endpoint was added.

**All audio control deferred to the main loop.** File streaming reads the card
from `tick()` (main loop task). To keep the audio `File` handle touched by only
one task, the GATT callbacks (preset play included) just latch a request, and
`ble::loop()` services it on the main loop task. This mirrors the existing
Wi-Fi scan/connect deferral.

> **Known limitation (not guarded):** an SD upload or self-test is still handled
> directly in its GATT callback (host task), so triggering one *while a file is
> playing* crosses tasks on the SD/SPI bus. That combination doesn't happen in
> normal validation (you play or you upload, not both at once). A future
> revision could serialise all SD access behind a mutex if it ever needs to be
> concurrent.

## Sources

- WAV/RIFF container layout (canonical `RIFF`/`WAVE`/`fmt `/`data` chunks).
- ESP-IDF legacy I²S `driver/i2s.h` (install/uninstall, DMA buffer sizing) for
  the per-file sample-rate switch; `i2s_set_clk` was rejected (channel/framing
  mismatch played files back slow).
- Existing patterns in this repo: `service_wifi_scan()` (chunked notify) and the
  Wi-Fi defer-to-loop mechanism in `firmware/src/ble.cpp`.
