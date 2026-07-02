# 010: Persist Wi-Fi credentials in NVS with manual reconnect

**Date:** 2026-06-24
**Status:** Decided

## Context

Wi-Fi credentials were RAM-only: the webapp sent SSID and password over BLE
(`WIFI_CONNECT`) on every connect, and a reboot or the explicit Disconnect lost
them. The webapp form is also empty after a page reload. The annoyance is
re-typing the password to get the Wi-Fi data path (SD upload, latency test)
back. The board should remember the last network instead.

The constraint is the low-latency control design (see
`journal/decisions/008-low-latency-control-path.md`): BLE is the control path and
is kept coexistence-free, Wi-Fi is on-demand only, and `disconnect_sta()`
deliberately leaves the radio in idle STA without auto-reconnecting. Anything
that brings Wi-Fi up on its own works against that.

## Options Considered

1. **Board NVS, manual reconnect** — the board saves SSID and password to NVS on
   a successful connect (same `Preferences` mechanism as the servo calibration
   and audio volume). Wi-Fi stays off until asked: a "Reconnect saved" button in
   the webapp triggers a new connect-to-saved command, so BLE keeps its
   low-latency link by default. Secret lives on the board, not the browser.
2. **Board NVS, auto-reconnect on boot** — convenient for headless use, but Wi-Fi
   is then always on from startup, adding BLE latency and jitter. Directly
   against decision 008.
3. **Browser localStorage pre-fill** — no firmware change; the webapp stores the
   credentials and pre-fills the form. Matches the webapp's other localStorage
   state (tracking calibration, clips), but puts the password in the browser and
   keeps the board stateless.

## Decision

Option 1. The board persists credentials and never auto-connects on boot, so the
coexistence-free BLE default from decision 008 is preserved.

Implementation:
- `wifi_scan::save_credentials/load_credentials` use `Preferences` (namespace
  `wificreds`, keys `ssid`/`pw`). `connect_sta` calls `save_credentials` on a
  successful join; a redundant rewrite (reconnecting to the same network) is
  skipped.
- The `WIFI_CONNECT` characteristic gets a third single-byte command, using the
  first byte that doubles as `ssid_len` (only ever 1..32): `0x00` = disconnect
  (existing), `0xFF` = connect using saved credentials (new). The work is
  deferred to the loop task like the other Wi-Fi operations.
- `service_wifi_connect_saved` loads the stored credentials and reuses
  `service_wifi_connect`; it replies status 1 (the bad-credential / failure code
  the webapp already handles) when nothing is saved.
- The webapp adds a "Reconnect saved" button that writes `0xFF`.

Credentials are kept on an explicit Disconnect (Disconnect only frees the radio
for BLE; it is not "forget"). They are overwritten on the next successful
connect. No "forget" control was added; that can come later if needed.

## Sources

- `journal/decisions/008-low-latency-control-path.md` (coexistence / on-demand Wi-Fi).
- `firmware/src/drivers/wifi_scan.cpp` (save/load, save-on-connect).
- `firmware/src/ble.cpp` (`CbWifiConnect`, `service_wifi_connect_saved`).
- `firmware/src/drivers/servos.cpp` (the `Preferences` NVS pattern followed here).
