# Latency results

**Date:** 2026-06-23
**Related:** `journal/decisions/008-low-latency-control-path.md` (design + verdict),
`journal/decisions/007-latency-test-suite.md` (how the panel measures).

## What this measures and why

The robotic hand is meant to mirror a patient's movement in real time, so the
delay on the path from the patient's hand to the mirror moving is a validation
result in its own right, not a vague "it feels responsive". This file records the
two delays that can be measured meaningfully: the app-to-board **round trip** (BLE
control path vs the Wi-Fi alternatives) and the **hand-tracking inference** time
(MediaPipe).

The remaining term, from a command reaching the board to the servo moving, is not
measured. The ESP32 starts the PWM timer inside an interrupt within about a
millisecond of the command arriving, which is negligible at this scale, and the
servo's mechanical travel after that is a property of the motor, not the board, so
it is out of scope. Timing it from a screen flash would be dominated by the
phone's display and refresh latency, which can't be separated out without
dedicated hardware (an LDAT), so no figure is reported.

All numbers come from the validation webapp's Latency panel (`webapp/js/latency.js`)
running in Chrome on the test phone, talking to the PCB1 board. The round-trip
runs are a sequential ping-pong: the app timestamps a request with
`performance.now()`, sends it, waits for the reply, and records the round-trip,
repeating for the configured count and reporting the statistics. The inference
readout is fed from the live tracking loop.

## Test conditions

- Test device: Samsung Galaxy S23, Chrome (Web Bluetooth + `fetch`).
- 50 pings per round-trip run, gap 0 ms (back to back), same board, same room.
- **BLE runs** use the `PING` echo characteristic with an 8-byte payload. The
  firmware echoes the bytes straight back inside the GATT callback with no
  deferral, so the board's own handling adds essentially nothing.
- **Wi-Fi / SoftAP runs** use an HTTP `GET /lat` echo on the board's web server
  (payload size does not apply). The bundled `WebServer` has no keep-alive, so
  every ping opens a fresh TCP connection and the handshake is part of the number.
- Clock: the phone's `performance.now()`. Every figure is relative to the phone
  and includes the browser's own Web Bluetooth / fetch queueing, not just the
  radio.

## Configurations

1. **BLE echo, Wi-Fi connected** — the BLE control path after the firmware
   connection-interval request (7.5-15 ms), with the board also joined to a Wi-Fi
   network as a STA. Both radios active, so BLE and Wi-Fi share the single 2.4 GHz
   radio (coexistence).
2. **BLE echo, Wi-Fi off** — the same BLE path with the board not on Wi-Fi, so the
   radio is BLE-only.
3. **Wi-Fi STA (via router)** — the HTTP `/lat` round trip with the board joined to
   the home network as a STA; BLE still connected (coexistence). The path runs
   phone to access point/router to board.
4. **SoftAP (BLE off)** — the board rebooted into the experiment mode: its own open
   access point, BLE never initialised, phone joined directly to the board. No
   router hop and no coexistence; the per-request TCP handshake still applies.

## Results

All times in milliseconds. n = samples returned / attempted; drops = timeouts.

| Configuration | n | min | median | avg | p95 | max | jitter σ | drops |
|---------------|---|----:|-------:|----:|----:|----:|---------:|------:|
| BLE echo, Wi-Fi connected | 50/50 | 11.9 | 22.6 | 28.8 | 45.3 | 56.4 | 11.4 | 0 |
| BLE echo, Wi-Fi off       | 50/50 | 12.1 | 22.5 | 22.5 | 23.0 | 33.9 |  3.1 | 0 |
| Wi-Fi STA (via router)    | 50/50 | 11.6 | 26.8 | 44.9 | 88.4 | 116.9 | 29.5 | 0 |
| SoftAP (BLE off)          | 50/50 | 12.3 | 23.4 | 24.7 | 40.8 | 60.8 |  9.3 | 0 |

## Reading the results

- **The connection-interval request worked.** BLE sits at a ~22.5 ms median, which
  matches a 7.5-15 ms connection interval (the round trip is about one to two
  intervals). With Wi-Fi off the jitter is tiny (σ 3.1 ms) and the tail is tight
  (p95 23.0, max 33.9). Standalone BLE is excellent for this small-payload round
  trip.
