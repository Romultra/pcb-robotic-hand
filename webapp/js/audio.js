import { ble, UUID_AUDIO_PLAY, UUID_AUDIO_VOL, UUID_AUDIO_FILE } from "./ble.js";
import { log } from "./log.js";

const PRESET_NAMES = {
    0: "silence",
    1: "1 kHz sine",
    2: "chirp",
    3: "success jingle",
    4: "fail tone",
};

async function play(presetId) {
    try {
        await ble.writeNoResponse(UUID_AUDIO_PLAY, new Uint8Array([presetId]));
        log(`Audio preset ${presetId} (${PRESET_NAMES[presetId] || "?"})`);
    } catch (e) {
        log(`audio_play failed: ${e.message}`);
    }
}

// --- SD WAV files (dedicated /audio folder) --------------------------------
// Mirrors firmware ble.cpp CbAudioFile / service_audio_*:
//   write 0x01            -> LIST
//   write 0x02 len name   -> PLAY (bare filename in /audio)
//   write 0x03            -> STOP
// Notify replies are tagged by a leading type byte:
//   0x01 chunk: then repeated [name_len(1) name(N) size_u32]
//   0x00 end of list
//   0x02 play status: result (0 = playing, 1 = failed, 2 = stopped)

const OP_LIST = 0x01;
const OP_PLAY = 0x02;
const OP_STOP = 0x03;

const decoder = new TextDecoder();
let fileAcc = [];   // entries accumulated across list chunks

function humanSize(n) {
    if (n >= 1024 * 1024) return `${(n / (1024 * 1024)).toFixed(1)} MB`;
    if (n >= 1024)        return `${(n / 1024).toFixed(1)} KB`;
    return `${n} B`;
}

function setStatus(txt) {
    const el = document.getElementById("audio-file-status");
    if (el) el.textContent = txt;
}

function renderList() {
    const ul = document.getElementById("audio-file-list");
    if (!ul) return;
    ul.innerHTML = "";
    if (fileAcc.length === 0) {
        setStatus("no files in /audio");
        return;
    }
    for (const f of fileAcc) {
        const li = document.createElement("li");
        const btn = document.createElement("button");
        btn.textContent = "Play";
        btn.addEventListener("click", () => playFile(f.name));
        const label = document.createElement("span");
        label.textContent = ` ${f.name} (${humanSize(f.size)})`;
        li.append(btn, label);
        ul.append(li);
    }
    setStatus(`${fileAcc.length} file(s)`);
}

function onNotify(dv) {
    if (dv.byteLength < 1) return;
    const type = dv.getUint8(0);
    if (type === OP_LIST) {              // list chunk
        let off = 1;
        while (off + 1 <= dv.byteLength) {
            const nameLen = dv.getUint8(off); off += 1;
            if (off + nameLen + 4 > dv.byteLength) break;
            const name = decoder.decode(new Uint8Array(dv.buffer, dv.byteOffset + off, nameLen));
            off += nameLen;
            const size = dv.getUint32(off, true); off += 4;
            fileAcc.push({ name, size });
        }
    } else if (type === 0x00) {          // end of list
        renderList();
    } else if (type === OP_PLAY) {       // play status
        const result = dv.byteLength >= 2 ? dv.getUint8(1) : 0;
        const txt = result === 0 ? "playing"
                  : result === 2 ? "stopped"
                  : "play failed (not a supported 16-bit PCM WAV?)";
        setStatus(txt);
        log(`SD audio: ${txt}`);
    }
}

async function requestList() {
    fileAcc = [];
    setStatus("listing…");
    try {
        await ble.writeNoResponse(UUID_AUDIO_FILE, new Uint8Array([OP_LIST]));
    } catch (e) {
        setStatus("(list failed)");
        log(`audio list failed: ${e.message}`);
    }
}

async function playFile(name) {
    const nameBytes = new TextEncoder().encode(name);
    const pkt = new Uint8Array(2 + nameBytes.length);
    pkt[0] = OP_PLAY;
    pkt[1] = nameBytes.length;
    pkt.set(nameBytes, 2);
    try {
        await ble.writeNoResponse(UUID_AUDIO_FILE, pkt);
        log(`SD audio play: ${name}`);
        setStatus(`playing ${name}…`);
    } catch (e) {
        log(`audio play failed: ${e.message}`);
    }
}

// Volume slider -> AUDIO_VOL. Same shape as the servos.js sliders: a fast drag
// fires many "input" events, so coalesce to the latest value and keep only one
// BLE write in flight (Android throws if writes overlap). Bound once with a
// named handler, since initAudio() runs again on every reconnect.
let volPending = null;   // latest unsent percentage, or null when idle
let volInflight = false;

async function pumpVolume() {
    if (volInflight) return;
    volInflight = true;
    while (volPending !== null) {
        const pct = volPending; volPending = null;
        try {
            await ble.writeNoResponse(UUID_AUDIO_VOL, new Uint8Array([pct]));
        } catch (e) {
            log(`audio volume failed: ${e.message}`);
        }
    }
    volInflight = false;
}

function onVolumeInput(ev) {
    const pct = parseInt(ev.target.value, 10);
    const value = document.getElementById("audio-volume-value");
    if (value) value.textContent = `${pct}%`;
    volPending = pct;
    pumpVolume();
}

let volWired = false;
function initVolume() {
    if (volWired) return;
    const slider = document.getElementById("audio-volume");
    if (!slider) return;
    slider.addEventListener("input", onVolumeInput);
    volWired = true;
}

async function stopFile() {
    try {
        await ble.writeNoResponse(UUID_AUDIO_FILE, new Uint8Array([OP_STOP]));
        log("SD audio stop");
    } catch (e) {
        log(`audio stop failed: ${e.message}`);
    }
}

let wired = false;   // bind DOM listeners once, even across reconnects

export async function initAudio() {
    // The BLE wrapper drops its notify handlers on disconnect, so re-subscribe
    // to AUDIO_FILE on every (re)connect.
    await ble.subscribeNotify(UUID_AUDIO_FILE, onNotify);

    if (wired) return;
    document.querySelectorAll("button[data-preset]").forEach(btn => {
        btn.addEventListener("click", () => {
            const id = parseInt(btn.dataset.preset, 10);
            play(id);
        });
    });
    initVolume();
    document.getElementById("btn-audio-list").addEventListener("click", requestList);
    document.getElementById("btn-audio-stop").addEventListener("click", stopFile);
    wired = true;
}
