#pragma once

#include <Arduino.h>

namespace buzzer {

void init();

// Play a tone at the given frequency for the given duration.
// freq_hz = 0 or duration_ms = 0 → stop immediately.
// The MLT-8530 on this board is a fixed ~2.7 kHz resonator; off-resonance
// frequencies will sound much quieter but the driver still works.
void play(uint16_t freq_hz, uint16_t duration_ms);

void stop();

// Called from loop(); auto-stops the tone when its duration has elapsed.
void tick();

}  // namespace buzzer
