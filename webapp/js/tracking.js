// Browser hand-tracking. Runs MediaPipe HandLandmarker on the webcam, turns the
// 21 landmarks into 5 continuous per-finger flexion angles (0..180°), and feeds
// them to:
//   - a live on-screen readout,
//   - the board in real time ("live mirror" toggle) via sendServoFrame,
//   - a recorded clip (handed to the clip store in playback.js).
//
// This replaces the previous prototype's Python cvzone + binary fingersUp()
// pipeline (see previous-prototype-code/). The MediaPipe runtime + model are
// dynamically imported from the jsdelivr CDN on first camera start, so the page
// needs no build step and index.html stays a plain ES-module page.

import { ble } from "./ble.js";
import { log } from "./log.js";
import { sendServoFrame } from "./servos.js";
import { addClip } from "./playback.js";

// Per-frame timing for the latency panel. tick() dispatches a "sample" event
// with the MediaPipe inference time and the wall-clock gap since the previous
// processed frame; latency.js listens. Decoupled from the camera lifecycle, so
// it just goes quiet when tracking isn't running.
export const trackingStats = new EventTarget();

// --- MediaPipe landmark indices (MCP, PIP/IP, TIP) per finger ---------------
// Angle at the middle joint: ~180° when the finger is straight, smaller as it
// curls. Thumb uses MCP-IP-TIP (2-3-4); the others use MCP-PIP-TIP.
const FINGERS = [
    { name: "Thumb",  joints: [2, 3, 4] },
    { name: "Index",  joints: [5, 6, 8] },
    { name: "Middle", joints: [9, 10, 12] },
    { name: "Ring",   joints: [13, 14, 16] },
    { name: "Pinky",  joints: [17, 18, 20] },
];
const N = FINGERS.length;

// --- Calibration ------------------------------------------------------------
// Per finger we store the curl metric (joint angle, degrees) at the "open"
// (extended) and "closed" (fully curled) poses, plus an invert flag for fingers
// whose servo linkage runs the opposite way. The defaults below are sane enough
// for the live readout to do something before the user calibrates.
const CALIB_KEY = "hand_calib";
const DEFAULT_CALIB = FINGERS.map(() => ({ open: 175, closed: 90, invert: false }));

function loadCalib() {
    try {
        const raw = localStorage.getItem(CALIB_KEY);
        if (raw) {
            const c = JSON.parse(raw);
            if (Array.isArray(c) && c.length === N) return c;
        }
    } catch { /* fall through to defaults */ }
    return structuredClone(DEFAULT_CALIB);
}
function saveCalib(c) {
    localStorage.setItem(CALIB_KEY, JSON.stringify(c));
}

let calib = loadCalib();

// --- Geometry ---------------------------------------------------------------
// Angle (degrees) at point b formed by segments b→a and b→c, in 3D.
function jointAngle(a, b, c) {
    const v1 = [a.x - b.x, a.y - b.y, a.z - b.z];
    const v2 = [c.x - b.x, c.y - b.y, c.z - b.z];
    const dot = v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
    const m1 = Math.hypot(v1[0], v1[1], v1[2]);
    const m2 = Math.hypot(v2[0], v2[1], v2[2]);
    if (m1 === 0 || m2 === 0) return 180;
    const cos = Math.max(-1, Math.min(1, dot / (m1 * m2)));
    return (Math.acos(cos) * 180) / Math.PI;
}

// Raw curl metric (joint angle in degrees) for every finger from one set of
// 21 landmarks.
function curlMetrics(lm) {
    return FINGERS.map((f) => jointAngle(lm[f.joints[0]], lm[f.joints[1]], lm[f.joints[2]]));
}

// Map a finger's metric through its calibration to a 0..180° servo angle.
// t = 0 at the open pose, 1 at the closed pose; invert flips the servo sense.
function metricToAngle(metric, cal) {
    const span = cal.closed - cal.open;
    let t = span === 0 ? 0 : (metric - cal.open) / span;
    t = Math.max(0, Math.min(1, t));
    if (cal.invert) t = 1 - t;
    return t * 180;
}

// --- State ------------------------------------------------------------------
let landmarker = null;      // MediaPipe HandLandmarker
let stream = null;          // MediaStream from getUserMedia
let rafId = 0;
let running = false;

let lastAngles = new Array(N).fill(90);   // last computed servo angles
let lastMetrics = null;                    // last raw metrics (for calibration capture)

let recording = false;
let recStart = 0;
let recFrames = [];

let lastTickT = 0;          // performance.now() of the previous processed frame

let mirror = false;

