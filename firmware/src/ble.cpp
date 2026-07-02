#include "ble.h"

#include <NimBLEDevice.h>
#include <string.h>

#include "config.h"
#include "drivers/audio.h"
#include "drivers/buzzer.h"
#include "drivers/logging.h"
#include "drivers/sdcard.h"
#include "drivers/servos.h"
#include "drivers/webfs.h"
#include "drivers/wifi_scan.h"

namespace ble {

namespace {

NimBLEServer*         g_server       = nullptr;
NimBLECharacteristic* g_c_status     = nullptr;
NimBLECharacteristic* g_c_sd         = nullptr;
NimBLECharacteristic* g_c_sd_upload  = nullptr;
NimBLECharacteristic* g_c_wscan      = nullptr;
NimBLECharacteristic* g_c_wconn      = nullptr;
NimBLECharacteristic* g_c_servo_cal  = nullptr;
NimBLECharacteristic* g_c_audio_file = nullptr;
NimBLECharacteristic* g_c_log        = nullptr;
NimBLECharacteristic* g_c_ping       = nullptr;

bool     g_client_connected   = false;
uint32_t g_last_status_notify = 0;

// Handle of the (single) connected central, kept so set_bulk_transfer() can
// request a connection-parameter update outside the connect callback. 0xFFFF =
// BLE_HS_CONN_HANDLE_NONE = no connection.
uint16_t g_conn_handle = 0xFFFF;

// Bulk-transfer (Wi-Fi upload) coexistence state. While active, BLE runs on the
// relaxed interval so Wi-Fi gets the radio. g_bulk_deadline_ms is a safety
// backstop: if the upload never cleanly signals completion, ble::loop() restores
// the tight interval once the deadline passes so we never get stuck relaxed.
bool     g_bulk_active      = false;
uint32_t g_bulk_deadline_ms = 0;
constexpr uint32_t BULK_MODE_MAX_MS = 130000;  // > the webapp's 120 s XHR timeout

// Deferred audio control. Audio playback streams from the SD card in the main
// loop task (audio::tick), so all audio control is latched here by the GATT
// callbacks and serviced from ble::loop() — that way the audio File handle is
// only ever touched by the main loop task (same rationale as the WiFi defer).
enum class AudioReq : uint8_t { None, Preset, List, Play, Stop };
volatile AudioReq g_audio_req    = AudioReq::None;
uint8_t           g_audio_preset = 0;
String            g_audio_name;

// Deferred WiFi work. The WiFi scan/connect are multi-second blocking radio
// operations; running them inside a NimBLE GATT callback would stall the host
// task, collide with WiFi/BLE radio coexistence, and drop the notifications.
// The callbacks only latch a request here; service_wifi() runs it from the
// main loop task (see ble::loop).
volatile bool g_wifi_scan_req       = false;
volatile bool g_wifi_conn_req       = false;
volatile bool g_wifi_conn_saved_req = false;   // connect using NVS-saved creds
volatile bool g_wifi_disc_req       = false;
String        g_wifi_conn_ssid;
String        g_wifi_conn_pw;

// Deferred logging control. set_enabled / set_wall_clock touch NVS and a status
// query reads the SD free space, so the GATT callback only latches the request
// and ble::loop() services it on the main loop task (same defer pattern again).
enum class LogReq : uint8_t { None, Enable, SetTime, Status };
volatile LogReq g_log_req      = LogReq::None;
uint8_t         g_log_enable   = 0;
uint64_t        g_log_epoch_ms = 0;

// --- Helpers ---------------------------------------------------------------

// Read a fixed-size payload safely. Returns false if length mismatch.
template <size_t N>
bool read_payload(const NimBLEAttValue& v, uint8_t (&out)[N]) {
    if (v.length() != N) {
        Serial.printf("[ble] payload size mismatch: got %u, expected %u\n",
                      (unsigned)v.length(), (unsigned)N);
        return false;
    }
    memcpy(out, v.data(), N);
    return true;
}

uint16_t le_u16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void put_le_u16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

void put_le_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

// --- Status notification ---------------------------------------------------

void notify_status() {
    if (!g_c_status || !g_client_connected) return;
    uint8_t buf[12];
    buf[0] = 0;  // reserved flags
    buf[1] = servos::is_power_on() ? 1 : 0;
    buf[2] = sdcard::is_card_present() ? 1 : 0;
    buf[3] = wifi_scan::is_connected() ? 1 : 0;
    put_le_u32(&buf[4],  millis());
    put_le_u32(&buf[8],  (uint32_t)ESP.getFreeHeap());
    g_c_status->setValue(buf, sizeof(buf));
    g_c_status->notify();
}

// --- Per-characteristic write handlers -------------------------------------

class CbServoPower : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        uint8_t v[1];
        if (read_payload(c->getValue(), v)) servos::set_power(v[0] != 0);
    }
};

