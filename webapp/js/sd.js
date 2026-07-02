import { ble, UUID_SD_TEST, UUID_SD_UPLOAD } from "./ble.js";
import { log } from "./log.js";
import { wifiLink, boardUploadUrl } from "./net.js";

const RESULT_NAMES = {
    0: "OK",
    1: "no card inserted",
    2: "SPI init failure",
    3: "mount failure",
    4: "filesystem unreadable",
};

function onNotify(dv) {
    if (dv.byteLength < 9) return;
    const result   = dv.getUint8(0);
    const sizeMB   = dv.getUint32(1, true);
    const freeMB   = dv.getUint32(5, true);
    const txt = result === 0
        ? `OK — ${sizeMB.toLocaleString()} MB total, ${freeMB.toLocaleString()} MB free`
        : `FAIL: ${RESULT_NAMES[result] || `code ${result}`}`;
    document.getElementById("sd-result").textContent = txt;
    log(`SD self-test: ${txt}`);
}

async function run() {
    document.getElementById("sd-result").textContent = "running…";
    try {
        await ble.writeNoResponse(UUID_SD_TEST, new Uint8Array([1]));
    } catch (e) {
        log(`sd_test trigger failed: ${e.message}`);
        document.getElementById("sd-result").textContent = "(trigger failed)";
    }
}

// --- File upload -----------------------------------------------------------
// Protocol mirrors firmware/src/ble.cpp CbSdUpload:
//   0x01 BEGIN  : op path_len path total_size(LE u32)
//   0x02 CHUNK  : op data
//   0x03 END    : op
//   0x04 ABORT  : op
// Reply: status(1) bytes_received(LE u32)

const OP_BEGIN = 0x01;
const OP_CHUNK = 0x02;
const OP_END   = 0x03;
const OP_ABORT = 0x04;

const ST_READY     = 0x00;
const ST_CHUNK_OK  = 0x01;
const ST_COMPLETE  = 0x02;

const STATUS_NAMES = {
    0x00: "ready",
    0x01: "chunk ok",
    0x02: "complete",
    0x80: "no card",
    0x81: "mount failure",
    0x82: "open failure",
    0x83: "write failure",
    0x84: "protocol error",
    0x85: "aborted",
};

// Stay well under the 247-byte MTU. NimBLE on ESP32 reserves ~3 bytes of ATT
// header; with our 1-byte opcode that leaves ~243 bytes for payload, but some
// stacks negotiate a smaller MTU so we keep a safe margin.
const CHUNK_SIZE = 180;

// Single-flight reply waiter. Only one upload can be in progress at a time.
let pendingReply = null;
function awaitReply(timeoutMs = 8000) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            if (pendingReply) {
                pendingReply = null;
                reject(new Error("upload reply timeout"));
            }
        }, timeoutMs);
        pendingReply = (status, received) => {
            clearTimeout(timer);
            pendingReply = null;
            resolve({ status, received });
        };
    });
}

function onUploadNotify(dv) {
    if (dv.byteLength < 5) return;
    const status   = dv.getUint8(0);
    const received = dv.getUint32(1, true);
    if (pendingReply) pendingReply(status, received);
}

let cancelRequested = false;
let activeXhr = null;

// How many times to retry the Wi-Fi upload before dropping to the slow BLE
// path. A single transient stall (radio coexistence, a busy SD card) shouldn't
// banish the whole transfer to BLE, so give Wi-Fi a couple more goes first.
const WIFI_UPLOAD_ATTEMPTS = 3;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function statusName(s) { return STATUS_NAMES[s] || `0x${s.toString(16)}`; }

function setUploadUi(running, total = 0) {
    const prog   = document.getElementById("sd-upload-progress");
    const cancel = document.getElementById("btn-sd-upload-cancel");
    const upload = document.getElementById("btn-sd-upload");
    upload.disabled = running;
    cancel.disabled = !running;
    if (running) {
        prog.classList.add("active");
        prog.value = 0;
        prog.max   = total || 1;
    } else {
        prog.classList.remove("active");
    }
}

// Reflects which transport the next upload will use, from the Wi-Fi link state.
function updateTransportHint() {
    const el = document.getElementById("sd-upload-hint");
    if (!el) return;
    el.textContent = wifiLink.ip
        ? `Transfer path: Wi-Fi (${wifiLink.ip}) — fast`
        : "Transfer path: BLE — connect Wi-Fi above for a fast transfer";
}

