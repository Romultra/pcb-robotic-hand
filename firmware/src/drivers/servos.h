#pragma once

#include <Arduino.h>

#include "config.h"

namespace servos {

// Per-channel calibration. open_cmd / closed_cmd are the servo commands (tenths
// of a degree, 0..1800) at the safe fully-open and fully-closed finger poses;
// storing both (rather than a min/max pair) also captures the linkage direction,
// so an inverted finger is handled naturally. home_cmd is the park position
// (defaults to the open/slack end). valid stays 0 until the channel is
// calibrated; an uncalibrated channel is only clamped to the absolute 0..180°.
struct Calib {
    uint16_t open_cmd;
    uint16_t closed_cmd;
    uint16_t home_cmd;
    uint8_t  valid;
};

void init();

// SERVO_EN line on GPIO8 → TPS56637 EN.
// false = 5V-SERVO rail off, servos lose power; true = rail on.
// On enable, calibrated channels are first parked at their home command so the
// rail energises toward the slack end instead of yanking into over-tension.
void set_power(bool enable);
bool is_power_on();

// Raw angle path (used by SERVO_SET). angle_deg_x10 is tenths of a degree,
// 0..1800. Clamped to the channel's calibrated [open,closed] band in normal
// mode; clamped only to 0..1800 in calibration mode or before calibration.
void set_angle(uint8_t channel, uint16_t angle_deg_x10);

// Normalized finger path (used by SERVO_FRAME / playback / live-mirror).
// norm_0_180 is 0 = fully open, 180 = fully closed; it is remapped into the
// channel's [open_cmd,closed_cmd] band. Falls back to a straight 0..180° write
// when the channel is not yet calibrated.
void set_finger(uint8_t channel, uint8_t norm_0_180);

// --- Calibration / measurement -------------------------------------------
// Calibration mode relaxes the soft band so a finger can be jogged to its real
// limit; normal mode re-applies the band on every command.
void set_cal_mode(bool on);
bool cal_mode();

// Jog a channel by a relative delta (tenths of a degree, signed). The
// magnitude is capped at cfg::SERVO_JOG_MAX_X10 so a single command can never
// make a large sudden move. Applies immediately. Returns the new command.
uint16_t jog(uint8_t channel, int16_t delta_x10);

// Ramped move to an absolute command (tenths of a degree). Gentle: tick()
// walks the servo there at cfg::SERVO_RAMP_STEP_X10 per cfg::SERVO_RAMP_PERIOD_MS.
uint16_t goto_target(uint8_t channel, uint16_t angle_deg_x10);

// Capture the channel's current command as open (0), closed (1) or home (2).
void capture(uint8_t channel, uint8_t which);

// Ramp a channel (or all channels) gently to its calibrated home command.
void home_channel(uint8_t channel);
void home_all();

uint16_t current(uint8_t channel);   // last command, tenths of a degree
Calib    get_calib(uint8_t channel);

// Last command expressed as normalized flexion percent: 0 = fully open, 100 =
// fully closed, inverting the channel's calibrated [open_cmd,closed_cmd] band.
// A flipped band (closed_cmd < open_cmd) is handled naturally. An uncalibrated
// channel maps linearly over the full 0..1800 command range. Used by the
// exercise logger so logs stay comparable across recalibration.
uint8_t flexion_pct(uint8_t channel);

void save_calib();    // persist all channels to NVS (Preferences)
void reset_calib();   // clear calibration (all channels back to uncalibrated)

// Services in-progress ramps. Call from loop(); cheap no-op when idle.
void tick();

}  // namespace servos
