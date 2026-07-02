// Web Bluetooth wrapper. Exposes one BleClient singleton plus the per-
// characteristic UUIDs that match the firmware's ble.h.

import { log } from "./log.js";

export const SVC_UUID          = "5e6c1b00-0001-4a4e-a5f0-5e0e6a5a0001";
export const UUID_STATUS       = "5e6c1b00-0002-4a4e-a5f0-5e0e6a5a0001";
export const UUID_SERVO_POWER  = "5e6c1b00-0003-4a4e-a5f0-5e0e6a5a0001";
export const UUID_SERVO_SET    = "5e6c1b00-0004-4a4e-a5f0-5e0e6a5a0001";
export const UUID_BUZZER       = "5e6c1b00-0005-4a4e-a5f0-5e0e6a5a0001";
// 5e6c1b00-0006-… (NeoPixel) retired — WS2812B LEDs non-functional on PCB1
// (reversed supply pins; see journal/decisions/001-ws2812b-power-pin-reversal.md).
export const UUID_AUDIO_PLAY   = "5e6c1b00-0007-4a4e-a5f0-5e0e6a5a0001";
export const UUID_SD_TEST      = "5e6c1b00-0008-4a4e-a5f0-5e0e6a5a0001";
export const UUID_WIFI_SCAN    = "5e6c1b00-0009-4a4e-a5f0-5e0e6a5a0001";
export const UUID_WIFI_CONNECT = "5e6c1b00-000a-4a4e-a5f0-5e0e6a5a0001";
export const UUID_SD_UPLOAD    = "5e6c1b00-000b-4a4e-a5f0-5e0e6a5a0001";
// One write sets all 5 servos at once (5-byte frame, each a normalized finger
// position 0..180 the firmware remaps into that finger's calibrated band).
// Used for streamed hand-tracking playback / live mirror — see tracking.js.
export const UUID_SERVO_FRAME  = "5e6c1b00-000c-4a4e-a5f0-5e0e6a5a0001";
// Per-finger calibration + safety limits (jog/capture/save/home). See calib.js.
export const UUID_SERVO_CAL    = "5e6c1b00-000d-4a4e-a5f0-5e0e6a5a0001";
// Play WAV files from the SD card's dedicated /audio folder (LIST/PLAY/STOP),
// strictly confined to that folder. See audio.js.
export const UUID_AUDIO_FILE   = "5e6c1b00-000e-4a4e-a5f0-5e0e6a5a0001";
// Exercise position logging control (ENABLE / SET_TIME / STATUS). The board
// auto-logs finger positions to CSV in the SD /logs folder during exercises;
// retrieval is by pulling the card. Defined here so the firmware/webapp UUID
// sets stay in sync (tools/check_ble_uuids.py); no webapp UI yet — TODO: a logs
// panel that pushes SET_TIME and lists/downloads sessions. See firmware
// drivers/logging.{h,cpp}.
export const UUID_LOG_CTRL     = "5e6c1b00-000f-4a4e-a5f0-5e0e6a5a0001";
// Latency ping: the board echoes whatever payload we write straight back as a
// notification, so the latency panel can time the BLE round trip. See latency.js.
export const UUID_PING         = "5e6c1b00-0010-4a4e-a5f0-5e0e6a5a0001";
// Audio master volume: one byte 0..100 (%). The amp has fixed gain, so the
// board scales the PCM samples digitally. See audio.js.
export const UUID_AUDIO_VOL    = "5e6c1b00-0011-4a4e-a5f0-5e0e6a5a0001";

class BleClient extends EventTarget {
    constructor() {
        super();
        this.device = null;
        this.server = null;
        this.service = null;
        this.chars = {};   // uuid -> BluetoothRemoteGATTCharacteristic
        this._notifyHandlers = new Map();   // uuid -> Set<fn>
        this._onDisconnect = this._onDisconnect.bind(this);
    }

    get connected() {
        return !!(this.device && this.device.gatt && this.device.gatt.connected);
    }

    async connect() {
        if (!navigator.bluetooth) {
            throw new Error("Web Bluetooth not supported in this browser. Use Chrome on Android.");
        }
        log("Requesting device…");
        this.device = await navigator.bluetooth.requestDevice({
            filters: [{ services: [SVC_UUID] }],
        });
        this.device.addEventListener("gattserverdisconnected", this._onDisconnect);

        log(`Connecting to ${this.device.name || "(unnamed)"}…`);
        this.server = await this.device.gatt.connect();
        this.service = await this.server.getPrimaryService(SVC_UUID);

        const uuids = [
            UUID_STATUS, UUID_SERVO_POWER, UUID_SERVO_SET, UUID_SERVO_FRAME,
            UUID_SERVO_CAL, UUID_BUZZER, UUID_AUDIO_PLAY, UUID_AUDIO_VOL,
            UUID_AUDIO_FILE, UUID_SD_TEST, UUID_SD_UPLOAD, UUID_WIFI_SCAN,
            UUID_WIFI_CONNECT, UUID_PING,
        ];
        for (const u of uuids) {
            this.chars[u] = await this.service.getCharacteristic(u);
        }
        log("Connected; all characteristics cached.");
        this.dispatchEvent(new Event("connected"));
    }

    async disconnect() {
        if (this.device && this.device.gatt.connected) {
            this.device.gatt.disconnect();
        }
    }

    _onDisconnect() {
        log("Disconnected.");
        this.chars = {};
        this._notifyHandlers.clear();
        this.dispatchEvent(new Event("disconnected"));
    }

    async writeNoResponse(uuid, bytes) {
        const c = this.chars[uuid];
        if (!c) throw new Error(`No characteristic cached for ${uuid}`);
        const buf = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
        await c.writeValueWithoutResponse(buf);
    }

    async writeWithResponse(uuid, bytes) {
        const c = this.chars[uuid];
        if (!c) throw new Error(`No characteristic cached for ${uuid}`);
        const buf = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
        await c.writeValueWithResponse(buf);
    }

    async subscribeNotify(uuid, handler) {
        const c = this.chars[uuid];
        if (!c) throw new Error(`No characteristic cached for ${uuid}`);
        let set = this._notifyHandlers.get(uuid);
        if (!set) {
            set = new Set();
            this._notifyHandlers.set(uuid, set);
            c.addEventListener("characteristicvaluechanged", (ev) => {
                const dv = ev.target.value;
                for (const fn of set) fn(dv);
            });
            await c.startNotifications();
        }
        set.add(handler);
    }
}

export const ble = new BleClient();
