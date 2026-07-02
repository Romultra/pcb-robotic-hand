// Latency test panel. Two measurements that matter for a hand that mirrors a
// patient in real time:
//   1. App-to-board round-trip — write a ping to the board's echo characteristic
//      and time how long the echoed notification takes to come back. Driven
//      mostly by the BLE connection interval. Also a "write-ack" mode (ATT ack,
//      no app echo) and a Wi-Fi mode (HTTP /lat) for comparison.
//   2. Hand-tracking inference — MediaPipe time per frame, fed from tracking.js.
//
// Command-to-movement is intentionally not measured here: the ESP32 starts the
// PWM in an interrupt within ~1 ms of the command arriving (taken as negligible),
// the servo's mechanical travel is out of scope, and a screen-flash marker would
// be dominated by display / refresh latency that needs an LDAT to untangle.
//
// See journal/decisions/007-latency-test-suite.md.

import { ble, UUID_PING } from "./ble.js";
import { log } from "./log.js";
import { trackingStats } from "./tracking.js";
import { wifiLink, boardLatUrl } from "./net.js";

const PING_TIMEOUT_MS = 2000;

// --- BLE echo ping ----------------------------------------------------------
let pingSubscribed = false;
let seqCounter = 0;
const pending = new Map();   // seq -> resolve(rtt_ms)

// Echoed notification: first 2 bytes are the little-endian sequence we sent.
function onPong(dv) {
    if (dv.byteLength < 2) return;
    const seq = dv.getUint16(0, true);
    const resolve = pending.get(seq);
    if (resolve) resolve(performance.now());
}

async function ensurePingSub() {
    if (pingSubscribed) return;
    await ble.subscribeNotify(UUID_PING, onPong);
    pingSubscribed = true;
}

