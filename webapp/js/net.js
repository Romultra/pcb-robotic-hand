// Shared knowledge of the board's Wi-Fi data path.
//
// BLE stays the control channel. When the board joins a network (via the Wi-Fi
// panel) the firmware reports its IP over BLE; we remember it here so bulk
// transfers (SD uploads) can go straight to the board over HTTP instead of the
// slow chunk-acked BLE path.
//
// Reaching the board's plain-HTTP endpoint from this HTTPS page relies on
// Chrome's Local Network Access (Chrome >=142): a request to a private-IP
// literal is exempted from the mixed-content block once the user grants the
// one-time "access devices on your local network" permission. See
// journal/decisions/002-wifi-file-transfer-lna.md.

import { ble } from "./ble.js";

export const wifiLink = { ip: null };

function notifyChanged() {
    // Reuse the ble EventTarget so other modules can react without a new bus.
    ble.dispatchEvent(new Event("wifilink"));
}

export function setBoardIp(ip) {
    wifiLink.ip = ip;
    notifyChanged();
}

export function clearBoardIp() {
    wifiLink.ip = null;
    notifyChanged();
}

// URL for a streamed file upload. null when no Wi-Fi link is known.
export function boardUploadUrl(path) {
    if (!wifiLink.ip) return null;
    return `http://${wifiLink.ip}/upload?path=${encodeURIComponent(path)}`;
}

// Latency-echo endpoint for the Wi-Fi round-trip test. null when no link.
export function boardLatUrl(seq) {
    return wifiLink.ip ? `http://${wifiLink.ip}/lat?seq=${seq}` : null;
}

// The IP is meaningless once BLE drops (we no longer know the board's state).
ble.addEventListener("disconnected", clearBoardIp);
