# 002: Fast SD File Transfer over Wi-Fi (STA + Local Network Access)

**Date:** 2026-06-11
**Status:** Decided

## Context

SD-card file transfer from the validation webapp runs over the BLE
`sd_upload` characteristic. It works, but it is slow — and not because of BLE's
PHY ceiling. The protocol (`webapp/js/sd.js` ↔ `firmware/src/ble.cpp`
`CbSdUpload`) sends one `writeWithResponse` of ≤180 bytes and waits for a
notify-ack before the next chunk, so throughput is bounded by the BLE
connection interval (a full round-trip per chunk), realistically a few KB/s.

The Wi-Fi STA path (scan + `connect_sta`) is now validated (see `journal/log.md`
2026-06-10), so Wi-Fi is available as a bulk-data channel. Goal: a much faster
SD transfer, **keeping BLE as the control/orchestration link and keeping the
single existing webapp** — no second front end to maintain.

### The constraint that drives the whole design

The webapp is served over **HTTPS** (GitHub Pages) because Web Bluetooth
requires a secure context. Two browser rules then block the obvious "just POST
the file to the board over Wi-Fi" approach:

- **Mixed content:** an HTTPS page cannot make active requests (`fetch`/XHR/
  WebSocket) to a plain-`http://` target. The board cannot present a
  browser-trusted TLS cert for a LAN IP, so `https://<board>` is not an option
  either.
- This historically forced the file-transfer UI to be **served by the device
  itself** over HTTP (a separate, device-hosted page), because a same-origin
  HTTP page can talk to the device without mixed-content issues.

What changes the calculus: **Chrome 142+ ships Local Network Access (LNA).** A
`fetch()`/XHR that Chrome knows is local *before* DNS resolution — a
**private-IP literal** (e.g. `192.168.1.50`), a **`.local`** host, or a call
annotated `targetAddressSpace:"local"` — is **exempted from the mixed-content
block**, gated by a **one-time user permission**. This makes it possible for the
existing HTTPS app to POST directly to the board with no second page and no TLS
cert. (LNA covers `fetch`/XHR/subresources only — not WebSocket/WebTransport/
WebRTC — which is fine for a request/response file upload.)

## Options Considered

### Front end

1. **Device-hosted HTTP upload page (pre-LNA classic).** Board serves the upload
   UI over HTTP; BLE app hands off to it.
   - *Pros:* works on any browser, no LNA dependency.
   - *Cons:* a **second front end** (or a duplicated, device-served copy) to
     build and maintain; runtime tab-switch; can't reuse the HTTPS app's state.
     Rejected — the explicit goal was to keep one app.
2. **Single HTTPS app POSTs to the board via LNA (chosen).**
   - *Pros:* one front end; BLE stays the control plane; the bulk path is a
     near-silent `fetch`/XHR to the board.
   - *Cons:* requires **Chrome ≥142**; an unavoidable **one-time permission
     prompt**; needs CORS headers on the device responses.

### Wi-Fi mode

A. **STA — board joins an existing network (chosen).**
   - *Pros:* reuses the validated `connect_sta`, which **already returns the
     board IP over BLE** — the app knows exactly where to POST. Phone keeps its
     internet (same network). No captive-portal handling.
   - *Cons:* needs a network the board can join (credentials entered via the
     existing BLE connect flow); a network with **AP client isolation** would
     block phone↔board; DHCP IP rather than a fixed one (mitigated — delivered
     over BLE).

B. **SoftAP — board hosts its own AP at `192.168.4.1`.**
   - *Pros:* no infrastructure; fixed URL; a captive-portal responder can
     auto-open a page.
   - *Cons:* phone drops off its normal network (loses Wi-Fi internet during
     transfer); more firmware (AP + DNS); the captive-portal auto-open is the
     genuinely slick part, but that benefits a *device-hosted* page more than the
     LNA single-page design we picked.

### HTTP server library

- **Built-in `WebServer` (chosen).** Part of arduino-esp32, no new dependency,
  lowest CI/build risk; its upload handler streams the body in pieces. Cost: the
  upload runs inside `handleClient()` on the main loop, so BLE status
  notifications pause for the transfer.
- **`ESPAsyncWebServer`.** Non-blocking, nicer streaming/backpressure, but adds
  `AsyncTCP` + the library as dependencies. Overkill for a validator; noted as
  the upgrade path if concurrent BLE+upload is ever needed.

## Decision

**Single HTTPS app + Wi-Fi STA + Local Network Access, with built-in
`WebServer`, falling back to the existing BLE upload.**

- **Firmware:** new `drivers/webfs.cpp` — `WebServer` on :80 started from
  `service_wifi_connect()` after a successful STA join (idempotent).
  `POST /upload?path=` streams the multipart body to the existing
  `sdcard::upload_*` state machine (no RAM buffering — the WROOM-1-N8 has no
  PSRAM); `GET /ping` for reachability; `Access-Control-Allow-Origin: *` on every
  response (the request is cross-origin from the HTTPS app). **No new BLE
  characteristics**, so `tools/check_ble_uuids.py` is unaffected.
- **Webapp:** `net.js` holds the board IP (set on Wi-Fi connect, cleared on BLE
  disconnect). `sd.js` POSTs the file via **XHR** (chosen over `fetch` for real
  upload-progress events) to the private-IP literal, and **falls back to the BLE
  chunk protocol** on any failure — board unreachable, denied LNA permission, or
  timeout. A hint line shows which transport is active.

Rationale: STA reuses the most code and the IP we already deliver over BLE, and
keeps the phone online; LNA is what makes the one-app design legal in the
browser; the BLE fallback means the feature degrades gracefully on older Chrome
or hostile networks rather than breaking the existing flow.

### Caveats / follow-ups

- **Requires Chrome ≥142** on the phone for the Wi-Fi path; otherwise it falls
  back to BLE automatically.
- **AP client isolation** on the joined network blocks phone↔board — SoftAP is
  the documented escape hatch if this bites in the clinic.
- Build **not** verified locally (no PlatformIO on the dev machine); relying on
  the `firmware.yml` CI build. Hardware bring-up still to do: BLE connect → join
  Wi-Fi → upload, confirm the one-time LNA prompt, the speed-up, and the BLE
  fallback when Wi-Fi is off.

## Sources

- [Chrome — Local Network Access permission](https://developer.chrome.com/blog/local-network-access)
- [WICG Local Network Access explainer](https://github.com/WICG/local-network-access/blob/main/explainer.md)
- [W3C Secure Contexts](https://www.w3.org/TR/secure-contexts/) (why the app must be HTTPS / why only `localhost` is exempt)
- [MDN — Web Bluetooth API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Bluetooth_API) (secure-context requirement)
- `firmware/src/drivers/webfs.cpp`, `webapp/js/net.js`, `webapp/js/sd.js` (implementation)
- `journal/topics/wireless.md` — "Wi-Fi file transfer" section; `journal/topics/storage.md` — SD-over-SPI write speed.