class CbServoSet : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        uint8_t v[3];
        if (read_payload(c->getValue(), v)) {
            servos::set_angle(v[0], le_u16(&v[1]));
        }
    }
};

class CbServoFrame : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        uint8_t v[cfg::SERVO_COUNT];  // normalized finger pos 0..180 per channel
        if (read_payload(c->getValue(), v)) {
            // set_finger remaps each normalized value into the channel's
            // calibrated [open,closed] band, so playback/live-mirror stay
            // inside every finger's safe travel.
            for (uint8_t i = 0; i < cfg::SERVO_COUNT; ++i) {
                servos::set_finger(i, v[i]);
            }
            // SERVO_FRAME is the exercise stream (live-mirror + playback). This
            // is the ONLY path that drives the exercise logger — manual sliders
            // (SERVO_SET) and calibration (SERVO_CAL) deliberately do not.
            logging::note_exercise_frame();
        }
    }
};

// Notify the current calibration of one channel (ch < SERVO_COUNT) or every
// channel (ch >= SERVO_COUNT, e.g. 0xFF). Per-channel frame (all LE):
//   op(1) ch(1) open_u16 closed_u16 home_u16 flags(1) cur_u16 reserved(1)
//   flags: bit0 = calibrated, bit1 = calibration mode active.
// Uses the explicit-buffer notify() so back-to-back frames each snapshot their
// own payload (same reason as service_wifi_scan()).
void notify_cal(uint8_t op, uint8_t ch) {
    if (!g_c_servo_cal || !g_client_connected) return;
    auto send_one = [&](uint8_t i) {
        const servos::Calib cal = servos::get_calib(i);
        uint8_t out[12];
        out[0] = op;
        out[1] = i;
        put_le_u16(&out[2], cal.open_cmd);
        put_le_u16(&out[4], cal.closed_cmd);
        put_le_u16(&out[6], cal.home_cmd);
        out[8] = (uint8_t)((cal.valid ? 0x01 : 0) | (servos::cal_mode() ? 0x02 : 0));
        put_le_u16(&out[9], servos::current(i));
        out[11] = 0;
        g_c_servo_cal->notify(out, sizeof(out));
    };
    if (ch < cfg::SERVO_COUNT) {
        send_one(ch);
    } else {
        for (uint8_t i = 0; i < cfg::SERVO_COUNT; ++i) send_one(i);
    }
}

class CbServoCal : public NimBLECharacteristicCallbacks {
    // Command protocol (all multi-byte fields little-endian):
    //   0x01 MODE     : op(1) on(1)              enter(1)/exit(0) calibration mode
    //   0x02 JOG      : op(1) ch(1) delta_i16(2) relative jog (firmware-capped)
    //   0x03 GOTO     : op(1) ch(1) target_u16(2) ramped move to absolute cmd
    //   0x04 CAPTURE  : op(1) ch(1) which(1)     which: 0 open, 1 closed, 2 home
    //   0x05 SAVE     : op(1)                    persist to NVS
    //   0x06 RESET    : op(1)                    clear calibration
    //   0x07 HOME     : op(1) ch(1)              ramp to home (ch 0xFF = all)
    //   0x08 READ     : op(1) ch(1)              read back (ch 0xFF = all)
    // Reply (notify): one per-channel calibration frame (see notify_cal).
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        const NimBLEAttValue v = c->getValue();
        if (v.length() < 1) return;
        const uint8_t* p = v.data();
        const size_t   n = v.length();
        const uint8_t  op = p[0];
        uint8_t reply_ch = 0xFF;   // default: refresh the whole table

