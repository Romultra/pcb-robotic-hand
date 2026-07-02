# 007: Latency test suite in the validation app

**Date:** 2026-06-22
**Status:** Decided (motor command-to-movement test removed 2026-06-23, see Revision)

## Context

The webapp checks that each subsystem works, but it never measures *how fast* the
board responds. For a hand that mirrors a patient's movement in real time, latency
is a real validation result and belongs in the testing chapter, not a vague "it
feels responsive". Three delays sit on the path from the patient's hand to the
mirror moving:

1. BLE round-trip between the phone and the board. This is the dominant tunable
   delay and is set mostly by the negotiated BLE connection interval.
2. The command-to-movement delay: from the app issuing a servo command to the
   mirror physically moving. This includes BLE, firmware handling, and the servo's
   own mechanical response.
3. The software cost of hand tracking: turning one camera frame into five finger
   angles with MediaPipe.

Two facts shaped the design. The board has no way to report when a servo has
physically moved (no encoders), so the command-to-movement delay can only be
measured externally, with a camera. And the browser's `performance.now()` clock is
the only common timebase the app has, so every in-app measurement is relative to
the phone, not the board.

## Options Considered

1. **How to measure BLE round-trip — dedicated echo vs reuse an existing
   characteristic.** A `writeWithResponse` to any existing characteristic already
   gives a link-layer acknowledged round trip with no firmware change. But it only
   covers up to the ATT ack, not a full application round trip, and it can't probe
   payload size. A dedicated echo characteristic that notifies the written bytes
   straight back measures the complete app-to-app path and lets the payload grow.
   Chose to add the echo characteristic as the primary measurement and keep the
   `writeWithResponse` timing as a second, clearly-labelled comparison mode, so the
   report can separate transport from board handling.
2. **Where the echo runs in firmware — GATT callback vs deferred to the loop.**
   Most of our characteristics defer their work to `ble::loop()` to keep the NimBLE
   host task free (Wi-Fi, audio, logging). The echo is the opposite case: deferring
   it would fold our own loop-scheduling jitter into the number we are trying to
   measure. Chose to notify the echo immediately inside the `onWrite` callback. The
   echo does no SD, NVS, or radio work, so it is callback-safe.
3. **Marking the motor command for the camera — screen flash vs other markers.**
   The board can't timestamp its own movement, so the measurement is done off-app
   with a high-speed camera filming both the screen and the hand. The app needs to
   put a visible mark in the same footage at the moment it sends the command. A
   full-screen white flash is the simplest high-contrast marker a phone can produce
   and is trivial to spot frame-by-frame. Chose a fixed `position: fixed` overlay
   toggled white, with no CSS transition so the on-edge is a single frame. The flash
   and the BLE write happen in the same synchronous handler.
4. **Where the hand-tracking timing lives.** The tracking loop already calls
   `detectForVideo` every frame. Rather than a second camera consumer, the existing
   loop is instrumented and emits a small per-frame stats event; the latency panel
   only listens. This keeps one owner of the camera and means the readout works
   whenever tracking runs.

## Decision

**A new `PING` characteristic (discriminator `0010`, WRITE|WRITE_NR|NOTIFY) that
echoes its payload.** `CbPing::onWrite` calls `notify(v.data(), v.length())`
immediately, with the explicit-buffer form so each echo snapshots its own mbuf
(same reason as `service_wifi_scan`). The payload is opaque to the firmware; the
app puts a little-endian sequence number in the first two bytes and pads the rest.

**A new `Latency` panel (`webapp/js/latency.js`)** with three tests:

- *BLE round-trip:* sequential ping-pong over `PING`. For each ping the app records
  `performance.now()`, writes the sequence, and waits for the echo (with a timeout
  that counts as a drop). It reports min, median, average, p95, max, jitter (σ) and
  drop count over a configurable count and payload size, with a Copy CSV button for
  the raw samples. A second button runs the `writeWithResponse` (ATT-ack) variant
  for comparison.
- *Motor command to movement:* a FIRE button that adds a `.flash-on` class to a
  full-screen overlay and sends a single-finger `SERVO_SET` move in the same
  handler. The finger and flash duration are selectable; each press toggles between
  the calibrated open and closed ends. The mirror movement and flash are read off
  the camera footage, not the app.
- *Hand-tracking inference:* a read-only rolling readout (last / average / max
  inference ms and achieved FPS) fed by a `trackingStats` event added to
  `tracking.js`.

The panel is wired at load like the tracking and playback panels; the BLE-dependent
actions self-guard on `ble.connected` and re-subscribe to the echo notify after a
reconnect.

> **Known limitations (documented, not guarded):** the BLE numbers are relative to
> the phone's clock and include the browser's Web Bluetooth queueing, not just the
> radio. The motor-latency flash carries a fixed display-latency offset (a few ms of
> phone screen pipeline), so the measured flash-to-movement time is slightly longer
> than the true command-to-movement time by that near-constant amount; with a
> high-speed camera the offset can be characterised once and subtracted. The
> hand-tracking number is inference only and does not include camera exposure or
> display latency.

## Revision (2026-06-23): motor command-to-movement test removed

The motor command-to-movement test (the FIRE button plus the full-screen flash
marker) was dropped from the suite. The ESP32 starts the servo PWM inside an
interrupt within about a millisecond of the command arriving, which is negligible
at this scale; the servo's mechanical travel after that is a motor property, out
of scope for a board thesis; and timing it from a screen flash is dominated by the
phone's display and refresh latency, which cannot be separated without dedicated
hardware (an LDAT). The latency experiment now reports only the two meaningful
numbers, the app-to-board round trip and the hand-tracking inference. The flash
overlay, the FIRE button, and the `SERVO_SET`-based mover were removed from
`webapp/js/latency.js`, `webapp/index.html`, and `webapp/style.css`. Results are
in `journal/results/latency.md`.

## Sources

- Existing repo patterns reused: the per-characteristic callback structure and the
  explicit-buffer `notify()` reasoning in `firmware/src/ble.cpp`; the normalized
  open/closed band from `webapp/js/calstate.js`
  (`journal/decisions/004-servo-finger-calibration-safety-limits.md`); the
  load-time panel wiring used by `tracking.js` / `playback.js`.
- BLE latency being bounded by the connection interval: NimBLE / Bluetooth Core
  connection-event model (`journal/topics/wireless.md`).
