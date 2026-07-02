// Subscribes to the status notify characteristic and updates the dashboard.

import { ble, UUID_STATUS } from "./ble.js";
import { log } from "./log.js";

function fmtUptime(ms) {
    const s = Math.floor(ms / 1000);
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const ss = s % 60;
    return `${h}h ${m}m ${ss}s`;
}

function update(dv) {
    if (dv.byteLength < 12) return;
    // u8 flags, u8 servo_rail_on, u8 sd_present, u8 wifi_connected, u32 uptime_ms, u32 free_heap
    const servoOn   = dv.getUint8(1) !== 0;
    const sdPresent = dv.getUint8(2) !== 0;
    const wifi      = dv.getUint8(3) !== 0;
    const uptime    = dv.getUint32(4, true);
    const freeHeap  = dv.getUint32(8, true);

    document.getElementById("st-servo-rail").textContent = servoOn   ? "ON"   : "OFF";
    document.getElementById("st-sd").textContent         = sdPresent ? "yes"  : "no";
    document.getElementById("st-wifi").textContent       = wifi      ? "yes"  : "no";
    document.getElementById("st-uptime").textContent     = fmtUptime(uptime);
    document.getElementById("st-heap").textContent       = `${freeHeap.toLocaleString()} bytes`;
}

export async function initStatus() {
    await ble.subscribeNotify(UUID_STATUS, update);
    log("Status notifications subscribed.");
}
