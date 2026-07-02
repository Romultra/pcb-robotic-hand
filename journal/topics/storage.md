# Local Storage

**Implemented:** push-push microSD card socket (CARD1) wired for SPI mode to
the ESP32. Card capacity is not fixed by the design — anything that speaks the
SD specification works.

Pin mapping (see `esp32-pinout.md` for the full GPIO table):

| Signal | ESP32 GPIO | Series term / pull-up |
|--------|-----------|-----------------------|
| SCLK   | GPIO35    | 22 Ω series (R6), 47 kΩ pull-up |
| MOSI   | GPIO36    | 47 kΩ pull-up |
| MISO   | GPIO45    | 47 kΩ pull-up |
| CS     | GPIO37    | 47 kΩ pull-up |
| CD     | GPIO48    | 47 kΩ pull-up |

Pull-ups are provided by RN1 + RN2, both 4-resistor 47 kΩ 0603 arrays.

Why SD over an SPI flash chip:

- Removable — clinicians can physically pull the card to retrieve data, which
  cleanly sidesteps the "no cloud" medical-data constraint.
- Capacities (GB-scale) far exceed anything reasonable for years of logged
  hand-tracking and movement data.
- Standard FAT filesystem support in ESP-IDF / Arduino — no custom flash
  driver work.

The ESP32-S3 supports both SPI and 1/4-bit SDIO host modes. The current wiring
is SPI; switching to SDIO would be firmware-only on these same pins if higher
throughput is ever needed (though SPI is more than enough for the expected
write rate of motion-tracking logs).

---

## Reference: SPI vs SDMMC on ESP32-S3

The ESP32-S3 supports both:

- **SDMMC host** — dedicated SD/MMC peripheral, 2 slots, 1-bit / 4-bit SD
  mode, pins routable via the GPIO matrix. Higher throughput.
- **SPI mode** — uses SPI2 (FSPI) or SPI3 (HSPI). Fewer pins (4 signal lines),
  shares bus with other SPI devices, but roughly half the speed of SDMMC
  even in 1-bit mode.