        Serial.printf("[ble] servo_cal op=0x%02x (%u bytes)\n", op, (unsigned)n);

        switch (op) {
            case 0x01:  // MODE
                if (n >= 2) servos::set_cal_mode(p[1] != 0);
                break;
            case 0x02:  // JOG
                if (n >= 4) {
                    servos::jog(p[1], (int16_t)le_u16(&p[2]));
                    reply_ch = p[1];
                }
                break;
            case 0x03:  // GOTO
                if (n >= 4) {
                    servos::goto_target(p[1], le_u16(&p[2]));
                    reply_ch = p[1];
                }
                break;
            case 0x04:  // CAPTURE
                if (n >= 3) {
                    servos::capture(p[1], p[2]);
                    reply_ch = p[1];
                }
                break;
            case 0x05:  // SAVE
                servos::save_calib();
                break;
            case 0x06:  // RESET
                servos::reset_calib();
                break;
            case 0x07:  // HOME
                if (n >= 2) {
                    if (p[1] >= cfg::SERVO_COUNT) servos::home_all();
                    else                          servos::home_channel(p[1]);
                    reply_ch = p[1];
                }
                break;
            case 0x08:  // READ
                if (n >= 2) reply_ch = p[1];
                break;
            default:
                break;
        }
        notify_cal(op, reply_ch);
    }
};

class CbBuzzer : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        uint8_t v[4];
        if (read_payload(c->getValue(), v)) {
            buzzer::play(le_u16(&v[0]), le_u16(&v[2]));
        }
    }
};

class CbAudio : public NimBLECharacteristicCallbacks {
    // Latch only — play_preset touches the same state as file playback, so it
    // runs from the main loop task (see ble::loop / the AudioReq machinery).
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        uint8_t v[1];
        if (read_payload(c->getValue(), v)) {
            g_audio_preset = v[0];
            g_audio_req    = AudioReq::Preset;
        }
    }
};

class CbAudioVol : public NimBLECharacteristicCallbacks {
    // Master volume, one byte 0..100 (%). Just a float store in the driver, so
    // it's safe to apply directly here rather than deferring to the loop task.
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        uint8_t v[1];
        if (read_payload(c->getValue(), v)) audio::set_volume(v[0]);
    }
};

class CbAudioFile : public NimBLECharacteristicCallbacks {
    // Command protocol:
    //   0x01 LIST : op(1)
    //   0x02 PLAY : op(1) name_len(1) name(N)   bare filename in /audio
    //   0x03 STOP : op(1)
    // Replies are notifications tagged by a leading type byte (see
    // service_audio_list / service_audio_play). The SD/audio work is deferred
    // to ble::loop() so it runs on the main loop task.
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        const NimBLEAttValue v = c->getValue();
        if (v.length() < 1) return;
        const uint8_t* p = v.data();
        const size_t   n = v.length();
        switch (p[0]) {
            case 0x01:  // LIST
                g_audio_req = AudioReq::List;
                break;
            case 0x02: {  // PLAY
                if (n < 2) break;
                const uint8_t name_len = p[1];
                if ((size_t)2 + name_len > n) break;
                g_audio_name = String((const char*)&p[2], name_len);
                g_audio_req  = AudioReq::Play;
                break;
            }
            case 0x03:  // STOP
                g_audio_req = AudioReq::Stop;
                break;
            default:
                break;
        }
    }
};

