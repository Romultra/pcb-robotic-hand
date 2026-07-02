# User Nudge System

The nudge system has two parallel paths: **audio** (richer prompts, exercise
guidance) and **visual** (low-power background indication, status feedback).

## Audio

Two devices share the audio role at very different fidelities:

| Device | Use | Drive path |
|--------|-----|-----------|
| **MAX98357A** I²S Class-D mono amp (U7) → external speaker via CN1 | Spoken prompts, melodic alerts, full-quality audio | I²S from ESP32: GPIO42 DIN, GPIO2 BCLK, GPIO1 LRCLK. GPIO41 controls SD_MODE / gain through a 634 kΩ resistor (R10). Powered from the 5 V rail. |
| **MLT-8530** 2.7 kHz piezo buzzer (BUZZER1) | Cheap "blip" alerts (button press, reminder beep) without spinning up the I²S amp | GPIO38 → 470 Ω (R8) → 2N7002 N-FET (Q2). Low-side switch from 3.3 V; flyback diode D6 (1N4148) across the coil. |

Having both lets firmware decide between a "wake the patient" full-audio
reminder and a passive low-power chirp.

The validation firmware drives the MAX98357A two ways: a handful of synthesised
preset tones, and **16-bit PCM WAV files streamed off the microSD card**. File
playback is confined to a dedicated `/audio` folder so it can never read
arbitrary card contents, and the webapp can list that folder over BLE. See
`journal/decisions/005-sd-wav-audio-playback.md`.

## Visual

> ⚠️ **Non-functional on PCB1.** The WS2812B supply pins are reversed in the
> schematic (VDD pad → GND, VSS pad → +5 V) on all four LEDs; data pins are
> correct. The LEDs are reverse-powered and dead, and the reversal drives U3
> hot. Left as-is on this board (rework not worth it); trivial schematic fix for
> the next revision. See `journal/decisions/001-ws2812b-power-pin-reversal.md`.

- **4 × WS2812B RGB LEDs** (LED2–LED5) daisy-chained, addressable.
- ESP32 GPIO39 drives the chain through an **SN74LV1T34 (U3)** single-bit
  buffer that level-shifts 3.3 V → 5 V. (WS2812Bs nominally require V_IH ≥ 0.7
  × V_DD = 3.5 V at 5 V supply; direct 3.3 V drive is marginal.)
- Three additional **status indicator LEDs** tied directly to power rails (not
  ESP32-controlled): LED1 = 3.3 V live, LED6 = 5V-SERVO live, LED7 = 5 V live.

The four addressable LEDs are intended for patient-facing patterns (per-finger
animation, exercise progress, idle-state heartbeat). The status LEDs are for
the developer/clinician — they confirm the power tree is healthy at a glance.

See `board-architecture.md` for the full nudge subsystem wiring.
