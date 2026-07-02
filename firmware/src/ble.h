#pragma once

namespace ble {

// Brings up the NimBLE host, registers the validation service, starts advertising.
void init();

// Called from loop(); pushes periodic status notifications to subscribed clients.
void loop();

// Bulk-transfer mode: while a Wi-Fi SD upload runs, relax the BLE connection so
// it stops stealing radio airtime from Wi-Fi (true on upload start), then put
// the tight control interval back (false on finish). No-op when no client is
// connected. See journal/decisions/011-wifi-upload-robustness.md.
void set_bulk_transfer(bool active);

}  // namespace ble

// --- BLE service / characteristic UUIDs -------------------------------------
// One service, fifteen characteristics. UUIDs share a common prefix so they're
// easy to spot in a BLE inspector. The 16-bit field at positions 4-7 is the
// per-characteristic discriminator (0001 = service itself). Discriminator 0006
// (NeoPixel) is retired — the WS2812B LEDs are non-functional on PCB1 (reversed
// supply pins; see journal/decisions/001-ws2812b-power-pin-reversal.md).
#define BLE_UUID_SERVICE       "5e6c1b00-0001-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_STATUS        "5e6c1b00-0002-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_SERVO_POWER   "5e6c1b00-0003-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_SERVO_SET     "5e6c1b00-0004-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_BUZZER        "5e6c1b00-0005-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_AUDIO_PLAY    "5e6c1b00-0007-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_SD_TEST       "5e6c1b00-0008-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_WIFI_SCAN     "5e6c1b00-0009-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_WIFI_CONNECT  "5e6c1b00-000a-4a4e-a5f0-5e0e6a5a0001"
#define BLE_UUID_SD_UPLOAD     "5e6c1b00-000b-4a4e-a5f0-5e0e6a5a0001"
// Discriminator 000c: one write sets all 5 servos at once (5-byte frame, one
// NORMALIZED finger position 0..180 per channel, 0 = open, 180 = closed). The
// firmware remaps each channel into its calibrated [open,closed] band. Used for
// streamed hand-tracking playback / live mirror — far cheaper than 5× SERVO_SET.
#define BLE_UUID_SERVO_FRAME   "5e6c1b00-000c-4a4e-a5f0-5e0e6a5a0001"
// Discriminator 000d: per-finger calibration + safety-limit control (jog,
// capture open/closed, save/reset to NVS, move-to-open). Write a small
// command, notify replies with the per-channel calibration. See servos.{h,cpp}
// and journal/decisions/004-servo-finger-calibration-safety-limits.md.
#define BLE_UUID_SERVO_CAL     "5e6c1b00-000d-4a4e-a5f0-5e0e6a5a0001"
// Discriminator 000e: play WAV files from the SD card's dedicated /audio
// folder. Write LIST/PLAY/STOP commands; notify replies carry the folder's
// file list (chunked) and play status. Strictly confined to that folder — see
// sdcard::list_audio/open_audio and journal/decisions/005-sd-wav-audio-playback.md.
#define BLE_UUID_AUDIO_FILE    "5e6c1b00-000e-4a4e-a5f0-5e0e6a5a0001"
// Discriminator 000f: exercise position logging control. The board auto-logs
// the five finger positions to a CSV per session in the SD card's /logs folder
// during exercises (SERVO_FRAME streams only). Write ENABLE / SET_TIME / STATUS
// commands; notify replies carry a status frame. WRITE|NOTIFY. See
// drivers/logging.{h,cpp} and journal/decisions/006-exercise-position-logging.md.
#define BLE_UUID_LOG_CTRL      "5e6c1b00-000f-4a4e-a5f0-5e0e6a5a0001"
// Discriminator 0010: latency ping. Echoes the written payload straight back as
// a notification, immediately in the GATT callback (no deferral), so the webapp
// latency panel can measure BLE round-trip time. Payload is opaque to the
// firmware. WRITE|WRITE_NR|NOTIFY. See webapp/js/latency.js and
// journal/decisions/007-latency-test-suite.md.
#define BLE_UUID_PING          "5e6c1b00-0010-4a4e-a5f0-5e0e6a5a0001"
// Discriminator 0011: audio master volume. One byte, 0..100 (%). The MAX98357A
// is a fixed-gain amp with no volume register, so the firmware scales the PCM
// samples digitally (see audio::set_volume). Applied immediately in the GATT
// callback. WRITE|WRITE_NR. See journal/decisions/009-audio-master-volume.md.
#define BLE_UUID_AUDIO_VOL     "5e6c1b00-0011-4a4e-a5f0-5e0e6a5a0001"