class CbSdTest : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        const sdcard::TestResult r = sdcard::run_self_test();
        uint8_t out[9];
        out[0] = (uint8_t)r.result;
        put_le_u32(&out[1], r.size_mb);
        put_le_u32(&out[5], r.free_mb);
        if (g_c_sd) {
            g_c_sd->setValue(out, sizeof(out));
            g_c_sd->notify();
        }
    }
};

class CbSdUpload : public NimBLECharacteristicCallbacks {
    // Chunked upload protocol — all fields little-endian:
    //   0x01 BEGIN  : op(1) path_len(1) path(N) total_size(4)
    //   0x02 CHUNK  : op(1) data(N)
    //   0x03 END    : op(1)
    //   0x04 ABORT  : op(1)
    // Reply (notify): status(1) bytes_received(4)
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        const NimBLEAttValue v = c->getValue();
        if (v.length() < 1) return;
        const uint8_t* p = v.data();
        const size_t   n = v.length();

        sdcard::UploadProgress r = { sdcard::UploadStatus::ProtocolErr, 0 };
        switch (p[0]) {
            case 0x01: {  // BEGIN
                if (n < 6) break;
                const uint8_t path_len = p[1];
                if ((size_t)2 + path_len + 4 > n) break;
                String path((const char*)&p[2], path_len);
                const uint8_t* sz = &p[2 + path_len];
                const uint32_t total = (uint32_t)sz[0]
                                     | ((uint32_t)sz[1] << 8)
                                     | ((uint32_t)sz[2] << 16)
                                     | ((uint32_t)sz[3] << 24);
                r = sdcard::upload_begin(path, total);
                break;
            }
            case 0x02:    // CHUNK
                if (n < 2) break;
                r = sdcard::upload_chunk(&p[1], n - 1);
                break;
            case 0x03:    // END
                r = sdcard::upload_end();
                break;
            case 0x04:    // ABORT
                r = sdcard::upload_abort();
                break;
            default:
                break;
        }

        uint8_t out[5];
        out[0] = (uint8_t)r.status;
        put_le_u32(&out[1], r.bytes_received);
        if (g_c_sd_upload) {
            g_c_sd_upload->setValue(out, sizeof(out));
            g_c_sd_upload->notify();
        }
    }
};

class CbWifiScan : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        // Latch only — the blocking scan runs from the main loop task.
        g_wifi_scan_req = true;
    }
};

class CbWifiConnect : public NimBLECharacteristicCallbacks {
    // Connect payload: ssid_len(1) ssid(N) pw_len(1) pw(M). Two single-byte
    // commands are special, distinguished by the first byte (ssid_len is only
    // ever 1..32):
    //   0x00 = disconnect from the current network
    //   0xFF = connect using the credentials saved in NVS ("Reconnect saved")
    // All are deferred to the loop task (the connect blocks for seconds).
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        const NimBLEAttValue v = c->getValue();
        if (v.length() < 1) return;
        const uint8_t* p = v.data();
        if (p[0] == 0) {            // ssid_len 0 = disconnect request
            g_wifi_disc_req = true;
            return;
        }
        if (p[0] == 0xFF) {         // connect using saved credentials
            g_wifi_conn_saved_req = true;
            return;
        }
        if (v.length() < 2) return;
        const uint8_t ssid_len = p[0];
        if ((size_t)1 + ssid_len + 1 > v.length()) return;
        const uint8_t pw_len = p[1 + ssid_len];
        if ((size_t)1 + ssid_len + 1 + pw_len > v.length()) return;

        // Parse here (cheap), but defer the blocking connect to the loop task.
        g_wifi_conn_ssid = String((const char*)&p[1], ssid_len);
        g_wifi_conn_pw   = String((const char*)&p[2 + ssid_len], pw_len);
        g_wifi_conn_req  = true;
    }
};