// Coalescing live-mirror sender: only the most recent frame is ever in flight,
// so a slow BLE link drops intermediate frames instead of queueing them up.
let mirrorPending = null;
let mirrorInflight = false;
async function mirrorSend(angles) {
    mirrorPending = angles;
    if (mirrorInflight) return;
    mirrorInflight = true;
    while (mirrorPending) {
        const a = mirrorPending; mirrorPending = null;
        try {
            await sendServoFrame(a);
        } catch (e) {
            log(`live mirror send failed: ${e.message}`);
            mirrorPending = null;   // drop; don't spin on a dead link
            break;
        }
    }
    mirrorInflight = false;
}

// --- DOM helpers ------------------------------------------------------------
let els = {};

function buildReadout() {
    const host = els.readout;
    host.innerHTML = "";
    for (let i = 0; i < N; ++i) {
        const row = document.createElement("div");
        row.className = "servo-row";
        row.innerHTML = `
            <label>${FINGERS[i].name}</label>
            <progress data-ch="${i}" value="90" max="180"></progress>
            <span class="val" data-ch="${i}">90°</span>
        `;
        host.appendChild(row);
    }
}

function updateReadout(angles) {
    for (let i = 0; i < N; ++i) {
        const bar = els.readout.querySelector(`progress[data-ch="${i}"]`);
        const val = els.readout.querySelector(`.val[data-ch="${i}"]`);
        if (bar) bar.value = angles[i];
        if (val) val.textContent = `${Math.round(angles[i])}°`;
    }
}

function setStatus(msg) {
    if (els.status) els.status.textContent = msg;
}

// --- MediaPipe loading ------------------------------------------------------
const MP_VERSION = "0.10";
const MODEL_URL =
    "https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task";

async function ensureLandmarker() {
    if (landmarker) return landmarker;
    setStatus("Loading MediaPipe…");
    const vision = await import(
        `https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@${MP_VERSION}/+esm`
    );
    const fileset = await vision.FilesetResolver.forVisionTasks(
        `https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@${MP_VERSION}/wasm`
    );
    landmarker = await vision.HandLandmarker.createFromOptions(fileset, {
        baseOptions: { modelAssetPath: MODEL_URL },
        runningMode: "VIDEO",
        numHands: 1,
    });
    return landmarker;
}

// --- Detect loop ------------------------------------------------------------
function drawOverlay(lm) {
    const cv = els.canvas;
    if (!cv) return;
    const v = els.video;
    if (cv.width !== v.videoWidth || cv.height !== v.videoHeight) {
        cv.width = v.videoWidth;
        cv.height = v.videoHeight;
    }
    const ctx = cv.getContext("2d");
    ctx.clearRect(0, 0, cv.width, cv.height);
    if (!lm) return;
    ctx.fillStyle = "#36c";
    for (const p of lm) {
        ctx.beginPath();
        ctx.arc(p.x * cv.width, p.y * cv.height, 4, 0, Math.PI * 2);
        ctx.fill();
    }
}

function tick() {
    if (!running) return;
    const v = els.video;
    if (v && v.readyState >= 2 && landmarker) {
        let res = null;
        const t0 = performance.now();
        try {
            res = landmarker.detectForVideo(v, t0);
        } catch (e) {
            log(`detect error: ${e.message}`);
        }
        const inferenceMs = performance.now() - t0;
        const frameIntervalMs = lastTickT ? t0 - lastTickT : 0;
        lastTickT = t0;
        trackingStats.dispatchEvent(new CustomEvent("sample", {
            detail: { inferenceMs, frameIntervalMs },
        }));
        const lm = res && res.landmarks && res.landmarks[0];
        if (lm) {
            lastMetrics = curlMetrics(lm);
            lastAngles = lastMetrics.map((m, i) => metricToAngle(m, calib[i]));
            drawOverlay(lm);
        } else {
            drawOverlay(null);   // hold lastAngles when no hand is visible
        }
        updateReadout(lastAngles);

        if (mirror && ble.connected) mirrorSend(lastAngles);

        if (recording) {
            recFrames.push({
                t: Math.round(performance.now() - recStart),
                a: lastAngles.map((x) => Math.round(x)),
            });
        }
    }
    rafId = requestAnimationFrame(tick);
}

// --- Camera control ---------------------------------------------------------
async function startCamera() {
    if (running) return;
    try {
        await ensureLandmarker();
        setStatus("Starting camera…");
        stream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: "user" } });
        els.video.srcObject = stream;
        await els.video.play();
        running = true;
        els.btnStart.disabled = true;
        els.btnStop.disabled = false;
        setStatus("Tracking.");
        log("Hand tracking started.");
        rafId = requestAnimationFrame(tick);
    } catch (e) {
        setStatus(`camera/MediaPipe failed: ${e.message}`);
        log(`tracking start failed: ${e.message}`);
    }
}

