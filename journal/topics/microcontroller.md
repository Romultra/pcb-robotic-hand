# Microcontroller

**Selected: ESP32-S3-WROOM-1-N8** (Espressif). Dual-core Xtensa LX7 @ 240 MHz,
512 KB SRAM, 8 MB on-module flash, no PSRAM. 2.4 GHz Wi-Fi (b/g/n) + Bluetooth
5 LE with onboard PCB antenna. Module is powered from the board's 3.3 V LDO
rail.

Why this one over the main wireless-MCU alternatives (nRF52840, STM32WB55,
ESP32-C6):

- Wi-Fi + BLE in a single pre-certified module → no separate radio IC, no RF
  layout effort, FCC/CE pre-certs already done.
- Native full-speed USB controller on GPIO19/20 → no USB-to-UART bridge IC, the
  programming port doubles as the host comms link for the companion app during
  bring-up.
- Comfortable GPIO count (45+ usable) — leaves headroom for future revisions
  (IMU, encoders, debug header).
- Mature dev ecosystem (ESP-IDF, Arduino-ESP32, MicroPython) — keeps the BSc
  scope realistic for one student.

Full pinout and per-pin function: `esp32-pinout.md`.
Board-level role and connections: `board-architecture.md`.

---

## Reference: ESP32-S3 SoC / WROOM-1 module

### SoC
- **CPU:** Xtensa dual-core 32-bit LX7, up to 240 MHz
- **Internal SRAM:** 512 KB; ROM: 384 KB; RTC SRAM: 16 KB (8 KB fast + 8 KB slow)
- **BLE:** Bluetooth LE 5.0 (certified up to 5.4 in software). Coded PHY (long range) + 2 Mbps PHY. No Bluetooth Classic.
- **Wi-Fi:** 802.11 b/g/n, 2.4 GHz, 20/40 MHz bandwidth
- **GPIO:** 45 programmable
- **LEDC PWM:** 8 channels, 4 timers (one speed group on S3)
- **MCPWM:** 2 units (3 operators / 3 timers / 6 outputs each)
- **ADC:** 2× 12-bit SAR, 20 channels total (ADC2 unavailable during Wi-Fi)
- **SPI:** 4 controllers (SPI0/1 reserved for flash/PSRAM; SPI2/3 general-purpose)
- **I²C:** 2; **UART:** 3 (up to 5 Mbps); **I²S:** 2
- **USB:** 1× Full-Speed USB 2.0 OTG + 1× USB Serial/JTAG
- **Other:** 1× SDIO host (2 slots), 1× TWAI (CAN 2.0), 1× RMT, 1× PCNT, LCD/camera interface, GDMA
- **Touch:** 14 capacitive-touch GPIOs
- **ULP:** RISC-V + FSM coprocessors

### Module (WROOM-1, PCB antenna)
- 18.0 × 25.5 × 3.1 mm, 41 pins
- 3.3 V supply
- WROOM-1U variant has a U.FL connector instead of the PCB antenna (18.0 × 19.2 × 3.2 mm)

### Power consumption (Espressif official measurements)

| Mode | Typical | Notes |
|------|---------|-------|
| Wi-Fi TX | 180–240 mA | Depends on TX power, modulation, data rate |
| CPU @ 240 MHz, no RF | 30–68 mA | |
| CPU @ 80 MHz, no RF | 20–25 mA | |
| Modem-sleep | ~10 mA | Wi-Fi clock gated |
| Light-sleep | ~800 µA | |
| Deep-sleep | 8.14 µA | Espressif Joulescope measurement on WROOM-1 |
| Power off (CHIP_PU low) | ~0.1 µA | |

Source: Espressif ESP-IDF current-consumption measurement modules.

### Why the **N8** suffix specifically

WROOM-1 ships in many flash/PSRAM combinations. The selection driver was
"enough flash for OTA partitions + filesystem reserve, no need for PSRAM":

