# Engineering Notebook — BSc Thesis PCB Design

Chronological log of research, notes, and progress. Entries can be quick one-liners
or longer write-ups — whatever fits the moment.

---

## 2026-02-25

### Setting up project documentation workflow

Explored how to track research, design decisions, and progress throughout the thesis
without things getting scattered. Considered several approaches:

- **Engineering notebook** (markdown in repo) vs **PKM apps** (Logseq, Obsidian) — went
  with plain markdown in the repo. Lower friction, no extra tools, version-controlled,
  and avoids context switching between apps.
- **Zotero vs Logseq** — minimal overlap. Zotero is for references and BibTeX export,
  Logseq is for note-taking. Different tools for different jobs.
- Decided against timestamps on log entries — date-level granularity is enough for a
  thesis project.
- Discussed whether to have a separate `docs/` folder — decided against it. The
  `journal/topics/` files already serve as flat, scannable reference material. A separate
  docs layer would just be redundant middle ground between the journal and the thesis.
- Renamed the role of `topics/` to be broader — not just subsystem specs but any
  deep-dive: literature reviews, preliminary research, regulatory notes, etc.

**Final structure:**
- `journal/log.md` — chronological working notes (this file)
- `journal/topics/` — deep-dives and reference material by topic
- `journal/decisions/` — design decision records
Updated the README to reflect the new structure.


### Looked at EDA tools for PCB design

Looked at KiCad and EasyEDA, I created an account on EasyEDA to test it out. Stopped there.

## 2026-02-26

### Looked at integrating papis in the workflow

Discovered CLI tool papis for reference management which integrates seamlessly in a terminal heavy git tracked project.
References are stored in human readable yaml files, which allows for simple git version control.
Also looked at a papis plugins which links papis to the zotero web scraper browser extension. 
This link works by running `papis zotero serve`.

Papis is a python package. Therefore I created a .venv to install this tool, and added a requirements.txt file
I should reference the requirements instalations steps in the readme file of th repo

## 2026-06-08

### Bring-up: WS2812B LEDs dead — power pins reversed in schematic