(SPI0/SPI1 are reserved for internal flash/PSRAM and can't be used.)

### Typical throughput

| Mode | Read | Write | Notes |
|------|------|-------|-------|
| SPI @ 4 MHz (Arduino SD default) | ~270–500 KB/s | ~200–270 KB/s | |
| SPI optimised, 20–40 MHz | ~1.0–1.7 MB/s | ~0.5–1.0 MB/s | Raw sector access |
| SDMMC 4-bit @ 20 MHz raw | ~7–9 MB/s | ~5–7 MB/s | ESP-IDF SDMMC driver |
| SDMMC 4-bit + FatFs | ~3–5 MB/s | ~2–4 MB/s | Realistic filesystem use |

Speed depends heavily on block size (larger = faster), card class, and raw vs
filesystem access.

3.3 V GPIO is directly compatible with SD-card I/O — no level shifting.

---

## Alternatives considered

Before the microSD socket was chosen, two on-board storage options were
evaluated. The summary below is *why* removable SD won, not just *what was
considered*.

### Option A — SPI NOR flash (Winbond W25Q128JV)

128 Mbit (16 MB) serial NOR in 8-pin SOIC/WSON.

| Parameter | Value |
|-----------|-------|
| Capacity | 16 MB |
| Interface | Standard / Dual / Quad SPI |
| Max SPI clock | 133 MHz |
| Supply | 2.7–3.6 V |
| Page / sector erase | 256 B / 4 KB |
| Endurance | ~100 k P/E cycles per sector |
| Active read | ~15 mA (max 25 mA) |
| Power-down current | ~1 µA |

Pros: extremely low power, soldered (no connector failure), tiny footprint,
3.3 V native. Cons: **16 MB ceiling**, no built-in wear levelling, can't be
physically removed for data retrieval.

### Option B — SD NAND (XTX XTSD series, LGA-8)

An "SD card in a chip" — NAND + controller in an 8 × 6 mm SMD package,
speaking SD 2.0 over SPI or SDIO.

| Parameter | Value |
|-----------|-------|
| Capacities | XTSD01G/02G/04G/08G — 128 MB / 256 MB / 512 MB / 1 GB |
| Interface | SD 2.0 (SDIO + SPI compatible) |
| Max clock | 50 MHz |
| Supply | 2.7–3.6 V |
| Endurance | 50 k–100 k P/E cycles (SLC NAND) |
| Built-in | ECC, wear levelling, power-failure protection |
| Read / write current | max 30 mA |
| Sleep | ~200 µA |

Pros: soldered (no connector), drop-in software-compatible with SD-card
drivers, built-in ECC + wear levelling, power-failure protection. Cons:
not removable, max 1 GB, less widely stocked than µSD sockets.

### Power-consumption comparison

| Parameter | microSD | W25Q128JV | XTX XTSD04G |
|-----------|---------|-----------|-------------|
| Active read | 40–100 mA (spiky) | ~15 mA (max 25) | max 30 mA |
| Active write | 60–100 mA (peaks 300 mA) | ~15 mA (max 25) | max 30 mA |
| Standby/idle | 0.2–30 mA (variable) | ~25 µA | max 20 mA (clock active) |
| Sleep/power-down | ~250 µA (spec) | ~1 µA | ~200 µA (clock stopped) |
| Max capacity | 32 GB (FAT32) | 16 MB | 1 GB |
| Removable | **Yes** | No | No |
| Wear levelling | Built-in (SD controller) | None (host-managed) | Built-in |

NOR flash wins on power by a wide margin if 16 MB suffices. SD NAND is the
middle ground. microSD is the most power-unpredictable but uniquely
**removable** — the deciding factor for a device that needs to hand off
patient data without uploading it anywhere.

µSD spiky current draw also imposes the most demanding decoupling
requirement of the three. Source for the µSD numbers:
[Gough's Tech Zone microSD power experiment](https://goughlui.com/2021/02/27/experiment-microsd-card-power-consumption-spi-performance/).

---

## ESP-IDF filesystem options

| Filesystem | Storage target | Wear levelling | Directories | Power-loss safe | Status |
|------------|----------------|----------------|-------------|-----------------|--------|
| **FatFs (SD card)** | µSD / SD NAND | Built into SD controller | Yes | Depends on card | Active, well-supported |
| FatFs (SPI flash) | Internal or external NOR | ESP-IDF WL layer | Yes | Risk during power-off | Active |
| **LittleFS** | Internal or external NOR | Built-in | Yes | Yes (log-structured) | Recommended (ESP Component Registry) |
| SPIFFS | Internal or external NOR | Built-in | No (flat) | No | Deprecated |
| NVS | Internal flash | Built-in | Key-value only | Yes | Active (config only, not bulk data) |

The board uses **FatFs over SDSPI**. The validation firmware
(`firmware/src/drivers/sdcard.cpp`) wraps it via Arduino's `SD.h`
(`SD.begin(cs, spi, 20 MHz)`); a future ESP-IDF firmware would call the
underlying `esp_vfs_fat_sdspi_mount()` directly. Either way the on-card
filesystem is FAT (FAT12/16/32; no exFAT in ESP-IDF). Long filenames are
enabled in ESP-IDF via `CONFIG_FATFS_LONG_FILENAMES`.

The driver exposes the card four ways: a self-test, a chunked file upload (with a
fast Wi-Fi/HTTP path), audio playback, and exercise logging. Audio access is
walled off to a single `/audio` folder via `list_audio()` / `open_audio()`, so
the WAV player and its file list can never reach the rest of the card. See
`journal/decisions/005-sd-wav-audio-playback.md`.

## Exercise position logging (`/logs`)

This is the storage subsystem's intended purpose finally built (requirements
STO-1/2/3). While an exercise runs, the board samples the five finger positions
and writes them to a CSV in a dedicated `/logs` folder, one file per session plus
an `index.csv` manifest. An exercise is detected by command channel: only
`SERVO_FRAME` streams (live hand-mirror and clip playback) are logged, never
manual sliders (`SERVO_SET`) or calibration (`SERVO_CAL`). A session opens on the
first exercise frame and closes after a 3 s idle gap, so each bout is its own
file.

Values are normalized flexion percent (0 = open, 100 = closed), calibration-
independent; each file header records the per-finger calibration band so raw
degrees stay reconstructable. The board has no RTC, so files use a monotonic NVS
session counter (`sess-NNNNN.csv`) unless the webapp pushes wall-clock time over
BLE, in which case they are date-named (`YYYYMMDD-HHMMSS.csv`).

Folder confinement mirrors `/audio`: `sdcard::open_log()` / `list_logs()` are the
only paths to `/logs`, both using the same bare-filename guard. The `logging`
driver runs its state machine from the main loop (single-task SD access), and a
`log_ctrl` BLE characteristic (`000f`) enables/disables logging, pushes time, and
reports status. At 20 Hz × 6 small columns the write rate is trivial for SPI, as
anticipated above. See `journal/decisions/006-exercise-position-logging.md`.

Retrieval is currently by pulling the card; a webapp panel to list/download
sessions over the existing transport is future work.

Source: [ESP-IDF File System Considerations (S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/file-system-considerations.html).

---

## Sources

- [ESP-IDF FATFS docs (S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/fatfs.html)
- [ESP-IDF SDMMC host driver (S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/sdmmc_host.html)
- [ESP-IDF sharing SPI bus among SD cards (S3)](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/sdspi_share.html)
- [Winbond W25Q128JV datasheet (Mouser)](https://www.mouser.com/datasheet/2/949/w25q128jv_revf_03272018_plus-1489608.pdf)
- [XTX XTSD04G datasheet (Adafruit CDN)](https://cdn-shop.adafruit.com/product-files/4899/2005251034_XTX-XTSD04GLGEAG_C558839(2).pdf)
- [Gough's Tech Zone microSD power experiment](https://goughlui.com/2021/02/27/experiment-microsd-card-power-consumption-spi-performance/)
