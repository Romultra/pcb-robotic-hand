# 004: Servo ↔ finger calibration and safety limits

**Date:** 2026-06-21
**Status:** Decided

## Context

Bring-up so far drove the five servos with nothing attached. The next step is to
wire each servo to its mechanical finger through the tendon strings and test real
motion. That introduces two ways to physically damage the hand:

1. A servo's usable travel may be less than the commanded 0–180° (horn
   orientation, internal stops, linkage geometry).
2. Driving a finger past its mechanical end over-tensions the string and can
   snap the printed finger or the linkage.

The firmware as written had no protection and one active hazard:

- `servos::init()` wrote **90°** to every channel on boot. With strings attached,
  any move on boot is a risk. (The rail is off at boot, so this only bites at the
  first power-on, but the blind 90° target is still wrong once strings are on.)
- `servos::set_angle()` clamped only to the absolute 0–1800 (0–180°) range.
- `SERVO_SET` (sliders) and `SERVO_FRAME` (playback / live-mirror) both routed
  through `set_angle`, so a single clamp point can protect every path.
- The firmware persisted nothing (no NVS use yet).

We need a way to measure each servo's real range and each finger's safe
open↔closed travel, then store soft limits so the firmware keeps every later
command inside them.

## Options Considered

1. **Where the limits live — firmware (NVS) vs webapp.**
   - *Firmware-enforced:* per-channel band + home in NVS; `set_angle`/`set_finger`
     clamp on every path. Pros: the clamp protects sliders, frames, playback,
     live-mirror, and any future client; survives reset; the board is the single
     source of truth. Cons: a bit more firmware (a new characteristic, a
     Preferences store, a calibration mode).
   - *Webapp-only:* map/clamp before sending. Pros: minimal firmware change.
     Cons: advisory only — any raw write or buggy client bypasses it and can
     break a finger. For a feature whose whole purpose is "don't break the
     mechanism," that is the wrong place for the guard.

2. **Boot behaviour — servos are absolute-position, so no homing is needed.** A
   standard hobby servo always drives to the same physical angle for a given
   command, every power cycle, so there is no reference-search ("homing") step.
   The only question is what the board does on boot/reset. *No move on boot:*
   attach the PWM channels but write nothing; SERVO_EN is low so the rail is off
   and the servos stay limp until powered. This avoids any jump if the MCU resets
   mid-motion while the ropes are tensioned. (A separate, optional "move to open"
   action is for parking the fingers before installing or removing ropes.)

3. **Calibration data model — min/max vs open/closed.** A plain `[min,max]` band
   loses which end is "open." Storing `open_cmd` and `closed_cmd` captures the
   direction too, so an inverted finger linkage needs no special case, and the
   normalized 0..180 finger value from hand-tracking maps straight into the band.

4. **Measurement granularity / safety.** A "go to angle" command could make a
   large sudden move into a hard stop. A bounded **relative jog** (firmware caps
   the step) lets the finger be inched to its real limit while watched.

## Decision

Firmware-enforced limits in NVS (option 1a), no motion on boot (option 2),
`open_cmd`/`closed_cmd`/`home_cmd` per channel (option 3), and bounded jogging
(option 4). Because the servo holds an absolute position, the stored angles are
reused as-is across power cycles and stay valid as long as the ropes are not
repositioned.

Concretely:

- **`servos.{h,cpp}`** gains a per-channel `Calib { open_cmd, closed_cmd,
  home_cmd, valid }` stored in NVS via `Preferences`. `set_angle` (raw) clamps to
  the `[open,closed]` band; new `set_finger` remaps a normalized 0..180 value into
  the band. A calibration mode relaxes the band for jogging. `jog()` is capped at
  `cfg::SERVO_JOG_MAX_X10`; `goto_target()`/move-to-open set a target that a
  non-blocking `tick()` ramps toward (gentle, so a tensioned rope isn't jerked) at
  `cfg::SERVO_RAMP_STEP_X10` per `cfg::SERVO_RAMP_PERIOD_MS`. `init()` no longer
  writes 90°. `home_cmd` is the park position (set to the open end when OPEN is
  captured); `set_power(true)` parks calibrated channels there before energising.
- **BLE `SERVO_CAL`** (discriminator `000d`, WRITE|NOTIFY): a small command set
  (MODE, JOG, GOTO, CAPTURE, SAVE, RESET, move-to-open, READ); every command is
  logged on serial and the notify reply carries each channel's calibration.
  `CbServoFrame` now calls `set_finger`, so playback/live-mirror automatically
  respect every finger's band.
- **Webapp** `calib.js` + a "Finger Calibration" panel: per-finger jog with a live
  angle and a "seen" motor-travel readout, capture safe open/closed, in-panel
  servo power, save/read/reset, and move-all-to-open. Every action logs to the
  on-screen log. The board's NVS is the source of truth; the app keeps a
  `localStorage` copy for records.

The measurement routine is run on hardware by the author (it needs the board, the
barrel-jack rail, and the ropes); this change is the tooling, not the measured
numbers. The numbers go in `log.md` once captured.

## Sources

- `journal/topics/motor-driver.md` — servo path, MG995 range, LEDC.
- `journal/decisions/003-browser-handtracking-playback.md` — `SERVO_FRAME` and the
  normalized-angle data path this builds on.
- ESP32 Arduino `Preferences` (NVS) — https://docs.espressif.com/projects/arduino-esp32/en/latest/api/preferences.html
