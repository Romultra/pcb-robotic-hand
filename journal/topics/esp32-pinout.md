# ESP32-S3-WROOM-1 GPIO Map (PCB1)

GPIO assignments on the rehabilitation-hand controller PCB, derived from
`production/Netlist_Schematic1_2026-06-02.tel`. Module pin numbers follow the
Espressif ESP32-S3-WROOM-1 datasheet (also shown in `datasheets/esp32.png`).

## Quick reference

| Module pin | GPIO | Net | Subsystem | Direction | Notes |
|-----------:|:----:|-----|-----------|:---------:|-------|
| 1   | GND   | GND | Power | — | |
| 2   | 3V3   | 3.3V | Power | — | From AP2114H LDO. |
| 3   | EN    | RESET | Reset | in | SW2 RESET button. Pull-up R5 (5.1 kΩ), debounce C5 (1 µF). |
| 4   | GPIO4 | (to U6 level-shifter) | Servos | out | Servo PWM channel. |
| 5   | GPIO5 | (to U6 level-shifter) | Servos | out | Servo PWM channel. |
| 6   | GPIO6 | (to U6 level-shifter) | Servos | out | Servo PWM channel. |
| 7   | GPIO7 | (to U6 level-shifter) | Servos | out | Servo PWM channel. |
| 8   | GPIO15 | (to U6 level-shifter) | Servos | out | Servo PWM channel. U6 OE is hard-tied on the PCB. |
| 12  | GPIO8 | SERVO_EN | Servo power | out | Drives TPS56637 EN. **High = servo rail on.** Strapping-pin caveat below. |
| 13  | GPIO19 | D− | USB | bidir | Native USB-OTG D−. To USB-C (with D3 TVS). |
| 14  | GPIO20 | D+ | USB | bidir | Native USB-OTG D+. To USB-C (with D2 TVS). |
| 25  | GPIO48 | CD | microSD | in | Card-detect mechanical switch on CARD1. |
| 26  | GPIO45 | MISO | microSD | in | SPI MISO. 47 kΩ pull-up (RN2). Strapping-pin caveat below. |
| 27  | GPIO0 | GPIO0 | Boot | in | SW1 BOOT button. Strapping pin — keep released at boot. |
| 28  | GPIO35 | SCLK | microSD | out | SPI clock. **22 Ω series term (R6).** 47 kΩ pull-up (RN1). |
| 29  | GPIO36 | MOSI | microSD | out | SPI MOSI. 47 kΩ pull-up (RN1). |
| 30  | GPIO37 | CS | microSD | out | SPI chip-select. 47 kΩ pull-up (RN1). |
| 31  | GPIO38 | (via R8 → Q2 gate) | Buzzer | out | 2.7 kHz MLT-8530 buzzer, low-side N-FET driven. 470 Ω gate resistor. |
| 32  | GPIO39 | NEO | RGB LEDs | out | WS2812B chain data, via SN74LV1T34 level-shifter (U3). |
| 34  | GPIO41 | (via R10 → U7.SD_MODE) | Audio amp | out | MAX98357A SD_MODE / gain select, through 634 kΩ. |
| 35  | GPIO42 | DIN | Audio I²S | out | MAX98357A I²S data. |
| 38  | GPIO2 | BCLK | Audio I²S | out | MAX98357A I²S bit clock. |
| 39  | GPIO1 | LRCLK | Audio I²S | out | MAX98357A I²S word select. |
| 40  | GND | GND | Power | — | |
| 41  | EPAD | GND | Power | — | Thermal pad. |

All other module pins (GPIO9–14, 16–18, 21, 46–47, GPIO3, RXD0/GPIO44,
TXD0/GPIO43, and the IO0-pin alternates) are **left unconnected** on the PCB.

## GPIO usage rollup (which features lock which pins)

| Subsystem | GPIOs used |
|-----------|-----------|
| microSD (SPI) | 35 SCLK, 36 MOSI, 45 MISO, 37 CS, 48 CD |
| I²S audio | 42 DIN, 2 BCLK, 1 LRCLK, 41 SD_MODE/gain |
| WS2812B LEDs | 39 (via U3) |
| Buzzer | 38 |
| Servo PWMs | 4, 5, 6, 7, 15 (headers H1..H5 = GPIO 15, 7, 6, 5, 4 — reverse order) |
| Servo power enable | 8 |
| USB (native) | 19 D−, 20 D+ |
| Boot button | 0 |

## Strapping & special-purpose pin notes

ESP32-S3 has several strapping pins read at reset; the design picks GPIOs to
avoid stepping on them, but a few connections need care:

- **GPIO0** (SW1 BOOT button) — strapping. Must read 1 at reset for SPI-flash
  boot; pulling it to 0 enters USB-DFU / download mode. Internal pull-up keeps
  it high when the button is released.
- **GPIO45** (MISO) — strapping. Selects VDD_SPI source at reset; the SD pull-up
  on RN2 puts it at logic-1 at boot, which is the safe default.
- **GPIO46** — strapping. Not connected on this board.
- **GPIO3** — strapping. Not connected.
- **GPIO8** (SERVO_EN) — *not* a strapping pin, but it directly enables the
  servo power rail. After reset its state is undefined until firmware drives it;
  TPS56637 will stay off (EN has its own internal pull-down behavior, and
  GPIO8 floats) which is the safe failure mode for the hand. Firmware should
  drive it low explicitly during early init regardless.

## Notes on availability for future revisions

A lot of useful peripherals on the ESP32-S3 are unconnected and could be wired
in a future revision without losing existing features:

- **UART0** (GPIO43/44 — RXD0/TXD0, module pins 36/37) is free, so a debug
  console header is trivial to add.
- **Many ADC1 channels** (GPIO1–10) are free *in principle*, but several are
  consumed by audio + servo functions. ADC2 channels (GPIO11–20) overlap with
  Wi-Fi usage and shouldn't be used while the radio is active.
- The lower-numbered GPIOs (9–14, 16–18, 21) are entirely unconnected and good
  candidates for future buttons, sensors, or an external interface (e.g. I²C
  for an IMU).

## Sources

- `production/Netlist_Schematic1_2026-06-02.tel`
- `datasheets/esp32.png` — module pinout reference
- Espressif ESP32-S3 Series Datasheet (strapping-pin behavior)
- `journal/topics/board-architecture.md` — what each signal does on the board
