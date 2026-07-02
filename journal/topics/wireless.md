# Wireless Connectivity

**Implemented:** Wi-Fi (2.4 GHz 802.11 b/g/n) + Bluetooth 5 LE, both provided
by the ESP32-S3-WROOM-1-N8 module's onboard radio + PCB antenna. No external
RF components.

Both stacks are available simultaneously on the ESP32-S3; the choice between
BLE and Wi-Fi for the companion-device link is a firmware-level decision and
isn't constrained by the hardware.

Pragmatic split being assumed for firmware design:

- **BLE GATT** — primary control channel from the companion smartphone app
  (exercise commands, status). Low power, no router dependency, fine for the
  small command payloads.
- **Wi-Fi** — opportunistic / bulk channel for firmware updates and bulk
  download of logged movement data when the device is near a configured AP.

See `board-architecture.md` for how the ESP32 module sits on the board.

---

## Reference: ESP32-S3 BLE 5.0

Source: ESP32-S3 SoC datasheet, ESP32-S3-WROOM-1 datasheet (v1.7),
ESP-IDF v5.5.x docs.

### PHY modes
- **LE 1M PHY** — 1 Mbps (mandatory BLE 5.0)
- **LE 2M PHY** — 2 Mbps (higher throughput, shorter range)
- **LE Coded PHY (S=2)** — 500 kbps (~2× range vs LE 1M)
- **LE Coded PHY (S=8)** — 125 kbps (~4× range vs LE 1M)
- Advertising Extensions (up to 1650 bytes adv. data)
- Bluetooth Mesh supported

### Measured application-layer throughput (ESP-IDF `ble_throughput`, S3↔S3)
- LE 1M PHY: ~0.73 Mbit/s
- LE 2M PHY: ~1.35 Mbit/s
- Coded PHY not published by Espressif; expect lower due to coding overhead

### TX power
- Configurable via `esp_ble_tx_power_set()`
- 16 levels: -24, -21, -18, -15, -12, -9, -6, -3, 0, +3, +6, +9, +12, +15, +18, +20 dBm
- Default: +9 dBm (`ESP_PWR_LVL_P9`)
- Per-handle control (separate power for advertising, scanning, connections)

### Receiver sensitivity (WROOM-1 datasheet)
- LE 2M PHY: **-92 dBm** typical @ 30.8 % PER (confirmed)
- LE 1M / Coded PHY values are in datasheet tables 7-12 through 7-15 but
  couldn't be extracted programmatically. Expected ~ -95 to -97 dBm (1M),
  Coded PHY S=8 ~12 dB better than 1M per Bluetooth-5 spec.
  **Verify directly from the datasheet before citing in thesis.**

### Simultaneous connections
- Max BLE instances (adv + scan + conn): **10**
- Max connection slots: up to **9** (NimBLE) or **7** (Bluedroid)
- Default: **3**; recommended 3–5 for stability (~13 KB RAM per connection)
- This project: only **1** (device ↔ phone)

---

## Reference: ESP32-S3 Wi-Fi

### Standards & frequency
- IEEE 802.11 b/g/n, **2.4 GHz only** (no 5 GHz)
- 20 / 40 MHz channel bandwidth (HT20 / HT40)
- 1×1 SISO

### Max PHY data rate
- HT20: 72 Mbps (MCS7)
- HT40: 150 Mbps (MCS7)

### Typical TX power (WROOM-1 datasheet)

| Mode | TX Power |
|------|----------|
| 802.11b, 1 Mbps | 20.5 dBm |
| 802.11g, 6 Mbps | 20.0 dBm |
| 802.11n HT20, MCS0 | 19.0 dBm |
| 802.11n HT20, MCS7 | 17.5 dBm |
| 802.11n HT40, MCS0 | 18.5 dBm |
| 802.11n HT40, MCS7 | 17.0 dBm |

### RX sensitivity
- 802.11b 1 Mbps: **-98 dBm**
- 802.11n HT20 MCS7: typically -72 to -76 dBm (ESP32-family ballpark)

### Power consumption
- Wi-Fi TX (HT20 MCS7 @ 17.5 dBm): ~286 mA
- Wi-Fi TX (HT40 MCS7 @ 17.0 dBm): ~285 mA
- Wi-Fi RX: ~95–97 mA

### Features
- STA + SoftAP
- WPA3
- 802.11mc FTM (Fine Timing Measurement)
- Espressif proprietary Long Range (LR) mode (S3↔S3 only, ~4 dB better than 802.11b)

---

## BLE stack: NimBLE vs Bluedroid

Both certified for BLE 5.4 on ESP32-S3. Neither supports Bluetooth Classic on
S3 (the original ESP32 is the only family member with Classic BT hardware).

