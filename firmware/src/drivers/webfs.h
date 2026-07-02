#pragma once

// HTTP file server for fast SD uploads over Wi-Fi.
//
// BLE stays the control channel; this is purely a bulk-data path. Once the
// board has joined a network as a STA (see wifi_scan::connect_sta), the webapp
// learns the board IP over BLE and POSTs files straight here over HTTP, which
// is far faster than the chunk-acked BLE upload. The browser is allowed to
// reach this plain-HTTP endpoint from the HTTPS app via Chrome's Local Network
// Access (LNA, Chrome >=142): a request to a private-IP literal is exempted
// from the mixed-content block once the one-time LNA permission is granted.
// See journal/decisions/002-wifi-file-transfer-lna.md.

namespace webfs {

// Start the HTTP server (idempotent). Safe to call repeatedly; only the first
// call binds the socket. Call once the STA link is up so it binds the STA IP.
void begin();

// Service pending HTTP clients. Call from the main loop. No-op until begin().
// NOTE: an in-flight upload runs to completion inside this call, so BLE status
// notifications pause for the transfer duration — acceptable for a validator.
void loop();

// True once begin() has started the server.
bool is_running();

}  // namespace webfs