class CbLogCtrl : public NimBLECharacteristicCallbacks {
    // Command protocol (multi-byte fields little-endian):
    //   0x01 ENABLE  : op(1) on(1)            enable(1)/disable(0) logging
    //   0x02 SET_TIME: op(1) epoch_ms(8)      wall clock for dated filenames
    //   0x03 STATUS  : op(1)                  request a status notify
    // Reply (notify): see notify_log_status(). All work is latched and run from
    // ble::loop() on the main loop task.
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        const NimBLEAttValue v = c->getValue();
        if (v.length() < 1) return;
        const uint8_t* p = v.data();
        const size_t   n = v.length();
        switch (p[0]) {
            case 0x01:  // ENABLE
                if (n >= 2) {
                    g_log_enable = p[1];
                    g_log_req    = LogReq::Enable;
                }
                break;
            case 0x02:  // SET_TIME
                if (n >= 9) {
                    uint64_t e = 0;
                    for (int i = 0; i < 8; ++i) e |= (uint64_t)p[1 + i] << (8 * i);
                    g_log_epoch_ms = e;
                    g_log_req      = LogReq::SetTime;
                }
                break;
            case 0x03:  // STATUS
                g_log_req = LogReq::Status;
                break;
            default:
                break;
        }
    }
};

class CbPing : public NimBLECharacteristicCallbacks {
    // Latency ping: echo the written payload straight back as a notification,
    // right here in the GATT callback with no deferral, so the webapp's measured
    // round-trip reflects pure BLE transport rather than our loop scheduling. The
    // payload is opaque (the app puts a sequence number in it). Explicit-buffer
    // notify() so the echo snapshots its own mbuf (same reason as service_wifi_scan).
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        const NimBLEAttValue v = c->getValue();
        if (g_c_ping && g_client_connected) {
            g_c_ping->notify(v.data(), v.length());
        }
    }
};

// --- Deferred WiFi service (runs on the main loop task) --------------------

void service_wifi_scan() {
    const auto entries = wifi_scan::scan();
    // Pack into chunks that fit the negotiated MTU (~244 bytes payload).
    // Format per entry: i8 rssi, u8 ssid_len, ssid bytes.
    constexpr size_t MAX_PAYLOAD = 240;
    uint8_t buf[MAX_PAYLOAD];
    size_t  used = 0;

    auto flush = [&] {
        if (!g_c_wscan || used == 0) { used = 0; return; }
        // Pass the payload directly to notify() so it snapshots into its own
        // mbuf immediately. The no-arg notify()/setValue() path defers the
        // send and re-reads the characteristic value at transmit time — with
        // back-to-back packets the value gets overwritten before it goes out,
        // so every notification ships the *last* value (the empty terminator).
        g_c_wscan->notify(buf, used);
        used = 0;
    };

    for (const auto& e : entries) {
        const size_t ssid_len = min<size_t>(e.ssid.length(), 32);
        const size_t need = 2 + ssid_len;
        if (used + need > MAX_PAYLOAD) {
            flush();
        }
        buf[used++] = (uint8_t)e.rssi;
        buf[used++] = (uint8_t)ssid_len;
        memcpy(&buf[used], e.ssid.c_str(), ssid_len);
        used += ssid_len;
    }
    flush();

    // Empty terminator notification so the app knows the list is complete.
    if (g_c_wscan) {
        uint8_t empty = 0;
        g_c_wscan->setValue(&empty, 0);
        g_c_wscan->notify();
    }
}

void service_wifi_connect() {
    const wifi_scan::ConnectResult r =
        wifi_scan::connect_sta(g_wifi_conn_ssid, g_wifi_conn_pw);
    // On a successful join, bring up the HTTP file server so the webapp can
    // push SD uploads over Wi-Fi (idempotent — only the first call binds).
    if (r.status == 0) webfs::begin();
    uint8_t out[5];
    out[0] = r.status;
    memcpy(&out[1], r.ip, 4);
    if (g_c_wconn) {
        g_c_wconn->setValue(out, sizeof(out));
        g_c_wconn->notify();
    }
}

