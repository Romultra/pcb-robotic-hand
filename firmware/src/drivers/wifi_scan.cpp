#include "wifi_scan.h"

#include <Preferences.h>
#include <WiFi.h>
#include <algorithm>

#include "config.h"

namespace wifi_scan {

namespace {
// NVS store for the last successfully-joined network. Separate namespace from
// the servo calibration / audio volume stores.
constexpr const char* NVS_NS       = "wificreds";
constexpr const char* NVS_KEY_SSID = "ssid";
constexpr const char* NVS_KEY_PW   = "pw";
}  // namespace

void init() {
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi] driver init; radio idle");
}

std::vector<ScanEntry> scan() {
    std::vector<ScanEntry> out;

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);

    const int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
    if (n < 0) {
        Serial.printf("[wifi] scan error: %d\n", n);
        return out;
    }

    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        ScanEntry e;
        e.rssi = WiFi.RSSI(i);
        e.ssid = WiFi.SSID(i);
        out.push_back(e);
    }
    WiFi.scanDelete();

    std::sort(out.begin(), out.end(),
              [](const ScanEntry& a, const ScanEntry& b) { return a.rssi > b.rssi; });

    // Leave the radio in idle STA mode rather than WIFI_OFF: fully
    // de-initializing WiFi (esp_wifi_deinit) while the BLE controller is
    // running trips the WiFi/BT coexistence teardown timeout
    // ("timeout when WiFi un-init, type=4"). An unconnected, non-scanning STA
    // uses negligible air time, so BLE is unaffected.
    Serial.printf("[wifi] scan complete: %d networks\n", (int)out.size());
    return out;
}

ConnectResult connect_sta(const String& ssid, const String& password) {
    ConnectResult res = { 2, {0, 0, 0, 0} };

    if (ssid.length() == 0) {
        res.status = 1;
        return res;
    }

    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    WiFi.begin(ssid.c_str(), password.length() ? password.c_str() : nullptr);

    const uint32_t deadline = millis() + 10000;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
        const IPAddress ip = WiFi.localIP();
        res.status = 0;
        res.ip[0] = ip[0];
        res.ip[1] = ip[1];
        res.ip[2] = ip[2];
        res.ip[3] = ip[3];
        save_credentials(ssid, password);   // remember for "Reconnect saved"
        Serial.printf("[wifi] connected, IP %u.%u.%u.%u\n",
                      res.ip[0], res.ip[1], res.ip[2], res.ip[3]);
    } else {
        res.status = 1;
        // Leave radio in idle STA (no WIFI_OFF) — see note in scan().
        Serial.printf("[wifi] connect to '%s' failed/timeout\n", ssid.c_str());
    }
    return res;
}

void disconnect_sta() {
    // Disassociate but keep the driver in idle STA mode (wifioff=false) — fully
    // turning Wi-Fi off while BLE runs trips the coexistence teardown timeout
    // (see scan()). eraseap=true clears the stored credentials so it doesn't
    // silently auto-reconnect.
    WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/true);
    Serial.println("[wifi] disconnected (idle STA)");
}

bool is_connected() {
    return WiFi.status() == WL_CONNECTED;
}

void save_credentials(const String& ssid, const String& password) {
    if (ssid.length() == 0) return;
    Preferences prefs;
    if (!prefs.begin(NVS_NS, /*readOnly=*/false)) {
        Serial.println("[wifi] save creds: NVS open failed");
        return;
    }
    // Skip a redundant rewrite (e.g. reconnecting to the already-saved network).
    if (prefs.getString(NVS_KEY_SSID, "") != ssid ||
        prefs.getString(NVS_KEY_PW, "")   != password) {
        prefs.putString(NVS_KEY_SSID, ssid);
        prefs.putString(NVS_KEY_PW, password);
        Serial.printf("[wifi] credentials for '%s' saved to NVS\n", ssid.c_str());
    }
    prefs.end();
}

bool load_credentials(String& ssid, String& password) {
    Preferences prefs;
    // Open read-write so the namespace is created on first boot and nvs_open
    // doesn't log NOT_FOUND before anything has been saved (same as servocal).
    if (!prefs.begin(NVS_NS, /*readOnly=*/false)) return false;
    ssid     = prefs.getString(NVS_KEY_SSID, "");
    password = prefs.getString(NVS_KEY_PW, "");
    prefs.end();
    return ssid.length() > 0;
}

}  // namespace wifi_scan
