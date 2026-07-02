# firmware — PCB1 validation firmware

Bring-up firmware for the rehabilitation-hand controller PCB. Exposes every
working subsystem (servos, audio, buzzer, microSD, Wi-Fi) over a custom BLE
GATT service, designed to be driven from the validation webapp in `../webapp/`.

> The WS2812B RGB LEDs are **not** exposed: they are non-functional on PCB1
> (reversed supply pins — see
> `../journal/decisions/001-ws2812b-power-pin-reversal.md`), so all LED
> handling was removed from this firmware.

## Stack

- **PlatformIO** + Arduino-ESP32 (`espressif32@~6.7.0`)
- **NimBLE-Arduino** for BLE (much less RAM than Bluedroid)
- **Direct LEDC PWM** for the five servo channels (`ledcSetup`/`ledcAttachPin`/`ledcWrite`, channels 0–4 at 50 Hz/14-bit). ESP32Servo was dropped: its auto channel allocation aliased pins on the S3 (one slider drove several motors).
- Built-in `SD.h` (over SPI) and `driver/i2s.h` (for preset tones through MAX98357A)

## Layout

```
include/config.h         pin assignments (matches journal/topics/esp32-pinout.md)
src/main.cpp             setup/loop, prints boot banner over native USB-CDC
src/ble.{h,cpp}          NimBLE GATT server + characteristic dispatch
src/drivers/             per-subsystem code (one .h/.cpp pair each)
```

## Flash + monitor

Install PlatformIO Core (CLI) or the VS Code extension. From this folder:

```pwsh
pio run                    # build
pio run -t upload          # flash over USB-C (native USB-CDC, no extra bridge)
pio device monitor         # 115200, with exception decoder filter
```

If the board doesn't enumerate on first connection, hold SW1 (BOOT, GPIO0) and
tap SW2 (RESET) to enter download mode, then release SW1.

## CI

`.github/workflows/firmware.yml` runs `pio run -d firmware` on every push/PR
that touches `firmware/**`, so a broken build is caught before merge. A
separate `ble-uuid-sync` workflow asserts the BLE UUIDs here match the webapp
(see `tools/check_ble_uuids.py`).

## BLE service

One custom 128-bit service (`5e6c1b00-0001-…`) with these characteristics —
all payloads little-endian, fixed length:

| Characteristic | Properties | Payload |
|---|---|---|
| `status`        | READ, NOTIFY | `u8 flags`, `u8 servo_rail_on`, `u8 sd_present`, `u8 wifi_connected`, `u32 uptime_ms`, `u32 free_heap` |
| `servo_power`   | WRITE        | `u8 enable` |
| `servo_set`     | WRITE        | `u8 channel`, `u16 angle_deg_x10` (clamped to the calibrated band) |
| `servo_frame`   | WRITE        | `u8 norm[5]` — normalized finger pos 0..180 per channel, remapped into each band |
| `servo_cal`     | WRITE, NOTIFY | calibration command (see below) → per-channel calibration frame |
| `buzzer`        | WRITE        | `u16 freq_hz`, `u16 duration_ms` (0/0 = stop) |
| `audio_play`    | WRITE        | `u8 preset_id` (0..4) |
| `audio_file`    | WRITE, NOTIFY | WAV playback from the SD `/audio` folder (see below) |
| `sd_test`       | WRITE, NOTIFY | trigger byte → `u8 result`, `u32 size_mb`, `u32 free_mb` |
| `wifi_scan`     | WRITE, NOTIFY | trigger byte → packed `i8 rssi, u8 ssid_len, char[…]` list, empty notification terminates |
| `wifi_connect`  | WRITE, NOTIFY | `u8 ssid_len, ssid…, u8 pw_len, pw…` → `u8 status, u8 ip[4]` |
| `ping`          | WRITE, NOTIFY | latency echo: the written payload is notified straight back inside the callback (no defer) so the webapp can time the BLE round trip |

Advertise name: `PCB1-Validator`.

## Finger calibration & safety limits

Once the servos pull the real fingers through the tendon strings, the firmware
keeps every command inside each finger's safe travel so a finger can't be
over-tensioned. Hobby servos are absolute-position, so a given command always
drives to the same physical angle: there is no homing step, just stored values
that stay valid across power cycles as long as the ropes aren't repositioned.
Each channel stores `open_cmd`, `closed_cmd` and `home_cmd` (the open/park end)
in NVS (`Preferences`); `servos::set_angle` clamps to the `[open,closed]` band and
`set_finger` remaps a normalized 0..180 value into it, so sliders, `servo_frame`,
playback and live-mirror are all protected. `init()` makes **no move on boot**
(the rail is off and the servos stay limp, so a reset mid-motion can't jerk a
rope); "move to open" ramps gently to `home_cmd` for parking.