First power-up of PCB1. The four addressable WS2812B LEDs (LED2–LED5, visual
nudge chain on GPIO39 via U3) wouldn't light, and the SN74LV1T34 level-shifter
(U3) ran hot — its output swinging only 5 V↔3 V (couldn't pull below ~3 V) as
the input swung 3.3 V↔0 V. Both assembled boards behaved identically.

Chased it by elimination — verified U3's pinout/netlist against the TI
datasheet (fine), confirmed the U3 input toggled cleanly 0↔3.3 V while its
output only fell to ~3 V at the low end and ran hot, and ruled out a floating
input (GPIO39/MTCK is pulled up at reset), a missing decoupling cap, and a
missing series resistor.

**Actual root cause: the WS2812B supply pins are swapped in the schematic** —
VDD pad tied to GND, VSS pad tied to +5 V, on all four LEDs. Data pins (DIN/DOUT)
are correct. Netlist confirms `LED*.1 ∈ GND`, `LED*.3 ∈ 5V` (should be the
reverse). The reversed rails forward-bias the LED's internal ESD/substrate
diodes, so the part is reverse-powered (dead) and turns its data input into a
clamp powered off the misplaced 5 V — which is what U3 was fighting and
overheating against. A schematic error, hence identical on every board.

Decided **not** to rework the existing boards. The only fix for the fabricated
prototypes is rotating each LED 180° (swaps VDD/VSS *and* DIN/DOUT, so the data
input would have to be rerouted to the other end of the chain — which sits right
next to the start, since the LEDs form a square loop — and run in reverse)
— too much soldering on the tightly-packed array just to exercise a
non-critical feature on a validation board. Leaving the LEDs non-functional on
PCB1; it's a one-line schematic correction for the next revision. Full writeup
and the next-rev fix in `journal/decisions/001-ws2812b-power-pin-reversal.md`.
This is worth discussing in the thesis (both the error and the
debugging/validation methodology that found it).

## 2026-06-10

### Bring-up: WiFi scan reached the firmware but never the webapp

WiFi scan over BLE: the firmware logged `scan complete: 3 networks` plus
`E … wifi:timeout when WiFi un-init, type=4`, but the validation webapp received
no results. Two distinct WiFi+BLE coexistence bugs on the ESP32-S3:

1. **Blocking work on the NimBLE host task.** The scan (and the up-to-10 s
   connect) ran synchronously *inside* the GATT write callback, which executes
   on the NimBLE host task. Blocking it for seconds stalls the BLE link, so the
   `notify()` calls fired into a disrupted connection and the app got nothing.
   Fix: the write callbacks now only latch a request flag; the actual scan/
   connect + notifications run from `ble::loop()` on the main loop task.
2. **De-initializing WiFi while BLE is live.** `WiFi.mode(WIFI_OFF)` calls
   `esp_wifi_deinit()`, and tearing the WiFi stack all the way down while the
   BLE controller holds shared RF/scheduler resources trips the coexistence
   teardown timeout (`type=4`) — non-fatal here but a known crash path for
   others. Fix: leave the radio in idle STA between operations instead of
   `WIFI_OFF` (an unconnected, non-scanning STA costs BLE negligible airtime).
   The `WIFI_OFF` in `wifi_scan::init()` stays — it runs at boot before BLE.

**Takeaways for the rest of bring-up:** never `WIFI_OFF` while BLE is active,
and never do heavy/blocking WiFi work on the NimBLE host task — defer it to the
loop task. Worth a sentence in the thesis on WiFi/BLE coexistence constraints.
Refs: esp-idf #499 (deinit-during-coex crash), home-assistant/core #90823
(un-init timeout non-fatal).

### NimBLE deferred-notify gotcha — multi-packet scan shipped empties

After the above, the scan reached the firmware but the webapp logged
`0 APs` twice. Cause: NimBLE's no-arg `notify()` calls `ble_gatts_chr_updated()`,
which *defers* the send and re-reads the characteristic value at transmit time
— it does not snapshot. The scan did `setValue(data); notify(); setValue(empty);
notify();` back-to-back, so both deferred sends transmitted the final (empty)
value and the data packet never went out. Fix: send each data packet via
`notify(buf, len)` (the explicit-value overload builds its mbuf immediately =
real snapshot); the lone 0-length terminator can stay on the deferred path since
nothing overwrites it. **Rule: for any characteristic that notifies more than
once in quick succession, pass the payload to `notify(value, len)` — never rely
on `setValue` + bare `notify()`.**

## 2026-06-11

### Fast SD file transfer over Wi-Fi (BLE stays the orchestrator)

The BLE SD upload works but is slow — not because of BLE's PHY ceiling but
because the protocol does one `writeWithResponse` + notify-ack per 180-byte
chunk, so it pays a connection-interval round-trip per chunk. Now that the
Wi-Fi STA scan/connect is validated, added a Wi-Fi bulk-data path while keeping
BLE as the control channel and **keeping the single existing webapp** (no second
front end).

**The constraint that shaped it:** the webapp is served over HTTPS (GitHub
Pages — required for Web Bluetooth), and an HTTPS page historically cannot reach
a plain-HTTP LAN device (mixed content), nor can the ESP present a trusted TLS
cert for a private IP. That used to force a device-hosted page. **But Chrome
142+ ships Local Network Access (LNA):** a `fetch`/XHR to a *private-IP literal*
is exempted from the mixed-content block once the user grants a one-time
"access devices on your local network" permission. So the HTTPS app can POST
straight to the board — exactly the "silent parallel Wi-Fi datapath" wanted.

Chose **STA only** (board joins an existing network) over SoftAP: it reuses the
working `connect_sta` (which already returns the board IP over BLE, so the app
knows where to POST), the phone keeps internet, and there's no AP/captive-portal
juggling. SoftAP stays a documented fallback for credential-less environments.
Full rationale + the LNA mechanics in
`journal/decisions/002-wifi-file-transfer-lna.md`.

**Implementation.** Firmware: new `drivers/webfs.cpp` — built-in synchronous
`WebServer` on :80, started from `service_wifi_connect()` after a successful
join; `POST /upload?path=` is streamed to the existing `sdcard::upload_*` state
machine (no RAM buffering — no PSRAM), plus `GET /ping` and an
`Access-Control-Allow-Origin` header on every response (cross-origin from the
HTTPS app). No new BLE characteristics, so the UUID-sync check stays green.
Picked the built-in `WebServer` over `ESPAsyncWebServer` to avoid a new
dependency; the cost is that an upload blocks `loop()` (BLE status notifications
pause for the transfer) — fine for a validator. Webapp: `net.js` holds the board
IP (set on Wi-Fi connect, cleared on BLE disconnect); `sd.js` POSTs via XHR
(real upload-progress events) and falls back to the BLE chunk path on any
failure or denied permission; a hint line shows which transport is active.

Couldn't build locally (no PlatformIO on this machine) — relying on the
`firmware.yml` CI build. **To verify on hardware:** connect over BLE → join
Wi-Fi in the app → upload a file; expect the one-time LNA prompt, then a
transfer far faster than BLE, and confirm it still falls back to BLE when Wi-Fi
is off. Good material for the thesis (transport trade-off + the browser-security
constraint that drove the design).

## 2026-06-11

### Browser hand-tracking → record / playback → board over BLE

Started bridging the old prototype's hand-tracking to the new board. The
prototype (`previous-prototype-code/`) ran cvzone/MediaPipe in **Python**, reduced
each frame to a **binary 5-bit** `fingersUp()` vector, and pushed it over USB
serial to an Arduino that drove each PCA9685 servo to a hard min/max. Goal for
the thesis: the new board makes the hand **follow a real hand in real time**;
in-scope first step is **record finger movement, play it back on the board**,
streamed frame-by-frame so the data path already matches the real-time case.

**Decisions** (see `decisions/003-browser-handtracking-playback.md`): port
tracking into the existing webapp with **MediaPipe HandLandmarker (JS)** — no
Python, no tether, everything in one front end; **continuous** per-finger
flexion angles (joint angle at the PIP, with per-finger open/closed calibration)
instead of binary open/closed; stream over **BLE** (reuses the control channel);
clips **front-end only** (`localStorage` + JSON export/import); MediaPipe runtime
+ model from the **jsdelivr CDN** (vendoring deferred to the final demo). Added a
**"live mirror"** toggle that streams live tracking straight to the board through
the same path — a free preview of the real-time goal.

**Implementation.** Firmware: new `SERVO_FRAME` BLE characteristic (discriminator
`000c`) — one write sets all 5 servos (5-byte payload, one angle 0–180° per
channel), so a frame is a single BLE write instead of 5× `SERVO_SET`. Mirrors the
existing `CbServoSet` pattern; reuses `servos::set_angle`, no new firmware deps.
Webapp: `tracking.js` (camera + HandLandmarker + landmark→angle mapping +
calibration + recording + live mirror), `playback.js` (clip store +
timestamp-driven playback; the browser is the clock and drops overdue frames to
stay wall-clock-synced), shared `sendServoFrame()` in `servos.js`, two new
collapsible panels in `index.html`. Both modules init at load (camera /
calibration / clip management work offline; sends are guarded on `ble.connected`).

**To verify on hardware:** Chrome on Android over HTTPS → start camera (grant
MediaPipe CDN + webcam) → calibrate open/closed → connect BLE, servo rail ON
(barrel-jack power) → live-mirror should track with low lag; record a clip and
play it back and confirm the servos reproduce the motion at the right timing.

## 2026-06-15

### Dropped papis from the workflow

Removed papis and the `requirements.txt` that installed it. It is not used for
the thesis going forward. References are kept directly in
`thesis/bibliography.bib` (biblatex + biber), which is simpler and keeps
everything inside the Overleaf submodule. This supersedes the 2026-02-26 note
above about integrating papis.

## 2026-06-21

### Servo ↔ finger calibration + safety limits

Prepping to wire the five servos to the real mechanical fingers (tendon strings).
Two damage risks: the servo's usable range may not be a full 180°, and pulling a
finger past its mechanical end over-tensions the string and can break it. Needed
a measure-then-clamp routine.

**Decisions** (see `decisions/004-servo-finger-calibration-safety-limits.md`):
limits live in **firmware (NVS)** and are clamped on **every** path (sliders,
`SERVO_FRAME`, playback, live-mirror), not just the webapp — so no client can
over-drive a finger. **No motion on boot** (rail is off; servos stay limp) with
an explicit **ramped Home**, replacing the old blind `write(90)` in
`servos::init()`. Store **open_cmd / closed_cmd / home_cmd** per channel (keeps
the direction, so inverted fingers need no special case). Jogging is
**firmware-capped** so a single step can't make a big sudden move.

**Implementation.** Firmware: rewrote `servos.{h,cpp}` (per-channel `Calib`,
calibration mode, `jog`/`capture`/`goto`/`home`, non-blocking ramp `tick()`,
`Preferences` save/load, `set_finger` remap). New `SERVO_CAL` characteristic
(`000d`, WRITE|NOTIFY) with a small command protocol; `CbServoFrame` now calls
`set_finger` so playback respects each band. Tunables in `config.h`. Webapp:
`calib.js` + a "Finger Calibration" panel (jog, capture open/closed/home, save,
read, home all). Build OK (`pio run`, 31% flash), UUID-sync OK (12 UUIDs).

**To measure on hardware:** DC jack in, connect BLE, servo Power ON. Stage A —
string slack, jog each servo to its physical ends. Stage B — string attached, jog
to safe fully-open ("Set open" + "Set home") and safe fully-closed ("Set
closed"), per finger. Save. Reset the board and confirm: no boot-yank, Home ramps
gently, sliders/playback stay inside the bands, calibration survives a power
cycle. Record the open/closed/home numbers here once captured.

### Calibration follow-up: model correction + feedback

First bench try: the calibration buttons looked dead. They weren't broken (the
BLE writes were going through), but most actions logged nothing and the motors
need the servo rail powered to move, so it felt like nothing happened. Fixed:
every `servo_cal` command now prints on serial and logs in the webapp, the panel
has its own Power ON/OFF, and there's a per-finger live angle plus a "seen" motor
range that fills in as you jog. Also silenced the harmless first-boot
`nvs_open NOT_FOUND` log (open the namespace read-write so it's created).

Corrected a misunderstanding from the first pass: these are **absolute-position**
servos, so there is **no homing**. A given command always reaches the same
physical angle, so the stored open/closed values are simply reused across power
cycles and stay valid as long as the ropes aren't repositioned. Dropped the
"homing" framing; the park action is now just "move to open". Kept "no move on
boot" purely as a safety measure (a reset mid-motion shouldn't jerk a tensioned
rope). Updated decision 004, motor-driver.md, and the firmware README
to match.

### Servo channel aliasing: dropped ESP32Servo for direct LEDC

Bench testing showed the servos were cross-linked: the thumb slider drove motors
1, 3 and 5 at once and the index slider drove 2 and 4, i.e. ESP32Servo was only
handing out two LEDC channels and aliasing the five pins onto them. Reserving the
timers with `ESP32PWM::allocateTimer(0..3)` didn't help. Root cause is
ESP32Servo/ESP32PWM's auto channel allocation misbehaving on the S3 (8 channels,
4 timers).

