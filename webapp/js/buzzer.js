import { ble, UUID_BUZZER } from "./ble.js";
import { log } from "./log.js";

async function fire() {
    const freq = parseInt(document.getElementById("buz-freq").value, 10);
    const dur  = parseInt(document.getElementById("buz-dur").value, 10);
    const buf = new Uint8Array(4);
    buf[0] = freq & 0xff;
    buf[1] = (freq >> 8) & 0xff;
    buf[2] = dur & 0xff;
    buf[3] = (dur >> 8) & 0xff;
    try {
        await ble.writeNoResponse(UUID_BUZZER, buf);
        log(`Buzzer: ${freq} Hz / ${dur} ms`);
    } catch (e) {
        log(`buzzer failed: ${e.message}`);
    }
}

async function stop() {
    try {
        await ble.writeNoResponse(UUID_BUZZER, new Uint8Array([0, 0, 0, 0]));
        log("Buzzer stop");
    } catch (e) {
        log(`buzzer stop failed: ${e.message}`);
    }
}

export function initBuzzer() {
    document.getElementById("btn-buzzer-fire").addEventListener("click", fire);
    document.getElementById("btn-buzzer-stop").addEventListener("click", stop);
}