| Feature | NimBLE | Bluedroid |
|---------|--------|-----------|
| BLE | Yes | Yes |
| Classic BT | No | Yes (ESP32 only, not S3) |
| Flash savings vs Bluedroid | ~170–254 KB less | Baseline |
| RAM savings vs Bluedroid | ~33–100 KB less heap | Baseline |
| Max BLE connections | up to 9 | up to 7 |
| Reconnection speed | ~200 ms (consistent) | ~600–700 ms (variable) |
| Bluetooth Mesh | Yes | Yes |
| BluFi (Wi-Fi provisioning) | Yes | Yes |
| API style | Single init API, Apache NimBLE | Layered (BTU + BTC), Android-derived |

### RAM detail (ESP-IDF v5.0, measured)
- NimBLE default: ~47 KB IRAM + ~14 KB DRAM static + ~88 KB heap ≈ 149 KB
- Switching Bluedroid → NimBLE: ~167 KB flash + ~8.5 KB heap saved (after tuning)
- NimBLE menuconfig (block/buffer counts) can save ~20 KB additional heap

**This board uses NimBLE** (`firmware/platformio.ini` depends on
`NimBLE-Arduino`). Justification: ESP32-S3 has no Classic BT hardware so
Bluedroid's main differentiator is irrelevant; significantly lower flash/RAM
leaves headroom for application + SD + future OTA; faster reconnection
improves the smartphone-app UX.

---

## Antenna & module placement (WROOM-1)

Source: Espressif ESP32-S3 Hardware Design Guidelines.

### WROOM-1 vs WROOM-1U
- **WROOM-1**: integrated PCB antenna, requires a keep-out zone (used on this board)
- **WROOM-1U**: U.FL/IPEX connector for external antenna, no on-module keep-out

### PCB-antenna keep-out (WROOM-1)
- **≥ 15 mm clearance in all directions** around the antenna area
- No copper, no traces, no components inside the keep-out
- Single most common cause of ESP32 antenna failures in custom designs

### Module placement on the base board
- Strongly recommended: antenna portion **overhangs the base-board edge**
- Feed point of the antenna closest to the edge
- If base board must exist under the antenna, **cut it away**
- Surrounding the antenna with base-board copper on multiple sides is *not* recommended

### Things to keep away from the antenna
- Crystals, DDR SDRAM, high-frequency clocks
- USB connector, USB-to-serial chip, UART signal traces, test points, headers
- Place all of these as far from the antenna as possible

### Ground plane
- Module thermal/ground pad connected through ≥ 9 ground vias
- Continuous unbroken GND plane under the RF, crystal, and chip areas

### End-product verification
- Test throughput and range with the final enclosure — housing material
  and geometry meaningfully affect antenna performance

If using WROOM-1U with an external antenna, RF trace rules apply:
constant trace width, no branching, no via layer changes, 135° bends or
arcs (no 90°), dense ground stitching on both sides, complete GND plane
on the adjacent layer.

---

## OTA firmware updates over Wi-Fi

Source: ESP-IDF v5.5.x (`esp32s3/api-reference/system/ota.html`).

### Partition requirements
- At least **two OTA app partitions** (`ota_0`, `ota_1`)
- One **OTA data partition** (`type: data`, `subtype: ota`, size `0x2000` = 8 KB / two flash sectors)
- Optional `factory` partition for fallback
- A/B scheme — new image written to whichever slot isn't currently active

### Key features
- **HTTPS OTA**: `esp_https_ota` API for downloading over HTTPS
- **Application rollback**: new firmware gets one boot attempt; must call
  `esp_ota_mark_app_valid_cancel_rollback()` to confirm, otherwise automatic
  rollback on next reboot
- **Anti-rollback**: prevents downgrade below a security version (eFuse counter)
- **Signed OTA**: firmware signature verification without enabling full Secure Boot
- **Secure Boot V2**: supported on S3, up to 3 key digests, key rotation
- **Power-failure safe**: OTA data partition uses dual-sector writes with counters

### Relevance to this project
- Update fielded devices without physical access — important for a deployed rehab device
- Wi-Fi connection to a local network or phone hotspot is the update channel
- Signed OTA is worth implementing from day one for any medical-adjacent device
- Flash partitioning must reserve room for two app images + OTA data — already
  satisfied by the 8 MB N8 part (see `microcontroller.md`)

---

## Wi-Fi file transfer (browser ↔ board)