Fix: drive the LEDC peripheral directly in `servos.cpp` — one explicit channel
per servo (0–4), `ledcSetup(ch, 50, 14)` + `ledcAttachPin(pin, ch)` +
`ledcWrite(ch, duty)`, with duty from the pulse width (500–2500 µs). 14-bit is
the S3 LEDC timer max; channels pair onto timers (ch/2) at the same 50 Hz so they
share timers cleanly with independent duty. The buzzer already sits on channel 7,
clear of these. Removed the ESP32Servo dependency from `platformio.ini`; flash
dropped slightly. All calibration/jog/ramp/NVS logic is unchanged — only the PWM
backend swapped. **To verify on hardware:** each slider should now move only its
own motor.

After flashing, the aliasing was gone but the slider order was reversed (S1 drove
finger 5, S5 drove finger 1). Traced the netlist: the level shifter routes the
GPIOs in reverse header order — GPIO15→H1, GPIO7→H2, GPIO6→H3, GPIO5→H4, GPIO4→H5
— so `SERVO_PINS` was in ascending-GPIO order instead of header order. Reordered
it to `{15, 7, 6, 5, 4}` so LEDC channel i / slider Si drives finger i+1. No
calibration change needed (per-finger open/closed already handles direction).

## 2026-06-22

### SD WAV playback from a dedicated /audio folder

Added playing real audio off the card, not just the synth presets. `audio.cpp`
gained `play_file()`: opens the file, parses the WAV header (16-bit PCM, mono or
stereo), retunes the I²S clock to the file's rate, and streams it from `tick()`
(stereo downmixed to mono, 0.8 gain for headroom). Returning to a preset restores
16 kHz.