| Code | Flash | PSRAM | Temp |
|------|-------|-------|------|
| N4 | 4 MB | — | -40…85 °C |
| **N8** | **8 MB** | **—** | **-40…85 °C** |
| N16 | 16 MB | — | -40…85 °C |
| H4 | 4 MB | — | -40…105 °C |
| N4R2 / N8R2 / N16R2 | 4/8/16 MB | 2 MB Quad SPI | -40…85 °C |
| N4R8 / N8R8 / N16R8 | 4/8/16 MB | 8 MB Octal SPI | -40…65 °C |
| N16R16VA | 16 MB | 16 MB Octal SPI | -40…65 °C |

Practical constraint that rules out the PSRAM variants for this design:
- **R8/R16 (Octal SPI PSRAM)** uses **GPIO33–37**, which collides directly
  with the pins this board uses for SD-card SPI (GPIO35/36/37 — see
  `esp32-pinout.md`). An R8/R16 part on this board would not boot the SD
  card without rewiring.
- **R2 (Quad SPI PSRAM)** uses GPIO26–32. None of those pins are currently
  used on the board, so R2 would technically be wireable, but it would
  consume the same lower-GPIO pool reserved for future expansion (IMU, debug
  header, buttons) — and there is no application need for PSRAM in any case.

There is no application need for PSRAM either — firmware is a thin BLE/I²S/SPI
glue layer, not a video-buffer / camera / large-model workload. 8 MB of flash
comfortably fits two OTA app partitions + NVS + a future LittleFS reserve.
N4 would also work but offers no headroom; N16 is overkill.

Naming: N = normal-temp flash, H = high-temp flash; number after = flash MB;
R = PSRAM, number after R = PSRAM MB.

### Certifications (carried by the module)

FCC (ID: 2AC7Z-ESPS3WROOM1), CE-RED, IC/ISED, TELEC/MIC, KCC, SRRC, NCC, BQB,
RoHS/REACH. Pre-certification is one of the main reasons the module form
factor wins over a bare SoC for a one-student BSc project (see `wireless.md`
for the regulatory discussion).

---

## Alternatives considered

Three wireless MCUs were evaluated in the early design phase before settling
on ESP32-S3. None of them carry both Wi-Fi and BLE in one chip — the deciding
factor.

### nRF52840 (Nordic Semiconductor)

| Spec | Value |
|------|-------|
| CPU | ARM Cortex-M4F, 64 MHz, single-core |
| Flash / RAM | 1 MB / 256 KB (internal) |
| BLE | 5.3 (+ Mesh, Thread, Zigbee, 802.15.4, ANT) |
| **Wi-Fi** | **None** |
| GPIO | 48 |
| PWM | 4× 4-ch = 16 outputs (per-module shared period) |
| ADC | 12-bit, 8 ch |
| USB | Full-Speed USB 2.0 device |
| NFC | NFC-A tag built in |
| Supply | 1.7 – 5.5 V (LDO or DC-DC) |
| Deep sleep (System OFF) | ~0.3 µA SoC, ~3 µA typical board |
| BLE TX / RX | ~5–6 mA each |
| Temp | -40 to 85 °C |

**Strengths:** order-of-magnitude lower power than ESP32-S3, best-in-class BLE
stack, wide supply range, built-in NFC.
**Killer:** no Wi-Fi → an external Wi-Fi module would erase the
single-package-saves-RF-effort argument that motivates the choice to begin with.

### STM32WB55 (STMicroelectronics)

| Spec | Value |
|------|-------|
| CPU | Cortex-M4 @ 64 MHz (app) + Cortex-M0+ (dedicated radio) |
| Flash / RAM | up to 1 MB / 256 KB |
| BLE | 5.4 + 802.15.4 (Zigbee, Thread) |
| **Wi-Fi** | **None** |
| GPIO | up to 72 |
| USB | USB 2.0 FS (crystal-less) |
| Supply | 1.71 – 3.6 V |
| Shutdown / Stop / Standby | 13 nA / 600 nA / 600 nA |
| BLE TX (0 dBm) / RX | 5.2 mA / 4.5 mA |
| Security | 2× AES, PKA, RNG, customer key storage |

