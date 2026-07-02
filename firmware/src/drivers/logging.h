#pragma once

#include <Arduino.h>

// Exercise position logging. While an exercise is running the board samples the
// five finger positions and writes them, as normalized flexion percent, to a
// CSV file in the SD card's /logs folder. One file per exercise session plus a
// manifest (index.csv).
//
// An exercise is detected purely by the command channel: only SERVO_FRAME
// streams (live hand-mirror and recorded-clip playback) count. Manual sliders
// (SERVO_SET) and calibration (SERVO_CAL) are never logged. A session opens on
// the first exercise frame and closes after an idle gap with no frames, so each
// bout of activity becomes its own file. See drivers/logging.cpp and
// journal/decisions/006-exercise-position-logging.md.

namespace logging {

// Loads the enable flag and the next session counter from NVS. Call once at boot
// after sdcard::init().
void init();

// Main-loop service: opens / samples / closes the session file. All SD work
// happens here so the single open File handle is only touched by the loop task
// (same rationale as the audio and Wi-Fi defers). Cheap no-op when idle.
void tick();

// Marks exercise activity. Lightweight and callback-safe: it only stamps the
// last-activity time and sets a pending flag, so it can be called straight from
// the SERVO_FRAME GATT callback on the NimBLE host task. No SD work here. Call
// this ONLY from the exercise path, never from manual / calibration commands.
void note_exercise_frame();

// Enables / disables logging; persisted to NVS. Disabling closes any open
// session. Logging is on by default.
void set_enabled(bool on);
bool is_enabled();

// Pushes wall-clock time (epoch milliseconds, from the webapp) so session files
// get real date-stamped names and headers. Until set, files use the monotonic
// session counter (sess-NNNNN.csv) and a boot-relative start stamp.
void set_wall_clock(uint64_t epoch_ms);

// Status, for the BLE log-control notification.
bool     is_active();
uint16_t current_session();   // id of the active (or most recent) session
uint32_t frames_written();    // rows written in the active session

}  // namespace logging
