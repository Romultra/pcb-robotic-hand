# 006: Log exercise finger positions to CSV files on the SD card

**Date:** 2026-06-22
**Status:** Decided

## Context

Logging the hand's position history during exercises was scoped at the start of
the project but never built. It is a stated requirement, not an extra: the PDR
research question asks about a "local storage architecture for long-term medical
data logging without cloud dependency", the project description names a module
for "long-term logging of hand position and movement history", and the thesis
requirements list it as mandatory (STO-1 log session/movement history to
on-device storage, STO-2 survive power cycle, STO-3 removable multi-GB). The
microSD slot and its driver work, but until now only the self-test, chunked
upload, and audio playback used the card. This adds the first real use of the
storage subsystem for its intended purpose.

Two facts shaped the design. The board has no position encoders, so the only
position it knows is the one it commanded. And it has no RTC and no NTP, so it
has no wall clock of its own; the only time it can name files with is whatever
the webapp pushes over BLE.

## Options Considered

1. **Where the log lives — board (SD) vs browser.** The requirement is explicit
   that the data is medical and stays on-device with no cloud, retrieved by
   pulling the card. Board-side logging to the microSD is the only option that
   meets that. The browser already keeps recorded clips in localStorage, but
   that is the author's recording, not the patient's session history.
2. **What to record — normalized flexion % vs raw servo command.** Raw command
   (tenths of a degree) is the exact hardware value but is tied to the current
   calibration, so it is not comparable across a recalibration. Normalized
   flexion percent (0 = open, 100 = closed) is calibration-independent and is
   the clinically meaningful "how far each finger moved". Chose flexion %, with
   the calibration band recorded in each file header so raw degrees stay
   reconstructable.
3. **What counts as a session — every command vs exercises only.** Logging every
   command would capture manual slider fiddling and calibration jogs, which are
   not exercises. The board already has a clean discriminator: exercises stream
   over `SERVO_FRAME` (live hand-mirror and recorded-clip playback), while manual
   sliders use `SERVO_SET` and calibration uses `SERVO_CAL`. Chose to log only
   `SERVO_FRAME`, automatically: a session opens on the first exercise frame and
   closes after an idle gap, so each bout of activity is its own file.
4. **File naming without a clock.** A monotonic session counter in NVS always
   works but produces opaque names (`sess-00042.csv`). Real date-time names are
   far better for a therapist but need a clock. Chose both: when the webapp has
   pushed wall-clock time the file is named `YYYYMMDD-HHMMSS.csv`, otherwise it
   falls back to the counter. The session id is recorded inside the file and the
   manifest either way.

## Decision

**Board logs to a dedicated `/logs` folder, one CSV per exercise session, plus a
manifest.** Folder confinement reuses the `/audio` pattern: `sdcard::open_log()`
and `list_logs()` are the only paths that reach `/logs`, both going through the
same bare-filename guard (`name_is_safe`, rejecting `/`, `\`, `..`) now shared
with `open_audio`, so a log file can never escape the folder.

**Automatic, exercise-only sessions.** A new `logging` driver runs its state
machine from the main loop (`logging::tick()`). The `SERVO_FRAME` GATT handler
calls `logging::note_exercise_frame()` (lightweight, callback-safe, no SD work);
nothing else does. `tick()` opens a session on the first exercise frame when the
servo rail is powered and calibration mode is off, samples the five
`servos::flexion_pct()` values at 20 Hz, flushes every second, and closes the
file after a 3 s idle gap. On close it appends a row to `index.csv` and bumps the
NVS session counter. All file work stays on the loop task, matching the
single-task SD rule used by audio and the Wi-Fi defer.

**File format.** Each session file is self-contained: a short `#`-comment header
(session id, firmware version, sample rate, start stamp, per-finger calibration
band, units) then a `t_ms,thumb,index,middle,ring,pinky` table of flexion
percent. The `#` headers are pandas-friendly (`comment='#'`). `index.csv` lists
every session (id, filename, start, duration, frames, rate, powered) so the card
is browsable without opening each file.

**BLE control.** A new `log_ctrl` characteristic (discriminator `000f`,
WRITE|NOTIFY) carries `ENABLE` (on/off, persisted to NVS), `SET_TIME` (epoch ms
from the browser, for dated filenames), and `STATUS` (query). Status notifies
also fire automatically when a session opens or closes. The commands are latched
and serviced from `ble::loop()`, same defer pattern as Wi-Fi and audio. Logging
is on by default.

**Scope.** Firmware only for now. Retrieval is by pulling the card, which matches
the original "clinician pulls the card" vision. The webapp side (a logs panel
that pushes `SET_TIME` and lists/downloads sessions over the existing Wi-Fi/BLE
transport, plus `LIST`/`DELETE` opcodes) is left for later; the UUID is mirrored
into `webapp/js/ble.js` only so the firmware/webapp UUID sets stay in sync.

> **Known limitations (documented, not guarded):** positions are commanded, not
> sensed (no encoders), so the log reflects what the board drove, which is all
> the hardware can know. A power loss costs at most ~1 s of unflushed rows. As
> with audio (decision 005), SD access is not yet mutex-protected, so logging
> concurrently with an upload or self-test would cross tasks on the SPI bus; an
> audio cue playing during an exercise opens a second file handle on the same
> volume, which FatFs allows. None of these happen in normal validation.

## Sources

- Requirements STO-1/2/3 (`thesis/Chapters/03_Requirements.tex`) and the PDR
  (`plan/research question.tex`, `plan/project description.tex`).
- Existing repo patterns reused: the `/audio` folder confinement and the
  defer-to-`ble::loop()` mechanism (`journal/decisions/005-sd-wav-audio-playback.md`),
  the per-finger calibration band (`journal/decisions/004-servo-finger-calibration-safety-limits.md`).
- `journal/topics/storage.md` for the FatFs-over-SDSPI choice and write-rate headroom.
