// Shared per-finger servo state: the calibrated [open,closed] band and the last
// commanded position, in raw servo degrees. calib.js fills it from SERVO_CAL
// notifies; servos.js updates it when a normalized slider commands a position.
// Both panels read it so the calibration readout and the Servos sliders stay in
// sync, and the sliders can show the live position.

const N = 5;
const table = Array.from({ length: N }, () => ({ open: 0, closed: 180, valid: false, cur: 90 }));

export const calState = new EventTarget();

export function getCal(ch) { return table[ch]; }
export function getAll() { return table; }

// Merge fields into a channel and notify listeners. `source` lets a listener
// skip echoes it caused itself (e.g. the slider that triggered the change).
export function setCal(ch, data, source = "") {
    if (ch < 0 || ch >= N) return;
    Object.assign(table[ch], data);
    calState.dispatchEvent(new CustomEvent("update", { detail: { ch, source } }));
}

// Normalized finger position (0 = open, 1 = closed) -> raw servo degrees, using
// the channel's band. Handles a flipped linkage (open may be > closed). Falls
// back to a straight 0..180° when the channel isn't calibrated yet.
export function normToDeg(ch, t) {
    const c = table[ch];
    t = Math.max(0, Math.min(1, t));
    return c.valid ? c.open + t * (c.closed - c.open) : t * 180;
}

// Raw servo degrees -> normalized 0..1, for placing a slider at the live position.
export function degToNorm(ch, deg) {
    const c = table[ch];
    const base = c.valid ? c.open : 0;
    const span = c.valid ? (c.closed - c.open) : 180;
    if (span === 0) return 0;
    return Math.max(0, Math.min(1, (deg - base) / span));
}
