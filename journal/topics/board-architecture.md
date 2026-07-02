# Board Architecture (PCB1, 2026-06-02)

Functional overview of the first rev of the rehabilitation-hand controller PCB.
Source of truth: the schematic + `production/Netlist_Schematic1_2026-06-02.tel`. The BOM
(`production/BOM_Board1_PCB1_2026-06-25.csv`) lists exact part numbers.

The board is built around an **ESP32-S3-WROOM-1-N8** module and integrates five
subsystems: power, microcontroller + wireless, local storage, user nudge (audio +
visual), and motor (servo) drive.

```
                +----------------+
   DC barrel ---| Q1 P-FET (rev. |---+--> VIN ---> [TPS56637 U4] --EN-- 5V-SERVO --> 5 servo headers
   jack DC1     | polarity prot.)|   |
                +----------------+   +--> [TPS54202 U8] -------------> 5V-IN
                                                                          |
                                                                          v Q3 P-FET (off when VBUS present)
   USB-C VBUS -----[D8 Schottky]------------------------------------> 5V (rail)
                                                                          |
                                                                          v
                                                                    [AP2114H U2 LDO]
                                                                          |
                                                                          v
                                                                        3.3V
```

## Power topology

Two physical inputs share a single regulated 5 V rail, with the USB-C port
taking priority over the DC jack.

| Stage | Part | Role |
|------|------|------|
| DC reverse-polarity protection | **Q1 AO4485** P-MOSFET (SOP-8) | Ideal-diode-style protection on DC jack. Gate pulled to GND by R7 (10 kΩ), V_GS clamped at −12 V by Zener D4 (BZT52C12). When the jack is plugged in correctly, body diode conducts first, then the FET fully enhances. |
| DC input transient suppression | **D9 SMBJ22CA** (22 V bidir TVS) | Clamps spikes on DC input. |
| Main 5 V buck (logic + audio + LEDs) | **TPS54202 U8** | V_IN → 5V-IN. L2 = 15 µH, Cout2 = 47 µF, divider R11 (100 kΩ) / R12 (13.7 kΩ) → V_out ≈ 5 V. |
| Servo 5 V buck | **TPS56637 U4** | V_IN → 5V-SERVO. L1 = 3.3 µH, Cout = 47 µF, divider Rfbt (73.2 kΩ) / Rfbb (10 kΩ) → V_out ≈ 5 V. **EN pin is wired to ESP32 GPIO8 (SERVO_EN)** — firmware can power the servo rail down when the hand is idle. |
| 5 V power-or-mux (USB vs DC) | **D8 PMEG3050EP** Schottky + **Q3 DMG3415U** P-FET | D8 lets USB VBUS feed the 5 V rail directly (Vf ≈ 0.3 V). Q3 is gated by VBUS: when USB is plugged in, Q3 turns off and isolates the TPS54202 output (5V-IN) from the 5 V rail, preventing backfeed. When USB is unplugged, Q3 conducts and 5V-IN feeds 5 V. |
| 3.3 V rail | **AP2114H-3.3 U2** LDO (SOT-223) | 5 V → 3.3 V for the ESP32 module, microSD, level-shifter A-side, buzzer, status LEDs. |
| Bulk input filtering | 4 × 470 µF electrolytics (C21–C24) + 15 µF ceramics (Cin, Cin2) | Input ripple + servo current transients. |
| USB ESD | **D1 LESD5D5.0CT** on VBUS, **D2/D3** on D+/D− | Bidirectional TVS on the USB connector. |

### Servos cannot run on USB power alone
Because TPS56637 is a step-down (buck) and USB-C delivers 5 V (the same as the
servo rail target), the servo rail is only available with the DC jack supplying a
higher input. With USB-only power the board can boot, log to SD, play audio, and
blink LEDs, but **servos will not move**. This is intentional: USB is the
programming/debug path, not the power source for motion.

### Status LEDs (rail indicators)
- **LED1 (red, 0603)** — on whenever the 3.3 V rail is live. Powered through R4 (5.1 kΩ) from 3.3 V.
- **LED6 (green, 0603)** — on whenever the 5 V-SERVO rail is enabled. Powered through R18 (1.2 kΩ) from 5V-SERVO. Doubles as a visual confirmation that GPIO8 (SERVO_EN) is asserted.
- **LED7 (0603)** — on whenever the 5 V rail is live. Powered through R19 (1.2 kΩ) from 5 V.

## Microcontroller + wireless

**Espressif ESP32-S3-WROOM-1-N8** (U1).

