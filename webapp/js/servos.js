import { ble, UUID_SERVO_POWER, UUID_SERVO_SET, UUID_SERVO_FRAME } from "./ble.js";
import { log } from "./log.js";
import { calState, getCal, normToDeg, degToNorm, setCal } from "./calstate.js";

const N = 5;

// Apply all 5 servo angles in a single BLE write. `angles5` is an array of 5
// numbers in degrees (0..180); out-of-range values are clamped. Shared by the
// live-mirror and playback paths (tracking.js / playback.js) so every multi-
// servo frame goes through one place. Throws if the write fails — callers that
// stream frames should swallow/throttle on error.
export async function sendServoFrame(angles5) {
    const buf = new Uint8Array(N);
    for (let i = 0; i < N; ++i) {
        buf[i] = Math.max(0, Math.min(180, Math.round(angles5[i] ?? 90)));
    }
    await ble.writeNoResponse(UUID_SERVO_FRAME, buf);
}

// The sliders work in normalized finger space: 0 % = open (left), 100 % = closed
// (right), so the full slider always spans that finger's usable range whatever
// its calibrated angles or flipped linkage. calstate.js maps % <-> raw degrees;
// an uncalibrated channel falls back to a straight 0..180°.
function buildSliders() {
    const host = document.getElementById("servo-sliders");
    host.innerHTML = "";
    for (let i = 0; i < N; ++i) {
        const row = document.createElement("div");
        row.className = "servo-row";
        row.innerHTML = `
            <label>S${i + 1}</label>
            <input type="range" min="0" max="100" value="0" data-ch="${i}" />
            <span class="val" data-ch="${i}">0%</span>
        `;
        host.appendChild(row);
    }
}

function setVal(ch, pct) {
    const v = document.querySelector(`.servo-row .val[data-ch="${ch}"]`);
    if (v) v.textContent = `${pct}%`;
}

// Place a slider (and its label) at pct without sending — used to reflect the
// live board position. Setting input.value doesn't fire "input", so no loop.
function setSlider(ch, pct) {
    const inp = document.querySelector(`.servo-row input[data-ch="${ch}"]`);
    if (inp) inp.value = pct;
    setVal(ch, pct);
}

let pending = null;
let inflight = false;
async function onInput(ev) {
    const inp = ev.target;
    if (inp.tagName !== "INPUT") return;
    const ch = parseInt(inp.dataset.ch, 10);
    const pct = parseInt(inp.value, 10);
    setVal(ch, pct);
    const deg = normToDeg(ch, pct / 100);
    setCal(ch, { cur: deg }, "servo");   // keep the calib readout in sync
    pending = { ch, deg };
    if (inflight) return;
    inflight = true;
    while (pending) {
        const p = pending; pending = null;
        await sendOne(p.ch, p.deg);
    }
    inflight = false;
}

// Send one raw angle (degrees) over SERVO_SET. The firmware clamps it to the
// channel's calibrated band, so playback/sliders can't exceed it.
async function sendOne(ch, deg) {
    const degx10 = Math.max(0, Math.min(1800, Math.round(deg * 10)));
    const buf = new Uint8Array(3);
    buf[0] = ch;
    buf[1] = degx10 & 0xff;
    buf[2] = (degx10 >> 8) & 0xff;
    try {
        await ble.writeNoResponse(UUID_SERVO_SET, buf);
    } catch (e) {
        log(`servo write failed: ${e.message}`);
    }
}

async function setPower(on) {
    try {
        await ble.writeNoResponse(UUID_SERVO_POWER, new Uint8Array([on ? 1 : 0]));
        log(`Servo rail ${on ? "ON" : "OFF"}`);
    } catch (e) {
        log(`servo_power failed: ${e.message}`);
    }
}

async function setAllPct(pct) {
    for (let i = 0; i < N; ++i) {
        setSlider(i, pct);
        const deg = normToDeg(i, pct / 100);
        setCal(i, { cur: deg }, "servo");
        await sendOne(i, deg);
    }
}

async function openCloseMid() {
    await setAllPct(50);   // halfway through every finger's range
}

async function sweep() {
    for (const p of [0, 50, 100, 50]) {   // open -> mid -> closed -> mid
        await setAllPct(p);
        await new Promise(r => setTimeout(r, 500));
    }
}

// Reflect the live board position on the sliders. Fires when a SERVO_CAL notify
// (jog, read, move-to-open) updates a channel; we skip our own "servo" echoes so
// dragging a slider isn't fought. Works even while the panel is locked during
// calibration, so the grayed sliders still show the finger moving.
calState.addEventListener("update", (e) => {
    if (e.detail.source === "servo") return;
    const ch = e.detail.ch;
    setSlider(ch, Math.round(degToNorm(ch, getCal(ch).cur) * 100));
});

let wired = false;   // bind DOM listeners once, even across reconnects

export function initServos() {
    buildSliders();
    for (let i = 0; i < N; ++i) {
        setSlider(i, Math.round(degToNorm(i, getCal(i).cur) * 100));
    }
    if (wired) return;
    document.getElementById("servo-sliders").addEventListener("input", onInput);
    document.getElementById("btn-servo-on").addEventListener("click", () => setPower(true));
    document.getElementById("btn-servo-off").addEventListener("click", () => setPower(false));
    document.getElementById("btn-servo-centre-all").addEventListener("click", openCloseMid);
    document.getElementById("btn-servo-sweep").addEventListener("click", sweep);
    wired = true;
}
