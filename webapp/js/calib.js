// Per-finger servo calibration + safety limits.
//
// Standard hobby servos are absolute-position devices: a given angle command
// always drives to the same physical spot, every power cycle. So there is no
// homing step. We only need to learn, and store on the board (NVS), two things
// per finger:
//   - the motor's usable travel (jog the servo to where it physically stops),
//   - the safe finger range once the rope is on (the most-open and most-closed
//     angles that don't over-pull the mechanism).
// The firmware clamps every command (sliders, frames, playback) to the safe
// range, so a calibrated finger can't be driven past it. As long as the ropes
// aren't repositioned, the stored values stay valid across power cycles.
//
// Suggested flow (also shown on the panel):
//   1. DC barrel jack in, press "Power ON".
//   2. Tick "Calibration mode" so jogging ignores the saved limits.
//   3. Rope slack: jog each finger to find the motor's full travel (range shown).
//   4. Rope on: jog to the safe most-open pose -> "Set OPEN", to the safe
//      most-closed pose -> "Set CLOSED".
//   5. "Save to board". Untick calibration mode to enforce the limits.
//   "Power OFF" cuts the servo rail and is the emergency stop.

import { ble, UUID_SERVO_CAL, UUID_SERVO_POWER } from "./ble.js";
import { log } from "./log.js";
import { calState, getCal, getAll, setCal } from "./calstate.js";

const FINGERS = ["Thumb", "Index", "Middle", "Ring", "Pinky"];
const N = FINGERS.length;

// Command opcodes — must match CbServoCal in firmware/src/ble.cpp.
const OP_MODE = 0x01, OP_JOG = 0x02, OP_GOTO = 0x03, OP_CAPTURE = 0x04,
      OP_SAVE = 0x05, OP_RESET = 0x06, OP_OPEN = 0x07, OP_READ = 0x08;

const STORE_KEY = "servo_calib";

// The calibration band + live position live in calstate.js (shared with the
// Servos panel). Min/max servo angle seen while jogging stays local here — it's
// the measured motor travel, shown next to each finger.
let seen = Array.from({ length: N }, () => ({ min: null, max: null }));

let els = {};
let wired = false;   // bind DOM listeners once, even across reconnects

function setStatus(msg) {
    if (els.status) els.status.textContent = msg;
}

// --- Sending ----------------------------------------------------------------
async function send(bytes, note) {
    if (!ble.connected) {
        setStatus("Connect to the board first.");
        log(`calib: not connected (${note || "command"} ignored)`);
        return;
    }
    try {
        await ble.writeNoResponse(UUID_SERVO_CAL, new Uint8Array(bytes));
        if (note) log(`calib: ${note}`);
    } catch (e) {
        log(`calib write failed: ${e.message}`);
        setStatus(`Write failed: ${e.message}`);
    }
}

function jog(ch, deltaX10) {
    const u = deltaX10 & 0xffff;          // two's-complement 16-bit, little-endian
    const d = deltaX10 / 10;
    send([OP_JOG, ch, u & 0xff, (u >> 8) & 0xff],
         `${FINGERS[ch]} jog ${d > 0 ? "+" : ""}${d}°`);
}
function capture(ch, which) {
    send([OP_CAPTURE, ch, which],
         `${FINGERS[ch]} set ${which === 0 ? "OPEN" : "CLOSED"} = ${Math.round(getCal(ch).cur)}°`);
}
function readAll()     { send([OP_READ, 0xff], "read calibration from board"); }
function saveToBoard() { send([OP_SAVE], "saved calibration to board (NVS)"); }
function resetBoard()  { send([OP_RESET], "reset calibration on board"); }
function setMode(on)   { send([OP_MODE, on ? 1 : 0], `calibration mode ${on ? "ON" : "OFF"}`); }
function openAll() {
    // Firmware only moves calibrated channels, so this is a no-op until at least
    // one finger has open/closed set — say so instead of looking dead.
    if (!getAll().some((t) => t.valid)) {
        setStatus("Move to open: no fingers calibrated yet — set OPEN/CLOSED first.");
        log("calib: move-to-open ignored (nothing calibrated)");
        return;
    }
    send([OP_OPEN, 0xff], "move all fingers to open");
    refreshAfterRamp();
}