- Dual-core Xtensa LX7 @ 240 MHz, 512 KB SRAM, 8 MB external flash on-module, no PSRAM.
- 2.4 GHz Wi-Fi (802.11 b/g/n) + Bluetooth 5 LE — onboard PCB antenna.
- Powered from 3.3 V (U2 LDO output).
- **Reset path**: RESET button SW2 → EN pin (U1.3), with R5 (5.1 kΩ) pull-up to 3.3 V and C5 (1 µF) debounce/POR cap.
- **Boot/download path**: BOOT button SW1 → GPIO0. Hold during reset for download mode (relies on ESP32's internal pull-up on GPIO0).
- **Native USB**: GPIO19/GPIO20 → USB-C D−/D+ (no USB-to-UART bridge IC — the ESP32-S3 has a built-in full-speed USB controller and supports USB-CDC and USB-DFU directly).
- **USB-C config**: R1 + R2 (5.1 kΩ each) on CC1 and CC2 to GND → device advertises as a USB sink (no PD negotiation, accepts default 5 V).

See `journal/topics/esp32-pinout.md` for the full GPIO map.

## Local storage — microSD

**Push-push microSD socket (CARD1)** wired for SPI mode.

| Signal | ESP32 GPIO | Notes |
|--------|-----------|-------|
| SCLK | GPIO35 | Series-terminated through R6 (22 Ω) for signal integrity. |
| MOSI | GPIO36 | |
| MISO | GPIO45 | |
| CS   | GPIO37 | |
| CD (card detect) | GPIO48 | Mechanical detect switch on the socket. |

**RN1** and **RN2** are 47 kΩ 4-resistor arrays (0603-8P) providing pull-ups to
3.3 V on SCLK, MOSI, MISO, CS, and CD per SD-card best practice.

The socket runs the standard SD-over-SPI protocol. The ESP32-S3 also supports
1-bit and 4-bit SDIO host modes, but the current wiring is SPI only — switching
modes would require firmware-only changes (no rerouting needed) provided the
4-bit data pins map cleanly.

Use case: long-term logging of patient hand position / movement history and
hand-tracking data from the companion device. Medical data must stay on-device,
so no cloud sync.

## Motor driver — 5 servos

Standard hobby-servo control: one PWM channel per finger, 50 Hz, 1–2 ms pulse
width range.

- **5 × 3-pin 2.54 mm pin headers (H1–H5)** with pinout `[SIG, +5V, GND]` (pin 1
  = signal, pin 2 = 5V-SERVO, pin 3 = GND).
- **TI TXS0108E (U6)** — 8-bit bidirectional voltage-level translator in TSSOP-20.
  Shifts the ESP32's 3.3 V PWM up to 5 V for the servo signal pins. A-side
  powered from 3.3 V, B-side from 5 V. Five of the eight channels are used. OE
  is hard-tied on the PCB — firmware does not control it.
- **Series resistors R13–R17 (220 Ω each)** on each shifted output before the
  header — protection / current limiting against accidentally backfed signal
  pins.
- **TVS diodes D1–D3 (LESD5D5.0CT)** — ESD protection. D1 sits on the USB VBUS
  path; D2 and D3 sit on the USB D+/D− lines.
- **GPIO8 → SERVO_EN** controls the **TPS56637 EN pin**, allowing the firmware
  to power the entire 5 V-SERVO rail down when idle. This is the headline
  power-saving feature on the board.
- **PWM source pins**: ESP32 GPIO 4, 5, 6, 7, 15 → U6 A-side → U6 B-side → 220 Ω
  → H1–H5 signal pins.

## User nudge system — audio

I²S Class-D mono amplifier driving an external speaker.

- **MAX98357A (U7)** — TQFN-16, 3.2 W into 4 Ω, internal DAC, no I²C
  configuration needed.
- **I²S bus**: GPIO42 → DIN, GPIO2 → BCLK, GPIO1 → LRCLK.
- **Gain/slot select**: GPIO41 → 634 kΩ (R10) → SD_MODE pin. The resistor value
  sets the chip's gain mode per the datasheet's R-to-V mapping; this lets
  firmware pull GPIO41 high or low to switch between "active" and "shutdown"
  states while keeping the gain at a fixed value.
- **Speaker connector**: 2-pin 1.25 mm JST-style (CN1) carrying OUTP+/OUTN−
  (bridge-tied load).
- **Buzzer (BUZZER1, MLT-8530, 2.7 kHz magnetic)** — supplementary simple-tone
  output. Wired as:
  ```
  3.3V -- buzzer coil -- Q2 drain
                          Q2 source -- GND
                          Q2 gate <-- 470 Ω (R8) <-- GPIO38
                          1N4148 (D6) flyback diode across buzzer coil
  ```
  This is a low-side N-MOSFET driver — GPIO38 high turns the buzzer on. The
  flyback diode (cathode on 3.3 V, anode on the drain node) absorbs the coil's
  inductive kickback.

## User nudge system — visual

> ⚠️ **Non-functional on PCB1 — WS2812B power pins reversed in the schematic**
> (VDD pad tied to GND, VSS pad tied to +5 V, on all four LEDs; data pins are
> correct). The LEDs are reverse-powered and dead, and the reversal also makes
> U3 overheat (it fights the LED's reversed-rail-powered ESD clamp). Not
> reworked on this board; one-line schematic fix for the next revision. See
> `journal/decisions/001-ws2812b-power-pin-reversal.md`.

- **4 × WS2812B RGB LEDs (LED2–LED5)** — daisy-chained addressable LEDs (the
  classic "NeoPixel"). 5 V power, GND, DIN in, DOUT out.
- **SN74LV1T34 (U3)** — single-bit non-inverting buffer / level translator in
  SC-70-5. Shifts the ESP32's 3.3 V data signal up to 5 V for the first LED's
  DIN pin. WS2812Bs nominally want V_IH ≥ 0.7 × VDD = 3.5 V, so direct 3.3 V
  drive is marginal — this buffer makes the timing reliable.
- **GPIO39 → NEO net → U3 input → LED2.DIN → … → LED5.DOUT**.

## Sources

- `production/Netlist_Schematic1_2026-06-02.tel` — connectivity ground truth
- `production/BOM_Board1_PCB1_2026-06-25.csv` — parts list
- `datasheets/esp32.png` — ESP32-S3-WROOM module pinout
