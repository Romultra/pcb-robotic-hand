#include "servos.h"

#include <Preferences.h>

#include "config.h"

namespace servos {

namespace {

constexpr int N = cfg::SERVO_COUNT;

// Direct LEDC PWM: one dedicated channel per servo (channels 0..N-1). The S3
// LEDC timer maxes at 14 bits; channels pair onto timers (ch/2), all at 50 Hz,
// so they share timers cleanly while keeping independent duty. We assign the
// channels explicitly instead of letting ESP32Servo auto-allocate them — on the
// S3 that library aliased several pins onto the same channel (one slider drove
// channels 0/2/4 together, another drove 1/3). The buzzer uses channel 7, clear
// of these.
constexpr uint32_t LEDC_FREQ_HZ  = 50;
constexpr uint8_t  LEDC_RES_BITS = 14;
constexpr uint32_t LEDC_MAX      = 1u << LEDC_RES_BITS;        // 16384 levels
constexpr uint32_t FRAME_US      = 1000000UL / LEDC_FREQ_HZ;  // 20000 µs
constexpr uint32_t MIN_PULSE_US  = 500;
constexpr uint32_t MAX_PULSE_US  = 2500;

bool     g_power_on = false;
bool     g_cal_mode = false;

Calib    g_cal[N];
uint16_t g_cur_x10[N];      // last command actually written (tenths of a degree)
uint16_t g_target_x10[N];   // ramp goal (tenths of a degree)
uint32_t g_last_ramp_ms = 0;

// NVS layout: one version byte followed by the packed Calib array.
constexpr uint8_t CAL_VERSION = 1;
constexpr const char* NVS_NS  = "servocal";
constexpr const char* NVS_KEY = "cal";

// Clamp a request for the current mode: absolute 0..1800 always, plus the
// calibrated band when in normal mode on a calibrated channel.
uint16_t clamp_for_mode(uint8_t ch, int32_t x10) {
    if (x10 < 0)    x10 = 0;
    if (x10 > 1800) x10 = 1800;
    if (!g_cal_mode && g_cal[ch].valid) {
        uint16_t lo = g_cal[ch].open_cmd < g_cal[ch].closed_cmd
                          ? g_cal[ch].open_cmd : g_cal[ch].closed_cmd;
        uint16_t hi = g_cal[ch].open_cmd < g_cal[ch].closed_cmd
                          ? g_cal[ch].closed_cmd : g_cal[ch].open_cmd;
        if (x10 < lo) x10 = lo;
        if (x10 > hi) x10 = hi;
    }
    return (uint16_t)x10;
}

// Convert a command (tenths of a degree, 0..1800) to a servo pulse and write it
// to the channel's LEDC duty. Keeps full resolution (no whole-degree rounding).
void write_now(uint8_t ch, uint16_t x10) {
    if (x10 > 1800) x10 = 1800;
    const uint32_t us   = MIN_PULSE_US + (uint32_t)((uint64_t)x10 * (MAX_PULSE_US - MIN_PULSE_US) / 1800);
    const uint32_t duty = (uint32_t)((uint64_t)us * LEDC_MAX / FRAME_US);
    ledcWrite(ch, duty);
    g_cur_x10[ch] = x10;
}

void load_calib() {
    for (int i = 0; i < N; ++i) {
        g_cal[i] = { 0, 1800, 900, 0 };   // uncalibrated: full range, no home
    }
    Preferences prefs;
    // Open read-write: this creates the namespace on first boot so nvs_open
    // doesn't log a NOT_FOUND error before anything has been saved. We don't
    // write here; getBytesLength just returns 0 when no calibration exists yet.
    if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return;
    const size_t want = 1 + N * sizeof(Calib);
    if (prefs.getBytesLength(NVS_KEY) == want) {
        uint8_t buf[1 + N * sizeof(Calib)];
        prefs.getBytes(NVS_KEY, buf, want);
        if (buf[0] == CAL_VERSION) {
            memcpy(g_cal, &buf[1], N * sizeof(Calib));
        }
    }
    prefs.end();
}

}  // namespace

void init() {
    // SERVO_EN must default low so the rail stays off until firmware asks for it.
    // Because the rail is off at boot, nothing the servos are commanded to here
    // can actually move them — the danger moment is set_power(true), handled there.
    pinMode(cfg::SERVO_EN_PIN, OUTPUT);
    digitalWrite(cfg::SERVO_EN_PIN, LOW);
    g_power_on = false;

    // One dedicated LEDC channel per servo (ch i -> SERVO_PINS[i]), 50 Hz, 14-bit.
    // No initial ledcWrite, so the duty stays 0 and nothing pulses on boot — with
    // tendon strings attached an unbidden move on every reset could over-tension a
    // finger, and the rail is off anyway until the user powers it.
    for (int i = 0; i < N; ++i) {
        ledcSetup(i, LEDC_FREQ_HZ, LEDC_RES_BITS);
        ledcAttachPin(cfg::SERVO_PINS[i], i);
        g_cur_x10[i]    = 900;   // bookkeeping only; actual output is no-pulse until written
        g_target_x10[i] = 900;
    }

    load_calib();

    Serial.printf("[servos] %d channels (direct LEDC ch0-%d); rail OFF, no boot move\n", N, N - 1);
}

void set_power(bool enable) {
    if (enable) {
        // Park calibrated channels at home before energising so the rail comes
        // up toward the slack/home end rather than wherever the servo last sat.
        // Open-loop servos can't be ramped across a power-off→on edge, so this
        // controlled snap toward home is the safest available behaviour; the
        // explicit Home action ramps gently once the rail is already on.
        for (int i = 0; i < N; ++i) {
            if (g_cal[i].valid) {
                write_now(i, g_cal[i].home_cmd);
                g_target_x10[i] = g_cur_x10[i];
            }
        }
    }
    digitalWrite(cfg::SERVO_EN_PIN, enable ? HIGH : LOW);
    g_power_on = enable;
    Serial.printf("[servos] rail %s\n", enable ? "ON" : "OFF");
}

bool is_power_on() {
    return g_power_on;
}

void set_angle(uint8_t channel, uint16_t angle_deg_x10) {
    if (channel >= N) {
        Serial.printf("[servos] set_angle: invalid channel %u\n", channel);
        return;
    }
    write_now(channel, clamp_for_mode(channel, angle_deg_x10));
    g_target_x10[channel] = g_cur_x10[channel];   // immediate, cancel any ramp
}

void set_finger(uint8_t channel, uint8_t norm_0_180) {
    if (channel >= N) return;
    if (norm_0_180 > 180) norm_0_180 = 180;

    uint16_t x10;
    if (g_cal[channel].valid && !g_cal_mode) {
        const int32_t o = g_cal[channel].open_cmd;
        const int32_t c = g_cal[channel].closed_cmd;
        x10 = (uint16_t)(o + (int32_t)norm_0_180 * (c - o) / 180);
    } else {
        x10 = (uint16_t)norm_0_180 * 10;   // fallback: straight 0..180°
    }
    write_now(channel, clamp_for_mode(channel, x10));
    g_target_x10[channel] = g_cur_x10[channel];
}

// --- Calibration / measurement ---------------------------------------------

void set_cal_mode(bool on) {
    g_cal_mode = on;
    Serial.printf("[servos] calibration mode %s\n", on ? "ON" : "OFF");
}

bool cal_mode() {
    return g_cal_mode;
}

uint16_t jog(uint8_t channel, int16_t delta_x10) {
    if (channel >= N) return 0;
    int16_t d = delta_x10;
    const int16_t cap = (int16_t)cfg::SERVO_JOG_MAX_X10;
    if (d >  cap) d =  cap;
    if (d < -cap) d = -cap;
    write_now(channel, clamp_for_mode(channel, (int32_t)g_cur_x10[channel] + d));
    g_target_x10[channel] = g_cur_x10[channel];
    Serial.printf("[servos] ch%u jog %+d -> %u.%u deg%s\n", channel, d,
                  g_cur_x10[channel] / 10, g_cur_x10[channel] % 10,
                  g_power_on ? "" : " (rail OFF, no motion)");
    return g_cur_x10[channel];
}

uint16_t goto_target(uint8_t channel, uint16_t angle_deg_x10) {
    if (channel >= N) return 0;
    g_target_x10[channel] = clamp_for_mode(channel, angle_deg_x10);
    Serial.printf("[servos] ch%u goto %u.%u deg\n", channel,
                  g_target_x10[channel] / 10, g_target_x10[channel] % 10);
    return g_target_x10[channel];   // tick() walks the servo there
}

void capture(uint8_t channel, uint8_t which) {
    if (channel >= N) return;
    const uint16_t cur = g_cur_x10[channel];
    switch (which) {
        // Capturing OPEN also sets the park position to the same spot, so
        // "move to open" has somewhere to go without a separate capture step.
        case 0: g_cal[channel].open_cmd = cur; g_cal[channel].home_cmd = cur; g_cal[channel].valid = 1; break;
        case 1: g_cal[channel].closed_cmd = cur; g_cal[channel].valid = 1; break;
        case 2: g_cal[channel].home_cmd   = cur;                           break;
        default: return;
    }
    Serial.printf("[servos] ch%u capture %u = %u.%u deg\n",
                  channel, which, cur / 10, cur % 10);
}

void home_channel(uint8_t channel) {
    if (channel >= N) return;
    if (!g_cal[channel].valid) {
        Serial.printf("[servos] ch%u move-to-open skipped (not calibrated)\n", channel);
        return;
    }
    g_target_x10[channel] = clamp_for_mode(channel, g_cal[channel].home_cmd);
    Serial.printf("[servos] ch%u move to open %u.%u deg\n", channel,
                  g_target_x10[channel] / 10, g_target_x10[channel] % 10);
}

void home_all() {
    for (int i = 0; i < N; ++i) home_channel((uint8_t)i);
}

uint16_t current(uint8_t channel) {
    return channel < N ? g_cur_x10[channel] : 0;
}

Calib get_calib(uint8_t channel) {
    return channel < N ? g_cal[channel] : Calib{ 0, 0, 0, 0 };
}

uint8_t flexion_pct(uint8_t channel) {
    if (channel >= N) return 0;
    const Calib& c = g_cal[channel];
    const int32_t cur = g_cur_x10[channel];
    // open end maps to 0 %, closed end to 100 %. Use the raw open/closed pair
    // (not sorted) so a finger whose linkage runs the other way still reads
    // 0 = open. An uncalibrated channel falls back to the full command range.
    const int32_t lo = c.valid ? c.open_cmd   : 0;
    const int32_t hi = c.valid ? c.closed_cmd : 1800;
    if (hi == lo) return 0;
    int32_t pct = (cur - lo) * 100 / (hi - lo);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return (uint8_t)pct;
}

void save_calib() {
    uint8_t buf[1 + N * sizeof(Calib)];
    buf[0] = CAL_VERSION;
    memcpy(&buf[1], g_cal, N * sizeof(Calib));
    Preferences prefs;
    if (!prefs.begin(NVS_NS, /*readOnly=*/false)) {
        Serial.println("[servos] save_calib: NVS open failed");
        return;
    }
    prefs.putBytes(NVS_KEY, buf, sizeof(buf));
    prefs.end();
    Serial.println("[servos] calibration saved to NVS");
}

void reset_calib() {
    for (int i = 0; i < N; ++i) g_cal[i] = { 0, 1800, 900, 0 };
    Preferences prefs;
    if (prefs.begin(NVS_NS, /*readOnly=*/false)) {
        prefs.remove(NVS_KEY);
        prefs.end();
    }
    Serial.println("[servos] calibration reset");
}

void tick() {
    const uint32_t now = millis();
    if (now - g_last_ramp_ms < cfg::SERVO_RAMP_PERIOD_MS) return;
    g_last_ramp_ms = now;

    const int32_t step = (int32_t)cfg::SERVO_RAMP_STEP_X10;
    for (int i = 0; i < N; ++i) {
        const int32_t cur = g_cur_x10[i];
        const int32_t tgt = g_target_x10[i];
        if (cur == tgt) continue;
        int32_t next;
        if (tgt > cur) next = (tgt - cur <= step) ? tgt : cur + step;
        else           next = (cur - tgt <= step) ? tgt : cur - step;
        write_now((uint8_t)i, clamp_for_mode((uint8_t)i, next));
    }
}

}  // namespace servos