// One echo round trip. Resolves with the RTT in ms, or null on timeout / error
// (counted as a drop). The payload is padded to `size` bytes; only the first two
// carry the sequence number, the rest is filler so we can probe payload-size
// effects on latency.
function echoPing(seq, size, timeoutMs) {
    return new Promise((resolve) => {
        const buf = new Uint8Array(Math.max(2, size));
        buf[0] = seq & 0xff;
        buf[1] = (seq >> 8) & 0xff;
        let settled = false;
        const finish = (val) => {
            if (settled) return;
            settled = true;
            clearTimeout(timer);
            pending.delete(seq);
            resolve(val);
        };
        const timer = setTimeout(() => finish(null), timeoutMs);
        const t0 = performance.now();
        pending.set(seq, (tEcho) => finish(tEcho - t0));
        ble.writeNoResponse(UUID_PING, buf).catch(() => finish(null));
    });
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let lastSamples = [];   // raw RTT samples of the most recent run (for Copy CSV)
let lastMode = "";

async function runEchoTest() {
    if (!ble.connected) { setPingStatus("Connect to the board first."); return; }
    await ensurePingSub();
    await runPings("echo", (seq, size) => echoPing(seq, size, PING_TIMEOUT_MS));
}

async function runWriteAckTest() {
    if (!ble.connected) { setPingStatus("Connect to the board first."); return; }
    // writeWithResponse resolves on the ATT acknowledgement — a link-layer round
    // trip with no application echo, so it isolates transport from board handling.
    await runPings("write-ack", async (seq, size) => {
        const buf = new Uint8Array(Math.max(2, size));
        buf[0] = seq & 0xff;
        buf[1] = (seq >> 8) & 0xff;
        const t0 = performance.now();
        try {
            await ble.writeWithResponse(UUID_PING, buf);
            return performance.now() - t0;
        } catch {
            return null;
        }
    });
}

// --- Wi-Fi echo ping --------------------------------------------------------
// HTTP round trip to the board's /lat echo over the board's Wi-Fi link, to
// compare Wi-Fi against the BLE figure (motor commands themselves always go over
// BLE). Includes a fresh TCP handshake per request (the board's WebServer can't
// hold the connection open), so this is the latency the webapp actually achieves
// over Wi-Fi, not a bare RTT — the BLE comparison is in the same spirit (it
// includes the browser's Web Bluetooth queueing).
function wifiPing(seq, timeoutMs) {
    return new Promise((resolve) => {
        const url = boardLatUrl(seq);
        if (!url) { resolve(null); return; }
        const ctrl = new AbortController();
        const timer = setTimeout(() => ctrl.abort(), timeoutMs);
        const t0 = performance.now();
        fetch(url, { signal: ctrl.signal, cache: "no-store" })
            .then((r) => r.text().then(() => (r.ok ? performance.now() - t0 : null)))
            .catch(() => null)
            .then((v) => { clearTimeout(timer); resolve(v); });
    });
}

async function runWifiTest() {
    if (!wifiLink.ip) {
        setPingStatus("No Wi-Fi link — connect the board to Wi-Fi first (Wi-Fi panel).");
        return;
    }
    // Payload size doesn't apply to the Wi-Fi echo; Count and Gap still do.
    await runPings("wifi", (seq) => wifiPing(seq, PING_TIMEOUT_MS));
}

// Shared sequential ping-pong loop. `once(seq,size)` does one round trip and
// returns the RTT or null.
async function runPings(mode, once) {
    const count = clampInt(els.pingCount.value, 1, 1000, 50);
    const size = clampInt(els.pingSize.value, 2, 240, 8);
    const gap = clampInt(els.pingGap.value, 0, 1000, 0);

    setButtonsBusy(true);
    const samples = [];
    let drops = 0;
    for (let i = 0; i < count; ++i) {
        const rtt = await once(seqCounter++ & 0xffff, size);
        if (rtt == null) drops++;
        else samples.push(rtt);
        if (i % 5 === 0 || i === count - 1) setPingStatus(`Pinging (${mode})… ${i + 1}/${count}`);
        if (gap) await sleep(gap);
    }
    setButtonsBusy(false);

    lastSamples = samples;
    lastMode = mode;
    renderStats(mode, samples, drops, count);
    const s = stats(samples);
    if (s) log(`BLE ${mode}: n=${s.n} min=${s.min.toFixed(1)} med=${s.median.toFixed(1)} `
              + `avg=${s.avg.toFixed(1)} p95=${s.p95.toFixed(1)} max=${s.max.toFixed(1)} `
              + `jitter=${s.std.toFixed(1)} drops=${drops} (ms)`);
    setPingStatus(`Done (${mode}): ${samples.length} ok, ${drops} dropped.`);
}

function stats(arr) {
    if (arr.length === 0) return null;
    const s = [...arr].sort((a, b) => a - b);
    const n = s.length;
    const avg = s.reduce((a, b) => a + b, 0) / n;
    const variance = s.reduce((a, b) => a + (b - avg) ** 2, 0) / n;
    const pct = (p) => s[Math.min(n - 1, Math.floor(p * n))];
    return { n, min: s[0], max: s[n - 1], avg, median: pct(0.5), p95: pct(0.95), std: Math.sqrt(variance) };
}

function renderStats(mode, samples, drops, count) {
    const s = stats(samples);
    const fmt = (x) => (x == null ? "—" : `${x.toFixed(1)} ms`);
    setText("lat-mode", mode);
    setText("lat-n", s ? `${s.n} / ${count}` : "—");
    setText("lat-min", s ? fmt(s.min) : "—");
    setText("lat-median", s ? fmt(s.median) : "—");
    setText("lat-avg", s ? fmt(s.avg) : "—");
    setText("lat-p95", s ? fmt(s.p95) : "—");
    setText("lat-max", s ? fmt(s.max) : "—");
    setText("lat-std", s ? fmt(s.std) : "—");
    setText("lat-drops", String(drops));
}

async function copyCsv() {
    if (lastSamples.length === 0) { setPingStatus("No samples yet — run a test first."); return; }
    const csv = `i,rtt_ms\n${lastSamples.map((v, i) => `${i},${v.toFixed(3)}`).join("\n")}\n`;
    try {
        await navigator.clipboard.writeText(csv);
        setPingStatus(`Copied ${lastSamples.length} ${lastMode} samples as CSV.`);
    } catch {
        // Clipboard API can be blocked; fall back to dumping into the log.
        log(`CSV (${lastMode}):\n${csv}`);
        setPingStatus("Clipboard blocked — CSV written to the log instead.");
    }
}

// --- Hand-tracking inference readout ----------------------------------------
const TRACK_WIN = 60;
const trackWin = [];

trackingStats.addEventListener("sample", (e) => {
    trackWin.push(e.detail);
    if (trackWin.length > TRACK_WIN) trackWin.shift();
    renderTrackStats();
});

function renderTrackStats() {
    if (trackWin.length === 0) return;
    const inf = trackWin.map((x) => x.inferenceMs);
    const last = inf[inf.length - 1];
    const avg = inf.reduce((a, b) => a + b, 0) / inf.length;
    const max = Math.max(...inf);
    const intervals = trackWin.map((x) => x.frameIntervalMs).filter((x) => x > 0);
    const fps = intervals.length
        ? 1000 / (intervals.reduce((a, b) => a + b, 0) / intervals.length)
        : 0;
    setText("trk-last", `${last.toFixed(1)} ms`);
    setText("trk-avg", `${avg.toFixed(1)} ms`);
    setText("trk-max", `${max.toFixed(1)} ms`);
    setText("trk-fps", fps ? `${fps.toFixed(1)} fps` : "—");
}

// --- DOM helpers ------------------------------------------------------------
let els = {};

function setText(id, txt) {
    const e = document.getElementById(id);
    if (e) e.textContent = txt;
}
function setPingStatus(t) { setText("ping-status", t); }

function setButtonsBusy(busy) {
    els.btnEcho.disabled = busy;
    els.btnWrAck.disabled = busy;
    if (els.btnWifi) els.btnWifi.disabled = busy;
}

function clampInt(v, lo, hi, dflt) {
    const n = parseInt(v, 10);
    if (Number.isNaN(n)) return dflt;
    return Math.max(lo, Math.min(hi, n));
}

// Drop our cached subscription flag on disconnect; the BLE client clears its
// notify handlers, so the next test re-subscribes.
ble.addEventListener("disconnected", () => { pingSubscribed = false; });

let wired = false;

export function initLatency() {
    els = {
        pingCount: document.getElementById("ping-count"),
        pingSize: document.getElementById("ping-size"),
        pingGap: document.getElementById("ping-gap"),
        btnEcho: document.getElementById("btn-ping-run"),
        btnWrAck: document.getElementById("btn-ping-wr"),
        btnWifi: document.getElementById("btn-ping-wifi"),
        btnCsv: document.getElementById("btn-ping-csv"),
    };
    if (!els.btnEcho) return;   // panel not present
    if (wired) return;

    els.btnEcho.addEventListener("click", runEchoTest);
    els.btnWrAck.addEventListener("click", runWriteAckTest);
    if (els.btnWifi) els.btnWifi.addEventListener("click", runWifiTest);
    els.btnCsv.addEventListener("click", copyCsv);
    wired = true;
}