// --- Wi-Fi data path -------------------------------------------------------
// Streams the file straight to the board's HTTP server (firmware webfs.cpp).
// POSTs to a private-IP literal so Chrome's Local Network Access lets this
// HTTPS page reach the board's plain-HTTP endpoint (one-time permission). XHR
// (not fetch) gives a real upload-progress event for the progress bar.
function uploadOverWifi(file, path, progEl) {
    return new Promise((resolve, reject) => {
        const url = boardUploadUrl(path);
        if (!url) { reject(new Error("no Wi-Fi link")); return; }

        const fd = new FormData();
        // Carry the destination path as the multipart filename too: the
        // firmware reads it from here if the ?path= query arg isn't parsed
        // during the upload callback (varies by arduino-esp32 version).
        fd.append("file", file, path.replace(/^\//, ""));

        const xhr = new XMLHttpRequest();
        activeXhr = xhr;
        xhr.open("POST", url);
        // The Wi-Fi path runs ~200 kB/s, so a large WAV needs minutes. Size the
        // timeout to a conservative ~50 kB/s floor (2 min minimum) so a big but
        // healthy upload isn't killed mid-flight; Cancel is the manual escape.
        xhr.timeout = Math.max(120000, Math.ceil(file.size / 50));
        xhr.upload.addEventListener("progress", (e) => {
            if (e.lengthComputable) { progEl.max = e.total; progEl.value = e.loaded; }
        });
        xhr.addEventListener("load", () => {
            activeXhr = null;
            if (xhr.status >= 200 && xhr.status < 300) {
                resolve(xhr.responseText || "");
            } else {
                // The firmware returns {"error":"<reason>"} on a 500 (no card,
                // write failure, …); surface that instead of a bare status.
                let detail = xhr.responseText || "";
                try { detail = JSON.parse(detail).error || detail; } catch (_) { /* keep raw */ }
                reject(new Error(`HTTP ${xhr.status}${detail ? `: ${detail}` : ""}`));
            }
        });
        xhr.addEventListener("error", () => {
            activeXhr = null;
            // Usually: board unreachable, or the local-network permission was
            // denied/dismissed. The caller falls back to BLE.
            reject(new Error("network error (board unreachable or local-network permission denied)"));
        });
        xhr.addEventListener("timeout", () => { activeXhr = null; reject(new Error("timeout")); });
        xhr.addEventListener("abort",   () => { activeXhr = null; reject(new Error("cancelled by user")); });
        xhr.send(fd);
    });
}

// --- BLE data path (fallback) ----------------------------------------------
// The original chunk-acked protocol: one write-with-response per CHUNK_SIZE
// bytes, each confirmed by a notify. Reliable but slow; used when Wi-Fi is down.
async function uploadOverBle(data, pathBytes, progEl) {
    const total = data.length;

    // BEGIN
    const begin = new Uint8Array(2 + pathBytes.length + 4);
    begin[0] = OP_BEGIN;
    begin[1] = pathBytes.length;
    begin.set(pathBytes, 2);
    new DataView(begin.buffer).setUint32(2 + pathBytes.length, total, true);

    let waiter = awaitReply();
    await ble.writeWithResponse(UUID_SD_UPLOAD, begin);
    let r = await waiter;
    if (r.status !== ST_READY) {
        throw new Error(`BEGIN rejected: ${statusName(r.status)}`);
    }

    // CHUNKS
    let offset = 0;
    const chunk = new Uint8Array(1 + CHUNK_SIZE);
    chunk[0] = OP_CHUNK;
    while (offset < total) {
        if (cancelRequested) {
            waiter = awaitReply();
            await ble.writeWithResponse(UUID_SD_UPLOAD, new Uint8Array([OP_ABORT]));
            await waiter;
            throw new Error("cancelled by user");
        }
        const end = Math.min(offset + CHUNK_SIZE, total);
        const len = end - offset;
        const pkt = len === CHUNK_SIZE ? chunk : new Uint8Array(1 + len);
        if (len !== CHUNK_SIZE) pkt[0] = OP_CHUNK;
        pkt.set(data.subarray(offset, end), 1);

        waiter = awaitReply();
        await ble.writeWithResponse(UUID_SD_UPLOAD, pkt);
        r = await waiter;
        if (r.status !== ST_CHUNK_OK) {
            throw new Error(`chunk @${offset} rejected: ${statusName(r.status)}`);
        }
        offset = end;
        progEl.value = offset;
    }

    // END
    waiter = awaitReply();
    await ble.writeWithResponse(UUID_SD_UPLOAD, new Uint8Array([OP_END]));
    r = await waiter;
    if (r.status !== ST_COMPLETE) {
        throw new Error(`END rejected: ${statusName(r.status)}`);
    }
    return r.received;
}

// Best-effort abort so the firmware closes any open handle after a BLE failure.
async function bleAbortQuietly() {
    try {
        const waiter = awaitReply(2000);
        await ble.writeWithResponse(UUID_SD_UPLOAD, new Uint8Array([OP_ABORT]));
        await waiter;
    } catch (_) { /* swallow */ }
}

async function doUpload() {
    const fileInput = document.getElementById("sd-upload-file");
    const pathInput = document.getElementById("sd-upload-path");
    const resultEl  = document.getElementById("sd-upload-result");
    const progEl    = document.getElementById("sd-upload-progress");

    const f = fileInput.files && fileInput.files[0];
    if (!f) {
        resultEl.textContent = "pick a file first";
        return;
    }
    let path = (pathInput.value || "").trim();
    if (!path) path = "/" + f.name;
    if (path[0] !== "/") path = "/" + path;

    const enc = new TextEncoder();
    const pathBytes = enc.encode(path);
    if (pathBytes.length > 200) {
        resultEl.textContent = "path too long (max 200 bytes UTF-8)";
        return;
    }

    const total = f.size;
    cancelRequested = false;
    setUploadUi(true, total);

    // Prefer Wi-Fi when the board is on a network; retry a few times before
    // falling back to BLE on persistent failure.
    if (wifiLink.ip) {
        let lastErr = null;
        for (let attempt = 1; attempt <= WIFI_UPLOAD_ATTEMPTS && !cancelRequested; attempt++) {
            const tag = attempt > 1 ? ` (attempt ${attempt}/${WIFI_UPLOAD_ATTEMPTS})` : "";
            resultEl.textContent = `uploading over Wi-Fi → ${path} (${total.toLocaleString()} bytes)${tag}…`;
            log(`SD upload via Wi-Fi (${wifiLink.ip}): ${f.name} → ${path} (${total} bytes)${tag}`);
            progEl.value = 0;
            try {
                const resp = await uploadOverWifi(f, path, progEl);
                let received = total;
                try { received = JSON.parse(resp).received ?? total; } catch (_) { /* keep total */ }
                resultEl.textContent = `done over Wi-Fi — ${received.toLocaleString()} bytes written to ${path}`;
                log(`SD upload complete over Wi-Fi: ${received} bytes → ${path}`);
                setUploadUi(false);
                cancelRequested = false;
                return;
            } catch (e) {
                if (cancelRequested) {
                    resultEl.textContent = "cancelled";
                    log("SD upload cancelled (Wi-Fi)");
                    setUploadUi(false);
                    cancelRequested = false;
                    return;
                }
                lastErr = e;
                log(`Wi-Fi upload attempt ${attempt} failed: ${e.message}`);
                if (attempt < WIFI_UPLOAD_ATTEMPTS) await sleep(300 * attempt);
            }
        }
        // Cancelled during a retry backoff: the loop exits without throwing, so
        // catch it here rather than wrongly dropping into the BLE fallback.
        if (cancelRequested) {
            resultEl.textContent = "cancelled";
            log("SD upload cancelled (Wi-Fi)");
            setUploadUi(false);
            cancelRequested = false;
            return;
        }
        log(`Wi-Fi upload failed after ${WIFI_UPLOAD_ATTEMPTS} attempts (${lastErr ? lastErr.message : "unknown"}); falling back to BLE`);
        resultEl.textContent = "Wi-Fi failed, retrying over BLE…";
        progEl.value = 0;
    }

    // BLE fallback. Only now do we pull the whole file into memory.
    const data = new Uint8Array(await f.arrayBuffer());
    progEl.max = total || 1;
    resultEl.textContent = `uploading over BLE → ${path} (${total.toLocaleString()} bytes)…`;
    log(`SD upload via BLE: ${f.name} → ${path} (${total} bytes)`);
    try {
        const received = await uploadOverBle(data, pathBytes, progEl);
        resultEl.textContent = `done over BLE — ${received.toLocaleString()} bytes written to ${path}`;
        log(`SD upload complete over BLE: ${received} bytes → ${path}`);
    } catch (e) {
        resultEl.textContent = `FAIL: ${e.message}`;
        log(`SD upload failed: ${e.message}`);
        await bleAbortQuietly();
    } finally {
        setUploadUi(false);
        cancelRequested = false;
    }
}

export async function initSd() {
    await ble.subscribeNotify(UUID_SD_TEST, onNotify);
    await ble.subscribeNotify(UUID_SD_UPLOAD, onUploadNotify);
    document.getElementById("btn-sd-test").addEventListener("click", run);
    document.getElementById("btn-sd-upload").addEventListener("click", doUpload);
    document.getElementById("btn-sd-upload-cancel").addEventListener("click", () => {
        cancelRequested = true;
        if (activeXhr) activeXhr.abort();  // cancels an in-flight Wi-Fi upload
    });
    // Show (and keep updating) which transport an upload will take.
    ble.addEventListener("wifilink", updateTransportHint);
    updateTransportHint();
}