async function setPower(on) {
    if (!ble.connected) { setStatus("Connect to the board first."); return; }
    try {
        await ble.writeNoResponse(UUID_SERVO_POWER, new Uint8Array([on ? 1 : 0]));
        log(`calib: servo rail ${on ? "ON" : "OFF"}`);
        setStatus(on
            ? "Servo rail ON. (Needs the DC barrel jack to actually move.)"
            : "Servo rail OFF (stopped).");
    } catch (e) {
        log(`servo power failed: ${e.message}`);
    }
}

// A move-to-open ramp finishes after the command returns, so pull the final
// position a moment later to refresh the readout.
function refreshAfterRamp() {
    setTimeout(readAll, 250);
    setTimeout(readAll, 800);
}

// --- Receiving --------------------------------------------------------------
function onNotify(dv) {
    if (dv.byteLength < 11) return;
    const ch = dv.getUint8(1);
    if (ch >= N) return;
    const flags = dv.getUint8(8);
    const cur = dv.getUint16(9, true) / 10;
    // Track the motor travel actually reached (before setCal so the render the
    // update fires sees the latest "seen" range).
    const s = seen[ch];
    s.min = s.min === null ? cur : Math.min(s.min, cur);
    s.max = s.max === null ? cur : Math.max(s.max, cur);

    setCal(ch, {
        open:   dv.getUint16(2, true) / 10,
        closed: dv.getUint16(4, true) / 10,
        valid:  (flags & 0x01) !== 0,
        cur,
    }, "calib");   // notifies calstate -> renders this row + updates the slider

    applyMode((flags & 0x02) !== 0);
    persist();
}

function persist() {
    const slim = getAll().map((t) => ({ open: t.open, closed: t.closed, valid: t.valid }));
    localStorage.setItem(STORE_KEY, JSON.stringify(slim));
}

// --- UI ---------------------------------------------------------------------
function buildRows() {
    els.rows.innerHTML = "";
    for (let i = 0; i < N; ++i) {
        const row = document.createElement("div");
        row.className = "cal-row";
        row.dataset.ch = i;
        row.innerHTML = `
            <span class="cal-name">${FINGERS[i]}</span>
            <span class="cal-cur" data-ch="${i}">—</span>
            <span class="cal-jog">
                <button data-ch="${i}" data-delta="-50">−5°</button>
                <button data-ch="${i}" data-delta="-10">−1°</button>
                <button data-ch="${i}" data-delta="10">+1°</button>
                <button data-ch="${i}" data-delta="50">+5°</button>
            </span>
            <span class="cal-cap">
                <button data-ch="${i}" data-which="0">Set OPEN</button>
                <button data-ch="${i}" data-which="1">Set CLOSED</button>
            </span>
            <span class="cal-band" data-ch="${i}">—</span>
        `;
        els.rows.appendChild(row);
    }
}

function renderRow(ch) {
    const t = getCal(ch);
    const s = seen[ch];
    const cur = els.rows.querySelector(`.cal-cur[data-ch="${ch}"]`);
    const band = els.rows.querySelector(`.cal-band[data-ch="${ch}"]`);
    if (cur) cur.textContent = `${Math.round(t.cur)}°`;
    if (band) {
        const safe = t.valid
            ? `safe ${Math.round(t.open)}–${Math.round(t.closed)}°`
            : "uncalibrated";
        const travel = s.min === null ? "" : ` · seen ${Math.round(s.min)}–${Math.round(s.max)}°`;
        band.textContent = safe + travel;
        band.classList.toggle("cal-uncal", !t.valid);
    }
}

