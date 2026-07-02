#pragma once

#include <Arduino.h>
#include <vector>

namespace wifi_scan {

struct ScanEntry {
    int8_t  rssi;
    String  ssid;
};

struct ConnectResult {
    uint8_t  status;   // 0 = success, 1 = invalid creds / timeout, 2 = stack error
    uint8_t  ip[4];    // valid only when status == 0
};

void init();

// Run a blocking Wi-Fi scan (~2 s). Returns visible APs sorted by RSSI.
// Internally toggles Wi-Fi into STA mode for the scan and then back off, so
// BLE keeps its full radio share when no Wi-Fi feature is active.
std::vector<ScanEntry> scan();

// Attempt to join the given network as a STA. Blocks up to ~10 s.
// On success the connection stays up so subsequent IP-level features can use it.
ConnectResult connect_sta(const String& ssid, const String& password);

// Disconnect from the current network. Leaves the radio in idle STA (not
// WIFI_OFF, see the note in scan()) and clears the stored credentials so it
// won't auto-reconnect. Used to drop Wi-Fi after testing so the BLE link runs
// without coexistence again.
void disconnect_sta();

// True if currently connected as STA.
bool is_connected();

// Persist / restore the last successfully-joined network in NVS so the webapp's
// "Reconnect saved" button can rejoin without re-entering credentials.
// save_credentials() is called automatically on a successful connect_sta();
// load_credentials() returns false when nothing is stored. The board never
// auto-connects on boot — Wi-Fi stays on-demand (see journal/decisions/008 and
// 010), so these are only used for an explicit reconnect.
void save_credentials(const String& ssid, const String& password);
bool load_credentials(String& ssid, String& password);

}  // namespace wifi_scan
