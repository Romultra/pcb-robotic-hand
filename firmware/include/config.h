#pragma once

#include <Arduino.h>

// PCB1 pin assignments — single source of truth.
// Matches journal/topics/esp32-pinout.md, derived from
// production/Netlist_Schematic1_2026-06-02.tel.

namespace cfg {

// Servos — five PWM channels through the TXS0108E level shifter (U6).
// U6 OE is hard-tied on the PCB; firmware does not control it.
// Order is header/finger order (S1..S5), NOT ascending GPIO: the PCB routes the
// level shifter so header H1 (finger 1) lands on GPIO15 and H5 (finger 5) on
// GPIO4 — i.e. the GPIOs run in reverse header order. Verified against
// production/Netlist_Schematic1 (GPIO15→H1 … GPIO4→H5) and on the bench. Listing
// them this way makes LEDC channel i / slider Si drive finger i+1.
constexpr uint8_t SERVO_PINS[5] = { 15, 7, 6, 5, 4 };
constexpr int     SERVO_COUNT   = sizeof(SERVO_PINS) / sizeof(SERVO_PINS[0]);

// SERVO_EN drives the TPS56637 (U4) EN pin: high = 5V-SERVO rail powered.
// Floats low at boot, so the rail is safely off until firmware enables it.
constexpr uint8_t SERVO_EN_PIN  = 8;

// Servo safety / calibration tunables (tenths of a degree unless noted).
// JOG_MAX caps a single jog command so a measurement step can never make a big
// sudden move; RAMP_STEP/RAMP_PERIOD set the gentle move-to-open ramp speed.
constexpr uint16_t SERVO_JOG_MAX_X10    = 50;   // ±5.0° max per jog command
constexpr uint16_t SERVO_RAMP_STEP_X10  = 20;   // 2.0° per ramp tick
constexpr uint32_t SERVO_RAMP_PERIOD_MS = 15;   // ramp update cadence

// Exercise position logging. The board logs the five finger positions to a CSV
// per exercise session in the SD card's /logs folder, sampled while an exercise
// (SERVO_FRAME stream) is running. SAMPLE_PERIOD sets the row cadence, a gap
// longer than IDLE_TIMEOUT with no exercise frame ends the session, and the open
// file is flushed every FLUSH_PERIOD so a power loss costs at most ~1 s of rows.
// See drivers/logging.{h,cpp} and journal/decisions/006-exercise-position-logging.md.
constexpr uint32_t LOG_SAMPLE_PERIOD_MS = 50;     // 20 Hz position sampling
constexpr uint32_t LOG_IDLE_TIMEOUT_MS  = 3000;   // no frame this long ends a session
constexpr uint32_t LOG_FLUSH_PERIOD_MS  = 1000;   // flush the open log this often

// GPIO39 wires to the WS2812B chain (LED2–LED5) via the SN74LV1T34 buffer (U3),
// but those LEDs are non-functional on PCB1 — their VDD/VSS pads are reversed in
// the schematic (see journal/decisions/001-ws2812b-power-pin-reversal.md).
// Firmware does not drive GPIO39, so no NeoPixel pin/count constants here.

// MLT-8530 magnetic buzzer, low-side switched by 2N7002 N-FET (Q2)
// through 470 Ω gate resistor. Coil sits between 3.3 V and Q2 drain.
constexpr uint8_t BUZZER_PIN = 38;

// I²S to MAX98357A (mono Class-D amp → external speaker on CN1).
constexpr uint8_t I2S_BCLK_PIN   = 2;
constexpr uint8_t I2S_LRCLK_PIN  = 1;
constexpr uint8_t I2S_DIN_PIN    = 42;
constexpr uint8_t AUDIO_MODE_PIN = 41;   // MAX98357A SD_MODE / gain (via 634 kΩ)

// microSD over SPI.
constexpr uint8_t SD_SCLK_PIN = 35;      // 22 Ω series term on the trace (R6)
constexpr uint8_t SD_MOSI_PIN = 36;
constexpr uint8_t SD_MISO_PIN = 45;
constexpr uint8_t SD_CS_PIN   = 37;
constexpr uint8_t SD_CD_PIN   = 48;      // active-low mechanical detect switch

// User-facing buttons.
constexpr uint8_t BOOT_BTN_PIN = 0;      // GPIO0 / SW1 (strapping pin)

// Status notify cadence over BLE (milliseconds).
constexpr uint32_t STATUS_NOTIFY_PERIOD_MS = 200;

// BLE connection-parameter request, issued on connect. The central (the phone)
// picks the connection interval, and Android's default is conservative (tens of
// ms); that interval, not our firmware, sets the command round-trip on the
// live-mirror path. Asking for a tight interval is the main firmware lever on
// BLE latency. Units: interval in 1.25 ms steps, supervision timeout in 10 ms
// steps. 6 = 7.5 ms (the BLE minimum), 12 = 15 ms; latency 0 so the board never
// skips a connection event; 400 = 4.0 s timeout (kept long so Wi-Fi/BLE
// coexistence airtime stealing doesn't trip a spurious supervision drop).
constexpr uint16_t BLE_CONN_ITVL_MIN = 6;
constexpr uint16_t BLE_CONN_ITVL_MAX = 12;
constexpr uint16_t BLE_CONN_LATENCY  = 0;
constexpr uint16_t BLE_CONN_TIMEOUT  = 400;

// Relaxed "bulk transfer" connection parameters, requested while a Wi-Fi SD
// upload is in flight and restored to the tight set above when it finishes.
// The S3 has a single 2.4 GHz radio shared between BLE and Wi-Fi, so the tight
// 7.5 ms / latency-0 interval above (great for control round-trip) starves the
// concurrent Wi-Fi upload — measured in journal/decisions/008. During an upload
// BLE carries no traffic (control is paused), so we ask the central for a slow
// interval (30–50 ms) plus a slave latency, letting the peripheral skip idle
// connection events and hand almost all airtime to Wi-Fi. The link still stays
// up well inside the 4.0 s supervision timeout: (1+latency)*itvl_max*2 ≈ 0.5 s.
// 24 = 30 ms, 40 = 50 ms.
constexpr uint16_t BLE_CONN_ITVL_MIN_BULK = 24;
constexpr uint16_t BLE_CONN_ITVL_MAX_BULK = 40;
constexpr uint16_t BLE_CONN_LATENCY_BULK  = 4;

// Build identification.
constexpr const char* FW_NAME    = "PCB1-Validator";
constexpr const char* FW_VERSION = "0.1.0";

}  // namespace cfg