`servo_cal` command bytes (little-endian); every command is logged on serial and
the reply is a per-channel calibration notify (`op, ch, u16 open, u16 closed,
u16 home, u8 flags, u16 cur, u8 rsv`; `flags` bit0 = calibrated, bit1 =
calibration mode):

| Op | Command | Payload |
|---|---|---|
| `0x01` | MODE | `u8 on` — relax limits to jog freely |
| `0x02` | JOG | `u8 ch`, `i16 delta_x10` (firmware-capped) |
| `0x03` | GOTO | `u8 ch`, `u16 target_x10` (ramped) |
| `0x04` | CAPTURE | `u8 ch`, `u8 which` (0 open+park, 1 closed, 2 park only) |
| `0x05` | SAVE | — persist to NVS |
| `0x06` | RESET | — clear calibration |
| `0x07` | move to open | `u8 ch` (`0xFF` = all), ramped park to the open end |
| `0x08` | READ | `u8 ch` (`0xFF` = all) |

The measurement routine is run from the webapp's Finger Calibration panel. See
`../journal/decisions/004-servo-finger-calibration-safety-limits.md`.

## SD audio playback (`/audio` folder)

`audio_file` plays **16-bit PCM WAV** files (mono or stereo, any sample rate)
streamed off the card straight into the I²S amp. Playback and the file list are
**strictly confined to the card's `/audio` folder** — `sdcard::list_audio()` and
`sdcard::open_audio()` are the only paths that reach the card for audio, and
both hard-code the folder prefix and reject any name with `/`, `\`, or `..`, so
nothing outside `/audio` can ever be listed or played. Upload clips there with
the Wi-Fi/BLE uploader using a path like `/audio/clip.wav`.

Command bytes (write); replies are notifications tagged by a leading type byte:

| Op | Command | Payload | Reply |
|---|---|---|---|
| `0x01` | LIST | — | one or more `0x01` chunks of `u8 name_len, name…, u32 size`, then a `0x00` end byte |
| `0x02` | PLAY | `u8 name_len, name…` (bare filename in `/audio`) | `0x02 u8 result` (0 = playing, 1 = open/format failure) |
| `0x03` | STOP | — | `0x02 0x02` (stopped) |

All audio control (presets included) is latched in the GATT callback and run
from the main loop task, because file streaming reads the card from `loop()` and
the file handle must stay single-task. See
`../journal/decisions/005-sd-wav-audio-playback.md` for the format choice and the
upload-while-playing limitation.

## Wi-Fi file upload (fast SD transfer)

BLE is the control channel, but the chunk-acked BLE upload (`sd_upload` above)
is slow — one write-with-response round-trip per 180 bytes. For bulk SD
transfers the firmware brings up a small HTTP server (`src/drivers/webfs.cpp`,
built-in `WebServer` on port 80) the moment the board joins a network as a STA
(triggered from `wifi_connect`). The webapp learns the board's IP over BLE and
POSTs the file straight to it; the upload is **streamed** to the SD driver, so
nothing large is buffered in RAM (no PSRAM on this module).

| Endpoint | Method | Notes |
|---|---|---|
| `/upload?path=/foo.bin` | POST | `multipart/form-data`, file streamed to SD → `{"received": N}` |
| `/ping` | GET | returns the firmware name; reachability probe |

All responses carry `Access-Control-Allow-Origin: *` (the request comes from the
HTTPS webapp, a different origin). The HTTPS page is allowed to reach this
plain-HTTP endpoint via Chrome's **Local Network Access** (Chrome ≥142): a
request to a private-IP literal is exempted from the mixed-content block once
the user grants the one-time local-network permission. No new BLE
characteristics are involved, so the UUID-sync check is unaffected. Rationale
and the SoftAP-vs-STA trade-off are in
`../journal/decisions/002-wifi-file-transfer-lna.md`.

> Trade-off: the upload runs to completion inside `WebServer::handleClient()`
> (called from `loop()`), so BLE status notifications pause for the transfer's
> duration. Acceptable for a validation tool; an async server (`ESPAsyncWebServer`)
> would avoid it if this ever needs to be concurrent.

## Power note

Servos only move when the **DC barrel jack** is providing input. USB-C alone
boots the board and runs the firmware, but the TPS56637 buck for `5V-SERVO`
can't step up from 5 V to 5 V — the rail stays unpowered. Firmware does not
detect this; the webapp shows a permanent banner. See
`../journal/topics/board-architecture.md`.