The validation firmware/webapp use **BLE as the control channel and Wi-Fi as an
opportunistic bulk-data channel for SD uploads.** The chunk-acked BLE upload is
correct but slow: it does one `writeWithResponse` + notify-ack per 180-byte
chunk, so throughput is bounded by the BLE connection interval (round-trip per
chunk), not by the PHY rate. Wi-Fi removes the link as the bottleneck — at which
point the SD-over-SPI write speed (~0.5–1 MB/s at the 20 MHz mount, see
`storage.md`) becomes the limit, still far above the acked-BLE rate.

### The browser-security constraint

The webapp must be served over **HTTPS** (Web Bluetooth requires a secure
context; only `localhost` is exempt). Two long-standing browser rules then
collide with talking to the board over Wi-Fi:

- **Mixed content** — an HTTPS page may not issue active requests (`fetch`/XHR/
  WebSocket) to a plain-`http://` origin. The board can't realistically present
  a browser-trusted TLS cert for a LAN IP, so `https://<board>` is out too.
- Historically this forced the upload UI to be **served by the device itself**
  over HTTP (a second page), since a same-origin HTTP page can talk to the
  device freely.

### Local Network Access (LNA) — what unlocks the single-page design

**Chrome 142+** ships **Local Network Access**. A `fetch()`/XHR whose target is
known-local *before* DNS resolution — a **private-IP literal** (`192.168.x.x`), a
**`.local`** name, or a call annotated `targetAddressSpace: "local"` — is
**exempted from the mixed-content block**, gated behind a **one-time user
permission** ("allow this site to access devices on your local network"). This
lets the existing HTTPS app POST straight to the board's plain-HTTP endpoint —
no second page, no TLS cert. Limits: covers `fetch`/XHR/subresources only
(WebSocket/WebTransport/WebRTC are *not* exempted), and the permission prompt is
unavoidable (it is the whole point of LNA — stopping silent LAN probing).

### Implemented design (STA, see decision 002)

- Board joins an existing network as a **STA** (`wifi_scan::connect_sta`), which
  already reports the DHCP IP back over BLE.
- On a successful join the firmware starts a small HTTP server
  (`drivers/webfs.cpp`, built-in `WebServer` :80) with `POST /upload?path=`
  (streamed to SD) and `GET /ping`, all with `Access-Control-Allow-Origin`.
- The webapp POSTs the file to that IP via XHR; **falls back to the BLE chunk
  protocol** if Wi-Fi is down or the LNA permission is denied.
- **SoftAP** (board hosts `192.168.4.1` + captive portal) was considered and is
  the documented fallback for environments with no usable network / client
  isolation, but STA is simpler and keeps the phone online.

