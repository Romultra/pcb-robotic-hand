# 001: WS2812B Power-Pin Reversal (PCB1) — LEDs Left As-Is

**Date:** 2026-06-08
**Status:** Decided

## Context

During first bring-up of PCB1, the four patient-facing WS2812B RGB LEDs
(LED2–LED5, the visual-nudge chain on GPIO39 via the SN74LV1T34 level-shifter
U3) would not light. Two assembled boards from the same fabrication batch
behaved identically.

Investigation revealed a **schematic wiring error**: on all four addressable
LEDs the **supply pins are swapped** — the **VDD pad is tied to GND** and the
**VSS pad is tied to the +5 V rail**. The part is a Worldsemi **WS2812B-B/T**
(4-pin 5050), whose datasheet pinout is:

| Pin | Function | Should connect to | Actually wired to (PCB1) |
|----:|----------|-------------------|--------------------------|
| 1 | VDD  | +5 V | **GND** ✗ |
| 2 | DOUT | next DIN | next DIN ✓ |
| 3 | VSS (GND) | GND | **+5 V** ✗ |
| 4 | DIN  | prev DOUT / U3 out | prev DOUT / U3 out ✓ |

The **data pins are correct**; only the **power pins are reversed**. The error
is in the schematic symbol-to-net mapping and is therefore identical on every
board (authoritative source: `production/Netlist_Schematic1_2026-06-02.tel` —
`LED*.1 ∈ GND`, `LED*.3 ∈ 5V`).

### Why this produced the observed symptoms

With the IC powered backwards (VDD pad at 0 V, VSS pad at +5 V):

- **The LED is reverse-biased and cannot function.** Its internal
  ESD/substrate diode network between VSS and VDD is forward-biased by the
  swapped rails, so the part conducts a fault current from 5 V to GND instead of
  operating, and never illuminates.
- **U3 overheats.** The WS2812B data input's ESD clamps now reference the
  swapped rails, so the +5 V sitting on the VSS pad sources current into the
  data node through the LED's internal protection. This holds the line up: when
  U3 drives its input high the output reaches a clean 5 V, but when the input
  goes to 0 V the output only falls to ~3 V — U3 cannot pull below the clamp and
  dissipates heavily fighting it, so it runs hot. In the slow toggle test the
  output therefore swings **5 V ↔ ~3 V** as the input swings 3.3 V ↔ 0 V, rather
  than 5 V ↔ 0 V. U3 itself is not defective; it is being abused by the
  downstream miswire.

This reconciles every bring-up observation: LEDs dark, "5 V present on the LED
pins" (it is — on the wrong pad), U3 hot, output only able to fall to ~3 V
(swinging 5 V↔3 V, not 5 V↔0 V), and identical failure on both boards.

### Debugging path (for the thesis methodology section)

The fault was isolated by elimination, not inspection: verified the U3 pinout
and netlist against the TI datasheet (correct); confirmed the level-shifter
input toggled cleanly 0↔3.3 V while its output only fell to ~3 V at the low end
(never reaching 0 V) and ran hot; considered and rejected a floating-input
shoot-through (GPIO39/MTCK is
pulled-up at reset, not floating), a missing local decoupling cap, and a
missing series resistor. Only after narrowing the fault to "the output is being
held by the only thing on that net — the LED" did inspection of the LED supply
connections reveal the reversal. Lesson: the symptom (hot buffer, clamped
output) was a *secondary effect* of a downstream power miswire, several layers
removed from the obvious suspect.

## Options Considered

1. **Rework the existing PCB1 boards to make the LEDs functional.**
   The only field fix for already-fabricated boards is to **rotate each LED
   180°** in-plane. On a 4-pad 5050, a 180° rotation swaps pads 1↔3 (fixing
   VDD/VSS) *and* 2↔4 (swapping DIN/DOUT), so the data flow through each device
   and through the chain reverses. The data input from U3 would then have to be
   **rerouted to the other end of the chain** (the LEDs are laid out in a square
   loop, so that end sits right next to the start), with the chain propagating
   LED5→LED4→LED3→LED2.
   - *Pros:* restores the visual-nudge LEDs on the existing prototype.
   - *Cons:* requires rotating four **tightly packed** 5050 packages and
     cutting/rerouting the data trace by hand. High-effort, high-risk rework for
     a feature that is **not essential to this board's purpose** (electrical
     validation of the wiring/layout/component choices). The audio nudge path
     and the rail-status LEDs are unaffected.

2. **Leave the LEDs non-functional on PCB1; document the error; fix in the next
   revision.**
   - *Pros:* zero rework risk; the error and its fix are fully understood and
     captured. The next-rev fix is **trivial** — correct the schematic so the
     VDD pad maps to 5 V and the VSS pad to GND (no layout penalty, no part
     change).
   - *Cons:* the addressable visual-nudge feature is unvalidated on this board
     (mitigated: the design intent and corrected wiring are documented, so it is
     understood to be correct-by-design for the next spin).

## Decision

**Chosen: Option 2.** The WS2812B LEDs are left non-functional on PCB1. The
180° rotation + data reroute is technically valid and *would* restore the LEDs,
but the soldering effort on the tightly-packed array is not worth it merely to
exercise a non-critical feature on a validation board. The error is documented
here and is a one-line schematic correction for the next revision.

Action items for the next revision:
- Correct the WS2812B supply nets: **VDD pad → 5 V, VSS pad → GND.**
- While in the symbol, also revisit the previously-noted robustness
  improvements for the LED data path: a **100–330 Ω series resistor** at U3's
  output and a **0.1 µF decoupling cap** at U3's VCC (pin 5) — these are not the
  cause of this failure but are good practice for a WS2812B data line.

## Sources

- `production/Netlist_Schematic1_2026-06-02.tel` — confirms `LED*.1 ∈ GND`, `LED*.3 ∈ 5V` (reversed).
- `production/BOM_Board1_PCB1_2026-06-25.csv` — LED2–LED5 = WS2812B-B/T (Worldsemi, LCSC C2761795).
- WS2812B-B/T datasheet — 4-pin pinout (1 = VDD, 2 = DOUT, 3 = VSS, 4 = DIN).
- Bring-up observations, 2026-06-08 (see `journal/log.md`).
