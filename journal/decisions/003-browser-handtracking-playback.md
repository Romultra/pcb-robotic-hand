# 003: Browser hand-tracking with record / playback over BLE

**Date:** 2026-06-11
**Status:** Decided

## Context

A project goal is for the robotic hand driven by the new PCB to **follow a real hand
in real time**. To stay in scope for the BSc thesis, the first milestone is to
**pre-record finger movement and play it back on the board**, with the recording
streamed frame-by-frame so the data path already matches the eventual real-time use.

The previous prototype (`previous-prototype-code/`) did hand tracking in **Python**
(cvzone/MediaPipe), reduced each frame to a **binary 5-bit** open/closed vector with
`fingersUp()`, and pushed it over **USB serial** to an Arduino that slammed each
PCA9685 servo to a hard min/max. The new board is more capable: native ESP32-S3 PWM,
continuous `servos::set_angle` (0–180°), and an existing BLE + Web Bluetooth control
channel between the validation webapp and the board.

## Options Considered

1. **Keep Python tracking, bridge to the board** — reuse the prototype script, add a
   serial/websocket bridge. Cons: keeps a desktop Python dependency and USB tether,
   doesn't match "data stays in the front end," and the webapp already owns the BLE link.
2. **Port tracking into the browser (MediaPipe HandLandmarker JS)** — run the camera +
   landmark detection in the existing webapp; stream angles to the board over BLE. Pros:
   no extra runtime, everything in one front end, identical code path for record/playback
   and the future real-time mode. Cons: needs HTTPS (already have it via GitHub Pages)
   and a one-time CDN/model download.
3. **Binary vs. continuous angles** — replicate the prototype's open/closed, or compute
   a continuous per-finger flexion angle. Continuous is a clear upgrade for mirror-style
   rehab and the board supports it natively.
4. **Transport: BLE vs. Wi-Fi/WebSocket** — BLE reuses the existing control channel at a
   send rate (~20–25 fps of 5-byte frames) well within its budget; Wi-Fi would only matter
   for >30 fps and adds complexity.
5. **Model hosting: CDN vs. vendored** — jsdelivr CDN (zero repo bloat, needs internet)
   vs. committing ~10 MB WASM + `.task` model for offline reproducibility.

## Decision

Browser-side tracking (option 2), **continuous** per-finger angles (option 3) with
per-finger open/closed calibration, streamed over **BLE** (option 4) via a new
`SERVO_FRAME` characteristic (one write = all 5 servos, 5-byte payload). Clips are stored
**front-end only** (`localStorage`, JSON export/import). The model is loaded from the
**jsdelivr CDN** for now (option 5); vendoring is deferred to the final thesis demo for
offline reproducibility. A **"live mirror"** toggle streams live tracking straight to the
board through the same send path — a working preview of the real-time goal at near-zero
extra cost.

Implementation: `webapp/js/tracking.js` (camera, HandLandmarker, angle mapping,
calibration, recording, live mirror), `webapp/js/playback.js` (clip store + timestamp-
driven playback), `sendServoFrame()` in `webapp/js/servos.js`, and `BLE_UUID_SERVO_FRAME`
(discriminator `000c`) added to `firmware/src/ble.{h,cpp}` + `webapp/js/ble.js`.

## Sources

- Previous prototype: `previous-prototype-code/` (handtracking.py, Serial_com_HandGestures.ino).
- MediaPipe Tasks Vision — HandLandmarker (JS), https://ai.google.dev/edge/mediapipe/solutions/vision/hand_landmarker/web_js