// Drop the Wi-Fi link (e.g. after latency testing, to restore a coexistence-free
// BLE link). Replies on the same characteristic with status 3 = disconnected and
// a zero IP so the webapp can clear its stored board IP.
void service_wifi_disconnect() {
    wifi_scan::disconnect_sta();
    uint8_t out[5] = { 3, 0, 0, 0, 0 };
    if (g_c_wconn) {
        g_c_wconn->setValue(out, sizeof(out));
        g_c_wconn->notify();
    }
}

// Reconnect to the network whose credentials were saved in NVS on the last
// successful connect. Replies status 1 (same as a bad-credential failure) when
// nothing is stored; otherwise reuses service_wifi_connect().
void service_wifi_connect_saved() {
    String ssid, pw;
    if (!wifi_scan::load_credentials(ssid, pw)) {
        Serial.println("[wifi] reconnect: no saved credentials");
        uint8_t out[5] = { 1, 0, 0, 0, 0 };
        if (g_c_wconn) {
            g_c_wconn->setValue(out, sizeof(out));
            g_c_wconn->notify();
        }
        return;
    }
    g_wifi_conn_ssid = ssid;
    g_wifi_conn_pw   = pw;
    service_wifi_connect();
}

// --- Deferred audio service (runs on the main loop task) -------------------

// Notifies the dedicated-folder file list, chunked into MTU-sized frames.
// Each chunk starts with a 0x01 type byte, then repeated entries:
//   name_len(1) name(name_len) size_u32. A final 0x00 byte marks the end.
// Same explicit-buffer notify() reasoning as service_wifi_scan().
void service_audio_list() {
    if (!g_c_audio_file) return;
    const auto files = sdcard::list_audio();

    constexpr size_t MAX_PAYLOAD = 240;
    uint8_t buf[MAX_PAYLOAD];
    size_t  used = 1;
    buf[0] = 0x01;   // type: list chunk

    auto flush = [&] {
        if (used > 1) g_c_audio_file->notify(buf, used);
        used = 1;     // keep the type byte for the next chunk
    };

    for (const auto& fle : files) {
        // Clamp so a single entry always fits a fresh chunk (1 tag + len + name + 4).
        const size_t name_len = min<size_t>(fle.name.length(), 230);
        const size_t need = 1 + name_len + 4;
        if (used + need > MAX_PAYLOAD) flush();
        buf[used++] = (uint8_t)name_len;
        memcpy(&buf[used], fle.name.c_str(), name_len);
        used += name_len;
        put_le_u32(&buf[used], fle.size);
        used += 4;
    }
    flush();

    uint8_t term = 0x00;   // type: list complete
    g_c_audio_file->notify(&term, 1);
}

// Plays a file from the dedicated folder and notifies the result.
// Reply: type 0x02, result (0 = playing, 1 = open/format failure).
void service_audio_play() {
    const bool ok = audio::play_file(g_audio_name);
    if (g_c_audio_file) {
        uint8_t out[2] = { 0x02, (uint8_t)(ok ? 0 : 1) };
        g_c_audio_file->notify(out, sizeof(out));
    }
}

// --- Logging status notification -------------------------------------------
// Frame (all LE): type(1)=0x01 enabled(1) active(1) session_u16 frames_u32
// free_mb_u32. Sent on a STATUS request and whenever a session opens/closes.
void notify_log_status() {
    if (!g_c_log || !g_client_connected) return;
    uint8_t out[13];
    out[0] = 0x01;
    out[1] = logging::is_enabled() ? 1 : 0;
    out[2] = logging::is_active() ? 1 : 0;
    put_le_u16(&out[3], logging::current_session());
    put_le_u32(&out[5], logging::frames_written());
    put_le_u32(&out[9], sdcard::free_mb());
    g_c_log->notify(out, sizeof(out));
}