Sources: [Chrome Local Network Access](https://developer.chrome.com/blog/local-network-access),
[WICG LNA explainer](https://github.com/WICG/local-network-access/blob/main/explainer.md),
[W3C Secure Contexts](https://www.w3.org/TR/secure-contexts/).

---

## BLE GATT profile considerations

Source: Bluetooth SIG Assigned Numbers + GATT spec.

### Architecture
- **Server** = the PCB (exposes data)
- **Client** = the smartphone app (reads / writes / subscribes)
- Hierarchy: Profile > Service > Characteristic > Descriptor

### SIG-adopted services worth using (16-bit UUIDs)

| Service | UUID | Use here |
|---------|------|----------|
| Device Information | 0x180A | Manufacturer, model, firmware version, serial |
| Battery Service | 0x180F | Battery level (char 0x2A19) — placeholder for future battery revision |

### Custom services (128-bit UUIDs)
No SIG profile exists for rehab robotic hands. Custom services anticipated:
1. **Motor control** — receive finger targets (5 servos), report current positions
2. **Exercise session** — start/stop, exercise type, progress
3. **Sensor data** — hand position / movement telemetry (notifications for streaming)
4. **Device configuration** — nudge settings, sleep/wake, calibration
5. **Data sync** — bulk transfer of logged sessions to the app

Note: the validation firmware in `firmware/` uses one custom service with
per-subsystem characteristics; UUIDs are checked across firmware ↔ webapp by
`tools/check_ble_uuids.py` (see `.github/workflows/ble-uuid-sync.yml`).

### Characteristic design notes
- Use **Notifications** for real-time data (motor positions) — client subscribes via CCCD (0x2902)
- Use **Write With Response** for commands needing confirmation (motor targets, session control)
- Use **Read** for static info (battery level, device info, configuration)
- Default BLE MTU is 23 B (20 B payload). Negotiate higher MTU (up to 517 B
  in BLE 5.0) for bulk transfers.
- LE 2M PHY can be negotiated after connection for higher throughput during data sync.

### Security
- BLE pairing with **LE Secure Connections (LESC)** for encrypted comms
- Bonding (store keys) so re-pairing isn't needed each session
- Consider passkey / numeric-comparison confirmation for initial pairing

---

## Regulatory considerations

Source: FCC Part 15, EU RED 2014/53/EU, EU MDR 2017/745, Bluetooth SIG.

### Pre-certified module advantage
The ESP32-S3-WROOM-1 module carries FCC ID (USA, Part 15.247 for 2.4 GHz ISM),
CE declaration (EU RED), plus IC/TELEC/KCC/SRRC/NCC where applicable. Using a
pre-certified module means the **end product does not need full RF
certification testing** — weeks of paperwork instead of months of lab time,
and significantly lower cost. The end product references the module's FCC ID.

### What is still required
1. **Bluetooth SIG declaration** — must be a SIG Adopter (free) and file a
   product Declaration (~$4 000–$8 000 listing fee) to use the Bluetooth name
   / logo / trademarks.
2. **CE marking (RED)** — manufacturer's self-declaration in Europe. End
   product (with enclosure, antenna placement) must still meet RED.
3. **FCC** — product labelled with the module's FCC ID. Some integrations
   require end-product verification (Class 1 Permissive Change).

### Medical-device regulations (EU MDR 2017/745)
Radio/wireless certification (RED/FCC) is separate from medical-device
certification (MDR). If this device is classified as a medical device:
- CE marking under EU MDR is required in addition to RED CE
- Classification depends on intended use and risk class
- Requires QMS (ISO 13485), technical documentation (Annex II/III), clinical
  evaluation, post-market surveillance
- Class I devices can be self-certified; higher classes require a Notified Body
- Person Responsible for Regulatory Compliance (PRRC) must be appointed
- The BLE stack (error handling, data integrity, retries) will be scrutinised

### Practical notes for this BSc prototype
- Full MDR certification is **out of scope** for the thesis, but the design
  should be **MDR-aware**.
- Keeping data local (microSD, no cloud — see `storage.md`) aligns well with
  GDPR and medical-data constraints.
- Implement BLE LE Secure Connections from the start — easier than retrofitting.
- Document design decisions around data integrity and communication reliability
  in `journal/decisions/` — they'll be valuable if the device ever moves
  toward certification.

### TX-power regulatory limits
- **EU (ETSI):** EIRP limit 20 dBm in the 2.4 GHz band
- **USA (FCC):** up to 30 dBm EIRP for frequency-hopping in 2.4 GHz ISM (BLE qualifies)
- ESP32-S3 max TX +20 dBm is within both (assuming ~0 dBi PCB antenna)
- Default +9 dBm is well within limits and adequate for in-room range

---

## Coexistence: BLE + Wi-Fi on the same 2.4 GHz radio

- ESP32-S3 supports Wi-Fi + BLE coexistence via hardware time-division
- Standard BLE + Wi-Fi STA combination works fine
- Avoid simultaneous Wi-Fi SmartConfig and BLE Mesh

---

## Sources

### Espressif (official)
- [ESP32-S3 product page](https://www.espressif.com/en/products/socs/esp32-s3/)
- [ESP32-S3 SoC datasheet (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [ESP32-S3-WROOM-1/1U datasheet (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- [ESP32-S3 hardware design guidelines](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/pcb-layout-design.html)
- [ESP-IDF BLE overview (NimBLE vs Bluedroid)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/ble/overview.html)
- [ESP-IDF BLE controller & HCI (TX power)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/bluetooth/controller_vhci.html)
- [ESP-IDF OTA — S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/ota.html)
- [ESP-IDF Wi-Fi driver — S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi.html)
- [ESP BLE FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/bt/ble.html)

### Bluetooth SIG
- [Bluetooth Assigned Numbers](https://www.bluetooth.com/specifications/assigned-numbers/)
- [Nordic Bluetooth Numbers Database](https://github.com/NordicSemiconductor/bluetooth-numbers-database)

### Third-party / community
- [NimBLE vs Bluedroid — Hackaday](https://hackaday.io/project/177896/log/241437-bluedroid-vs-nimble-and-blekeyboard)
- [ESP32 Forum: NimBLE advantages](https://www.esp32.com/viewtopic.php?t=16094)
- [Custom GATT services design — Novel Bits](https://novelbits.io/bluetooth-gatt-services-characteristics/)
- [Bluetooth compliance guide (FCC, CE, SIG)](https://bluemaestro.com/resources/bluetooth-compliance-fcc-ce)
- [EU MDR CE-marking process — Emergo by UL](https://www.emergobyul.com/resources/european-medical-devices-regulation-mdr-ce-marking-regulatory-process)
- [Silicon Labs AN1048: regulatory RF module certifications (PDF)](https://www.silabs.com/documents/public/application-notes/an1048-regulatory-certifications.pdf)