- **Coexistence is the dominant jitter source.** The only difference between the
  first two rows is whether Wi-Fi is also active, and it moves the BLE jitter from
  σ 3.1 to 11.4 ms and the max from 33.9 to 56.4 ms. One radio and one antenna
  time-shared between the two stacks costs both of them.
- **SoftAP removes most of the Wi-Fi penalty but does not win.** Dropping the
  router hop and the coexistence takes the Wi-Fi STA result (median 26.8, p95 88.4,
  σ 29.5) down to median 23.4, p95 40.8, σ 9.3. That is a large improvement over
  STA, yet it only ties BLE's median and still has about three times BLE's
  standalone jitter. The floor that remains is the per-request TCP handshake, which
  a small HTTP request/response cannot get under.
- **The minima are all close (~12 ms).** Best case, every path is fast; the
  difference between them is entirely in the tail and the jitter, which is what
  matters for a smooth real-time mirror.

## Verdict

BLE stays the real-time control path. The connection-interval fix is the
worthwhile firmware change. Wi-Fi earns its place as the bulk-transfer channel
(SD uploads), where throughput matters and per-request setup is amortised, not as
a low-latency control path. On top of being no faster, the SoftAP route is far
less practical than BLE (it needs BLE off, a manual Wi-Fi switch to the board's
AP, a reboot to enter and a RESET to leave, and no phone internet meanwhile).
Beating BLE over Wi-Fi would need a persistent or datagram transport (WebSocket /
WebRTC) on a board-hosted page, which is recorded as future work in decision 008.

## Hand-tracking inference (MediaPipe)

A separate delay on the patient-hand-to-mirror path is the software cost of
turning one camera frame into five finger angles. The webapp runs MediaPipe
HandLandmarker on the webcam (`webapp/js/tracking.js`); the Latency panel reads
out the per-frame inference time and the achieved frame rate. Measured on the test
phone (Samsung Galaxy S23, Chrome):

| metric | value |
|--------|------:|
| last inference   | 31.6 ms |
| average inference | 32.6 ms |
| max inference    | 36.1 ms |
| frame rate       | 29.4 fps |

About 32 ms per frame caps the tracking loop near 30 fps, which lines up with the
phone camera's usual 30 fps video stream, so inference keeps pace with the camera:
hand tracking is not a bottleneck beyond the camera's own frame rate. This is
inference only and excludes camera exposure and display latency.

For the end-to-end picture, the software path from a moved finger to a servo
command is roughly the inference (~32 ms) plus the BLE round trip (~22 ms median),
so on the order of ~55 ms before the board acts. The ESP32 then starts the PWM
within about a millisecond (interrupt); the servo's mechanical travel after that
is a motor property outside the board's scope (see the note in the intro).

## What changed as a result

- Firmware keeps the BLE connection-interval request on connect.
- Motor commands are BLE-only again; the Wi-Fi motor path (`POST /servo`) was
  dropped from mainline and lives on the `feat/low-latency-control` branch.
- The Wi-Fi round-trip latency test (`/lat`) and the SD-upload Wi-Fi path are
  kept; a Wi-Fi **Disconnect** option was added so the link can be dropped after
  testing to restore the coexistence-free BLE state.

## Limitations

- Every figure is on the phone's clock and includes the browser's Web Bluetooth or
  fetch queueing, so it is an app-to-app round trip, not a bare radio RTT.
- The Wi-Fi / SoftAP figures include a fresh TCP handshake per ping.
- The round-trip figures are app-to-app, not glass-to-motion: they exclude camera
  exposure, MediaPipe inference (its own section above), and display latency.
- The inference figure is inference only: it excludes camera exposure and display
  latency, and the achieved frame rate is partly bounded by the camera stream.
- Command-to-movement is not measured (see the intro): the board's command-to-PWM
  start is a sub-millisecond interrupt taken as negligible, and the servo's
  mechanical travel is out of scope.
- Single phone (Galaxy S23), single room, one session per configuration.