Folder confinement is the important bit — the user wanted playback and the file
list locked to one folder so it can't read the rest of the card. All audio card
access goes through two new `sdcard` functions and nothing else: `list_audio()`
(only `/audio`, files only, base names) and `open_audio(name)` (rejects `/`, `\`,
`..`, then prefixes `/audio/`). New `audio_file` BLE characteristic (`000e`,
WRITE|NOTIFY) with LIST/PLAY/STOP; the list comes back chunked like the Wi-Fi
scan, tagged with a leading type byte. Webapp Audio panel now lists the folder
and plays a file per row.

Because file streaming reads the card from the main loop task, I moved *all*
audio control (presets too) to a deferred latch serviced in `ble::loop()`, so the
file handle is only ever touched by one task — same trick as the Wi-Fi defer.
Known gap left documented: uploading/self-testing while a file plays still crosses
tasks; doesn't happen in normal use. Builds clean, UUID-sync green.

### WAV playback: fixed slowdown + glitching

First bench test: file playback worked but was slowed down and glitchy. Two
separate firmware bugs, not an SD bandwidth limit (44.1 kHz stereo is ~176 KB/s
vs the card's ~270–500 KB/s):

- **Slowdown = wrong clock.** Runtime retune used `i2s_set_clk(..., MONO)`, which
  on arduino-esp32 2.0.16 disagrees with the ONLY_LEFT install framing and clocks
  the wrong rate. Presets were fine because they never call set_clk. Fixed by
  uninstalling/reinstalling the driver at the file's rate (same config the presets
  use). A uniform slowdown is always a clock problem — underruns give gaps, not drag.
- **Glitch = DMA underruns.** Buffer was only 4×256 (~23 ms at 44.1 kHz), fed one
  chunk per loop with a `delay(5)` and SD/BLE/logging jitter. Deepened to 8 buffers,
  `tick()` now feeds 4 chunks per visit, and `loop()` skips the delay while audio
  plays (i2s_write already paces it). To re-verify on the bench with a 44.1 kHz file.

### Exercise position logging to the SD card (STO-1 finally built)

Built the original logging feature — the one the PDR and the STO requirements
always called for but that nothing had used the card for yet. The board now
writes one CSV per exercise to a dedicated `/logs` folder, plus an `index.csv`
manifest. Columns are `t_ms` + the five fingers as flexion percent (0 = open,
100 = closed), so the data stays comparable even if a finger is recalibrated; the
header carries the calibration band so raw degrees are reconstructable.

The interesting design question was "what is an exercise". I don't want to log
manual slider pokes or calibration jogs, only real movements. The board already
distinguishes them by channel: exercises come over `SERVO_FRAME` (live mirror and
clip playback), manual is `SERVO_SET`, calibration is `SERVO_CAL`. So only the
`SERVO_FRAME` handler calls `logging::note_exercise_frame()`. A session opens
automatically on the first frame (rail powered, not calibrating), samples at
20 Hz, and closes after a 3 s idle gap — so each bout becomes its own file.

No RTC on the board, so filenames fall back to a monotonic NVS counter
(`sess-NNNNN.csv`); when the webapp pushes wall-clock time over the new `log_ctrl`
characteristic (`000f`, ENABLE/SET_TIME/STATUS), they become `YYYYMMDD-HHMMSS.csv`
instead. New `logging` driver owns the state machine, runs from the main loop so
the file handle stays single-task, folder-locked through new `sdcard::open_log()`
/ `list_logs()` sharing the same path guard as `/audio`. Firmware only for now —
retrieval is by pulling the card, matching the "clinician pulls the card" idea;
webapp logs panel is a later job. Builds clean, UUID-sync green. See
`journal/decisions/006-exercise-position-logging.md`.

### Latency test panel in the webapp

Added a way to actually measure how fast the board reacts, for the testing
chapter. New `Latency` panel with three tests. BLE round-trip needed firmware
help: a `PING` characteristic (`0010`, WRITE|NOTIFY) whose `onWrite` notifies the
written bytes straight back, right in the GATT callback with no defer — the whole
point is to not fold our loop scheduling into the number. The app pings with a
sequence number, times the echo, and reports min/median/avg/p95/max/jitter/drops
over N pings with a configurable payload, plus a Copy CSV. A second button times
`writeWithResponse` (ATT-ack only) so I can compare link latency vs the full echo.

Motor command-to-movement can't be timed in software (no encoders), so the app
just provides a marker for a high-speed camera: FIRE flashes the whole screen
white and sends the servo move in the same handler, both land in the footage and
you count frames between flash and mirror moving. Each press toggles open/closed
for the picked finger. Third test is hand-tracking inference time, read off a
small stats event I added around the existing `detectForVideo` call in
`tracking.js` (last/avg/max ms + FPS). Firmware builds clean, UUID-sync green. See
`journal/decisions/007-latency-test-suite.md`.

## 2026-06-23

### Chasing the BLE latency: firmware fix + a Wi-Fi motor path

The round-trip the latency panel showed felt high, so I traced the path to see
if it was us or the link. The echo path is already as tight as it gets — `CbPing`
notifies straight back inside the GATT callback, no defer, MTU already 247. The
real miss was connection-parameter control: the phone picks the connection
interval at connect and Android's default is slow (tens of ms), and a write + the
echoed notify each wait for the next connection event, so the round-trip is ~1–2
intervals. We were just accepting whatever Android chose.

Fix: on connect, `onConnect` now calls `updateConnParams` asking for 7.5–15 ms
(constants in `config.h`, latency 0, 4 s supervision timeout so coexistence
airtime stealing doesn't drop the link). The phone can clamp to its own minimum,
so it's safe. That's the firmware lever; whatever's left after that is the BLE
floor for the interval the phone allows.

Because some of the delay is genuinely the transport, I also added the Wi-Fi
motor path the task asked for (and would've added regardless). The board already
runs an HTTP server after a STA join (for SD uploads), so I added `POST /servo`
(five normalized finger positions hex-encoded in `?f=`, same `set_finger` remap +
`note_exercise_frame` as `SERVO_FRAME`, so it's a true drop-in) and `GET /lat`
(echoes a seq for timing). No WebSocket — Chrome's Local Network Access only
exempts fetch/XHR, not WebSocket, so a body-less preflight-free POST over fetch
is the move. The bundled `WebServer` has no keep-alive (its keep-alive branch is
commented out upstream for a Chrome bug and there's no `keepAlive()` setter), so
each frame pays a TCP handshake; the coalescing senders keep one frame in flight
so nothing piles up, and the handshake just lands in the measured number.

Webapp: `net.js` gets a `preferWifi` flag + `wifiControlActive()`; the single
`sendServoFrame` chokepoint routes over Wi-Fi when active, else BLE, so live
mirror and playback both follow the toggle with no other change. Toggle lives in
the Servos panel (disabled until a Wi-Fi link exists, off on disconnect). Latency
panel gets a **Wi-Fi round-trip** button that pings `/lat` and reports into the
same stats table (mode = `wifi`) so BLE vs Wi-Fi compare directly. No new BLE
characteristic, so UUID-sync stays green; firmware builds clean (31 % flash).
Numbers to be captured on the bench. See
`journal/decisions/008-low-latency-control-path.md`.

### Bench numbers + Wi-Fi tuning

First run (50 pings): BLE echo med 22.6 ms (min 11.9, p95 45.3, σ 11.4) — bang on
a 7.5–15 ms interval, so the conn-interval request worked. Wi-Fi (STA via the home
router) med 26.8, p95 88.4, max 116.9, σ 29.5. The Wi-Fi *min* (11.6) is tied with
BLE, so the link's fine; the tail is overhead, not a floor.

Tried two firmware fixes for the Wi-Fi tail and **backed both out** — they made it
worse on the bench: (1) `WiFi.setSleep(false)` on connect **reset the board** every
time it joined Wi-Fi (probably brownout under BLE/Wi-Fi coexistence, or a PM-mode
coexistence fault); removing it makes the join reliable again. (2) `delay(1)`
instead of `delay(5)` while the server is up made latency *worse* (lots of dropped
pings, big latency) rather than better — spinning the loop harder seems to hurt
the single-radio coexistence. Reverted both; default modem sleep + `delay(5)` is
what gave the good first run (med 26.8, 0 drops), so that's kept.

Takeaway: within STA-via-router the Wi-Fi tail is close to expected (per-request
TCP with no keep-alive, the extra router hop, single-radio coexistence). The real
levers are topology (SoftAP: phone talks straight to the board, drops the router
hop but also the phone's internet) and keep-alive (async server / persistent
socket) — both bigger changes, left as options. Builds clean (flash back to the
pre-tuning size).

### Coexistence is the real jitter source (clean measurement)

Re-ran the BLE echo with **Wi-Fi disconnected**: min 12.1 / med 22.5 / avg 22.5 /
p95 23.0 / max 33.9 / σ 3.1, 0 drops. Same median (the interval) but jitter fell
from σ 11.4 to 3.1 and the tail from max 56 to 34. That's the BLE/Wi-Fi
coexistence tax measured directly — one 2.4 GHz radio + one antenna, time-shared,
so each stack degrades the other while both run. Explains everything: the Wi-Fi
tail, the worse BLE-with-Wi-Fi run, and the `setSleep(false)` reset (worst case
for the arbiter). Good §5.9 result on its own.

On "can Wi-Fi hit gaming-grade latency here": yes in principle, no in this
architecture. Would need all of: BLE off while streaming (don't split the radio),
SoftAP (direct phone↔board, no router hop, AP doesn't sleep), a persistent/datagram
transport (WebSocket or WebRTC DataChannel, not one HTTP request per frame), and —
because browsers block WS/WebRTC from an HTTPS page to a plain-HTTP LAN device —
the page served *by the board* over HTTP (the device-hosted design we chose not to
use, decision 002). That's a different product; logging it as future work, not
retrofitting. See `journal/decisions/008-low-latency-control-path.md`.

### SoftAP latency experiment (measure the floor without coexistence)

Built a scoped experiment to actually measure the Wi-Fi floor with the coexistence
removed — the first two of those four steps (BLE off + SoftAP), keeping the same
HTTP `/lat` echo so it's directly comparable to the other tests. New `SYS` BLE
characteristic (`0011`, WRITE): write 0x01 → latches a one-shot NVS flag + reboots.
On that boot `setup()` reads/clears the flag and comes up as an open SoftAP
(`PCB1-LatencyAP`, 192.168.4.1) with the HTTP server and **no BLE init at all**, so
the radio isn't shared. RESET returns to normal. Chose reboot-into-mode over a
runtime radio switch on purpose — the runtime `setSleep(false)` flip is exactly
what reset the board, so bringing SoftAP up cleanly at boot with BLE never started
sidesteps that. Webapp: a "SoftAP round-trip" test in the Latency panel (Enter
SoftAP mode button + Run SoftAP test), same stats table (mode `softap`), pings the
fixed AP IP — no BLE/IP guard since BLE's off. Manual step is switching the phone's
Wi-Fi to the board's AP. Builds clean, UUID-sync green (16).

### Results: BLE wins, Wi-Fi is for bulk only

50 pings each, gap 0, BLE payload 8 B. All times in ms. Full results +
conditions in `journal/results/latency.md`.

| Path | n | min | median | avg | p95 | max | σ | drops |
|------|--:|----:|-------:|----:|----:|----:|--:|------:|
| BLE echo, Wi-Fi connected | 50/50 | 11.9 | 22.6 | 28.8 | 45.3 | 56.4 | 11.4 | 0 |
| BLE echo, Wi-Fi off       | 50/50 | 12.1 | 22.5 | 22.5 | 23.0 | 33.9 |  3.1 | 0 |
| Wi-Fi STA (via router)    | 50/50 | 11.6 | 26.8 | 44.9 | 88.4 | 116.9 | 29.5 | 0 |
| SoftAP (BLE off)          | 50/50 | 12.3 | 23.4 | 24.7 | 40.8 | 60.8 |  9.3 | 0 |

The conn-interval fix landed BLE at ~22.5 ms median, and standalone (Wi-Fi off)
its jitter is tiny (σ 3.1). SoftAP killed most of the Wi-Fi STA tail (p95 88→41,
σ 29.5→9.3) by dropping the router hop + coexistence, but it only *ties* BLE's
median and is still ~3× the jitter, with the per-request TCP handshake as the
floor. And it's far less practical (BLE off, manual AP switch, reboot/RESET, no
internet). So: **BLE stays the control path; Wi-Fi is the bulk-transfer channel.**
Gaming-grade would need WebSocket/WebRTC on a device-hosted page (future work).
The SoftAP experiment stays on the `feat/low-latency-control` branch as evidence,
not in mainline. See `journal/decisions/008-low-latency-control-path.md`.

### Curating mainline: drop the Wi-Fi motor path, keep the measurement

Acting on the results above, trimmed mainline to what's worth keeping. Removed the
Wi-Fi motor path (firmware `POST /servo`, the webapp `sendServoFrame` Wi-Fi
routing + the Servos-panel transport toggle + `net.js` `preferWifi`): Wi-Fi never
beat BLE for the small-payload round trip and was less practical, so motor
commands are BLE-only again. Kept the **Wi-Fi round-trip latency test** (`/lat` +
the panel) as a measurement tool, and the SD-upload Wi-Fi path is untouched. Added
a **Disconnect** button to the Wi-Fi panel (a 0-length-SSID write to
`WIFI_CONNECT`; firmware `wifi_scan::disconnect_sta()` disassociates but stays in
idle STA, replies status 3) so the Wi-Fi link can be dropped after testing to get
the BLE link back to its low-jitter, coexistence-free state. Builds clean,
UUID-sync green (15). The full Wi-Fi motor path remains on the
`feat/low-latency-control` branch.

## 2026-06-24

### Audio volume knob

Added a master volume control to the audio panel. Quick myth-bust first: volume
is not something you set "over I²S" — I²S is just the digital transport, and the
MAX98357A is a fixed-gain amp (gain set by the SD_MODE/GAIN resistor, no register
to write). So volume is done by scaling the PCM samples digitally in firmware
before `i2s_write`, reusing the headroom multipliers the render paths already had.
New `audio::set_volume(pct)` stores a `volatile float g_volume` (0..1) that both
`render_chunk` (presets) and `stream_file_chunk` (files) multiply by. Exposed over
a new BLE characteristic `AUDIO_VOL` (discriminator `0011`, WRITE|WRITE_NR, one
byte 0..100), applied straight in the GATT callback since it touches no file/SD
state. Webapp gets a slider in the Audio panel. RAM-only, resets to 100 % on boot
(NVS persistence left as a later add). UUID-sync green (16). See
`journal/decisions/009-audio-master-volume.md`.

### Volume NVS persistence + webapp leak fix + Wi-Fi credential persistence

Follow-ups on the volume work. (1) Volume now **persists to NVS** (namespace
`audio`, key `vol`). To avoid writing flash on every step of a slider drag,
`set_volume` only updates RAM + marks dirty, and the write is **debounced in
`audio::tick()`** (fires once the value's been stable ~1.5 s, skips a no-op
rewrite). Restored on boot, default 100 %. (2) Fixed a webapp listener leak in
`audio.js`: `initAudio()` runs on every reconnect, and it was binding anonymous
listeners (preset buttons + the volume slider) each time, so they stacked up.
Now the DOM is wired once behind a `wired` guard while the per-connect
`AUDIO_FILE` notify re-subscribe stays. The volume slider also coalesces writes
(one BLE write in flight), matching `servos.js`.

(3) **Wi-Fi credentials persist to NVS** (namespace `wificreds`). The board saves
SSID+password on a successful connect and a new **Reconnect saved** button (a
`0xFF` write to `WIFI_CONNECT`) rejoins without re-typing. Chose board NVS +
manual reconnect over auto-reconnect-on-boot on purpose: auto-connect would keep
Wi-Fi always on and hurt the BLE latency the control path depends on (decision
008). The board never auto-connects on boot. Creds survive an explicit
Disconnect (that's just freeing the radio, not "forget"). Firmware builds clean
(31 % flash), UUID-sync green (16, no new characteristic). See
`journal/decisions/010-wifi-credential-persistence.md`.

### SD upload: slow and failing — fixed the root causes

The Wi-Fi SD upload was slow and failed most of the time. Reading the path end to
end turned up several separate problems, not one.

The big one is **coexistence**. The S3 shares a single radio between BLE and
Wi-Fi, and on connect we ask for a tight 7.5–15 ms BLE interval with latency 0
(decision 008) for control responsiveness — which makes BLE grab a radio slot
every 7.5 ms even when idle and starves a concurrent Wi-Fi transfer. Decision 008
already measured this on a single ping; on a sustained upload it is worse, and a
long enough stall trips the WebServer's 5 s read timeout and fails the upload.
Fix: while a Wi-Fi upload runs, BLE has no traffic anyway, so `set_bulk_transfer`
relaxes the link (30–50 ms interval, slave latency 4) via `updateConnParams` and
restores the tight set when it finishes. Best case a big speed-up, worst case the
phone declines and nothing changes — and the link stays well inside the 4 s
supervision timeout. A deadline backstop in `ble::loop()` un-relaxes if an upload
never signals completion.

The other big one was a **plain bug**: `upload_begin` opened the file without
creating its parent folder, and FatFS won't make parents on open. So uploading to
`/audio/x.wav` (the real use case — playback is locked to `/audio`) failed to open
whenever `/audio` didn't exist yet, 500'd, and fell back to slow BLE. Added a
mkdir -p before the open.

Plus robustness: `upload_chunk` retries a short write a couple of times instead of
failing the whole transfer; the 500 body now names the real reason (no card /
mount / open / write) instead of "upload failed"; `HTTP_UPLOAD_BUFLEN` bumped to
4096 so the body lands on the card in bigger writes; and the webapp retries the
Wi-Fi upload up to 3× with backoff before dropping to BLE, surfacing the
firmware's error string. Firmware builds clean (`pio run -e pcb1`, 31 % flash),
UUID-sync unaffected. **Not bench-tested yet** — no board on hand; needs a re-run
of the upload + the decision-008 latency panel once I can test. Considered
swapping to `ESPAsyncWebServer` but left it as future work: untestable right now
and the coexistence relaxation is the cheaper, verifiable win. See
`journal/decisions/011-wifi-upload-robustness.md`.

## 2026-06-25

### SD upload bring-up: the real culprit was the SD write clock

Flashed yesterday's upload fixes and tried a real transfer (a 28 MB WAV). The
mkdir-p fix worked — `/audio/...` opens now — but the upload still failed, and the
serial log finally named the actual cause:

```
[W][sd_diskio.cpp:186] sdCommand(): token error [13] 0x23
[sdcard] upload_chunk: short write 0/4096 after retries
```

`0x23` isn't a valid SD SPI data-response token, so that's a write-path failure,
not a full/locked card. The card mounts fine (mount is just the 400 kHz init + a
CSD read). My first read was a clock / signal-integrity margin — that turned out
wrong (the next entries pin it on the multi-block write path, not the clock). The
chunk retry was useless either way.

Fix: `ensure_mounted` now walks a clock ladder and keeps the first clock that
passes a real one-sector **write + read-back probe** (since `SD.begin` succeeding
only proves reads). 20 MHz was dropped here on the wrong clock assumption (a later
entry puts it back). The chosen clock is logged.

Two more things the run showed and I fixed: the WebServer drained the whole 28 MB
body before sending the 500 (so attempt 1 hit the 120 s XHR timeout and a real
write error took ~2 min to surface) — now the upload callback closes the
connection the moment a write fails; and the XHR timeout is now sized to the file
(~50 kB/s floor) instead of a flat 120 s. Builds clean (31 % flash).

Throughput is ~200 kB/s, which is the bundled WebServer's byte-at-a-time read
ceiling, not SD or radio. Fine for the short nudge clips this is actually for; a
multi-MB file just takes a few minutes. Faster needs `ESPAsyncWebServer` (future
work). See `journal/decisions/011-wifi-upload-robustness.md`.

### SD upload, take 2: it was the multi-block write

The clock ladder helped but didn't fix it — re-flashed and the card write-verifies
at 10 MHz, yet the upload still failed with the same `token error [13] 0x23`.
Decoded it from `sd_diskio.cpp`: the `[13]` is the command, CMD13 (`SEND_STATUS`),
the post-write status poll, and `0x23` is the bad R1 byte. The mount probe writes
one sector → single-block write (CMD24), which passes; a 4 KB chunk in one
`f.write()` → multi-block write (CMD25), and it's that path's `SEND_STATUS` that
errors. So single-block is solid at 10 MHz, multi-block isn't. Fix: `upload_chunk`
now writes one 512-byte sector at a time, keeping every write on the working
single-block path (FatFS never issues CMD25). No throughput hit — the WebServer
read is still the ceiling. Builds clean.

### SD upload works — and a reset regression I caused

Single-block writes did it: the 28 MB Wi-Fi upload now completes end to end
(`upload_end: 28449988 bytes committed`, ~117 kB/s). But the SD self-test and the
BLE upload then **reset the board** — a regression from my mount write-probe. The
probe put two 512-byte buffers on the stack, and both of those paths call the SD
driver from inside the NimBLE GATT callback (host task, ~4 KB stack), so 1 KB of
probe buffers overflowed it. Wi-Fi upload was fine because it mounts from the main
loop task. Made the probe buffers `static` (off the stack) — SD access is
serialized so one shared pair is safe. Builds clean.

Speed is ~117 kB/s now that writes actually happen single-block (was ~200 kB/s
while discarding). Fine for the short nudge clips this is for; the 28 MB stress
file just takes ~4 min. Faster would need the async server or working multi-block
writes. Logged in decision 011.

### SD upload, final: it was never the clock

Tested the obvious question — was the "20 MHz" failure actually just the
multi-block thing all along? Put 20 MHz back at the top of the probe ladder and
re-flashed: `mounted, write-verified at 20000 kHz` and a full 28 MB upload
completed clean (~122 kB/s, same as 10 MHz). So single-block writes are fine at
20 MHz; the clock was a red herring and the original failure was purely the
multi-block (CMD25) `SEND_STATUS` path. Kept the card at 20 MHz and scrubbed the
wrong "signal integrity / future-revision routing" notes from the comments and
decision 011 — it's a card/library multi-block quirk, not a PCB hardware issue.
Speed is unchanged because the WebServer read, not the SPI clock, is the ceiling.

### Power-rail measurements (multimeter)

Took proper multimeter readings of the rails for the testing chapter. The two
rails straight off a converter are spot on: 3.3 V reads **3.29 V**, 5V-SERVO
reads **4.99 V** (the latter only exists on barrel-jack power, since its buck is a
step-down).

The shared 5 V logic/audio rail is the interesting one because it's power-OR'd,
so its value depends on which input feeds it:

- **USB:** VBUS 5.01 V → rail **4.86 V**, i.e. **0.15 V** across the D8 Schottky.
  That's a normal Schottky Vf and even better than the ~0.3 V design estimate.
- **Barrel jack:** TPS54202 output (5V-IN) 4.95 V → rail **4.44 V**, i.e.
  **0.51 V** across Q3 (DMG3415U) drain-source.

The 0.51 V is the odd one. A fully-enhanced pass FET should drop a few mV, not
half a volt — 0.51 V is the size of a forward silicon junction, so the current
looks like it's going through Q3's body diode rather than the channel on the
barrel-jack path. Checked the netlist: Q3.1(gate)=VBUS with R9 to GND as a
pulldown, Q3.2(source)=5V rail, Q3.3(drain)=5V-IN. With USB out, the gate is
pulled to 0 and the source sits at ~4.4 V, so Vgs ≈ −4.4 V and the channel *should*
enhance — which makes the diode-sized drop genuinely puzzling. Didn't chase the
root cause on the bench (functional testing only). Doesn't matter functionally:
4.44 V still leaves the AP2114H plenty of headroom (proven by the clean 3.29 V it
makes) and is well inside the MAX98357A range. Logged it honestly and flagged
"reduce the DC-path drop / revisit Q3" as a Medium next-rev recommendation.

Updated the thesis: filled the real numbers into the §5.2 rails table, added a
paragraph on the two-input 5 V drops, added the next-rev row in §6.6, and synced
the §5.2 bullet in `thesis-outline.md`. Also recorded measured values in
`topics/power-budget.md` (which previously only had design-time estimates).

### Chasing the 0.51 V Q3 drop — leading hypothesis (not yet bench-confirmed)

The DMG3415U datasheet gives the body-diode forward voltage V_SD at V_GS = 0 V as
**max −1.2 V** (typ/min not stated). The measured 0.51 V is comfortably under
that, so it is consistent with the body diode carrying the rail current. Body
diode conducting = **channel not enhancing = V_GS ≈ 0**, i.e. the gate is sitting
near the source (~4.4 V), not near ground.

Per the schematic that shouldn't happen. Net check: Q3 gate = VBUS, source = 5 V
rail, drain = 5V-IN; **R9 = 100 kΩ** from VBUS to GND is the gate pulldown; D8
(anode = VBUS, cathode = 5 V) blocks the rail from backfeeding VBUS. So with USB
unplugged VBUS should be pulled to ~0, giving V_GS ≈ −4.4 V and a hard-on channel
(drop in mV). The body-diode-sized drop means the gate is *not* being held low.

Things on the VBUS net besides the gate + R9: D8 (PMEG3050EP Schottky) and D1
(LESD5D5.0CT1G ESD diode to GND). D1 leaks VBUS→GND so it *helps* the pulldown.
The only thing that can charge VBUS **up** is D8's reverse leakage from the 4.44 V
rail. To bias the gate to ~3.7 V (within a threshold of the source, so the channel
stays off) through 100 kΩ needs ~37 µA of D8 leakage. That's high for a Schottky
at room temp and only a few volts reverse, and it self-limits (as VBUS rises the
reverse voltage across D8 falls), so pure room-temp leakage probably **doesn't**
account for it on its own. Candidate causes, in rough order:

1. **Gate pulldown not effective** — R9 open / poor joint / wrong value, so the
   gate floats up near the source via Cgs/Cgd and Q3 never turns on. (If both
   boards show it identically, a random joint is less likely → leans design.)
2. **Gate node too high-impedance** — 100 kΩ is a weak pull on a net shared with
   leaky parts; marginal even if it works on some units/temps.
3. **Gate-net / footprint mismatch** on Q3 so the physical gate isn't tied to the
   intended pulldown.

**Decisive bench checks (do these next):** (a) measure V(VBUS / Q3 gate) to GND on
barrel-jack power, USB unplugged — predict it sits well above 0 (volts), should be
~0; (b) measure R9 in-circuit and reflow it; (c) tack a 1–10 kΩ from gate to GND
and watch the rail — if it jumps from 4.44 V toward ~4.95 V, that confirms a
pulldown problem. Also note whether the second board behaves the same.

**Next-rev fix regardless of which:** a strong local gate pulldown (~1–10 kΩ) at
Q3, or replace the discrete USB-vs-DC OR with a proper ideal-diode / power-mux
controller. Board is fine meanwhile (rail 4.44 V, LDO makes a clean 3.29 V).

Kept the thesis wording as "exact cause was not chased down on the bench" since
this is still a hypothesis; will finalise §5.2 once the gate voltage is measured.

### Gate measured: it's held at the source potential (leakage into the gate)

Measured on the bench: **4.44 V across R9, i.e. Q3's gate sits at 4.44 V to GND** =
the source/rail potential, so **V_GS ≈ 0** and the channel is held off, exactly the
"body diode carries the rail" condition. So R9 is being *overpowered*: 4.44 V over
100 kΩ = **~44 µA being injected into the gate node**. A healthy MOSFET gate leaks
<1 µA (I_GSS max ~100 nA), and on a clean board R9 would hold the gate at ~0 V, so
this 44 µA is abnormal — something with roughly 10 kΩ-class impedance is feeding
the gate from a 5 V node.

What this rules in/out:
- **Not a hard gate-source short** (solder bridge across pins 1–2): a dead short
  would force VBUS = rail, but on USB they differ (VBUS 5.01 V, rail 4.86 V across
  D8). So it's a *finite* leakage (~10 kΩ-ish), not a bridge.
- **Not D8 reverse leakage**: with the gate at 4.44 V and the rail at 4.44 V there's
  ~0 V across D8, so D8 can't be the 44 µA source. The current comes from a path to
  the source (4.44 V rail) or the drain (4.95 V, 5V-IN) into the gate.
- **Consistent with USB**: when USB drives VBUS hard to 5.01 V it swamps the ~44 µA
  leak, Q3 stays off for isolation, rail = 4.86 V via D8 — all as measured.

Leading cause: **a leaky gate on Q3** (gate-oxide damage from ESD/overstress giving
gate-to-source/drain leakage) **or surface leakage/contamination** (flux residue
between Q3's gate pad and an adjacent power pad). Both inject current into the gate;
the weak 100 kΩ pulldown can't hold against a ~10 kΩ leak.

**Tests to isolate it (de-powered, ohmmeter):** measure Q3 **gate→GND** (expect
~100 kΩ = R9; lower ⇒ extra leakage), **gate→source** and **gate→drain** (a healthy
gate is MΩ+/open; a finite ~10 kΩ reading reveals the leak and which node it's to).
Then: clean the Q3 area with IPA and re-measure (gone ⇒ contamination); if it
persists, **replace Q3** and re-test the barrel-jack rail (predict gate → ~0 V,
rail → ~4.95 V). Check whether **board 2** shows the same (one-off damage vs a
systematic process/layout issue). Independent of root cause, a **1–10 kΩ gate
pulldown** would swamp a ~100 kΩ-class leak and is the robust next-rev fix.

Thesis still says "exact cause was not chased down on the bench" — accurate, since
the leak isn't isolated yet. Can tighten §5.2 to the measured fact (gate at source,
channel off, rail via body diode) now, and name the root cause after the swap/clean.

### Latency results consolidated + command-to-movement test dropped

Wrote up the latency results in a dedicated `journal/results/latency.md` (full
8-column table, conditions, verdict) and expanded this log's summary table to all
reported metrics. Measured on a Galaxy S23: BLE round trip ~22.5 ms median (σ 3.1
with Wi-Fi off, 11.4 with it on, so coexistence is the jitter source); Wi-Fi STA
and SoftAP are both worse or only tied and less practical, so BLE stays the control
path. MediaPipe hand-tracking inference is ~32 ms/frame at ~30 fps, so tracking
keeps pace with the camera.

Dropped the third latency test (command-to-movement: the FIRE button + full-screen
flash marker). At this scale it can't be measured meaningfully: the ESP32 starts
the servo PWM in an interrupt within ~1 ms of the command (negligible), the servo's
mechanical travel is a motor issue out of scope, and a screen-flash marker is
dominated by the phone's display + refresh latency that needs an LDAT to separate.
Removed the flash overlay + FIRE button + `SERVO_SET` mover from `latency.js`,
`index.html`, and `style.css`, and reframed the experiment to the two real numbers
in the results file, decision 007 (revision note), the webapp README, and
thesis §5.9.

## 2026-06-26

### Q3 root cause confirmed: failed gate oxide (gate shorted to source)

De-powered ohmmeter/diode-mode measurements on Q3 and neighbours settle the 0.51 V
drop. The smoking guns are the gate-isolation readings — a healthy MOSFET gate is
open (MΩ+) to both other terminals:

- **Gate–source: 72.3 Ω** (should be MΩ+). Gate is essentially **shorted to the source**.
- **Gate–drain: 8.86 kΩ** (should be open). Gate also **leaks to the drain**.
- Source–drain: 200 kΩ — normal off-state (channel off, body diode reverse this way).
- Body diode forward (diode mode): 0.376 V — **intact**, and it's the conduction path
  (rises to ~0.51 V at the rail's load current).

So Q3's **gate oxide has failed**. With the gate tied to the source (72 Ω) the gate
can't be driven below the source, V_GS is pinned at ~0, the channel never enhances,
and the barrel-jack rail is delivered through the body diode → the 0.51 V drop.
This is exactly the earlier bench picture (gate measured at 4.44 V = source). It's a
**damaged component**, not a topology error, and it confirms (and supersedes) the
earlier "leaky gate" hypothesis.

The two odd in-circuit readings are explained by the **same short**, since the gate
is on VBUS and the source is on the 5 V rail, so the 72 Ω gate–source short bridges
VBUS↔5V rail right across D8:
- **"D8 = 0.040 V" in diode mode** is the 72 Ω short read in parallel with D8, not D8
  shorted. D8 is fine — USB still drops a healthy ~0.15 V under load (4.86 V from 5.01 V).
- **"R9 = 15.76 kΩ" in-circuit** is R9 (100 k) ∥ (the 72 Ω short → 5 V rail → its
  ground paths), not R9's true value. R9 is fine.
- **R19 = 1.196 kΩ** (≈ nominal 1.2 k) is a good control: the meter is accurate, so
  the 72 Ω reading is trustworthy.

Likely failure mechanism: the gate is tied **directly to the externally-exposed USB
VBUS net with no series resistor or gate clamp**. Normal operating V_GS (+0.15 V on
USB, −4.4 V on the jack) is well within the ±12 V rating, so this isn't operational
overstress — it points to an ESD or USB hot-plug transient (VBUS inrush/ringing)
punching through the thin gate oxide. The LESD5D5.0 (D1) clamps VBUS but the gate
itself is unprotected.

**Both assembled boards show the same Q3 failure**, which rules out a random one-off
ESD kill and makes it **systematic** — a cause that repeats on every board, i.e. the
design itself, not bad luck on one part. Given the topology that means the
unprotected gate on VBUS: the gate is tied straight to the externally-exposed USB
VBUS net with no series resistor and no dedicated gate clamp, so it sees whatever
transient lands on VBUS (USB hot-plug inrush/ringing, ESD). The board is connected
over USB constantly (it is the programming port), so any per-connect stress repeats
on every board. The **exact** overstress path isn't pinned down: note the source
tracks the gate through D8, so V_GS stays small, which points more at a gate–drain
stress (on USB-only the buck is off, so the drain sits near 0 V while the gate is at
VBUS) or an ESD event than a simple V_GS spike. A scope on VBUS and the gate during
hot-plug would settle the mechanism; the failure and its systematic (both-board)
nature are not in doubt.

Actions: (1) replace Q3 on a board and re-test — predict gate–source goes open and
the barrel-jack rail jumps to ~4.95 V; (2) next-rev hardening: a series gate resistor
(~1–10 kΩ) between VBUS and the gate plus the pulldown (and optionally a gate–source
Zener clamp), or a dedicated ideal-diode / power-mux controller whose gate isn't
exposed. This turns the §6.6 "revisit Q3" item from "reduce the drop" into "protect
the gate drive".

Thesis not yet updated — with both boards affected this is effectively a **second
design fault**: a latent reliability defect (the board still works through the body
diode, but the OR-ing FET is being destroyed by the unprotected gate), to document
alongside the LED. The power subsystem still *functions* on the first board, so the
validation-success narrative holds; needs a deliberate framing call before editing
§5.2 / §6.6 / §6.3.
