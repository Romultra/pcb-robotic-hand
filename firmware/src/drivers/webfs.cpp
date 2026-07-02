#include "webfs.h"

#include <WebServer.h>
#include <WiFi.h>

#include "ble.h"
#include "config.h"
#include "drivers/sdcard.h"

namespace webfs {

namespace {

WebServer g_server(80);
bool      g_started = false;

// Per-upload state, carried across the WebServer upload callback invocations
// (START → WRITE* → END) and read by the completion handler.
bool     g_up_ok    = false;   // false once any stage of the current upload fails
uint32_t g_up_bytes = 0;       // bytes committed by the last completed upload
uint8_t  g_up_fail  = 0;       // sdcard::UploadStatus of the failing stage (0 = none)

// Human-readable name for an sdcard::UploadStatus failure code, so the HTTP 500
// body tells the webapp *why* an upload failed instead of a generic error.
const char* upload_fail_name(uint8_t s) {
    switch ((sdcard::UploadStatus)s) {
        case sdcard::UploadStatus::NoCard:      return "no card";
        case sdcard::UploadStatus::MountFail:   return "mount failure";
        case sdcard::UploadStatus::OpenFail:    return "open failure";
        case sdcard::UploadStatus::WriteFail:   return "write failure";
        case sdcard::UploadStatus::ProtocolErr: return "protocol error";
        case sdcard::UploadStatus::Aborted:     return "aborted";
        default:                                return "upload failed";
    }
}

void add_cors() {
    // The upload originates from the HTTPS webapp (a different origin), so the
    // response must carry CORS headers or the browser hides it from the JS.
    g_server.sendHeader("Access-Control-Allow-Origin", "*");
}

// CORS preflight. A multipart/form-data POST is a "simple" request and usually
// skips this, but handle it anyway so a future header addition can't break us.
void handle_options() {
    add_cors();
    g_server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    g_server.sendHeader("Access-Control-Allow-Headers", "*");
    g_server.send(204, "text/plain", "");
}

// Reachability probe — lets the webapp confirm the board is up before sending.
void handle_ping() {
    add_cors();
    g_server.send(200, "text/plain", cfg::FW_NAME);
}

// Low-latency echo for the Wi-Fi round-trip latency test. Does no real work,
// just bounces the sequence number straight back so the webapp can time the HTTP
// round trip over the Wi-Fi link (the BLE equivalent is the PING echo). Motor
// commands always go over BLE; this is purely a measurement endpoint.
// GET /lat?seq=N -> 200 "N". Cache-busted so the browser never serves a stale reply.
void handle_lat() {
    add_cors();
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(200, "text/plain",
                  g_server.hasArg("seq") ? g_server.arg("seq") : String("0"));
}

// Response sent after the body (and the upload callback below) has been fully
// consumed. g_up_ok reflects whether every stage succeeded.
void handle_upload_done() {
    add_cors();
    // The transfer is over either way, so drop BLE back to the tight control
    // interval (no-op if it was never relaxed). This is the reliable restore
    // point: it runs once the request body has been fully consumed.
    ble::set_bulk_transfer(false);
    if (g_up_ok) {
        char body[48];
        snprintf(body, sizeof(body), "{\"received\":%u}", (unsigned)g_up_bytes);
        g_server.send(200, "application/json", body);
    } else {
        char body[64];
        snprintf(body, sizeof(body), "{\"error\":\"%s\"}", upload_fail_name(g_up_fail));
        g_server.send(500, "application/json", body);
    }
}

// Streaming upload callback. WebServer hands us the multipart file body in
// HTTP_UPLOAD_BUFLEN-sized pieces; we forward each straight to the SD driver so
// nothing large is ever buffered in RAM (the S3 has no PSRAM).
void handle_upload_data() {
    HTTPUpload& up = g_server.upload();

    switch (up.status) {
        case UPLOAD_FILE_START: {
            // Hand the radio to Wi-Fi for the duration: relax the BLE link so its
            // tight control interval stops stealing airtime from this transfer.
            // Restored in handle_upload_done / on abort. (BLE carries no traffic
            // mid-upload, so this costs nothing on the control side.)
            ble::set_bulk_transfer(true);
            g_up_fail = 0;
            // Destination path. Prefer the ?path= query arg, but whether the URL
            // query is parsed during a multipart upload callback varies across
            // arduino-esp32 versions, so the webapp also sends the path as the
            // multipart filename (always available here) as a robust fallback.
            // The SD driver normalises the leading slash.
            String path = g_server.hasArg("path") ? g_server.arg("path")
                                                   : up.filename;
            // total_size is unknown at START (only known at END), and the SD
            // driver uses it for logging only, so pass 0.
            const sdcard::UploadProgress p = sdcard::upload_begin(path, 0);
            g_up_ok    = (p.status == sdcard::UploadStatus::Ready);
            g_up_bytes = 0;
            if (!g_up_ok) g_up_fail = (uint8_t)p.status;
            break;
        }
        case UPLOAD_FILE_WRITE:
            if (g_up_ok) {
                const sdcard::UploadProgress p =
                    sdcard::upload_chunk(up.buf, up.currentSize);
                if (p.status != sdcard::UploadStatus::ChunkOk) {
                    g_up_ok   = false;
                    g_up_fail = (uint8_t)p.status;
                    // Don't sit and drain a multi-MB body we can no longer store:
                    // drop the link + connection so the client fails fast instead
                    // of waiting out the whole transfer before the 500 is even
                    // sent. The webapp surfaces this as a network error.
                    ble::set_bulk_transfer(false);
                    g_server.client().stop();
                }
            }
            break;
        case UPLOAD_FILE_END:
            if (g_up_ok) {
                const sdcard::UploadProgress p = sdcard::upload_end();
                g_up_ok    = (p.status == sdcard::UploadStatus::Complete);
                g_up_bytes = p.bytes_received;
                if (!g_up_ok) g_up_fail = (uint8_t)p.status;
            }
            break;
        case UPLOAD_FILE_ABORTED:
            sdcard::upload_abort();
            g_up_ok = false;
            if (g_up_fail == 0) g_up_fail = (uint8_t)sdcard::UploadStatus::Aborted;
            // The client vanished mid-transfer; restore the tight interval now
            // since handle_upload_done may not run for an aborted request.
            ble::set_bulk_transfer(false);
            break;
    }
}

}  // namespace

void begin() {
    if (g_started) return;

    g_server.on("/upload", HTTP_OPTIONS, handle_options);
    g_server.on("/upload", HTTP_POST, handle_upload_done, handle_upload_data);
    g_server.on("/ping", HTTP_GET, handle_ping);
    g_server.on("/lat", HTTP_GET, handle_lat);
    g_server.onNotFound([] {
        add_cors();
        g_server.send(404, "text/plain", "not found");
    });

    // NOTE: this bundled WebServer closes the TCP connection after every response
    // (its HTTP keep-alive path is disabled upstream for a Chrome bug), so each
    // /lat request pays a fresh TCP handshake — folded into the measured Wi-Fi
    // round-trip, which the latency panel reports as-is.
    g_server.begin();
    g_started = true;
    Serial.printf("[webfs] HTTP file server up at http://%s/\n",
                  WiFi.localIP().toString().c_str());
}

void loop() {
    if (g_started) g_server.handleClient();
}

bool is_running() {
    return g_started;
}

}  // namespace webfs