// --- Server connection state ----------------------------------------------

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        g_client_connected = true;
        g_conn_handle      = info.getConnHandle();
        g_bulk_active      = false;   // fresh link starts on the tight interval
        // The central (phone) chooses the connection interval at connect, and
        // Android's default is slow (tens of ms), which dominates the command
        // round-trip on the live-mirror path. Request a tight interval so the
        // mirror tracks responsively — this is the main firmware-side lever on
        // BLE latency. The central may clamp the request to its own minimum.
        s->updateConnParams(info.getConnHandle(),
                            cfg::BLE_CONN_ITVL_MIN, cfg::BLE_CONN_ITVL_MAX,
                            cfg::BLE_CONN_LATENCY, cfg::BLE_CONN_TIMEOUT);
        Serial.printf("[ble] client connected; requested %.1f-%.1f ms interval\n",
                      cfg::BLE_CONN_ITVL_MIN * 1.25f, cfg::BLE_CONN_ITVL_MAX * 1.25f);
    }
    void onDisconnect(NimBLEServer* s, NimBLEConnInfo&, int) override {
        g_client_connected = false;
        g_conn_handle      = 0xFFFF;
        g_bulk_active      = false;
        Serial.println("[ble] client disconnected; resuming advertise");
        NimBLEDevice::startAdvertising();
    }
};

}  // namespace

