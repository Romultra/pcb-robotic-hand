import { ble } from "./ble.js";
import { log, clearLog } from "./log.js";
import { initStatus } from "./status.js";
import { initServos } from "./servos.js";
import { initCalib } from "./calib.js";
import { initBuzzer } from "./buzzer.js";
import { initAudio } from "./audio.js";
import { initSd } from "./sd.js";
import { initWifi } from "./wifi.js";
import { initTracking } from "./tracking.js";
import { initPlayback } from "./playback.js";
import { initLatency } from "./latency.js";

const btnConnect    = document.getElementById("btn-connect");
const btnDisconnect = document.getElementById("btn-disconnect");
const connStateEl   = document.getElementById("conn-state");

function setConnectedUi(on) {
    btnConnect.disabled    = on;
    btnDisconnect.disabled = !on;
    connStateEl.textContent = on ? "Connected" : "Disconnected";
    connStateEl.className   = on ? "conn-on" : "conn-off";
}

async function onConnect() {
    btnConnect.disabled = true;
    try {
        await ble.connect();
        // Subscribe to notify-only characteristics once before binding UI.
        await initStatus();
        await initSd();
        await initWifi();
        await initCalib();   // subscribes to SERVO_CAL notify, then reads state
        await initAudio();   // subscribes to AUDIO_FILE notify, then binds UI
        // Plain WRITE characteristics — wire UI handlers only.
        initServos();
        initBuzzer();
        setConnectedUi(true);
    } catch (e) {
        log(`connect failed: ${e.message}`);
        setConnectedUi(false);
    }
}

async function onDisconnect() {
    await ble.disconnect();
}

ble.addEventListener("connected", () => setConnectedUi(true));
ble.addEventListener("disconnected", () => setConnectedUi(false));

btnConnect.addEventListener("click", onConnect);
btnDisconnect.addEventListener("click", onDisconnect);
document.getElementById("btn-clear-log").addEventListener("click", clearLog);

// Hand tracking + clip playback work offline (camera, calibration, recording,
// clip management). They only need BLE when streaming frames to the board, which
// is guarded internally — so wire them once at load rather than on connect.
initTracking();
initPlayback();
// Latency panel: BLE-dependent actions self-guard on ble.connected, and the
// hand-tracking readout just listens for tracking samples, so wire it at load.
initLatency();

// Browser feature gate — warn early if Web Bluetooth isn't there.
if (!navigator.bluetooth) {
    log("Web Bluetooth not detected. Use Chrome on Android.");
    btnConnect.disabled = true;
    connStateEl.textContent = "Unsupported browser";
}
