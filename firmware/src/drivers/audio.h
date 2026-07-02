#pragma once

#include <Arduino.h>

namespace audio {

void init();

// Drives the MAX98357A through I²S with one of a handful of preset tones.
//   0 = silence (stops any in-progress playback, including a file)
//   1 = 1 kHz sine,  ~500 ms
//   2 = chirp 500 Hz → 4 kHz, ~700 ms
//   3 = success jingle (3 notes)
//   4 = fail tone (low buzz)
void play_preset(uint8_t preset_id);

// Streams a 16-bit PCM WAV file (mono or stereo, any sample rate) from the
// dedicated SD audio folder to the speaker. `name` is a bare filename inside
// that folder (see sdcard::open_audio). Returns false if the file can't be
// opened or isn't a supported WAV. Stops any current playback first.
//
// Must be called from the main loop task only (SD reads in tick() run there
// too, so the file handle is touched by exactly one task).
bool play_file(const String& name);

// Sets the master output volume as a percentage (0..100, clamped). The
// MAX98357A is a fixed-gain amp with no volume register, so this scales the
// PCM samples digitally before they reach I²S. Applies to both presets and
// file playback, immediately. The value is persisted to NVS (debounced, written
// from tick()) and restored on boot; defaults to 100 % when never set.
void set_volume(uint8_t pct);

// Called from loop(); feeds the I²S DMA buffers when a preset or file is
// mid-playback.
void tick();

bool is_playing();

}  // namespace audio
