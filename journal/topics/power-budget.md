# Power Budget

The first PCB revision is **wall-powered**, not battery-powered. The original
"~1 week autonomy" target from the PDR was deferred: the BOM contains no
Li-ion charger, no fuel gauge, and no battery connector. Inputs are either the
DC barrel jack or USB-C.

This page records the current rail-level power tree so a future battery
revision has a starting point.

## Power tree summary

```
DC jack (DC1) --[Q1 rev-prot. P-FET]--> VIN ---+--> TPS56637 (U4) ---> 5V-SERVO  [EN ← GPIO8]
                                                |        \
                                                |         L1 = 3.3 µH
                                                |
                                                +--> TPS54202 (U8) ---> 5V-IN ---+
                                                         \                       |  (Q3 P-FET, gated by VBUS)
                                                          L2 = 15 µH             |
                                                                                 v
USB-C VBUS ---[D8 Schottky]------------------------------------------------> 5V rail ---> AP2114H (U2) ---> 3.3V
                                                                                  |
                                                                                  +--> MAX98357A, WS2812B ×4, etc.
```

USB takes priority when plugged in (forward-biased D8); the Q3 P-FET isolates
TPS54202's output so the buck doesn't backfeed when USB is present.

## Per-rail loads (rough, design-time estimates — not measured)

| Rail | Voltage | Source | Loads | Notes |
|------|---------|--------|-------|-------|
| 5V-SERVO | ~5 V | TPS56637 (U4), enable-gated | 5 hobby servos | Dominant load when active. Peak current depends on servo choice and how many move simultaneously — TPS56637 is good for 6 A continuous, well above any plausible servo demand. |
| 5 V | ~5 V | TPS54202 (U8) **or** USB VBUS via D8 | MAX98357A (audio peaks ~600 mA into 4 Ω at full output), 4× WS2812B (up to ~60 mA each at full white), AP2114H LDO input | TPS54202 rated for 2 A continuous, easily covers worst case. |
| 3.3 V | 3.3 V | AP2114H LDO (U2) | ESP32-S3 (peak ~500 mA during radio TX), microSD (peak ~100 mA on write), TXS0108E A-side, buzzer driver, status LED | AP2114H rated 1 A; ESP32's typical average is ~80–150 mA with Wi-Fi active. |

## Measured rails (PCB1, multimeter, 2026-06-25)

The loads above are design-time estimates. The rail voltages below are measured.

| Rail | Nominal | Measured | Notes |
|------|---------|----------|-------|
| 3.3 V | 3.3 V | **3.29 V** | AP2114H LDO output, on target. |
| 5V-SERVO | 5 V | **4.99 V** | TPS56637 output. Barrel-jack power only (step-down). |
| 5 V logic (USB) | 5 V | **4.86 V** | From VBUS 5.01 V through D8 Schottky → **0.15 V** drop (normal Schottky Vf). |
| 5 V logic (barrel jack) | 5 V | **4.44 V** | From 5V-IN buck 4.95 V through Q3 → **0.51 V** drop across the DMG3415U D-S. |

The 0.51 V drop on the barrel-jack path is the one anomaly. It is the magnitude
of a forward silicon junction, not the few-mV drop a fully-enhanced pass FET
should give, so the current appears to be taking Q3's body diode rather than its
channel on this path. The netlist has Q3 wired gate=VBUS (R9 pulldown to GND),
source=5V rail, drain=5V-IN, so with USB unplugged the gate is at 0 V and the
channel *should* turn on, which leaves the diode-sized drop unexplained from the
schematic alone. It does not affect operation (the 4.44 V rail still feeds a clean
3.29 V from the LDO and is inside the audio amp's range), so the cause was not run
down on the bench. Reducing it is a next-revision recommendation (Ch 6 §6.6).

## Battery-future hooks

If a battery revision happens, the cleanest path is:

1. Add a Li-ion charger (e.g. MCP73831 or BQ25180) between USB VBUS and a
   single-cell Li-ion pack.
2. Route the battery + into a load switch and into the same VIN node currently
   fed by the DC jack — the existing TPS56637 / TPS54202 buck pair already
   handles 3.7 V Li-ion input (both are wide-input).
3. Reuse GPIO8 (SERVO_EN) as the master "wake servos for an exercise session"
   gate — it already exists for this purpose.
4. Decide whether to keep the DC jack as an alternative input or drop it.

The "1 week autonomy" target probably implies the patient's pattern of one
~30 min exercise session per day with the servos otherwise off — i.e. servos
duty-cycled near 2 %, ESP32 in modem-sleep most of the time, and the I²S amp +
LEDs off between cues. Numbers and battery sizing left to a future power-budget
worksheet once measurements are available.
