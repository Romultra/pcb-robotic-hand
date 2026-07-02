#include "logging.h"

#include <FS.h>
#include <Preferences.h>
#include <time.h>

#include "config.h"
#include "sdcard.h"
#include "servos.h"

namespace logging {

namespace {

constexpr int N = cfg::SERVO_COUNT;
// The CSV header and row formatter name the five fingers explicitly, so the
// column count is fixed. Guard against a config change silently desyncing them.
static_assert(N == 5, "logging CSV is written for exactly 5 finger columns");

constexpr const char* NVS_NS    = "hndlog";
constexpr const char* NVS_EN    = "en";
constexpr const char* NVS_SEQ   = "seq";
constexpr const char* kIndexName = "index.csv";
constexpr const char* kFingerNames = "thumb,index,middle,ring,pinky";

bool     g_enabled = true;     // logging on by default; NVS may override
uint32_t g_seq     = 1;        // next session id, persisted across power cycles

// Wall clock, if the webapp has pushed one: epoch_ms == g_epoch_at_ref at the
// instant millis() == g_epoch_ref_ms. Without it, filenames use the counter.
uint64_t g_epoch_at_ref = 0;
uint32_t g_epoch_ref_ms = 0;
bool     g_have_clock   = false;

// Activity, set by the BLE host task in note_exercise_frame(), consumed on the
// main loop in tick(). millis() and these plain reads/writes are safe to share.
volatile bool     g_frame_pending    = false;
volatile uint32_t g_last_activity_ms = 0;

// Active-session state — main loop task only.
bool     g_active         = false;
File     g_file;
String   g_session_fname;
String   g_start_stamp;
uint16_t g_session_id     = 0;
uint32_t g_session_start  = 0;
uint32_t g_frames         = 0;
uint32_t g_last_sample    = 0;
uint32_t g_last_flush     = 0;
bool     g_session_powered = false;

uint32_t sample_rate_hz() {
    return cfg::LOG_SAMPLE_PERIOD_MS ? (1000u / cfg::LOG_SAMPLE_PERIOD_MS) : 0;
}

// Current wall-clock time as "YYYY-MM-DDTHH:MM:SSZ", or a boot-relative stamp
// "boot+<ms>ms" when no clock has been pushed yet.
String stamp_now() {
    char b[32];
    if (g_have_clock) {
        const uint64_t ms = g_epoch_at_ref + (uint64_t)(millis() - g_epoch_ref_ms);
        const time_t   s  = (time_t)(ms / 1000ULL);
        struct tm tmv;
        gmtime_r(&s, &tmv);
        strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
        return String(b);
    }
    snprintf(b, sizeof(b), "boot+%lums", (unsigned long)millis());
    return String(b);
}

// File name for a new session: a real date-time when the clock is known, else
// the monotonic counter. The 3 s idle gap guarantees date-named sessions are
// always > 3 s apart, so the second resolution can't collide.
String session_filename() {
    char b[32];
    if (g_have_clock) {
        const uint64_t ms = g_epoch_at_ref + (uint64_t)(millis() - g_epoch_ref_ms);
        const time_t   s  = (time_t)(ms / 1000ULL);
        struct tm tmv;
        gmtime_r(&s, &tmv);
        strftime(b, sizeof(b), "%Y%m%d-%H%M%S.csv", &tmv);
        return String(b);
    }
    snprintf(b, sizeof(b), "sess-%05u.csv", (unsigned)g_session_id);
    return String(b);
}

void load_state() {
    Preferences prefs;
    // Open read-write so the namespace is created on first boot (no NOT_FOUND
    // log before anything is saved). Defaults apply when keys are absent.
    if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return;
    g_enabled = prefs.getBool(NVS_EN, true);
    g_seq     = prefs.getUInt(NVS_SEQ, 1);
    prefs.end();
}

void save_seq() {
    Preferences prefs;
    if (prefs.begin(NVS_NS, /*readOnly=*/false)) {
        prefs.putUInt(NVS_SEQ, g_seq);
        prefs.end();
    }
}

void start_session() {
    g_session_id    = (uint16_t)g_seq;
    g_session_fname = session_filename();

    g_file = sdcard::open_log(g_session_fname, /*append=*/false);
    if (!g_file) {
        // No card / open failed: stay inactive and retry on the next frame.
        Serial.printf("[logging] open failed for %s; not logging this bout\n",
                      g_session_fname.c_str());
        return;
    }

    g_session_start   = millis();
    g_start_stamp     = stamp_now();
    g_session_powered = servos::is_power_on();

    g_file.printf("# pcb1 hand log  session=%u  fw=%s  rate_hz=%u\n",
                  (unsigned)g_session_id, cfg::FW_VERSION, (unsigned)sample_rate_hz());
    g_file.printf("# start=%s\n", g_start_stamp.c_str());
    g_file.print("# calib open,closed (0.1deg):");
    for (int i = 0; i < N; ++i) {
        const servos::Calib c = servos::get_calib(i);
        g_file.printf(" ch%d=%u,%u", i, c.open_cmd, c.closed_cmd);
    }
    g_file.print("\n");
    g_file.print("# values per finger: flexion %, 0=open 100=closed\n");
    g_file.printf("t_ms,%s\n", kFingerNames);

    g_active      = true;
    g_frames      = 0;
    g_last_sample = g_session_start - cfg::LOG_SAMPLE_PERIOD_MS;  // sample at once
    g_last_flush  = g_session_start;
    Serial.printf("[logging] session %u started -> /logs/%s\n",
                  (unsigned)g_session_id, g_session_fname.c_str());
}

// Writes one sample row. Returns false on a short write (card pulled / full).
bool sample_row() {
    char line[80];
    const int len = snprintf(
        line, sizeof(line), "%lu,%u,%u,%u,%u,%u\n",
        (unsigned long)(millis() - g_session_start),
        servos::flexion_pct(0), servos::flexion_pct(1), servos::flexion_pct(2),
        servos::flexion_pct(3), servos::flexion_pct(4));
    if (len <= 0) return true;
    if (g_file.write((const uint8_t*)line, (size_t)len) != (size_t)len) return false;
    ++g_frames;
    return true;
}

// Appends the manifest row for the just-finished session, writing the header
// first if index.csv is new.
void append_index(uint32_t duration_ms) {
    File idx = sdcard::open_log(kIndexName, /*append=*/true);
    if (!idx) {
        Serial.println("[logging] could not open index.csv");
        return;
    }
    if (idx.size() == 0) {
        idx.print("session,filename,start,duration_ms,frames,rate_hz,powered\n");
    }
    idx.printf("%u,%s,%s,%lu,%lu,%u,%u\n",
               (unsigned)g_session_id, g_session_fname.c_str(), g_start_stamp.c_str(),
               (unsigned long)duration_ms, (unsigned long)g_frames,
               (unsigned)sample_rate_hz(), g_session_powered ? 1 : 0);
    idx.flush();
    idx.close();
}

void close_session(bool clean) {
    if (!g_active) return;
    const uint32_t dur = millis() - g_session_start;
    g_file.flush();
    g_file.close();
    g_active = false;

    append_index(dur);
    ++g_seq;
    save_seq();
    Serial.printf("[logging] session %u closed: %lu frames, %lu ms%s\n",
                  (unsigned)g_session_id, (unsigned long)g_frames,
                  (unsigned long)dur, clean ? "" : " (aborted)");
}

}  // namespace

void init() {
    load_state();
    g_frame_pending    = false;
    g_last_activity_ms = millis();
    Serial.printf("[logging] init: %s, next session id %u\n",
                  g_enabled ? "enabled" : "disabled", (unsigned)g_seq);
}

void note_exercise_frame() {
    g_last_activity_ms = millis();
    g_frame_pending    = true;
}

void tick() {
    // Snapshot and clear the activity flag set by the BLE task.
    const bool pending = g_frame_pending;
    if (pending) g_frame_pending = false;

    if (!g_enabled) {
        if (g_active) close_session(true);   // honour a mid-session disable
        return;
    }

    const uint32_t now = millis();

    if (!g_active) {
        // Open only on a real exercise frame, with the rail powered and not
        // while calibrating — manual/calibration motion is intentionally
        // excluded (those paths never call note_exercise_frame() anyway).
        if (pending && servos::is_power_on() && !servos::cal_mode()) {
            start_session();
        }
        return;
    }

    // End the session after an idle gap with no exercise frames.
    if ((uint32_t)(now - g_last_activity_ms) > cfg::LOG_IDLE_TIMEOUT_MS) {
        close_session(true);
        return;
    }

    if ((uint32_t)(now - g_last_sample) >= cfg::LOG_SAMPLE_PERIOD_MS) {
        g_last_sample = now;
        if (!sample_row()) {
            Serial.println("[logging] write failed (card removed/full?); aborting session");
            close_session(false);
            return;
        }
    }

    if ((uint32_t)(now - g_last_flush) >= cfg::LOG_FLUSH_PERIOD_MS) {
        g_last_flush = now;
        g_file.flush();
    }
}

void set_enabled(bool on) {
    g_enabled = on;
    Preferences prefs;
    if (prefs.begin(NVS_NS, /*readOnly=*/false)) {
        prefs.putBool(NVS_EN, on);
        prefs.end();
    }
    Serial.printf("[logging] %s\n", on ? "enabled" : "disabled");
}

bool is_enabled() { return g_enabled; }

void set_wall_clock(uint64_t epoch_ms) {
    g_epoch_at_ref = epoch_ms;
    g_epoch_ref_ms = millis();
    g_have_clock   = true;
    Serial.printf("[logging] wall clock set: %s\n", stamp_now().c_str());
}

bool     is_active()       { return g_active; }
uint16_t current_session() { return g_session_id; }
uint32_t frames_written()  { return g_frames; }

}  // namespace logging
