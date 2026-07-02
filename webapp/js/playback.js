// Recorded hand-clip storage and playback. Clips are produced by tracking.js
// and persisted front-end only (localStorage). Playback walks a clip by its
// per-frame timestamps — the browser is the clock — and streams one 5-servo
// frame per due frame to the board over BLE, mimicking the eventual real-time
// "board follows the hand" data path.
//
// Clip shape: { name, createdAt, fps, frames: [{ t: ms-from-start, a: [5 angles 0..180] }] }

import { ble } from "./ble.js";
import { log } from "./log.js";
import { sendServoFrame } from "./servos.js";

const STORE_KEY = "hand_clips";

// --- Clip store (localStorage) ----------------------------------------------
function getClips() {
    try {
        const raw = localStorage.getItem(STORE_KEY);
        const arr = raw ? JSON.parse(raw) : [];
        return Array.isArray(arr) ? arr : [];
    } catch {
        return [];
    }
}
function setClips(clips) {
    localStorage.setItem(STORE_KEY, JSON.stringify(clips));
}

// Called by tracking.js when a recording is stopped. Names collide-safe.
export function addClip(clip) {
    const clips = getClips();
    let name = clip.name;
    let n = 2;
    while (clips.some((c) => c.name === name)) name = `${clip.name}-${n++}`;
    clip.name = name;
    clips.push(clip);
    setClips(clips);
    renderList();
}

function deleteClip(name) {
    setClips(getClips().filter((c) => c.name !== name));
    renderList();
}

// --- Playback ---------------------------------------------------------------
let playing = false;
let rafId = 0;
let frames = [];
let sendIdx = 0;
let startT = 0;
let playingName = "";

// Coalescing sender: keep at most one frame in flight; if BLE can't keep up,
// newer frames overwrite the pending one rather than queueing.
let pending = null;
let inflight = false;
async function send(angles) {
    pending = angles;
    if (inflight) return;
    inflight = true;
    while (pending) {
        const a = pending; pending = null;
        try {
            await sendServoFrame(a);
        } catch (e) {
            log(`playback send failed: ${e.message}`);
            pending = null;
            break;
        }
    }
    inflight = false;
}

function step() {
    if (!playing) return;
    const elapsed = performance.now() - startT;
    let due = -1;
    while (sendIdx < frames.length && frames[sendIdx].t <= elapsed) {
        due = sendIdx;
        sendIdx++;
    }
    if (due >= 0) send(frames[due].a);   // latest due frame; intermediate frames are dropped
    if (sendIdx >= frames.length) {
        stopPlayback(`Finished "${playingName}".`);
        return;
    }
    rafId = requestAnimationFrame(step);
}

function startPlayback(name) {
    if (playing) return;
    const clip = getClips().find((c) => c.name === name);
    if (!clip || !clip.frames || clip.frames.length === 0) {
        setStatus("Clip empty or missing.");
        return;
    }
    if (!ble.connected) {
        setStatus("Connect to the board first.");
        return;
    }
    frames = clip.frames;
    sendIdx = 0;
    playingName = name;
    startT = performance.now();
    playing = true;
    if (els.btnStop) els.btnStop.disabled = false;
    setStatus(`Playing "${name}"… (ensure servo rail is ON — needs barrel-jack power)`);
    log(`Playback "${name}": ${frames.length} frames.`);
    rafId = requestAnimationFrame(step);
}

function stopPlayback(msg) {
    playing = false;
    if (rafId) cancelAnimationFrame(rafId);
    rafId = 0;
    if (els.btnStop) els.btnStop.disabled = true;
    setStatus(msg || "Playback stopped.");
}

// --- Export / import --------------------------------------------------------
function exportClip(name) {
    const clip = getClips().find((c) => c.name === name);
    if (!clip) return;
    const blob = new Blob([JSON.stringify(clip, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = `${name}.json`;
    a.click();
    URL.revokeObjectURL(url);
}

function importFile(file) {
    const reader = new FileReader();
    reader.onload = (e) => {
        try {
            const parsed = JSON.parse(e.target.result);
            const clips = Array.isArray(parsed) ? parsed : [parsed];
            for (const c of clips) {
                if (c && c.frames && Array.isArray(c.frames)) {
                    addClip({ ...c, name: c.name || "imported" });
                }
            }
            setStatus(`Imported ${clips.length} clip(s).`);
        } catch (err) {
            setStatus(`Import failed: ${err.message}`);
        }
    };
    reader.readAsText(file);
}

// --- UI ---------------------------------------------------------------------
let els = {};

function setStatus(msg) {
    if (els.status) els.status.textContent = msg;
}

function renderList() {
    if (!els.list) return;
    const clips = getClips();
    els.list.innerHTML = "";
    if (clips.length === 0) {
        els.list.innerHTML = `<li class="hint">No saved clips yet.</li>`;
        return;
    }
    for (const c of clips) {
        const dur = c.frames.length ? (c.frames[c.frames.length - 1].t / 1000).toFixed(1) : "0";
        const li = document.createElement("li");
        li.className = "clip-row";
        li.innerHTML = `
            <span class="clip-name">${c.name}</span>
            <span class="hint">${c.frames.length}f · ${dur}s · ${c.fps || "?"}fps</span>
            <button data-act="play" data-name="${c.name}">Play</button>
            <button data-act="export" data-name="${c.name}">Export</button>
            <button data-act="delete" data-name="${c.name}">Delete</button>
        `;
        els.list.appendChild(li);
    }
}

export function initPlayback() {
    els = {
        list: document.getElementById("clip-list"),
        status: document.getElementById("playback-status"),
        btnStop: document.getElementById("btn-playback-stop"),
        importInput: document.getElementById("clip-import"),
    };
    if (!els.list) return;   // panel not present

    els.list.addEventListener("click", (ev) => {
        const btn = ev.target;
        if (btn.tagName !== "BUTTON") return;
        const name = btn.dataset.name;
        if (btn.dataset.act === "play") startPlayback(name);
        else if (btn.dataset.act === "export") exportClip(name);
        else if (btn.dataset.act === "delete") deleteClip(name);
    });
    if (els.btnStop) els.btnStop.addEventListener("click", () => stopPlayback());
    if (els.importInput) {
        els.importInput.addEventListener("change", (ev) => {
            const f = ev.target.files[0];
            if (f) importFile(f);
            ev.target.value = "";
        });
    }
    renderList();
}