function stopCamera() {
    running = false;
    lastTickT = 0;   // so a later restart's first frame doesn't report a huge gap
    if (rafId) cancelAnimationFrame(rafId);
    rafId = 0;
    if (stream) {
        stream.getTracks().forEach((t) => t.stop());
        stream = null;
    }
    if (els.video) els.video.srcObject = null;
    drawOverlay(null);
    els.btnStart.disabled = false;
    els.btnStop.disabled = true;
    setStatus("Stopped.");
    log("Hand tracking stopped.");
}

// --- Calibration capture ----------------------------------------------------
function capture(which) {
    if (!lastMetrics) {
        setStatus("No hand detected — hold your hand in view first.");
        return;
    }
    for (let i = 0; i < N; ++i) calib[i][which] = Math.round(lastMetrics[i]);
    saveCalib(calib);
    setStatus(`Captured "${which}" pose.`);
    log(`Calibration "${which}": ${lastMetrics.map((m) => Math.round(m)).join(", ")}`);
}

function resetCalib() {
    calib = structuredClone(DEFAULT_CALIB);
    saveCalib(calib);
    setStatus("Calibration reset to defaults.");
}

function toggleInvert(ch, on) {
    calib[ch].invert = on;
    saveCalib(calib);
}

// --- Recording --------------------------------------------------------------
function startRecording() {
    if (!running) {
        setStatus("Start the camera before recording.");
        return;
    }
    recFrames = [];
    recStart = performance.now();
    recording = true;
    els.btnRec.disabled = true;
    els.btnRecStop.disabled = false;
    setStatus("Recording…");
}

function stopRecording() {
    recording = false;
    els.btnRec.disabled = false;
    els.btnRecStop.disabled = true;
    if (recFrames.length === 0) {
        setStatus("Recording empty — nothing saved.");
        return;
    }
    const dur = recFrames[recFrames.length - 1].t || 1;
    const name = (els.recName.value || "").trim() || `clip-${recFrames.length}f`;
    const clip = {
        name,
        createdAt: new Date().toISOString(),
        fps: Math.round((recFrames.length * 1000) / dur),
        frames: recFrames,
    };
    addClip(clip);
    setStatus(`Saved "${name}" (${recFrames.length} frames, ${(dur / 1000).toFixed(1)}s).`);
    log(`Saved recording "${name}": ${recFrames.length} frames.`);
    recFrames = [];
}

// --- Init -------------------------------------------------------------------
export function initTracking() {
    els = {
        video: document.getElementById("track-video"),
        canvas: document.getElementById("track-canvas"),
        readout: document.getElementById("track-readout"),
        status: document.getElementById("track-status"),
        btnStart: document.getElementById("btn-track-start"),
        btnStop: document.getElementById("btn-track-stop"),
        btnCalibOpen: document.getElementById("btn-calib-open"),
        btnCalibClosed: document.getElementById("btn-calib-closed"),
        btnCalibReset: document.getElementById("btn-calib-reset"),
        btnRec: document.getElementById("btn-record"),
        btnRecStop: document.getElementById("btn-record-stop"),
        recName: document.getElementById("record-name"),
        mirror: document.getElementById("track-mirror"),
        invertHost: document.getElementById("track-invert"),
    };
    if (!els.readout) return;   // panel not present

    buildReadout();
    updateReadout(lastAngles);

    // Per-finger invert checkboxes.
    if (els.invertHost) {
        els.invertHost.innerHTML = "";
        for (let i = 0; i < N; ++i) {
            const lab = document.createElement("label");
            lab.innerHTML =
                `<input type="checkbox" data-ch="${i}" ${calib[i].invert ? "checked" : ""}/> ${FINGERS[i].name}`;
            els.invertHost.appendChild(lab);
        }
        els.invertHost.addEventListener("change", (ev) => {
            const cb = ev.target;
            if (cb.tagName === "INPUT") toggleInvert(parseInt(cb.dataset.ch, 10), cb.checked);
        });
    }

    els.btnStart.addEventListener("click", startCamera);
    els.btnStop.addEventListener("click", stopCamera);
    els.btnCalibOpen.addEventListener("click", () => capture("open"));
    els.btnCalibClosed.addEventListener("click", () => capture("closed"));
    if (els.btnCalibReset) els.btnCalibReset.addEventListener("click", resetCalib);
    els.btnRec.addEventListener("click", startRecording);
    els.btnRecStop.addEventListener("click", stopRecording);
    if (els.mirror) {
        els.mirror.addEventListener("change", () => {
            mirror = els.mirror.checked;
            if (mirror && !ble.connected) {
                setStatus("Live mirror on — connect BLE + power servos to see motion.");
            } else {
                setStatus(mirror ? "Live mirror ON." : "Live mirror OFF.");
            }
        });
    }
}
