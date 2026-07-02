import { ble, UUID_WIFI_SCAN, UUID_WIFI_CONNECT } from "./ble.js";
import { log } from "./log.js";
import { setBoardIp, clearBoardIp } from "./net.js";

let scanBatch = [];

function decodeScanPacket(dv) {
    // Format per entry: i8 rssi, u8 ssid_len, ssid bytes.
    const out = [];
    let off = 0;
    while (off + 2 <= dv.byteLength) {
        const rssi = dv.getInt8(off);
        const len  = dv.getUint8(off + 1);
        off += 2;
        if (off + len > dv.byteLength) break;
        const bytes = new Uint8Array(dv.buffer, dv.byteOffset + off, len);
        const ssid = new TextDecoder().decode(bytes);
        out.push({ rssi, ssid });
        off += len;
    }
    return out;
}

function renderScan(entries) {
    const ul = document.getElementById("wifi-list");
    ul.innerHTML = "";
    for (const e of entries) {
        const li = document.createElement("li");
        li.textContent = `${e.rssi.toString().padStart(4)} dBm  ${e.ssid}`;
        li.addEventListener("click", () => {
            document.getElementById("wifi-ssid").value = e.ssid;
        });
        ul.appendChild(li);
    }
}

function onScanNotify(dv) {
    if (dv.byteLength === 0) {
        // End-of-scan terminator.
        renderScan(scanBatch);
        log(`Wi-Fi scan: ${scanBatch.length} APs`);
        scanBatch = [];
        return;
    }
    const entries = decodeScanPacket(dv);
    scanBatch.push(...entries);
}

function onConnectNotify(dv) {
    if (dv.byteLength < 5) return;
    const status = dv.getUint8(0);
    const ip = `${dv.getUint8(1)}.${dv.getUint8(2)}.${dv.getUint8(3)}.${dv.getUint8(4)}`;
    const el = document.getElementById("wifi-conn-result");
    if (status === 0) {
        el.textContent = `Connected — IP ${ip}`;
        log(`Wi-Fi connected: ${ip}`);
        // Hand the IP to the data-path layer so SD uploads can use Wi-Fi.
        setBoardIp(ip);
    } else if (status === 3) {
        el.textContent = "Disconnected";
        log("Wi-Fi disconnected");
        clearBoardIp();
    } else {
        el.textContent = `Failed (status ${status})`;
        log(`Wi-Fi connect failed (status ${status})`);
        clearBoardIp();
    }
}

async function startScan() {
    document.getElementById("wifi-list").innerHTML = "<li>scanning…</li>";
    scanBatch = [];
    try {
        await ble.writeNoResponse(UUID_WIFI_SCAN, new Uint8Array([1]));
    } catch (e) {
        log(`wifi_scan trigger failed: ${e.message}`);
    }
}

async function connect() {
    const ssid = document.getElementById("wifi-ssid").value;
    const pw   = document.getElementById("wifi-pw").value;
    if (!ssid) { log("Need an SSID"); return; }

    const ssidBytes = new TextEncoder().encode(ssid);
    const pwBytes   = new TextEncoder().encode(pw);
    if (ssidBytes.length > 32 || pwBytes.length > 63) {
        log("SSID/password too long");
        return;
    }
    const buf = new Uint8Array(2 + ssidBytes.length + pwBytes.length);
    let off = 0;
    buf[off++] = ssidBytes.length;
    buf.set(ssidBytes, off); off += ssidBytes.length;
    buf[off++] = pwBytes.length;
    buf.set(pwBytes, off);
    document.getElementById("wifi-conn-result").textContent = "connecting…";
    try {
        await ble.writeWithResponse(UUID_WIFI_CONNECT, buf);
    } catch (e) {
        log(`wifi_connect failed: ${e.message}`);
    }
}

// Reconnect to the network saved on the board (NVS). A single 0xFF byte on the
// connect characteristic tells the firmware to load its stored credentials and
// rejoin, so the SSID/password never have to be re-entered. The board replies on
// the same notify path (status 1 if it has nothing saved).
async function reconnectSaved() {
    document.getElementById("wifi-conn-result").textContent = "reconnecting…";
    try {
        await ble.writeWithResponse(UUID_WIFI_CONNECT, new Uint8Array([0xFF]));
    } catch (e) {
        log(`wifi reconnect failed: ${e.message}`);
    }
}

// Drop the Wi-Fi link. A single 0x00 byte (ssid_len 0) is the disconnect command
// on the same characteristic; the board replies with status 3. Useful after the
// Wi-Fi latency test, to get the BLE link back to a coexistence-free state.
async function disconnect() {
    document.getElementById("wifi-conn-result").textContent = "disconnecting…";
    try {
        await ble.writeWithResponse(UUID_WIFI_CONNECT, new Uint8Array([0]));
    } catch (e) {
        log(`wifi_disconnect failed: ${e.message}`);
    }
}

export async function initWifi() {
    await ble.subscribeNotify(UUID_WIFI_SCAN, onScanNotify);
    await ble.subscribeNotify(UUID_WIFI_CONNECT, onConnectNotify);
    document.getElementById("btn-wifi-scan").addEventListener("click", startScan);
    document.getElementById("btn-wifi-connect").addEventListener("click", connect);
    document.getElementById("btn-wifi-reconnect").addEventListener("click", reconnectSaved);
    document.getElementById("btn-wifi-disconnect").addEventListener("click", disconnect);
}