**Strengths:** the lowest sleep current of any candidate (nA-range), dedicated
radio core, strong hardware security.
**Killer:** same as nRF52840 — no Wi-Fi. Plus a smaller community for hobby
BLE work and turnkey antenna modules.

### ESP32-C6 (Espressif)

| Spec | Value |
|------|-------|
| CPU | RISC-V HP @ 160 MHz + LP core @ 20 MHz |
| SRAM | 512 KB HP + 16 KB LP |
| BLE | 5.3 |
| Wi-Fi | **Wi-Fi 6 (802.11ax) + b/g/n**, 2.4 GHz |
| 802.15.4 | Yes (Thread, Zigbee) |
| GPIO | 30 (QFN40) |
| LEDC | 6 channels, low-speed only |
| ADC | 12-bit, 7 ch |
| USB | USB Serial/JTAG only (no OTG) |
| Deep sleep | ~5–7 µA |
| Active (Wi-Fi) | ~60 mA average |

**Strengths:** Wi-Fi 6 with TWT (much better duty-cycled Wi-Fi power),
802.15.4 / Matter-ready, RISC-V LP coprocessor.
**Killer for this board:** only 30 GPIO (tight for servos + SD + nudge + future
expansion), only 6 LEDC channels (workable but no headroom), no USB OTG.
Worth re-evaluating for a future revision if/when battery life becomes the
dominant constraint.

### Summary table

| Feature | ESP32-S3 (chosen) | nRF52840 | STM32WB55 | ESP32-C6 |
|---------|-------------------|----------|-----------|----------|
| CPU | 2× LX7 @ 240 MHz | M4F @ 64 MHz | M4 @ 64 MHz + M0+ radio | RISC-V @ 160 MHz + LP |
| Wi-Fi | b/g/n | **None** | **None** | Wi-Fi 6 + b/g/n |
| BLE | 5.0 | 5.3 | 5.4 | 5.3 |
| 802.15.4 | No | Yes | Yes | Yes |
| SRAM | 512 KB | 256 KB | 256 KB | 512 KB + 16 KB LP |
| Flash | 4–16 MB ext. | 1 MB int. | up to 1 MB int. | 4–8 MB ext. |
| PSRAM | up to 16 MB | None | None | None |
| GPIO | 45 | 48 | up to 72 | 30 |
| Deep sleep | ~7–8 µA | ~0.3–3 µA | 13 nA – 600 nA | ~5–7 µA |
| Active Wi-Fi TX | 180–240 mA | N/A | N/A | ~60 mA avg |
| Active BLE | ~100 mA | ~5–6 mA | ~4.5–5.2 mA | (not verified) |
| Vcc | 3.3 V | 1.7–5.5 V | 1.71–3.6 V | 3.3 V |
| Module price | ~$3–4 | ~$5–8 | ~$5–10 | ~$2–4 |

The single dominant criterion — Wi-Fi + BLE in one pre-certified module —
eliminates nRF52840 and STM32WB55. Between ESP32-S3 and ESP32-C6 the
deciding factor was GPIO budget and ecosystem maturity for this board's mix
of servos / SD / I²S / nudge peripherals.

---

## Key references

- [ESP32-S3 SoC datasheet (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [ESP32-S3-WROOM-1/1U datasheet (PDF)](https://www.espressif.com/sites/default/files/documentation/esp32-s3-wroom-1_wroom-1u_datasheet_en.pdf)
- [ESP32-S3 product page](https://www.espressif.com/en/products/socs/esp32-s3)
- [ESP-IDF current-consumption measurement (S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/current-consumption-measurement-modules.html)
- [ESP-IDF LEDC (S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/ledc.html)
- [Espressif certificates](https://www.espressif.com/en/support/documents/certificates)
- [nRF52840 product specification](https://docs.nordicsemi.com/bundle/ps_nrf52840/page/keyfeatures_html5.html)
- [STM32WB55 datasheet](https://www.st.com/resource/en/datasheet/stm32wb55cc.pdf)
- [ESP32-C6 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c6_datasheet_en.pdf)