function renderAll() {
    for (let i = 0; i < N; ++i) renderRow(i);
}

// --- Mode: who controls the motors -----------------------------------------
// Calibration mode hands control to this panel: jog/capture are active here only
// in calibration mode (you can't usefully calibrate with the limits enforced),
// and the manual Servos panel is locked so the motors are only driven from one
// place. Normal mode is the reverse. The board's notify is authoritative, so
// applyMode is driven from onNotify; the toggle handler applies it optimistically.
function setCalControlsEnabled(on) {
    els.rows.querySelectorAll(".cal-jog button, .cal-cap button").forEach((b) => { b.disabled = !on; });
}
function setManualPanelEnabled(on) {
    const panel = document.getElementById("panel-servos");
    if (!panel) return;
    panel.classList.toggle("panel-locked", !on);
    panel.querySelectorAll("input, button").forEach((el) => { el.disabled = !on; });
    const tag = document.getElementById("servo-locked");
    if (tag) tag.hidden = on;
}
function applyMode(calMode) {
    if (els.mode) els.mode.checked = calMode;
    setCalControlsEnabled(calMode);
    setManualPanelEnabled(!calMode);
}

export async function initCalib() {
    els = {
        rows:   document.getElementById("cal-rows"),
        status: document.getElementById("cal-status"),
        mode:   document.getElementById("cal-mode"),
        btnOn:    document.getElementById("btn-cal-power-on"),
        btnOff:   document.getElementById("btn-cal-power-off"),
        btnOpen:  document.getElementById("btn-cal-open-all"),
        btnRead:  document.getElementById("btn-cal-read"),
        btnSave:  document.getElementById("btn-cal-save"),
        btnReset: document.getElementById("btn-cal-reset"),
    };
    if (!els.rows) return;   // panel not present

    buildRows();
    renderAll();
    applyMode(false);   // default until the board's READ reply confirms the mode

    if (!wired) {
        // A jog click sends one bounded BLE write; binding once avoids a
        // reconnect doubling clicks (a +5° turning into +10° mid-calibration).
        els.rows.addEventListener("click", (ev) => {
            const b = ev.target;
            if (b.tagName !== "BUTTON") return;
            const ch = parseInt(b.dataset.ch, 10);
            if (b.dataset.delta !== undefined) {
                jog(ch, parseInt(b.dataset.delta, 10));
            } else if (b.dataset.which !== undefined) {
                capture(ch, parseInt(b.dataset.which, 10));
            }
        });
        if (els.mode) {
            els.mode.addEventListener("change", () => {
                const on = els.mode.checked;
                setMode(on);
                applyMode(on);   // optimistic; onNotify confirms from the board
                if (on) seen = Array.from({ length: N }, () => ({ min: null, max: null }));
                setStatus(on
                    ? "Calibration mode ON — control here, limits off, Servos panel locked."
                    : "Calibration mode OFF — limits enforced, Servos panel unlocked.");
            });
        }
        // Re-render a finger row whenever its shared state changes — from a
        // SERVO_CAL notify here or from the Servos panel moving the same channel.
        calState.addEventListener("update", (e) => renderRow(e.detail.ch));
        // If the link drops, unlock the manual panel so it's usable next time.
        ble.addEventListener("disconnected", () => setManualPanelEnabled(true));
        if (els.btnOn)    els.btnOn.addEventListener("click", () => setPower(true));
        if (els.btnOff)   els.btnOff.addEventListener("click", () => setPower(false));
        if (els.btnOpen)  els.btnOpen.addEventListener("click", openAll);
        if (els.btnRead)  els.btnRead.addEventListener("click", readAll);
        if (els.btnSave)  els.btnSave.addEventListener("click", saveToBoard);
        if (els.btnReset) els.btnReset.addEventListener("click", resetBoard);
        wired = true;
    }

    await ble.subscribeNotify(UUID_SERVO_CAL, onNotify);
    await readAll();   // populate the table from the board
}