void init() {
    NimBLEDevice::init(cfg::FW_NAME);
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setPower(ESP_PWR_LVL_P3);

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks());

    NimBLEService* svc = g_server->createService(BLE_UUID_SERVICE);

    g_c_status = svc->createCharacteristic(
        BLE_UUID_STATUS,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    auto* c_servo_power = svc->createCharacteristic(
        BLE_UUID_SERVO_POWER, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    c_servo_power->setCallbacks(new CbServoPower());

    auto* c_servo_set = svc->createCharacteristic(
        BLE_UUID_SERVO_SET, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    c_servo_set->setCallbacks(new CbServoSet());

    auto* c_servo_frame = svc->createCharacteristic(
        BLE_UUID_SERVO_FRAME, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    c_servo_frame->setCallbacks(new CbServoFrame());

    g_c_servo_cal = svc->createCharacteristic(
        BLE_UUID_SERVO_CAL,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_c_servo_cal->setCallbacks(new CbServoCal());

    auto* c_buzzer = svc->createCharacteristic(
        BLE_UUID_BUZZER, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    c_buzzer->setCallbacks(new CbBuzzer());

    auto* c_audio = svc->createCharacteristic(
        BLE_UUID_AUDIO_PLAY, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    c_audio->setCallbacks(new CbAudio());

    auto* c_audio_vol = svc->createCharacteristic(
        BLE_UUID_AUDIO_VOL, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    c_audio_vol->setCallbacks(new CbAudioVol());

    g_c_audio_file = svc->createCharacteristic(
        BLE_UUID_AUDIO_FILE,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_c_audio_file->setCallbacks(new CbAudioFile());

    g_c_log = svc->createCharacteristic(
        BLE_UUID_LOG_CTRL,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_c_log->setCallbacks(new CbLogCtrl());

    g_c_sd = svc->createCharacteristic(
        BLE_UUID_SD_TEST, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    g_c_sd->setCallbacks(new CbSdTest());

    g_c_sd_upload = svc->createCharacteristic(
        BLE_UUID_SD_UPLOAD,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_c_sd_upload->setCallbacks(new CbSdUpload());

    g_c_wscan = svc->createCharacteristic(
        BLE_UUID_WIFI_SCAN, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    g_c_wscan->setCallbacks(new CbWifiScan());

    g_c_wconn = svc->createCharacteristic(
        BLE_UUID_WIFI_CONNECT, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
    g_c_wconn->setCallbacks(new CbWifiConnect());

    g_c_ping = svc->createCharacteristic(
        BLE_UUID_PING,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
    g_c_ping->setCallbacks(new CbPing());

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_UUID_SERVICE);
    adv->setName(cfg::FW_NAME);
    adv->enableScanResponse(true);
    NimBLEDevice::startAdvertising();

    Serial.printf("[ble] advertising as '%s'\n", cfg::FW_NAME);
}

void set_bulk_transfer(bool active) {
    // Arm/refresh the safety backstop whenever bulk mode is requested, before any
    // early-out, so ble::loop() always has a valid deadline while g_bulk_active.
    if (active) g_bulk_deadline_ms = millis() + BULK_MODE_MAX_MS;

    if (active == g_bulk_active) return;   // already in this mode
    g_bulk_active = active;

    // Only the radio request needs a live connection; the flag is tracked
    // regardless so the state stays consistent across (un)expected drops.
    if (!g_client_connected || g_conn_handle == 0xFFFF || !g_server) return;

    if (active) {
        g_server->updateConnParams(g_conn_handle,
                                   cfg::BLE_CONN_ITVL_MIN_BULK,
                                   cfg::BLE_CONN_ITVL_MAX_BULK,
                                   cfg::BLE_CONN_LATENCY_BULK,
                                   cfg::BLE_CONN_TIMEOUT);
        Serial.println("[ble] bulk mode on: relaxed conn interval for Wi-Fi upload");
    } else {
        g_server->updateConnParams(g_conn_handle,
                                   cfg::BLE_CONN_ITVL_MIN,
                                   cfg::BLE_CONN_ITVL_MAX,
                                   cfg::BLE_CONN_LATENCY,
                                   cfg::BLE_CONN_TIMEOUT);
        Serial.println("[ble] bulk mode off: restored tight conn interval");
    }
}

void loop() {
    // Backstop: if bulk (Wi-Fi upload) mode was left on because an upload never
    // signalled completion, restore the tight control interval past its deadline.
    if (g_bulk_active && (int32_t)(millis() - g_bulk_deadline_ms) >= 0) {
        set_bulk_transfer(false);
    }

    // Service deferred WiFi work on this (main loop) task so the NimBLE host
    // task is never blocked by the multi-second radio operations.
    if (g_wifi_scan_req) {
        g_wifi_scan_req = false;
        service_wifi_scan();
    }
    if (g_wifi_conn_req) {
        g_wifi_conn_req = false;
        service_wifi_connect();
    }
    if (g_wifi_conn_saved_req) {
        g_wifi_conn_saved_req = false;
        service_wifi_connect_saved();
    }
    if (g_wifi_disc_req) {
        g_wifi_disc_req = false;
        service_wifi_disconnect();
    }

    // Service any latched audio command. Clear the latch first so a command
    // arriving during the work is picked up on the next loop, not dropped.
    const AudioReq audio_req = g_audio_req;
    if (audio_req != AudioReq::None) {
        g_audio_req = AudioReq::None;
        switch (audio_req) {
            case AudioReq::Preset: audio::play_preset(g_audio_preset); break;
            case AudioReq::List:   service_audio_list(); break;
            case AudioReq::Play:   service_audio_play(); break;
            case AudioReq::Stop:
                audio::play_preset(0);   // 0 = silence / stop (also closes file)
                if (g_c_audio_file) {
                    uint8_t out[2] = { 0x02, 2 };   // type 0x02, result 2 = stopped
                    g_c_audio_file->notify(out, sizeof(out));
                }
                break;
            default: break;
        }
    }

    // Service any latched logging-control command on this task (NVS / SD work).
    const LogReq log_req = g_log_req;
    if (log_req != LogReq::None) {
        g_log_req = LogReq::None;
        switch (log_req) {
            case LogReq::Enable:  logging::set_enabled(g_log_enable != 0); break;
            case LogReq::SetTime: logging::set_wall_clock(g_log_epoch_ms);  break;
            default: break;   // Status just falls through to the notify below
        }
        notify_log_status();
    }

    // Push a logging status notify whenever a session opens or closes, so a
    // subscribed client sees session boundaries without polling.
    static bool s_last_log_active = false;
    const bool log_active = logging::is_active();
    if (log_active != s_last_log_active) {
        s_last_log_active = log_active;
        notify_log_status();
    }

    const uint32_t now = millis();
    if (g_client_connected && (now - g_last_status_notify) >= cfg::STATUS_NOTIFY_PERIOD_MS) {
        g_last_status_notify = now;
        notify_status();
    }
}

}  // namespace ble
